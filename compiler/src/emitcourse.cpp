// The course function. A lowered course becomes one resumable C function,
// static uint32_t k_<Contract>_course(void* ctx, GeasCourseFrame* f), a
// state machine the runtime enters again at its saved step. The function
// opens with a dispatch switch that jumps to the resume label of the saved
// step, then runs the lowered instructions in order.
//
// Every course local, every hidden value, and a for loop's collection copy
// and position live in f->slots, read and written through the slot an
// expression of the course arena names. C locals hold only values that die
// inside one instruction: each instruction lowers inside its own brace
// block, so the labels the dispatch switch and the jumps reach all sit at
// function scope with no C local in scope, and no goto ever crosses an
// initialization. No array in the function has a runtime length
#include <unordered_set>

#include "emitc.hpp"
#include "emittext.hpp"
#include "hash.hpp"
#include "overloaded.hpp"

namespace geas {

namespace {

std::string labelName(CourseLabelId label) {
    return joined("label_", decimal(label.index));
}

std::string stepLabel(std::uint32_t step) {
    return joined("step_", decimal(static_cast<std::int64_t>(step)));
}

// The labels some instruction jumps to; a label nothing names is left out so
// the function carries no unused label
std::unordered_set<std::int32_t> referencedLabels(const LoweredCourse& course) {
    std::unordered_set<std::int32_t> labels;
    for (const CourseInstruction& instruction : course.instructions) {
        std::visit(Overloaded{
            [&](const CourseJump& jump) { labels.insert(jump.target.index); },
            [&](const CourseBranch& branch) {
                labels.insert(branch.whenTrue.index);
                labels.insert(branch.whenFalse.index);
            },
            [&](const CourseMatch& match) {
                for (const CourseMatchArm& arm : match.arms) { labels.insert(arm.target.index); }
            },
            [&](const CourseSelect& select) {
                for (const CourseSelectArm& arm : select.arms) { labels.insert(arm.target.index); }
            },
            [&](const CourseForNext& loop) {
                labels.insert(loop.body.index);
                labels.insert(loop.done.index);
            },
            [](const auto&) {},
        }, instruction);
    }
    return labels;
}

}  // namespace

/**
 * Emits the course function of one contract.
 * @params shape - the contract whose lowered course is emitted
 * @returns {std::string} - the function's name, empty when the contract has no course
 */
std::string CEmitter::emitCourse(const ContractShape& shape) {
    const auto found = lookup_.tables().courses.find(shape.decl);
    if (found == lookup_.tables().courses.end()) {
        return "";
    }
    const LoweredCourse& course = found->second;
    const std::string function = joined(contractStem(shape.decl->name), "_course");
    beginBody(TyId{}, shape.course->lo, shape.course->hi);
    course_ = &course;
    const std::unordered_set<std::int32_t> labels = referencedLabels(course);
    for (const CourseInstruction& instruction : course.instructions) {
        const auto* label = std::get_if<CourseLabel>(&instruction);
        if (label == nullptr || labels.contains(label->label.index)) {
            genCourseInstruction(instruction);
        }
    }
    course_ = nullptr;
    top(joined("static uint32_t ", function, "(void* ctx, GeasCourseFrame* f) {"));
    top("    GeasContract* cc = (GeasContract*)ctx;");
    top("    (void)cc;");
    emitCourseDispatch(course);
    out_.append(body_);
    body_.clear();
    top("}");
    top("");
    return function;
}

// The switch on the saved step: 0 starts at the top, every resume point
// jumps to its label, and a step the function never numbered stops the course
void CEmitter::emitCourseDispatch(const LoweredCourse& course) {
    top("    switch (f->step) {");
    top("        case 0: break;");
    for (const CourseStep& step : course.steps) {
        top(joined("        case ", decimal(step.number), ": goto ", stepLabel(step.number), ";"));
    }
    top("        default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;");
    top("    }");
}

/**
 * The descriptor line that carries the course: its function, slot count, and
 * fingerprint, written with designators so every tail field before it reads
 * as zero when the contract declares no other workflow surface.
 * @params shape - the contract, function - the course function's name
 * @returns {std::string} - the line, empty when the contract has no course
 */
std::string CEmitter::emitCourseTail(const ContractShape& shape, std::string_view function) {
    const auto found = lookup_.tables().courses.find(shape.decl);
    if (function.empty() || found == lookup_.tables().courses.end()) {
        return "";
    }
    const LoweredCourse& course = found->second;
    return joined("    .course_fn = ", function, ", .course_nslots = ",
                  decimal(static_cast<std::int64_t>(course.slots.size())), ", .course_hash = 0x",
                  hashHex16(course.courseHash), "ULL,");
}

void CEmitter::genCourseInstruction(const CourseInstruction& instruction) {
    std::visit(Overloaded{
        [&](const CourseStore& store) { genCourseStore(store); },
        [&](const CourseAssign& assignment) {
            openCourseBlock();
            genAssign(AssignStmt{assignment.target, assignment.value});
            closeCourseBlock();
        },
        [&](const CourseCheckTarget& check) { genCourseCheckTarget(check); },
        [&](const CourseEvaluate& evaluate) { genCourseEvaluate(evaluate); },
        [&](const CourseResume& resume) { genCourseResume(resume); },
        [&](const CourseLabel& label) { courseLabel(labelName(label.label)); },
        [&](const CourseJump& jump) { line(joined("goto ", labelName(jump.target), ";")); },
        [&](const CourseBranch& branch) { genCourseBranch(branch); },
        [&](const CourseReturn&) { line("return GEAS_COURSE_DONE;"); },
        [&](const CourseMatch& match) { genCourseMatch(match); },
        [&](const CourseSelect& select) { genCourseSelectDispatch(select); },
        [&](const CourseForNext& loop) { genCourseForNext(loop); },
    }, instruction);
}

// A label sits at function scope, flush left, reached only by goto
void CEmitter::courseLabel(std::string_view name) {
    body_.append(name);
    body_.append(":\n");
}

// Opens the brace block one instruction lowers inside
void CEmitter::openCourseBlock() {
    courseBlockMarks_.push_back(body_.size());
    line("{");
    ++indent_;
    courseBlockMarks_.push_back(body_.size());
}

// Closes the block, dropping its braces when nothing landed inside
void CEmitter::closeCourseBlock() {
    --indent_;
    const std::size_t opened = courseBlockMarks_.back();
    courseBlockMarks_.pop_back();
    const std::size_t start = courseBlockMarks_.back();
    courseBlockMarks_.pop_back();
    if (body_.size() == opened) {
        body_.resize(start);
        return;
    }
    line("}");
}

std::string CEmitter::slotText(CourseSlotId slot) const {
    return joined("f->slots[", decimal(slot.index), "]");
}

TyId CEmitter::slotType(CourseSlotId slot) const {
    return course_->slots[static_cast<std::size_t>(slot.index)].type;
}

// The slot a course arena expression reads, unset outside a course and for
// every expression that is not a slot read
CourseSlotId CEmitter::courseSlotOf(ExprId id) const {
    if (course_ == nullptr) {
        return {};
    }
    return course_->exprSlots[static_cast<std::size_t>(id.index)];
}

// The two expressions a course lowers its own way: a slot read is the slot
// itself, and now() reads the runtime clock once where it runs
std::optional<CValue> CEmitter::courseValue(ExprId id) {
    const CourseSlotId slot = courseSlotOf(id);
    if (slot.index >= 0) {
        return CValue{slotText(slot), slotType(slot)};
    }
    if (course_ == nullptr ||
        course_->exprUses[static_cast<std::size_t>(id.index)] != ExprUse::NowCall) {
        return std::nullopt;
    }
    std::string value = newValue();
    setTag(value, "GEAS_TY_INT");
    line(joined(value, ".as.i = geas_course_now(ctx);"));
    return CValue{std::move(value), types().scalar(TyKind::Time)};
}

// A slot takes its own deep copy of a composite, the isolation a let makes,
// so no later write through another binding reaches it
void CEmitter::storeSlot(CourseSlotId slot, const CValue& value) {
    const CValue copy = copyOut(CValue{value.name, slotType(slot)});
    line(joined(slotText(slot), " = ", copy.name, ";"));
}

void CEmitter::genCourseStore(const CourseStore& store) {
    openCourseBlock();
    if (store.value) {
        storeSlot(store.slot, genExpr(*store.value, slotType(store.slot)));
    } else {
        const std::string unit = newValue();
        setTag(unit, "GEAS_TY_UNIT");
        line(joined(slotText(store.slot), " = ", unit, ";"));
    }
    closeCourseBlock();
}

// An expression run for its effect; a bare slot read has none
void CEmitter::genCourseEvaluate(const CourseEvaluate& evaluate) {
    if (courseSlotOf(evaluate.value).index >= 0) {
        return;
    }
    openCourseBlock();
    const CValue value = genExpr(evaluate.value, TyId{});
    line(joined("(void)", value.name, ";"));
    closeCourseBlock();
}

// Walks an assignment target before the effects its right side runs, so a
// bad index stops the course before a suspending operation. The final index
// of a map may insert, every other step of the path must already exist
void CEmitter::genCourseCheckTarget(const CourseCheckTarget& check) {
    if (courseSlotOf(check.target).index >= 0) {
        return;
    }
    const auto* index = std::get_if<IndexExpr>(&arena().expr(check.target).kind);
    openCourseBlock();
    std::optional<LSlot> slot = genLvalue(index != nullptr && check.canInsert ? index->base
                                                                                : check.target);
    if (slot && index != nullptr && check.canInsert) {
        const TyNode node = nodeOf(slot->type);
        if (node.kind == TyKind::List) {
            slot = LSlot{genListSlot(slot->pointer, index->index), node.args[0]};
        }
    }
    if (slot) {
        line(joined("(void)", slot->pointer, ";"));
    }
    closeCourseBlock();
}

void CEmitter::genCourseBranch(const CourseBranch& branch) {
    openCourseBlock();
    const CValue condition = genExpr(branch.condition, types().scalar(TyKind::Bool));
    line(joined("if (", condition.name, ".as.b) goto ", labelName(branch.whenTrue), ";"));
    line(joined("goto ", labelName(branch.whenFalse), ";"));
    closeCourseBlock();
}

// One pass of a for loop over the collection copy: a List steps one element
// at a time, a Map its flat [k0, v0, k1, v1, ...] store one key at a time
void CEmitter::genCourseForNext(const CourseForNext& loop) {
    const TyNode node = nodeOf(slotType(loop.collection));
    if (node.kind != TyKind::List && node.kind != TyKind::Map) {
        lookup_.unsupported(spanLo_, spanHi_, "for over this type in a course");
        return;
    }
    const std::string collection = slotText(loop.collection);
    const std::string position = slotText(loop.position);
    const std::string_view stride = node.kind == TyKind::Map ? "2" : "1";
    openCourseBlock();
    const std::string index = freshName();
    line(joined("uint64_t ", index, " = (uint64_t)", position, ".as.i * ", stride, ";"));
    line(joined("if (", index, " >= ", collection, ".as.list.len) goto ", labelName(loop.done),
                ";"));
    const std::string element = freshName();
    line(joined("GeasValue ", element, " = ((const GeasValue*)", collection, ".as.list.data)[",
                index, "];"));
    line(joined(position, ".as.i += 1;"));
    storeSlot(loop.variable, CValue{element, slotType(loop.variable)});
    line(joined("goto ", labelName(loop.body), ";"));
    closeCourseBlock();
}

}  // namespace geas
