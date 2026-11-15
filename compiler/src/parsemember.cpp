// Pledges, clauses, subcontracts, schemas, and attribute lists. A pledge's
// terms may sit on their own lines under the signature, while its attribute
// list and body must open on the line of whatever came right before them
#include "parser.hpp"

#include <array>
#include <utility>

namespace geas {

namespace {

constexpr std::array knownAttrKeys{
    std::string_view{"version"},
    std::string_view{"term"},
    std::string_view{"abi"},
    std::string_view{"symbol"},
};

bool isKnownAttrKey(std::string_view key) {
    for (const std::string_view known : knownAttrKeys) {
        if (key == known) {
            return true;
        }
    }
    return false;
}

}  // namespace

// pledge IDENT "(" [ Params ] ")" "->" Type Terms [ Attrs ] [ Block ]. A
// missing arrow is reported once and the pledge keeps a placeholder return
PledgeDecl Parser::parsePledge() {
    const std::int64_t lo = current().lo;
    bump();
    std::string name = ident();
    const std::int64_t hi = previousHi();
    std::vector<Field> params = parseParams();
    const TypeId ret = parseReturnType("a pledge declares its return type with '->'");
    std::vector<Term> terms = parseTerms();
    std::vector<Attr> attrs = parseAttrsOnLine();
    const std::optional<BlockId> body = parseBodyOnLine();
    return PledgeDecl{std::move(name), lo,      hi, std::move(params), ret, std::move(terms),
                      std::move(attrs), body};
}

// clause IDENT "(" [ Params ] ")" "->" Type Block, never abstract
ClauseDecl Parser::parseClause() {
    const std::int64_t lo = current().lo;
    bump();
    std::string name = ident();
    const std::int64_t hi = previousHi();
    std::vector<Field> params = parseParams();
    const TypeId ret = parseReturnType("a clause declares its return type with '->'");
    std::optional<BlockId> body = parseBodyOnLine();
    if (!body) {
        error("a clause needs a body block after its return type");
        body = emptyBlock();
    }
    return ClauseDecl{std::move(name), lo, hi, std::move(params), ret, *body};
}

// subcontract [ IDENT ] [ transactional ] Terms "{" { SubMember NL } "}"
SubcontractDecl Parser::parseSubcontract() {
    const std::int64_t lo = current().lo;
    bump();
    std::optional<std::string> name;
    if (at(TokenKind::Ident)) {
        name = ident();
    }
    const bool isTransactional = eat(TokenKind::KwTransactional);
    const std::int64_t hi = previousHi();
    std::vector<Term> terms = parseTerms();
    std::vector<SubMember> members;
    expect(TokenKind::LBrace, "'{' to open the subcontract body");
    while (!at(TokenKind::RBrace) && !atEnd()) {
        const std::size_t before = pos_;
        parseSubMember(members);
        guardProgress(before, "unexpected token in the subcontract");
    }
    expectClose("the subcontract body is not closed; expected '}' before end of input");
    return SubcontractDecl{std::move(name),  lo, hi, isTransactional, std::move(terms),
                           std::move(members)};
}

// A pledge or a signal. A nested subcontract is reported, then parsed and
// discarded, so the parse resumes after the whole intruder
void Parser::parseSubMember(std::vector<SubMember>& members) {
    if (at(TokenKind::KwPledge)) {
        members.emplace_back(parsePledge());
        expectSeparator("expected a newline after the pledge");
        return;
    }
    if (at(TokenKind::KwSignal)) {
        members.emplace_back(parseSignal());
        expectSeparator("expected a newline after the signal");
        return;
    }
    if (!at(TokenKind::KwSubcontract)) {
        error(expectedFound("a pledge or a signal inside the subcontract"));
        return;
    }
    error("a subcontract cannot contain another subcontract");
    if (enterNesting()) {
        parseSubcontract();
    }
    leaveNesting();
}

// schema IDENT "{" { IDENT ":" Type [ "," ] NL } "}". A comma separates two
// columns on one line, so the block form and the inline form both parse
SchemaDecl Parser::parseSchema() {
    const std::int64_t lo = current().lo;
    bump();
    std::string name = ident();
    const std::int64_t hi = previousHi();
    std::vector<Field> columns;
    expect(TokenKind::LBrace, "'{' to open the schema body");
    while (!at(TokenKind::RBrace) && !atEnd()) {
        const std::size_t before = pos_;
        columns.push_back(parseField("':' between the column name and its type"));
        if (!eat(TokenKind::Comma)) {
            expectSeparator("expected a newline or ',' after the schema column");
        }
        guardProgress(before, "expected a column in the schema body");
    }
    expectClose("the schema body is not closed; expected '}' before end of input");
    return SchemaDecl{std::move(name), lo, hi, std::move(columns)};
}

// "->" Type, or the message and a placeholder so one slip does not cascade
TypeId Parser::parseReturnType(std::string_view missingMessage) {
    if (eat(TokenKind::Arrow)) {
        return parseType();
    }
    error(std::string{missingMessage});
    return placeholderType();
}

// An attribute list opening on the current line, or none
std::vector<Attr> Parser::parseAttrsOnLine() {
    if (!at(TokenKind::LBracket) || hasNewlineHere()) {
        return {};
    }
    return parseAttrs();
}

// Attrs ::= "[" IDENT ":" Literal { "," IDENT ":" Literal } "]"
std::vector<Attr> Parser::parseAttrs() {
    bump();
    enterGroup();
    std::vector<Attr> attrs;
    while (!at(TokenKind::RBracket) && !atEnd()) {
        parseAttr(attrs);
        if (!eat(TokenKind::Comma)) {
            break;
        }
    }
    leaveGroup();
    expect(TokenKind::RBracket, "']'");
    return attrs;
}

// One key and literal. The known keys are fixed by the grammar, so an
// unknown one is refused here rather than left to confuse the checker
void Parser::parseAttr(std::vector<Attr>& attrs) {
    std::string key = ident();
    if (!isKnownAttrKey(key)) {
        error("unknown attribute key; the known keys are version, term, abi, and symbol");
    }
    expect(TokenKind::Colon, "':' between the attribute key and its value");
    const std::optional<ExprId> value = parseLiteralExpr();
    if (!value) {
        error("an attribute value must be a literal");
        bump();
        return;
    }
    attrs.push_back(Attr{std::move(key), *value});
}

// A body block opening on the current line, or none
std::optional<BlockId> Parser::parseBodyOnLine() {
    if (!at(TokenKind::LBrace) || hasNewlineHere()) {
        return std::nullopt;
    }
    return parseBlock();
}

}  // namespace geas
