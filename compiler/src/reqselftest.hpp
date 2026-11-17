// Internal requirements solver cross check
#pragma once

#include <span>

namespace geas {

/**
 * Compares satisfiability search with enumeration on sources and random trees
 * @params paths - source paths whose requirements blocks are cross checked
 * @returns {int} - zero on agreement, one on a read error or disagreement
 */
int runRequirementsSelfTest(std::span<char*> paths);

}  // namespace geas
