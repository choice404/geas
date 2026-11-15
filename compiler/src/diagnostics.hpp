// Diagnostics located by a byte span. A fault is raised with the span the
// lexer knows and only resolved to a line and column when it prints, so the
// cost lands on the error path alone. The printed form is the path, the
// location, the message, then the source line with a caret run under the span
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace geas {

// A fault over the half open byte span [lo, hi)
struct Diagnostic {
    std::int64_t lo;
    std::int64_t hi;
    std::string message;
};

// A 1 based line and a column counted in Unicode scalars
struct LineColumn {
    std::int64_t line;
    std::int64_t column;
};

/**
 * The line and column of a byte offset, clamped back to a scalar boundary.
 * @params text - the source, offset - the byte offset to resolve
 * @returns {LineColumn} - the 1 based line and column
 */
LineColumn lineColumnAt(std::string_view text, std::int64_t offset);

/**
 * Renders a fault as `path: line:col: error: message` with the source line
 * and the caret run beneath, no trailing newline.
 * @params path - the file name to print, text - the source, diagnostic - the fault
 * @returns {std::string} - the rendered block
 */
std::string renderDiagnostic(std::string_view path, std::string_view text,
                             const Diagnostic& diagnostic);

}  // namespace geas
