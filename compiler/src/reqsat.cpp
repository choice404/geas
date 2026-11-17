// Tseitin clauses preserve the tree's truth with one variable per binary
// node. Unit propagation forces consequences before DPLL branches on atoms
#include "reqsat.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <utility>

namespace geas {

namespace {

constexpr std::int8_t unassigned = -1;
constexpr std::int8_t isFalse = 0;
constexpr std::int8_t isTrue = 1;
constexpr std::size_t clauseWidth = 3;
using Clause = std::array<int, clauseWidth>;
using Assignment = std::vector<std::int8_t>;

void collectAtoms(const Ast& ast, ReqId id, RequirementAtoms& atoms) {
    const ReqNode& node = ast.req(id);
    if (const auto* atom = std::get_if<ReqAtom>(&node.kind)) {
        if (std::find(atoms.begin(), atoms.end(), atom->name) == atoms.end()) {
            atoms.push_back(atom->name);
        }
    } else if (const auto* negation = std::get_if<ReqNot>(&node.kind)) {
        collectAtoms(ast, negation->operand, atoms);
    } else if (const auto* conjunction = std::get_if<ReqAnd>(&node.kind)) {
        collectAtoms(ast, conjunction->lhs, atoms);
        collectAtoms(ast, conjunction->rhs, atoms);
    } else if (const auto* disjunction = std::get_if<ReqOr>(&node.kind)) {
        collectAtoms(ast, disjunction->lhs, atoms);
        collectAtoms(ast, disjunction->rhs, atoms);
    }
}

struct Formula {
    std::vector<Clause> clauses;
    std::size_t variableCount;

    int encode(const Ast& ast, ReqId id, const RequirementAtoms& atoms);
    int combine(int lhs, int rhs, bool isAnd);
    std::optional<std::uint64_t> satisfy(const std::vector<int>& roots,
                                        std::size_t atomCount) const;
};

int Formula::combine(int lhs, int rhs, bool isAnd) {
    const int result = static_cast<int>(++variableCount);
    if (isAnd) {
        clauses.push_back({-result, lhs, 0});
        clauses.push_back({-result, rhs, 0});
        clauses.push_back({result, -lhs, -rhs});
    } else {
        clauses.push_back({result, -lhs, 0});
        clauses.push_back({result, -rhs, 0});
        clauses.push_back({-result, lhs, rhs});
    }
    return result;
}

int Formula::encode(const Ast& ast, ReqId id, const RequirementAtoms& atoms) {
    const ReqNode& node = ast.req(id);
    if (const auto* atom = std::get_if<ReqAtom>(&node.kind)) {
        const auto found = std::find(atoms.begin(), atoms.end(), atom->name);
        return static_cast<int>(found - atoms.begin()) + 1;
    }
    if (const auto* negation = std::get_if<ReqNot>(&node.kind)) {
        return -encode(ast, negation->operand, atoms);
    }
    if (const auto* conjunction = std::get_if<ReqAnd>(&node.kind)) {
        const int lhs = encode(ast, conjunction->lhs, atoms);
        const int rhs = encode(ast, conjunction->rhs, atoms);
        return combine(lhs, rhs, true);
    }
    const auto& disjunction = std::get<ReqOr>(node.kind);
    const int lhs = encode(ast, disjunction.lhs, atoms);
    const int rhs = encode(ast, disjunction.rhs, atoms);
    return combine(lhs, rhs, false);
}

bool assignLiteral(Assignment& assignment, int literal) {
    auto& value = assignment[static_cast<std::size_t>(std::abs(literal))];
    const std::int8_t wanted = literal > 0 ? isTrue : isFalse;
    if (value != unassigned) {
        return value == wanted;
    }
    value = wanted;
    return true;
}

// A satisfied clause needs nothing; one remaining literal forces its value
bool propagateClause(const Clause& clause, Assignment& assignment, bool& hasChanged) {
    int remaining = 0;
    std::size_t remainingCount = 0;
    for (const int literal : clause) {
        if (literal == 0) {
            continue;
        }
        const auto value = assignment[static_cast<std::size_t>(std::abs(literal))];
        if (value == unassigned) {
            remaining = literal;
            ++remainingCount;
        } else if ((value == isTrue) == (literal > 0)) {
            return true;
        }
    }
    if (remainingCount == 1) {
        hasChanged = true;
        return assignLiteral(assignment, remaining);
    }
    return remainingCount != 0;
}

bool propagate(const std::vector<Clause>& clauses, Assignment& assignment) {
    bool hasChanged = true;
    while (hasChanged) {
        hasChanged = false;
        for (const Clause& clause : clauses) {
            if (!propagateClause(clause, assignment, hasChanged)) {
                return false;
            }
        }
    }
    return true;
}

std::uint64_t assignmentMask(const Assignment& assignment, std::size_t atomCount) {
    std::uint64_t mask = 0;
    for (std::size_t index = 0; index < atomCount; ++index) {
        if (assignment[index + 1] == isTrue) {
            mask |= std::uint64_t{1} << index;
        }
    }
    return mask;
}

// Highest atom first and false before true preserve enumeration's first
// witness. Auxiliary variables follow from the atoms through propagation
std::optional<std::uint64_t> search(const std::vector<Clause>& clauses,
                                    Assignment assignment, std::size_t atomCount) {
    if (!propagate(clauses, assignment)) {
        return std::nullopt;
    }
    for (std::size_t index = atomCount; index > 0; --index) {
        if (assignment[index] != unassigned) {
            continue;
        }
        assignment[index] = isFalse;
        if (const auto result = search(clauses, assignment, atomCount)) {
            return result;
        }
        assignment[index] = isTrue;
        return search(clauses, std::move(assignment), atomCount);
    }
    return assignmentMask(assignment, atomCount);
}

std::optional<std::uint64_t> Formula::satisfy(const std::vector<int>& roots,
                                            std::size_t atomCount) const {
    Assignment assignment(variableCount + 1, unassigned);
    for (const int root : roots) {
        if (root == 0 || !assignLiteral(assignment, root)) {
            return std::nullopt;
        }
    }
    return search(clauses, std::move(assignment), atomCount);
}

}  // namespace

RequirementAtoms collectRequirementAtoms(const Ast& ast, const RequirementsDecl& block) {
    RequirementAtoms atoms;
    for (const std::optional<ReqId>& line : {block.fulfill, block.partial, block.breakLine}) {
        if (line) {
            collectAtoms(ast, *line, atoms);
        }
    }
    return atoms;
}

RequirementVerdict searchRequirements(const Ast& ast, const RequirementsDecl& block,
                                      const RequirementAtoms& atoms) {
    Formula formula{{}, atoms.size()};
    const int fulfill = block.fulfill ? formula.encode(ast, *block.fulfill, atoms) : 0;
    const int breakLine = block.breakLine ? formula.encode(ast, *block.breakLine, atoms) : 0;
    return RequirementVerdict{formula.satisfy({fulfill}, atoms.size()).has_value(),
                              formula.satisfy({breakLine}, atoms.size()).has_value(),
                              formula.satisfy({fulfill, breakLine}, atoms.size())};
}

}  // namespace geas
