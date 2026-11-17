// Store.query and the predicate machinery every predicate taking Store
// operation shares. A predicate is a tree of comparisons under &&, ||, and
// !. Every ! is eliminated here, at compile time: a pending negation rides
// down the tree, a && or || under it becomes the other joiner, De Morgan's
// law, and a comparison under it flips its operator. Schema columns are
// never null, so the rewrite is exact. What comes out is the disjunctive
// normal form, an OR of AND groups. A predicate with one group is a pure &&
// predicate and keeps the where, ordered, and page primitives; one with
// more rides geas_store_select, its groups flattened into one term array
// beside a parallel array of group lengths
#include "emitc.hpp"
#include "emittext.hpp"

namespace geas {

namespace {

constexpr std::string_view noOrder = "GEAS_STORE_NO_ORDER";
constexpr std::string_view ascending = "0u";
constexpr std::string_view descending = "1u";
constexpr std::string_view noLimit = "NULL";
constexpr std::size_t whereArgs = 2;
constexpr std::size_t orderedArgs = 3;
constexpr std::size_t pageArgs = 4;

std::string unsignedText(std::int64_t value) {
    return joined(decimal(value), "u");
}

std::string countText(std::size_t count) {
    return unsignedText(static_cast<std::int64_t>(count));
}

std::string_view comparisonName(BinaryOp op) {
    switch (op) {
        case BinaryOp::Eq: return "GEAS_CMP_EQ";
        case BinaryOp::Ne: return "GEAS_CMP_NE";
        case BinaryOp::Lt: return "GEAS_CMP_LT";
        case BinaryOp::Le: return "GEAS_CMP_LE";
        case BinaryOp::Gt: return "GEAS_CMP_GT";
        default: return "GEAS_CMP_GE";
    }
}

// Flips a comparison operator to its own negation. A negated comparison is
// still one comparison, so a ! over a leaf never grows the predicate
BinaryOp flipComparison(BinaryOp op) {
    switch (op) {
        case BinaryOp::Eq: return BinaryOp::Ne;
        case BinaryOp::Ne: return BinaryOp::Eq;
        case BinaryOp::Lt: return BinaryOp::Ge;
        case BinaryOp::Le: return BinaryOp::Gt;
        case BinaryOp::Gt: return BinaryOp::Le;
        case BinaryOp::Ge: return BinaryOp::Lt;
        default: return op;
    }
}

// A && multiplies the two sides, every left group joined with every right
StoreGroups productOf(const StoreGroups& left, const StoreGroups& right) {
    StoreGroups out;
    for (const std::vector<StoreTerm>& leftGroup : left) {
        for (const std::vector<StoreTerm>& rightGroup : right) {
            std::vector<StoreTerm> group = leftGroup;
            group.insert(group.end(), rightGroup.begin(), rightGroup.end());
            out.push_back(std::move(group));
        }
    }
    return out;
}

}  // namespace

// The disjunctive normal form of a Store predicate with every ! eliminated.
// The checker already proved every leaf a comparison
StoreGroups CEmitter::predicateGroups(ExprId id, bool isNegated) const {
    const Expr& expr = arena().expr(id);
    if (const auto* unary = std::get_if<UnaryExpr>(&expr.kind)) {
        if (unary->op == UnaryOp::Not) {
            return predicateGroups(unary->operand, !isNegated);
        }
    }
    const auto* binary = std::get_if<BinaryExpr>(&expr.kind);
    if (binary == nullptr) {
        return StoreGroups{{}};
    }
    const bool isJoiner = binary->op == BinaryOp::And || binary->op == BinaryOp::Or;
    if (!isJoiner) {
        const BinaryOp op = isNegated ? flipComparison(binary->op) : binary->op;
        return StoreGroups{{StoreTerm{op, binary->lhs, binary->rhs}}};
    }
    StoreGroups left = predicateGroups(binary->lhs, isNegated);
    const StoreGroups right = predicateGroups(binary->rhs, isNegated);
    const bool isConjunction = (binary->op == BinaryOp::And) != isNegated;
    if (isConjunction) {
        return productOf(left, right);
    }
    left.insert(left.end(), right.begin(), right.end());
    return left;
}

// The schema column a bare name in a Store call resolves to
std::optional<FieldSlot> CEmitter::columnOf(const StoreOp& op, ExprId column) const {
    const auto* name = std::get_if<Identifier>(&arena().expr(column).kind);
    std::optional<FieldSlot> slot;
    for (std::size_t i = 0; name != nullptr && op.row != nullptr && i < op.row->fields.size();
         ++i) {
        if (op.row->fields[i].name == name->name) {
            slot = FieldSlot{static_cast<std::int64_t>(i), op.row->fields[i].type};
        }
    }
    return slot;
}

// One comparison lowered into its own GeasStoreTerm initializer, { column
// index, comparison, &value }. The value lowers against the column's type
// into a fresh temporary the initializer borrows
std::string CEmitter::genStoreTerm(const StoreOp& op, const StoreTerm& term) {
    const std::optional<FieldSlot> column = columnOf(op, term.column);
    const CValue value = genExpr(term.value, column ? column->type : unknown());
    return joined("{ ", unsignedText(column ? column->index : -1), ", ", comparisonName(term.op),
                  ", &", value.name, " }");
}

// Emits one C array declaration from initializers already formatted
void CEmitter::emitInitArray(std::string_view declaration, std::string_view array,
                             const std::vector<std::string>& items) {
    std::string text = joined(declaration, array, "[] = { ");
    for (std::size_t i = 0; i < items.size(); ++i) {
        text.append(i > 0 ? ", " : "");
        text.append(items[i]);
    }
    text.append(" };");
    line(text);
}

// Lowers one AND group into a GeasStoreTerm array and returns its name
std::string CEmitter::genTermArray(const StoreOp& op, const std::vector<StoreTerm>& terms) {
    std::vector<std::string> items;
    for (const StoreTerm& term : terms) {
        items.push_back(genStoreTerm(op, term));
    }
    std::string array = freshName();
    emitInitArray("GeasStoreTerm ", array, items);
    return array;
}

// Flattens the groups into one GeasStoreTerm array and a parallel uint32_t
// group length array, the shape the group aware primitives read. A term
// repeated across groups lowers its value once per group. Returns the term
// array's name and hands the length array's name back through lengths
std::string CEmitter::genGroupArrays(const StoreOp& op, const StoreGroups& groups,
                                     std::string& lengths) {
    std::vector<std::string> items;
    std::vector<std::string> sizes;
    for (const std::vector<StoreTerm>& group : groups) {
        for (const StoreTerm& term : group) {
            items.push_back(genStoreTerm(op, term));
        }
        sizes.push_back(countText(group.size()));
    }
    std::string array = freshName();
    emitInitArray("GeasStoreTerm ", array, items);
    lengths = freshName();
    emitInitArray("uint32_t ", lengths, sizes);
    return array;
}

// A predicate that carries a || lowered onto geas_store_select
void CEmitter::genStoreSelect(const StoreOp& op, const StoreGroups& groups,
                              std::string_view orderColumn, std::string_view direction,
                              std::string_view limitRef) {
    std::string lengths;
    const std::string terms = genGroupArrays(op, groups, lengths);
    line(joined("GeasStatus ", op.status, " = geas_store_select(cc, ", op.schemaRef, ", ", terms,
                ", ", lengths, ", ", countText(groups.size()), ", ", orderColumn, ", ", direction,
                ", ", limitRef, ", &", op.out, ");"));
    returnUnlessOk(op.status);
}

// query takes four shapes by argument count: a predicate, a predicate then
// asc(column) or desc(column), those two then limit(count), and the bare
// column and value of the equality form
CValue CEmitter::genStoreQuery(const StoreOp& op) {
    const std::vector<Argument>& args = op.call->args;
    if (args.size() == whereArgs) {
        return genStoreWhere(op);
    }
    if (args.size() == pageArgs) {
        return genStorePage(op);
    }
    const auto* order = args.size() == orderedArgs
                            ? std::get_if<CallExpr>(&arena().expr(args[2].value).kind)
                            : nullptr;
    const auto* direction =
        order == nullptr ? nullptr : std::get_if<Identifier>(&arena().expr(order->callee).kind);
    const bool isOrdered =
        direction != nullptr && (direction->name == "asc" || direction->name == "desc");
    if (isOrdered) {
        return genStoreOrdered(op, direction->name, order->args.front().value);
    }
    return genStoreEq(op);
}

CValue CEmitter::genStoreWhere(const StoreOp& op) {
    const TyId rows = types().list(op.rowType);
    const StoreGroups groups = predicateGroups(op.call->args[1].value, false);
    if (groups.size() > 1) {
        genStoreSelect(op, groups, noOrder, ascending, noLimit);
        return CValue{op.out, types().result(rows, op.errorType)};
    }
    const std::string terms = genTermArray(op, groups.front());
    return finishStoreOp(op,
                         joined("geas_store_query_where(cc, ", op.schemaRef, ", ", terms, ", ",
                                countText(groups.front().size()), ", &", op.out, ");"),
                         rows);
}

// The ordered form: the callee name fixes the direction and the primitive
// answers every row the predicate accepts, ordered by that column
CValue CEmitter::genStoreOrdered(const StoreOp& op, std::string_view direction, ExprId column) {
    const std::optional<FieldSlot> order = columnOf(op, column);
    if (!order) {
        unsupportedAt(*op.expr, "a Store.query ordered by an unknown column");
        return badValue();
    }
    const TyId rows = types().list(op.rowType);
    const std::string_view flag = direction == "desc" ? descending : ascending;
    const StoreGroups groups = predicateGroups(op.call->args[1].value, false);
    if (groups.size() > 1) {
        genStoreSelect(op, groups, unsignedText(order->index), flag, noLimit);
        return CValue{op.out, types().result(rows, op.errorType)};
    }
    const std::string terms = genTermArray(op, groups.front());
    return finishStoreOp(op,
                         joined("geas_store_query_ordered(cc, ", op.schemaRef, ", ", terms, ", ",
                                countText(groups.front().size()), ", ", unsignedText(order->index),
                                ", ", flag, ", &", op.out, ");"),
                         rows);
}

// The bounded form: the count lowers into its own Int temporary and its
// address rides the page primitive, which answers the leading rows of the
// ordered result the count keeps
CValue CEmitter::genStorePage(const StoreOp& op) {
    const std::vector<Argument>& args = op.call->args;
    const auto* order = std::get_if<CallExpr>(&arena().expr(args[2].value).kind);
    const auto* limit = std::get_if<CallExpr>(&arena().expr(args[3].value).kind);
    const auto* direction =
        order == nullptr ? nullptr : std::get_if<Identifier>(&arena().expr(order->callee).kind);
    const std::optional<FieldSlot> column =
        order == nullptr ? std::nullopt : columnOf(op, order->args.front().value);
    if (!column || limit == nullptr) {
        unsupportedAt(*op.expr, "a Store.query ordered by an unknown column");
        return badValue();
    }
    const bool isDescending = direction != nullptr && direction->name == "desc";
    const std::string_view flag = isDescending ? descending : ascending;
    const TyId rows = types().list(op.rowType);
    const TyId integer = types().scalar(TyKind::Int);
    const StoreGroups groups = predicateGroups(args[1].value, false);
    if (groups.size() > 1) {
        const CValue count = genExpr(limit->args.front().value, integer);
        genStoreSelect(op, groups, unsignedText(column->index), flag, joined("&", count.name));
        return CValue{op.out, types().result(rows, op.errorType)};
    }
    const std::string terms = genTermArray(op, groups.front());
    const CValue count = genExpr(limit->args.front().value, integer);
    return finishStoreOp(op,
                         joined("geas_store_query_page(cc, ", op.schemaRef, ", ", terms, ", ",
                                countText(groups.front().size()), ", ", unsignedText(column->index),
                                ", ", flag, ", &", count.name, ", &", op.out, ");"),
                         rows);
}

// The equality form: a bare column name and the value it must equal
CValue CEmitter::genStoreEq(const StoreOp& op) {
    const std::optional<FieldSlot> column = columnOf(op, op.call->args[1].value);
    if (!column) {
        unsupportedAt(*op.expr, "a Store.query on an unknown column");
        return badValue();
    }
    const CValue value = genExpr(op.call->args[2].value, column->type);
    return finishStoreOp(
        op,
        joined("geas_store_query_eq(cc, ", op.schemaRef, ", ", unsignedText(column->index), ", &",
               value.name, ", &", op.out, ");"),
        types().list(op.rowType));
}

}  // namespace geas
