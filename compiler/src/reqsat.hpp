// Boolean search shared by the checker and its enumeration cross check
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ast.hpp"

namespace geas {

inline constexpr std::size_t requirementsAtomLimit = 64;
using RequirementAtoms = std::vector<std::string>;

struct RequirementVerdict {
    bool canFulfill = false;
    bool canBreak = false;
    std::optional<std::uint64_t> both;

    bool operator==(const RequirementVerdict&) const = default;
};

/**
 * Collects distinct names in first appearance order across all three lines
 * @params ast - the syntax tree, block - the requirements block
 * @returns {RequirementAtoms} - the ordered atom names
 */
RequirementAtoms collectRequirementAtoms(const Ast& ast, const RequirementsDecl& block);

/**
 * Searches the policy lines and their intersection for satisfying assignments
 * @params ast - the tree, block - the policy, atoms - at most 64 ordered names
 * @returns {RequirementVerdict} - reachability and the smallest overlap assignment
 */
RequirementVerdict searchRequirements(const Ast& ast, const RequirementsDecl& block,
                                      const RequirementAtoms& atoms);

}  // namespace geas
