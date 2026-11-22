// The hidden dump command's deterministic course listing
#include "coursedumper.hpp"

#include <array>
#include <charconv>
#include <system_error>
#include <utility>

#include "astdumper.hpp"
#include "lexdump.hpp"
#include "hash.hpp"
#include "overloaded.hpp"

namespace geas {

namespace {

constexpr std::array slotNames = {"local", "temporary", "collection", "position", "choice", "payload"};
constexpr std::array stepNames = {"call", "await", "sleep", "select", "together"};
constexpr std::size_t indentationWidth = 4;

std::string slotText(CourseSlotId slot) {
    return slot.index < 0 ? "None" : "Slot(" + std::to_string(slot.index) + ")";
}

std::string labelText(CourseLabelId label) {
    return "Label(" + std::to_string(label.index) + ")";
}

std::string boolText(bool isTrue) {
    return isTrue ? "true" : "false";
}

}  // namespace

void CourseDumper::line(int level, const std::string& text) {
    output_.append(static_cast<std::size_t>(level) * indentationWidth, ' ');
    output_ += text + "\n";
}

// A pledge or signal by its descriptor index, or by its name in the
// canonical text, so adding a member before it leaves the text alone
std::string CourseDumper::memberText(std::uint32_t index) const {
    if (!isCanonical()) {
        return std::to_string(index);
    }
    return index < members_.size() ? members_[index] : "#" + std::to_string(index);
}

// An expression's text followed by its type code in the canonical text
std::string CourseDumper::typed(ExprId value, std::string text) const {
    if (!isCanonical()) {
        return text;
    }
    const TyId type = course_.exprTypes[static_cast<std::size_t>(value.index)];
    return text + ":" + std::string{type.isSet() ? types_.canon(type) : "?"};
}

// A slot by its index, name, and type; the canonical text drops the name,
// which no frame keeps, and spells the type by its code
void CourseDumper::renderSlot(const CourseSlot& slot) {
    const std::string kind = std::string{slotNames[static_cast<std::size_t>(slot.kind)]} +
                             (slot.isMutable ? ", mut" : "") + "),";
    if (isCanonical()) {
        const std::string type = slot.type.isSet() ? std::string{types_.canon(slot.type)} : "?";
        line(3, slotText(slot.id) + ": " + type + " (" + kind);
        return;
    }
    line(3, slotText(slot.id) + " \"" + slot.name + "\": " + types_.render(slot.type) + " (" + kind);
}

void CourseDumper::renderCall(const CourseCall& call) {
    line(4, "Call {");
    line(5, "pledge: " + memberText(call.pledge) + ",");
    line(5, "arguments: " + arguments(call.arguments) + ",");
    line(5, "result: " + slotText(call.result) + ",");
    line(5, "question: " + boolText(call.hasQuestion) + ",");
    line(4, "},");
}

void CourseDumper::renderStep(const CourseStep& step) {
    line(3, "Step " + std::to_string(step.number) + " " +
        stepNames[static_cast<std::size_t>(step.kind)] + " {");
    if (step.member) {
        line(4, std::string{step.kind == CourseStepKind::Call ? "pledge: " : "signal: "} +
            memberText(*step.member) + ",");
    }
    line(4, "arguments: " + arguments(step.arguments) + ",");
    line(4, "result: " + slotText(step.result) + ",");
    line(4, "question: " + boolText(step.hasQuestion) + ",");
    if (step.kind == CourseStepKind::Await) {
        line(4, "within: " + (step.withinMilliseconds ?
            std::to_string(*step.withinMilliseconds) : "None") + ",");
    }
    for (const CourseWaitArm& arm : step.arms) {
        line(4, arm.isTimeout ? "Timeout(" + std::to_string(arm.milliseconds) + ")," :
            "Await(signal: " + memberText(arm.signal) + ", payload: " + slotText(arm.payload) + "),");
    }
    for (const CourseCall& call : step.calls) {
        renderCall(call);
    }
    line(3, "},");
}

void CourseDumper::renderMatch(const CourseMatch& match) {
    line(3, "Match " + expression(match.value) + " {");
    for (const CourseMatchArm& arm : match.arms) {
        line(4, pattern(arm.pattern, arm.bindings) + " -> " + labelText(arm.target) + " " +
            bindings(arm.bindings) + ",");
    }
    line(3, "},");
}

void CourseDumper::renderSelect(const CourseSelect& select) {
    line(3, "Select " + std::to_string(select.step) + " {");
    for (std::size_t i = 0; i < select.arms.size(); ++i) {
        const CourseSelectArm& arm = select.arms[i];
        const std::string armPattern = arm.pattern ? pattern(*arm.pattern, arm.bindings) : "_";
        line(4, "Arm " + std::to_string(i) + " " + armPattern +
            " -> " + labelText(arm.target) + " " + bindings(arm.bindings) + ",");
    }
    line(3, "},");
}

void CourseDumper::renderInstruction(const CourseInstruction& instruction) {
    std::visit(Overloaded{
        [&](const CourseStore& store) {
            line(3, "Store " + slotText(store.slot) + " = " +
                (store.value ? expression(*store.value) : "Unit") + ",");
        },
        [&](const CourseAssign& assignment) {
            line(3, "Assign " + expression(assignment.target) + " = " + expression(assignment.value) + ",");
        },
        [&](const CourseCheckTarget& target) {
            line(3, "CheckTarget " + expression(target.target) +
                " (insert: " + boolText(target.canInsert) + "),");
        },
        [&](const CourseEvaluate& evaluate) { line(3, "Evaluate " + expression(evaluate.value) + ","); },
        [&](const CourseResume& resume) { line(3, "Resume " + std::to_string(resume.step) + ","); },
        [&](const CourseLabel& label) { line(3, labelText(label.label) + ":"); },
        [&](const CourseJump& jump) { line(3, "Jump " + labelText(jump.target) + ","); },
        [&](const CourseBranch& branch) {
            line(3, "Branch " + expression(branch.condition) + " ? " + labelText(branch.whenTrue) +
                " : " + labelText(branch.whenFalse) + ",");
        },
        [&](const CourseReturn&) { line(3, "Return,"); },
        [&](const CourseMatch& match) { renderMatch(match); },
        [&](const CourseSelect& select) { renderSelect(select); },
        [&](const CourseForNext& loop) {
            line(3, "ForNext(collection: " + slotText(loop.collection) + ", position: " +
                slotText(loop.position) + ", variable: " + slotText(loop.variable) +
                ", body: " + labelText(loop.body) + ", done: " + labelText(loop.done) + "),");
        },
    }, instruction);
}

std::string CourseDumper::run(const std::string& name) {
    line(1, "Course \"" + name + "\" {");
    line(2, "hash: 0x" + hashHex16(course_.courseHash) + ",");
    line(2, "entry: 0,");
    renderBody();
    line(1, "},");
    return std::move(output_);
}

std::string CourseDumper::canonical() {
    renderBody();
    return std::move(output_);
}

// The slots, the resume steps, and the lowered statements
void CourseDumper::renderBody() {
    line(2, "slots: [");
    for (const CourseSlot& slot : course_.slots) { renderSlot(slot); }
    line(2, "],");
    line(2, "steps: [");
    for (const CourseStep& step : course_.steps) { renderStep(step); }
    line(2, "],");
    line(2, "statements: [");
    for (const CourseInstruction& instruction : course_.instructions) { renderInstruction(instruction); }
    line(2, "],");
}

std::string dumpCourses(const CheckedProgram& checked) {
    std::string result = "Courses [\n";
    for (const ContractShape& contract : checked.contracts) {
        const auto found = checked.tables.courses.find(contract.decl);
        if (found != checked.tables.courses.end()) {
            result += CourseDumper{found->second, checked.types}.run(contract.decl->name);
        }
    }
    return result + "]\n";
}

namespace {

constexpr std::array binaryNames = {"Add", "Sub", "Mul", "Div", "Mod", "Eq", "Ne", "Lt",
                                    "Le", "Gt", "Ge", "And", "Or"};

// Room for the shortest round trip text of any double, sign and exponent included
constexpr std::size_t floatTextCapacity = 32;

std::string quotedName(const std::string& name) {
    return "\"" + name + "\"";
}

// The fields of a record literal in the order the record declares them,
// any field the record does not declare after them in source order
std::vector<const FieldInit*> inDeclarationOrder(const RecordLiteral& record,
                                                 const RecordSig& declared) {
    std::vector<const FieldInit*> ordered;
    std::vector<bool> isTaken(record.fields.size(), false);
    for (const FieldSig& field : declared.fields) {
        for (std::size_t i = 0; i < record.fields.size(); ++i) {
            if (!isTaken[i] && record.fields[i].name == field.name) {
                ordered.push_back(&record.fields[i]);
                isTaken[i] = true;
                break;
            }
        }
    }
    for (std::size_t i = 0; i < record.fields.size(); ++i) {
        if (!isTaken[i]) {
            ordered.push_back(&record.fields[i]);
        }
    }
    return ordered;
}

// A Float literal by its value: the shortest decimal in scientific form that
// reads back as the same double, which the C++ standard fixes digit for
// digit, so every platform spells one value one way. A spelling that does
// not read as a double stays as written, underscores dropped
std::string canonicalFloat(const std::string& spelling) {
    std::string digits;
    for (const char c : spelling) {
        if (c != '_') { digits.push_back(c); }
    }
    double value = 0.0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (end != digits.data() + digits.size()) {
        return digits;
    }
    // Below the least subnormal reads as zero, the double the module holds
    if (error == std::errc::result_out_of_range) {
        value = 0.0;
    } else if (error != std::errc{}) {
        return digits;
    }
    std::array<char, floatTextCapacity> text{};
    const auto written = std::to_chars(text.data(), text.data() + text.size(), value,
                                       std::chars_format::scientific);
    return std::string{text.data(), written.ptr};
}

}  // namespace

std::string CourseDumper::arguments(const std::vector<ExprId>& values) const {
    std::string result = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) { result += ", "; }
        result += expression(values[i]);
    }
    return result + "]";
}

