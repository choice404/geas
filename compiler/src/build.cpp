// The artifact writing commands, see build.hpp
#include "build.hpp"

#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "check.hpp"
#include "codegen.hpp"
#include "emittext.hpp"

namespace geas {

namespace {

constexpr int exitSuccess = 0;
constexpr int exitFailure = 1;
constexpr std::string_view outputDirectory = "target/geas-out";
constexpr std::string_view checkoutProbe = "runtime/include/geas/geas.h";
constexpr std::string_view rootVariable = "GEAS_ROOT";

void report(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fputc('\n', stderr);
}

int reportFaults(const Program& program, const std::vector<Diagnostic>& faults) {
    for (const Diagnostic& fault : faults) {
        report(renderProgramDiagnostic(program, fault));
    }
    return exitFailure;
}

// The root file's name with its directories and its last extension dropped
std::string stemOf(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    return std::string{name.substr(0, name.rfind('.'))};
}

// The front end. Prints every fault and hands back nothing when the program
// cannot be emitted
std::expected<CheckedProgram, int> checkForEmit(std::string_view path) {
    auto checked = checkProgram(path);
    if (!checked) {
        for (const std::string& fault : checked.error()) {
            report(fault);
        }
        return std::unexpected(exitFailure);
    }
    return std::move(*checked);
}

bool makeOutputDirectory() {
    std::error_code error;
    std::filesystem::create_directories(outputDirectory, error);
    if (error) {
        report("geas: could not create the output directory");
        return false;
    }
    return true;
}

// Writes a whole file, false when any byte failed to land
bool writeFile(const std::string& path, std::string_view text) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const bool isWritten = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    const bool isClosed = std::fclose(file) == 0;
    return isWritten && isClosed;
}

std::string outputPath(std::string_view name) {
    return joined(outputDirectory, "/", name);
}

// One shell argument: a leading space, then the argument in single quotes
// so an awkward path survives the shell as one word
std::string shellArg(std::string_view arg) {
    std::string out{" '"};
    for (const char c : arg) {
        if (c == '\'') {
            out.append("'\\''");
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

// The directory under the toolchain root a build reads, the root being
// GEAS_ROOT when set and the working directory when it is a checkout. Empty
// under the installed layout, where cc and ld search their own paths
std::string runtimeDirectory(std::string_view relative) {
    const char* root = std::getenv(std::string{rootVariable}.c_str());
    if (root != nullptr && *root != '\0') {
        return joined(root, "/", relative);
    }
    std::error_code error;
    if (std::filesystem::is_regular_file(checkoutProbe, error)) {
        return std::string{relative};
    }
    return "";
}

// Runs one shell command, true when it exited zero
bool runCommand(const std::string& command) {
    std::fflush(stderr);
    return std::system(command.c_str()) == 0;
}

// Both artifact kinds name the runtime and find it beside the artifact
std::string runtimeLinkFlags() {
    const std::string library = runtimeDirectory(outputDirectory);
    std::string flags;
    if (!library.empty()) {
        flags.append(joined(" -L", shellArg(library)));
    }
    flags.append(joined(" -lgeasrt", shellArg("-Wl,-rpath,$ORIGIN")));
    if (!library.empty()) {
        flags.append(shellArg(joined("-Wl,-rpath,", library)));
    }
    return flags;
}

// Compiles the emitted C into a shared library with cc
bool linkModule(const std::string& modulePath, const std::string& libraryPath) {
    const std::string include = runtimeDirectory("runtime/include");
    std::string command = joined("cc -shared -fPIC", shellArg(modulePath));
    if (!include.empty()) {
        command.append(joined(" -I", shellArg(include)));
    }
    command.append(runtimeLinkFlags());
    command.append(joined(" -o", shellArg(libraryPath)));
    report("[geas] linking with cc");
    if (!runCommand(command)) {
        report("geas: cc failed to link the module");
        return false;
    }
    return true;
}

// Links the entry wrapper and the module C into a standalone executable
// against libgeasrt. The $ORIGIN rpath stays under every layout, since a
// binary shipped next to its runtime is the point of it
bool linkExecutable(const std::string& wrapperPath, const std::string& modulePath,
                    const std::string& binaryPath) {
    const std::string include = runtimeDirectory("runtime/include");
    std::string command = joined("cc", shellArg(wrapperPath), shellArg(modulePath));
    if (!include.empty()) {
        command.append(joined(" -I", shellArg(include)));
    }
    command.append(runtimeLinkFlags());
    command.append(joined(" -o", shellArg(binaryPath)));
    report("[geas] linking the executable with cc");
    if (!runCommand(command)) {
        report("geas: cc failed to link the executable");
        return false;
    }
    return true;
}

// Writes the entry wrapper beside the module C and links the pair
int finishExecutable(const std::string& stem, const std::string& modulePath) {
    const std::string wrapperPath = outputPath(joined(stem, ".main.c"));
    if (!writeFile(wrapperPath, emitMainWrapper(stem))) {
        report("geas: could not write the entry wrapper C");
        return exitFailure;
    }
    report("[geas] emitted the entry wrapper C");
    if (!linkExecutable(wrapperPath, modulePath, outputPath(stem))) {
        return exitFailure;
    }
    report("[geas] wrote the executable");
    return exitSuccess;
}

}  // namespace

int runBuild(std::string_view path, bool isBin) {
    auto checked = checkForEmit(path);
    if (!checked) {
        return checked.error();
    }
    if (isBin) {
        const std::vector<Diagnostic> faults = checkMainContract(*checked);
        if (!faults.empty()) {
            return reportFaults(checked->program, faults);
        }
    }
    const auto moduleText = emitModuleC(*checked);
    if (!moduleText) {
        return reportFaults(checked->program, moduleText.error());
    }
    if (!makeOutputDirectory()) {
        return exitFailure;
    }
    const std::string stem = stemOf(path);
    const std::string modulePath = outputPath(joined(stem, ".c"));
    if (!writeFile(modulePath, *moduleText)) {
        report("geas: could not write the module C");
        return exitFailure;
    }
    report("[geas] emitted module C");
    if (isBin) {
        return finishExecutable(stem, modulePath);
    }
    if (!linkModule(modulePath, outputPath(joined("lib", stem, ".geas.so")))) {
        return exitFailure;
    }
    report("[geas] wrote the module library");
    return exitSuccess;
}

int runEmitHeader(std::string_view path) {
    auto checked = checkForEmit(path);
    if (!checked) {
        return checked.error();
    }
    const std::string stem = stemOf(path);
    const auto header = emitHeader(*checked, stem);
    if (!header) {
        return reportFaults(checked->program, header.error());
    }
    if (!makeOutputDirectory()) {
        return exitFailure;
    }
    if (!writeFile(outputPath(joined(stem, ".geas.h")), *header)) {
        report("geas: could not write the header");
        return exitFailure;
    }
    report("[geas] wrote the header");
    return exitSuccess;
}

int runEmitProto(std::string_view path) {
    auto checked = checkForEmit(path);
    if (!checked) {
        return checked.error();
    }
    const std::string stem = stemOf(path);
    const auto files = emitProto(*checked, stem);
    if (!files) {
        return reportFaults(checked->program, files.error());
    }
    if (!makeOutputDirectory()) {
        return exitFailure;
    }
    if (!writeFile(outputPath(joined(stem, ".proto")), files->proto)) {
        report("geas: could not write the proto");
        return exitFailure;
    }
    const bool hasWrappers = writeFile(outputPath(joined(stem, "_session.go")), files->goSession) &&
                             writeFile(outputPath(joined(stem, "_session.ts")), files->tsSession);
    if (!hasWrappers) {
        report("geas: could not write the session wrapper");
        return exitFailure;
    }
    report("[geas] wrote the proto and the session wrappers");
    return exitSuccess;
}

}  // namespace geas
