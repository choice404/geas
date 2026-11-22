// Parser state and the low level operations every grammar file builds on:
// token navigation, diagnostics, the newline discipline, the grouping depth
// that silences newlines inside parentheses and brackets, and the nesting
// ceiling that keeps a hostile input from overflowing the stack
#include "parser.hpp"

#include <utility>

namespace geas {

namespace {

// Recursive entry points past this depth report once and halt the parse
constexpr int nestingCeiling = 500;

}  // namespace

ParseResult parse(std::string_view text, const std::vector<Token>& tokens) {
    return Parser{text, tokens}.run();
}

ParseResult parseInto(std::string_view text, const std::vector<Token>& tokens, Ast seed) {
    return Parser{text, tokens, std::move(seed)}.run();
}

Parser::Parser(std::string_view text, const std::vector<Token>& tokens)
    : text_{text}, tokens_{tokens} {}

Parser::Parser(std::string_view text, const std::vector<Token>& tokens, Ast seed)
    : text_{text}, tokens_{tokens}, ast_{std::move(seed)} {}

ParseResult Parser::run() && {
    parseFile();
    return ParseResult{std::move(ast_), std::move(diagnostics_)};
}

const Token& Parser::current() const {
    return tokens_[pos_];
}

// The token after the current one, or a synthetic Eof at the end of the
// previous token, so a two token lookahead never needs a bounds check
Token Parser::lookahead() const {
    if (pos_ + 1 >= tokens_.size()) {
        const std::int64_t hi = previousHi();
        return Token{TokenKind::Eof, hi, hi, false};
    }
    return tokens_[pos_ + 1];
}

bool Parser::at(TokenKind kind) const {
    return current().kind == kind;
}

bool Parser::nextIs(TokenKind kind) const {
    return lookahead().kind == kind;
}

bool Parser::atEof() const {
    return at(TokenKind::Eof);
}

// End of input as the grammar loops see it: the real end, or the halt after
// the nesting ceiling, which ends every construct at once
bool Parser::atEnd() const {
    return hasHitDepthLimit_ || atEof();
}

// Whether the current token is an identifier spelling the word, the test
// behind every contextual keyword
bool Parser::atIdentText(std::string_view spelling) const {
    return at(TokenKind::Ident) && currentTextIs(spelling);
}

bool Parser::currentTextIs(std::string_view spelling) const {
    return tokenText(current()) == spelling;
}

// The raw newline flag, for spots where a newline matters regardless of
// grouping, such as a pledge body having to open on its signature's line
bool Parser::hasNewlineHere() const {
    return current().hasNewlineBefore;
}

// The newline flag as a terminator: only outside every open parenthesis and
// bracket. A halted parser terminates everything so the loops wind down
bool Parser::isNewlineTerminator() const {
    if (hasHitDepthLimit_) {
        return true;
    }
    if (groupDepth_ > 0) {
        return false;
    }
    return current().hasNewlineBefore;
}

std::int64_t Parser::previousHi() const {
    return pos_ > 0 ? tokens_[pos_ - 1].hi : 0;
}

std::string_view Parser::tokenText(const Token& token) const {
    return text_.substr(static_cast<std::size_t>(token.lo),
                        static_cast<std::size_t>(token.hi - token.lo));
}

void Parser::bump() {
    if (!atEof()) {
        ++pos_;
    }
}

bool Parser::eat(TokenKind kind) {
    if (!at(kind)) {
        return false;
    }
    ++pos_;
    return true;
}

// Consumes a token of the kind or reports against the one found, without
// consuming it, so the caller's recovery sees the same stream
bool Parser::expect(TokenKind kind, std::string_view what) {
    if (eat(kind)) {
        return true;
    }
    error(expectedFound(what));
    return false;
}

// The current token for an "expected x, found y" message
std::string Parser::foundText() const {
    if (atEof()) {
        return "end of input";
    }
    return "'" + std::string{tokenText(current())} + "'";
}

std::string Parser::expectedFound(std::string_view what) const {
    return "expected " + std::string{what} + ", found " + foundText();
}

// Records a fault at the current token. After the ceiling every further
// fault is noise from the abandoned parse, so it is dropped
void Parser::error(std::string message) {
    errorSpan(current().lo, current().hi, std::move(message));
}

void Parser::errorSpan(std::int64_t lo, std::int64_t hi, std::string message) {
    if (hasHitDepthLimit_) {
        return;
    }
    diagnostics_.push_back(Diagnostic{lo, hi, std::move(message)});
}

// An identifier, or an empty name and a fault without consuming anything
std::string Parser::ident() {
    if (!at(TokenKind::Ident)) {
        error(expectedFound("an identifier"));
        return "";
    }
    std::string name{tokenText(current())};
    bump();
    return name;
}

// A member name after a dot: any identifier or keyword, so payment.break()
// parses while break stays a hard keyword for loop control
std::string Parser::memberName() {
    if (!at(TokenKind::Ident) && !isKeyword(current().kind)) {
        error(expectedFound("a member name after '.'"));
        return "";
    }
    std::string name{tokenText(current())};
    bump();
    return name;
}

void Parser::enterGroup() {
    ++groupDepth_;
}

void Parser::leaveGroup() {
    if (groupDepth_ > 0) {
        --groupDepth_;
    }
}

// Braces reset the grouping depth: a block inside an argument list holds
// statements again, and their newlines terminate
int Parser::saveBraceDepth() {
    return std::exchange(groupDepth_, 0);
}

void Parser::restoreBraceDepth(int depth) {
    groupDepth_ = depth;
}

// A false return means the ceiling was hit and reported, and the caller must
// build a placeholder without recursing. Every call pairs with leaveNesting
bool Parser::enterNesting() {
    ++nestingDepth_;
    if (nestingDepth_ <= nestingCeiling) {
        return true;
    }
    if (!hasHitDepthLimit_) {
        diagnostics_.push_back(Diagnostic{current().lo, current().hi,
                                          "nesting is too deep; the parser stopped here"});
        hasHitDepthLimit_ = true;
    }
    return false;
}

void Parser::leaveNesting(int levels) {
    nestingDepth_ -= levels;
}

bool Parser::isHalted() const {
    return hasHitDepthLimit_;
}

// The stall guard for recovery loops: when a whole element consumed nothing,
// report the stuck token once and step over it
void Parser::guardProgress(std::size_t before, std::string_view context) {
    if (pos_ != before) {
        return;
    }
    error(std::string{context} + ": " + foundText());
    bump();
}

// After an element the grammar ends with NL, the next token must sit on a new
// line unless it closes the block or ends the input. Consumes nothing
void Parser::expectSeparator(std::string_view message) {
    if (atEof() || at(TokenKind::RBrace) || isNewlineTerminator()) {
        return;
    }
    error(std::string{message});
}

// The closing brace of a body. At the end of input the message names the
// construct left open, elsewhere the generic form points at the stray token
void Parser::expectClose(std::string_view eofMessage) {
    if (eat(TokenKind::RBrace)) {
        return;
    }
    if (atEof()) {
        error(std::string{eofMessage});
        return;
    }
    error(expectedFound("'}'"));
}

// Recovery for a line the grammar cannot read: steps past the current token
// to the first one that opens a new line, closes the body, or ends the input
void Parser::skipRestOfLine() {
    if (atEnd() || at(TokenKind::RBrace)) {
        return;
    }
    bump();
    while (!atEnd() && !at(TokenKind::RBrace) && !hasNewlineHere()) {
        bump();
    }
}

TypeId Parser::placeholderType() {
    return ast_.addType(NamedType{"", {}});
}

ExprId Parser::placeholderExpr() {
    return ast_.addExpr(Expr{current().lo, current().hi, Identifier{""}});
}

BlockId Parser::emptyBlock() {
    return ast_.addBlock(Block{});
}

}  // namespace geas
