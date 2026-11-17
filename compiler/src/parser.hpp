// The geas parser, hand written recursive descent over the lexed tokens with
// precedence climbing for binary operators. It never stops at the first
// fault: every entry point hands back a node, a placeholder when the source
// was wrong, and records the fault, so one mistake does not hide the next.
// The one exception is the nesting ceiling, which reports once and halts.
//
// Newlines arrive as a flag on the next token. A newline ends a statement
// only outside every open parenthesis and bracket, braces reset that, and
// the workflow continuations, a term keyword, an "or" in a sum, or an
// "else", read across a newline. The member functions are spread over the
// parse*.cpp files by grammar area
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ast.hpp"
#include "diagnostics.hpp"
#include "token.hpp"

namespace geas {

// What a finished parse hands back, the tree and every fault in order
struct ParseResult {
    Ast ast;
    std::vector<Diagnostic> diagnostics;
};

/**
 * Parses a whole token stream into a file tree.
 * @params text - the source the tokens span, tokens - the lexed stream ending in Eof
 * @returns {ParseResult} - the tree and the faults, the parse failed when faults is non empty
 */
ParseResult parse(std::string_view text, const std::vector<Token>& tokens);

/**
 * Parses a token stream into a tree that already holds other files, the
 * module loader's entry. The new nodes land after the seeded ones, so every
 * id handed out earlier stays valid.
 * @params text - the buffer the token spans index, tokens - the lexed stream ending in Eof,
 * seed - the tree to extend
 * @returns {ParseResult} - the extended tree and the faults of this file alone
 */
ParseResult parseInto(std::string_view text, const std::vector<Token>& tokens, Ast seed);

class Parser {
public:
    Parser(std::string_view text, const std::vector<Token>& tokens);
    Parser(std::string_view text, const std::vector<Token>& tokens, Ast seed);
    ParseResult run() &&;

private:
    // Token navigation and diagnostics, parser.cpp
    const Token& current() const;
    Token lookahead() const;
    bool at(TokenKind kind) const;
    bool nextIs(TokenKind kind) const;
    bool atEof() const;
    bool atEnd() const;
    bool atIdentText(std::string_view spelling) const;
    bool currentTextIs(std::string_view spelling) const;
    bool hasNewlineHere() const;
    bool isNewlineTerminator() const;
    std::int64_t previousHi() const;
    std::string_view tokenText(const Token& token) const;
    void bump();
    bool eat(TokenKind kind);
    bool expect(TokenKind kind, std::string_view what);
    std::string foundText() const;
    std::string expectedFound(std::string_view what) const;
    void error(std::string message);
    void errorSpan(std::int64_t lo, std::int64_t hi, std::string message);
    std::string ident();
    std::string memberName();
    void enterGroup();
    void leaveGroup();
    int saveBraceDepth();
    void restoreBraceDepth(int depth);
    bool enterNesting();
    void leaveNesting();
    bool isHalted() const;
    void guardProgress(std::size_t before, std::string_view context);
    void expectSeparator(std::string_view message);
    void expectClose(std::string_view eofMessage);
    void skipRestOfLine();
    TypeId placeholderType();
    ExprId placeholderExpr();
    BlockId emptyBlock();

    // The file and its declarations, parseitem.cpp
    void parseFile();
    void parseImport();
    void parseDecl();
    AttributeDecl parseAttribute(bool isInternal);
    void parseInternalDecl();
    RecordDecl parseRecord();
    SumDecl parseSum();
    Variant parseVariant();
    std::vector<Field> parseVariantFields();
    ProvisionalDecl parseProvisional(bool isInternal);
    void parseClauseSig(std::vector<ClauseSig>& sigs);
    ContractDecl parseContract(bool isInternal);
    void parseMember(std::vector<Member>& members);
    bool parseWorkflowMember(std::vector<Member>& members);
    IncorporateDecl parseIncorporate();
    VowDecl parseVow();

    // Pledges, clauses, subcontracts, schemas, attributes, parsemember.cpp
    PledgeDecl parsePledge();
    ClauseDecl parseClause();
    SubcontractDecl parseSubcontract();
    void parseSubMember(std::vector<SubMember>& members);
    SchemaDecl parseSchema();
    TypeId parseReturnType(std::string_view missingMessage);
    std::vector<Attr> parseAttrsOnLine();
    std::vector<Attr> parseAttrs();
    void parseAttr(std::vector<Attr>& attrs);
    std::optional<BlockId> parseBodyOnLine();

