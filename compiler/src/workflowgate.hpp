// The gate in front of code generation for the engine driven half of the
// workflow surface: the course, amends, await, select, sleep, together, and
// now. The host driven half, terms, remedies, signals, the term attribute,
// the Duration and Time types, and the generic constructors, lowers onto the
// runtime's descriptor tail and its signal and clock surface; what the gate
// still names lands with the engine's journal, and a program that uses any
// of it is refused whole before a byte is emitted, never lowered halfway.
//
// This is the first of the two seams the course lowering lands on: when an
// emitter learns a construct, its entry leaves this scan. The second is
// emitworkflow.cpp, where the body level constructs get their lowering
#pragma once

#include <string>
#include <vector>

#include "check.hpp"

namespace geas {

/**
 * Finds every workflow construct a checked program uses.
 * @params checked - a checked program
 * @returns {std::vector<std::string>} - one description per kind of construct in first appearance
 * order, empty when the program stays inside what the emitters lower
 */
std::vector<std::string> findWorkflowConstructs(const CheckedProgram& checked);

}  // namespace geas
