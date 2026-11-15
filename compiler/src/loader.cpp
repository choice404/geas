// The module loader, see loader.hpp
#include "loader.hpp"

#include <cstdlib>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "lexer.hpp"
#include "parser.hpp"
#include "source.hpp"

namespace geas {

namespace {

constexpr char pathSeparator = '/';
constexpr std::string_view sourceExtension = ".geas";
constexpr std::string_view homeVariable = "GEAS_HOME";
constexpr std::string_view checkoutLibrary = "lib";
constexpr std::string_view localShare = "/usr/local/share/geas";
constexpr std::string_view systemShare = "/usr/share/geas";

// Where a path stands in the walk, loading while its imports are still open
enum class LoadStatus { Loading, Done };

// The directory of a path, "." when the path has no separator
std::string directoryOf(std::string_view path) {
    const std::size_t last = path.rfind(pathSeparator);
    if (last == std::string_view::npos) {
        return ".";
    }
    if (last == 0) {
        return "/";
    }
    return std::string{path.substr(0, last)};
}

// A dotted import path as a relative file path, foo.bar is foo/bar.geas
std::string importRelativePath(std::string_view importPath) {
    std::string out;
    for (const char c : importPath) {
        out.push_back(c == '.' ? pathSeparator : c);
    }
    out.append(sourceExtension);
    return out;
}

std::string pathUnder(std::string_view directory, std::string_view relative) {
    std::string out{directory};
    out.push_back(pathSeparator);
    out.append(relative);
    return out;
}

bool isRegularFile(const std::string& path) {
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0) {
        return false;
    }
    return S_ISREG(info.st_mode);
}

// The search roots tried after the root file's own directory, in order
std::vector<std::string> searchHomes() {
    std::vector<std::string> homes;
    const char* home = std::getenv(std::string{homeVariable}.c_str());
    if (home != nullptr && home[0] != '\0') {
        homes.emplace_back(home);
    }
    homes.emplace_back(checkoutLibrary);
    homes.emplace_back(localShare);
    homes.emplace_back(systemShare);
    return homes;
}

class Loader {
public:
    explicit Loader(std::string_view rootPath)
        : rootDirectory_{directoryOf(rootPath)}, homes_{searchHomes()} {}

    Program run(std::string_view rootPath) &&;

private:
    void loadFile(const std::string& path);
    bool parseFile(const std::string& path, const Source& source, std::int64_t base);
    void loadImports(const std::string& path, const LoadedFile& file, std::size_t importsBegin,
                     std::size_t importsEnd);
    void loadImport(const std::string& path, const LoadedFile& file, const Import& import);
    std::optional<std::string> resolveImport(std::string_view importPath) const;
    void fileError(const LoadedFile& file, std::int64_t lo, std::int64_t hi, std::string message);
    int packageOf(const std::string& directory);
    void injectBuiltins();

