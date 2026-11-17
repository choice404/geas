// Requirements satisfiability uses unit propagation and atom branching.
// An overlapping fulfill and break line reports the first witness in atom
// assignment order. An unsatisfiable line reports a dead policy
#pragma once

#include <vector>

#include "ast.hpp"
#include "diagnostics.hpp"

namespace geas {

/**
 * Checks the first requirements block of every contract.
 * @params ast - the program tree, diagnostics - where faults land
 * @returns {void} - diagnostics receive overlap and dead policy faults
 */
void solveRequirements(const Ast& ast, std::vector<Diagnostic>& diagnostics);

}  // namespace geas
