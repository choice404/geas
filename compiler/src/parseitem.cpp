// The file and declaration grammar. A file is imports first, then top level
// declarations: records, sums, provisional clauses, and contracts, each
// ended by a newline. Contract members dispatch on their leading keyword and
// keep their source order
#include "parser.hpp"

#include <utility>

namespace geas {

void Parser::parseFile() {
    while (at(TokenKind::KwImport)) {
        parseImport();
        expectSeparator("expected a newline after the import");
    }
    while (!atEnd()) {
        const std::size_t before = pos_;
        parseDecl();
        expectSeparator("expected a newline after the declaration");
        guardProgress(before, "expected a declaration at the top level");
    }
}

// import IDENT { "." IDENT }, the dotted path kept as one string
void Parser::parseImport() {
    const std::int64_t lo = current().lo;
    bump();
    std::string path = ident();
    while (eat(TokenKind::Dot)) {
        path.push_back('.');
        path.append(ident());
    }
    ast_.imports.push_back(Import{std::move(path), lo, previousHi()});
}

void Parser::parseDecl() {
    if (at(TokenKind::KwImport)) {
        // A late import is reported and still recorded, so the rest of the
        // file parses against the names it was written against
        error("imports must appear before every declaration");
        parseImport();
        return;
    }
    if (at(TokenKind::KwRecord)) {
        ast_.decls.emplace_back(parseRecord());
        return;
    }
    if (eat(TokenKind::KwInternal)) {
        parseInternalDecl();
        return;
    }
    if (at(TokenKind::KwProvisional)) {
        ast_.decls.emplace_back(parseProvisional(false));
        return;
    }
    if (at(TokenKind::KwContract)) {
        ast_.decls.emplace_back(parseContract(false));
        return;
    }
    if (at(TokenKind::Ident) && nextIs(TokenKind::KwIs)) {
        ast_.decls.emplace_back(parseSum());
        return;
    }
    error(expectedFound("a declaration"));
    bump();
}

// What follows "internal", a provisional clause or a contract
void Parser::parseInternalDecl() {
    if (at(TokenKind::KwProvisional)) {
        ast_.decls.emplace_back(parseProvisional(true));
        return;
    }
    if (at(TokenKind::KwContract)) {
        ast_.decls.emplace_back(parseContract(true));
        return;
    }
    error("'internal' applies to a contract or a provisional clause");
}

// record IDENT "{" { FieldDecl NL } "}"
RecordDecl Parser::parseRecord() {
    const std::int64_t lo = current().lo;
    bump();
    std::string name = ident();
    const std::int64_t hi = previousHi();
    std::vector<Field> fields;
    expect(TokenKind::LBrace, "'{' to open the record body");
    while (!at(TokenKind::RBrace) && !atEnd()) {
        const std::size_t before = pos_;
        fields.push_back(parseField("':' between the field name and its type"));
        expectSeparator("expected a newline after the record field");
        guardProgress(before, "expected a field in the record body");
    }
    expectClose("the record body is not closed; expected '}' before end of input");
    return RecordDecl{std::move(name), lo, hi, std::move(fields)};
}

// IDENT "is" "either" Variant { "or" Variant }. An "or" continues the chain
// even when it opens a new line, so a long sum can list one variant a line
SumDecl Parser::parseSum() {
    const std::int64_t lo = current().lo;
    std::string name = ident();
    bump();
    expect(TokenKind::KwEither, "'either' after 'is'");
    std::vector<Variant> variants;
    variants.push_back(parseVariant());
    while (!isHalted() && eat(TokenKind::KwOr)) {
        variants.push_back(parseVariant());
    }
    return SumDecl{std::move(name), lo, previousHi(), std::move(variants)};
}

// Variant ::= IDENT [ "(" FieldDecl { "," FieldDecl } ")" ], the field list
// opening on the variant's own line
Variant Parser::parseVariant() {
    std::string name = ident();
    if (!at(TokenKind::LParen) || isNewlineTerminator()) {
        return Variant{std::move(name), {}};
    }
    return Variant{std::move(name), parseVariantFields()};
}

std::vector<Field> Parser::parseVariantFields() {
    bump();
    enterGroup();
    std::vector<Field> fields;
    while (!at(TokenKind::RParen) && !atEnd()) {
        fields.push_back(parseField("':' between the field name and its type"));
        if (!eat(TokenKind::Comma)) {
            break;
        }
    }
    leaveGroup();
    expect(TokenKind::RParen, "')'");
    return fields;
}

// [ internal ] provisional clause IDENT "{" { ClauseSig NL } "}", signatures
// only, the bodies belong to the incorporating contract
ProvisionalDecl Parser::parseProvisional(bool isInternal) {
    const std::int64_t lo = current().lo;
    bump();
    expect(TokenKind::KwClause, "'clause' after 'provisional'");
    std::string name = ident();
    const std::int64_t hi = previousHi();
    std::vector<ClauseSig> sigs;
    expect(TokenKind::LBrace, "'{' to open the provisional clause body");
    while (!at(TokenKind::RBrace) && !atEnd()) {
        const std::size_t before = pos_;
        parseClauseSig(sigs);
        guardProgress(before, "unexpected token in the provisional clause");
    }
    expectClose("the provisional clause body is not closed; expected '}' before end of input");
    return ProvisionalDecl{isInternal, std::move(name), lo, hi, std::move(sigs)};
}

// ClauseSig ::= clause IDENT "(" [ Params ] ")" "->" Type
void Parser::parseClauseSig(std::vector<ClauseSig>& sigs) {
    if (!eat(TokenKind::KwClause)) {
        error("expected a 'clause' signature inside the provisional clause");
        return;
    }
    std::string name = ident();
    std::vector<Field> params = parseParams();
    const TypeId ret = parseReturnType("a clause signature declares its return type with '->'");
    sigs.push_back(ClauseSig{std::move(name), std::move(params), ret});
    expectSeparator("expected a newline after the clause signature");
}

// [ internal ] contract IDENT [ Attrs ] "{" { Member NL } "}"
ContractDecl Parser::parseContract(bool isInternal) {
    const std::int64_t lo = current().lo;
    bump();
    std::string name = ident();
    const std::int64_t hi = previousHi();
    std::vector<Attr> attrs = parseAttrsOnLine();
    std::vector<Member> members;
    expect(TokenKind::LBrace, "'{' to open the contract body");
    while (!at(TokenKind::RBrace) && !atEnd()) {
        const std::size_t before = pos_;
        parseMember(members);
        expectSeparator("expected a newline after the contract member");
        guardProgress(before, "expected a contract member");
    }
    expectClose("the contract body is not closed; expected '}' before end of input");
    return ContractDecl{isInternal, std::move(name), lo, hi, std::move(attrs), std::move(members)};
}

void Parser::parseMember(std::vector<Member>& members) {
    switch (current().kind) {
        case TokenKind::KwIncorporate: members.emplace_back(parseIncorporate()); return;
        case TokenKind::KwVow: members.emplace_back(parseVow()); return;
        case TokenKind::KwPledge: members.emplace_back(parsePledge()); return;
        case TokenKind::KwClause: members.emplace_back(parseClause()); return;
        case TokenKind::KwSubcontract: members.emplace_back(parseSubcontract()); return;
        case TokenKind::KwSchema: members.emplace_back(parseSchema()); return;
        case TokenKind::KwRequirements: members.emplace_back(parseRequirements()); return;
        default: break;
    }
    if (parseWorkflowMember(members)) {
        return;
    }
    error(expectedFound("a contract member"));
    bump();
}

// The workflow members, remedy, signal, course, and amends. False when the
// current token opens none of them
bool Parser::parseWorkflowMember(std::vector<Member>& members) {
    switch (current().kind) {
        case TokenKind::KwRemedy: members.emplace_back(parseRemedy()); return true;
        case TokenKind::KwSignal: members.emplace_back(parseSignal()); return true;
        case TokenKind::KwCourse: members.emplace_back(parseCourse()); return true;
        case TokenKind::KwAmends: members.emplace_back(parseAmends()); return true;
        default: return false;
    }
}

// incorporate IDENT
IncorporateDecl Parser::parseIncorporate() {
    const std::int64_t lo = current().lo;
    bump();
    std::string name = ident();
    return IncorporateDecl{std::move(name), lo, previousHi()};
}

// vow IDENT ":" Type [ "=" Expr ]
VowDecl Parser::parseVow() {
    const std::int64_t lo = current().lo;
    bump();
    std::string name = ident();
    const std::int64_t hi = previousHi();
    expect(TokenKind::Colon, "':' between the vow name and its type");
    const TypeId type = parseType();
    std::optional<ExprId> init;
    if (eat(TokenKind::Assign)) {
        init = parseExpr();
    }
    return VowDecl{std::move(name), lo, hi, type, init};
}

}  // namespace geas
