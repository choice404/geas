// Name resolution, see resolve.hpp
#include "resolve.hpp"

#include <utility>

#include "quote.hpp"

namespace geas {

namespace {

constexpr std::size_t requirementsAtomLimit = 16;

std::string_view globalKindWord(GlobalKind kind) {
    switch (kind) {
        case GlobalKind::Record: return "record";
        case GlobalKind::Sum: return "sum";
        case GlobalKind::Variant: return "variant";
        case GlobalKind::Provisional: return "provisional clause";
        case GlobalKind::Contract: return "contract";
    }
    return "contract";
}

class Resolver {
public:
    explicit Resolver(const Program& program) : program_{program}, ast_{program.ast} {}

    Resolved run() &&;

private:
    void emit(std::int64_t lo, std::int64_t hi, std::string message);
    void declareGlobal(GlobalKind kind, int declIndex, int package, const std::string& name,
                       std::int64_t lo, std::int64_t hi);
    void collectGlobals();
    void collectMembers(int declIndex, const ContractDecl& contract);
    void declareMember(const ContractDecl& contract, MemberNode node, const SubcontractDecl* owner,
                       const std::string& name, std::int64_t lo, std::int64_t hi);
    void collectSub(const ContractDecl& contract, const SubcontractDecl& sub);
    void declareSubMember(const ContractDecl& contract, const SubcontractDecl& sub, MemberNode node,
                          const std::string& name, std::int64_t lo, std::int64_t hi);
    void resolveIncorporates(const ContractDecl& contract);
    void resolveIncorporate(const ContractDecl& contract, const IncorporateDecl& incorporate,
                            std::unordered_map<std::string, std::string>& provided);
    void checkProvided(const ContractDecl& contract, const IncorporateDecl& incorporate,
                       const ProvisionalDecl& provisional,
                       std::unordered_map<std::string, std::string>& provided);
    void resolveRequirements(const ContractDecl& contract);
    void resolveReqExpr(const ContractDecl& contract, ReqId id,
                        std::unordered_set<std::string>& seen);
    void resolveReqAtom(const ContractDecl& contract, const ReqNode& node, const std::string& name);
    const MemberEntry* namedSubMember(const ContractDecl& contract, std::string_view name) const;

