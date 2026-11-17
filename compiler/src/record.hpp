// geas record FILE [--force]: runs the whole front end over the program
// rooted at a file, then writes the recorded shape of every public contract
// that file declares to shapes/<Contract>.v<version>.shape beside it,
// creating shapes/ when it is absent. An amends block of a later version
// is checked against that file. Recording the same shape again leaves the
// file as it is, and a file that already records a different shape of the
// same version is refused whole, nothing written, unless --force is given.
// Progress and faults go to stderr and nothing goes to stdout
#pragma once

#include <string_view>

namespace geas {

/**
 * Records the shape of every public contract a file declares.
 * @params path - the root .geas file, isForced - whether a differing recorded shape is replaced
 * @returns {int} - the process exit code, 0 on success
 */
int runRecord(std::string_view path, bool isForced);

}  // namespace geas
