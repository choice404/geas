// Lowering one expression to C. Every expression becomes a fresh GeasValue
// temporary built by a few statements, and the lowering returns the
// temporary's name beside its checked type. The type questions were all
// settled by the checker before code generation runs; the types read here
// pick union fields and runtime helpers, they never report faults. The
// expected type rides down as want, unset when the position asks for
// nothing, and decides what the checker decided with it: an integer literal
// is Int unless the position asked for UInt or Byte, and an empty Option or
// list takes its shape from the position
#include "emitc.hpp"
#include "emittext.hpp"
#include "overloaded.hpp"

namespace geas {

namespace {

std::string_view operatorText(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add: return "+";
        case BinaryOp::Sub: return "-";
        case BinaryOp::Mul: return "*";
        case BinaryOp::Div: return "/";
        case BinaryOp::Mod: return "%";
        case BinaryOp::Eq: return "==";
        case BinaryOp::Ne: return "!=";
        case BinaryOp::Lt: return "<";
        case BinaryOp::Le: return "<=";
        case BinaryOp::Gt: return ">";
        case BinaryOp::Ge: return ">=";
        case BinaryOp::And: return "&&";
        case BinaryOp::Or: return "||";
    }
    return "||";
}

bool isEquality(BinaryOp op) {
    return op == BinaryOp::Eq || op == BinaryOp::Ne;
}

bool isOrdering(BinaryOp op) {
    return op == BinaryOp::Lt || op == BinaryOp::Le || op == BinaryOp::Gt || op == BinaryOp::Ge;
}

}  // namespace

CValue CEmitter::genExpr(ExprId id, TyId want) {
    const Expr& expr = ast().expr(id);
    spanLo_ = expr.lo;
    spanHi_ = expr.hi;
    return std::visit(
        Overloaded{
            [&](const IntLiteral& e) { return genInt(e, want); },
            [&](const FloatLiteral& e) {
                return genScalar("GEAS_TY_FLOAT", "f", e.spelling, TyKind::Float);
            },
            [&](const StringLiteral& e) { return genString(e); },
            [&](const CharLiteral& e) {
                return genScalar("GEAS_TY_CHAR", "ch", decimal(e.scalar), TyKind::Char);
            },
            [&](const BoolLiteral& e) {
                return genScalar("GEAS_TY_BOOL", "b", e.value ? "1" : "0", TyKind::Bool);
            },
            [&](const Identifier& e) { return genIdentifier(expr, e); },
            [&](const UnaryExpr& e) { return genUnary(expr, e); },
            [&](const BinaryExpr& e) { return genBinary(expr, e, want); },
            [&](const CallExpr& e) { return genCall(expr, e); },
            [&](const IndexExpr& e) { return genIndex(expr, e); },
            [&](const MemberExpr& e) { return genMember(expr, e); },
            [&](const QuestionExpr& e) { return genQuestion(expr, e); },
            [&](const TupleExpr& e) { return genTuple(e, want); },
            [&](const ListExpr& e) { return genList(e, want); },
            [&](const RecordLiteral& e) { return genRecord(expr, e); },
            [&](const MatchExpr& e) { return genMatch(e, want); },
            [&](const WrapExpr& e) { return genWrap(e, want); },
            [&](const NoneExpr&) { return genNone(want); },
            [&](const MapNewExpr& e) { return genMapNew(expr, e); },
            [&](const auto&) { return genWorkflowExpr(expr); },
        },
        expr.kind);
}

// An integer literal, typed the checker's way: Int unless the position asked
// for UInt or Byte
CValue CEmitter::genInt(const IntLiteral& literal, TyId want) {
    const TyKind wanted = kindOf(want);
    const TyKind kind = wanted == TyKind::UInt || wanted == TyKind::Byte ? wanted : TyKind::Int;
    const TyId type = types().scalar(kind);
    return genScalar(lookup_.tagOf(type), lookup_.unionFieldOf(type), decimal(literal.value), kind);
}

// A scalar literal: a fresh value, its tag, and its one union field
CValue CEmitter::genScalar(std::string_view tag, std::string_view field, std::string_view text,
                           TyKind kind) {
    std::string value = newValue();
    setTag(value, tag);
    line(joined(value, ".as.", field, " = ", text, ";"));
    return CValue{std::move(value), types().scalar(kind)};
}

