// The course, the contract's orchestration, and the forms only a course may
// hold. Inside it the contract's own pledges are called by bare name, each
// call a durable step. await waits for a signal and yields its payload,
// select takes the first of several awaits or a timeout, sleep pauses, and
// together runs its statements side by side. A course returns nothing, so a
// '?' on any Option or Result just ends it
#include "checker.hpp"

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
// may bind its payload with a pattern, checked against the payload type
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
            if (awaited->binding) {
                bindPattern(*awaited->binding,
                            signal == nullptr ? unknown() : signalPayload(*signal), expr);
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
    if (!isInCourse_) {
        emitInBody("'together' appears outside a course");
    }
    for (const StmtId statement : ast_.block(together.body).stmts) {
        checkStmt(statement);
    }
}

// The clock read, now(). The engine journals it on first run and replays it
// on resume, which only a course can do. The arguments were already walked
TyId Checker::checkNow(const Expr& expr, const CallExpr& call) {
    if (!isInCourse_) {
        emitAt(expr, "now() is only available inside a course");
    } else if (!call.args.empty()) {
        emitAt(expr, "now takes no arguments");
    }
    markUse(expr, ExprUse::NowCall);
    return scalar(TyKind::Time);
}

}  // namespace geas
