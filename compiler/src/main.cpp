// The geas command line: version, the front end commands lex, parse, and
// check, and the artifact commands build, emit-header, and emit-proto. The
// exit codes and the messages are part of the surface, the golden suites
// pin both
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "astdump.hpp"
#include "build.hpp"
#include "check.hpp"
#include "diagnostics.hpp"
#include "lexdump.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "reqselftest.hpp"
#include "source.hpp"

namespace {

constexpr std::string_view versionString = "geas 0.8.3";
constexpr int exitSuccess = 0;
constexpr int exitFailure = 1;

void printLine(std::FILE* stream, std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stream);
    std::fputc('\n', stream);
}

void printError(std::string_view text) {
    printLine(stderr, text);
}

void printUsage() {
    printError("usage: geas <command> [file]");
    printError("commands:");
    printError("  version       print the compiler version");
    printError("  lex <file>    dump the token stream of a source file");
    printError("  parse <file>  dump the AST of a source file");
    printError("  check <file>  load the program rooted at file and check it");
    printError("  build <file>  emit the module C and link it into a shared library");
    printError("  build --bin <file>  link a standalone executable around the Main contract");
    printError("  emit-header <file>  write the C header naming the module's hashes and symbols");
    printError(
        "  emit-proto <file>   write the gRPC .proto and the Go and TypeScript session wrappers");
}

void printDiagnostics(std::string_view path, std::string_view text,
                      const std::vector<geas::Diagnostic>& diagnostics) {
    for (const geas::Diagnostic& diagnostic : diagnostics) {
        printError(geas::renderDiagnostic(path, text, diagnostic));
    }
}

// Lexes the file at path, the dump on stdout and every fault on stderr after
// it. Fails when the file cannot load or the lexer recorded a fault, and the
// dump still prints so a faulted file shows what was scanned
int runLex(std::string_view path) {
    const auto source = geas::loadSource(path);
    if (!source) {
        printError("geas: cannot read source file: " +
                   std::string{geas::sourceErrorMessage(source.error())});
        return exitFailure;
    }
    const geas::LexResult result = geas::lex(source->text);
    std::string dump;
    for (const geas::Token& token : result.tokens) {
        dump.append(geas::renderToken(source->text, token));
        dump.push_back('\n');
    }
    std::fwrite(dump.data(), 1, dump.size(), stdout);
    std::fflush(stdout);
    printDiagnostics(path, source->text, result.diagnostics);
    return result.diagnostics.empty() ? exitSuccess : exitFailure;
}

// Lexes and parses the file at path, the tree dump on stdout and the lex
// faults then the parse faults on stderr. The parser runs over a faulted
// token stream too, so the dump always prints
int runParse(std::string_view path) {
    const auto source = geas::loadSource(path);
    if (!source) {
        printError("geas: cannot read source file: " +
                   std::string{geas::sourceErrorMessage(source.error())});
        return exitFailure;
    }
    const geas::LexResult lexed = geas::lex(source->text);
    const geas::ParseResult parsed = geas::parse(source->text, lexed.tokens);
    printLine(stdout, geas::dumpAst(parsed.ast));
    std::fflush(stdout);
    printDiagnostics(path, source->text, lexed.diagnostics);
    printDiagnostics(path, source->text, parsed.diagnostics);
    const bool isClean = lexed.diagnostics.empty() && parsed.diagnostics.empty();
    return isClean ? exitSuccess : exitFailure;
}

// Loads the whole program rooted at path and runs the resolver, the type
// checker, and the requirements solver over it. Nothing goes to stdout;
// faults go to stderr and decide the exit code, and a clean program reports
// one line so a caller watching stderr sees the pass happened
int runCheck(std::string_view path) {
    const auto checked = geas::checkProgram(path);
    if (!checked) {
        for (const std::string& fault : checked.error()) {
            printError(fault);
        }
        return exitFailure;
    }
    printError("[geas] check ok");
    return exitSuccess;
}

// The hidden debug command: checks the program, then prints every shape
// string, shape hash, and mangled name on stdout
int runDumpShape(std::string_view path) {
    const auto checked = geas::checkProgram(path);
    if (!checked) {
        for (const std::string& fault : checked.error()) {
            printError(fault);
        }
        return exitFailure;
    }
    const std::string listing = geas::dumpShapes(*checked);
    std::fwrite(listing.data(), 1, listing.size(), stdout);
    std::fflush(stdout);
    return exitSuccess;
}

int missingFile(std::string_view command) {
    printError("geas: " + std::string{command} + " needs a file argument");
    printUsage();
    return exitFailure;
}

// build takes its file after an optional --bin
int runBuildCommand(std::span<char*> args) {
    const bool isBin = std::string_view{args[2]} == "--bin";
    if (!isBin) {
        return geas::runBuild(args[2], false);
    }
    if (args.size() < 4) {
        return missingFile("build --bin");
    }
    return geas::runBuild(args[3], true);
}

// Runs the command that needs a file argument, or explains it is missing
int runFileCommand(std::span<char*> args, std::string_view command) {
    if (args.size() < 3) {
        return missingFile(command);
    }
    if (command == "build") {
        return runBuildCommand(args);
    }
    if (command == "emit-header") {
        return geas::runEmitHeader(args[2]);
    }
    if (command == "emit-proto") {
        return geas::runEmitProto(args[2]);
    }
    if (command == "parse") {
        return runParse(args[2]);
    }
    if (command == "check") {
        return runCheck(args[2]);
    }
    if (command == "dump-shape") {
        return runDumpShape(args[2]);
    }
    return runLex(args[2]);
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> args{argv, static_cast<std::size_t>(argc)};
    if (args.size() < 2) {
        printError("geas: no command given");
        printUsage();
        return exitFailure;
    }
    const std::string_view command = args[1];
    if (command == "version") {
        printLine(stdout, versionString);
        return exitSuccess;
    }
    if (command == "test-requirements") {
        return geas::runRequirementsSelfTest(args.subspan(2));
    }
    const bool isFrontEnd =
        command == "lex" || command == "parse" || command == "check" || command == "dump-shape";
    const bool isArtifact =
        command == "build" || command == "emit-header" || command == "emit-proto";
    if (isFrontEnd || isArtifact) {
        return runFileCommand(args, command);
    }
    printError("geas: unknown command");
    printUsage();
    return exitFailure;
}