CValue CEmitter::genString(const StringLiteral& literal) {
    const CString lowered = cString(literal.body);
    std::string value = newValue();
    setTag(value, "GEAS_TY_STRING");
    line(joined(value, ".as.s.ptr = (uint8_t*)", lowered.literal, ";"));
    line(joined(value, ".as.s.len = ", decimal(lowered.length), ";"));
    return CValue{std::move(value), types().scalar(TyKind::String)};
}

// A parameter, a let binding, a vow of the enclosing contract, or a bare sum
// variant, which the parser leaves as an identifier on purpose
CValue CEmitter::genIdentifier(const Expr& expr, const Identifier& identifier) {
    if (const CValue* local = lookupLocal(identifier.name)) {
        return *local;
    }
    const MemberEntry* member = lookup_.resolved().member(shape_->decl, identifier.name);
    if (member != nullptr && member->kind() == MemberKind::Vow) {
        return genVowRead(*member->vow());
    }
    const GlobalEntry* global = lookup_.resolved().global(identifier.name);
    if (global != nullptr && global->kind == GlobalKind::Variant) {
        return genVariant(expr, identifier.name, {});
    }
    unsupportedAt(expr, joined("the name '", identifier.name, "' in a pledge body"));
    return badValue();
}

// A vow of the enclosing contract read through the instance
CValue CEmitter::genVowRead(const VowDecl& vow) {
    const std::string borrowed = freshName();
    line(joined("const GeasValue* ", borrowed, " = geas_vow_ref(cc, \"", vow.name, "\");"));
    line(joined("if (!", borrowed, ") return GEAS_ERR_NAME;"));
    std::string value = freshName();
    line(joined("GeasValue ", value, " = *", borrowed, ";"));
    return CValue{std::move(value), lookup_.vowType(vow)};
}

CValue CEmitter::genUnary(const Expr& expr, const UnaryExpr& unary) {
    const CValue operand = genExpr(unary.operand, TyId{});
    if (unary.op == UnaryOp::Not) {
        std::string value = newValue();
        setTag(value, "GEAS_TY_BOOL");
        line(joined(value, ".as.b = !", operand.name, ".as.b;"));
        return CValue{std::move(value), types().scalar(TyKind::Bool)};
    }
    const std::string_view field = lookup_.unionFieldOf(operand.type);
    if (field.empty()) {
        unsupportedAt(expr, "unary '-' on this type");
        return badValue();
    }
    std::string value = newValue();
    setTag(value, lookup_.tagOf(operand.type));
    line(joined(value, ".as.", field, " = -", operand.name, ".as.", field, ";"));
    return CValue{std::move(value), operand.type};
}

// String concatenation through the runtime helper, which allocates the
// result on the instance the thunk runs against
CValue CEmitter::genConcat(const CValue& lhs, const CValue& rhs) {
    std::string value = newValue();
    setTag(value, "GEAS_TY_STRING");
    line(joined("if (geas_string_concat(cc, &", lhs.name, ", &", rhs.name, ", &", value,
                ") != GEAS_OK) return GEAS_ERR_OOM;"));
    return CValue{std::move(value), types().scalar(TyKind::String)};
}

// Short circuit: the left operand lowers, then the right lowers inside a
// guard so its statements only run when the left has not already decided
// the answer
CValue CEmitter::genShortCircuit(const BinaryExpr& binary) {
    const TyId boolean = types().scalar(TyKind::Bool);
    const CValue lhs = genExpr(binary.lhs, boolean);
    std::string value = newValue();
    setTag(value, "GEAS_TY_BOOL");
    line(joined(value, ".as.b = ", lhs.name, ".as.b;"));
    line(joined("if (", binary.op == BinaryOp::And ? "" : "!", value, ".as.b) {"));
    ++indent_;
    const CValue rhs = genExpr(binary.rhs, boolean);
    line(joined(value, ".as.b = ", rhs.name, ".as.b;"));
    --indent_;
    line("}");
    return CValue{std::move(value), boolean};
}

