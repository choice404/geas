// Ordered expression hoisting and durable operation operands
#include "coursebuilder.hpp"

#include "overloaded.hpp"

namespace geas {

// Only eager operands belong here; match arms and logical right sides have
// their own control flow, and a call's callee precedes its arguments
std::vector<ExprId*> CourseBuilder::operands(ExprKind& kind) const {
    std::vector<ExprId*> result;
    const auto addArguments = [&](std::vector<Argument>& arguments) {
        for (Argument& argument : arguments) {
            result.push_back(&argument.value);
        }
    };
    std::visit(Overloaded{
        [&](UnaryExpr& node) { result.push_back(&node.operand); },
        [&](BinaryExpr& node) { result = {&node.lhs, &node.rhs}; },
        [&](CallExpr& node) { result.push_back(&node.callee); addArguments(node.args); },
        [&](IndexExpr& node) { result = {&node.base, &node.index}; },
        [&](MemberExpr& node) { result.push_back(&node.base); },
        [&](QuestionExpr& node) { result.push_back(&node.operand); },
        [&](WrapExpr& node) { result.push_back(&node.inner); },
        [&](TupleExpr& node) {
            for (ExprId& element : node.elements) { result.push_back(&element); }
        },
        [&](ListExpr& node) {
            for (ExprId& element : node.elements) { result.push_back(&element); }
        },
        [&](RecordLiteral& node) {
            for (FieldInit& field : node.fields) { result.push_back(&field.value); }
        },
        [&](GenericNewExpr& node) { addArguments(node.args); },
        [](auto&) {},
    }, kind);
    return result;
}

bool CourseBuilder::needsStatements(ExprId expression) const {
    ExprKind kind = source_.expr(expression).kind;
    if (useOf(expression) == ExprUse::CoursePledgeCall ||
        std::holds_alternative<AwaitExpr>(kind) || std::holds_alternative<SelectExpr>(kind) ||
        std::holds_alternative<MatchExpr>(kind)) {
        return true;
    }
    for (const ExprId* operand : operands(kind)) {
        if (needsStatements(*operand)) {
            return true;
        }
    }
    return false;
}

// Snapshot earlier values before a later operand emits instructions, even
// for a local read whose binding a later match arm might change
void CourseBuilder::lowerOperands(ExprKind& kind, bool isPersistent) {
    const std::vector<ExprId*> children = operands(kind);
    std::vector<bool> hasLaterStatements(children.size(), false);
    bool hasStatements = false;
    for (std::size_t i = children.size(); i > 0; --i) {
        hasLaterStatements[i - 1] = hasStatements;
        hasStatements = hasStatements || needsStatements(*children[i - 1]);
    }
    for (std::size_t i = 0; i < children.size(); ++i) {
        ExprId value = lowerExpression(*children[i]);
        if (isPersistent || hasLaterStatements[i]) {
            value = capture(value);
        }
        *children[i] = value;
    }
}

ExprId CourseBuilder::lowerExpression(ExprId expression, bool hasQuestion) {
    const ExprKind& kind = source_.expr(expression).kind;
    if (const auto* identifier = std::get_if<Identifier>(&kind)) {
        if (useOf(expression) == ExprUse::Local) {
            return slotValue(lookup(identifier->name));
        }
    }
    if (const auto* question = std::get_if<QuestionExpr>(&kind)) {
        const ExprId value = lowerExpression(question->operand, true);
        return copyExpression(expression, QuestionExpr{value});
    }
    if (const auto* match = std::get_if<MatchExpr>(&kind)) {
        return lowerMatch(expression, *match);
    }
    if (const auto* select = std::get_if<SelectExpr>(&kind)) {
        return lowerSelect(expression, *select, hasQuestion);
    }
    if (useOf(expression) == ExprUse::CoursePledgeCall ||
        std::holds_alternative<AwaitExpr>(kind)) {
        return lowerOperation(expression, hasQuestion);
    }
    return lowerOrdinary(expression);
}

ExprId CourseBuilder::lowerOrdinary(ExprId expression) {
    ExprKind kind = source_.expr(expression).kind;
    if (const auto* binary = std::get_if<BinaryExpr>(&kind)) {
        const bool isLogical = binary->op == BinaryOp::And || binary->op == BinaryOp::Or;
        if (isLogical && needsStatements(binary->rhs)) {
            return lowerLogical(expression, *binary);
        }
    }
    lowerOperands(kind);
    return copyExpression(expression, std::move(kind));
}

ExprId CourseBuilder::lowerOperation(ExprId expression, bool hasQuestion) {
    ExprKind kind = source_.expr(expression).kind;
    const bool isAwait = std::holds_alternative<AwaitExpr>(kind);
    const std::uint32_t number = newStep(isAwait ? CourseStepKind::Await : CourseStepKind::Call,
                                         hasQuestion);
    lowerOperands(kind, true);
    const CourseSlotId result = addSlot(typeOf(expression), CourseSlotKind::Temporary);
    CourseStep& operation = step(number);
    operation.result = result;
    if (const auto* await = std::get_if<AwaitExpr>(&kind)) {
        operation.member = memberIndex(await->signal, true);
        operation.withinMilliseconds = await->withinMilliseconds;
    } else {
        const auto& call = std::get<CallExpr>(kind);
        const auto& callee = std::get<Identifier>(course_.ast.expr(call.callee).kind);
        operation.member = memberIndex(callee.name, false);
        for (const Argument& argument : call.args) {
            operation.arguments.push_back(argument.value);
        }
    }
    emit(CourseResume{number});
    return slotValue(result);
}

ExprId CourseBuilder::lowerLogical(ExprId expression, const BinaryExpr& binary) {
    const ExprId left = lowerExpression(binary.lhs);
    const CourseSlotId result = addSlot(typeOf(expression), CourseSlotKind::Temporary);
    const CourseLabelId rightLabel = newLabel();
    const CourseLabelId shortLabel = newLabel();
    const CourseLabelId done = newLabel();
    const bool isAnd = binary.op == BinaryOp::And;
    emit(CourseBranch{left, isAnd ? rightLabel : shortLabel, isAnd ? shortLabel : rightLabel});
    emit(CourseLabel{shortLabel});
    const ExprId shortValue = append(BoolLiteral{!isAnd}, typeOf(expression));
    emit(CourseStore{result, shortValue});
    emit(CourseJump{done});
    emit(CourseLabel{rightLabel});
    const ExprId right = lowerExpression(binary.rhs);
    emit(CourseStore{result, right});
    emit(CourseLabel{done});
    return slotValue(result);
}

// Preserve the mutable root and save only the evaluated index operands
ExprId CourseBuilder::lowerTarget(ExprId expression) {
    ExprKind kind = source_.expr(expression).kind;
    if (auto* index = std::get_if<IndexExpr>(&kind)) {
        index->base = lowerTarget(index->base);
        emit(CourseCheckTarget{index->base, false});
        index->index = capture(lowerExpression(index->index));
    } else if (auto* member = std::get_if<MemberExpr>(&kind)) {
        member->base = lowerTarget(member->base);
    } else {
        return lowerExpression(expression);
    }
    return copyExpression(expression, std::move(kind));
}

ExprId CourseBuilder::lowerMatch(ExprId expression, const MatchExpr& match) {
    ExprId value = lowerExpression(match.scrutinee);
    if (needsStatements(match.scrutinee)) {
        value = capture(value);
    }
    const CourseSlotId result = addSlot(typeOf(expression), CourseSlotKind::Temporary);
    const CourseLabelId done = newLabel();
    CourseMatch dispatch{value, {}};
    const std::size_t dispatchPosition = course_.instructions.size();
    emit(dispatch);
    for (const MatchArm& arm : match.arms) {
        const CourseLabelId target = newLabel();
        CourseMatchArm lowered{arm.pattern, target, {}};
        scopes_.emplace_back();
        bindPattern(arm.pattern, lowered.bindings);
        emit(CourseLabel{target});
        lowerArm(arm.body, result);
        scopes_.pop_back();
        emit(CourseJump{done});
        dispatch.arms.push_back(std::move(lowered));
    }
    course_.instructions[dispatchPosition] = std::move(dispatch);
    emit(CourseLabel{done});
    return slotValue(result);
}

CourseWaitArm CourseBuilder::waitArm(const SelectArm& arm) {
    if (const auto* timeout = std::get_if<TimeoutArm>(&arm)) {
        return CourseWaitArm{true, 0, timeout->milliseconds, {}};
    }
    const auto& awaited = std::get<AwaitArm>(arm);
    const std::uint32_t signal = memberIndex(awaited.signal, true);
    const CourseSlotId payload = addSlot(signalPayload(signal), CourseSlotKind::Payload);
    return CourseWaitArm{false, signal, 0, payload};
}

ExprId CourseBuilder::lowerSelect(ExprId expression, const SelectExpr& select, bool hasQuestion) {
    const std::uint32_t number = newStep(CourseStepKind::Select, hasQuestion);
    const CourseSlotId choice = addSlot(checked_.types.scalar(TyKind::Int), CourseSlotKind::Choice);
    step(number).result = choice;
    for (const SelectArm& arm : select.arms) {
        step(number).arms.push_back(waitArm(arm));
    }
    const CourseSlotId result = addSlot(typeOf(expression), CourseSlotKind::Temporary);
    const CourseLabelId done = newLabel();
    emit(CourseResume{number});
    const std::size_t dispatchPosition = course_.instructions.size();
    CourseSelect dispatch{number, {}};
    emit(dispatch);
    for (const SelectArm& arm : select.arms) {
        const auto* awaited = std::get_if<AwaitArm>(&arm);
        CourseSelectArm lowered{awaited == nullptr ? std::nullopt : awaited->binding,
                                 newLabel(), {}};
        scopes_.emplace_back();
        if (lowered.pattern) {
            bindPattern(*lowered.pattern, lowered.bindings);
        }
        emit(CourseLabel{lowered.target});
        lowerArm(awaited == nullptr ? std::get<TimeoutArm>(arm).body : awaited->body, result);
        scopes_.pop_back();
        emit(CourseJump{done});
        dispatch.arms.push_back(std::move(lowered));
    }
    course_.instructions[dispatchPosition] = std::move(dispatch);
    emit(CourseLabel{done});
    return slotValue(result);
}

}  // namespace geas
