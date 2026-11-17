// Internal state shared by the course lowering passes
#pragma once

#include <unordered_map>
#include <utility>

#include "check.hpp"
#include "course.hpp"

namespace geas {

// The index a member the course names but the contract lacks resolves to,
// always alongside a recorded build error
inline constexpr std::uint32_t kMissingMember = 0xffffffffu;

class CourseBuilder {
public:
    CourseBuilder(CheckedProgram& checked, const ContractShape& contract);
    std::expected<LoweredCourse, std::string> run();

private:
    // Slots, expression arenas, and numbered control points
    TyId typeOf(ExprId expression) const;
    ExprUse useOf(ExprId expression) const;
    ExprId append(ExprKind kind, TyId type, ExprUse use = ExprUse::None);
    ExprId copyExpression(ExprId original, ExprKind kind);
    ExprId slotValue(CourseSlotId slot);
    CourseSlotId addSlot(TyId type, CourseSlotKind kind, std::string name = "",
                         bool isMutable = false);
    CourseSlotId bindLocal(const std::string& name, TyId type, bool isMutable = false);
    CourseSlotId lookup(const std::string& name) const;
    ExprId capture(ExprId expression);
    bool hasContractType(TyId type) const;
    CourseLabelId newLabel();
    std::uint32_t newStep(CourseStepKind kind, bool hasQuestion = false);
    CourseStep& step(std::uint32_t number);
    void emit(CourseInstruction instruction);
    std::uint32_t memberIndex(const std::string& name, bool isSignal);
    TyId signalPayload(std::uint32_t signal);
    std::uint64_t hashCourse() const;

    // Expressions and ordered operands
    bool needsStatements(ExprId expression) const;
    std::vector<ExprId*> operands(ExprKind& kind) const;
    void lowerOperands(ExprKind& kind, bool isPersistent = false);
    ExprId lowerExpression(ExprId expression, bool hasQuestion = false);
    ExprId lowerOrdinary(ExprId expression);
    ExprId lowerOperation(ExprId expression, bool hasQuestion);
    ExprId lowerLogical(ExprId expression, const BinaryExpr& binary);
    ExprId lowerTarget(ExprId expression);

    // Blocks, branches, and loops
    void lowerBlock(BlockId block);
    void lowerStatement(StmtId statement);
    void lowerLet(const LetStmt& binding);
    void lowerIf(const IfStmt& statement);
    void lowerWhile(const WhileStmt& loop);
    void lowerFor(const ForStmt& loop);
    void lowerSleep(const SleepStmt& sleep);
    void lowerTogether(const TogetherStmt& together);

    // Arm bodies and pattern bindings
    void bindPattern(PatternId pattern, std::vector<CoursePatternSlot>& bindings);
    void lowerArm(const ArmBody& body, CourseSlotId result);
    ExprId lowerMatch(ExprId expression, const MatchExpr& match);
    ExprId lowerSelect(ExprId expression, const SelectExpr& select, bool hasQuestion);
    CourseWaitArm waitArm(const SelectArm& arm);

    CheckedProgram& checked_;
    const Ast& source_;
    const ContractShape& contract_;
    LoweredCourse course_;
    std::vector<std::unordered_map<std::string, CourseSlotId>> scopes_;
    std::vector<std::pair<CourseLabelId, CourseLabelId>> loops_;
    std::int32_t labelCount_ = 0;
    std::string error_;
};

}  // namespace geas
