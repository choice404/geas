// The writing primitives of the parse dump and its entry point, see
// astdumper.hpp
#include "astdumper.hpp"

#include <utility>

#include "astdump.hpp"

namespace geas {

namespace {

constexpr std::string_view indentUnit = "    ";

}  // namespace

std::string_view wrapKindName(WrapKind kind) {
    switch (kind) {
        case WrapKind::Some: return "Some";
        case WrapKind::Ok: return "Ok";
        case WrapKind::Err: return "Err";
        case WrapKind::Mut: return "Mut";
    }
    return "Mut";
}

std::string dumpAst(const Ast& ast) {
    return AstDumper{ast}.run();
}

AstDumper::AstDumper(const Ast& ast) : ast_{ast} {}

// File { imports: [...], decls: [...], }
std::string AstDumper::run() && {
    write("File {\n");
    key(1, "imports");
    renderImports();
    endField();
    key(1, "decls");
    list(ast_.decls, 1, [&](const Decl& decl, int level) { renderDecl(decl, level); });
    endField();
    write("}");
    return std::move(out_);
}

void AstDumper::write(std::string_view text) {
    out_.append(text);
}

void AstDumper::indent(int level) {
    for (int i = 0; i < level; ++i) {
        out_.append(indentUnit);
    }
}

void AstDumper::quoted(std::string_view text) {
    out_.push_back('"');
    out_.append(text);
    out_.push_back('"');
}

void AstDumper::boolean(bool value) {
    write(value ? "true" : "false");
}

void AstDumper::openStruct(std::string_view name) {
    write(name);
    write(" {\n");
}

// The start of one field line, the indentation and "name: "
void AstDumper::key(int level, std::string_view name) {
    indent(level);
    write(name);
    write(": ");
}

void AstDumper::endField() {
    write(",\n");
}

void AstDumper::closeStruct(int level) {
    indent(level);
    write("}");
}

// A one argument wrapper, Name(\n <inner>,\n )
void AstDumper::openWrap(std::string_view name, int level) {
    write(name);
    write("(\n");
    indent(level + 1);
}

void AstDumper::closeWrap(int level) {
    endField();
    indent(level);
    write(")");
}

}  // namespace geas
