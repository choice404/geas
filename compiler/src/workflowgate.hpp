// The gate in front of code generation for the one workflow construct the
// emitters do not lower: an amends block, which migrates a parked instance
// of an earlier version. Everything else of the workflow surface lowers,
// the host driven half onto the descriptor tail and the signal and clock
// surface, and the course onto a resumable C function the runtime drives. A
// program with an amends block is refused whole before a byte is emitted,
// never lowered halfway
#pragma once

#include <string>
#include <vector>

#include "check.hpp"

namespace geas {

/**
 * Finds every workflow construct a checked program uses that the emitters
 * do not lower.
 * @params checked - a checked program
 * @returns {std::vector<std::string>} - one description per amends block in declaration order,
 * empty when the program stays inside what the emitters lower
 */
std::vector<std::string> findWorkflowConstructs(const CheckedProgram& checked);

}  // namespace geas
