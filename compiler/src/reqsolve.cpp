// Requirements satisfiability, see reqsolve.hpp
#include "reqsolve.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "quote.hpp"

namespace geas {

namespace {

constexpr std::size_t atomLimit = 16;

// The distinct atoms of one block in first appearance order, the order the
// witness rendering follows so the fault reads the way the block was written
struct AtomTable {
    std::vector<std::string> names;
    std::unordered_map<std::string, std::size_t> indices;
};

void collectAtoms(const Ast& ast, ReqId id, AtomTable& atoms) {
    const ReqNode& node = ast.req(id);
    if (const auto* atom = std::get_if<ReqAtom>(&node.kind)) {
        if (atoms.indices.emplace(atom->name, atoms.names.size()).second) {
            atoms.names.push_back(atom->name);
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

// Evaluates one line under one assignment, bit i of mask is atom i
bool evaluate(const Ast& ast, ReqId id, const AtomTable& atoms, std::uint32_t mask) {
    const ReqNode& node = ast.req(id);
    if (const auto* atom = std::get_if<ReqAtom>(&node.kind)) {
        const auto found = atoms.indices.find(atom->name);
        const std::size_t index = found == atoms.indices.end() ? 0 : found->second;
        return ((mask >> index) & 1U) == 1U;
    }
    if (const auto* negation = std::get_if<ReqNot>(&node.kind)) {
        return !evaluate(ast, negation->operand, atoms, mask);
    }
    if (const auto* conjunction = std::get_if<ReqAnd>(&node.kind)) {
        return evaluate(ast, conjunction->lhs, atoms, mask) &&
               evaluate(ast, conjunction->rhs, atoms, mask);
    }
    const auto& disjunction = std::get<ReqOr>(node.kind);
    return evaluate(ast, disjunction.lhs, atoms, mask) ||
           evaluate(ast, disjunction.rhs, atoms, mask);
}

// The witness assignment as the atom list, each name bare when true and
// negated when false
std::string witness(const AtomTable& atoms, std::uint32_t mask) {
    std::string out;
    for (std::size_t i = 0; i < atoms.names.size(); ++i) {
        if (i > 0) {
            out.append(", ");
        }
        if (((mask >> i) & 1U) == 0U) {
            out.push_back('!');
        }
        out.append(atoms.names[i]);
    }
    return out;
}

// What enumerating every assignment found about one block
struct Verdict {
    bool canFulfill = false;
    bool canBreak = false;
    std::optional<std::uint32_t> both;
};

Verdict enumerate(const Ast& ast, const RequirementsDecl& block, const AtomTable& atoms) {
    Verdict verdict;
    const std::uint32_t total = std::uint32_t{1} << atoms.names.size();
    for (std::uint32_t mask = 0; mask < total; ++mask) {
        const bool isFulfilled = block.fulfill && evaluate(ast, *block.fulfill, atoms, mask);
        const bool isBroken = block.breakLine && evaluate(ast, *block.breakLine, atoms, mask);
        verdict.canFulfill = verdict.canFulfill || isFulfilled;
        verdict.canBreak = verdict.canBreak || isBroken;
        if (isFulfilled && isBroken && !verdict.both) {
            verdict.both = mask;
        }
    }
    return verdict;
}

void solveBlock(const Ast& ast, const std::string& contractName, const RequirementsDecl& block,
                std::vector<Diagnostic>& diagnostics) {
    AtomTable atoms;
    for (const std::optional<ReqId>& line : {block.fulfill, block.partial, block.breakLine}) {
        if (line) {
            collectAtoms(ast, *line, atoms);
        }
    }
    if (atoms.names.size() > atomLimit) {
        return;
    }
    const Verdict verdict = enumerate(ast, block, atoms);
    const auto fault = [&](std::string message) {
        diagnostics.push_back(Diagnostic{block.lo, block.hi, std::move(message)});
    };
    const std::string contract = "contract " + quoted(contractName);
    if (verdict.both) {
        fault("requirements 'fulfill' and 'break' can both hold in " + contract + " when " +
              witness(atoms, *verdict.both));
    }
    if (block.fulfill && !verdict.canFulfill) {
        fault("requirements 'fulfill' can never hold in " + contract);
    }
    if (block.breakLine && !verdict.canBreak) {
        fault("requirements 'break' can never hold in " + contract);
    }
}

}  // namespace

void solveRequirements(const Ast& ast, std::vector<Diagnostic>& diagnostics) {
    for (const Decl& decl : ast.decls) {
        const auto* contract = std::get_if<ContractDecl>(&decl);
        if (contract == nullptr) {
            continue;
        }
        for (const Member& member : contract->members) {
            if (const auto* block = std::get_if<RequirementsDecl>(&member)) {
                solveBlock(ast, contract->name, *block, diagnostics);
                break;
            }
        }
    }
}

}  // namespace geas