std::string CourseDumper::callArguments(const std::vector<Argument>& values, bool isLabeled) const {
    std::string result = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) { result += ", "; }
        if (isLabeled && values[i].label) { result += *values[i].label + ": "; }
        result += expression(values[i].value);
    }
    return result + "]";
}

// A call. The checker holds every label on a variant's payload to the field
// at its position, so the canonical text drops them
std::string CourseDumper::call(const CallExpr& value) const {
    const auto* callee = std::get_if<Identifier>(&course_.ast.expr(value.callee).kind);
    const bool isVariant =
        callee != nullptr && sigs_ != nullptr && sigs_->variant(callee->name) != nullptr;
    const bool isLabeled = !isCanonical() || !isVariant;
    return "Call(" + expression(value.callee) + ", " + callArguments(value.args, isLabeled) + ")";
}

// A record literal. The canonical text lists the fields in declaration
// order, the order the emitted code builds them in, so reordering them in
// the source leaves the text alone
std::string CourseDumper::recordLiteral(const RecordLiteral& record) const {
    std::vector<const FieldInit*> fields;
    for (const FieldInit& field : record.fields) {
        fields.push_back(&field);
    }
    const RecordSig* declared = sigs_ == nullptr ? nullptr : sigs_->record(record.name);
    if (isCanonical() && declared != nullptr) {
        fields = inDeclarationOrder(record, *declared);
    }
    std::string result = "Record(" + quotedName(record.name) + ", [";
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i != 0) { result += ", "; }
        result += fields[i]->name + ": " + expression(fields[i]->value);
    }
    return result + "])";
}

