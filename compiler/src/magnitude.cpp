// Numeric literal magnitudes, see magnitude.hpp
#include "magnitude.hpp"

#include "duration.hpp"
#include "lexdump.hpp"
#include "size.hpp"

namespace geas {

std::optional<std::uint64_t> literalMagnitude(std::string_view text, const Token& token) {
    const auto start = static_cast<std::size_t>(token.lo);
    const std::string_view spelling = text.substr(start, static_cast<std::size_t>(token.hi) - start);
    switch (token.kind) {
        case TokenKind::Int: return intMagnitude(text, token.lo, token.hi);
        case TokenKind::Size: {
            const auto bytes = parseSizeMagnitude(spelling);
            return bytes ? std::optional{*bytes} : std::nullopt;
        }
        case TokenKind::Duration: {
            const auto milliseconds = parseDurationMagnitude(spelling);
            return milliseconds ? std::optional{*milliseconds} : std::nullopt;
        }
        default: return std::nullopt;
    }
}

bool isLeastMagnitude(std::string_view text, const Token& token) {
    return literalMagnitude(text, token) == leastIntMagnitude;
}

bool isLeastLiteral(const ExprKind& kind) {
    if (const auto* integer = std::get_if<IntLiteral>(&kind)) {
        return integer->value < 0;
    }
    if (const auto* duration = std::get_if<DurationLiteral>(&kind)) {
        return duration->milliseconds < 0;
    }
    return false;
}

}  // namespace geas
