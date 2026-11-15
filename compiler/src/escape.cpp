// The escape scanner, see escape.hpp
#include "escape.hpp"

#include "utf8.hpp"

namespace geas {

namespace {

constexpr std::int64_t maxUnicodeDigits = 6;
constexpr std::int64_t hexBase = 16;
constexpr std::int64_t surrogateLow = 0xD800;
constexpr std::int64_t surrogateHigh = 0xDFFF;
constexpr std::int64_t maxScalar = 0x10FFFF;

EscapeResult refuse(std::int64_t next, EscapeError error) {
    return EscapeResult{next, -1, error};
}

// Checks the digit count and the value range of a finished \u{...} body
EscapeResult finishUnicode(std::int64_t next, std::int64_t value, std::int64_t digitCount) {
    if (digitCount == 0 || digitCount > maxUnicodeDigits) {
        return refuse(next, EscapeError::UnicodeDigitCount);
    }
    if (value >= surrogateLow && value <= surrogateHigh) {
        return refuse(next, EscapeError::UnicodeSurrogate);
    }
    if (value > maxScalar) {
        return refuse(next, EscapeError::UnicodeAboveMax);
    }
    return EscapeResult{next, value, EscapeError::Ok};
}

// Scans the body of a \u{...} escape from pos, the offset just past the u.
// Digits past the sixth are consumed but not accumulated, so a long run still
// ends at its brace and faults once instead of overflowing
EscapeResult scanUnicodeEscape(std::string_view text, std::int64_t pos) {
    if (byteAt(text, pos) != '{') {
        return refuse(pos, EscapeError::UnicodeNoBrace);
    }
    std::int64_t cursor = pos + 1;
    std::int64_t value = 0;
    std::int64_t digitCount = 0;
    while (byteAt(text, cursor) != '}') {
        const int byte = byteAt(text, cursor);
        if (!isHexDigit(byte)) {
            return refuse(cursor, EscapeError::UnicodeUnterminated);
        }
        if (digitCount < maxUnicodeDigits) {
            value = value * hexBase + hexValue(byte);
        }
        ++digitCount;
        ++cursor;
    }
    return finishUnicode(cursor + 1, value, digitCount);
}

// The scalar of a one letter escape, or -1 when c is not one
std::int64_t simpleEscapeScalar(int c) {
    switch (c) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case '0': return 0;
        case '\\': return '\\';
        case '"': return '"';
        case '\'': return '\'';
        default: return -1;
    }
}

}  // namespace

EscapeResult scanEscape(std::string_view text, std::int64_t pos) {
    const int c = byteAt(text, pos);
    const std::int64_t simpleScalar = simpleEscapeScalar(c);
    if (simpleScalar >= 0) {
        return EscapeResult{pos + 1, simpleScalar, EscapeError::Ok};
    }
    if (c == 'u') {
        return scanUnicodeEscape(text, pos + 1);
    }
    if (c == endOfInput) {
        return refuse(pos, EscapeError::Unknown);
    }
    return refuse(pos + 1, EscapeError::Unknown);
}

std::string_view escapeErrorMessage(EscapeError error) {
    switch (error) {
        case EscapeError::Ok: return "invalid escape";
        case EscapeError::Unknown: return "unknown escape";
        case EscapeError::UnicodeNoBrace: return "expected '{' after \\u";
        case EscapeError::UnicodeUnterminated: return "unterminated \\u escape; expected '}'";
        case EscapeError::UnicodeDigitCount: return "\\u escape needs 1 to 6 hex digits";
        case EscapeError::UnicodeSurrogate:
            return "\\u escape is a surrogate code point, not a scalar value";
        case EscapeError::UnicodeAboveMax:
            return "\\u escape is above 0x10FFFF, the Unicode maximum";
    }
    return "invalid escape";
}

}  // namespace geas
