// The requirements block and its small boolean grammar over atoms. The
// labels fulfill and partial are contextual identifiers while break is the
// hard keyword doing double duty; "!" binds tighter than "&&", which binds
// tighter than "||"
#include "parser.hpp"

#include <utility>

namespace geas {

// requirements "{" { ReqLabel ":" ReqExpr NL } "}"
RequirementsDecl Parser::parseRequirements() {
    RequirementsDecl block{current().lo, current().hi, std::nullopt, std::nullopt, std::nullopt};
    bump();
    expect(TokenKind::LBrace, "'{' to open the requirements block");
    while (!at(TokenKind::RBrace) && !atEnd()) {
        const std::size_t before = pos_;
        parseRequirementsLine(block);
        guardProgress(before, "unexpected token in the requirements block");
    }
    expectClose("the requirements block is not closed; expected '}' before end of input");
    return block;
}

// One labeled line. An unknown label consumes its whole line so it reports
// exactly once
void Parser::parseRequirementsLine(RequirementsDecl& block) {
    if (atIdentText("fulfill")) {
        storeRequirementsLine(block.fulfill, "fulfill");
        return;
    }
    if (atIdentText("partial")) {
        storeRequirementsLine(block.partial, "partial");
        return;
    }
    if (at(TokenKind::KwBreak)) {
        storeRequirementsLine(block.breakLine, "break");
        return;
    }
    error("unknown requirements label; expected 'fulfill', 'partial', or 'break'");
    bump();
    if (eat(TokenKind::Colon)) {
        parseReqOr();
    }
}

// Parses the line after its label into the slot, or reports a second one
void Parser::storeRequirementsLine(std::optional<ReqId>& slot, std::string_view label) {
    bump();
    const ReqId line = parseReqLine();
    if (slot) {
        error("duplicate '" + std::string{label} + "' line in the requirements block");
        return;
    }
    slot = line;
}

ReqId Parser::parseReqLine() {
    expect(TokenKind::Colon, "':' after the requirements label");
    const ReqId formula = parseReqOr();
    expectSeparator("expected a newline after the requirements line");
    return formula;
}

ReqId Parser::parseReqOr() {
    ReqId formula;
    if (enterNesting()) {
        formula = parseReqOrInner();
    } else {
        formula = ast_.addReq(ReqNode{current().lo, current().hi, ReqAtom{""}});
    }
    leaveNesting();
    return formula;
}

ReqId Parser::parseReqOrInner() {
    ReqId lhs = parseReqAnd();
    while (!isNewlineTerminator() && eat(TokenKind::OrOr)) {
        lhs = combineReq(lhs, parseReqAnd(), false);
    }
    return lhs;
}

ReqId Parser::parseReqAnd() {
    ReqId lhs = parseReqNot();
    while (!isNewlineTerminator() && eat(TokenKind::AndAnd)) {
        lhs = combineReq(lhs, parseReqNot(), true);
    }
    return lhs;
}

// An and or an or node spanning both operands
ReqId Parser::combineReq(ReqId lhs, ReqId rhs, bool isAnd) {
    const std::int64_t lo = ast_.req(lhs).lo;
    const std::int64_t hi = ast_.req(rhs).hi;
    if (isAnd) {
        return ast_.addReq(ReqNode{lo, hi, ReqAnd{lhs, rhs}});
    }
    return ast_.addReq(ReqNode{lo, hi, ReqOr{lhs, rhs}});
}

ReqId Parser::parseReqNot() {
    if (!eat(TokenKind::Bang)) {
        return parseReqAtom();
    }
    const std::int64_t lo = previousHi() - 1;
    const ReqId operand = parseReqAtom();
    return ast_.addReq(ReqNode{lo, ast_.req(operand).hi, ReqNot{operand}});
}

ReqId Parser::parseReqAtom() {
    if (eat(TokenKind::LParen)) {
        enterGroup();
        const ReqId inner = parseReqOr();
        leaveGroup();
        expect(TokenKind::RParen, "')'");
        return inner;
    }
    const std::int64_t lo = current().lo;
    const std::int64_t hi = current().hi;
    if (at(TokenKind::Ident)) {
        return ast_.addReq(ReqNode{lo, hi, ReqAtom{ident()}});
    }
    error(expectedFound("a subcontract, pledge, or signal name"));
    bump();
    return ast_.addReq(ReqNode{lo, hi, ReqAtom{""}});
}

}  // namespace geas
