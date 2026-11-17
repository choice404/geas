// The contract operations: sign and resume on a contract name, and on a
// signed instance its pledges, its signals, the lifecycle builtins break,
// status, park, and partial, a vow read, and a pledge taken as a first
// class value. Member access on records lives here too, since a member
// expression only learns which of these it is from the type of its base
#include "checker.hpp"

namespace geas {

// One argument of a sign call: labeled with a vow name, supplied once, and
// matching the vow type
void Checker::checkSignArg(const ContractDecl& target, const Argument& arg,
                           std::unordered_set<std::string>& supplied) {
    const Expr& value = ast_.expr(arg.value);
    if (!arg.label) {
        emitAt(value, "sign takes named vow arguments");
        inferExpr(arg.value, TyId{});
        return;
    }
    const std::string& label = *arg.label;
    const MemberEntry* member = resolved_.member(&target, label);
    if (member == nullptr || member->kind() != MemberKind::Vow) {
        emitAt(value, "contract " + quoted(target.name) + " has no vow " + quoted(label));
        inferExpr(arg.value, TyId{});
        return;
    }
    if (!supplied.insert(label).second) {
        emitAt(value, "vow " + quoted(label) + " is supplied twice in sign");
    }
    const TyId wanted = vowType(*member->vow());
    const TyId actual = inferExpr(arg.value, wanted);
    if (!isCompatible(wanted, actual)) {
        const std::string head =
            "vow " + quoted(label) + " of " + quoted(target.name) + " expects ";
        emitAt(value, typesMessage(head, wanted, ", got ", actual));
    }
}

// Contract.sign(named vow overrides): every vow without a default must be
// supplied. Returns the signed instance type
TyId Checker::checkSign(const Expr& expr, const CallExpr& call, const ContractDecl& target) {
    std::unordered_set<std::string> supplied;
    for (const Argument& arg : call.args) {
        checkSignArg(target, arg, supplied);
    }
    for (const Member& member : target.members) {
        const auto* vow = std::get_if<VowDecl>(&member);
        if (vow != nullptr && !vow->init && !supplied.contains(vow->name)) {
            emitAt(expr, "sign of " + quoted(target.name) + " is missing vow " + quoted(vow->name));
        }
    }
    markUse(expr, ExprUse::SignCall);
    return types_.contract(target.name);
}

// The two operations called on a contract name, sign and resume
TyId Checker::callStatic(const Expr& expr, const CallExpr& call, const ContractDecl& target,
                         const std::string& member) {
    if (member == "sign") {
        checkInternalSign(expr, target);
        return checkSign(expr, call, target);
    }
    if (member == "resume") {
        // Resume stands a parked instance back up, so it wears the sign rules
        checkInternalSign(expr, target);
        checkTwoStrings(expr, call, "resume");
        markUse(expr, ExprUse::ResumeCall);
        return types_.contract(target.name);
    }
    walkArgs(call.args);
    emitAt(expr, "only sign and resume are called on the contract " + quoted(target.name));
    return unknown();
}

// The lifecycle operations every signed instance answers, nullopt when the
// member name is none of them
std::optional<TyId> Checker::callInstanceBuiltin(const Expr& expr, const CallExpr& call,
                                                 const std::string& member) {
    const auto takesNothing = [&](TyId answer) {
        if (!call.args.empty()) {
            emitAt(expr, member + " takes no arguments");
        }
        walkArgs(call.args);
        markUse(expr, ExprUse::InstanceBuiltinCall);
        return answer;
    };
    if (member == "break") {
        return takesNothing(scalar(TyKind::Unit));
    }
    if (member == "status") {
        return takesNothing(scalar(TyKind::String));
    }
    if (member == "park") {
        checkTwoStrings(expr, call, "park");
        markUse(expr, ExprUse::InstanceBuiltinCall);
        return scalar(TyKind::Unit);
    }
    if (member == "partial") {
        return takesNothing(types_.named("PartialResult"));
    }
    if (member == "sign" || member == "resume") {
        walkArgs(call.args);
        emitAt(expr, member + " is called on the contract name, not an instance");
        return unknown();
    }
    return std::nullopt;
}

// A contract with a course is driven by its engine, so its pledges are not
// the host's to call. Signals still are
void Checker::refuseCoursePledge(const Expr& expr, const ContractDecl& target,
                                 std::string_view pledge) {
    if (!hasCourse(target)) {
        return;
    }
    emitAt(expr, "contract " + quoted(target.name) + " is driven by its course; pledge " +
                     quoted(pledge) + " cannot be called through an instance");
}

// An operation on a signed instance, called form
TyId Checker::callOnInstance(const Expr& expr, const CallExpr& call, TyId base,
                             const std::string& member) {
    const std::string contractName{types_.nameOf(base)};
    const ContractDecl* target = contractNamed(contractName);
    if (target == nullptr) {
        walkArgs(call.args);
        return unknown();
    }
    if (const std::optional<TyId> answer = callInstanceBuiltin(expr, call, member)) {
        return *answer;
    }
    if (const PledgeDecl* pledge = resolved_.findPledge(target, member)) {
        refuseCoursePledge(expr, *target, member);
        const CallableSig& sig = pledgeSig(*pledge);
        checkArgs(sig.params, call, expr, "pledge", member);
        markUse(expr, ExprUse::InstancePledgeCall);
        return sig.ret;
    }
    if (const SignalDecl* signal = resolved_.findSignal(target, member)) {
        checkArgs(signalSig(*signal).params, call, expr, "signal", member);
        markUse(expr, ExprUse::InstanceSignalCall);
        return scalar(TyKind::Unit);
    }
    walkArgs(call.args);
    const MemberEntry* entry = resolved_.member(target, member);
    if (entry != nullptr && entry->kind() == MemberKind::Vow) {
        emitAt(expr, "the vow " + quoted(member) + " is read through the instance, not called");
        return unknown();
    }
    emitAt(expr, noPledgeFault(*target, entry, member));
    return unknown();
}

// Why a name is no pledge of the instance: a clause is private to its
// contract, anything else is simply not there
std::string Checker::noPledgeFault(const ContractDecl& target, const MemberEntry* entry,
                                   std::string_view name) const {
    if (entry != nullptr && entry->kind() == MemberKind::Clause) {
        return "clause " + quoted(name) + " is called by bare name inside its own contract";
    }
    return "contract " + quoted(target.name) + " has no pledge " + quoted(name);
}

// A member of a signed instance in value position: a vow read, or a pledge
// as a first class value
TyId Checker::instanceMember(const Expr& expr, const MemberExpr& member, TyId base) {
    const std::string contractName{types_.nameOf(base)};
    const std::string& name = member.name;
    const ContractDecl* target = contractNamed(contractName);
    if (target == nullptr) {
        return unknown();
    }
    const bool isLifecycle = name == "break" || name == "sign" || name == "status" ||
                             name == "park" || name == "resume" || name == "partial";
    if (isLifecycle) {
        emitAt(expr, quoted(name) + " must be called");
        return unknown();
    }
    const MemberEntry* entry = resolved_.member(target, name);
    if (entry != nullptr && entry->kind() == MemberKind::Vow) {
        markUse(expr, ExprUse::InstanceVow);
        return vowType(*entry->vow());
    }
    if (const PledgeDecl* pledge = resolved_.findPledge(target, name)) {
        refuseCoursePledge(expr, *target, name);
        const CallableSig& sig = pledgeSig(*pledge);
        markUse(expr, ExprUse::InstancePledgeValue);
        return types_.pledge(sig.params, sig.ret);
    }
    if (resolved_.findSignal(target, name) != nullptr) {
        emitAt(expr, "signal " + quoted(name) + " is delivered with a call, it is not a value");
        return unknown();
    }
    emitAt(expr, noPledgeFault(*target, entry, name));
    return unknown();
}

// A member expression given its base already inferred, the value position
TyId Checker::memberWithBase(const Expr& expr, const MemberExpr& member, TyId base) {
    const TyKind kind = kindOf(base);
    if (kind == TyKind::Contract) {
        return instanceMember(expr, member, base);
    }
    if (kind == TyKind::Unknown) {
        return unknown();
    }
    if (kind != TyKind::Named) {
        emitAt(expr, "a value of type " + quoted(types_.render(base)) + " has no members");
        return unknown();
    }
    const std::string typeName{types_.nameOf(base)};
    const GlobalEntry* global = resolved_.global(typeName);
    if (global != nullptr && global->kind == GlobalKind::Record) {
        if (const FieldSig* field = recordField(typeName, member.name)) {
            markUse(expr, ExprUse::RecordField);
            return field->type;
        }
        emitAt(expr, quoted(typeName) + " has no field " + quoted(member.name));
        return unknown();
    }
    if (global != nullptr && global->kind == GlobalKind::Sum) {
        emitAt(expr, "a value of " + quoted(typeName) + " has no fields; match it");
        return unknown();
    }
    // An opaque outside name, its fields are unknowable so the access passes
    return unknown();
}

// The package boundary on sign: an internal contract signs only from a body
// inside its own package
void Checker::checkInternalSign(const Expr& expr, const ContractDecl& target) {
    if (!target.isInternal) {
        return;
    }
    const int targetPackage = packageOf(target);
    const int ownPackage = contract_ == nullptr ? targetPackage : packageOf(*contract_);
    if (ownPackage != targetPackage) {
        emitAt(expr,
               "internal contract " + quoted(target.name) + " is not visible outside its package");
    }
}

// The contract a base expression names statically: a bare identifier no
// local shadows that resolves to a contract
const ContractDecl* Checker::staticContract(ExprId base) const {
    const auto* ident = std::get_if<Identifier>(&ast_.expr(base).kind);
    if (ident == nullptr || lookup(ident->name) != nullptr) {
        return nullptr;
    }
    return contractNamed(ident->name);
}

// A member expression in value position
TyId Checker::inferMember(const Expr& expr, const MemberExpr& member) {
    if (isPriorBase(member.base)) {
        return inferPriorRead(expr, member);
    }
    if (const ContractDecl* target = staticContract(member.base)) {
        if (member.name == "sign" || member.name == "resume") {
            emitAt(expr, quoted(member.name) + " must be called");
            return unknown();
        }
        emitAt(expr, "only sign and resume are named on the contract " + quoted(target->name));
        return unknown();
    }
    const TyId base = inferExpr(member.base, TyId{});
    return memberWithBase(expr, member, base);
}

}  // namespace geas
