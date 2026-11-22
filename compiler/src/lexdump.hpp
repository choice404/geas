// The lex dump, one line per token, `{lo:>4}..{hi:<4} nl_before={bool} {kind}`.
// A literal's payload is rebuilt from its span, and every scalar outside
// printable ASCII prints as a \u{hex} escape, so the dump is a byte stable
// golden format that never shifts under a locale
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "token.hpp"

namespace geas {

/**
 * The decoded value of an integer span, prefix and underscores handled. An
 * overflowing literal decodes as 0, the value that sits beside its fault.
 * @params text - the source, lo - start of the literal, hi - past its end
 * @returns {std::int64_t} - the value
 */
std::int64_t intValue(std::string_view text, std::int64_t lo, std::int64_t hi);

/**
 * The magnitude of an integer span, prefix and underscores handled, up to
 * the magnitude of the least Int, see magnitude.hpp.
 * @params text - the source, lo - start of the literal, hi - past its end
 * @returns {std::optional<std::uint64_t>} - the magnitude, nullopt past the least Int's
 */
std::optional<std::uint64_t> intMagnitude(std::string_view text, std::int64_t lo, std::int64_t hi);

/**
 * One scalar in canonical form: printable ASCII as itself, the quote and the
 * backslash escaped, everything else as a \u{hex} escape.
 * @params scalar - the scalar to render, quote - the quote of the enclosing literal
 * @returns {std::string} - the canonical spelling
 */
std::string canonicalScalar(std::int64_t scalar, char quote);

/**
 * The canonical body of a string literal, quotes stripped, decoded the way
 * the lexer walked it so an invalid escape adds nothing. The parse dump and
 * the lex dump share it, so the two never disagree about a string.
 * @params text - the source, token - the string token
 * @returns {std::string} - the re escaped body without its quotes
 */
std::string stringLiteralBody(std::string_view text, const Token& token);

/**
 * The leading scalar of a char literal body. An empty body, the end of
 * input, and a failed escape all decode as the null scalar.
 * @params text - the source, bodyStart - the offset just past the opening quote
 * @returns {std::int64_t} - the scalar
 */
std::int64_t charLiteralScalar(std::string_view text, std::int64_t bodyStart);

/**
 * The full dump line for one token, no trailing newline.
 * @params text - the source, token - the token to render
 * @returns {std::string} - the rendered line
 */
std::string renderToken(std::string_view text, const Token& token);

}  // namespace geas
