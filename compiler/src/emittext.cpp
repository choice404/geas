// The template filler, see emittext.hpp
#include "emittext.hpp"

namespace geas {

namespace {

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

std::string fillTemplate(std::string_view text, std::initializer_list<TemplateFill> fills) {
    std::string out{text};
    for (const TemplateFill& fill : fills) {
        out = replaceMarker(out, fill.first, fill.second);
    }
    return out;
}

}  // namespace geas
