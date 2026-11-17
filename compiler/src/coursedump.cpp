// The hidden dump command's deterministic course listing
#include "coursedumper.hpp"

#include <array>
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

void CourseDumper::renderSlot(const CourseSlot& slot) {
    line(3, slotText(slot.id) + " \"" + slot.name + "\": " + types_.render(slot.type) +
        " (" + slotNames[static_cast<std::size_t>(slot.kind)] +
        (slot.isMutable ? ", mut" : "") + "),");
}

void CourseDumper::renderCall(const CourseCall& call) {
    line(4, "Call {");
    line(5, "pledge: " + std::to_string(call.pledge) + ",");
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
            std::to_string(*step.member) + ",");
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
            "Await(signal: " + std::to_string(arm.signal) + ", payload: " + slotText(arm.payload) + "),");
    }
    for (const CourseCall& call : step.calls) {
        renderCall(call);
    }
    line(3, "},");
}

void CourseDumper::renderMatch(const CourseMatch& match) {
    line(3, "Match " + expression(match.value) + " {");
    for (const CourseMatchArm& arm : match.arms) {
        line(4, pattern(arm.pattern) + " -> " + labelText(arm.target) + " " + bindings(arm.bindings) + ",");
    }
    line(3, "},");
}

void CourseDumper::renderSelect(const CourseSelect& select) {
    line(3, "Select " + std::to_string(select.step) + " {");
    for (std::size_t i = 0; i < select.arms.size(); ++i) {
        const CourseSelectArm& arm = select.arms[i];
        line(4, "Arm " + std::to_string(i) + " " + (arm.pattern ? pattern(*arm.pattern) : "_") +
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
    line(2, "slots: [");
    for (const CourseSlot& slot : course_.slots) { renderSlot(slot); }
    line(2, "],");
    line(2, "steps: [");
    for (const CourseStep& step : course_.steps) { renderStep(step); }
    line(2, "],");
    line(2, "statements: [");
    for (const CourseInstruction& instruction : course_.instructions) { renderInstruction(instruction); }
    line(2, "],");
    line(1, "},");
    return std::move(output_);
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

std::string quotedName(const std::string& name) {
    return "\"" + name + "\"";
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

std::string CourseDumper::callArguments(const std::vector<Argument>& values) const {
    std::string result = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) { result += ", "; }
        if (values[i].label) { result += *values[i].label + ": "; }
        result += expression(values[i].value);
    }
    return result + "]";
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
        return "Slot(" + std::to_string(slot.index) + ")";
    }
    return std::visit(Overloaded{
        [](const IntLiteral& literal) { return "Int(" + std::to_string(literal.value) + ")"; },
        [](const FloatLiteral& literal) { return "Float(" + literal.spelling + ")"; },
        [](const StringLiteral& literal) { return "Str(" + quotedName(literal.body) + ")"; },
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
        [&](const CallExpr& call) {
            return "Call(" + expression(call.callee) + ", " + callArguments(call.args) + ")";
        },
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
        [&](const RecordLiteral& record) {
            std::string result = "Record(" + quotedName(record.name) + ", [";
            for (std::size_t i = 0; i < record.fields.size(); ++i) {
                if (i != 0) { result += ", "; }
                result += record.fields[i].name + ": " + expression(record.fields[i].value);
            }
            return result + "])";
        },
        [](const NoneExpr&) { return std::string{"None"}; },
        [&](const MapNewExpr& map) { return "MapNew(" + type(map.type) + ")"; },
        [&](const GenericNewExpr& constructor) {
            return "New(" + type(constructor.type) + ", " + callArguments(constructor.args) + ")";
        },
        [](const auto&) { return std::string{"Unlowered"}; },
    }, course_.ast.expr(value).kind);
}

std::string CourseDumper::pattern(PatternId value) const {
    return std::visit(Overloaded{
        [](const WildcardPattern&) { return std::string{"_"}; },
        [&](const LiteralPattern& literal) { return expression(literal.literal); },
        [](const BindPattern& binding) { return binding.name; },
        [](const NonePattern&) { return std::string{"None"}; },
        [&](const WrapPattern& wrap) {
            return std::string{wrapKindName(wrap.kind)} + "(" + pattern(wrap.inner) + ")";
        },
        [&](const VariantPattern& variant) {
            std::string result = variant.name + "(";
            for (std::size_t i = 0; i < variant.fields.size(); ++i) {
                if (i != 0) { result += ", "; }
                result += pattern(variant.fields[i]);
            }
            return result + ")";
        },
    }, course_.ast.pattern(value));
}

std::string CourseDumper::bindings(const std::vector<CoursePatternSlot>& values) const {
    std::string result = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) { result += ", "; }
        result += pattern(values[i].pattern) + " -> Slot(" + std::to_string(values[i].slot.index) + ")";
    }
    return result + "]";
}

}  // namespace geas