    // The requirements block, parsereq.cpp
    RequirementsDecl parseRequirements();
    void parseRequirementsLine(RequirementsDecl& block);
    void storeRequirementsLine(std::optional<ReqId>& slot, std::string_view label);
    ReqId parseReqLine();
    ReqId parseReqOr();
    ReqId parseReqOrInner();
    ReqId parseReqAnd();
    ReqId parseReqNot();
    ReqId parseReqAtom();
    ReqId combineReq(ReqId lhs, ReqId rhs, bool isAnd);

    // Terms, remedies, signals, the course, and amends, parseworkflow.cpp
    std::vector<Term> parseTerms();
    Term parseWithin();
    Term parseAfter();
    Term parseRetry();
    void parseRetryOptions(RetryTerm& retry);
    std::vector<std::string> parseNameList(std::string_view what);
    std::int64_t expectDuration(std::string_view what);
    void skipStrayValue();
    RemedyDecl parseRemedy();
    std::string parseRemedyTarget();
    std::optional<Field> parseRemedyParam();
    SignalDecl parseSignal();
    CourseDecl parseCourse();
    AmendsDecl parseAmends();
    void parseAmendItem(std::vector<AmendItem>& items);
    AmendItem parseAmendVow();
    AmendItem parseAmendRename();
    AmendItem parseAmendLatch();

    // Types and parameter lists, parsetype.cpp
    TypeId parseType();
    TypeId parseTypeInner();
    std::vector<TypeId> parseTypeArgs();
    TypeId parsePledgeType();
    std::vector<TypeId> parseTypeList();
    std::vector<Field> parseParams();
    Field parseField(std::string_view colonWhat);

    // Expressions, parseexpr.cpp
    ExprId parseExpr();
    ExprId parseExprNoRecord();
    ExprId parseExprWithRecords();
    ExprId parseBinary(int minPower);
    ExprId parseBinaryInner(int minPower);
    ExprId parseUnary();
    ExprId parseUnaryInner();
    ExprId parsePostfix();
    ExprId parseIndexSuffix(ExprId base);
    std::vector<Argument> parseCallArgs();
    Argument parseArg();
    std::optional<ExprId> parseLiteralExpr();
    ExprId parsePrimary();
    ExprId spanExpr(std::int64_t lo, std::int64_t hi, ExprKind kind);

    // Primaries that open a nested form, parseprimary.cpp
    ExprId parseMutExpr();
    ExprId parseParen(std::int64_t lo);
    ExprId parseList(std::int64_t lo);
    ExprId parseIdentPrimary(const Token& token);
    ExprId parseWrapCtor(const Token& token, WrapKind kind);
    ExprId parseMapNew(const Token& token);
    ExprId parseGenericNew(const Token& token);
    ExprId parseRecordLiteral(std::string name, std::int64_t lo);
    ExprId parseAwait();
    std::vector<ExprId> parseExprList(TokenKind close);

    // Match, select, and patterns, parsematch.cpp
    ExprId parseMatch();
    ArmBody parseArmBody();
    ExprId parseSelect();
    void parseSelectArm(std::vector<SelectArm>& arms);
    PatternId parsePattern();
    PatternId parsePatternInner();
    PatternId parseNegativePattern();
    PatternId parseIdentPattern(const Token& token);
    PatternId parseVariantPattern();

    // Blocks and statements, parsestmt.cpp
    BlockId parseBlock();
    StmtId parseStmt();
    StmtId parseSimpleStmt();
    StmtId parseLet();
    StmtId parseReturn();
    StmtId parseIf();
    std::optional<BlockId> parseElse();
    StmtId parseWhile();
    StmtId parseFor();
    StmtId parseSleep();
    StmtId parseTogether();

    std::string_view text_;
    const std::vector<Token>& tokens_;
    std::size_t pos_ = 0;
    Ast ast_;
    std::vector<Diagnostic> diagnostics_;
    int groupDepth_ = 0;
    int nestingDepth_ = 0;
    bool hasHitDepthLimit_ = false;
    bool isRecordBanned_ = false;
};

}  // namespace geas
