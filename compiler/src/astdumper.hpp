// The parse dump writer, internal to the astdump*.cpp files. The dump is the
// byte stable golden text tests/parse diffs: four space indentation, one
// field or element per line, trailing commas, and a leaf on a single line.
// Change a rule here and every golden regenerates. Each render function
// writes its node starting at the current column and ends without a comma;
// level is the indentation of the line the node opened on
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "ast.hpp"
#include "overloaded.hpp"

namespace geas {

/**
 * The dump name of a one operand constructor.
 * @params kind - the constructor
 * @returns {std::string_view} - Some, Ok, Err, or Mut
 */
std::string_view wrapKindName(WrapKind kind);

class AstDumper {
public:
    explicit AstDumper(const Ast& ast);
    std::string run() &&;

private:
    // Writing primitives, astdumper.cpp
    void write(std::string_view text);
    void indent(int level);
    void quoted(std::string_view text);
    void boolean(bool value);
    void openStruct(std::string_view name);
    void key(int level, std::string_view name);
    void endField();
    void closeStruct(int level);
    void openWrap(std::string_view name, int level);
    void closeWrap(int level);

    // A bracketed list, "[]" when empty, else one element a line
    template <typename Items, typename Render>
    void list(const Items& items, int level, Render render) {
        if (items.empty()) {
            write("[]");
            return;
        }
        write("[\n");
        for (const auto& item : items) {
            indent(level + 1);
            render(item, level + 1);
            endField();
        }
        indent(level);
        write("]");
    }

    // Declarations, astdump.cpp
    void renderImports();
    void renderDecl(const Decl& decl, int level);
    void renderFields(const std::vector<Field>& fields, int level);
    void renderField(const Field& field, int level);
    void renderAttrs(const std::vector<Attr>& attrs, int level);
    void renderRecord(const RecordDecl& record, int level);
    void renderSum(const SumDecl& sum, int level);
    void renderProvisional(const ProvisionalDecl& provisional, int level);
    void renderContract(const ContractDecl& contract, int level);
    void renderMember(const Member& member, int level);
    void renderVow(const VowDecl& vow, int level);
    void renderPledge(const PledgeDecl& pledge, int level);
    void renderClause(const ClauseDecl& clause, int level);
    void renderSubcontract(const SubcontractDecl& subcontract, int level);
    void renderSubMembers(const std::vector<SubMember>& members, int level);
    void renderSchema(const SchemaDecl& schema, int level);
    void renderRequirements(const RequirementsDecl& requirements, int level);
    void renderReqSlot(const std::optional<ReqId>& slot, int level);
    void renderReq(ReqId id, int level);
    void renderOptionalBlock(const std::optional<BlockId>& body, int level);

    // Workflow declarations, astdumpworkflow.cpp
    void renderTerms(const std::vector<Term>& terms, int level);
    void renderTerm(const Term& term, int level);
    void renderRetry(const RetryTerm& retry, int level);
    void renderNames(const std::vector<std::string>& names, int level);
    void renderDuration(std::int64_t milliseconds);
    void renderOptionalDuration(const std::optional<std::int64_t>& milliseconds);
    void renderRemedy(const RemedyDecl& remedy, int level);
    void renderSignal(const SignalDecl& signal, int level);
    void renderCourse(const CourseDecl& course, int level);
    void renderAmends(const AmendsDecl& amends, int level);
    void renderAmendItem(const AmendItem& item, int level);

    // Types and expressions, astdumpexpr.cpp
    void renderType(TypeId id, int level);
    void renderTypeList(const std::vector<TypeId>& types, int level);
    void renderExpr(ExprId id, int level);
    bool renderLeafExpr(const ExprKind& kind);
    void renderUnary(const UnaryExpr& unary, int level);
    void renderBinary(const BinaryExpr& binary, int level);
    void renderMember(const MemberExpr& member, int level);
    void renderWrapped(std::string_view name, ExprId inner, int level);
    void renderBracketed(std::string_view name, const std::vector<ExprId>& exprs, int level);
    void renderMapNew(const MapNewExpr& mapNew, int level);
    void renderExprList(const std::vector<ExprId>& exprs, int level);
    void renderArgs(const std::vector<Argument>& args, int level);
    void renderCall(const CallExpr& call, int level);
    void renderPair(std::string_view name, ExprId first, ExprId second, int level);
    void renderRecordLiteral(const RecordLiteral& record, int level);
    void renderGenericNew(const GenericNewExpr& generic, int level);
    void renderAwait(const AwaitExpr& await, int level);

    // Patterns, arms, blocks, and statements, astdumpstmt.cpp
    void renderPattern(PatternId id, int level);
    void renderVariantPattern(const VariantPattern& variant, int level);
    void renderArmBody(const ArmBody& body, int level);
    void renderMatch(const MatchExpr& match, int level);
    void renderSelect(const SelectExpr& select, int level);
    void renderSelectArm(const SelectArm& arm, int level);
    void renderBlock(BlockId id, int level);
    void renderStmt(StmtId id, int level);
    void renderReturn(const ReturnStmt& returnStmt, int level);
    void renderTogether(const TogetherStmt& together, int level);
    void renderLet(const LetStmt& let, int level);
    void renderIf(const IfStmt& ifStmt, int level);
    void renderWhile(const WhileStmt& whileStmt, int level);
    void renderFor(const ForStmt& forStmt, int level);

    const Ast& ast_;
    std::string out_;
};

}  // namespace geas
