// Decoding one backslash escape inside a string or char literal. The lexer
// walks past an escape with this to find where a literal ends and records any
// fault, and the dump decodes the same escape back to a scalar, so the two
// always agree on spans and faults. The set is exactly the grammar's, \n \t
// \r \0 \\ \" \' and \u{hex}
#pragma once

#include <cstdint>
#include <string_view>

namespace geas {

// Why an escape was refused, Ok when it decoded cleanly
enum class EscapeError {
    Ok,
    Unknown,
    UnicodeNoBrace,
    UnicodeUnterminated,
    UnicodeDigitCount,
    UnicodeSurrogate,
    UnicodeAboveMax,
};

// The outcome of one escape: the offset just past it, the decoded scalar or
// -1 when it produced no value, and the error
struct EscapeResult {
    std::int64_t next;
    std::int64_t scalar;
    EscapeError error;
};

/**
 * Scans one escape whose backslash sits just before pos, so pos is the escape
 * character itself.
 * @params text - the source, pos - the offset of the character after the backslash
 * @returns {EscapeResult} - where to resume, the scalar, and the error
 */
EscapeResult scanEscape(std::string_view text, std::int64_t pos);

/**
 * The diagnostic message for an escape error.
 * @params error - the refusal to name
 * @returns {std::string_view} - a static message
 */
std::string_view escapeErrorMessage(EscapeError error);

}  // namespace geas