    const Program& program_;
    const Ast& ast_;
    Resolved resolved_;
};

void Resolver::emit(std::int64_t lo, std::int64_t hi, std::string message) {
    resolved_.diagnostics.push_back(Diagnostic{lo, hi, std::move(message)});
}

// Declares one global name. A repeat of any kind is a fault named for the
// later declaration, and the first entry stays so lookups see one answer. An
// empty name is a parse recovery placeholder and is skipped
void Resolver::declareGlobal(GlobalKind kind, int declIndex, int package, const std::string& name,
                             std::int64_t lo, std::int64_t hi) {
    if (name.empty()) {
        return;
    }
    if (resolved_.globals.contains(name)) {
        emit(lo, hi, "duplicate " + std::string{globalKindWord(kind)} + " " + quoted(name));
        return;
    }
    resolved_.globals.emplace(name, GlobalEntry{kind, declIndex, package, lo, hi});
}

// Fills the global namespace in declaration order. A variant has no span of
// its own and borrows its sum's
void Resolver::collectGlobals() {
    for (std::size_t i = 0; i < ast_.decls.size(); ++i) {
        const int index = static_cast<int>(i);
        const int package = program_.declPackages[i];
        const Decl& decl = ast_.decls[i];
        if (const auto* record = std::get_if<RecordDecl>(&decl)) {
            declareGlobal(GlobalKind::Record, index, package, record->name, record->lo, record->hi);
        } else if (const auto* sum = std::get_if<SumDecl>(&decl)) {
            declareGlobal(GlobalKind::Sum, index, package, sum->name, sum->lo, sum->hi);
            for (const Variant& variant : sum->variants) {
                declareGlobal(GlobalKind::Variant, index, package, variant.name, sum->lo, sum->hi);
            }
        } else if (const auto* provisional = std::get_if<ProvisionalDecl>(&decl)) {
            declareGlobal(GlobalKind::Provisional, index, package, provisional->name,
                          provisional->lo, provisional->hi);
        } else if (const auto* contract = std::get_if<ContractDecl>(&decl)) {
            declareGlobal(GlobalKind::Contract, index, package, contract->name, contract->lo,
                          contract->hi);
        }
    }
}

// Declares one contract member. Every member kind shares the namespace, so a
// repeat across kinds is as much a fault as a repeat within one
void Resolver::declareMember(const ContractDecl& contract, MemberNode node,
                             const SubcontractDecl* owner, const std::string& name, std::int64_t lo,
                             std::int64_t hi) {
    if (name.empty()) {
        return;
    }
    const MemberEntry entry{node, owner, lo, hi};
    MemberTable& table = resolved_.contracts[&contract].members;
    if (table.contains(name)) {
        emit(lo, hi,
             "duplicate " + std::string{memberKindWord(entry.kind())} + " " + quoted(name) +
                 " in contract " + quoted(contract.name));
        return;
    }
    table.emplace(name, entry);
}

void Resolver::declareSubMember(const ContractDecl& contract, const SubcontractDecl& sub,
                                MemberNode node, const std::string& name, std::int64_t lo,
                                std::int64_t hi) {
    if (name.empty()) {
        return;
    }
    const MemberEntry entry{node, &sub, lo, hi};
    MemberTable& table = resolved_.contracts[&contract].subMembers[&sub];
    if (table.contains(name)) {
        emit(lo, hi,
             "duplicate " + std::string{memberKindWord(entry.kind())} + " " + quoted(name) +
                 " in subcontract " + quoted(*sub.name));
        return;
    }
    table.emplace(name, entry);
}

// One subcontract's names. A named subcontract takes a member slot and keeps
// its pledges and signals in its own namespace, an anonymous one contributes
// only its members, which land in the contract namespace marked with it
void Resolver::collectSub(const ContractDecl& contract, const SubcontractDecl& sub) {
    if (sub.name) {
        declareMember(contract, &sub, nullptr, *sub.name, sub.lo, sub.hi);
    }
    for (const SubMember& member : sub.members) {
        const auto* pledge = std::get_if<PledgeDecl>(&member);
        const auto* signal = std::get_if<SignalDecl>(&member);
        const MemberNode node = pledge != nullptr ? MemberNode{pledge} : MemberNode{signal};
        const std::string& name = pledge != nullptr ? pledge->name : signal->name;
        const std::int64_t lo = pledge != nullptr ? pledge->lo : signal->lo;
        const std::int64_t hi = pledge != nullptr ? pledge->hi : signal->hi;
        if (sub.name) {
            declareSubMember(contract, sub, node, name, lo, hi);
        } else {
            declareMember(contract, node, &sub, name, lo, hi);
        }
    }
}

// Fills one contract's member namespace in source order and reports a second
// requirements block or a second course, which the grammar gives no meaning
void Resolver::collectMembers(int declIndex, const ContractDecl& contract) {
    const GlobalEntry* self = resolved_.global(contract.name);
    const int package = self == nullptr ? -1 : self->package;
    resolved_.contracts[&contract].package = package;
    bool hasRequirements = false;
    bool hasCourse = false;
    for (const Member& member : contract.members) {
        if (const auto* schema = std::get_if<SchemaDecl>(&member)) {
            // A schema stands up a row record of the same name
            declareGlobal(GlobalKind::Record, declIndex, package, schema->name, schema->lo,
                          schema->hi);
        } else if (const auto* vow = std::get_if<VowDecl>(&member)) {
            declareMember(contract, vow, nullptr, vow->name, vow->lo, vow->hi);
        } else if (const auto* pledge = std::get_if<PledgeDecl>(&member)) {
            declareMember(contract, pledge, nullptr, pledge->name, pledge->lo, pledge->hi);
        } else if (const auto* clause = std::get_if<ClauseDecl>(&member)) {
            declareMember(contract, clause, nullptr, clause->name, clause->lo, clause->hi);
        } else if (const auto* sub = std::get_if<SubcontractDecl>(&member)) {
            collectSub(contract, *sub);
        } else if (const auto* signal = std::get_if<SignalDecl>(&member)) {
            declareMember(contract, signal, nullptr, signal->name, signal->lo, signal->hi);
        } else if (const auto* remedy = std::get_if<RemedyDecl>(&member)) {
            declareMember(contract, remedy, nullptr, remedy->name, remedy->lo, remedy->hi);
        } else if (const auto* requirements = std::get_if<RequirementsDecl>(&member)) {
            if (hasRequirements) {
                emit(requirements->lo, requirements->hi,
                     "duplicate requirements block in contract " + quoted(contract.name));
            }
            hasRequirements = true;
        } else if (const auto* course = std::get_if<CourseDecl>(&member)) {
            if (hasCourse) {
                emit(course->lo, course->hi,
                     "duplicate course in contract " + quoted(contract.name));
            }
            hasCourse = true;
        }
    }
}

// One incorporate. The name must be a provisional clause, visible from this
// package, that this contract has not already incorporated. provided maps
// each contributed clause name to the provisional clause that brought it
void Resolver::resolveIncorporate(const ContractDecl& contract, const IncorporateDecl& incorporate,
                                  std::unordered_map<std::string, std::string>& provided) {
    const std::string& name = incorporate.name;
    const auto fault = [&](std::string message) {
        emit(incorporate.lo, incorporate.hi, std::move(message));
    };
    if (name.empty()) {
        return;
    }
    const GlobalEntry* global = resolved_.global(name);
    if (global == nullptr) {
        return fault("unknown provisional clause " + quoted(name));
    }
    if (global->kind != GlobalKind::Provisional) {
        return fault(quoted(name) + " is not a provisional clause");
    }
    const auto& provisional =
        std::get<ProvisionalDecl>(ast_.decls[static_cast<std::size_t>(global->declIndex)]);
    ContractScope& scope = resolved_.contracts[&contract];
    if (provisional.isInternal && global->package != scope.package) {
        return fault("internal provisional clause " + quoted(name) +
                     " is not visible outside its package");
    }
    if (!scope.incorporated.insert(&provisional).second) {
        return fault("duplicate incorporate of " + quoted(name) + " in contract " +
                     quoted(contract.name));
    }
    checkProvided(contract, incorporate, provisional, provided);
}

// Every signature the provisional clause carries: no other incorporated
// provisional clause may provide the same name, and the contract must
// declare a clause of that name
void Resolver::checkProvided(const ContractDecl& contract, const IncorporateDecl& incorporate,
                             const ProvisionalDecl& provisional,
                             std::unordered_map<std::string, std::string>& provided) {
    const std::string& name = incorporate.name;
    const auto fault = [&](std::string message) {
        emit(incorporate.lo, incorporate.hi, std::move(message));
    };
    for (const ClauseSig& sig : provisional.sigs) {
        if (sig.name.empty()) {
            continue;
        }
        const auto [slot, isNew] = provided.emplace(sig.name, name);
        if (!isNew) {
            fault("provisional clauses " + quoted(slot->second) + " and " + quoted(name) +
                  " both provide clause " + quoted(sig.name));
            continue;
        }
        const MemberEntry* member = resolved_.member(&contract, sig.name);
        if (member == nullptr || member->kind() != MemberKind::Clause) {
            fault("contract " + quoted(contract.name) + " does not implement clause " +
                  quoted(sig.name) + " from provisional clause " + quoted(name));
        }
    }
}

void Resolver::resolveIncorporates(const ContractDecl& contract) {
    std::unordered_map<std::string, std::string> provided;
    for (const Member& member : contract.members) {
        if (const auto* incorporate = std::get_if<IncorporateDecl>(&member)) {
            resolveIncorporate(contract, *incorporate, provided);
        }
    }
}

// The pledge or signal of that name inside one of the contract's named
// subcontracts, the case the requirements fault calls out by what it is
const MemberEntry* Resolver::namedSubMember(const ContractDecl& contract,
                                            std::string_view name) const {
    for (const Member& member : contract.members) {
        const auto* sub = std::get_if<SubcontractDecl>(&member);
        if (sub == nullptr || !sub->name) {
            continue;
        }
        if (const MemberEntry* entry = resolved_.subMember(&contract, sub, name)) {
            return entry;
        }
    }
    return nullptr;
}

// One requirements atom. It must name a named subcontract, or a pledge or
// signal declared outside any subcontract
void Resolver::resolveReqAtom(const ContractDecl& contract, const ReqNode& node,
                              const std::string& name) {
    const auto insideSub = [&](std::string_view word) {
        emit(node.lo, node.hi,
             std::string{word} + " " + quoted(name) +
                 " is declared inside a subcontract and cannot be named in requirements");
    };
    const MemberEntry* member = resolved_.member(&contract, name);
    if (member == nullptr) {
        const MemberEntry* nested = namedSubMember(contract, name);
        if (nested == nullptr) {
            emit(node.lo, node.hi, "unknown name " + quoted(name) + " in requirements");
            return;
        }
        return insideSub(memberKindWord(nested->kind()));
    }
    switch (member->kind()) {
        case MemberKind::Subcontract: return;
        case MemberKind::Pledge:
        case MemberKind::Signal:
            if (member->owner != nullptr) {
                insideSub(memberKindWord(member->kind()));
            }
            return;
        case MemberKind::Remedy:
            emit(node.lo, node.hi, "remedy " + quoted(name) + " cannot be named in requirements");
            return;
        default:
            emit(node.lo, node.hi,
                 "requirements name " + quoted(name) + " is not a subcontract or a pledge");
    }
}

// Every named atom counts toward the contract limit whether it resolved or
// not, since the limit bounds the enumeration over the names as written
void Resolver::resolveReqExpr(const ContractDecl& contract, ReqId id,
                              std::unordered_set<std::string>& seen) {
    const ReqNode& node = ast_.req(id);
    if (const auto* atom = std::get_if<ReqAtom>(&node.kind)) {
        if (atom->name.empty()) {
            return;
        }
        seen.insert(atom->name);
        resolveReqAtom(contract, node, atom->name);
    } else if (const auto* negation = std::get_if<ReqNot>(&node.kind)) {
        resolveReqExpr(contract, negation->operand, seen);
    } else if (const auto* conjunction = std::get_if<ReqAnd>(&node.kind)) {
        resolveReqExpr(contract, conjunction->lhs, seen);
        resolveReqExpr(contract, conjunction->rhs, seen);
    } else if (const auto* disjunction = std::get_if<ReqOr>(&node.kind)) {
        resolveReqExpr(contract, disjunction->lhs, seen);
        resolveReqExpr(contract, disjunction->rhs, seen);
    }
}

// The contract's first requirements block, a second was already reported
void Resolver::resolveRequirements(const ContractDecl& contract) {
    for (const Member& member : contract.members) {
        const auto* block = std::get_if<RequirementsDecl>(&member);
        if (block == nullptr) {
            continue;
        }
        std::unordered_set<std::string> seen;
        for (const std::optional<ReqId>& line :
             {block->fulfill, block->partial, block->breakLine}) {
            if (line) {
                resolveReqExpr(contract, *line, seen);
            }
        }
        if (seen.size() > requirementsAtomLimit) {
            emit(block->lo, block->hi,
                 "too many requirements atoms in contract " + quoted(contract.name) +
                     "; the limit is 16");
        }
        return;
    }
}

// Globals first, then every contract's member table, and only then the
// incorporate and requirements checks, so a reference to something declared
// later resolves the same as one declared earlier
Resolved Resolver::run() && {
    collectGlobals();
    for (std::size_t i = 0; i < ast_.decls.size(); ++i) {
        if (const auto* contract = std::get_if<ContractDecl>(&ast_.decls[i])) {
            collectMembers(static_cast<int>(i), *contract);
        }
    }
    for (const Decl& decl : ast_.decls) {
        if (const auto* contract = std::get_if<ContractDecl>(&decl)) {
            resolveIncorporates(*contract);
            resolveRequirements(*contract);
        }
    }
    return std::move(resolved_);
}

}  // namespace

Resolved resolveProgram(const Program& program) {
    return Resolver{program}.run();
}

}  // namespace geas
