// The text helpers every emitter shares: joining string pieces into one
// line, spelling a number, and filling a template whose blanks are @NAME@
// markers. The emitted files are pinned byte for byte by the codegen
// goldens, so every line is built from exact pieces
#pragma once

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace geas {

/**
 * Joins string pieces into one string, in order.
 * @params parts - anything a string view can be built from
 * @returns {std::string} - the pieces end to end
 */
template <typename... Parts>
std::string joined(const Parts&... parts) {
    std::string out;
    (out.append(std::string_view{parts}), ...);
    return out;
}

/**
 * A signed integer as decimal text.
 * @params value - the number to spell
 * @returns {std::string} - its decimal digits
 */
inline std::string decimal(std::int64_t value) {
    return std::to_string(value);
}

/**
 * A signed integer as C source. The least Int is spelled as an expression,
 * since its magnitude has no signed C constant and a bare negated one would
 * read as unsigned.
 * @params value - the number to spell
 * @returns {std::string} - its decimal digits, or the expression for the least Int
 */
inline std::string cInteger(std::int64_t value) {
    if (value == std::numeric_limits<std::int64_t>::min()) {
        return "(-9223372036854775807 - 1)";
    }
    return decimal(value);
}

/**
 * A Float literal as C source: the shortest decimal that reads back as the
 * same double, with a dot or an exponent so C reads a double and not an
 * integer. Underscores are separators in Geas and nothing in C, so the
 * value is what is spelled, never the source text. A value below the
 * least subnormal is spelled as zero, the double it rounds to, and a
 * spelling that does not read as a double is kept as written, underscores
 * dropped.
 * @params spelling - the literal as the source wrote it
 * @returns {std::string} - a C double constant of the same value
 */
std::string cFloat(std::string_view spelling);

// One blank of a template and the text that fills it
using TemplateFill = std::pair<std::string_view, std::string_view>;

/**
 * Fills every @NAME@ marker of a template.
 * @params text - the template, fills - each marker with its replacement
 * @returns {std::string} - the filled text
 */
std::string fillTemplate(std::string_view text, std::initializer_list<TemplateFill> fills);

}  // namespace geas
