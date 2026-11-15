// Code generation as a handful of calls over a checked program: the module
// C, the entry wrapper and the entry point rule behind build --bin, the C
// header behind emit-header, and the gRPC surface behind emit-proto. Every
// emitter is deterministic in declaration order, so two runs over one
// source give byte identical text. An emitter that meets something it does
// not lower hands back faults instead of text, and the caller writes
// nothing. The emitters intern types as they go, so they take the checked
// program by mutable reference
#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "check.hpp"
#include "diagnostics.hpp"

namespace geas {

// The three files emit-proto writes
struct ProtoFiles {
    std::string proto;
    std::string goSession;
    std::string tsSession;
};

/**
 * Emits the module C of a checked program.
 * @params checked - the checked program
 * @returns {std::expected<std::string, std::vector<Diagnostic>>} - the translation unit, or the
 * build faults
 */
std::expected<std::string, std::vector<Diagnostic>> emitModuleC(CheckedProgram& checked);

/**
 * Checks the entry point rule of a standalone build: exactly one Main
 * contract, not internal, with a concrete run pledge outside any
 * subcontract whose signature is (List<String>) -> Result<Int, E>.
 * @params checked - the checked program
 * @returns {std::vector<Diagnostic>} - the faults, empty when the rule holds
 */
std::vector<Diagnostic> checkMainContract(CheckedProgram& checked);

/**
 * Emits the C main() that turns a module into a program: bring the runtime
 * up, register the module, sign Main, fulfill run over argv, and map the
 * outcome onto the process exit code.
 * @params stem - the root file's stem, the name the program reports under
 * @returns {std::string} - the wrapper translation unit
 */
std::string emitMainWrapper(std::string_view stem);

/**
 * Emits the C header a foreign host compiles against: one shape hash
 * define per public contract and one mangled name define per pledge.
 * @params checked - the checked program, stem - the root file's stem
 * @returns {std::expected<std::string, std::vector<Diagnostic>>} - the header, or the build faults
 */
std::expected<std::string, std::vector<Diagnostic>> emitHeader(CheckedProgram& checked,
                                                               std::string_view stem);

/**
 * Emits the gRPC surface: the .proto spelling every public contract as a
 * typed service, and the Go and TypeScript session wrappers beside it.
 * @params checked - the checked program, stem - the root file's stem
 * @returns {std::expected<ProtoFiles, std::vector<Diagnostic>>} - the three files, or the build
 * faults
 */
std::expected<ProtoFiles, std::vector<Diagnostic>> emitProto(CheckedProgram& checked,
                                                             std::string_view stem);

}  // namespace geas
