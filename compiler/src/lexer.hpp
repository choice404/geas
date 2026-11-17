// The geas lexer. It walks the source by byte offset, pushing tokens and
// recording faults, and always runs to the end so one bad literal does not
// hide the faults after it. A line break never becomes a token of its own,
// it sets a flag the next token carries and the parser decides what it means
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "diagnostics.hpp"
#include "token.hpp"

namespace geas {

// What a finished lex hands back, the token stream ending in Eof and every
// fault in the order it was found
struct LexResult {
    std::vector<Token> tokens;
    std::vector<Diagnostic> diagnostics;
};

/**
 * Lexes a whole source buffer.
 * @params text - the source, already checked to be valid UTF-8
 * @returns {LexResult} - the tokens and the faults, the run failed when faults is non empty
 */
LexResult lex(std::string_view text);

// The scanner state behind lex. The number scanners live in lexnumber.cpp and
// everything else in lexer.cpp
class Lexer {
public:
    explicit Lexer(std::string_view text);
    LexResult run() &&;

private:
    int peek(std::int64_t ahead = 0) const;
    void advance(std::int64_t count = 1);
    void push(TokenKind kind, std::int64_t start);
    void report(std::int64_t start, std::string_view message);
    void bumpScalar();

    void skipTrivia();
    void skipLineComment();
    void skipBlockComment();
    void scanToken();
    void scanWord(std::int64_t start);
    void scanString(std::int64_t start);
    void scanChar(std::int64_t start);
    void closeCharQuote(std::int64_t start);
    void scanEscape();
    void scanSymbol(std::int64_t start, int lead);

    void scanNumber(std::int64_t start);
    void scanRadixInt(std::int64_t start, int base, std::string_view emptyMessage);
    std::int64_t scanDigitRun(int base);
    void scanExponent();
    void scanNumberJunk(std::int64_t start);
    void scanDuration(std::int64_t start);
    void scanSize(std::int64_t start);
    void checkIntOverflow(std::int64_t start, std::int64_t lo, std::int64_t hi, int base);

    std::string_view text_;
    std::int64_t pos_ = 0;
    bool hasPendingNewline_ = false;
    std::vector<Token> tokens_;
    std::vector<Diagnostic> diagnostics_;
};

/**
 * Whether c can start an identifier, an ASCII letter or an underscore.
 * @params c - the byte to test
 * @returns {bool} - true for an identifier start
 */
bool isIdentStart(int c);

/**
 * Whether c can continue an identifier, an identifier start or a digit.
 * @params c - the byte to test
 * @returns {bool} - true for an identifier continuation
 */
bool isIdentContinue(int c);

/**
 * Whether c is an ASCII decimal digit.
 * @params c - the byte to test
 * @returns {bool} - true for 0 to 9
 */
bool isDigit(int c);

/**
 * Whether the digits of text[lo, hi) in base overflow a signed 64 bit
 * integer, underscores skipped.
 * @params text - the source, lo - first digit, hi - past the last, base - 2, 10, or 16
 * @returns {bool} - true when the value cannot be represented
 */
bool doesIntOverflow(std::string_view text, std::int64_t lo, std::int64_t hi, int base);

}  // namespace geas