std::string CourseDumper::type(TypeId value) const {
    const Type& node = course_.ast.type(value);
    if (const auto* named = std::get_if<NamedType>(&node)) {
        std::string result = named->name;
        if (named->args.empty()) { return result; }
        result += "<";
        for (std::size_t i = 0; i < named->args.size(); ++i) {
            if (i != 0) { result += ", "; }
            result += type(named->args[i]);
        }
        return result + ">";
    }
    const auto& pledge = std::get<PledgeType>(node);
    std::string result = "pledge(";
    for (std::size_t i = 0; i < pledge.params.size(); ++i) {
        if (i != 0) { result += ", "; }
        result += type(pledge.params[i]);
    }
    return result + ") -> " + type(pledge.ret);
}

std::string CourseDumper::expression(ExprId value) const {
    const CourseSlotId slot = course_.exprSlots[static_cast<std::size_t>(value.index)];
    if (slot.index >= 0) {
        return typed(value, "Slot(" + std::to_string(slot.index) + ")");
    }
    return typed(value, std::visit(Overloaded{
        [](const IntLiteral& literal) { return "Int(" + std::to_string(literal.value) + ")"; },
        [&](const FloatLiteral& literal) {
            const std::string text = isCanonical() ? canonicalFloat(literal.spelling) : literal.spelling;
            return "Float(" + text + ")";
        },
        [&](const StringLiteral& literal) {
            if (isCanonical()) {
                return "Str(" + std::to_string(literal.body.size()) + ":" + literal.body + ")";
            }
            return "Str(" + quotedName(literal.body) + ")";
        },
        [](const CharLiteral& literal) {
            return "Char('" + canonicalScalar(literal.scalar, '\'') + "')";
        },
        [](const BoolLiteral& literal) { return std::string{literal.value ? "true" : "false"}; },
        [](const DurationLiteral& literal) {
            return "Duration(" + std::to_string(literal.milliseconds) + ")";
        },
        [](const Identifier& name) { return "Ident(" + quotedName(name.name) + ")"; },
        [&](const UnaryExpr& unary) {
            return std::string{unary.op == UnaryOp::Not ? "Not(" : "Neg("} +
                expression(unary.operand) + ")";
        },
        [&](const BinaryExpr& binary) {
            return std::string{binaryNames[static_cast<std::size_t>(binary.op)]} + "(" +
                expression(binary.lhs) + ", " + expression(binary.rhs) + ")";
        },
        [&](const CallExpr& value) { return call(value); },
        [&](const IndexExpr& index) {
            return "Index(" + expression(index.base) + ", " + expression(index.index) + ")";
        },
        [&](const MemberExpr& member) {
            return "Member(" + expression(member.base) + ", " + quotedName(member.name) + ")";
        },
        [&](const QuestionExpr& question) { return "Question(" + expression(question.operand) + ")"; },
        [&](const WrapExpr& wrap) {
            return std::string{wrapKindName(wrap.kind)} + "(" + expression(wrap.inner) + ")";
        },
        [&](const ListExpr& list) { return "List(" + arguments(list.elements) + ")"; },
        [&](const TupleExpr& tuple) { return "Tuple(" + arguments(tuple.elements) + ")"; },
        [&](const RecordLiteral& record) { return recordLiteral(record); },
        [](const NoneExpr&) { return std::string{"None"}; },
        [&](const MapNewExpr& map) {
            return isCanonical() ? std::string{"MapNew()"} : "MapNew(" + type(map.type) + ")";
        },
        [&](const GenericNewExpr& constructor) {
            const std::string arguments = callArguments(constructor.args);
            return isCanonical() ? "New(" + arguments + ")"
                                 : "New(" + type(constructor.type) + ", " + arguments + ")";
        },
        [&](const auto&) {
            const std::size_t kind = course_.ast.expr(value).kind.index();
            return isCanonical() ? "Unlowered(" + std::to_string(kind) + ")" : std::string{"Unlowered"};
        },
    }, course_.ast.expr(value).kind));
}

