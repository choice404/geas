// The type grammar and the parameter list. A type is a name with optional
// angle bracket arguments, Tuple<..> included, or the first class pledge
// type pledge(T, ...) -> R. The lexer never fuses ">>", so nested arguments
// close one ">" at a time. Params, record fields, variant fields, and schema
// columns are all the same FieldDecl production and share parseField
#include "parser.hpp"

#include <utility>

namespace geas {

TypeId Parser::parseType() {
    TypeId type;
    if (enterNesting()) {
        type = parseTypeInner();
    } else {
        type = placeholderType();
    }
    leaveNesting();
    return type;
}

TypeId Parser::parseTypeInner() {
    if (at(TokenKind::KwPledge)) {
        return parsePledgeType();
    }
    if (!at(TokenKind::Ident)) {
        error(expectedFound("a type"));
        return placeholderType();
    }
    std::string name = ident();
    if (!at(TokenKind::Lt)) {
        return ast_.addType(NamedType{std::move(name), {}});
    }
    std::vector<TypeId> args = parseTypeArgs();
    return ast_.addType(NamedType{std::move(name), std::move(args)});
}

// "<" Type { "," Type } ">", a group so a long instantiation can wrap
std::vector<TypeId> Parser::parseTypeArgs() {
    expect(TokenKind::Lt, "'<'");
    enterGroup();
    std::vector<TypeId> args;
    if (!at(TokenKind::Gt)) {
        args = parseTypeList();
    }
    leaveGroup();
    expect(TokenKind::Gt, "'>'");
    return args;
}

// pledge "(" [ Type { "," Type } ] ")" "->" Type
TypeId Parser::parsePledgeType() {
    bump();
    expect(TokenKind::LParen, "'('");
    enterGroup();
    std::vector<TypeId> params;
    if (!at(TokenKind::RParen)) {
        params = parseTypeList();
    }
    leaveGroup();
    expect(TokenKind::RParen, "')'");
    const TypeId ret = parseReturnType("a pledge type declares its return type with '->'");
    return ast_.addType(PledgeType{std::move(params), ret});
}

// Comma separated types up to whatever closes the list
std::vector<TypeId> Parser::parseTypeList() {
    std::vector<TypeId> types;
    while (!atEof()) {
        types.push_back(parseType());
        if (!eat(TokenKind::Comma)) {
            break;
        }
    }
    return types;
}

// "(" [ Param { "," Param } ] ")" with Param ::= IDENT ":" Type
std::vector<Field> Parser::parseParams() {
    expect(TokenKind::LParen, "'('");
    enterGroup();
    std::vector<Field> params;
    if (!at(TokenKind::RParen)) {
        while (!atEof()) {
            params.push_back(parseField("':' between the parameter name and its type"));
            if (!eat(TokenKind::Comma)) {
                break;
            }
        }
    }
    leaveGroup();
    expect(TokenKind::RParen, "')'");
    return params;
}

// IDENT ":" Type, the colon message naming what the pair is
Field Parser::parseField(std::string_view colonWhat) {
    std::string name = ident();
    expect(TokenKind::Colon, colonWhat);
    const TypeId type = parseType();
    return Field{std::move(name), type};
}

}  // namespace geas