CValue CEmitter::genBinary(const Expr& expr, const BinaryExpr& binary, TyId want) {
    const BinaryOp op = binary.op;
    if (op == BinaryOp::And || op == BinaryOp::Or) {
        return genShortCircuit(binary);
    }
    // Arithmetic operands take the position's type, a comparison's take none
    const bool takesWant =
        want.isSet() && kindOf(want) != TyKind::Bool && !isEquality(op) && !isOrdering(op);
    const CValue lhs = genExpr(binary.lhs, takesWant ? want : TyId{});
    const CValue rhs = genExpr(binary.rhs, lhs.type);
    const TyId operand = kindOf(lhs.type) == TyKind::Unknown ? rhs.type : lhs.type;
    if (op == BinaryOp::Add && kindOf(operand) == TyKind::String) {
        return genConcat(lhs, rhs);
    }
    if (isEquality(op)) {
        return genEquality(expr, binary, lhs, rhs, operand);
    }
    const std::string_view field = lookup_.unionFieldOf(operand);
    if (field.empty()) {
        unsupportedAt(expr, joined("operator '", operatorText(op), "' on this type"));
        return badValue();
    }
    if (isOrdering(op)) {
        return genCompare(binary, lhs, rhs, field);
    }
    if (op == BinaryOp::Mod && field == "f") {
        unsupportedAt(expr, "'%' on Float");
        return badValue();
    }
    std::string value = newValue();
    setTag(value, lookup_.tagOf(operand));
    line(joined(value, ".as.", field, " = ", lhs.name, ".as.", field, " ", operatorText(op), " ",
                rhs.name, ".as.", field, ";"));
    // Time minus Time is the Duration between them, every other temporal
    // pairing keeps the left operand's type
    const bool isSpan = op == BinaryOp::Sub && kindOf(lhs.type) == TyKind::Time &&
                        kindOf(rhs.type) == TyKind::Time;
    return CValue{std::move(value), isSpan ? types().scalar(TyKind::Duration) : operand};
}

// Equality: Strings through geas_string_eq, composites and instance handles
// through the structural geas_value_eq, scalars on their union field
CValue CEmitter::genEquality(const Expr& expr, const BinaryExpr& binary, const CValue& lhs,
                             const CValue& rhs, TyId operand) {
    std::string value = newValue();
    setTag(value, "GEAS_TY_BOOL");
    const std::string_view bang = binary.op == BinaryOp::Ne ? "!" : "";
    const TyId boolean = types().scalar(TyKind::Bool);
    const std::string_view field = lookup_.unionFieldOf(operand);
    const bool isString = kindOf(operand) == TyKind::String;
    const bool isDeep = kindOf(operand) == TyKind::Contract || isComposite(operand);
    if (isString || (field.empty() && isDeep)) {
        const std::string_view helper = isString ? "geas_string_eq(&" : "geas_value_eq(&";
        line(joined(value, ".as.b = ", bang, helper, lhs.name, ", &", rhs.name, ");"));
        return CValue{std::move(value), boolean};
    }
    if (field.empty()) {
        unsupportedAt(expr, "'==' on this type");
        return badValue();
    }
    line(joined(value, ".as.b = (", lhs.name, ".as.", field, " ", operatorText(binary.op), " ",
                rhs.name, ".as.", field, ");"));
    return CValue{std::move(value), boolean};
}

CValue CEmitter::genCompare(const BinaryExpr& binary, const CValue& lhs, const CValue& rhs,
                            std::string_view field) {
    std::string value = newValue();
    setTag(value, "GEAS_TY_BOOL");
    line(joined(value, ".as.b = (", lhs.name, ".as.", field, " ", operatorText(binary.op), " ",
                rhs.name, ".as.", field, ");"));
    return CValue{std::move(value), types().scalar(TyKind::Bool)};
}

// Map<K, V>(): an empty map value carrying its key tag. Nothing is
// allocated, so the helper returns the value directly
CValue CEmitter::genMapNew(const Expr& expr, const MapNewExpr& ctor) {
    const TyId type = lowering_.lower(ctor.type, expr.lo, expr.hi);
    const TyNode node = nodeOf(type);
    const TyId key = node.args.empty() ? TyId{} : node.args[0];
    std::string value = freshName();
    line(joined("GeasValue ", value, " = geas_map_new(cc, ", lookup_.tagOf(key), ");"));
    return CValue{std::move(value), type};
}

}  // namespace geas
