// Terms and ordering. Which terms each declaration accepts: a pledge takes
// within, after, and retry, a subcontract and a signal take within and
// after, a remedy takes within and retry, and no term repeats. A retry
// counts at least one attempt, backs off by at least 1.0, and its on list
// names variants of the declaration's own error sum. An after term names
// pledges, named subcontracts, or signals of the same contract, never the
// item itself or the subcontract it sits in, and the ordering the after
// terms describe must have no cycle. A transactional subcontract holds no
// signal and no pledge with a retry
#include "checker.hpp"

#include <algorithm>
#include <cstdlib>

#include "overloaded.hpp"

namespace geas {

namespace {

constexpr std::int64_t minimumRetryCount = 1;
constexpr double minimumBackoff = 1.0;

std::string_view termWord(const Term& term) {
    if (std::holds_alternative<WithinTerm>(term.kind)) {
        return "within";
    }
    return std::holds_alternative<AfterTerm>(term.kind) ? "after" : "retry";
}

// A float spelling as a number, underscores dropped
double floatValue(const std::string& spelling) {
    std::string digits;
    for (const char c : spelling) {
        if (c != '_') {
            digits.push_back(c);
        }
    }
    return std::strtod(digits.c_str(), nullptr);
}

const void* keyOf(const OrderTarget& target) {
    return std::visit([](const auto* node) { return static_cast<const void*>(node); }, target);
}

// The subcontract a pledge or signal is declared in, null for a loose item
const SubcontractDecl* ownerOf(const ContractDecl& contract, const void* key) {
    for (const Member& member : contract.members) {
        const auto* sub = std::get_if<SubcontractDecl>(&member);
        if (sub == nullptr) {
            continue;
        }
        for (const SubMember& item : sub->members) {
            const void* itemKey =
                std::visit([](const auto& node) { return static_cast<const void*>(&node); }, item);
            if (itemKey == key) {
                return sub;
            }
        }
    }
    return nullptr;
}

// One item of the ordering graph and the items it waits on
struct OrderNode {
    const void* key;
    std::string name;
    std::int64_t lo;
    std::int64_t hi;
    std::vector<const void*> waitsOn;
};

enum class VisitState { Unseen, Open, Done };

// The ordering graph of one contract. An item waits on every target its
// after terms name, a subcontract waits on its own members, and a member
// inherits what its subcontract waits on
class OrderGraph {
public:
    OrderGraph(const ContractDecl& contract, const CheckTables& tables);

    // Every cycle as a fault on the first item of the cycle
    std::vector<Diagnostic> cycles();

private:
    void addNode(const void* key, std::string name, std::int64_t lo, std::int64_t hi);
    OrderNode& nodeOf(const void* key);
    void addEdges(const void* from, const std::vector<Term>& terms);
    void addSub(const SubcontractDecl& sub);
    void visit(std::size_t index, std::vector<Diagnostic>& faults);
    Diagnostic cycleFault(std::size_t start) const;

