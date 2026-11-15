// Name resolution, the first checking pass over a loaded program. It builds
// the tables the type checker reads: the program wide namespace of records,
// sums, sum variants, provisional clauses, and contracts; one member
// namespace per contract where vows, pledges, clauses, named subcontracts,
// signals, and remedies share their names; and one namespace per named
// subcontract for the pledges and signals inside it. Members of an anonymous
// subcontract join the contract namespace, marked with the subcontract that
// declared them. With the tables built it checks the references the grammar
// leaves open: incorporates, requirements atoms, and the one course rule.
//
// Table entries point into the tree, so the program must outlive the tables
// and must not change once they are built
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "ast.hpp"
#include "diagnostics.hpp"
#include "loader.hpp"

namespace geas {

enum class GlobalKind { Record, Sum, Variant, Provisional, Contract };

// A global name: its kind, the top level declaration that owns it, the
// package of the file that declared it, and the span of that declaration. A
// variant names its sum's declaration and a schema row its contract's
struct GlobalEntry {
    GlobalKind kind;
    int declIndex;
    int package;
    std::int64_t lo;
    std::int64_t hi;
};

enum class MemberKind { Vow, Pledge, Clause, Subcontract, Signal, Remedy };

/**
 * The word a diagnostic uses for a member kind.
 * @params kind - the member kind
 * @returns {std::string_view} - vow, pledge, clause, subcontract, signal, or remedy
 */
std::string_view memberKindWord(MemberKind kind);

using MemberNode = std::variant<const VowDecl*, const PledgeDecl*, const ClauseDecl*,
                                const SubcontractDecl*, const SignalDecl*, const RemedyDecl*>;

// A contract member, and for a pledge or signal the subcontract it was
// declared in, null when it sits directly in the contract
struct MemberEntry {
    MemberNode node;
    const SubcontractDecl* owner;
    std::int64_t lo;
    std::int64_t hi;

    MemberKind kind() const { return static_cast<MemberKind>(node.index()); }
    const VowDecl* vow() const;
    const PledgeDecl* pledge() const;
    const ClauseDecl* clause() const;
    const SubcontractDecl* subcontract() const;
    const SignalDecl* signal() const;
    const RemedyDecl* remedy() const;
};

using MemberTable = std::unordered_map<std::string, MemberEntry>;

// The namespaces of one contract
struct ContractScope {
    int package = 0;
    MemberTable members;
    std::unordered_map<const SubcontractDecl*, MemberTable> subMembers;
    std::unordered_set<const ProvisionalDecl*> incorporated;
};

// The resolved tables handed to the type checker
struct Resolved {
    std::unordered_map<std::string, GlobalEntry> globals;
    std::unordered_map<const ContractDecl*, ContractScope> contracts;
    std::vector<Diagnostic> diagnostics;

    // The entry a global name resolves to, null when nothing declares it
    const GlobalEntry* global(std::string_view name) const;

    // The entry a name resolves to in a contract's member namespace
    const MemberEntry* member(const ContractDecl* contract, std::string_view name) const;

    // The entry a name resolves to inside one named subcontract
    const MemberEntry* subMember(const ContractDecl* contract, const SubcontractDecl* sub,
                                 std::string_view name) const;

    /**
     * A pledge or signal reachable by name on the contract: a member of the
     * contract namespace first, then a member of any named subcontract in
     * declaration order.
     * @params contract - the contract to search, name - the member name
     * @returns {const MemberEntry*} - the entry, null when no namespace holds the name
     */
    const MemberEntry* reachable(const ContractDecl* contract, std::string_view name) const;

    // The first member of one kind reachable by name, so a member of another
    // kind under the same name does not hide it
    const MemberEntry* reachableOfKind(const ContractDecl* contract, std::string_view name,
                                       MemberKind kind) const;

    // A pledge or a signal an instance of the contract answers by name
    const PledgeDecl* findPledge(const ContractDecl* contract, std::string_view name) const;
    const SignalDecl* findSignal(const ContractDecl* contract, std::string_view name) const;
};

/**
 * Resolves the names of a loaded program.
 * @params program - the loaded program, already free of load faults
 * @returns {Resolved} - the tables, and the faults in diagnostics
 */
Resolved resolveProgram(const Program& program);

/**
 * The contract a declaration index names.
 * @params ast - the tree, declIndex - an index into its declarations
 * @returns {const ContractDecl*} - the contract, null when the declaration is something else
 */
const ContractDecl* contractAt(const Ast& ast, int declIndex);

}  // namespace geas
