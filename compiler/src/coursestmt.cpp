// Course statements and explicit loop control
#include "coursebuilder.hpp"

#include "overloaded.hpp"

namespace geas {

void CourseBuilder::lowerBlock(BlockId block) {
    scopes_.emplace_back();
    for (const StmtId statement : source_.block(block).stmts) {
        lowerStatement(statement);
    }
    scopes_.pop_back();
}

void CourseBuilder::lowerLet(const LetStmt& binding) {
    const ExprId value = lowerExpression(binding.value);
    const CourseSlotId slot = bindLocal(binding.name, typeOf(binding.value), binding.isMutable);
    emit(CourseStore{slot, value});
}

void CourseBuilder::lowerStatement(StmtId statement) {
    std::visit(Overloaded{
        [&](const LetStmt& binding) { lowerLet(binding); },
        [&](const AssignStmt& assignment) {
            const ExprId target = lowerTarget(assignment.target);
            emit(CourseCheckTarget{target, true});
            const ExprId value = lowerExpression(assignment.value);
            emit(CourseAssign{target, value});
        },
        [&](const ExprStmt& expression) {
            const ExprId value = lowerExpression(expression.expr);
            emit(CourseEvaluate{value});
        },
        [&](const IfStmt& branch) { lowerIf(branch); },
        [&](const WhileStmt& loop) { lowerWhile(loop); },
        [&](const ForStmt& loop) { lowerFor(loop); },
        [&](const SleepStmt& sleep) { lowerSleep(sleep); },
        [&](const TogetherStmt& together) { lowerTogether(together); },
        [&](const ReturnStmt&) { emit(CourseReturn{}); },
        [&](const BreakStmt&) { emit(CourseJump{loops_.back().second}); },
        [&](const ContinueStmt&) { emit(CourseJump{loops_.back().first}); },
    }, source_.stmt(statement));
}

void CourseBuilder::lowerIf(const IfStmt& statement) {
    const ExprId condition = lowerExpression(statement.condition);
    const CourseLabelId thenLabel = newLabel();
    const CourseLabelId elseLabel = newLabel();
    const CourseLabelId done = newLabel();
    emit(CourseBranch{condition, thenLabel, elseLabel});
    emit(CourseLabel{thenLabel});
    lowerBlock(statement.thenBlock);
    emit(CourseJump{done});
    emit(CourseLabel{elseLabel});
    if (statement.elseBlock) {
        lowerBlock(*statement.elseBlock);
    }
    emit(CourseLabel{done});
}

// The condition and its hoisted operands run at every loop entry and continue
void CourseBuilder::lowerWhile(const WhileStmt& loop) {
    const CourseLabelId head = newLabel();
    const CourseLabelId body = newLabel();
    const CourseLabelId done = newLabel();
    emit(CourseLabel{head});
    ExprId condition = lowerExpression(loop.condition);
    if (needsStatements(loop.condition)) {
        condition = capture(condition);
    }
    emit(CourseBranch{condition, body, done});
    emit(CourseLabel{body});
    loops_.emplace_back(head, done);
    lowerBlock(loop.body);
    loops_.pop_back();
    emit(CourseJump{head});
    emit(CourseLabel{done});
}

void CourseBuilder::lowerFor(const ForStmt& loop) {
    const ExprId iterable = lowerExpression(loop.iterable);
    const TyId collectionType = typeOf(loop.iterable);
    const TyId elementType = checked_.types.node(collectionType).args.front();
    const TyId positionType = checked_.types.scalar(TyKind::Int);
    const CourseSlotId collection = addSlot(collectionType, CourseSlotKind::Collection);
    const CourseSlotId position = addSlot(positionType, CourseSlotKind::Position);
    emit(CourseStore{collection, iterable});
    emit(CourseStore{position, append(IntLiteral{0}, positionType)});
    scopes_.emplace_back();
    const CourseSlotId variable = bindLocal(loop.variable, elementType);
    const CourseLabelId head = newLabel();
    const CourseLabelId body = newLabel();
    const CourseLabelId done = newLabel();
    emit(CourseLabel{head});
    emit(CourseForNext{collection, position, variable, body, done});
    emit(CourseLabel{body});
    loops_.emplace_back(head, done);
    lowerBlock(loop.body);
    loops_.pop_back();
    scopes_.pop_back();
    emit(CourseJump{head});
    emit(CourseLabel{done});
}

void CourseBuilder::lowerSleep(const SleepStmt& sleep) {
    const std::uint32_t number = newStep(CourseStepKind::Sleep);
    const ExprId duration = capture(lowerExpression(sleep.duration));
    step(number).arguments.push_back(duration);
    emit(CourseResume{number});
}

// All arguments are captured before polling the group, then result bindings
// and question operators run in statement order after the whole group finishes
void CourseBuilder::lowerTogether(const TogetherStmt& together) {
    const std::uint32_t number = newStep(CourseStepKind::Together);
    std::vector<CourseInstruction> completions;
    for (const StmtId statement : source_.block(together.body).stmts) {
        const Stmt& node = source_.stmt(statement);
        const auto* binding = std::get_if<LetStmt>(&node);
        const ExprId value = binding != nullptr ? binding->value : std::get<ExprStmt>(node).expr;
        const auto* question = std::get_if<QuestionExpr>(&source_.expr(value).kind);
        const ExprId called = question != nullptr ? question->operand : value;
        ExprKind kind = source_.expr(called).kind;
        lowerOperands(kind, true);
        const auto& call = std::get<CallExpr>(kind);
        const auto& name = std::get<Identifier>(course_.ast.expr(call.callee).kind).name;
        const CourseSlotId result = addSlot(typeOf(called), CourseSlotKind::Temporary);
        CourseCall operation{memberIndex(name, false), {}, result, question != nullptr};
        for (const Argument& argument : call.args) {
            operation.arguments.push_back(argument.value);
        }
        step(number).calls.push_back(std::move(operation));
        ExprId completed = slotValue(result);
        if (question != nullptr) {
            completed = copyExpression(value, QuestionExpr{completed});
        }
        if (binding != nullptr) {
            const CourseSlotId slot = bindLocal(binding->name, typeOf(value), binding->isMutable);
            completions.push_back(CourseStore{slot, completed});
        } else {
            completions.push_back(CourseEvaluate{completed});
        }
    }
    emit(CourseResume{number});
    for (CourseInstruction& completion : completions) {
        emit(std::move(completion));
    }
}

void CourseBuilder::bindPattern(PatternId pattern, std::vector<CoursePatternSlot>& bindings) {
    const Pattern& node = source_.pattern(pattern);
    const TyId type = checked_.tables.patternTypes[static_cast<std::size_t>(pattern.index)];
    if (const auto* binding = std::get_if<BindPattern>(&node)) {
        const VariantSig* variant = checked_.sigs.variant(binding->name);
        if (variant != nullptr && checked_.types.nameOf(type) == variant->owner) {
            return;
        }
        bindings.push_back(CoursePatternSlot{pattern, bindLocal(binding->name, type)});
        return;
    }
    if (const auto* wrap = std::get_if<WrapPattern>(&node)) {
        bindPattern(wrap->inner, bindings);
        return;
    }
    if (const auto* variant = std::get_if<VariantPattern>(&node)) {
        for (const PatternId field : variant->fields) {
            bindPattern(field, bindings);
        }
    }
}

void CourseBuilder::lowerArm(const ArmBody& body, CourseSlotId result) {
    if (const auto* expression = std::get_if<ExprId>(&body)) {
        const ExprId value = lowerExpression(*expression);
        emit(CourseStore{result, value});
        return;
    }
    const auto& statements = source_.block(std::get<BlockId>(body)).stmts;
    scopes_.emplace_back();
    bool hasValue = false;
    for (std::size_t i = 0; i < statements.size(); ++i) {
        const auto* last = std::get_if<ExprStmt>(&source_.stmt(statements[i]));
        if (i + 1 == statements.size() && last != nullptr) {
            const ExprId value = lowerExpression(last->expr);
            emit(CourseStore{result, value});
            hasValue = true;
        } else {
            lowerStatement(statements[i]);
        }
    }
    if (!hasValue) {
        emit(CourseStore{result, std::nullopt});
    }
    scopes_.pop_back();
}

}  // namespace geas
