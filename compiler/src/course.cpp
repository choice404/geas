// Course slots, resume points, and the checked program integration
#include "coursebuilder.hpp"

#include <set>

#include "coursedumper.hpp"
#include "hash.hpp"
#include "layout.hpp"

namespace geas {

CourseBuilder::CourseBuilder(CheckedProgram& checked, const ContractShape& contract)
    : checked_{checked}, source_{checked.program.ast}, contract_{contract} {
    course_.ast.types = source_.types;
    course_.ast.patterns = source_.patterns;
    for (Pattern& pattern : course_.ast.patterns) {
        if (auto* literal = std::get_if<LiteralPattern>(&pattern)) {
            literal->literal = lowerExpression(literal->literal);
        }
    }
}

std::expected<LoweredCourse, std::string> CourseBuilder::run() {
    lowerBlock(contract_.course->body);
    emit(CourseReturn{});
    if (!error_.empty()) {
        return std::unexpected(std::move(error_));
    }
    course_.courseHash = hashCourse();
    return std::move(course_);
}

TyId CourseBuilder::typeOf(ExprId expression) const {
    return checked_.tables.exprTypes[static_cast<std::size_t>(expression.index)];
}

ExprUse CourseBuilder::useOf(ExprId expression) const {
    return checked_.tables.exprUses[static_cast<std::size_t>(expression.index)];
}

ExprId CourseBuilder::append(ExprKind kind, TyId type, ExprUse use) {
    const ExprId expression = course_.ast.addExpr(Expr{0, 0, std::move(kind)});
    course_.exprTypes.push_back(type);
    course_.exprUses.push_back(use);
    course_.exprSlots.emplace_back();
    return expression;
}

ExprId CourseBuilder::copyExpression(ExprId original, ExprKind kind) {
    const ExprId expression = append(std::move(kind), typeOf(original), useOf(original));
    Expr& node = course_.ast.exprs[static_cast<std::size_t>(expression.index)];
    node.lo = source_.expr(original).lo;
    node.hi = source_.expr(original).hi;
    return expression;
}

ExprId CourseBuilder::slotValue(CourseSlotId slot) {
    const CourseSlot& value = course_.slots[static_cast<std::size_t>(slot.index)];
    const ExprId expression = append(Identifier{value.name}, value.type, ExprUse::Local);
    course_.exprSlots[static_cast<std::size_t>(expression.index)] = slot;
    return expression;
}

bool CourseBuilder::hasContractType(TyId type) const {
    if (!type.isSet()) {
        return false;
    }
    const TyNode& node = checked_.types.node(type);
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

CourseSlotId CourseBuilder::addSlot(TyId type, CourseSlotKind kind, std::string name,
                                    bool isMutable) {
    const CourseSlotId slot{static_cast<std::int32_t>(course_.slots.size())};
    if (name.empty()) {
        name = "$hidden" + std::to_string(slot.index);
    }
    if (hasContractType(type) && error_.empty()) {
        error_ = "geas: a course temporary cannot hold a contract instance in its frame; signing another contract and holding its instance in a course "
                 "is not allowed";
    }
    course_.slots.push_back(CourseSlot{slot, std::move(name), type, kind, isMutable});
    return slot;
}

CourseSlotId CourseBuilder::bindLocal(const std::string& name, TyId type, bool isMutable) {
    const CourseSlotId slot = addSlot(type, CourseSlotKind::Local, name, isMutable);
    scopes_.back().insert_or_assign(name, slot);
    return slot;
}

CourseSlotId CourseBuilder::lookup(const std::string& name) const {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        const auto found = scope->find(name);
        if (found != scope->end()) {
            return found->second;
        }
    }
    return {};
}

// Constants rebuild without an effect, hidden values already live in the frame
ExprId CourseBuilder::capture(ExprId expression) {
    const std::size_t index = static_cast<std::size_t>(expression.index);
    const TyId type = course_.exprTypes[index];
    if (!type.isSet()) {
        return expression;
    }
    const CourseSlotId existing = course_.exprSlots[index];
    if (existing.index >= 0 &&
        course_.slots[static_cast<std::size_t>(existing.index)].kind != CourseSlotKind::Local) {
        return expression;
    }
    const ExprKind& kind = course_.ast.expr(expression).kind;
    const bool isConstant = std::holds_alternative<IntLiteral>(kind) ||
        std::holds_alternative<FloatLiteral>(kind) || std::holds_alternative<StringLiteral>(kind) ||
        std::holds_alternative<CharLiteral>(kind) || std::holds_alternative<BoolLiteral>(kind) ||
        std::holds_alternative<DurationLiteral>(kind) || std::holds_alternative<NoneExpr>(kind);
    if (isConstant) {
        return expression;
    }
    const CourseSlotId slot = addSlot(type, CourseSlotKind::Temporary);
    emit(CourseStore{slot, expression});
    return slotValue(slot);
}

CourseLabelId CourseBuilder::newLabel() {
    return CourseLabelId{labelCount_++};
}

std::uint32_t CourseBuilder::newStep(CourseStepKind kind, bool hasQuestion) {
    CourseStep operation;
    operation.number = static_cast<std::uint32_t>(course_.steps.size() + 1);
    operation.kind = kind;
    operation.hasQuestion = hasQuestion;
    course_.steps.push_back(std::move(operation));
    return course_.steps.back().number;
}

CourseStep& CourseBuilder::step(std::uint32_t number) {
    return course_.steps[static_cast<std::size_t>(number - 1)];
}

void CourseBuilder::emit(CourseInstruction instruction) {
    course_.instructions.push_back(std::move(instruction));
}

/**
 * Resolves a pledge or signal the course names to its descriptor index.
 * @params name the pledge or signal name, isSignal whether only signals match
 * @returns {std::uint32_t} - the index, or kMissingMember with the build error recorded
 */
std::uint32_t CourseBuilder::memberIndex(const std::string& name, bool isSignal) {
    // Pledges first, then signals, the order the descriptor lists them in
    if (!isSignal) {
        for (std::size_t i = 0; i < contract_.pledges.size(); ++i) {
            if (contract_.pledges[i].decl->name == name) {
                return static_cast<std::uint32_t>(i);
            }
        }
    }
    for (std::size_t i = 0; i < contract_.signals.size(); ++i) {
        if (contract_.signals[i].decl->name == name) {
            return static_cast<std::uint32_t>(contract_.pledges.size() + i);
        }
    }
    if (error_.empty()) {
        error_ = "geas: the course of " + contract_.decl->name + " names " +
                 (isSignal ? "signal " : "pledge ") + name + ", which the contract does not declare";
    }
    return kMissingMember;
}

TyId CourseBuilder::signalPayload(std::uint32_t signal) {
    // A member that did not resolve already failed the build, its payload is moot
    if (signal == kMissingMember || signal < contract_.pledges.size()) {
        return checked_.types.scalar(TyKind::Unit);
    }
    const std::size_t index = signal - contract_.pledges.size();
    const auto found = checked_.sigs.signals.find(contract_.signals[index].decl);
    const std::vector<TyId>& params = found->second.params;
    if (params.empty()) {
        return checked_.types.scalar(TyKind::Unit);
    }
    if (params.size() == 1) {
        return params.front();
    }
    return checked_.types.tuple(params);
}

/**
 * The course hash: FNV-1a 64 over the canonical text of the lowered course,
 * its slots with their kinds and type codes, its steps, and its lowered
 * statements with every expression spelled by structure, followed by the
 * layout of every declared record and sum a slot type reaches and the
 * columns of every schema the contract declares. Comments and spacing never
 * reach the lowered form, and the canonical text spells no local or binding
 * name and no literal the way the source wrote it, see coursedumper.hpp, so
 * only a change to what a frame holds or how the course reads it moves the
 * hash.
 * @returns {std::uint64_t} - the hash a park row carries beside the frame
 */
std::uint64_t CourseBuilder::hashCourse() const {
    std::vector<std::string> members;
    for (const PledgeShape& pledge : contract_.pledges) {
        members.push_back(pledge.decl->name);
    }
    for (const SignalShape& signal : contract_.signals) {
        members.push_back(signal.decl->name);
    }
    std::string text =
        CourseDumper{course_, checked_.types, CourseDumpMode::Canonical, std::move(members),
                     &checked_.sigs}
            .canonical();
    const LayoutIndex layouts{checked_.program.ast, checked_.sigs, checked_.types};
    std::set<std::string> names;
    for (const CourseSlot& slot : course_.slots) {
        layouts.reach(slot.type, names);
    }
    text.append(layoutSegments(layouts.layoutsOf(names)));
    for (const SchemaDecl* schema : contract_.schemas) {
        text.append(schemaSegment(checked_.sigs, checked_.types, *schema));
    }
    return fnv1a64(text);
}

std::expected<void, std::string> lowerCourses(CheckedProgram& checked) {
    for (const ContractShape& contract : checked.contracts) {
        if (contract.course == nullptr) {
            continue;
        }
        auto course = CourseBuilder{checked, contract}.run();
        if (!course) {
            return std::unexpected(std::move(course.error()));
        }
        checked.tables.courses.emplace(contract.decl, std::move(*course));
    }
    return {};
}

}  // namespace geas
