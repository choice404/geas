// Duration literals, 30s, 250ms, 1h30m, 7d. A duration is one token made of
// parts, each a decimal run and a unit, with the units running from largest
// to smallest and each used at most once. The value is a count of
// milliseconds, the smallest unit, so every literal is exact
#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

namespace geas {

// Why a duration literal was refused
enum class DurationError {
    UnknownUnit,
    MissingUnit,
    OutOfOrder,
    RepeatedUnit,
    StrayCharacter,
    Overflow,
};

/**
 * Whether c can open a duration unit, the letter the lexer looks for right
 * after the digits of a decimal literal.
 * @params c - the byte after the digits
 * @returns {bool} - true for d, h, m, or s
 */
bool isDurationUnitLead(int c);

/**
 * Decodes a whole duration literal, every byte of it, into milliseconds.
 * @params literal - the literal text, starting with a digit
 * @returns {std::expected<std::int64_t, DurationError>} - the milliseconds, or why it was refused
 */
std::expected<std::int64_t, DurationError> parseDuration(std::string_view literal);

/**
 * Decodes a whole duration literal into its count of milliseconds as a
 * magnitude, which reaches one past the greatest Int: the magnitude of the
 * least Int, a value only a literal right after a unary minus may spell.
 * @params literal - the literal text, starting with a digit
 * @returns {std::expected<std::uint64_t, DurationError>} - the milliseconds, or why it was refused
 */
std::expected<std::uint64_t, DurationError> parseDurationMagnitude(std::string_view literal);

/**
 * The diagnostic message for a refused duration.
 * @params error - the refusal to name
 * @returns {std::string_view} - a static message
 */
std::string_view durationErrorMessage(DurationError error);

}  // namespace geas