    Program program_;
    std::unordered_map<std::string, LoadStatus> status_;
    std::unordered_map<std::string, int> packages_;
    std::string rootDirectory_;
    std::vector<std::string> homes_;
};

Program Loader::run(std::string_view rootPath) && {
    loadFile(std::string{rootPath});
    injectBuiltins();
    return std::move(program_);
}

// The package id of a directory, handed out in first encounter order
int Loader::packageOf(const std::string& directory) {
    const auto found = packages_.find(directory);
    if (found != packages_.end()) {
        return found->second;
    }
    const int id = static_cast<int>(packages_.size());
    packages_.emplace(directory, id);
    return id;
}

// Renders one load fault against its file, the span local to that file
void Loader::fileError(const LoadedFile& file, std::int64_t lo, std::int64_t hi,
                       std::string message) {
    const std::string_view text = std::string_view{program_.text}.substr(
        static_cast<std::size_t>(file.base), static_cast<std::size_t>(file.length));
    program_.loadErrors.push_back(
        renderDiagnostic(file.path, text, Diagnostic{lo, hi, std::move(message)}));
    program_.isOk = false;
}

// Lexes and parses one file into the shared tree. The tokens are shifted to
// the file's base so every span the parser writes is already program wide.
// Returns whether the file came through with no fault
bool Loader::parseFile(const std::string& path, const Source& source, std::int64_t base) {
    const auto length = static_cast<std::int64_t>(source.text.size());
    const LoadedFile file{path, base, length, 0};
    LexResult lexed = lex(source.text);
    for (Token& token : lexed.tokens) {
        token.lo += base;
        token.hi += base;
    }
    ParseResult parsed = parseInto(program_.text, lexed.tokens, std::move(program_.ast));
    program_.ast = std::move(parsed.ast);
    for (const Diagnostic& fault : lexed.diagnostics) {
        fileError(file, fault.lo, fault.hi, fault.message);
    }
    for (const Diagnostic& fault : parsed.diagnostics) {
        fileError(file, fault.lo - base, fault.hi - base, fault.message);
    }
    return lexed.diagnostics.empty() && parsed.diagnostics.empty();
}

// Loads one file and, depth first, everything it imports. The file is marked
// loading while its imports walk, which is what makes a cycle observable
void Loader::loadFile(const std::string& path) {
    status_[path] = LoadStatus::Loading;
    const auto source = loadSource(path);
    if (!source) {
        program_.loadErrors.push_back("geas: cannot read source file '" + path + "'");
        program_.isOk = false;
        status_[path] = LoadStatus::Done;
        return;
    }
    const auto base = static_cast<std::int64_t>(program_.text.size());
    program_.text.append(source->text);
    program_.text.push_back('\n');
    const std::size_t importsBegin = program_.ast.imports.size();
    const bool isClean = parseFile(path, *source, base);
    const int package = packageOf(directoryOf(path));
    const auto length = static_cast<std::int64_t>(source->text.size());
    const LoadedFile file{path, base, length, package};
    program_.files.push_back(file);
    program_.declPackages.resize(program_.ast.decls.size(), package);
    status_[path] = LoadStatus::Done;
    // A faulted parse stops here, its import list may be recovery debris
    if (!isClean) {
        return;
    }
    loadImports(path, file, importsBegin, program_.ast.imports.size());
}

// The file's imports in declaration order, a repeated path reported once
void Loader::loadImports(const std::string& path, const LoadedFile& file, std::size_t importsBegin,
                         std::size_t importsEnd) {
    std::unordered_set<std::string> seen;
    for (std::size_t i = importsBegin; i < importsEnd; ++i) {
        // Copied, a nested load grows the import list under a reference
        const Import import = program_.ast.imports[i];
        if (import.path.empty()) {
            continue;
        }
        if (!seen.insert(import.path).second) {
            fileError(file, import.lo - file.base, import.hi - file.base,
                      "duplicate import '" + import.path + "'");
            continue;
        }
        loadImport(path, file, import);
    }
}

void Loader::loadImport(const std::string& path, const LoadedFile& file, const Import& import) {
    const std::int64_t lo = import.lo - file.base;
    const std::int64_t hi = import.hi - file.base;
    const std::optional<std::string> target = resolveImport(import.path);
    if (!target) {
        fileError(file, lo, hi, "cannot resolve import '" + import.path + "'");
        return;
    }
    const auto found = status_.find(*target);
    if (found == status_.end()) {
        // The importer is marked loading again around the walk, so a cycle
        // back into this file is caught however deep it closes
        status_[path] = LoadStatus::Loading;
        loadFile(*target);
        status_[path] = LoadStatus::Done;
        return;
    }
    if (found->second == LoadStatus::Loading) {
        fileError(file, lo, hi, "import cycle detected through '" + import.path + "'");
    }
}

// Resolves an import to a file, beside the root first and then under each
// home in turn. A path already seen counts as found without touching disk
std::optional<std::string> Loader::resolveImport(std::string_view importPath) const {
    const std::string relative = importRelativePath(importPath);
    const auto answers = [&](const std::string& candidate) {
        return status_.contains(candidate) || isRegularFile(candidate);
    };
    std::string beside = pathUnder(rootDirectory_, relative);
    if (answers(beside)) {
        return beside;
    }
    for (const std::string& home : homes_) {
        std::string candidate = pathUnder(home, relative);
        if (answers(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

// Appends the builtin records the surface names but no file declares, as one
// last declaration in the root package. PartialResult is what
// instance.partial() answers: the state and the three name lists the
// requirements policy sorts its pledges into. It carries no span, so a fault
// on it lands on the first file at offset zero
void Loader::injectBuiltins() {
    Ast& ast = program_.ast;
    const TypeId stringType = ast.addType(NamedType{"String", {}});
    const TypeId listType = ast.addType(NamedType{"List", {stringType}});
    RecordDecl record{"PartialResult", 0, 0, {}};
    record.fields.push_back(Field{"state", stringType});
    record.fields.push_back(Field{"fulfilled", listType});
    record.fields.push_back(Field{"pending", listType});
    record.fields.push_back(Field{"broken", listType});
    ast.decls.emplace_back(std::move(record));
    program_.declPackages.push_back(0);
}

}  // namespace

Program loadProgram(std::string_view rootPath) {
    return Loader{rootPath}.run(rootPath);
}

std::string renderProgramDiagnostic(const Program& program, const Diagnostic& diagnostic) {
    const std::string_view all{program.text};
    for (const LoadedFile& file : program.files) {
        if (diagnostic.lo < file.base || diagnostic.lo > file.base + file.length) {
            continue;
        }
        const std::string_view text =
            all.substr(static_cast<std::size_t>(file.base), static_cast<std::size_t>(file.length));
        return renderDiagnostic(
            file.path, text,
            Diagnostic{diagnostic.lo - file.base, diagnostic.hi - file.base, diagnostic.message});
    }
    if (program.files.empty()) {
        return diagnostic.message;
    }
    const LoadedFile& first = program.files.front();
    const std::string_view text =
        all.substr(static_cast<std::size_t>(first.base), static_cast<std::size_t>(first.length));
    return renderDiagnostic(first.path, text, Diagnostic{0, 0, diagnostic.message});
}

}  // namespace geas
