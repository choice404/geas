// Contracts and their bodies. Every incorporated signature must match its
// implementing clause exactly, every vow initializer is a constant literal
// of the vow type, and every pledge, clause, and remedy body checks under
// its declared return type with its parameters in scope
#include "checker.hpp"

namespace geas {

// A vow initializer is a constant literal, or a negated numeric literal.
// Everything else is supplied at sign time instead
bool Checker::isConstantLiteral(ExprId id) const {
    const ExprKind& kind = ast_.expr(id).kind;
    const auto isNumber = [](const ExprKind& candidate) {
        return std::holds_alternative<IntLiteral>(candidate) ||
               std::holds_alternative<FloatLiteral>(candidate) ||
               std::holds_alternative<DurationLiteral>(candidate);
    };
    if (isNumber(kind) || std::holds_alternative<StringLiteral>(kind) ||
        std::holds_alternative<CharLiteral>(kind) || std::holds_alternative<BoolLiteral>(kind)) {
        return true;
    }
    const auto* unary = std::get_if<UnaryExpr>(&kind);
    if (unary == nullptr || unary->op != UnaryOp::Neg) {
        return false;
    }
    return isNumber(ast_.expr(unary->operand).kind);
}

void Checker::checkVow(const VowDecl& vow) {
    if (!vow.init) {
        return;
    }
    const Expr& init = ast_.expr(*vow.init);
    if (!isConstantLiteral(*vow.init)) {
        emitAt(init, "vow " + quoted(vow.name) + " initializer must be a constant literal");
        return;
    }
    const TyId declared = vowType(vow);
    const TyId actual = inferExpr(*vow.init, declared);
    if (!isCompatible(declared, actual)) {
        emitAt(init, typesMessage("vow " + quoted(vow.name) + " initializer has type ", actual,
                                  " but the vow is ", declared));
    }
}

// Every signature an incorporated provisional clause carries must match the
// implementing clause exactly, compared through the canonical spellings
void Checker::checkIncorporate(const ContractDecl& contract, const IncorporateDecl& incorporate) {
    const GlobalEntry* global = resolved_.global(incorporate.name);
    if (global == nullptr || global->kind != GlobalKind::Provisional) {
        return;
    }
    const auto& provisional =
        std::get<ProvisionalDecl>(ast_.decls[static_cast<std::size_t>(global->declIndex)]);
    for (const ClauseSig& sig : provisional.sigs) {
        const MemberEntry* member = resolved_.member(&contract, sig.name);
        if (member == nullptr || member->kind() != MemberKind::Clause) {
            continue;
        }
        const std::string wanted = provisionalCanon(provisional, sig.name);
        const std::string actual = signatureCanon(types_, clauseSig(*member->clause()));
        if (wanted != actual) {
            emit(member->lo, member->hi,
                 "clause " + quoted(sig.name) +
                     " does not match its signature in provisional clause " +
                     quoted(incorporate.name));
        }
    }
}

// The canonical spelling of the first signature of that name in a
// provisional clause, the one the signature table kept
std::string Checker::provisionalCanon(const ProvisionalDecl& provisional,
                                      std::string_view name) const {
    for (const ClauseSig& sig : provisional.sigs) {
        if (sig.name != name) {
            continue;
        }
        const auto found = sigs_.provisionalClauses.find(&sig);
        if (found != sigs_.provisionalClauses.end()) {
            return signatureCanon(types_, found->second);
        }
    }
    return "";
}

// One body: the parameters land in a fresh scope with their lowered types,
// the declared return type rides along for return and '?', and the block
// checks under it
void Checker::checkBody(const std::vector<Field>& params, const std::vector<TyId>& paramTypes,
                        TyId ret, BlockId body, std::int64_t lo, std::int64_t hi) {
    returnType_ = ret;
    bodyLo_ = lo;
    bodyHi_ = hi;
    loopDepth_ = 0;
    pushScope();
    for (std::size_t i = 0; i < params.size(); ++i) {
        declare(params[i].name, i < paramTypes.size() ? paramTypes[i] : unknown(), false);
    }
    checkBlock(body);
    popScope();
    returnType_ = TyId{};
}

void Checker::checkPledgeBody(const PledgeDecl& pledge) {
    if (!pledge.body) {
        return;
    }
    const CallableSig& sig = pledgeSig(pledge);
    const TyId ret = sig.ret.isSet() ? sig.ret : unknown();
    checkBody(pledge.params, sig.params, ret, *pledge.body, pledge.lo, pledge.hi);
}

void Checker::checkClauseBody(const ClauseDecl& clause) {
    const CallableSig& sig = clauseSig(clause);
    const TyId ret = sig.ret.isSet() ? sig.ret : unknown();
    checkBody(clause.params, sig.params, ret, clause.body, clause.lo, clause.hi);
}

// Whether the contract declares a schema, the test for store backed
bool Checker::hasSchema(const ContractDecl& contract) const {
    for (const Member& member : contract.members) {
        if (std::holds_alternative<SchemaDecl>(member)) {
            return true;
        }
    }
    return false;
}

// A transactional subcontract groups durable writes, so it only means
// something over a contract that binds a database
void Checker::checkSubcontract(const ContractDecl& contract, const SubcontractDecl& sub) {
    if (sub.isTransactional && !hasSchema(contract)) {
        emit(sub.lo, sub.hi, "transactional requires a store-backed contract");
    }
    for (const SubMember& member : sub.members) {
        if (const auto* pledge = std::get_if<PledgeDecl>(&member)) {
            checkPledgeBody(*pledge);
        }
    }
}

void Checker::checkMember(const ContractDecl& contract, const Member& member) {
    if (const auto* vow = std::get_if<VowDecl>(&member)) {
        checkVow(*vow);
    } else if (const auto* pledge = std::get_if<PledgeDecl>(&member)) {
        checkPledgeBody(*pledge);
    } else if (const auto* clause = std::get_if<ClauseDecl>(&member)) {
        checkClauseBody(*clause);
    } else if (const auto* sub = std::get_if<SubcontractDecl>(&member)) {
        checkSubcontract(contract, *sub);
    } else if (const auto* remedy = std::get_if<RemedyDecl>(&member)) {
        checkRemedyBody(*remedy);
    } else if (const auto* course = std::get_if<CourseDecl>(&member)) {
        checkCourse(*course);
    } else if (const auto* amends = std::get_if<AmendsDecl>(&member)) {
        checkAmends(contract, *amends);
    }
}

// The declaration level rules run before the bodies: incorporated
// signatures, then the workflow rules that need no body, then each member
void Checker::checkContract(const ContractDecl& contract) {
    contract_ = &contract;
    for (const Member& member : contract.members) {
        if (const auto* incorporate = std::get_if<IncorporateDecl>(&member)) {
            checkIncorporate(contract, *incorporate);
        }
    }
    // A let annotation naming an internal contract is visible to this
    // contract's package alone
    lowering_.currentPackage = packageOf(contract);
    checkWorkflowRules(contract);
    for (const Member& member : contract.members) {
        checkMember(contract, member);
    }
    contract_ = nullptr;
}

}  // namespace geas
