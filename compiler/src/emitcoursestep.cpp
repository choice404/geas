// The suspending operations of a course. Each one is a numbered step: the
// step number is stored in the frame and checkpointed first, then the resume
// label the dispatch switch jumps to, then the operation itself, polled.
// Its argument and arm arrays are rebuilt from slots after the label, since
// a re-entry jumps straight to it. A status that is not GEAS_OK records the
// status and stops the course without touching a slot, a poll that is not
// ready suspends it, and a ready operation lands its value in the step's
// result slot
#include "emitc.hpp"
#include "emittext.hpp"

namespace geas {

namespace {

std::string countOf(std::size_t count) {
    return decimal(static_cast<std::int64_t>(count));
}

std::string stepLabel(std::uint32_t step) {
    return joined("step_", decimal(static_cast<std::int64_t>(step)));
}

}  // namespace

void CEmitter::genCourseResume(const CourseResume& resume) {
    const CourseStep& step = course_->steps[static_cast<std::size_t>(resume.step - 1)];
    line(joined("f->step = ", decimal(step.number), ";"));
    openCourseBlock();
    const std::string status = freshName();
    line(joined("GeasStatus ", status, " = geas_course_checkpoint(ctx, f);"));
    returnUnlessOk(status);
    closeCourseBlock();
    courseLabel(stepLabel(step.number));
    openCourseBlock();
    switch (step.kind) {
        case CourseStepKind::Call: genCourseCall(step); break;
        case CourseStepKind::Await: genCourseAwait(step); break;
        case CourseStepKind::Sleep: genCourseSleep(step); break;
        case CourseStepKind::Select: genCourseSelect(step); break;
        case CourseStepKind::Together: genCourseTogether(step); break;
    }
    closeCourseBlock();
}

// Whether a lowered index names a row of the descriptor's pledges table of
// the expected kind: a pledge before every signal, a signal after them all
bool CEmitter::checkMember(std::uint32_t member, bool isSignal, std::string_view what) {
    const std::size_t pledges = shape_->pledges.size();
    const std::size_t rows = pledges + shape_->signals.size();
    const bool isInRange = isSignal ? member >= pledges && member < rows : member < pledges;
    if (!isInRange) {
        lookup_.fault(spanLo_, spanHi_,
                      joined("codegen: a course ", what, " names no ",
                             isSignal ? "signal" : "pledge", " of the pledges table"));
    }
    return isInRange;
}

// The argument array of one pledge call, each argument a slot or a constant
// typed by the pledge's parameter, NULL for a pledge that takes none
std::string CEmitter::genCourseArgs(std::uint32_t pledge, const std::vector<ExprId>& arguments) {
    const CallableSig sig = lookup_.pledgeSig(*shape_->pledges[pledge].decl);
    std::vector<CValue> values;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        values.push_back(genExpr(arguments[i], i < sig.params.size() ? sig.params[i] : TyId{}));
    }
    return genValueArray(values);
}

// The ready flag a polled operation answers through, waiting until told
std::string CEmitter::genReadyFlag() {
    std::string ready = freshName();
    line(joined("uint32_t ", ready, " = GEAS_COURSE_WAIT;"));
    return ready;
}

void CEmitter::suspendUnlessReady(std::string_view ready) {
    line(joined("if (", ready, " != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;"));
}

void CEmitter::genCourseCall(const CourseStep& step) {
    const std::uint32_t pledge = step.member.value_or(0);
    if (!checkMember(pledge, false, "call")) {
        return;
    }
    line(joined("/* call ", shape_->pledges[pledge].decl->name, " */"));
    const std::string array = genCourseArgs(pledge, step.arguments);
    const std::string result = newValue();
    const std::string status = freshName();
    line(joined("GeasStatus ", status, " = geas_course_call(ctx, f, ", decimal(pledge), ", ", array,
                ", ", countOf(step.arguments.size()), ", &", result, ");"));
    returnUnlessOk(status);
    line(joined(slotText(step.result), " = ", result, ";"));
}

// await without within yields the raw payload, with within an Option of it
void CEmitter::genCourseAwait(const CourseStep& step) {
    const std::uint32_t signal = step.member.value_or(0);
    if (!checkMember(signal, true, "await")) {
        return;
    }
    const SignalDecl& decl = *shape_->signals[signal - shape_->pledges.size()].decl;
    line(joined("/* await ", decl.name, " */"));
    const std::string ready = genReadyFlag();
    const std::string result = newValue();
    const std::string status = freshName();
    line(joined("GeasStatus ", status, " = geas_course_await(ctx, f, ", decimal(signal), ", ",
                step.withinMilliseconds ? "1" : "0", ", ",
                decimal(step.withinMilliseconds.value_or(0)), ", &", ready, ", &", result, ");"));
    returnUnlessOk(status);
    suspendUnlessReady(ready);
    line(joined(slotText(step.result), " = ", result, ";"));
}

