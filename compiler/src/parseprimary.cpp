// The primaries that open a nested form: mut(..), parentheses and tuples,
// list literals, the syntactic constructors, generic constructors, record
// literals, and await. Every bracket here is a group, so its newlines are
// silent, and a fresh expression position, so record literals come back
#include "parser.hpp"

#include <array>
#include <utility>

namespace geas {

namespace {

// The builtin composites, apart from Map, whose "<" opens type arguments
constexpr std::array genericCtorNames{
    std::string_view{"List"},
    std::string_view{"Option"},
    std::string_view{"Result"},
    std::string_view{"Tuple"},
};

bool isGenericCtorName(std::string_view name) {
    for (const std::string_view generic : genericCtorNames) {
        if (name == generic) {
            return true;
        }
    }
    return false;
}

}  // namespace

// mut "(" Expr ")", the builtin that copies a vow into a mutable local
ExprId Parser::parseMutExpr() {
    const std::int64_t lo = current().lo;
    bump();
    expect(TokenKind::LParen, "'(' after 'mut'");
    enterGroup();
    const ExprId inner = parseExprWithRecords();
    leaveGroup();
    expect(TokenKind::RParen, "')'");
    return spanExpr(lo, previousHi(), WrapExpr{WrapKind::Mut, inner});
}

// A parenthesized expression or a tuple. One element re wraps the inner node
// with the outer span; there is no unit literal, so bare () is a fault
ExprId Parser::parseParen(std::int64_t lo) {
    bump();
    enterGroup();
    const bool saved = std::exchange(isRecordBanned_, false);
    if (at(TokenKind::RParen)) {
        error("expected an expression inside the parentheses");
        bump();
        isRecordBanned_ = saved;
        leaveGroup();
        return placeholderExpr();
    }
    const ExprId first = parseExpr();
    std::vector<ExprId> elements{first};
    while (eat(TokenKind::Comma)) {
        elements.push_back(parseExpr());
    }
    isRecordBanned_ = saved;
    leaveGroup();
    expect(TokenKind::RParen, "')'");
    if (elements.size() > 1) {
        return spanExpr(lo, previousHi(), TupleExpr{std::move(elements)});
    }
    ExprKind inner = ast_.expr(first).kind;
    return spanExpr(lo, previousHi(), std::move(inner));
}

// "[" [ Expr { "," Expr } ] "]"
ExprId Parser::parseList(std::int64_t lo) {
    bump();
    enterGroup();
    const bool saved = std::exchange(isRecordBanned_, false);
    std::vector<ExprId> elements = parseExprList(TokenKind::RBracket);
    isRecordBanned_ = saved;
    leaveGroup();
    expect(TokenKind::RBracket, "']'");
    return spanExpr(lo, previousHi(), ListExpr{std::move(elements)});
}

// Comma separated expressions up to the closing token, which is not eaten
std::vector<ExprId> Parser::parseExprList(TokenKind close) {
    std::vector<ExprId> elements;
    if (at(close)) {
        return elements;
    }
    while (!atEnd()) {
        elements.push_back(parseExpr());
        if (!eat(TokenKind::Comma)) {
            break;
        }
    }
    return elements;
}

// An identifier primary: Some, Ok, and Err applied, None, a generic
// constructor, a record literal when a brace opens on the same line and the
// position allows one, or a plain name
ExprId Parser::parseIdentPrimary(const Token& token) {
    const std::string_view name = tokenText(token);
    if (nextIs(TokenKind::LParen)) {
        if (name == "Some") {
            return parseWrapCtor(token, WrapKind::Some);
        }
        if (name == "Ok") {
            return parseWrapCtor(token, WrapKind::Ok);
        }
        if (name == "Err") {
            return parseWrapCtor(token, WrapKind::Err);
        }
    }
    if (name == "None") {
        bump();
        return spanExpr(token.lo, token.hi, NoneExpr{});
    }
    if (nextIs(TokenKind::Lt) && name == "Map") {
        return parseMapNew(token);
    }
    if (nextIs(TokenKind::Lt) && isGenericCtorName(name)) {
        return parseGenericNew(token);
    }
    bump();
    if (at(TokenKind::LBrace) && !isRecordBanned_ && !hasNewlineHere()) {
        return parseRecordLiteral(std::string{name}, token.lo);
    }
    return spanExpr(token.lo, token.hi, Identifier{std::string{name}});
}

// Some(e), Ok(e), Err(e)
ExprId Parser::parseWrapCtor(const Token& token, WrapKind kind) {
    bump();
    bump();
    enterGroup();
    const ExprId inner = parseExprWithRecords();
    leaveGroup();
    expect(TokenKind::RParen, "')'");
    return spanExpr(token.lo, previousHi(), WrapExpr{kind, inner});
}

// Map<K, V>(), the type grammar eating the arguments and the empty
// parentheses closing the constructor
ExprId Parser::parseMapNew(const Token& token) {
    const TypeId type = parseType();
    expect(TokenKind::LParen, "'(' after the Map constructor type");
    expect(TokenKind::RParen, "')'; a Map starts empty, its constructor takes no arguments");
    return spanExpr(token.lo, previousHi(), MapNewExpr{type});
}

// List<T>(..), Option<T>(..), Result<T, E>(..), Tuple<..>(..)
ExprId Parser::parseGenericNew(const Token& token) {
    const TypeId type = parseType();
    std::vector<Argument> args;
    if (at(TokenKind::LParen)) {
        args = parseCallArgs();
    } else {
        error(expectedFound("'(' after the constructor type"));
    }
    return spanExpr(token.lo, previousHi(), GenericNewExpr{type, std::move(args)});
}

// IDENT "{" [ IDENT ":" Expr { "," IDENT ":" Expr } ] "}". A newline between
// fields is fine, nothing here consults the newline flag
ExprId Parser::parseRecordLiteral(std::string name, std::int64_t lo) {
    bump();
    const int savedDepth = saveBraceDepth();
    std::vector<FieldInit> fields;
    while (!atEnd() && !at(TokenKind::RBrace)) {
        std::string fieldName = ident();
        expect(TokenKind::Colon, "':' between the field name and its value");
        const ExprId value = parseExprWithRecords();
        fields.push_back(FieldInit{std::move(fieldName), value});
        if (!eat(TokenKind::Comma)) {
            break;
        }
    }
    restoreBraceDepth(savedDepth);
    expect(TokenKind::RBrace, "'}'");
    return spanExpr(lo, previousHi(), RecordLiteral{std::move(name), std::move(fields)});
}

// await IDENT [ within DURATION ]. The within reads across a newline, the
// same continuation rule the terms follow
ExprId Parser::parseAwait() {
    const std::int64_t lo = current().lo;
    bump();
    std::string signal = ident();
    std::optional<std::int64_t> within;
    if (eat(TokenKind::KwWithin)) {
        within = expectDuration("a duration after 'within'");
    }
    return spanExpr(lo, previousHi(), AwaitExpr{std::move(signal), within});
}

}  // namespace geas