    const CheckTables& tables_;
    std::vector<OrderNode> nodes_;
    std::unordered_map<const void*, std::size_t> indices_;
    std::vector<VisitState> states_;
    std::vector<std::size_t> path_;
};

void OrderGraph::addNode(const void* key, std::string name, std::int64_t lo, std::int64_t hi) {
    indices_.emplace(key, nodes_.size());
    nodes_.push_back(OrderNode{key, std::move(name), lo, hi, {}});
}

// Every key asked for was added by the constructor's first walk
OrderNode& OrderGraph::nodeOf(const void* key) {
    return nodes_[indices_.find(key)->second];
}

void OrderGraph::addEdges(const void* from, const std::vector<Term>& terms) {
    OrderNode& node = nodeOf(from);
    for (const Term& term : terms) {
        const auto targets = tables_.afterTargets.find(&term);
        if (targets == tables_.afterTargets.end()) {
            continue;
        }
        for (const OrderTarget& target : targets->second) {
            node.waitsOn.push_back(keyOf(target));
        }
    }
}

void OrderGraph::addSub(const SubcontractDecl& sub) {
    addNode(&sub, sub.name ? *sub.name : "subcontract", sub.lo, sub.hi);
    for (const SubMember& member : sub.members) {
        std::visit([&](const auto& item) { addNode(&item, item.name, item.lo, item.hi); }, member);
    }
}

OrderGraph::OrderGraph(const ContractDecl& contract, const CheckTables& tables) : tables_{tables} {
    for (const Member& member : contract.members) {
        if (const auto* pledge = std::get_if<PledgeDecl>(&member)) {
            addNode(pledge, pledge->name, pledge->lo, pledge->hi);
        } else if (const auto* signal = std::get_if<SignalDecl>(&member)) {
            addNode(signal, signal->name, signal->lo, signal->hi);
        } else if (const auto* sub = std::get_if<SubcontractDecl>(&member)) {
            addSub(*sub);
        }
    }
    for (const Member& member : contract.members) {
        if (const auto* pledge = std::get_if<PledgeDecl>(&member)) {
            addEdges(pledge, pledge->terms);
        } else if (const auto* signal = std::get_if<SignalDecl>(&member)) {
            addEdges(signal, signal->terms);
        } else if (const auto* sub = std::get_if<SubcontractDecl>(&member)) {
            addEdges(sub, sub->terms);
            for (const SubMember& item : sub->members) {
                const void* key = std::visit(
                    [](const auto& node) { return static_cast<const void*>(&node); }, item);
                nodeOf(sub).waitsOn.push_back(key);
                std::visit([&](const auto& node) { addEdges(key, node.terms); }, item);
                addEdges(key, sub->terms);
            }
        }
    }
    states_.assign(nodes_.size(), VisitState::Unseen);
}

// The cycle that closes on the item at path position start, spelled a -> b -> a
Diagnostic OrderGraph::cycleFault(std::size_t start) const {
    const OrderNode& first = nodes_[path_[start]];
    std::string message = "the after terms form a cycle: ";
    for (std::size_t i = start; i < path_.size(); ++i) {
        message.append(nodes_[path_[i]].name + " -> ");
    }
    message.append(first.name);
    return Diagnostic{first.lo, first.hi, std::move(message)};
}

void OrderGraph::visit(std::size_t index, std::vector<Diagnostic>& faults) {
    states_[index] = VisitState::Open;
    path_.push_back(index);
    for (const void* key : nodes_[index].waitsOn) {
        const auto found = indices_.find(key);
        if (found == indices_.end()) {
            continue;
        }
        const std::size_t next = found->second;
        if (states_[next] == VisitState::Unseen) {
            visit(next, faults);
            continue;
        }
        if (states_[next] != VisitState::Open) {
            continue;
        }
        const auto start = std::find(path_.begin(), path_.end(), next);
        faults.push_back(cycleFault(static_cast<std::size_t>(start - path_.begin())));
    }
    path_.pop_back();
    states_[index] = VisitState::Done;
}

std::vector<Diagnostic> OrderGraph::cycles() {
    std::vector<Diagnostic> faults;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (states_[i] == VisitState::Unseen) {
            visit(i, faults);
        }
    }
    return faults;
}

}  // namespace

// Which terms the declaration accepts, and that none repeats
void Checker::checkTermList(const std::vector<Term>& terms, const TermOwner& owner) {
    std::unordered_set<std::string_view> seen;
    for (const Term& term : terms) {
        const std::string_view word = termWord(term);
        const bool isAccepted = (word == "within" && owner.takesWithin) ||
                                (word == "after" && owner.takesAfter) ||
                                (word == "retry" && owner.takesRetry);
        if (!isAccepted) {
            emit(term.lo, term.hi,
                 "a " + std::string{owner.kind} + " does not accept the term " + quoted(word));
            continue;
        }
        if (!seen.insert(word).second) {
            emit(term.lo, term.hi,
                 "the term " + quoted(word) + " appears more than once on " + owner.label);
            continue;
        }
        if (const auto* retry = std::get_if<RetryTerm>(&term.kind)) {
            checkRetry(term, *retry, owner);
        }
    }
}

