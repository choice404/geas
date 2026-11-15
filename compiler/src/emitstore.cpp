// The Store operations lowered onto their runtime primitives. The schema
// name is a bare identifier, resolved to the emitted descriptor entry and
// never lowered as a value; the key and row lower against the types the
// schema fixes. Each primitive runs in the thunk's exit convention, a status
// other than GEAS_OK returning straight through the body, and lands its
// Ok(...) result in the fresh temporary. The predicate forms of query live
// in emitstorequery.cpp; this file carries the keyed operations and the
// aggregates
#include "emitc.hpp"
#include "emittext.hpp"

namespace geas {

namespace {

constexpr std::string_view aggregateMin = "0u";
constexpr std::string_view aggregateMax = "1u";
constexpr std::string_view aggregateAvg = "2u";

std::string unsignedText(std::int64_t value) {
    return joined(decimal(value), "u");
}

std::string_view aggregateKind(std::string_view operation) {
    if (operation == "max") {
        return aggregateMax;
    }
    return operation == "avg" ? aggregateAvg : aggregateMin;
}

}  // namespace

// Resolves the schema a Store call names and opens the temporaries its
// result and status land in, nullopt after a fault
std::optional<StoreOp> CEmitter::openStoreOp(const Expr& expr, const CallExpr& call) {
    const auto* schemaName =
        call.args.empty() ? nullptr : std::get_if<Identifier>(&ast().expr(call.args[0].value).kind);
    std::size_t index = 0;
    const std::vector<const SchemaDecl*>& schemas = shape_->schemas;
    while (index < schemas.size() &&
           (schemaName == nullptr || schemas[index]->name != schemaName->name)) {
        ++index;
    }
    if (index == schemas.size()) {
        unsupportedAt(expr, "a Store operation on an unknown schema");
        return std::nullopt;
    }
    StoreOp op{};
    op.expr = &expr;
    op.call = &call;
    op.row = lookup_.sigs().record(schemas[index]->name);
    op.rowType = types().named(schemas[index]->name);
    op.errorType = types().named("StoreError");
    const bool hasKey = op.row != nullptr && !op.row->fields.empty();
    op.keyType = hasKey ? op.row->fields[0].type : unknown();
    op.schemaRef = joined("&k_", shape_->decl->name, "_schemas[",
                          decimal(static_cast<std::int64_t>(index)), "]");
    op.out = newValue();
    op.status = freshName();
    return op;
}

// Lands one primitive call and its status check, and types the answer
CValue CEmitter::finishStoreOp(const StoreOp& op, std::string_view callText, TyId okType) {
    line(joined("GeasStatus ", op.status, " = ", callText));
    returnUnlessOk(op.status);
    return CValue{op.out, types().result(okType, op.errorType)};
}

CValue CEmitter::genStoreOp(const Expr& expr, const CallExpr& call, std::string_view operation) {
    const std::optional<StoreOp> op = openStoreOp(expr, call);
    if (!op) {
        return badValue();
    }
    const TyId unit = types().scalar(TyKind::Unit);
    if (operation == "find") {
        return genStoreKeyed(*op, "geas_store_find", false, types().option(op->rowType));
    }
    if (operation == "query") {
        return genStoreQuery(*op);
    }
    if (operation == "count") {
        return genStoreCount(*op);
    }
    if (operation == "sum") {
        return genStoreSum(*op);
    }
    if (operation == "min" || operation == "max" || operation == "avg") {
        return genStoreAggregate(*op, operation);
    }
    if (operation == "insert") {
        return genStoreInsert(*op);
    }
    if (operation == "update") {
        return genStoreKeyed(*op, "geas_store_update", true, unit);
    }
    if (operation == "delete") {
        return genStoreKeyed(*op, "geas_store_delete", false, unit);
    }
    unsupportedAt(expr, joined("the Store operation '", operation, "' in a pledge body"));
    return badValue();
}

// find, update, and delete: the primary key, then the row for an update
CValue CEmitter::genStoreKeyed(const StoreOp& op, std::string_view primitive, bool takesRow,
                               TyId okType) {
    const CValue key = genExpr(op.call->args[1].value, op.keyType);
    std::string callText = joined(primitive, "(cc, ", op.schemaRef, ", &", key.name);
    if (takesRow) {
        const CValue row = genExpr(op.call->args[2].value, op.rowType);
        callText.append(joined(", &", row.name));
    }
    callText.append(joined(", &", op.out, ");"));
    return finishStoreOp(op, callText, okType);
}

CValue CEmitter::genStoreInsert(const StoreOp& op) {
    const CValue row = genExpr(op.call->args[1].value, op.rowType);
    return finishStoreOp(
        op, joined("geas_store_insert(cc, ", op.schemaRef, ", &", row.name, ", &", op.out, ");"),
        types().scalar(TyKind::Unit));
}

// count answers how many rows the predicate accepts without materializing
// any of them. A predicate carrying a || rides the group aware primitive; a
// pure && predicate keeps the where lowering
CValue CEmitter::genStoreCount(const StoreOp& op) {
    const StoreGroups groups = predicateGroups(op.call->args[1].value, false);
    const TyId integer = types().scalar(TyKind::Int);
    const std::string groupCount = unsignedText(static_cast<std::int64_t>(groups.size()));
    if (groups.size() > 1) {
        std::string lengths;
        const std::string terms = genGroupArrays(op, groups, lengths);
        return finishStoreOp(op,
                             joined("geas_store_count_any(cc, ", op.schemaRef, ", ", terms, ", ",
                                    lengths, ", ", groupCount, ", &", op.out, ");"),
                             integer);
    }
    const std::string terms = genTermArray(op, groups.front());
    const std::string termCount = unsignedText(static_cast<std::int64_t>(groups.front().size()));
    return finishStoreOp(op,
                         joined("geas_store_count_where(cc, ", op.schemaRef, ", ", terms, ", ",
                                termCount, ", &", op.out, ");"),
                         integer);
}

// sum totals one column over the rows the predicate accepts. The value is
// the column's own scalar, and an empty set sums to the column's own zero
CValue CEmitter::genStoreSum(const StoreOp& op) {
    const std::optional<FieldSlot> column = columnOf(op, op.call->args[1].value);
    if (!column) {
        unsupportedAt(*op.expr, "a Store.sum on an unknown column");
        return badValue();
    }
    const StoreGroups groups = predicateGroups(op.call->args[2].value, false);
    const std::string head = joined("(cc, ", op.schemaRef, ", ", unsignedText(column->index), ", ");
    if (groups.size() > 1) {
        std::string lengths;
        const std::string terms = genGroupArrays(op, groups, lengths);
        const std::string groupCount = unsignedText(static_cast<std::int64_t>(groups.size()));
        return finishStoreOp(op,
                             joined("geas_store_sum_any", head, terms, ", ", lengths, ", ",
                                    groupCount, ", &", op.out, ");"),
                             column->type);
    }
    const std::string terms = genTermArray(op, groups.front());
    const std::string termCount = unsignedText(static_cast<std::int64_t>(groups.front().size()));
    return finishStoreOp(
        op, joined("geas_store_sum_where", head, terms, ", ", termCount, ", &", op.out, ");"),
        column->type);
}

// min, max, and avg aggregate one column over the rows the predicate
// accepts. The predicate always rides the one group aware primitive, a pure
// && predicate being a single group. The answer is an Option the runtime
// fills Some on a match and None on the empty set, and avg answers a Float
// even over an Int column
CValue CEmitter::genStoreAggregate(const StoreOp& op, std::string_view operation) {
    const std::optional<FieldSlot> column = columnOf(op, op.call->args[1].value);
    if (!column) {
        unsupportedAt(*op.expr, "a Store aggregate on an unknown column");
        return badValue();
    }
    const StoreGroups groups = predicateGroups(op.call->args[2].value, false);
    std::string lengths;
    const std::string terms = genGroupArrays(op, groups, lengths);
    const std::string groupCount = unsignedText(static_cast<std::int64_t>(groups.size()));
    const TyId answer = operation == "avg" ? types().scalar(TyKind::Float) : column->type;
    return finishStoreOp(op,
                         joined("geas_store_agg_any(cc, ", op.schemaRef, ", ",
                                aggregateKind(operation), ", ", unsignedText(column->index), ", ",
                                terms, ", ", lengths, ", ", groupCount, ", &", op.out, ");"),
                         types().option(answer));
}

}  // namespace geas