void CEmitter::genCourseSleep(const CourseStep& step) {
    if (step.arguments.empty()) {
        lookup_.fault(spanLo_, spanHi_, "codegen: a course sleep carries no duration");
        return;
    }
    const CValue duration = genExpr(step.arguments.front(), types().scalar(TyKind::Duration));
    const std::string ready = genReadyFlag();
    const std::string status = freshName();
    line(joined("GeasStatus ", status, " = geas_course_sleep(ctx, f, ", duration.name, ".as.i, &",
                ready, ");"));
    returnUnlessOk(status);
    suspendUnlessReady(ready);
}

// The arm table of a select in source order, since the runtime takes the
// first ready arm in arm order. Empty after a fault
std::string CEmitter::genCourseArmTable(const CourseStep& step) {
    std::string arms;
    for (const CourseWaitArm& arm : step.arms) {
        if (!arm.isTimeout && !checkMember(arm.signal, true, "select arm")) {
            return "";
        }
        arms.append(arms.empty() ? "" : ", ");
        arms.append(arm.isTimeout ? joined("{ 1, 0, ", decimal(arm.milliseconds), " }")
                                  : joined("{ 0, ", decimal(arm.signal), ", 0 }"));
    }
    std::string table = freshName();
    line(joined("const GeasCourseArm ", table, "[", countOf(step.arms.size()), "] = { ", arms,
                " };"));
    return table;
}

// The step's result slot holds the chosen arm's index, and an await arm's
// payload lands in that arm's payload slot before dispatch
void CEmitter::genCourseSelect(const CourseStep& step) {
    if (step.arms.empty()) {
        lookup_.fault(spanLo_, spanHi_, "codegen: a course select carries no arm");
        return;
    }
    const std::string table = genCourseArmTable(step);
    if (table.empty()) {
        return;
    }
    const std::string ready = genReadyFlag();
    const std::string chosen = freshName();
    line(joined("uint32_t ", chosen, " = 0;"));
    const std::string payload = newValue();
    const std::string status = freshName();
    line(joined("GeasStatus ", status, " = geas_course_select(ctx, f, ", table, ", ",
                countOf(step.arms.size()), ", &", ready, ", &", chosen, ", &", payload, ");"));
    returnUnlessOk(status);
    suspendUnlessReady(ready);
    const std::string choice = newValue();
    setTag(choice, "GEAS_TY_INT");
    line(joined(choice, ".as.i = (int64_t)", chosen, ";"));
    line(joined(slotText(step.result), " = ", choice, ";"));
    for (std::size_t i = 0; i < step.arms.size(); ++i) {
        if (!step.arms[i].isTimeout) {
            line(joined("if (", chosen, " == ", countOf(i), ") ", slotText(step.arms[i].payload),
                        " = ", payload, ";"));
        }
    }
}

// The call table of a together group: every argument array is rebuilt
// first, each call naming its own. Empty after a fault
std::string CEmitter::genCourseCallTable(const CourseStep& step) {
    std::string calls;
    for (const CourseCall& call : step.calls) {
        if (!checkMember(call.pledge, false, "together call")) {
            return "";
        }
        const std::string array = genCourseArgs(call.pledge, call.arguments);
        calls.append(calls.empty() ? "" : ", ");
        calls.append(joined("{ ", decimal(call.pledge), ", ", array, ", ",
                            countOf(call.arguments.size()), " }"));
    }
    std::string table = freshName();
    line(joined("const GeasCourseCall ", table, "[", countOf(step.calls.size()), "] = { ", calls,
                " };"));
    return table;
}

// The calls start together; the outcomes land in the calls' result slots in
// call order, and the question marks the lowering placed after the step
// apply in statement order
void CEmitter::genCourseTogether(const CourseStep& step) {
    // A group with no call has nothing to wait for
    if (step.calls.empty()) {
        return;
    }
    const std::string table = genCourseCallTable(step);
    if (table.empty()) {
        return;
    }
    const std::string ready = genReadyFlag();
    const std::string outs = freshName();
    line(joined("GeasValue ", outs, "[", countOf(step.calls.size()), "] = {{0}};"));
    const std::string status = freshName();
    line(joined("GeasStatus ", status, " = geas_course_together(ctx, f, ", table, ", ",
                countOf(step.calls.size()), ", &", ready, ", ", outs, ");"));
    returnUnlessOk(status);
    suspendUnlessReady(ready);
    for (std::size_t i = 0; i < step.calls.size(); ++i) {
        line(joined(slotText(step.calls[i].result), " = ", outs, "[", countOf(i), "];"));
    }
}

}  // namespace geas