// A pattern; the canonical text spells a binding by its position among the
// bindings of its arm, so renaming one leaves the text alone
std::string CourseDumper::pattern(PatternId value,
                                  const std::vector<CoursePatternSlot>& slots) const {
    return std::visit(Overloaded{
        [](const WildcardPattern&) { return std::string{"_"}; },
        [&](const LiteralPattern& literal) { return expression(literal.literal); },
        [&](const BindPattern& binding) {
            if (!isCanonical()) {
                return binding.name;
            }
            std::size_t position = 0;
            while (position < slots.size() && slots[position].pattern.index != value.index) {
                ++position;
            }
            return "Bind(" + std::to_string(position) + ")";
        },
        [](const NonePattern&) { return std::string{"None"}; },
        [&](const WrapPattern& wrap) {
            return std::string{wrapKindName(wrap.kind)} + "(" + pattern(wrap.inner, slots) + ")";
        },
        [&](const VariantPattern& variant) {
            std::string result = variant.name + "(";
            for (std::size_t i = 0; i < variant.fields.size(); ++i) {
                if (i != 0) { result += ", "; }
                result += pattern(variant.fields[i], slots);
            }
            return result + ")";
        },
    }, course_.ast.pattern(value));
}

std::string CourseDumper::bindings(const std::vector<CoursePatternSlot>& values) const {
    std::string result = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) { result += ", "; }
        result += pattern(values[i].pattern, values) + " -> Slot(" +
                  std::to_string(values[i].slot.index) + ")";
    }
    return result + "]";
}

}  // namespace geas
