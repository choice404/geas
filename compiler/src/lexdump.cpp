// The lex dump renderer, see lexdump.hpp
#include "lexdump.hpp"

#include <limits>

#include "duration.hpp"
#include "escape.hpp"
#include "size.hpp"
#include "utf8.hpp"

namespace geas {

namespace {

constexpr std::size_t offsetFieldWidth = 4;
constexpr std::int64_t firstPrintable = 0x20;
constexpr std::int64_t lastPrintable = 0x7E;
constexpr int asciiLimit = 0x80;
constexpr int decimalBase = 10;
constexpr int binaryBase = 2;
constexpr int hexBase = 16;
constexpr std::int64_t maxInt = std::numeric_limits<std::int64_t>::max();

std::string_view spanText(std::string_view text, const Token& token) {
    return text.substr(static_cast<std::size_t>(token.lo),
                       static_cast<std::size_t>(token.hi - token.lo));
}

std::string padLeft(std::int64_t value, std::size_t width) {
    const std::string digits = std::to_string(value);
    return digits.size() >= width ? digits : std::string(width - digits.size(), ' ') + digits;
}

std::string padRight(std::int64_t value, std::size_t width) {
    const std::string digits = std::to_string(value);
    return digits.size() >= width ? digits : digits + std::string(width - digits.size(), ' ');
}

std::string lowerHex(std::int64_t value) {
    constexpr std::string_view hexDigits = "0123456789abcdef";
    if (value == 0) {
        return "0";
    }
    std::string reversed;
    for (std::int64_t rest = value; rest > 0; rest /= hexBase) {
        reversed.push_back(hexDigits[static_cast<std::size_t>(rest % hexBase)]);
    }
    return std::string(reversed.rbegin(), reversed.rend());
}

std::string renderChar(std::string_view text, const Token& token) {
    return "'" + canonicalScalar(charLiteralScalar(text, token.lo + 1), '\'') + "'";
}

// The literal payload part of the kind, or the fixed name for everything else
std::string renderKind(std::string_view text, const Token& token) {
    switch (token.kind) {
        case TokenKind::Ident: return "Ident(\"" + std::string{spanText(text, token)} + "\")";
        case TokenKind::Int: return "Int(" + std::to_string(intValue(text, token.lo, token.hi)) + ")";
        case TokenKind::Size:
            return "Size(" + std::to_string(parseSize(spanText(text, token)).value_or(0)) + ")";
        case TokenKind::Float: return "Float(\"" + std::string{spanText(text, token)} + "\")";
        case TokenKind::Duration:
            return "Duration(" + std::to_string(parseDuration(spanText(text, token)).value_or(0)) + ")";
        case TokenKind::String: return "Str(\"" + stringLiteralBody(text, token) + "\")";
        case TokenKind::Char: return "Char(" + renderChar(text, token) + ")";
        case TokenKind::KwTrue:
        case TokenKind::KwFalse: return "Bool(" + std::string{spanText(text, token)} + ")";
        default: break;
    }
    if (isKeyword(token.kind)) {
        return "Kw(" + std::string{kindDumpName(token.kind)} + ")";
    }
    return std::string{kindDumpName(token.kind)};
}

// The base of an integer literal and where its digits begin
struct IntPrefix {
    int base;
    std::int64_t digitsStart;
};

IntPrefix intPrefix(std::string_view text, std::int64_t lo) {
    if (byteAt(text, lo) != '0') {
        return IntPrefix{decimalBase, lo};
    }
    if (byteAt(text, lo + 1) == 'x') {
        return IntPrefix{hexBase, lo + 2};
    }
    if (byteAt(text, lo + 1) == 'b') {
        return IntPrefix{binaryBase, lo + 2};
    }
    return IntPrefix{decimalBase, lo};
}

}  // namespace

std::string canonicalScalar(std::int64_t scalar, char quote) {
    if (scalar == '\\') {
        return "\\\\";
    }
    if (scalar == quote) {
        return std::string{'\\', quote};
    }
    if (scalar >= firstPrintable && scalar <= lastPrintable) {
        return std::string(1, static_cast<char>(scalar));
    }
    return "\\u{" + lowerHex(scalar) + "}";
}

// An unterminated literal just runs out of span
std::string stringLiteralBody(std::string_view text, const Token& token) {
    std::string out;
    std::int64_t cursor = token.lo + 1;
    while (cursor < token.hi && byteAt(text, cursor) != '"') {
        const int byte = byteAt(text, cursor);
        if (byte == '\\') {
            const EscapeResult result = scanEscape(text, cursor + 1);
            if (result.scalar >= 0) {
                out.append(canonicalScalar(result.scalar, '"'));
            }
            cursor = result.next;
            continue;
        }
        const DecodedScalar decoded = decodeUTF8(text, cursor);
        out.append(canonicalScalar(decoded.scalar, '"'));
        cursor += decoded.length;
    }
    return out;
}

std::int64_t charLiteralScalar(std::string_view text, std::int64_t bodyStart) {
    const int byte = byteAt(text, bodyStart);
    if (byte == endOfInput || byte == '\'') {
        return 0;
    }
    if (byte == '\\') {
        const std::int64_t scalar = scanEscape(text, bodyStart + 1).scalar;
        return scalar < 0 ? 0 : scalar;
    }
    if (byte < asciiLimit) {
        return byte;
    }
    return decodeUTF8(text, bodyStart).scalar;
}

std::int64_t intValue(std::string_view text, std::int64_t lo, std::int64_t hi) {
    const IntPrefix prefix = intPrefix(text, lo);
    std::int64_t value = 0;
    for (std::int64_t cursor = prefix.digitsStart; cursor < hi; ++cursor) {
        const int byte = byteAt(text, cursor);
        if (byte == '_') {
            continue;
        }
        const int digit = hexValue(byte);
        if (digit < 0 || digit >= prefix.base) {
            break;
        }
        if (value > (maxInt - digit) / prefix.base) {
            return 0;
        }
        value = value * prefix.base + digit;
    }
    return value;
}

std::string renderToken(std::string_view text, const Token& token) {
    std::string line = padLeft(token.lo, offsetFieldWidth);
    line.append("..");
    line.append(padRight(token.hi, offsetFieldWidth));
    line.append(token.hasNewlineBefore ? " nl_before=true " : " nl_before=false ");
    line.append(renderKind(text, token));
    return line;
}

}  // namespace geas
