// The commands that write artifacts: build, build --bin, emit-header, and
// emit-proto. Each runs the whole front end over the program rooted at a
// file, hands the checked tree to its emitter, and writes into
// target/geas-out under the working directory, every file named after the
// root file's stem. build then links
// with cc: the module C into a shared library, or with --bin the module and
// its entry wrapper into an executable against libgeasrt.
//
// build finds the runtime three ways: GEAS_ROOT when it is set, the
// relative runtime/include and target/geas-out paths when the working
// directory is a checkout, and the compiler's own system search paths when
// it is neither, which is how an installed toolchain works with no
// environment set at all. Progress and faults go to stderr and nothing goes
// to stdout
#pragma once

#include <string_view>

namespace geas {

/**
 * Builds one program from its root file: the module C, then a shared
 * library, or an executable around the Main contract.
 * @params path - the root .geas file, isBin - whether to link a standalone executable
 * @returns {int} - the process exit code, 0 on success
 */
int runBuild(std::string_view path, bool isBin);

/**
 * Writes the C header naming the module's shape hashes and mangled names.
 * @params path - the root .geas file
 * @returns {int} - the process exit code, 0 on success
 */
int runEmitHeader(std::string_view path);

/**
 * Writes the gRPC .proto and the Go and TypeScript session wrappers.
 * @params path - the root .geas file
 * @returns {int} - the process exit code, 0 on success
 */
int runEmitProto(std::string_view path);

}  // namespace geas
