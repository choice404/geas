// The number scanners: decimal, hex, and binary integers with underscore
// separators, floats with a fraction and an optional exponent, and durations
#include <limits>

#include "duration.hpp"
#include "lexer.hpp"
#include "utf8.hpp"

namespace geas {

namespace {

constexpr int binaryBase = 2;
constexpr int decimalBase = 10;
constexpr int hexBase = 16;
constexpr std::int64_t maxInt = std::numeric_limits<std::int64_t>::max();

// Whether c is a digit of base, the only bases being 2, 10, and 16
bool isDigitOfBase(int c, int base) {
    if (base == hexBase) {
        return isHexDigit(c);
    }
    if (base == binaryBase) {
        return c == '0' || c == '1';
    }
    return isDigit(c);
}

bool isExponentMarker(int c) {
    return c == 'e' || c == 'E';
}

}  // namespace

bool doesIntOverflow(std::string_view text, std::int64_t lo, std::int64_t hi, int base) {
    std::int64_t value = 0;
    for (std::int64_t i = lo; i < hi; ++i) {
        const int byte = byteAt(text, i);
        if (byte == '_') {
            continue;
        }
        const int digit = hexValue(byte);
        if (value > (maxInt - digit) / base) {
            return true;
        }
        value = value * base + digit;
    }
    return false;
}

// Scans digits of base with underscores between, returning how many real
// digits it saw so a caller can refuse an empty run. A digit of a larger base
// ends the run and the junk check after the literal reports it
std::int64_t Lexer::scanDigitRun(int base) {
    std::int64_t digitCount = 0;
    while (true) {
        const int c = peek();
        if (c == '_') {
            advance();
        } else if (isDigitOfBase(c, base)) {
            advance();
            ++digitCount;
        } else {
            return digitCount;
        }
    }
}

void Lexer::checkIntOverflow(std::int64_t start, std::int64_t lo, std::int64_t hi, int base) {
    if (doesIntOverflow(text_, lo, hi, base)) {
        report(start, "integer literal overflows 64 bits");
    }
}

// Consumes identifier characters trailing a number as one fault, so 0x1G or
// 1.5ex is a single bad literal rather than a number and a stray identifier
void Lexer::scanNumberJunk(std::int64_t start) {
    bool hasJunk = false;
    while (isIdentContinue(peek())) {
        hasJunk = true;
        advance();
    }
    if (hasJunk) {
        report(start, "invalid character in numeric literal");
    }
}

// A hex or binary integer, the prefix already consumed. A prefixed literal is
// never a duration, so 0x1d is the integer 29
void Lexer::scanRadixInt(std::int64_t start, int base, std::string_view emptyMessage) {
    const std::int64_t digitsStart = pos_;
    const std::int64_t digitCount = scanDigitRun(base);
    const std::int64_t digitsEnd = pos_;
    scanNumberJunk(start);
    if (digitCount == 0) {
        report(start, emptyMessage);
    } else {
        checkIntOverflow(start, digitsStart, digitsEnd, base);
    }
    push(TokenKind::Int, start);
}

// Consumes an exponent only when one is really there, the marker, an
// optional sign, and at least one digit. A bare marker stays for the junk check
void Lexer::scanExponent() {
    const int afterMarker = peek(1);
    if (isDigit(afterMarker)) {
        advance(2);
        scanDigitRun(decimalBase);
        return;
    }
    if ((afterMarker == '+' || afterMarker == '-') && isDigit(peek(2))) {
        advance(2);
        scanDigitRun(decimalBase);
    }
}

// A duration, the digits of its first part already under the cursor. The
// whole identifier run is the literal, so 5mx and 3days fault as one bad
// token instead of splitting into a number and a name
void Lexer::scanDuration(std::int64_t start) {
    while (isIdentContinue(peek())) {
        advance();
    }
    const std::string_view literal = text_.substr(static_cast<std::size_t>(start),
                                                  static_cast<std::size_t>(pos_ - start));
    const auto milliseconds = parseDuration(literal);
    if (!milliseconds) {
        report(start, durationErrorMessage(milliseconds.error()));
    }
    push(TokenKind::Duration, start);
}

// A numeric literal. A 0x or 0b prefix routes to the radix scan, otherwise
// decimal digits, then a fraction that makes it a float, then an exponent on
// the float form only. A dot with no digit after it is left alone so member
// access on a literal still lexes. A unit letter right after the digits makes
// it a duration
void Lexer::scanNumber(std::int64_t start) {
    if (peek() == '0' && peek(1) == 'x') {
        advance(2);
        scanRadixInt(start, hexBase, "hex literal needs at least one digit");
        return;
    }
    if (peek() == '0' && peek(1) == 'b') {
        advance(2);
        scanRadixInt(start, binaryBase, "binary literal needs at least one digit");
        return;
    }
    scanDigitRun(decimalBase);
    const std::int64_t intEnd = pos_;
    if (peek() == '.' && isDigit(peek(1))) {
        advance();
        scanDigitRun(decimalBase);
        if (isExponentMarker(peek())) {
            scanExponent();
        }
        scanNumberJunk(start);
        push(TokenKind::Float, start);
        return;
    }
    if (isDurationUnitLead(peek())) {
        scanDuration(start);
        return;
    }
    scanNumberJunk(start);
    checkIntOverflow(start, start, intEnd, decimalBase);
    push(TokenKind::Int, start);
}

}  // namespace geas