void Checker::checkRetry(const Term& term, const RetryTerm& retry, const TermOwner& owner) {
    if (retry.count < minimumRetryCount) {
        emit(term.lo, term.hi, "retry count must be at least 1");
    }
    if (retry.backoff && floatValue(*retry.backoff) < minimumBackoff) {
        emit(term.lo, term.hi, "retry backoff must be at least 1.0");
    }
    if (!retry.on.empty()) {
        checkRetryOn(term, retry, owner);
    }
}

// The on list names variants of the error sum of the declaration's Result.
// Without a declared sum on the Err side there is nothing an on could name
void Checker::checkRetryOn(const Term& term, const RetryTerm& retry, const TermOwner& owner) {
    const TyNode ret = types_.node(owner.ret);
    if (ret.kind == TyKind::Unknown) {
        return;
    }
    if (ret.kind != TyKind::Result || !isSumType(ret.args[1])) {
        if (ret.kind == TyKind::Result && kindOf(ret.args[1]) == TyKind::Unknown) {
            return;
        }
        emit(term.lo, term.hi,
             "retry on needs " + owner.label +
                 " to return a Result whose error type is a declared sum");
        return;
    }
    const TyId errorType = ret.args[1];
    std::unordered_set<std::string> seen;
    for (const std::string& name : retry.on) {
        if (name.empty()) {
            continue;
        }
        if (!isVariantOf(errorType, name)) {
            emit(term.lo, term.hi,
                 quoted(name) + " is not a variant of " + quoted(types_.nameOf(errorType)) +
                     ", the error type of " + owner.label);
        } else if (!seen.insert(name).second) {
            emit(term.lo, term.hi,
                 "variant " + quoted(name) + " is listed more than once in retry on");
        }
    }
}

// The item an after name resolves to. A member of a named subcontract sees
// its siblings first, then the contract's own names, then the members of
// every named subcontract
std::optional<OrderTarget> Checker::afterTarget(const ContractDecl& contract,
                                                const SubcontractDecl* enclosing, const Term& term,
                                                const std::string& name) {
    const MemberEntry* entry = nullptr;
    if (enclosing != nullptr && enclosing->name) {
        entry = resolved_.subMember(&contract, enclosing, name);
    }
    if (entry == nullptr) {
        entry = resolved_.reachable(&contract, name);
    }
    if (entry == nullptr) {
        emit(term.lo, term.hi, "unknown name " + quoted(name) + " in an after term");
        return std::nullopt;
    }
    switch (entry->kind()) {
        case MemberKind::Pledge: return OrderTarget{entry->pledge()};
        case MemberKind::Signal: return OrderTarget{entry->signal()};
        case MemberKind::Subcontract: return OrderTarget{entry->subcontract()};
        default: break;
    }
    emit(term.lo, term.hi,
         quoted(name) + " is a " + std::string{memberKindWord(entry->kind())} +
             "; an after term names a pledge, a subcontract, or a signal");
    return std::nullopt;
}

// One name of an after term, filed as a target unless the item would wait
// on itself, on the subcontract it sits in, or, for a subcontract, on one
// of its own members
void Checker::fileAfterTarget(const ContractDecl& contract, const SubcontractDecl* enclosing,
                              const Term& term, const std::string& name, OrderTarget self,
                              const std::string& label, std::vector<OrderTarget>& targets) {
    const std::optional<OrderTarget> target = afterTarget(contract, enclosing, term, name);
    if (!target) {
        return;
    }
    const auto* selfSub = std::get_if<const SubcontractDecl*>(&self);
    const auto* targetSub = std::get_if<const SubcontractDecl*>(&*target);
    const bool isOwnMember = selfSub != nullptr && ownerOf(contract, keyOf(*target)) == *selfSub;
    if (*target == self) {
        emit(term.lo, term.hi, label + " cannot be ordered after itself");
    } else if (targetSub != nullptr && *targetSub == enclosing) {
        emit(term.lo, term.hi, label + " cannot be ordered after its own subcontract");
    } else if (isOwnMember) {
        emit(term.lo, term.hi, label + " cannot be ordered after its own member " + quoted(name));
    } else {
        targets.push_back(*target);
    }
}

