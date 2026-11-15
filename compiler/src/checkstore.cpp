// The Store operations: find, query, count, sum, min, max, avg, insert,
// update, and delete, each naming a schema declared in the same contract.
// The first argument is the schema name, read as a bare identifier rather
// than inferred as a value, and the rest are the key, column, predicate, or
// row the operation binds. A row is the schema's record, and a store failure
// rides the Err side of every answer as a StoreError
#include "checker.hpp"

#include <array>

namespace geas {

namespace {

constexpr auto storeOperations = std::to_array<std::string_view>({
    "find",
    "query",
    "count",
    "sum",
    "min",
    "max",
    "avg",
    "insert",
    "update",
    "delete",
});

constexpr std::string_view predicateFault =
    "the second argument of Store.query is a column and a value, or a predicate of comparisons "
    "joined with && and ||, any subtree negated with !";
constexpr std::string_view orderFault =
    "the third argument of Store.query is asc(column) or desc(column)";
constexpr std::string_view limitFault = "the fourth argument of Store.query is limit(count)";

bool isStoreOperation(std::string_view name) {
    for (const std::string_view operation : storeOperations) {
        if (operation == name) {
            return true;
        }
    }
    return false;
}

bool isComparison(BinaryOp op) {
    switch (op) {
        case BinaryOp::Eq:
        case BinaryOp::Ne:
        case BinaryOp::Lt:
        case BinaryOp::Le:
        case BinaryOp::Gt:
        case BinaryOp::Ge: return true;
        default: return false;
    }
}

// Where each argument of a Store call sits, the schema name first
constexpr std::size_t secondIndex = 1;
constexpr std::size_t thirdIndex = 2;
constexpr std::size_t fourthIndex = 3;

// The argument counts of the query forms: a predicate, an ordered predicate
// or the equality shorthand, and a bounded predicate
constexpr std::size_t predicateFormCount = 2;
constexpr std::size_t orderedFormCount = 3;
constexpr std::size_t boundedFormCount = 4;

// What the aggregate and the write forms take, the schema name included
constexpr std::size_t countFormCount = 2;
constexpr std::size_t columnFormCount = 3;
constexpr std::size_t keyedFormCount = 2;
constexpr std::size_t updateFormCount = 3;

const RecordSig emptyRow{};

}  // namespace

// Whether the base of a member call is the bare Store namespace: an
// identifier spelling Store that no local shadows and no contract owns
bool Checker::isStoreNamespace(ExprId base) const {
    const auto* ident = std::get_if<Identifier>(&ast_.expr(base).kind);
    if (ident == nullptr || ident->name != "Store" || lookup(ident->name) != nullptr) {
        return false;
    }
    return contractNamed(ident->name) == nullptr;
}

const SchemaDecl* Checker::findSchema(std::string_view name) const {
    for (const Member& member : contract_->members) {
        const auto* schema = std::get_if<SchemaDecl>(&member);
        if (schema != nullptr && schema->name == name) {
            return schema;
        }
    }
    return nullptr;
}

// The type of a schema column, the last of that name when a schema repeats
// one, unset when the schema has no such column
TyId Checker::columnType(const StoreCall& store, std::string_view name) const {
    TyId type;
    for (const FieldSig& column : store.row->fields) {
        if (column.name == name) {
            type = column.type;
        }
    }
    return type;
}

void Checker::emitNoColumn(const StoreCall& store, const Expr& at, std::string_view column) {
    emitAt(at, "no column " + quoted(column) + " in schema " + quoted(store.schema->name));
}

// The bare callee name of an argument that is a call through an identifier,
// empty otherwise. The asc, desc, and limit suffixes are read this way
std::string_view Checker::querySuffixName(ExprId id) const {
    const auto* call = std::get_if<CallExpr>(&ast_.expr(id).kind);
    if (call == nullptr) {
        return "";
    }
    const auto* callee = std::get_if<Identifier>(&ast_.expr(call->callee).kind);
    return callee == nullptr ? std::string_view{} : std::string_view{callee->name};
}

// One comparison of a predicate: the left side is a bare column name and the
// right side a value of that column's own type
void Checker::checkQueryTerm(const StoreCall& store, const BinaryExpr& comparison) {
    const Expr& left = ast_.expr(comparison.lhs);
    const auto* column = std::get_if<Identifier>(&left.kind);
    if (column == nullptr) {
        emitAt(left, "the left side of a Store.query comparison is a column name");
        inferExpr(comparison.rhs, TyId{});
        return;
    }
    const TyId wanted = columnType(store, column->name);
    if (!wanted.isSet()) {
        emitNoColumn(store, left, column->name);
        inferExpr(comparison.rhs, TyId{});
        return;
    }
    const TyId actual = inferExpr(comparison.rhs, wanted);
    if (!isCompatible(wanted, actual)) {
        emitAt(ast_.expr(comparison.rhs),
               typesMessage("the value of a Store.query comparison expects ", wanted, ", got ",
                            actual));
    }
}

// A predicate is a free boolean tree of comparisons over && and ||, any
// subtree negated with !, the same grammar for every Store form
void Checker::checkQueryPredicate(const StoreCall& store, ExprId id) {
    const Expr& expr = ast_.expr(id);
    const auto* unary = std::get_if<UnaryExpr>(&expr.kind);
    if (unary != nullptr && unary->op == UnaryOp::Not) {
        checkQueryPredicate(store, unary->operand);
        return;
    }
    const auto* binary = std::get_if<BinaryExpr>(&expr.kind);
    if (binary != nullptr && (binary->op == BinaryOp::And || binary->op == BinaryOp::Or)) {
        checkQueryPredicate(store, binary->lhs);
        checkQueryPredicate(store, binary->rhs);
        return;
    }
    if (binary != nullptr && isComparison(binary->op)) {
        checkQueryTerm(store, *binary);
        return;
    }
    emitAt(expr, std::string{predicateFault});
}

// The order suffix, asc(column) or desc(column), naming one bare column of the schema
void Checker::checkQueryOrder(const StoreCall& store, ExprId id) {
    const Expr& expr = ast_.expr(id);
    const auto& call = std::get<CallExpr>(expr.kind);
    const std::string fault =
        "the argument of " + std::string{querySuffixName(id)} + " is a column name";
    if (call.args.size() != 1) {
        emitAt(expr, fault);
        return;
    }
    const Expr& value = ast_.expr(call.args.front().value);
    const auto* column = std::get_if<Identifier>(&value.kind);
    if (column == nullptr) {
        emitAt(value, fault);
        return;
    }
    if (!columnType(store, column->name).isSet()) {
        emitNoColumn(store, value, column->name);
    }
}

// The bound suffix, limit(count), one Int the runtime reads to cap the ordered page
void Checker::checkQueryLimit(ExprId id) {
    const Expr& expr = ast_.expr(id);
    const auto& call = std::get<CallExpr>(expr.kind);
    if (call.args.size() != 1) {
        emitAt(expr, std::string{limitFault});
        return;
    }
    const TyId intType = scalar(TyKind::Int);
    const ExprId count = call.args.front().value;
    const TyId actual = inferExpr(count, intType);
    if (!isCompatible(intType, actual)) {
        emitAt(ast_.expr(count), "the argument of limit is an Int");
    }
}

// One trailing argument against the type the schema fixes for its position.
// An argument past the end was already reported by the count check
void Checker::checkStoreArg(const StoreCall& store, std::size_t index, TyId want,
                            std::string_view what) {
    if (index >= store.call->args.size()) {
        return;
    }
    const Argument& arg = store.call->args[index];
    const Expr& value = ast_.expr(arg.value);
    if (arg.label) {
        emitAt(value, std::string{labelFault});
    }
    const TyId actual = inferExpr(arg.value, want);
    if (!isCompatible(want, actual)) {
        emitAt(value, typesMessage(std::string{what} + " of the Store operation expects ", want,
                                   ", got ", actual));
    }
}

// The second argument of the forms that name a column: a bare name resolved
// against the schema, and for sum and avg an Int or Float column. Returns
// the column type, unknown after any fault
TyId Checker::storeColumnArg(const StoreCall& store, bool needsNumber) {
    if (store.call->args.size() <= secondIndex) {
        return unknown();
    }
    const Expr& value = ast_.expr(store.call->args[secondIndex].value);
    const auto* column = std::get_if<Identifier>(&value.kind);
    if (column == nullptr) {
        emitAt(value, "the second argument of Store." + store.operation + " is a column name");
        return unknown();
    }
    const TyId type = columnType(store, column->name);
    if (!type.isSet()) {
        emitNoColumn(store, value, column->name);
        return unknown();
    }
    const TyKind kind = kindOf(type);
    if (needsNumber && kind != TyKind::Int && kind != TyKind::Float) {
        emitAt(value, "Store." + store.operation + " takes an Int or Float column");
        return unknown();
    }
    return type;
}

bool Checker::isOrderSuffix(ExprId id) const {
    const std::string_view suffix = querySuffixName(id);
    return suffix == "asc" || suffix == "desc";
}

// The ordered shape of query, a predicate then asc(column) or desc(column).
// Returns whether the three argument call was that shape, false leaves the
// equality shorthand of a column and a value
bool Checker::checkOrderedQuery(const StoreCall& store) {
    const std::vector<Argument>& args = store.call->args;
    const Expr& third = ast_.expr(args[thirdIndex].value);
    // A bound cuts the page an order fixes, so a limit with no order ahead
    // of it is refused by name
    if (querySuffixName(args[thirdIndex].value) == "limit") {
        checkQueryPredicate(store, args[secondIndex].value);
        checkQueryLimit(args[thirdIndex].value);
        emitAt(third, "a bounded Store.query names its order");
        return true;
    }
    const bool hasOrder = isOrderSuffix(args[thirdIndex].value);
    const bool isColumn =
        std::holds_alternative<Identifier>(ast_.expr(args[secondIndex].value).kind);
    if (!hasOrder && isColumn) {
        return false;
    }
    checkQueryPredicate(store, args[secondIndex].value);
    if (hasOrder) {
        checkQueryOrder(store, args[thirdIndex].value);
    } else {
        emitAt(third, std::string{orderFault});
    }
    return true;
}

// The bounded shape of query: a predicate, an order, then limit(count)
void Checker::checkBoundedQuery(const StoreCall& store) {
    const std::vector<Argument>& args = store.call->args;
    checkQueryPredicate(store, args[secondIndex].value);
    if (isOrderSuffix(args[thirdIndex].value)) {
        checkQueryOrder(store, args[thirdIndex].value);
    } else {
        emitAt(ast_.expr(args[thirdIndex].value), std::string{orderFault});
    }
    if (querySuffixName(args[fourthIndex].value) == "limit") {
        checkQueryLimit(args[fourthIndex].value);
    } else {
        emitAt(ast_.expr(args[fourthIndex].value), std::string{limitFault});
    }
}

// A query answers every matching row, so the result carries a list. Two
// arguments are a predicate, three an ordered predicate or the equality
// shorthand of a column and a value, and four a bounded predicate
TyId Checker::checkStoreQuery(const StoreCall& store) {
    const std::vector<Argument>& args = store.call->args;
    const TyId answer = types_.result(types_.list(store.rowType), store.errorType);
    if (args.size() == predicateFormCount) {
        checkQueryPredicate(store, args[secondIndex].value);
        return answer;
    }
    if (args.size() == orderedFormCount && checkOrderedQuery(store)) {
        return answer;
    }
    if (args.size() == boundedFormCount) {
        checkBoundedQuery(store);
        return answer;
    }
    if (args.size() != orderedFormCount) {
        emitAt(*store.expr, "Store.query takes 3 arguments, got " +
                                countText(static_cast<std::int64_t>(args.size())));
    }
    const TyId column = storeColumnArg(store, false);
    checkStoreArg(store, thirdIndex, column, "the value");
    return answer;
}

// The aggregates count, sum, min, max, and avg answer over the rows a predicate accepts
// without materializing any of them. The empty set sums to zero and has no
// least, greatest, or mean, so min, max, and avg answer an Option
TyId Checker::checkStoreAggregate(const StoreCall& store) {
    const std::vector<Argument>& args = store.call->args;
    const std::string& operation = store.operation;
    if (operation == "count") {
        if (args.size() != countFormCount) {
            emitAt(*store.expr, "Store.count takes a schema and a predicate");
        }
        if (args.size() >= countFormCount) {
            checkQueryPredicate(store, args[secondIndex].value);
        }
        return types_.result(scalar(TyKind::Int), store.errorType);
    }
    if (args.size() != columnFormCount) {
        emitAt(*store.expr, "Store." + operation + " takes a schema, a column, and a predicate");
    }
    const bool needsNumber = operation == "sum" || operation == "avg";
    const TyId column = storeColumnArg(store, needsNumber);
    if (args.size() >= columnFormCount) {
        checkQueryPredicate(store, args[thirdIndex].value);
    }
    if (operation == "sum") {
        return types_.result(column, store.errorType);
    }
    const TyId value = operation == "avg" ? scalar(TyKind::Float) : column;
    return types_.result(types_.option(value), store.errorType);
}

// The keyed forms find, delete, insert, and update: the count first, then each trailing
// argument against the key or row type the schema fixes
TyId Checker::checkStoreWrite(const StoreCall& store) {
    const std::string& operation = store.operation;
    const std::size_t expected = operation == "update" ? updateFormCount : keyedFormCount;
    const std::size_t count = store.call->args.size();
    if (count != expected) {
        emitAt(*store.expr, "Store." + operation + " takes " +
                                countText(static_cast<std::int64_t>(expected)) +
                                " arguments, got " + countText(static_cast<std::int64_t>(count)));
    }
    const TyId unit = scalar(TyKind::Unit);
    if (operation == "find") {
        checkStoreArg(store, secondIndex, store.keyType, "the key");
        return types_.result(types_.option(store.rowType), store.errorType);
    }
    if (operation == "delete") {
        checkStoreArg(store, secondIndex, store.keyType, "the key");
        return types_.result(unit, store.errorType);
    }
    if (operation == "insert") {
        checkStoreArg(store, secondIndex, store.rowType, "the row");
        return types_.result(unit, store.errorType);
    }
    checkStoreArg(store, secondIndex, store.keyType, "the key");
    checkStoreArg(store, thirdIndex, store.rowType, "the row");
    return types_.result(unit, store.errorType);
}

// The schema a Store call names, null after reporting why it names none
const SchemaDecl* Checker::storeSchema(const Expr& expr, const CallExpr& call,
                                       const std::string& operation) {
    if (contract_ == nullptr) {
        walkArgs(call.args);
        emitAt(expr, "a Store operation runs only inside a contract pledge");
        return nullptr;
    }
    if (call.args.empty()) {
        emitAt(expr, "Store." + operation + " names a schema as its first argument");
        return nullptr;
    }
    const auto* name = std::get_if<Identifier>(&ast_.expr(call.args.front().value).kind);
    if (name == nullptr) {
        walkArgs(call.args);
        emitAt(expr, "the first argument of Store." + operation + " is a schema name");
        return nullptr;
    }
    const SchemaDecl* schema = findSchema(name->name);
    if (schema == nullptr) {
        walkArgs(call.args);
        emitAt(expr,
               "no schema " + quoted(name->name) + " in this contract for Store." + operation);
    }
    return schema;
}

TyId Checker::checkStoreOp(const Expr& expr, const CallExpr& call, const std::string& operation) {
    if (!isStoreOperation(operation)) {
        walkArgs(call.args);
        emitAt(expr, "Store has no operation " + quoted(operation));
        return unknown();
    }
    const TyId errorType = types_.named("StoreError");
    const SchemaDecl* schema = storeSchema(expr, call, operation);
    if (schema == nullptr) {
        return unknown();
    }
    const RecordSig* row = sigs_.record(schema->name);
    StoreCall store{&expr, &call, schema, row == nullptr ? &emptyRow : row, operation, {}, {}, {}};
    store.rowType = types_.named(schema->name);
    store.errorType = errorType;
    store.keyType = store.row->fields.empty() ? unknown() : store.row->fields.front().type;
    markUse(expr, ExprUse::StoreCall);
    if (operation == "query") {
        return checkStoreQuery(store);
    }
    const bool isWrite = operation == "find" || operation == "delete" || operation == "insert" ||
                         operation == "update";
    return isWrite ? checkStoreWrite(store) : checkStoreAggregate(store);
}

}  // namespace geas
