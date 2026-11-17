// Requirements satisfiability, see reqsolve.hpp
#include "reqsolve.hpp"

#include <cstdint>
#include <optional>
#include <string>

#include "quote.hpp"
#include "reqsat.hpp"

namespace geas {

namespace {

// The witness assignment as the atom list, each name bare when true and
// negated when false
std::string witness(const RequirementAtoms& atoms, std::uint64_t mask) {
    std::string out;
    for (std::size_t i = 0; i < atoms.size(); ++i) {
        if (i > 0) {
            out.append(", ");
        }
        if (((mask >> i) & 1U) == 0U) {
            out.push_back('!');
        }
        out.append(atoms[i]);
    }
    return out;
}

void solveBlock(const Ast& ast, const std::string& contractName, const RequirementsDecl& block,
                std::vector<Diagnostic>& diagnostics) {
    const RequirementAtoms atoms = collectRequirementAtoms(ast, block);
    if (atoms.size() > requirementsAtomLimit) {
        return;
    }
    const RequirementVerdict verdict = searchRequirements(ast, block, atoms);
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
