// Blocks and statements. A block resets the grouping depth, so statements
// inside it end on newlines even when the block sits in an argument list.
// sleep and together parse in any body; keeping them inside a course is the
// checker's job
#include "parser.hpp"

#include <utility>

namespace geas {

// Block ::= "{" { Stmt NL } "}"
BlockId Parser::parseBlock() {
    if (!enterNesting()) {
        leaveNesting();
        return emptyBlock();
    }
    expect(TokenKind::LBrace, "'{'");
    const int savedDepth = saveBraceDepth();
    Block block;
    while (!at(TokenKind::RBrace) && !atEnd()) {
        const std::size_t before = pos_;
        block.stmts.push_back(parseStmt());
        expectSeparator("expected a newline after the statement");
        guardProgress(before, "unexpected token in the block");
    }
    restoreBraceDepth(savedDepth);
    expectClose("the block is not closed; expected '}' before end of input");
    leaveNesting();
    return ast_.addBlock(std::move(block));
}

StmtId Parser::parseStmt() {
    switch (current().kind) {
        case TokenKind::KwLet: return parseLet();
        case TokenKind::KwReturn: return parseReturn();
        case TokenKind::KwIf: return parseIf();
        case TokenKind::KwWhile: return parseWhile();
        case TokenKind::KwFor: return parseFor();
        case TokenKind::KwSleep: return parseSleep();
        case TokenKind::KwTogether: return parseTogether();
        case TokenKind::KwBreak: bump(); return ast_.addStmt(BreakStmt{});
        case TokenKind::KwContinue: bump(); return ast_.addStmt(ContinueStmt{});
        default: return parseSimpleStmt();
    }
}

// Expr [ "=" Expr ], the assignment target checked as a place later
StmtId Parser::parseSimpleStmt() {
    const ExprId expr = parseExpr();
    if (!isNewlineTerminator() && eat(TokenKind::Assign)) {
        const ExprId value = parseExpr();
        return ast_.addStmt(AssignStmt{expr, value});
    }
    return ast_.addStmt(ExprStmt{expr});
}

// let [ mut ] IDENT [ ":" Type ] "=" Expr
StmtId Parser::parseLet() {
    bump();
    const bool isMutable = eat(TokenKind::KwMut);
    std::string name = ident();
    std::optional<TypeId> type;
    if (eat(TokenKind::Colon)) {
        type = parseType();
    }
    expect(TokenKind::Assign, "'=' in the let binding");
    const ExprId value = parseExpr();
    return ast_.addStmt(LetStmt{isMutable, std::move(name), type, value});
}

// return [ Expr ], bare when the statement ends right there
StmtId Parser::parseReturn() {
    bump();
    if (at(TokenKind::RBrace) || atEnd() || isNewlineTerminator()) {
        return ast_.addStmt(ReturnStmt{std::nullopt});
    }
    const ExprId value = parseExpr();
    return ast_.addStmt(ReturnStmt{value});
}

// if CondExpr Block { else if CondExpr Block } [ else Block ]. The else reads
// across a newline, nothing else can start with it
StmtId Parser::parseIf() {
    bump();
    const ExprId condition = parseExprNoRecord();
    const BlockId thenBlock = parseBlock();
    const std::optional<BlockId> elseBlock = parseElse();
    return ast_.addStmt(IfStmt{condition, thenBlock, elseBlock});
}

// The else arm, an else if chain stored as a block holding one if statement
std::optional<BlockId> Parser::parseElse() {
    if (!eat(TokenKind::KwElse)) {
        return std::nullopt;
    }
    if (!at(TokenKind::KwIf)) {
        return parseBlock();
    }
    Block chain;
    if (enterNesting()) {
        chain.stmts.push_back(parseIf());
    }
    leaveNesting();
    return ast_.addBlock(std::move(chain));
}

StmtId Parser::parseWhile() {
    bump();
    const ExprId condition = parseExprNoRecord();
    const BlockId body = parseBlock();
    return ast_.addStmt(WhileStmt{condition, body});
}

// for IDENT in CondExpr Block
StmtId Parser::parseFor() {
    bump();
    std::string variable = ident();
    expect(TokenKind::KwIn, "'in' between the loop variable and its sequence");
    const ExprId iterable = parseExprNoRecord();
    const BlockId body = parseBlock();
    return ast_.addStmt(ForStmt{std::move(variable), iterable, body});
}

// sleep Expr, a durable pause
StmtId Parser::parseSleep() {
    bump();
    const ExprId duration = parseExpr();
    return ast_.addStmt(SleepStmt{duration});
}

// together Block, every statement inside starting concurrently
StmtId Parser::parseTogether() {
    bump();
    const BlockId body = parseBlock();
    return ast_.addStmt(TogetherStmt{body});
}

}  // namespace geas
