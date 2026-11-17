// The expression core: the precedence ladder by binding power with every
// binary operator left associative, unary "!" and "-" above it, and the
// postfix call, index, member, and "?" suffixes binding tightest. A newline
// stops the binary and postfix loops, but only outside every open
// parenthesis and bracket
#include "parser.hpp"

#include <optional>
#include <utility>

#include "duration.hpp"
#include "lexdump.hpp"
#include "size.hpp"

namespace geas {

namespace {

// Binding powers per precedence level, loosest lowest
constexpr int orPower = 10;
constexpr int andPower = 20;
constexpr int equalityPower = 30;
constexpr int relationalPower = 40;
constexpr int additivePower = 50;
constexpr int multiplicativePower = 60;

struct BinaryOperator {
    BinaryOp op;
    int power;
};

std::optional<BinaryOperator> binaryOperatorFor(TokenKind kind) {
    switch (kind) {
        case TokenKind::OrOr: return BinaryOperator{BinaryOp::Or, orPower};
        case TokenKind::AndAnd: return BinaryOperator{BinaryOp::And, andPower};
        case TokenKind::Eq: return BinaryOperator{BinaryOp::Eq, equalityPower};
        case TokenKind::Ne: return BinaryOperator{BinaryOp::Ne, equalityPower};
        case TokenKind::Lt: return BinaryOperator{BinaryOp::Lt, relationalPower};
        case TokenKind::Le: return BinaryOperator{BinaryOp::Le, relationalPower};
        case TokenKind::Gt: return BinaryOperator{BinaryOp::Gt, relationalPower};
        case TokenKind::Ge: return BinaryOperator{BinaryOp::Ge, relationalPower};
        case TokenKind::Plus: return BinaryOperator{BinaryOp::Add, additivePower};
        case TokenKind::Minus: return BinaryOperator{BinaryOp::Sub, additivePower};
        case TokenKind::Star: return BinaryOperator{BinaryOp::Mul, multiplicativePower};
        case TokenKind::Slash: return BinaryOperator{BinaryOp::Div, multiplicativePower};
        case TokenKind::Percent: return BinaryOperator{BinaryOp::Mod, multiplicativePower};
        default: return std::nullopt;
    }
}

}  // namespace

ExprId Parser::parseExpr() {
    return parseBinary(0);
}

// The head of if, while, for, and match. A bare record literal is off here,
// so "if x {" reads the brace as the body; parentheses opt back in
ExprId Parser::parseExprNoRecord() {
    const bool saved = std::exchange(isRecordBanned_, true);
    const ExprId expr = parseBinary(0);
    isRecordBanned_ = saved;
    return expr;
}

// A fresh expression position inside brackets, where record literals are
// allowed again whatever the enclosing head said
ExprId Parser::parseExprWithRecords() {
    const bool saved = std::exchange(isRecordBanned_, false);
    const ExprId expr = parseBinary(0);
    isRecordBanned_ = saved;
    return expr;
}

ExprId Parser::spanExpr(std::int64_t lo, std::int64_t hi, ExprKind kind) {
    return ast_.addExpr(Expr{lo, hi, std::move(kind)});
}

ExprId Parser::parseBinary(int minPower) {
    ExprId expr;
    if (enterNesting()) {
        expr = parseBinaryInner(minPower);
    } else {
        expr = placeholderExpr();
    }
    leaveNesting();
    return expr;
}

ExprId Parser::parseBinaryInner(int minPower) {
    ExprId lhs = parseUnary();
    while (!isNewlineTerminator()) {
        const std::optional<BinaryOperator> op = binaryOperatorFor(current().kind);
        if (!op || op->power < minPower) {
            break;
        }
        bump();
        const ExprId rhs = parseBinary(op->power + 1);
        lhs = spanExpr(ast_.expr(lhs).lo, ast_.expr(rhs).hi, BinaryExpr{op->op, lhs, rhs});
    }
    return lhs;
}

ExprId Parser::parseUnary() {
    ExprId expr;
    if (enterNesting()) {
        expr = parseUnaryInner();
    } else {
        expr = placeholderExpr();
    }
    leaveNesting();
    return expr;
}

ExprId Parser::parseUnaryInner() {
    const std::int64_t lo = current().lo;
    if (!at(TokenKind::Minus) && !at(TokenKind::Bang)) {
        return parsePostfix();
    }
    const UnaryOp op = at(TokenKind::Minus) ? UnaryOp::Neg : UnaryOp::Not;
    bump();
    const ExprId operand = parseUnary();
    return spanExpr(lo, ast_.expr(operand).hi, UnaryExpr{op, operand});
}

ExprId Parser::parsePostfix() {
    ExprId expr = parsePrimary();
    while (!isNewlineTerminator()) {
        const std::int64_t lo = ast_.expr(expr).lo;
        if (at(TokenKind::LParen)) {
            std::vector<Argument> args = parseCallArgs();
            expr = spanExpr(lo, previousHi(), CallExpr{expr, std::move(args)});
        } else if (at(TokenKind::LBracket)) {
            expr = parseIndexSuffix(expr);
        } else if (eat(TokenKind::Dot)) {
            std::string name = memberName();
            expr = spanExpr(lo, previousHi(), MemberExpr{expr, std::move(name)});
        } else if (eat(TokenKind::Question)) {
            expr = spanExpr(lo, previousHi(), QuestionExpr{expr});
        } else {
            break;
        }
    }
    return expr;
}

// "[" Expr "]" after a postfix base
ExprId Parser::parseIndexSuffix(ExprId base) {
    bump();
    enterGroup();
    const ExprId index = parseExprWithRecords();
    leaveGroup();
    expect(TokenKind::RBracket, "']'");
    return spanExpr(ast_.expr(base).lo, previousHi(), IndexExpr{base, index});
}

// "(" [ Arg { "," Arg } ] ")", a fresh expression position for records
std::vector<Argument> Parser::parseCallArgs() {
    bump();
    enterGroup();
    const bool saved = std::exchange(isRecordBanned_, false);
    std::vector<Argument> args;
    if (!at(TokenKind::RParen)) {
        while (!atEnd()) {
            args.push_back(parseArg());
            if (!eat(TokenKind::Comma)) {
                break;
            }
        }
    }
    isRecordBanned_ = saved;
    leaveGroup();
    expect(TokenKind::RParen, "')'");
    return args;
}

// Labeled when an identifier is directly followed by a colon, which can
// never continue an expression that starts with a lone identifier
Argument Parser::parseArg() {
    std::optional<std::string> label;
    if (at(TokenKind::Ident) && nextIs(TokenKind::Colon)) {
        label = ident();
        bump();
    }
    const ExprId value = parseExpr();
    return Argument{std::move(label), value};
}

// A literal, or nullopt when the current token is not one. Shared by
// primaries, patterns, and attribute values, so all three decode alike
std::optional<ExprId> Parser::parseLiteralExpr() {
    const Token token = current();
    const auto make = [&](ExprKind kind) {
        bump();
        return spanExpr(token.lo, token.hi, std::move(kind));
    };
    switch (token.kind) {
        case TokenKind::Int: return make(IntLiteral{intValue(text_, token.lo, token.hi)});
        case TokenKind::Size:
            return make(IntLiteral{parseSize(tokenText(token)).value_or(0), true});
        case TokenKind::Float: return make(FloatLiteral{std::string{tokenText(token)}});
        case TokenKind::String: return make(StringLiteral{stringLiteralBody(text_, token)});
        case TokenKind::Char: return make(CharLiteral{charLiteralScalar(text_, token.lo + 1)});
        case TokenKind::KwTrue: return make(BoolLiteral{true});
        case TokenKind::KwFalse: return make(BoolLiteral{false});
        case TokenKind::Duration:
            return make(DurationLiteral{parseDuration(tokenText(token)).value_or(0)});
        default: return std::nullopt;
    }
}

ExprId Parser::parsePrimary() {
    if (const std::optional<ExprId> literal = parseLiteralExpr()) {
        return *literal;
    }
    const Token token = current();
    switch (token.kind) {
        case TokenKind::KwMatch: return parseMatch();
        case TokenKind::KwSelect: return parseSelect();
        case TokenKind::KwAwait: return parseAwait();
        case TokenKind::KwMut: return parseMutExpr();
        case TokenKind::LParen: return parseParen(token.lo);
        case TokenKind::LBracket: return parseList(token.lo);
        case TokenKind::Ident: return parseIdentPrimary(token);
        default: break;
    }
    if (at(TokenKind::Underscore)) {
        error("'_' is a pattern, not an expression");
        bump();
        return placeholderExpr();
    }
    error(expectedFound("an expression"));
    bump();
    return spanExpr(token.lo, token.hi, Identifier{""});
}

}  // namespace geas
