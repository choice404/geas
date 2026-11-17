// The two dispatches of a course. A match tests its arms in source order
// against the scrutinee, a select reads the arm index its step chose, and
// either one copies the arm's pattern bindings into their frame slots and
// jumps to the arm's label. The pattern tests are the match lowering's own
// do while zero walks, so a course pattern means exactly what it means in a
// pledge body. A dispatch that no arm catches, a case a checked program
// cannot reach, stops the course rather than run an arm it did not choose
#include "emitc.hpp"
#include "emittext.hpp"

namespace geas {

namespace {

std::string labelName(CourseLabelId label) {
    return joined("label_", decimal(label.index));
}

}  // namespace

/**
 * Emits one arm of a course dispatch: the pattern test against value, the
 * bindings copied into their slots, and the jump to the arm's label.
 * @params pattern - the arm's pattern, value - the value it tests, bindings - the slot of each
 * binding in pattern order, target - the arm's label
 * @returns {bool} - true when the pattern matches every value, so no later arm can run
 */
bool CEmitter::genCourseArm(PatternId pattern, const CValue& value,
                            const std::vector<CoursePatternSlot>& bindings,
                            CourseLabelId target) {
    if (isIrrefutable(pattern, value.type)) {
        if (!bindings.empty()) {
            storeSlot(bindings.front().slot, value);
        }
        line(joined("goto ", labelName(target), ";"));
        return true;
    }
    std::vector<PatternBind> binds;
    collectBinds(pattern, value.type, binds);
    if (binds.size() != bindings.size()) {
        lookup_.fault(spanLo_, spanHi_, "codegen: a course pattern binds other names than its slots");
        return false;
    }
    openCourseBlock();
    for (const PatternBind& bind : binds) {
        line(joined("GeasValue ", bind.variable, " = {0};"));
    }
    const std::string hit = freshName();
    line(joined("int ", hit, " = 0;"));
    line("do {");
    ++indent_;
    genPatternTest(pattern, value.name, value.type, binds, 0);
    line(joined(hit, " = 1;"));
    --indent_;
    line("} while (0);");
    line(joined("if (", hit, ") {"));
    ++indent_;
    for (std::size_t i = 0; i < binds.size(); ++i) {
        storeSlot(bindings[i].slot, CValue{binds[i].variable, binds[i].type});
    }
    line(joined("goto ", labelName(target), ";"));
    --indent_;
    line("}");
    closeCourseBlock();
    return false;
}

void CEmitter::genCourseNoArm() {
    line("f->status = GEAS_ERR_STATE;");
    line("return GEAS_COURSE_STOPPED;");
}

void CEmitter::genCourseMatch(const CourseMatch& match) {
    openCourseBlock();
    const CValue scrutinee = genExpr(match.value, TyId{});
    bool isCaught = false;
    for (const CourseMatchArm& arm : match.arms) {
        isCaught = genCourseArm(arm.pattern, scrutinee, arm.bindings, arm.target);
        if (isCaught) {
            break;
        }
    }
    if (!isCaught) {
        genCourseNoArm();
    }
    closeCourseBlock();
}

// The chosen index sits in the step's result slot and an await arm's payload
// in its own slot, both written when the select answered ready
void CEmitter::genCourseSelectDispatch(const CourseSelect& select) {
    const CourseStep& step = course_->steps[static_cast<std::size_t>(select.step - 1)];
    if (step.arms.size() != select.arms.size()) {
        lookup_.fault(spanLo_, spanHi_, "codegen: a course select dispatch disagrees with its step");
        return;
    }
    openCourseBlock();
    const std::string choice = slotText(step.result);
    for (std::size_t i = 0; i < select.arms.size(); ++i) {
        const CourseSelectArm& arm = select.arms[i];
        line(joined("if (", choice, ".as.i == ", decimal(static_cast<std::int64_t>(i)), ") {"));
        ++indent_;
        if (arm.pattern) {
            const CourseSlotId payload = step.arms[i].payload;
            genCourseArm(*arm.pattern, CValue{slotText(payload), slotType(payload)}, arm.bindings,
                         arm.target);
        } else {
            line(joined("goto ", labelName(arm.target), ";"));
        }
        --indent_;
        line("}");
    }
    genCourseNoArm();
    closeCourseBlock();
}

}  // namespace geas
