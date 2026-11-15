// The lookups over the resolved tables, see resolve.hpp
#include "resolve.hpp"

namespace geas {

namespace {

template <typename Node>
const Node* nodeOf(const MemberNode& node) {
    const auto* held = std::get_if<const Node*>(&node);
    return held == nullptr ? nullptr : *held;
}

}  // namespace

std::string_view memberKindWord(MemberKind kind) {
    switch (kind) {
        case MemberKind::Vow: return "vow";
        case MemberKind::Pledge: return "pledge";
        case MemberKind::Clause: return "clause";
        case MemberKind::Subcontract: return "subcontract";
        case MemberKind::Signal: return "signal";
        case MemberKind::Remedy: return "remedy";
    }
    return "subcontract";
}

const VowDecl* MemberEntry::vow() const {
    return nodeOf<VowDecl>(node);
}

const PledgeDecl* MemberEntry::pledge() const {
    return nodeOf<PledgeDecl>(node);
}

const ClauseDecl* MemberEntry::clause() const {
    return nodeOf<ClauseDecl>(node);
}

const SubcontractDecl* MemberEntry::subcontract() const {
    return nodeOf<SubcontractDecl>(node);
}

const SignalDecl* MemberEntry::signal() const {
    return nodeOf<SignalDecl>(node);
}

const RemedyDecl* MemberEntry::remedy() const {
    return nodeOf<RemedyDecl>(node);
}

const GlobalEntry* Resolved::global(std::string_view name) const {
    const auto found = globals.find(std::string{name});
    return found == globals.end() ? nullptr : &found->second;
}

const MemberEntry* Resolved::member(const ContractDecl* contract, std::string_view name) const {
    const auto scope = contracts.find(contract);
    if (scope == contracts.end()) {
        return nullptr;
    }
    const auto found = scope->second.members.find(std::string{name});
    return found == scope->second.members.end() ? nullptr : &found->second;
}

const MemberEntry* Resolved::subMember(const ContractDecl* contract, const SubcontractDecl* sub,
                                       std::string_view name) const {
    const auto scope = contracts.find(contract);
    if (scope == contracts.end()) {
        return nullptr;
    }
    const auto table = scope->second.subMembers.find(sub);
    if (table == scope->second.subMembers.end()) {
        return nullptr;
    }
    const auto found = table->second.find(std::string{name});
    return found == table->second.end() ? nullptr : &found->second;
}

const MemberEntry* Resolved::reachable(const ContractDecl* contract, std::string_view name) const {
    if (contract == nullptr) {
        return nullptr;
    }
    if (const MemberEntry* entry = member(contract, name)) {
        return entry;
    }
    for (const Member& item : contract->members) {
        const auto* sub = std::get_if<SubcontractDecl>(&item);
        if (sub == nullptr || !sub->name) {
            continue;
        }
        if (const MemberEntry* entry = subMember(contract, sub, name)) {
            return entry;
        }
    }
    return nullptr;
}

// The first member of one kind reachable by name, the contract namespace
// before the named subcontracts. A member of another kind under the same
// name does not hide it
const MemberEntry* Resolved::reachableOfKind(const ContractDecl* contract, std::string_view name,
                                             MemberKind kind) const {
    if (contract == nullptr) {
        return nullptr;
    }
    const MemberEntry* loose = member(contract, name);
    if (loose != nullptr && loose->kind() == kind) {
        return loose;
    }
    for (const Member& item : contract->members) {
        const auto* sub = std::get_if<SubcontractDecl>(&item);
        if (sub == nullptr || !sub->name) {
            continue;
        }
        const MemberEntry* entry = subMember(contract, sub, name);
        if (entry != nullptr && entry->kind() == kind) {
            return entry;
        }
    }
    return nullptr;
}

const PledgeDecl* Resolved::findPledge(const ContractDecl* contract, std::string_view name) const {
    const MemberEntry* entry = reachableOfKind(contract, name, MemberKind::Pledge);
    return entry == nullptr ? nullptr : entry->pledge();
}

const SignalDecl* Resolved::findSignal(const ContractDecl* contract, std::string_view name) const {
    const MemberEntry* entry = reachableOfKind(contract, name, MemberKind::Signal);
    return entry == nullptr ? nullptr : entry->signal();
}

const ContractDecl* contractAt(const Ast& ast, int declIndex) {
    if (declIndex < 0 || static_cast<std::size_t>(declIndex) >= ast.decls.size()) {
        return nullptr;
    }
    return std::get_if<ContractDecl>(&ast.decls[static_cast<std::size_t>(declIndex)]);
}

}  // namespace geas
