// The module loader. Given a root .geas file it loads the whole program:
// every import resolves to a file, every file lexes and parses into one
// shared tree, and the merged declaration list is what the checker walks, so
// one module comes out however many files went in.
//
// The search order per import: the dotted path becomes a relative file path,
// util is util.geas and foo.bar is foo/bar.geas, probed first beside the
// root file, then under GEAS_HOME when that is set, then lib/ in the working
// directory, then /usr/local/share/geas and /usr/share/geas. Imports walk
// depth first in declaration order, each file loads once, and an import that
// lands on a file still being loaded is a cycle.
//
// The text of every file sits in one program buffer, each file at its own
// base with one byte between neighbors, so a span is unique across the whole
// program and maps back to its file when a fault prints. The directory a
// file sits in is its package
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ast.hpp"
#include "diagnostics.hpp"

namespace geas {

// One loaded file, where its text sits in the program buffer, and its package
struct LoadedFile {
    std::string path;
    std::int64_t base;
    std::int64_t length;
    int package;
};

// A loaded program: the merged tree, the buffer and files behind its spans,
// one package id per declaration in declaration order, and the load faults
// already rendered. A program that is not ok must not be checked further
struct Program {
    Ast ast;
    std::string text;
    std::vector<LoadedFile> files;
    std::vector<int> declPackages;
    std::vector<std::string> loadErrors;
    bool isOk = true;
};

/**
 * Loads the whole program rooted at a file, imports included.
 * @params rootPath - the root .geas file
 * @returns {Program} - the merged program, isOk false when anything faulted
 */
Program loadProgram(std::string_view rootPath);

/**
 * Renders a fault whose span indexes the program buffer against the file it
 * came from. A span outside every file falls back to the first file at
 * offset zero so the message is never lost.
 * @params program - the loaded program, diagnostic - the fault to render
 * @returns {std::string} - the rendered block, no trailing newline
 */
std::string renderProgramDiagnostic(const Program& program, const Diagnostic& diagnostic);

}  // namespace geas
