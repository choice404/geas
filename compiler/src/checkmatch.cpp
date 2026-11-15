// The match expression. The scrutinee type drives every pattern check: a
// Some pattern needs an Option, a variant pattern needs its own sum, a
// literal pattern must type as the scrutinee does. Arms are checked for
// exhaustiveness over sums, Option, Result, and Bool; an open type such as
// Int needs a wildcard or binding arm. A constructor arm counts toward
// coverage only when every subpattern is irrefutable. Arms must agree in
// type: with an expected type each arm is held to it, and without one the
// first arm sets the type the rest must share. Patterns carry no spans of
// their own, so pattern faults land on the span of the whole expression
#include "checker.hpp"

namespace geas {

namespace {

bool isIrrefutable(const Pattern& pattern) {
    return std::holds_alternative<WildcardPattern>(pattern) ||
           std::holds_alternative<BindPattern>(pattern);
}

}  // namespace

// Whether a type is a declared sum
bool Checker::isSumType(TyId type) const {
    if (kindOf(type) != TyKind::Named) {
        return false;
    }
    const GlobalEntry* global = resolved_.global(types_.nameOf(type));
    return global != nullptr && global->kind == GlobalKind::Sum;
}

// Whether name is a variant belonging to the sum the type names
bool Checker::isVariantOf(TyId type, std::string_view name) const {
    const VariantSig* variant = sigs_.variant(name);
    return variant != nullptr && variant->owner == types_.nameOf(type);
}

// A bare name in a pattern: a payload free variant of the scrutinee's sum,
// or a fresh binding. Returns whether it is irrefutable
bool Checker::bindName(const BindPattern& bind, TyId type, const Expr& at) {
    if (isSumType(type) && isVariantOf(type, bind.name)) {
        if (!sigs_.variant(bind.name)->fields.empty()) {
            emitAt(at, "variant " + quoted(bind.name) + " carries a payload; match it as '" +
                           bind.name + "(...)'");
        }
        return false;
    }
    declare(bind.name, type, false);
    return true;
}

// Some(p), Ok(p), Err(p) against the Option or Result they open
void Checker::bindWrap(const WrapPattern& wrap, TyId type, const Expr& at) {
    const TyNode node = types_.node(type);
    const bool isSome = wrap.kind == WrapKind::Some;
    const TyKind needed = isSome ? TyKind::Option : TyKind::Result;
    if (node.kind == needed) {
        const std::size_t slot = wrap.kind == WrapKind::Err ? 1 : 0;
        bindPattern(wrap.inner, node.args[slot], at);
        return;
    }
    if (node.kind != TyKind::Unknown) {
        const std::string_view word = isSome                      ? "'Some'"
                                      : wrap.kind == WrapKind::Ok ? "'Ok'"
                                                                  : "'Err'";
        const std::string_view wanted = isSome ? "an Option" : "a Result";
        emitAt(at, std::string{word} + " pattern needs " + std::string{wanted} +
                       " scrutinee, got " + quoted(types_.render(type)));
    }
    bindPattern(wrap.inner, unknown(), at);
}

// A variant pattern with a payload list against the sum it belongs to
void Checker::bindVariant(const VariantPattern& pattern, TyId type, const Expr& at) {
    const std::string& name = pattern.name;
    const auto bindAllUnknown = [&] {
        for (const PatternId field : pattern.fields) {
            bindPattern(field, unknown(), at);
        }
    };
    if (!isSumType(type)) {
        if (kindOf(type) != TyKind::Unknown) {
            emitAt(at, "variant pattern " + quoted(name) + " needs a sum scrutinee, got " +
                           quoted(types_.render(type)));
        }
        return bindAllUnknown();
    }
    if (!isVariantOf(type, name)) {
        emitAt(at, quoted(name) + " is not a variant of " + quoted(types_.nameOf(type)));
        return bindAllUnknown();
    }
    const std::vector<FieldSig>& fields = sigs_.variant(name)->fields;
    if (pattern.fields.size() != fields.size()) {
        emitAt(at, "variant " + quoted(name) + " carries " +
                       countText(static_cast<std::int64_t>(fields.size())) + " fields, got " +
                       countText(static_cast<std::int64_t>(pattern.fields.size())));
    }
    for (std::size_t i = 0; i < pattern.fields.size(); ++i) {
        bindPattern(pattern.fields[i], i < fields.size() ? fields[i].type : unknown(), at);
    }
}

// Binds and type checks one pattern against the type it matches. Returns
// whether the pattern is irrefutable, a wildcard or a plain binding
bool Checker::bindPattern(PatternId id, TyId type, const Expr& at) {
    const Pattern& pattern = ast_.pattern(id);
    if (std::holds_alternative<WildcardPattern>(pattern)) {
        return true;
    }
    if (const auto* bind = std::get_if<BindPattern>(&pattern)) {
        return bindName(*bind, type, at);
    }
    if (const auto* literal = std::get_if<LiteralPattern>(&pattern)) {
        const TyId actual = inferExpr(literal->literal, type);
        if (!isCompatible(type, actual)) {
            emitAt(at, typesMessage("pattern literal has type ", actual, " but the scrutinee is ",
                                    type));
        }
        return false;
    }
    if (const auto* wrap = std::get_if<WrapPattern>(&pattern)) {
        bindWrap(*wrap, type, at);
        return false;
    }
    if (std::holds_alternative<NonePattern>(pattern)) {
        const TyKind kind = kindOf(type);
        if (kind != TyKind::Option && kind != TyKind::Unknown) {
            emitAt(at,
                   "'None' pattern needs an Option scrutinee, got " + quoted(types_.render(type)));
        }
        return false;
    }
    bindVariant(std::get<VariantPattern>(pattern), type, at);
    return false;
}

// Whether every subpattern of a constructor pattern is irrefutable, the
// condition for the arm to count toward coverage of its constructor
bool Checker::hasIrrefutableFields(const Pattern& pattern) const {
    if (const auto* wrap = std::get_if<WrapPattern>(&pattern)) {
        return isIrrefutable(ast_.pattern(wrap->inner));
    }
    if (const auto* variant = std::get_if<VariantPattern>(&pattern)) {
        for (const PatternId field : variant->fields) {
            if (!isIrrefutable(ast_.pattern(field))) {
                return false;
            }
        }
    }
    return true;
}

// Records what one arm's pattern covers, after bindPattern checked it
void Checker::coverPattern(PatternId id, TyId scrutinee, Cover& cover) const {
    const Pattern& pattern = ast_.pattern(id);
    if (std::holds_alternative<WildcardPattern>(pattern)) {
        cover.hasCatchAll = true;
        return;
    }
    if (const auto* bind = std::get_if<BindPattern>(&pattern)) {
        if (!isSumType(scrutinee) || !isVariantOf(scrutinee, bind->name)) {
            cover.hasCatchAll = true;
        } else if (sigs_.variant(bind->name)->fields.empty()) {
            cover.variants.insert(bind->name);
        }
        return;
    }
    if (const auto* literal = std::get_if<LiteralPattern>(&pattern)) {
        if (const auto* value = std::get_if<BoolLiteral>(&ast_.expr(literal->literal).kind)) {
            (value->value ? cover.hasTrue : cover.hasFalse) = true;
        }
        return;
    }
    if (!hasIrrefutableFields(pattern)) {
        return;
    }
    if (const auto* wrap = std::get_if<WrapPattern>(&pattern)) {
        cover.hasSome = cover.hasSome || wrap->kind == WrapKind::Some;
        cover.hasOk = cover.hasOk || wrap->kind == WrapKind::Ok;
        cover.hasErr = cover.hasErr || wrap->kind == WrapKind::Err;
    } else if (std::holds_alternative<NonePattern>(pattern)) {
        cover.hasNone = true;
    } else if (const auto* variant = std::get_if<VariantPattern>(&pattern)) {
        if (isVariantOf(scrutinee, variant->name)) {
            cover.variants.insert(variant->name);
        }
    }
}

void Checker::checkExhaustive(TyId scrutinee, const Cover& cover, const Expr& at) {
    if (cover.hasCatchAll) {
        return;
    }
    const auto missing = [&](bool isCovered, std::string_view message) {
        if (!isCovered) {
            emitAt(at, std::string{message});
        }
    };
    switch (kindOf(scrutinee)) {
        case TyKind::Unknown: return;
        case TyKind::Bool:
            missing(cover.hasTrue, "match on Bool is missing 'true'");
            missing(cover.hasFalse, "match on Bool is missing 'false'");
            return;
        case TyKind::Option:
            missing(cover.hasSome, "match is missing 'Some(_)'");
            missing(cover.hasNone, "match is missing 'None'");
            return;
        case TyKind::Result:
            missing(cover.hasOk, "match is missing 'Ok(_)'");
            missing(cover.hasErr, "match is missing 'Err(_)'");
            return;
        default: break;
    }
    checkSumCovered(scrutinee, cover, at);
}

// A sum needs an arm for every variant, any other open type a catch all
void Checker::checkSumCovered(TyId scrutinee, const Cover& cover, const Expr& at) {
    if (!isSumType(scrutinee)) {
        emitAt(at,
               "match over " + quoted(types_.render(scrutinee)) + " needs a '_' or binding arm");
        return;
    }
    const GlobalEntry* global = resolved_.global(types_.nameOf(scrutinee));
    const auto& sum = std::get<SumDecl>(ast_.decls[static_cast<std::size_t>(global->declIndex)]);
    for (const Variant& variant : sum.variants) {
        if (!cover.variants.contains(variant.name)) {
            emitAt(at, "match is missing variant " + quoted(variant.name));
        }
    }
}

// One arm body under the type the arms have agreed on so far. Returns the
// body's type; the fault for a disagreeing arm lands on the body when it is
// an expression and on the whole match or select when it is a block
TyId Checker::checkArmBody(const ArmBody& body, TyId want, const Expr& whole,
                           std::string_view what) {
    TyId actual;
    const Expr* at = &whole;
    if (const auto* block = std::get_if<BlockId>(&body)) {
        actual = blockValue(*block, want);
    } else {
        const ExprId value = std::get<ExprId>(body);
        actual = inferExpr(value, want);
        at = &ast_.expr(value);
    }
    if (want.isSet() && !isCompatible(want, actual)) {
        emitAt(*at, typesMessage(std::string{what} + " arms disagree in type; got ", actual,
                                 " where ", want));
    }
    return actual;
}

TyId Checker::checkMatch(const Expr& expr, const MatchExpr& match, TyId want) {
    const TyId scrutinee = inferExpr(match.scrutinee, TyId{});
    Cover cover;
    TyId first;
    for (const MatchArm& arm : match.arms) {
        if (cover.hasCatchAll) {
            emitAt(expr, "unreachable match arm after a catch all pattern");
        }
        const TyId armWant = want.isSet() ? want : first;
        pushScope();
        bindPattern(arm.pattern, scrutinee, expr);
        const TyId actual = checkArmBody(arm.body, armWant, expr, "match");
        popScope();
        if (!armWant.isSet()) {
            first = actual;
        }
        coverPattern(arm.pattern, scrutinee, cover);
    }
    checkExhaustive(scrutinee, cover, expr);
    if (want.isSet()) {
        return want;
    }
    return first.isSet() ? first : unknown();
}

}  // namespace geas
