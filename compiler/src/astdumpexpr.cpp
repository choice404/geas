// The dump of types and expressions. Leaves print on one line, operators
// and constructors as wrappers, and the larger forms as named structs
#include "astdumper.hpp"

#include <array>
#include <string>
#include <variant>

#include "lexdump.hpp"

namespace geas {

namespace {

// Indexed by BinaryOp, so the order follows the enum
constexpr std::array binaryOpNames{
    std::string_view{"Add"}, std::string_view{"Sub"}, std::string_view{"Mul"},
    std::string_view{"Div"}, std::string_view{"Mod"}, std::string_view{"Eq"},
    std::string_view{"Ne"},  std::string_view{"Lt"},  std::string_view{"Le"},
    std::string_view{"Gt"},  std::string_view{"Ge"},  std::string_view{"And"},
    std::string_view{"Or"},
};

std::string_view binaryOpName(BinaryOp op) {
    return binaryOpNames[static_cast<std::size_t>(op)];
}

}  // namespace

// Named("x") or Named("x", [ args ]), and PledgeType { params, ret }
void AstDumper::renderType(TypeId id, int level) {
    if (const auto* named = std::get_if<NamedType>(&ast_.type(id))) {
        write("Named(");
        quoted(named->name);
        if (!named->args.empty()) {
            write(", ");
            renderTypeList(named->args, level);
        }
        write(")");
        return;
    }
    const auto& pledge = std::get<PledgeType>(ast_.type(id));
    openStruct("PledgeType");
    key(level + 1, "params");
    renderTypeList(pledge.params, level + 1);
    endField();
    key(level + 1, "ret");
    renderType(pledge.ret, level + 1);
    endField();
    closeStruct(level);
}

void AstDumper::renderTypeList(const std::vector<TypeId>& types, int level) {
    list(types, level, [&](TypeId type, int inner) { renderType(type, inner); });
}

void AstDumper::renderExpr(ExprId id, int level) {
    const ExprKind& kind = ast_.expr(id).kind;
    if (renderLeafExpr(kind)) {
        return;
    }
    std::visit(Overloaded{
                   [&](const UnaryExpr& e) { renderUnary(e, level); },
                   [&](const BinaryExpr& e) { renderBinary(e, level); },
                   [&](const IndexExpr& e) { renderPair("Index", e.base, e.index, level); },
                   [&](const MemberExpr& e) { renderMember(e, level); },
                   [&](const QuestionExpr& e) { renderWrapped("Question", e.operand, level); },
                   [&](const WrapExpr& e) { renderWrapped(wrapKindName(e.kind), e.inner, level); },
                   [&](const CallExpr& e) { renderCall(e, level); },
                   [&](const TupleExpr& e) { renderBracketed("Tuple", e.elements, level); },
                   [&](const ListExpr& e) { renderBracketed("List", e.elements, level); },
                   [&](const RecordLiteral& e) { renderRecordLiteral(e, level); },
                   [&](const MatchExpr& e) { renderMatch(e, level); },
                   [&](const MapNewExpr& e) { renderMapNew(e, level); },
                   [&](const GenericNewExpr& e) { renderGenericNew(e, level); },
                   [&](const AwaitExpr& e) { renderAwait(e, level); },
                   [&](const SelectExpr& e) { renderSelect(e, level); },
                   [](const auto&) {},
               },
               kind);
}

// Literals, names, and None, each on one line
bool AstDumper::renderLeafExpr(const ExprKind& kind) {
    const auto leaf = [&](std::string_view name, const std::string& payload) {
        write(name);
        write("(");
        write(payload);
        write(")");
        return true;
    };
    return std::visit(
        Overloaded{
            [&](const IntLiteral& e) { return leaf("Int", std::to_string(e.value)); },
            [&](const FloatLiteral& e) { return leaf("Float", "\"" + e.spelling + "\""); },
            [&](const StringLiteral& e) { return leaf("Str", "\"" + e.body + "\""); },
            [&](const CharLiteral& e) {
                return leaf("Char", "'" + canonicalScalar(e.scalar, '\'') + "'");
            },
            [&](const BoolLiteral& e) { return leaf("Bool", e.value ? "true" : "false"); },
            [&](const DurationLiteral& e) {
                return leaf("Duration", std::to_string(e.milliseconds));
            },
            [&](const Identifier& e) { return leaf("Ident", "\"" + e.name + "\""); },
            [&](const NoneExpr&) {
                write("None");
                return true;
            },
            [](const auto&) { return false; },
        },
        kind);
}

void AstDumper::renderUnary(const UnaryExpr& unary, int level) {
    openWrap("Unary", level);
    write(unary.op == UnaryOp::Neg ? "Neg" : "Not");
    endField();
    indent(level + 1);
    renderExpr(unary.operand, level + 1);
    closeWrap(level);
}

void AstDumper::renderBinary(const BinaryExpr& binary, int level) {
    openWrap("Binary", level);
    write(binaryOpName(binary.op));
    endField();
    indent(level + 1);
    renderPair("", binary.lhs, binary.rhs, level);
}

// Two operands as a wrapper, Name(\n a,\n b,\n ). An empty name continues a
// wrapper the caller already opened
void AstDumper::renderPair(std::string_view name, ExprId first, ExprId second, int level) {
    if (!name.empty()) {
        openWrap(name, level);
    }
    renderExpr(first, level + 1);
    endField();
    indent(level + 1);
    renderExpr(second, level + 1);
    closeWrap(level);
}

void AstDumper::renderMember(const MemberExpr& member, int level) {
    openWrap("Member", level);
    renderExpr(member.base, level + 1);
    endField();
    indent(level + 1);
    quoted(member.name);
    closeWrap(level);
}

// A one operand wrapper, Question, Some, Ok, Err, and Mut
void AstDumper::renderWrapped(std::string_view name, ExprId inner, int level) {
    openWrap(name, level);
    renderExpr(inner, level + 1);
    closeWrap(level);
}

// Tuple([ ... ]) and List([ ... ])
void AstDumper::renderBracketed(std::string_view name, const std::vector<ExprId>& exprs,
                                int level) {
    write(name);
    write("(");
    renderExprList(exprs, level);
    write(")");
}

void AstDumper::renderMapNew(const MapNewExpr& mapNew, int level) {
    openWrap("MapNew", level);
    renderType(mapNew.type, level + 1);
    closeWrap(level);
}

void AstDumper::renderExprList(const std::vector<ExprId>& exprs, int level) {
    list(exprs, level, [&](ExprId expr, int inner) { renderExpr(expr, inner); });
}

void AstDumper::renderArgs(const std::vector<Argument>& args, int level) {
    list(args, level, [&](const Argument& arg, int inner) {
        openStruct("Arg");
        key(inner + 1, "label");
        if (arg.label) {
            quoted(*arg.label);
        } else {
            write("None");
        }
        endField();
        key(inner + 1, "expr");
        renderExpr(arg.value, inner + 1);
        endField();
        closeStruct(inner);
    });
}

void AstDumper::renderCall(const CallExpr& call, int level) {
    openStruct("Call");
    key(level + 1, "callee");
    renderExpr(call.callee, level + 1);
    endField();
    key(level + 1, "args");
    renderArgs(call.args, level + 1);
    endField();
    closeStruct(level);
}

void AstDumper::renderRecordLiteral(const RecordLiteral& record, int level) {
    openStruct("RecordLit");
    key(level + 1, "name");
    quoted(record.name);
    endField();
    key(level + 1, "fields");
    list(record.fields, level + 1, [&](const FieldInit& field, int inner) {
        openStruct("FieldInit");
        key(inner + 1, "name");
        quoted(field.name);
        endField();
        key(inner + 1, "expr");
        renderExpr(field.value, inner + 1);
        endField();
        closeStruct(inner);
    });
    endField();
    closeStruct(level);
}

void AstDumper::renderGenericNew(const GenericNewExpr& generic, int level) {
    openStruct("GenericNew");
    key(level + 1, "ty");
    renderType(generic.type, level + 1);
    endField();
    key(level + 1, "args");
    renderArgs(generic.args, level + 1);
    endField();
    closeStruct(level);
}

void AstDumper::renderAwait(const AwaitExpr& await, int level) {
    openStruct("Await");
    key(level + 1, "signal");
    quoted(await.signal);
    endField();
    key(level + 1, "within");
    renderOptionalDuration(await.withinMilliseconds);
    endField();
    closeStruct(level);
}

}  // namespace geas
