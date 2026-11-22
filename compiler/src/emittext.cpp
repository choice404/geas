// The template filler, see emittext.hpp
#include "emittext.hpp"

#include <array>
#include <charconv>
#include <system_error>

namespace geas {

namespace {

// Room for the longest shortest round trip spelling of a double
constexpr std::size_t floatTextCapacity = 32;

// Replaces every occurrence of one marker
std::string replaceMarker(std::string_view text, std::string_view marker,
                          std::string_view replacement) {
    std::string out;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const std::size_t found = text.find(marker, cursor);
        if (found == std::string_view::npos) {
            break;
        }
        out.append(text.substr(cursor, found - cursor));
        out.append(replacement);
        cursor = found + marker.size();
    }
    out.append(text.substr(cursor));
    return out;
}

}  // namespace

std::string cFloat(std::string_view spelling) {
    std::string digits;
    for (const char c : spelling) {
        if (c != '_') {
            digits.push_back(c);
        }
    }
    double value = 0.0;
    const char* end = digits.data() + digits.size();
    const auto parsed = std::from_chars(digits.data(), end, value);
    if (parsed.ptr != end) {
        return digits;
    }
    // Past the range is refused by the lexer, so out of range here is a
    // value below the least subnormal, which reads as zero
    if (parsed.ec == std::errc::result_out_of_range) {
        value = 0.0;
    } else if (parsed.ec != std::errc{}) {
        return digits;
    }
    std::array<char, floatTextCapacity> text{};
    const auto written = std::to_chars(text.data(), text.data() + text.size(), value);
    std::string out{text.data(), written.ptr};
    // A whole value spells as bare digits, which C would read as an integer
    if (out.find_first_of(".eE") == std::string::npos) {
        out.append(".0");
    }
    return out;
}

std::string fillTemplate(std::string_view text, std::initializer_list<TemplateFill> fills) {
    std::string out{text};
    for (const TemplateFill& fill : fills) {
        out = replaceMarker(out, fill.first, fill.second);
    }
    return out;
}

}  // namespace geas
