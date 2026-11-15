// The scan loop, trivia, words, strings, chars, and symbols. Numbers and
// durations are in lexnumber.cpp
#include "lexer.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <utility>

#include "escape.hpp"
#include "utf8.hpp"

namespace geas {

namespace {

constexpr int asciiLimit = 0x80;

// A two byte operator, its bytes and its kind
struct TwoByteOperator {
    char first;
    char second;
    TokenKind kind;
};

constexpr std::array twoByteOperators{
    TwoByteOperator{'-', '>', TokenKind::Arrow},
    TwoByteOperator{'=', '=', TokenKind::Eq},
    TwoByteOperator{'!', '=', TokenKind::Ne},
    TwoByteOperator{'<', '=', TokenKind::Le},
    TwoByteOperator{'>', '=', TokenKind::Ge},
    TwoByteOperator{'&', '&', TokenKind::AndAnd},
    TwoByteOperator{'|', '|', TokenKind::OrOr},
};

// A one byte token and its kind. A lone & or | is not in the grammar, so it
// is missing here and reports as an unexpected character
struct OneByteToken {
    char byte;
    TokenKind kind;
};

constexpr std::array oneByteTokens{
    OneByteToken{'=', TokenKind::Assign},   OneByteToken{'+', TokenKind::Plus},
    OneByteToken{'-', TokenKind::Minus},    OneByteToken{'*', TokenKind::Star},
    OneByteToken{'/', TokenKind::Slash},    OneByteToken{'%', TokenKind::Percent},
    OneByteToken{'<', TokenKind::Lt},       OneByteToken{'>', TokenKind::Gt},
    OneByteToken{'!', TokenKind::Bang},     OneByteToken{'?', TokenKind::Question},
    OneByteToken{':', TokenKind::Colon},    OneByteToken{',', TokenKind::Comma},
    OneByteToken{'.', TokenKind::Dot},      OneByteToken{'(', TokenKind::LParen},
    OneByteToken{')', TokenKind::RParen},   OneByteToken{'{', TokenKind::LBrace},
    OneByteToken{'}', TokenKind::RBrace},   OneByteToken{'[', TokenKind::LBracket},
    OneByteToken{']', TokenKind::RBracket},
};

std::optional<TokenKind> twoByteKind(int first, int second) {
    for (const TwoByteOperator& op : twoByteOperators) {
        if (op.first == first && op.second == second) {
            return op.kind;
        }
    }
    return std::nullopt;
}

std::optional<TokenKind> oneByteKind(int byte) {
    for (const OneByteToken& token : oneByteTokens) {
        if (token.byte == byte) {
            return token.kind;
        }
    }
    return std::nullopt;
}

bool isLetter(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

}  // namespace

bool isDigit(int c) {
    return c >= '0' && c <= '9';
}

bool isIdentStart(int c) {
    return isLetter(c) || c == '_';
}

bool isIdentContinue(int c) {
    return isIdentStart(c) || isDigit(c);
}

LexResult lex(std::string_view text) {
    return Lexer{text}.run();
}

Lexer::Lexer(std::string_view text) : text_(text) {}

// Every pass through scanToken advances the offset, so the loop cannot stall
// on bad input, the worst case is a fault per byte
LexResult Lexer::run() && {
    while (true) {
        skipTrivia();
        if (peek() == endOfInput) {
            push(TokenKind::Eof, pos_);
            break;
        }
        scanToken();
    }
    return LexResult{std::move(tokens_), std::move(diagnostics_)};
}

int Lexer::peek(std::int64_t ahead) const {
    return byteAt(text_, pos_ + ahead);
}

void Lexer::advance(std::int64_t count) {
    pos_ += count;
}

// Pushes a token over [start, pos) carrying the pending newline flag, which
// then clears so it attaches to this token only
void Lexer::push(TokenKind kind, std::int64_t start) {
    tokens_.push_back(Token{kind, start, pos_, hasPendingNewline_});
    hasPendingNewline_ = false;
}

void Lexer::report(std::int64_t start, std::string_view message) {
    diagnostics_.push_back(Diagnostic{start, pos_, std::string{message}});
}

// Steps past a whole UTF-8 scalar, clamped to the end of the buffer
void Lexer::bumpScalar() {
    const std::int64_t next = pos_ + utf8SequenceLength(peek());
    pos_ = std::min(next, static_cast<std::int64_t>(text_.size()));
}

// Skips whitespace and comments. A line comment stops before its newline so
// the next turn sees it and sets the flag, and a newline inside a block
// comment sets it too, so a comment across lines ends a statement just like
// the line break it swallowed
void Lexer::skipTrivia() {
    while (true) {
        const int c = peek();
        if (c == '\n') {
            hasPendingNewline_ = true;
            advance();
        } else if (c == ' ' || c == '\t' || c == '\r') {
            advance();
        } else if (c == '/' && peek(1) == '/') {
            skipLineComment();
        } else if (c == '/' && peek(1) == '*') {
            skipBlockComment();
        } else {
            return;
        }
    }
}

void Lexer::skipLineComment() {
    while (peek() != '\n' && peek() != endOfInput) {
        advance();
    }
}

// Block comments do not nest. An unterminated one is a fault at the opener
// and the scan stops at the end rather than hanging
void Lexer::skipBlockComment() {
    const std::int64_t start = pos_;
    advance(2);
    while (true) {
        const int c = peek();
        if (c == endOfInput) {
            report(start, "unterminated block comment");
            return;
        }
        if (c == '*' && peek(1) == '/') {
            advance(2);
            return;
        }
        if (c == '\n') {
            hasPendingNewline_ = true;
        }
        advance();
    }
}

void Lexer::scanToken() {
    const std::int64_t start = pos_;
    const int c = peek();
    if (isIdentStart(c)) {
        scanWord(start);
    } else if (isDigit(c)) {
        scanNumber(start);
    } else if (c == '"') {
        scanString(start);
    } else if (c == '\'') {
        scanChar(start);
    } else {
        scanSymbol(start, c);
    }
}

// An identifier, a keyword, or the lone underscore wildcard. A keyword after
// a dot staying a member name is the parser's call, not the lexer's
void Lexer::scanWord(std::int64_t start) {
    while (isIdentContinue(peek())) {
        advance();
    }
    const std::string_view word = text_.substr(static_cast<std::size_t>(start),
                                               static_cast<std::size_t>(pos_ - start));
    if (word == "_") {
        push(TokenKind::Underscore, start);
        return;
    }
    push(keywordKind(word).value_or(TokenKind::Ident), start);
}

// Steps one escape, the backslash under the cursor, recording its fault
void Lexer::scanEscape() {
    const std::int64_t backslash = pos_;
    advance();
    const EscapeResult result = geas::scanEscape(text_, pos_);
    pos_ = result.next;
    if (result.error != EscapeError::Ok) {
        report(backslash, escapeErrorMessage(result.error));
    }
}

// A string does not span lines, a raw newline ends it as unterminated and is
// left for the trivia pass, so the fault stays on its line and the next line
// lexes normally
void Lexer::scanString(std::int64_t start) {
    advance();
    while (true) {
        const int c = peek();
        if (c == endOfInput || c == '\n') {
            report(start, "unterminated string literal");
            break;
        }
        if (c == '"') {
            advance();
            break;
        }
        if (c == '\\') {
            scanEscape();
        } else {
            advance();
        }
    }
    push(TokenKind::String, start);
}

// A char literal holds exactly one scalar value. A malformed body recovers to
// the next quote or line end so the span stays sane
void Lexer::scanChar(std::int64_t start) {
    advance();
    const int c = peek();
    if (c == endOfInput) {
        report(start, "unterminated char literal");
    } else if (c == '\'') {
        report(start, "empty char literal");
    } else if (c == '\\') {
        scanEscape();
    } else if (c < asciiLimit) {
        advance();
    } else {
        bumpScalar();
    }
    closeCharQuote(start);
    push(TokenKind::Char, start);
}

// Consumes the closing quote, or scans to the next quote or newline, takes a
// quote if it found one, and records the fault over the recovered span
void Lexer::closeCharQuote(std::int64_t start) {
    if (peek() == '\'') {
        advance();
        return;
    }
    while (peek() != '\'' && peek() != '\n' && peek() != endOfInput) {
        advance();
    }
    if (peek() == '\'') {
        advance();
    }
    report(start, "char literal must contain exactly one character");
}

// One operator or punctuation token, longest form first. A byte outside ASCII
// here is outside any literal, the whole scalar is stepped so it faults once
void Lexer::scanSymbol(std::int64_t start, int lead) {
    if (lead >= asciiLimit) {
        bumpScalar();
        report(start, "unexpected character");
        return;
    }
    if (const auto kind = twoByteKind(lead, peek(1))) {
        advance(2);
        push(*kind, start);
        return;
    }
    const auto kind = oneByteKind(lead);
    advance();
    if (!kind) {
        report(start, "unexpected character");
        return;
    }
    push(*kind, start);
}

}  // namespace geas
