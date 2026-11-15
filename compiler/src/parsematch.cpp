// Match, select, and patterns. Both arm lists are newline separated inside
// braces, and an arm body is an expression or a block, told apart by the
// brace after the arrow. Select arms open with await or the contextual word
// timeout, an await arm binding its payload with the contextual word as
#include "parser.hpp"

#include <utility>

namespace geas {

// match CondExpr "{" { Pattern "->" ArmBody NL } "}"
ExprId Parser::parseMatch() {
    const std::int64_t lo = current().lo;
    bump();
    const ExprId scrutinee = parseExprNoRecord();
    std::vector<MatchArm> arms;
    expect(TokenKind::LBrace, "'{' to open the match arms");
    const int savedDepth = saveBraceDepth();
    while (!at(TokenKind::RBrace) && !atEnd()) {
        const std::size_t before = pos_;
        const PatternId pattern = parsePattern();
        if (!eat(TokenKind::Arrow)) {
            error("a match arm needs '->' between its pattern and its body");
        }
        arms.push_back(MatchArm{pattern, parseArmBody()});
        expectSeparator("expected a newline after the match arm");
        guardProgress(before, "unexpected token in the match arms");
    }
    restoreBraceDepth(savedDepth);
    expectClose("the match body is not closed; expected '}' before end of input");
    return spanExpr(lo, previousHi(), MatchExpr{scrutinee, std::move(arms)});
}

ArmBody Parser::parseArmBody() {
    if (at(TokenKind::LBrace)) {
        return parseBlock();
    }
    return parseExpr();
}

// select "{" { SelectArm NL } "}"
ExprId Parser::parseSelect() {
    const std::int64_t lo = current().lo;
    bump();
    std::vector<SelectArm> arms;
    expect(TokenKind::LBrace, "'{' to open the select arms");
    const int savedDepth = saveBraceDepth();
    while (!at(TokenKind::RBrace) && !atEnd()) {
        const std::size_t before = pos_;
        parseSelectArm(arms);
        expectSeparator("expected a newline after the select arm");
        guardProgress(before, "unexpected token in the select arms");
    }
    restoreBraceDepth(savedDepth);
    expectClose("the select body is not closed; expected '}' before end of input");
    return spanExpr(lo, previousHi(), SelectExpr{std::move(arms)});
}

// await IDENT [ as Pattern ] "->" ArmBody, or timeout DURATION "->" ArmBody.
// Anything else reports and skips its line, so the next arm still parses
void Parser::parseSelectArm(std::vector<SelectArm>& arms) {
    const std::string_view arrowMessage = "a select arm needs '->' before its body";
    if (eat(TokenKind::KwAwait)) {
        std::string signal = ident();
        std::optional<PatternId> binding;
        if (atIdentText("as")) {
            bump();
            binding = parsePattern();
        }
        if (!eat(TokenKind::Arrow)) {
            error(std::string{arrowMessage});
        }
        arms.emplace_back(AwaitArm{std::move(signal), binding, parseArmBody()});
        return;
    }
    if (atIdentText("timeout")) {
        bump();
        const std::int64_t milliseconds = expectDuration("a duration after 'timeout'");
        if (!eat(TokenKind::Arrow)) {
            error(std::string{arrowMessage});
        }
        arms.emplace_back(TimeoutArm{milliseconds, parseArmBody()});
        return;
    }
    error(expectedFound("a select arm, 'await SIGNAL' or 'timeout DURATION'"));
    skipRestOfLine();
}

PatternId Parser::parsePattern() {
    PatternId pattern;
    if (enterNesting()) {
        pattern = parsePatternInner();
    } else {
        pattern = ast_.addPattern(WildcardPattern{});
    }
    leaveNesting();
    return pattern;
}

// "_", a literal, a negative number, a constructor, a variant, or a binding
PatternId Parser::parsePatternInner() {
    if (eat(TokenKind::Underscore)) {
        return ast_.addPattern(WildcardPattern{});
    }
    if (const std::optional<ExprId> literal = parseLiteralExpr()) {
        return ast_.addPattern(LiteralPattern{*literal});
    }
    if (at(TokenKind::Minus)) {
        return parseNegativePattern();
    }
    if (at(TokenKind::Ident)) {
        return parseIdentPattern(current());
    }
    error(expectedFound("a pattern"));
    bump();
    return ast_.addPattern(WildcardPattern{});
}

// "-" ( INT | FLOAT | DURATION ), kept as a negated literal
PatternId Parser::parseNegativePattern() {
    const std::int64_t lo = current().lo;
    bump();
    const bool isNumber =
        at(TokenKind::Int) || at(TokenKind::Float) || at(TokenKind::Duration);
    if (!isNumber) {
        error(expectedFound("a number or a duration after '-' in a pattern"));
        return ast_.addPattern(WildcardPattern{});
    }
    const ExprId literal = *parseLiteralExpr();
    const ExprId negated = spanExpr(lo, previousHi(), UnaryExpr{UnaryOp::Neg, literal});
    return ast_.addPattern(LiteralPattern{negated});
}

// Some(p), Ok(p), Err(p), None, Variant(p, ...), or a bare binding
PatternId Parser::parseIdentPattern(const Token& token) {
    const std::string_view name = tokenText(token);
    if (!nextIs(TokenKind::LParen)) {
        if (name == "None") {
            bump();
            return ast_.addPattern(NonePattern{});
        }
        return ast_.addPattern(BindPattern{ident()});
    }
    std::optional<WrapKind> kind;
    if (name == "Some") {
        kind = WrapKind::Some;
    } else if (name == "Ok") {
        kind = WrapKind::Ok;
    } else if (name == "Err") {
        kind = WrapKind::Err;
    }
    if (!kind) {
        return parseVariantPattern();
    }
    bump();
    bump();
    enterGroup();
    const PatternId inner = parsePattern();
    leaveGroup();
    expect(TokenKind::RParen, "')'");
    return ast_.addPattern(WrapPattern{*kind, inner});
}

// IDENT "(" Pattern { "," Pattern } ")"
PatternId Parser::parseVariantPattern() {
    std::string name = ident();
    bump();
    enterGroup();
    std::vector<PatternId> fields;
    if (!at(TokenKind::RParen)) {
        while (!atEnd()) {
            fields.push_back(parsePattern());
            if (!eat(TokenKind::Comma)) {
                break;
            }
        }
    }
    leaveGroup();
    expect(TokenKind::RParen, "')'");
    return ast_.addPattern(VariantPattern{std::move(name), std::move(fields)});
}

}  // namespace geas
