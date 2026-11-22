// The magnitude of a numeric literal. An Int, a Size, and a Duration are
// signed 64 bit counts, and a literal is always the magnitude a unary minus
// may negate, so the one literal that spells more than the greatest Int is
// the magnitude of the least Int, and only right after a unary minus. The
// lexer lets that one through after a minus token, and the parser folds a
// unary minus on it into the least value itself; anywhere else it overflows
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "astnode.hpp"
#include "token.hpp"

namespace geas {

// The magnitude of the least Int, one past the greatest
inline constexpr std::uint64_t leastIntMagnitude = std::uint64_t{1} << 63;

/**
 * The magnitude an Int, Size, or Duration token spells.
 * @params text - the source, token - the literal token
 * @returns {std::optional<std::uint64_t>} - the magnitude, nullopt for any other token or a
 *     literal that overflows past the magnitude of the least Int
 */
std::optional<std::uint64_t> literalMagnitude(std::string_view text, const Token& token);

/**
 * Whether a token is an Int, Size, or Duration literal spelling exactly the
 * magnitude of the least Int.
 * @params text - the source, token - the token to test
 * @returns {bool} - true for the one literal only a unary minus may take
 */
bool isLeastMagnitude(std::string_view text, const Token& token);

/**
 * Whether a literal node holds the least value of an Int, a Size, or a
 * Duration, the one value the parser folds out of a unary minus.
 * @params kind - the expression node
 * @returns {bool} - true for an Int or Duration literal below zero
 */
bool isLeastLiteral(const ExprKind& kind);

}  // namespace geas
