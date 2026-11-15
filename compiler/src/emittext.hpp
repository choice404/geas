// The text helpers every emitter shares: joining string pieces into one
// line, spelling a number, and filling a template whose blanks are @NAME@
// markers. The emitted files are pinned byte for byte by the codegen
// goldens, so every line is built from exact pieces
#pragma once

#include <cstdint>
#include <initializer_list>
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

// One blank of a template and the text that fills it
using TemplateFill = std::pair<std::string_view, std::string_view>;

/**
 * Fills every @NAME@ marker of a template.
 * @params text - the template, fills - each marker with its replacement
 * @returns {std::string} - the filled text
 */
std::string fillTemplate(std::string_view text, std::initializer_list<TemplateFill> fills);

}  // namespace geas
