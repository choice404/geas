// Integer arithmetic in the emitted C. Every integer operation is checked:
// a sum, difference, product, or negation that leaves the range of its type
// and a division or remainder by zero, or of the least Int by minus one,
// leave the body with the status an index out of range reports, through the
// same exit, so a thunk returns it and a course records it and stops. The
// checks are spelled with the overflow builtins and explicit tests, never
// left to the C compiler's flags. Float follows IEEE and is never checked
#include "emitc.hpp"
#include "emittext.hpp"

namespace geas {

namespace {

// The status an arithmetic fault reports, the one a list index out of range does
constexpr std::string_view arithmeticFault = "GEAS_ERR_TYPE";

// The overflow builtin behind one of + - *, empty for any other operator
std::string_view overflowBuiltin(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add: return "__builtin_add_overflow";
        case BinaryOp::Sub: return "__builtin_sub_overflow";
        case BinaryOp::Mul: return "__builtin_mul_overflow";
        default: return "";
    }
}

// The C operator of one arithmetic operator, spaced
std::string_view arithmeticText(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add: return " + ";
        case BinaryOp::Sub: return " - ";
        case BinaryOp::Mul: return " * ";
        case BinaryOp::Div: return " / ";
        default: return " % ";
    }
}

}  // namespace

/**
 * Lowers one checked arithmetic operation on two scalar operands.
 * @params op - the operator, lhs and rhs - the lowered operands, field - their union field,
 * value - the zeroed result temporary
 */
void CEmitter::genCheckedArithmetic(BinaryOp op, const CValue& lhs, const CValue& rhs,
                                    std::string_view field, std::string_view value) {
    const std::string left = joined(lhs.name, ".as.", field);
    const std::string right = joined(rhs.name, ".as.", field);
    const std::string result = joined(value, ".as.", field);
    // Float follows IEEE, the checker already refused '%' on it
    if (field == "f") {
        line(joined(result, " = ", left, arithmeticText(op), right, ";"));
        return;
    }
    const std::string_view builtin = overflowBuiltin(op);
    if (!builtin.empty()) {
        line(joined("if (", builtin, "(", left, ", ", right, ", &", result, ")) ",
                    failExit(arithmeticFault)));
        return;
    }
    // Division and remainder: a zero divisor faults, and so does the least
    // Int over minus one, whose quotient Int cannot hold
    std::string guard = joined(right, " == 0");
    if (field == "i") {
        guard.append(joined(" || (", right, " == -1 && ", left, " == INT64_MIN)"));
    }
    line(joined("if (", guard, ") ", failExit(arithmeticFault)));
    line(joined(result, " = ", left, arithmeticText(op), right, ";"));
}

/**
 * Lowers a checked negation: an Int or Duration whose negation leaves the range faults.
 * @params operand - the lowered operand, field - its union field, value - the zeroed result
 */
void CEmitter::genCheckedNegation(const CValue& operand, std::string_view field,
                                  std::string_view value) {
    const std::string source = joined(operand.name, ".as.", field);
    const std::string result = joined(value, ".as.", field);
    if (field == "f") {
        line(joined(result, " = -", source, ";"));
        return;
    }
    line(joined("if (__builtin_sub_overflow((int64_t)0, ", source, ", &", result, ")) ",
                failExit(arithmeticFault)));
}

}  // namespace geas
