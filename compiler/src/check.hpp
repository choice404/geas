// The front end as one call: load the program rooted at a file, resolve its
// names, check its types, and solve its requirements. A program that
// faulted while loading never reaches the resolver, since the recovery
// placeholders in its tree would only echo the earlier faults, and one that
// faulted in the resolver never reaches the type checker, whose signature
// tables assume every name is unique.
//
// What comes back is everything code generation needs: the tree, the name
// tables, the type arena, the lowered signatures, the per expression side
// tables, and one flattened shape per contract. The tables point into the
// tree, so a CheckedProgram is moved, never copied
#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "checked.hpp"
#include "loader.hpp"
#include "resolve.hpp"
#include "shape.hpp"
#include "sigs.hpp"
#include "types.hpp"

namespace geas {

struct CheckedProgram {
    Program program;
    Resolved resolved;
    TypeArena types;
    Sigs sigs;
    CheckTables tables;
    std::vector<ContractShape> contracts;

    CheckedProgram() = default;
    CheckedProgram(CheckedProgram&&) = default;
    CheckedProgram& operator=(CheckedProgram&&) = default;
    CheckedProgram(const CheckedProgram&) = delete;
    CheckedProgram& operator=(const CheckedProgram&) = delete;
};

/**
 * Runs the whole front end over the program rooted at a file.
 * @params rootPath - the root .geas file
 * @returns {std::expected<CheckedProgram, std::vector<std::string>>} - the checked program, or
 * every fault already rendered, in the order they print
 */
std::expected<CheckedProgram, std::vector<std::string>> checkProgram(std::string_view rootPath);

/**
 * Holds every amends block's restart course to the two courses it bridges,
 * run once the courses have lowered and their hashes are known.
 * @params checked - a checked program with its courses lowered
 * @returns {std::vector<Diagnostic>} - one fault per block whose course rule fails
 */
std::vector<Diagnostic> checkAmendCourses(const CheckedProgram& checked);

/**
 * The debug listing behind geas dump-shape: for every contract that is not
 * internal, its shape string as a comment, its shape hash, and the mangled
 * name of each pledge and remedy, spelled the way the C header spells them.
 * @params checked - a checked program
 * @returns {std::string} - the listing, one line per entry
 */
std::string dumpShapes(const CheckedProgram& checked);

}  // namespace geas