// Resolves every after term of one declaration and files the targets
void Checker::resolveAfter(const ContractDecl& contract, const SubcontractDecl* enclosing,
                           const std::vector<Term>& terms, OrderTarget self,
                           const std::string& label) {
    for (const Term& term : terms) {
        const auto* after = std::get_if<AfterTerm>(&term.kind);
        if (after == nullptr) {
            continue;
        }
        std::vector<OrderTarget> targets;
        for (const std::string& name : after->names) {
            if (!name.empty()) {
                fileAfterTarget(contract, enclosing, term, name, self, label, targets);
            }
        }
        tables_.afterTargets.emplace(&term, std::move(targets));
    }
}

void Checker::checkSubTerms(const ContractDecl& contract, const SubcontractDecl& sub) {
    const std::string label = sub.name ? "subcontract " + quoted(*sub.name) : "this subcontract";
    checkTermList(sub.terms, TermOwner{"subcontract", label, true, true, false, unknown()});
    resolveAfter(contract, nullptr, sub.terms, OrderTarget{&sub}, label);
    for (const SubMember& member : sub.members) {
        if (const auto* pledge = std::get_if<PledgeDecl>(&member)) {
            const std::string name = "pledge " + quoted(pledge->name);
            checkTermList(pledge->terms,
                          TermOwner{"pledge", name, true, true, true, pledgeSig(*pledge).ret});
            resolveAfter(contract, &sub, pledge->terms, OrderTarget{pledge}, name);
            refuseTermAttr(pledge->attrs, name);
            refuseInTransaction(sub, member, name);
        } else if (const auto* signal = std::get_if<SignalDecl>(&member)) {
            const std::string name = "signal " + quoted(signal->name);
            checkTermList(signal->terms, TermOwner{"signal", name, true, true, false, unknown()});
            resolveAfter(contract, &sub, signal->terms, OrderTarget{signal}, name);
            refuseInTransaction(sub, member, name);
        }
    }
}

// The declaration level workflow rules of one contract, run before any body
void Checker::checkWorkflowRules(const ContractDecl& contract) {
    checkContractAttrs(contract);
    for (const Member& member : contract.members) {
        if (const auto* pledge = std::get_if<PledgeDecl>(&member)) {
            const std::string name = "pledge " + quoted(pledge->name);
            checkTermList(pledge->terms,
                          TermOwner{"pledge", name, true, true, true, pledgeSig(*pledge).ret});
            resolveAfter(contract, nullptr, pledge->terms, OrderTarget{pledge}, name);
            refuseTermAttr(pledge->attrs, name);
        } else if (const auto* signal = std::get_if<SignalDecl>(&member)) {
            const std::string name = "signal " + quoted(signal->name);
            checkTermList(signal->terms, TermOwner{"signal", name, true, true, false, unknown()});
            resolveAfter(contract, nullptr, signal->terms, OrderTarget{signal}, name);
        } else if (const auto* sub = std::get_if<SubcontractDecl>(&member)) {
            checkSubTerms(contract, *sub);
        } else if (const auto* remedy = std::get_if<RemedyDecl>(&member)) {
            const std::string name = "remedy " + quoted(remedy->name);
            checkTermList(remedy->terms,
                          TermOwner{"remedy", name, true, false, true, remedySig(*remedy).ret});
            refuseTermAttr(remedy->attrs, name);
        }
    }
    checkOrdering(contract);
    checkRemedies(contract);
    checkAmendsVersions(contract);
}

void Checker::checkOrdering(const ContractDecl& contract) {
    OrderGraph graph{contract, tables_};
    for (Diagnostic& fault : graph.cycles()) {
        diagnostics_.push_back(std::move(fault));
    }
}

}  // namespace geas
