// Assignment. The target resolves in place, pointer by pointer down the
// chain, never through the copy on read path an rvalue takes: the root is
// the address of the binding's own C variable, a record field is the
// address of its slot in the live field array, a list element is its bounds
// checked address through geas_list_get, and a map index in the middle of a
// chain is the borrowed value slot through geas_map_get, a miss being
// GEAS_ERR_TYPE since an lvalue has no Option to hang a None on. The one
// form that writes no single slot is a final map index, the insert or
// update geas_map_set carries out. Every other target takes a deep copied
// value, the same isolation a let makes
#include "emitc.hpp"
#include "emittext.hpp"

namespace geas {

void CEmitter::genAssign(const AssignStmt& assign) {
    const Expr& target = arena().expr(assign.target);
    if (const auto* index = std::get_if<IndexExpr>(&target.kind)) {
        genIndexAssign(assign, target, *index);
        return;
    }
    const std::optional<LSlot> slot = genLvalue(assign.target);
    if (!slot) {
        return;
    }
    const CValue value = genExpr(assign.value, slot->type);
    const CValue copy = copyOut(value);
    line(joined("*", slot->pointer, " = ", copy.name, ";"));
}

// An assignment whose target ends in an index: a map takes the insert or
// update, a list the bounds checked slot write
void CEmitter::genIndexAssign(const AssignStmt& assign, const Expr& target,
                              const IndexExpr& index) {
    const std::optional<LSlot> base = genLvalue(index.base);
    if (!base) {
        return;
    }
    const TyNode node = nodeOf(base->type);
    if (node.kind == TyKind::Map) {
        const CValue key = genExpr(index.index, node.args[0]);
        const CValue value = genExpr(assign.value, node.args[1]);
        const std::string status = freshName();
        line(joined("GeasStatus ", status, " = geas_map_set(cc, ", base->pointer, ", &", key.name,
                    ", &", value.name, ");"));
        returnUnlessOk(status);
        return;
    }
    if (node.kind != TyKind::List) {
        unsupportedAt(target, "assignment through this index in a pledge body");
        return;
    }
    const std::string slot = genListSlot(base->pointer, index.index);
    const CValue value = genExpr(assign.value, node.args[0]);
    const CValue copy = copyOut(value);
    line(joined("*", slot, " = ", copy.name, ";"));
}

// The bounds checked address of a list element, out of range being the
// standing GEAS_ERR_TYPE fault
std::string CEmitter::genListSlot(std::string_view base, ExprId index) {
    const CValue position = genExpr(index, types().scalar(TyKind::Int));
    std::string slot = freshName();
    line(joined("GeasValue* ", slot, " = (GeasValue*)geas_list_get(", base, ", (uint64_t)",
                position.name, ".as.i);"));
    line(joined("if (!", slot, ") ", failExit("GEAS_ERR_TYPE")));
    return slot;
}

// The lvalue walk, nullopt after a fault
std::optional<LSlot> CEmitter::genLvalue(ExprId id) {
    if (const std::optional<CValue> slot = courseValue(id)) {
        std::string pointer = freshName();
        line(joined("GeasValue* ", pointer, " = &", slot->name, ";"));
        return LSlot{std::move(pointer), slot->type};
    }
    const Expr& expr = arena().expr(id);
    if (const auto* identifier = std::get_if<Identifier>(&expr.kind)) {
        const CValue* local = lookupLocal(identifier->name);
        if (local == nullptr) {
            unsupportedAt(expr, joined("assignment to '", identifier->name, "' in a pledge body"));
            return std::nullopt;
        }
        std::string pointer = freshName();
        line(joined("GeasValue* ", pointer, " = &", local->name, ";"));
        return LSlot{std::move(pointer), local->type};
    }
    if (const auto* member = std::get_if<MemberExpr>(&expr.kind)) {
        return genMemberSlot(expr, *member);
    }
    if (const auto* index = std::get_if<IndexExpr>(&expr.kind)) {
        return genIndexSlot(expr, *index);
    }
    unsupportedAt(expr, "assignment to this expression in a pledge body");
    return std::nullopt;
}

std::optional<LSlot> CEmitter::genMemberSlot(const Expr& expr, const MemberExpr& member) {
    const std::optional<LSlot> base = genLvalue(member.base);
    if (!base) {
        return std::nullopt;
    }
    std::optional<FieldSlot> field;
    if (lookup_.namedKind(base->type) == GlobalKind::Record) {
        field = lookup_.fieldOf(types().nameOf(base->type), member.name);
    }
    if (!field) {
        unsupportedAt(expr, "assignment to this field in a pledge body");
        return std::nullopt;
    }
    std::string pointer = freshName();
    line(joined("GeasValue* ", pointer, " = ((GeasValue*)", base->pointer, "->as.list.data) + ",
                decimal(field->index), ";"));
    return LSlot{std::move(pointer), field->type};
}

std::optional<LSlot> CEmitter::genIndexSlot(const Expr& expr, const IndexExpr& index) {
    const std::optional<LSlot> base = genLvalue(index.base);
    if (!base) {
        return std::nullopt;
    }
    const TyNode node = nodeOf(base->type);
    if (node.kind == TyKind::List) {
        return LSlot{genListSlot(base->pointer, index.index), node.args[0]};
    }
    if (node.kind != TyKind::Map) {
        unsupportedAt(expr, "assignment through this index in a pledge body");
        return std::nullopt;
    }
    const CValue key = genExpr(index.index, node.args[0]);
    const std::string borrowed = freshName();
    line(joined("const GeasValue* ", borrowed, " = NULL;"));
    std::string pointer = freshName();
    line(joined("if (!geas_map_get(", base->pointer, ", &", key.name, ", &", borrowed,
                ")) ", failExit("GEAS_ERR_TYPE")));
    line(joined("GeasValue* ", pointer, " = (GeasValue*)", borrowed, ";"));
    return LSlot{std::move(pointer), node.args[1]};
}

}  // namespace geas
