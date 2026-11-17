// The shape of a checked contract, the flattened view code generation and
// the header emitter both read: its version and term, its vows, pledges,
// subcontracts, signals, remedies, and schemas in declaration order, the
// requirements atoms in first appearance order, the canonical shape string
// with its hash, and the mangled name of every pledge and remedy.
//
// The shape string spells the contract name, then each vow, pledge, and
// schema, so a contract that uses none of the workflow surface hashes
// exactly as it always has. Signals, remedies, terms, the course, and the
// term attribute each append a segment of their own, and only when the
// contract has one. Last come the layouts of the declared records and sums
// the boundary reaches, see layout.hpp, so a contract that reaches none keeps
// its string. A pledge's or remedy's signature hash is taken over its
// signature spelling followed by the layouts that signature alone reaches
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "amendshape.hpp"
#include "ast.hpp"
#include "checked.hpp"
#include "loader.hpp"
#include "sigs.hpp"
#include "types.hpp"

namespace geas {

// One pledge, loose or inside a subcontract, with the ordinal of the
// subcontract that owns it, -1 for a loose pledge
struct PledgeShape {
    const PledgeDecl* decl;
    int subOrdinal;
    std::string signature;
    std::uint64_t sigHash;
    std::string mangled;
};

struct SignalShape {
    const SignalDecl* decl;
    int subOrdinal;
};

// One remedy and the pledge it undoes
struct RemedyShape {
    const RemedyDecl* decl;
    const PledgeDecl* target;
    std::string signature;
    std::uint64_t sigHash;
    std::string mangled;
};

struct ContractShape {
    const ContractDecl* decl;
    std::int64_t version;
    std::optional<std::int64_t> termMilliseconds;
    std::vector<const VowDecl*> vows;
    std::vector<PledgeShape> pledges;
    std::vector<const SubcontractDecl*> subs;
    std::vector<SignalShape> signals;
    std::vector<RemedyShape> remedies;
    std::vector<const SchemaDecl*> schemas;
    const RequirementsDecl* requirements;
    std::vector<std::string> requirementAtoms;
    const CourseDecl* course;
    std::vector<TypeLayout> layouts;
    std::string shape;
    std::uint64_t shapeHash;
};

/**
 * The contract's version attribute, 1 when it carries none.
 * @params ast - the tree, contract - the contract to read
 * @returns {std::int64_t} - the version
 */
std::int64_t contractVersion(const Ast& ast, const ContractDecl& contract);

/**
 * The contract's term attribute in milliseconds.
 * @params ast - the tree, contract - the contract to read
 * @returns {std::optional<std::int64_t>} - the lifetime, nullopt when the contract carries no term
 */
std::optional<std::int64_t> contractTerm(const Ast& ast, const ContractDecl& contract);

/**
 * Flattens every contract of a checked program, in declaration order.
 * @params program - the loaded program, sigs - its lowered signatures, types - the arena, tables -
 * the checker's side tables
 * @returns {std::vector<ContractShape>} - one shape per contract
 */
std::vector<ContractShape> buildContractShapes(const Program& program, const Sigs& sigs,
                                               const TypeArena& types, const CheckTables& tables);

/**
 * Flattens one contract, the same view buildContractShapes builds for each.
 * @params ast - the tree, sigs - the lowered signatures, types - the arena, tables - the checker's
 * side tables, contract - the contract to flatten
 * @returns {ContractShape} - its shape
 */
ContractShape buildContractShape(const Ast& ast, const Sigs& sigs, const TypeArena& types,
                                 const CheckTables& tables, const ContractDecl& contract);

/**
 * The recorded form of a contract's shape, what geas record writes and what
 * an amends block of a later version is checked against.
 * @params shape - the flattened contract, sigs - its lowered signatures, types - the arena,
 * courseHash - the hash of its lowered course, 0 when it has none
 * @returns {RecordedShape} - the names and canonical type codes in descriptor order
 */
RecordedShape recordShape(const ContractShape& shape, const Sigs& sigs, const TypeArena& types,
                          std::uint64_t courseHash);

}  // namespace geas
