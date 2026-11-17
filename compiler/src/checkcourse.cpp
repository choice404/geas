// The course, the contract's orchestration, and the forms only a course may
// hold. Inside it the contract's own pledges are called by bare name, each
// call a durable step. await waits for a signal and yields its payload,
// select takes the first of several awaits or a timeout, sleep pauses, and
// together runs its statements side by side. A course returns nothing, so a
// '?' on any Option or Result just ends it
#include "checker.hpp"

#include "overloaded.hpp"

namespace geas {

void Checker::checkCourse(const CourseDecl& course) {
    isInCourse_ = true;
    checkBody({}, {}, scalar(TyKind::Unit), course.body, course.lo, course.hi);
    isInCourse_ = false;
}

// What an await on the signal yields: Unit for no params, the type for one,
// a Tuple for several
TyId Checker::signalPayload(const SignalDecl& signal) {
    const std::vector<TyId>& params = signalSig(signal).params;
    if (params.empty()) {
        return scalar(TyKind::Unit);
    }
    if (params.size() == 1) {
        return params.front();
    }
    return types_.tuple(params);
}

// The signal an await names, null after reporting why it names none
const SignalDecl* Checker::awaitedSignal(const Expr& expr, const std::string& name) {
    if (name.empty()) {
        return nullptr;
    }
    if (const SignalDecl* signal = resolved_.findSignal(contract_, name)) {
        return signal;
    }
    if (resolved_.reachable(contract_, name) != nullptr) {
        emitAt(expr, quoted(name) + " is not a signal; await names a signal of this contract");
    } else {
        emitAt(expr, "unknown signal " + quoted(name) + " in await");
    }
    return nullptr;
}

// An await yields the payload of its signal, and under a deadline Option of it,
// None when the deadline passes first
TyId Checker::inferAwait(const Expr& expr, const AwaitExpr& await) {
    if (!isInCourse_) {
        emitAt(expr, "'await' appears outside a course");
        return unknown();
    }
    const SignalDecl* signal = awaitedSignal(expr, await.signal);
    if (signal == nullptr) {
        return unknown();
    }
    const TyId payload = signalPayload(*signal);
    return await.withinMilliseconds ? types_.option(payload) : payload;
}

// The arms of a select agree on one type the way match arms do. An await arm
// may bind its payload with an irrefutable pattern, a name or a wildcard,
// checked against the payload type
TyId Checker::checkSelect(const Expr& expr, const SelectExpr& select, TyId want) {
    if (!isInCourse_) {
        emitAt(expr, "'select' appears outside a course");
    }
    TyId first;
    for (const SelectArm& arm : select.arms) {
        const TyId armWant = want.isSet() ? want : first;
        const auto* awaited = std::get_if<AwaitArm>(&arm);
        pushScope();
        if (awaited != nullptr) {
            const SignalDecl* signal = awaitedSignal(expr, awaited->signal);
            // The payload is bound, never tested: a select has no next arm to fall to
            const bool isIrrefutable = !awaited->binding ||
                bindPattern(*awaited->binding,
                            signal == nullptr ? unknown() : signalPayload(*signal), expr);
            if (!isIrrefutable) {
                emitAt(expr, "the 'as' pattern of a select arm must be irrefutable, a name or "
                             "'_'; bind the payload and match it inside the arm");
            }
        }
        const ArmBody& body = awaited != nullptr ? awaited->body : std::get<TimeoutArm>(arm).body;
        const TyId actual = checkArmBody(body, armWant, expr, "select");
        popScope();
        if (!armWant.isSet()) {
            first = actual;
        }
    }
    if (want.isSet()) {
        return want;
    }
    return first.isSet() ? first : unknown();
}

void Checker::checkSleep(const SleepStmt& sleep) {
    ++suspensionCount_;
    const Expr& duration = ast_.expr(sleep.duration);
    if (!isInCourse_) {
        emitAt(duration, "'sleep' appears outside a course");
    }
    const TyId wanted = scalar(TyKind::Duration);
    const TyId actual = inferExpr(sleep.duration, wanted);
    if (!isCompatible(wanted, actual)) {
        emitAt(duration, "sleep needs a 'Duration', got " + quoted(types_.render(actual)));
    }
}

// The statements run side by side and the block ends when all of them have.
// They check in the enclosing scope, so a binding made inside is visible
// after the block
void Checker::checkTogether(const TogetherStmt& together) {
    ++suspensionCount_;
    if (!isInCourse_) {
        emitInBody("'together' appears outside a course");
    }
    for (const StmtId statement : ast_.block(together.body).stmts) {
        checkTogetherStatement(statement);
    }
    for (auto& [name, binding] : scopes_.back()) {
        binding.isPendingTogether = false;
    }
}

// The clock read runs once at this position and its value can live in a frame slot
TyId Checker::checkNow(const Expr& expr, const CallExpr& call) {
    if (!isInCourse_) {
        emitAt(expr, "now() is only available inside a course");
    } else if (!call.args.empty()) {
        emitAt(expr, "now takes no arguments");
    }
    markUse(expr, ExprUse::NowCall);
    return scalar(TyKind::Time);
}

bool Checker::hasContractType(TyId type) const {
    if (!type.isSet()) {
        return false;
    }
    const TyNode& node = types_.node(type);
    if (node.kind == TyKind::Contract || hasContractType(node.ret)) {
        return true;
    }
    for (const TyId argument : node.args) {
        if (hasContractType(argument)) {
            return true;
        }
    }
    return false;
}

namespace {

std::string togetherExpressionName(const Ast& ast, ExprId expression) {
    return std::visit(Overloaded{
        [&](const CallExpr& call) {
            const auto* name = std::get_if<Identifier>(&ast.expr(call.callee).kind);
            return name == nullptr ? std::string{"a member or value call"} :
                "a call to " + quoted(name->name);
        },
        [](const AwaitExpr&) { return std::string{"an await"}; },
        [](const SelectExpr&) { return std::string{"a select"}; },
        [](const MatchExpr&) { return std::string{"a match"}; },
        [](const BinaryExpr&) { return std::string{"a binary expression"}; },
        [](const UnaryExpr&) { return std::string{"a unary expression"}; },
        [](const QuestionExpr&) { return std::string{"a '?' expression"}; },
        [](const Identifier&) { return std::string{"a name"}; },
        [](const auto&) { return std::string{"a literal or constructor"}; },
    }, ast.expr(expression).kind);
}

std::string togetherStatementName(const Stmt& statement) {
    return std::visit(Overloaded{
        [](const LetStmt&) { return "a let initializer"; },
        [](const AssignStmt&) { return "an assignment"; },
        [](const ReturnStmt&) { return "a return"; },
        [](const IfStmt&) { return "an if statement"; },
        [](const WhileStmt&) { return "a while loop"; },
        [](const ForStmt&) { return "a for loop"; },
        [](const BreakStmt&) { return "a break"; },
        [](const ContinueStmt&) { return "a continue"; },
        [](const ExprStmt&) { return "an expression"; },
        [](const SleepStmt&) { return "a sleep"; },
        [](const TogetherStmt&) { return "a together block"; },
    }, statement);
}

}  // namespace

void Checker::checkTogetherStatement(StmtId statement) {
    const std::size_t beforeStatement = suspensionCount_;
    checkStmt(statement);
    const Stmt& node = ast_.stmt(statement);
    ExprId value;
    if (const auto* binding = std::get_if<LetStmt>(&node)) {
        value = binding->value;
    } else if (const auto* expression = std::get_if<ExprStmt>(&node)) {
        value = expression->expr;
    }
    if (value.index >= 0) {
        if (const auto* question = std::get_if<QuestionExpr>(&ast_.expr(value).kind)) {
            value = question->operand;
        }
        if (tables_.exprUses[static_cast<std::size_t>(value.index)] == ExprUse::CoursePledgeCall) {
            if (suspensionCount_ != beforeStatement + 1) {
                emitAt(ast_.expr(value), "a together call argument cannot contain a suspending "
                                         "operation; bind it with 'let' first");
            }
            if (const auto* binding = std::get_if<LetStmt>(&node)) {
                scopes_.back().at(binding->name).isPendingTogether = true;
            }
            return;
        }
    }
    std::string found = togetherStatementName(node);
    if (value.index >= 0) {
        found += " (" + togetherExpressionName(ast_, value) + ")";
    }
    emitInBody("together holds only single calls of this contract's own pledges; found " + found);
}

}  // namespace geas
