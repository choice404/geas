// Lowering the match expression to C. The scrutinee lowers once into a
// temporary, the result rides an out temporary the arms assign, and the
// arms become a nested if else chain, first arm outermost, so tests run
// lazily in source order. Each refutable arm's test is a do while zero
// block: the pattern walks top down, every failing check breaks out, and
// only a walk that reaches the end raises the arm's hit flag. Pattern code
// contains no user statements, so that break can never capture a loop break
// written in an arm body; the bodies sit in plain if blocks where break and
// continue still mean the enclosing loop. Payload bindings hoist as zeroed
// GeasValue locals above the test so the body reads them in scope. A match
// nothing catches leaves the zeroed Unit result, a case a checked program
// cannot reach
#include "emitc.hpp"
#include "emittext.hpp"
#include "overloaded.hpp"

namespace geas {

// Whether a bind pattern names a variant of the scrutinee's sum, the one
// case where a bare identifier pattern tests instead of binding
bool CEmitter::isVariantBind(std::string_view name, TyId scrutinee) const {
    if (lookup_.namedKind(scrutinee) != GlobalKind::Sum) {
        return false;
    }
    const std::optional<VariantSlot> variant = lookup_.variantOf(name);
    return variant && variant->sig->owner == types().nameOf(scrutinee);
}

// Whether the pattern matches every value of its type, which lets the arm
// skip the test machinery entirely
bool CEmitter::isIrrefutable(PatternId id, TyId scrutinee) const {
    const Pattern& pattern = arena().pattern(id);
    if (std::holds_alternative<WildcardPattern>(pattern)) {
        return true;
    }
    const auto* bind = std::get_if<BindPattern>(&pattern);
    return bind != nullptr && !isVariantBind(bind->name, scrutinee);
}

// The payload type a Some, Ok, or Err pattern opens, unknown when the
// scrutinee is not the wrapper the pattern names
TyId CEmitter::wrapInner(WrapKind kind, TyId type) const {
    const TyNode node = nodeOf(type);
    if (kind == WrapKind::Some) {
        return node.kind == TyKind::Option ? node.args[0] : unknown();
    }
    if (node.kind != TyKind::Result) {
        return unknown();
    }
    return node.args[kind == WrapKind::Err ? 1 : 0];
}

// Collects the bindings a pattern introduces, in the same top down left to
// right order the test consumes them, each given a fresh C local
void CEmitter::collectBinds(PatternId id, TyId scrutinee, std::vector<PatternBind>& binds) {
    const Pattern& pattern = arena().pattern(id);
    if (const auto* bind = std::get_if<BindPattern>(&pattern)) {
        if (!isVariantBind(bind->name, scrutinee)) {
            binds.push_back(PatternBind{bind->name, freshName(), scrutinee});
        }
        return;
    }
    if (const auto* wrap = std::get_if<WrapPattern>(&pattern)) {
        collectBinds(wrap->inner, wrapInner(wrap->kind, scrutinee), binds);
        return;
    }
    const auto* variant = std::get_if<VariantPattern>(&pattern);
    if (variant == nullptr) {
        return;
    }
    const std::optional<VariantSlot> slot = lookup_.variantOf(variant->name);
    for (std::size_t i = 0; i < variant->fields.size(); ++i) {
        const bool hasField = slot && i < slot->sig->fields.size();
        collectBinds(variant->fields[i], hasField ? slot->sig->fields[i].type : unknown(), binds);
    }
}

// A literal pattern: the literal lowers against the scrutinee's type and
// the two compare on their union field, as Strings, or structurally
void CEmitter::genLiteralTest(const LiteralPattern& pattern, std::string_view value, TyId type) {
    const CValue literal = genExpr(pattern.literal, type);
    const std::string_view field = lookup_.unionFieldOf(type);
    if (!field.empty()) {
        line(joined("if (!(", value, ".as.", field, " == ", literal.name, ".as.", field,
                    ")) break;"));
        return;
    }
    const std::string_view helper =
        kindOf(type) == TyKind::String ? "geas_string_eq" : "geas_value_eq";
    line(joined("if (!", helper, "(&", value, ", &", literal.name, ")) break;"));
}

// Some(p), Ok(p), Err(p): the tag check, then the payload lifted out of the
// box for the inner pattern
std::size_t CEmitter::genWrapTest(const WrapPattern& wrap, std::string_view value, TyId type,
                                  const std::vector<PatternBind>& binds, std::size_t cursor) {
    const std::string_view wantedTag = wrap.kind == WrapKind::Ok ? "0" : "1";
    line(joined("if (", value, ".tag != ", wantedTag, ") break;"));
    const TyId inner = wrapInner(wrap.kind, type);
    const std::string payload = freshName();
    line(joined("GeasValue ", payload, " = *(const GeasValue*)", value, ".as.box;"));
    return genPatternTest(wrap.inner, payload, inner, binds, cursor);
}

// A variant pattern with a payload list: the tag check, then each payload
// slot against its own pattern
std::size_t CEmitter::genVariantTest(const VariantPattern& pattern, std::string_view value,
                                     const std::vector<PatternBind>& binds, std::size_t cursor) {
    const std::optional<VariantSlot> slot = lookup_.variantOf(pattern.name);
    if (!slot) {
        lookup_.unsupported(spanLo_, spanHi_,
                            joined("the pattern '", pattern.name, "' in a pledge body"));
        return cursor;
    }
    line(joined("if (", value, ".tag != ", decimal(slot->index), ") break;"));
    for (std::size_t i = 0; i < pattern.fields.size(); ++i) {
        const TyId type = i < slot->sig->fields.size() ? slot->sig->fields[i].type : unknown();
        const std::string payload = freshName();
        line(joined("GeasValue ", payload, " = ((const GeasValue*)", value, ".as.list.data)[",
                    decimal(static_cast<std::int64_t>(i)), "];"));
        cursor = genPatternTest(pattern.fields[i], payload, type, binds, cursor);
    }
    return cursor;
}

// Emits the checks of one pattern against a value inside the arm's do while
// block: a failed check breaks out, a passed one falls through, and
// bindings assign into their hoisted locals on the way down. cursor walks
// binds and the walk returns where it left it
std::size_t CEmitter::genPatternTest(PatternId id, std::string_view value, TyId type,
                                     const std::vector<PatternBind>& binds, std::size_t cursor) {
    return std::visit(
        Overloaded{
            [&](const WildcardPattern&) { return cursor; },
            [&](const BindPattern& bind) {
                if (isVariantBind(bind.name, type)) {
                    const std::int64_t tag = lookup_.variantOf(bind.name)->index;
                    line(joined("if (", value, ".tag != ", decimal(tag), ") break;"));
                    return cursor;
                }
                line(joined(binds[cursor].variable, " = ", value, ";"));
                return cursor + 1;
            },
            [&](const LiteralPattern& literal) {
                genLiteralTest(literal, value, type);
                return cursor;
            },
            [&](const NonePattern&) {
                line(joined("if (", value, ".tag != 0) break;"));
                return cursor;
            },
            [&](const WrapPattern& wrap) { return genWrapTest(wrap, value, type, binds, cursor); },
            [&](const VariantPattern& variant) {
                return genVariantTest(variant, value, binds, cursor);
            },
        },
        arena().pattern(id));
}

// Lowers one arm body with its bindings in scope, assigning the arm's value
// into the match's result temporary. Returns the body's checked value type,
// unset when a block body ends in anything but an expression
TyId CEmitter::genArmBody(const MatchArm& arm, const std::vector<PatternBind>& binds,
                          std::string_view result, TyId want) {
    pushScope();
    for (const PatternBind& bind : binds) {
        declare(bind.name, bind.variable, bind.type);
    }
    TyId type;
    if (const auto* block = std::get_if<BlockId>(&arm.body)) {
        type = genBlockInto(*block, want, result);
    } else {
        const CValue value = genExpr(std::get<ExprId>(arm.body), want);
        line(joined(result, " = ", value.name, ";"));
        type = value.type;
    }
    popScope();
    return type;
}

// Lowers the arms from index on as a nested if else chain and returns the
// value type of the first arm it lowered, the type later arms are held to
// when the match position asked for nothing
TyId CEmitter::genArms(const MatchExpr& match, const CValue& scrutinee, std::string_view result,
                       std::size_t index, TyId want) {
    if (index >= match.arms.size()) {
        return TyId{};
    }
    const MatchArm& arm = match.arms[index];
    std::vector<PatternBind> binds;
    if (isIrrefutable(arm.pattern, scrutinee.type)) {
        if (const auto* bind = std::get_if<BindPattern>(&arena().pattern(arm.pattern))) {
            binds.push_back(PatternBind{bind->name, scrutinee.name, scrutinee.type});
        }
        return genArmBody(arm, binds, result, want);
    }
    collectBinds(arm.pattern, scrutinee.type, binds);
    for (const PatternBind& bind : binds) {
        line(joined("GeasValue ", bind.variable, " = {0};"));
    }
    const std::string hit = freshName();
    line(joined("int ", hit, " = 0;"));
    line("do {");
    ++indent_;
    genPatternTest(arm.pattern, scrutinee.name, scrutinee.type, binds, 0);
    line(joined(hit, " = 1;"));
    --indent_;
    line("} while (0);");
    line(joined("if (", hit, ") {"));
    ++indent_;
    const TyId first = genArmBody(arm, binds, result, want);
    --indent_;
    if (index + 1 < match.arms.size()) {
        line("} else {");
        ++indent_;
        genArms(match, scrutinee, result, index + 1, want.isSet() ? want : first);
        --indent_;
    }
    line("}");
    return first;
}

// The match expression. The result temporary starts zeroed, Unit, which is
// also what a statement position match leaves behind when its arms end in
// statements rather than values
CValue CEmitter::genMatch(const MatchExpr& match, TyId want) {
    const CValue scrutinee = genExpr(match.scrutinee, TyId{});
    std::string result = newValue();
    const TyId first = genArms(match, scrutinee, result, 0, want);
    TyId type = want.isSet() ? want : first;
    if (!type.isSet()) {
        type = unknown();
    }
    return CValue{std::move(result), type};
}

}  // namespace geas
