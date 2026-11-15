// Requirements satisfiability. The resolver capped a contract at 16 atoms,
// so the policy lines are small boolean formulas and satisfiability is a
// plain enumeration of every atom assignment. A fulfill line and a break
// line that can hold at once is the contradiction the grammar names,
// reported with the witness assignment that makes both true. A fulfill or
// break line no assignment satisfies is a dead policy, a line that can
// never fire. A contract with no requirements block keeps the synthesized
// defaults, which are sound by construction
#pragma once

#include <vector>

#include "ast.hpp"
#include "diagnostics.hpp"

namespace geas {

/**
 * Checks the first requirements block of every contract.
 * @params ast - the tree of a program the resolver accepted, diagnostics - where faults land
 */
void solveRequirements(const Ast& ast, std::vector<Diagnostic>& diagnostics);

}  // namespace geas
