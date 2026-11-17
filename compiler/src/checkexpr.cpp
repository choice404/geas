// Expression inference. Inference walks the tree once with an expected type
// riding down, unset when the position expects nothing in particular. The
// expected type is what lets an integer literal become UInt or Byte where
// the value fits, lets Some, Ok, Err, and None take their payload types from
// context, and keeps every check exact: there is no implicit conversion
// anywhere, so a mismatch is a fault and never a coercion. A subtree that
// faults infers as the unknown type, which stands anywhere, so one fault
// reads as one diagnostic
#include "checker.hpp"

#include "overloaded.hpp"

namespace geas {

namespace {

constexpr std::int64_t byteMaximum = 255;

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

std::string operatorHead(BinaryOp op, std::string_view tail) {
    return "operator " + quoted(operatorText(op)) + std::string{tail};
}

}  // namespace

// Types one expression and files the answer in the side table
TyId Checker::inferExpr(ExprId id, TyId want) {
    const TyId type = inferKind(ast_.expr(id), want);
    tables_.exprTypes[static_cast<std::size_t>(id.index)] = type;
    const ExprKind& kind = ast_.expr(id).kind;
    if (tables_.exprUses[static_cast<std::size_t>(id.index)] == ExprUse::CoursePledgeCall ||
        std::holds_alternative<AwaitExpr>(kind) || std::holds_alternative<SelectExpr>(kind)) {
        ++suspensionCount_;
    }
    return type;
}

TyId Checker::inferKind(const Expr& expr, TyId want) {
    return std::visit(
        Overloaded{
            [&](const IntLiteral& literal) { return inferInt(expr, literal, want); },
            [&](const FloatLiteral&) { return scalar(TyKind::Float); },
            [&](const StringLiteral&) { return scalar(TyKind::String); },
            [&](const CharLiteral&) { return scalar(TyKind::Char); },
            [&](const BoolLiteral&) { return scalar(TyKind::Bool); },
            [&](const DurationLiteral&) { return scalar(TyKind::Duration); },
            [&](const Identifier& ident) { return inferIdent(expr, ident); },
            [&](const UnaryExpr& unary) { return inferUnary(expr, unary); },
            [&](const BinaryExpr& binary) { return inferBinary(expr, binary, want); },
            [&](const CallExpr& call) { return inferCall(expr, call); },
            [&](const IndexExpr& index) { return inferIndex(expr, index); },
            [&](const MemberExpr& member) { return inferMember(expr, member); },
            [&](const QuestionExpr& question) { return inferQuestion(expr, question); },
            [&](const TupleExpr& tuple) { return inferTuple(tuple, want); },
            [&](const ListExpr& list) { return inferList(list, want); },
            [&](const RecordLiteral& literal) { return inferRecordLiteral(expr, literal); },
            [&](const MatchExpr& match) { return checkMatch(expr, match, want); },
            [&](const WrapExpr& wrap) { return inferWrap(expr, wrap, want); },
            [&](const NoneExpr&) { return inferNone(expr, want); },
            [&](const MapNewExpr& map) { return lowering_.lower(map.type, expr.lo, expr.hi); },
            [&](const GenericNewExpr& ctor) { return inferGenericNew(expr, ctor); },
            [&](const AwaitExpr& await) { return inferAwait(expr, await); },
            [&](const SelectExpr& select) { return checkSelect(expr, select, want); },
        },
        expr.kind);
}

// Int, UInt, Float, and Byte, the unknown recovery type passing
bool Checker::isNumeric(TyId type) const {
    switch (kindOf(type)) {
        case TyKind::Int:
        case TyKind::UInt:
        case TyKind::Float:
        case TyKind::Byte:
        case TyKind::Unknown: return true;
        default: return false;
    }
}

bool Checker::isTemporal(TyId type) const {
    const TyKind kind = kindOf(type);
    return kind == TyKind::Duration || kind == TyKind::Time;
}

// An integer literal types as Int unless the expected type is UInt, or Byte
// with the value in range. The lexer already refused values above the Int
// maximum, so a literal always fits UInt
TyId Checker::inferInt(const Expr& expr, const IntLiteral& literal, TyId want) {
    if (!want.isSet()) {
        return scalar(TyKind::Int);
    }
    const TyKind wanted = kindOf(want);
    if (wanted == TyKind::UInt) {
        return scalar(TyKind::UInt);
    }
    if (wanted != TyKind::Byte) {
        return scalar(TyKind::Int);
    }
    if (literal.value > byteMaximum) {
        emitAt(expr, "integer literal does not fit in 'Byte'");
    }
    return scalar(TyKind::Byte);
}

// A bare name that resolved to a member of the contract being checked
TyId Checker::inferMemberIdent(const Expr& expr, const std::string& name,
                               const MemberEntry& member) {
    switch (member.kind()) {
        case MemberKind::Vow:
            markUse(expr, ExprUse::Vow);
            checkAmendVowRead(expr, name);
            return vowType(*member.vow());
        case MemberKind::Clause:
            emitAt(expr, "clause " + quoted(name) + " is not a value; call it");
            return unknown();
        case MemberKind::Pledge:
            emitAt(expr, isInCourse_ ? "pledge " + quoted(name) + " is not a value; call it"
                                     : "pledge " + quoted(name) +
                                           " is fulfilled through a signed instance");
            return unknown();
        case MemberKind::Subcontract:
            emitAt(expr, quoted(name) + " is a subcontract, not a value");
            return unknown();
        case MemberKind::Signal:
            emitAt(expr, "signal " + quoted(name) +
                             " is not a value; await it in a course or deliver it through a signed "
                             "instance");
            return unknown();
        case MemberKind::Remedy:
            emitAt(expr,
                   "remedy " + quoted(name) + " is not a value; it runs when the contract breaks");
            return unknown();
    }
    return unknown();
}

// A bare name nothing declares. Inside an amends value prior stands for the
// older instance, read one vow at a time as prior.NAME and never whole
TyId Checker::inferUndeclaredIdent(const Expr& expr, const std::string& name) {
    if (isInAmends_ && name == "prior") {
        markUse(expr, ExprUse::PriorInstance);
        emitAt(expr, "prior is the instance of the amended version, not a value; read one of its "
                     "vows as prior.NAME");
        return unknown();
    }
    if (isInCourse_ && resolved_.findPledge(contract_, name) != nullptr) {
        emitAt(expr, "pledge " + quoted(name) + " is not a value; call it");
        return unknown();
    }
    emitAt(expr, "unknown name " + quoted(name));
    return unknown();
}

// A bare variant name, a value of its sum when the variant has no payload
TyId Checker::inferVariantIdent(const Expr& expr, const std::string& name) {
    const VariantSig* variant = sigs_.variant(name);
    if (variant == nullptr) {
        return unknown();
    }
    if (!variant->fields.empty()) {
        emitAt(expr, "variant " + quoted(name) + " takes a payload");
    }
    markUse(expr, ExprUse::VariantValue);
    return types_.named(variant->owner);
}

// A bare name no local and no member claimed, read against the globals
TyId Checker::inferGlobalIdent(const Expr& expr, const std::string& name) {
    const GlobalEntry* global = resolved_.global(name);
    if (global == nullptr) {
        return inferUndeclaredIdent(expr, name);
    }
    switch (global->kind) {
        case GlobalKind::Variant: return inferVariantIdent(expr, name);
        case GlobalKind::Record:
        case GlobalKind::Sum:
            emitAt(expr, quoted(name) + " is a type, not a value");
            return unknown();
        case GlobalKind::Contract:
            emitAt(expr, quoted(name) + " is a contract; sign it with '" + name + ".sign(...)'");
            return unknown();
        case GlobalKind::Provisional:
            emitAt(expr, quoted(name) + " is a provisional clause, not a value");
            return unknown();
    }
    return unknown();
}

TyId Checker::inferIdent(const Expr& expr, const Identifier& ident) {
    const std::string& name = ident.name;
    if (name.empty()) {
        return unknown();
    }
    if (const Binding* binding = lookup(name)) {
        if (binding->isPendingTogether) {
            emitAt(expr, "together arguments cannot use binding " + quoted(name) +
                " before the group completes; bind the earlier call with 'let' before together");
        }
        markUse(expr, ExprUse::Local);
        return binding->type;
    }
    if (contract_ != nullptr) {
        if (const MemberEntry* member = resolved_.member(contract_, name)) {
            return inferMemberIdent(expr, name, *member);
        }
    }
    return inferGlobalIdent(expr, name);
}

TyId Checker::inferUnary(const Expr& expr, const UnaryExpr& unary) {
    const TyId operand = inferExpr(unary.operand, TyId{});
    const TyKind kind = kindOf(operand);
    if (unary.op == UnaryOp::Not) {
        if (kind != TyKind::Bool && kind != TyKind::Unknown) {
            emitAt(expr, "unary '!' needs 'Bool'");
        }
        return scalar(TyKind::Bool);
    }
    const bool isNegatable = kind == TyKind::Int || kind == TyKind::Float ||
                             kind == TyKind::Duration || kind == TyKind::Unknown;
    if (isNegatable) {
        return operand;
    }
    emitAt(expr, "unary '-' needs 'Int' or 'Float'");
    return unknown();
}

TyId Checker::inferLogical(const Expr& expr, const BinaryExpr& binary) {
    const TyId boolType = scalar(TyKind::Bool);
    const TyId left = inferExpr(binary.lhs, boolType);
    const std::size_t beforeRight = suspensionCount_;
    const TyId right = inferExpr(binary.rhs, boolType);
    if (isInCourse_ && suspensionCount_ != beforeRight) {
        emitAt(ast_.expr(binary.rhs), "a suspending operation in the right operand of " +
            quoted(operatorText(binary.op)) + " is not allowed; bind it with 'let' first");
    }
    if (!isCompatible(left, boolType) || !isCompatible(right, boolType)) {
        emitAt(expr, operatorHead(binary.op, " needs 'Bool' operands"));
    }
    return boolType;
}

TyId Checker::inferEquality(const Expr& expr, const BinaryExpr& binary) {
    const TyId left = inferExpr(binary.lhs, TyId{});
    const TyId right = inferExpr(binary.rhs, left);
    if (!isCompatible(left, right)) {
        emitAt(expr, typesMessage(operatorHead(binary.op, " needs matching operands; got "), left,
                                  " and ", right));
    }
    return scalar(TyKind::Bool);
}

// Ordering compares two numbers of one type, two Durations, or two Times
TyId Checker::inferComparison(const Expr& expr, const BinaryExpr& binary) {
    const TyId left = inferExpr(binary.lhs, TyId{});
    const TyId right = inferExpr(binary.rhs, isNumeric(left) ? left : TyId{});
    const auto isOrdered = [&](TyId type) { return isNumeric(type) || isTemporal(type); };
    if (!isOrdered(left) || !isOrdered(right) || !isCompatible(left, right)) {
        emitAt(expr, typesMessage(operatorHead(binary.op, " needs matching numeric operands; got "),
                                  left, " and ", right));
    }
    return scalar(TyKind::Bool);
}

// Arithmetic on time: Duration plus or minus Duration, Duration times Int,
// Time plus or minus Duration, and Time minus Time, which is a Duration
TyId Checker::inferTemporal(const Expr& expr, const BinaryExpr& binary, TyId left) {
    const TyKind leftKind = kindOf(left);
    const BinaryOp op = binary.op;
    const bool isScaling = leftKind == TyKind::Duration && op == BinaryOp::Mul;
    const TyId right = inferExpr(binary.rhs, isScaling ? scalar(TyKind::Int) : TyId{});
    const TyKind rightKind = kindOf(right);
    const bool isShift = op == BinaryOp::Add || op == BinaryOp::Sub;
    if (rightKind == TyKind::Unknown) {
        return unknown();
    }
    if (isShift && rightKind == TyKind::Duration) {
        return left;
    }
    if (isScaling && rightKind == TyKind::Int) {
        return left;
    }
    if (leftKind == TyKind::Time && op == BinaryOp::Sub && rightKind == TyKind::Time) {
        return scalar(TyKind::Duration);
    }
    emitAt(expr, typesMessage(operatorHead(op, " does not apply to "), left, " and ", right));
    return unknown();
}

// '+' concatenates String or Bytes operands of the same type
TyId Checker::inferArithmetic(const Expr& expr, const BinaryExpr& binary, TyId want) {
    TyId operandWant;
    if (want.isSet()) {
        const bool isConcatenation = binary.op == BinaryOp::Add &&
            (kindOf(want) == TyKind::String || kindOf(want) == TyKind::Bytes);
        if (isNumeric(want) || isConcatenation) {
            operandWant = want;
        }
    }
    const TyId left = inferExpr(binary.lhs, operandWant);
    if (binary.op == BinaryOp::Add &&
        (kindOf(left) == TyKind::String || kindOf(left) == TyKind::Bytes)) {
        const TyId right = inferExpr(binary.rhs, left);
        if (!isCompatible(left, right)) {
            emitAt(expr, typesMessage("operator '+' concatenates two " + types_.render(left) +
                                      " values; got ", left,
                                      " and ", right));
        }
        return left;
    }
    if (isTemporal(left)) {
        return inferTemporal(expr, binary, left);
    }
    const TyId right = inferExpr(binary.rhs, isNumeric(left) ? left : TyId{});
    if (kindOf(left) == TyKind::Unknown && isTemporal(right)) {
        return unknown();
    }
    if (!isNumeric(left) || !isNumeric(right) || !isCompatible(left, right)) {
        emitAt(expr, typesMessage(operatorHead(binary.op, " needs matching numeric operands; got "),
                                  left, " and ", right));
        return unknown();
    }
    return kindOf(left) == TyKind::Unknown ? right : left;
}

TyId Checker::inferBinary(const Expr& expr, const BinaryExpr& binary, TyId want) {
    switch (binary.op) {
        case BinaryOp::And:
        case BinaryOp::Or: return inferLogical(expr, binary);
        case BinaryOp::Eq:
        case BinaryOp::Ne: return inferEquality(expr, binary);
        case BinaryOp::Lt:
        case BinaryOp::Le:
        case BinaryOp::Gt:
        case BinaryOp::Ge: return inferComparison(expr, binary);
        default: return inferArithmetic(expr, binary, want);
    }
}

// '?' unwraps an Option or a Result. In a pledge or clause the enclosing
// return type has to be able to carry the early exit; in a course the '?'
// just ends the course, so any Option or Result may take it
TyId Checker::inferQuestion(const Expr& expr, const QuestionExpr& question) {
    const TyId operand = inferExpr(question.operand, TyId{});
    const TyNode node = types_.node(operand);
    const TyId ret = returnType_.isSet() ? returnType_ : unknown();
    const TyNode returned = types_.node(ret);
    if (node.kind == TyKind::Option) {
        const bool canCarry = returned.kind == TyKind::Option || returned.kind == TyKind::Unknown;
        if (!canCarry && !isInCourse_) {
            emitAt(expr, "'?' on an Option needs the enclosing body to return Option");
        }
        return node.args[0];
    }
    if (node.kind == TyKind::Result) {
        if (isInCourse_) {
            return node.args[0];
        }
        if (returned.kind == TyKind::Result) {
            if (!isCompatible(node.args[1], returned.args[1])) {
                emitAt(expr, typesMessage("'?' propagates error type ", node.args[1],
                                          " but the body returns error type ", returned.args[1]));
            }
        } else if (returned.kind != TyKind::Unknown) {
            emitAt(expr, "'?' on a Result needs the enclosing body to return Result");
        }
        return node.args[0];
    }
    if (node.kind == TyKind::Unknown) {
        return unknown();
    }
    emitAt(expr, "'?' needs an Option or a Result value");
    return unknown();
}

// A List takes an Int and yields its element; a Map takes its key type and
// yields Option of its value type, so a miss is a value and never a fault
TyId Checker::inferIndex(const Expr& expr, const IndexExpr& index) {
    const TyId base = inferExpr(index.base, TyId{});
    const TyNode node = types_.node(base);
    if (node.kind == TyKind::Map) {
        const TyId key = inferExpr(index.index, node.args[0]);
        if (!isCompatible(node.args[0], key)) {
            emitAt(expr, typesMessage("this map is keyed by ", node.args[0], ", got ", key));
        }
        return types_.option(node.args[1]);
    }
    const TyId intType = scalar(TyKind::Int);
    const TyId position = inferExpr(index.index, intType);
    if (node.kind == TyKind::Bytes) {
        if (!isCompatible(position, intType)) {
            emitAt(expr, typesMessage("a Bytes index must be ", intType, ", got ", position));
        }
        return scalar(TyKind::Byte);
    }
    if (node.kind == TyKind::List) {
        if (!isCompatible(position, intType)) {
            emitAt(expr, typesMessage("a list index must be ", intType, ", got ", position));
        }
        return node.args[0];
    }
    if (node.kind == TyKind::Unknown) {
        return unknown();
    }
    emitAt(expr, "cannot index a value of type " + quoted(types_.render(base)) +
                     "; only a List, Map, or Bytes is indexed");
    return unknown();
}

TyId Checker::inferTuple(const TupleExpr& tuple, TyId want) {
    TyNode wanted;
    if (want.isSet()) {
        wanted = types_.node(want);
    }
    const bool hasShape =
        wanted.kind == TyKind::Tuple && wanted.args.size() == tuple.elements.size();
    std::vector<TyId> elements;
    for (std::size_t i = 0; i < tuple.elements.size(); ++i) {
        elements.push_back(inferExpr(tuple.elements[i], hasShape ? wanted.args[i] : TyId{}));
    }
    return types_.tuple(std::move(elements));
}

// The first element sets the type the rest must share, unless the expected
// type already named it
TyId Checker::inferList(const ListExpr& list, TyId want) {
    TyId element;
    if (want.isSet() && kindOf(want) == TyKind::List) {
        element = types_.node(want).args[0];
    }
    for (const ExprId item : list.elements) {
        const TyId actual = inferExpr(item, element);
        if (!element.isSet()) {
            element = actual;
        } else if (!isCompatible(element, actual)) {
            emitAt(ast_.expr(item), typesMessage("list elements disagree in type; got ", actual,
                                                 " beside ", element));
        }
    }
    return types_.list(element.isSet() ? element : unknown());
}

}  // namespace geas
