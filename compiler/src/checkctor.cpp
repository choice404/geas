// The constructors: record literals, Some, Ok, Err, None, mut, and the
// generic constructors on the builtin composites. Ok, Err, and None lean on
// the expected type for the side they do not carry
#include "checker.hpp"

namespace geas {

// The first field of that name in a record, null when the record has none
const FieldSig* Checker::recordField(std::string_view record, std::string_view field) const {
    const RecordSig* sig = sigs_.record(record);
    if (sig == nullptr) {
        return nullptr;
    }
    for (const FieldSig& candidate : sig->fields) {
        if (candidate.name == field) {
            return &candidate;
        }
    }
    return nullptr;
}

// One field of a record literal against the record's declaration
void Checker::checkFieldInit(const RecordLiteral& literal, std::size_t index) {
    const FieldInit& init = literal.fields[index];
    const Expr& value = ast_.expr(init.value);
    bool isRepeated = false;
    for (std::size_t earlier = 0; earlier < index; ++earlier) {
        isRepeated = isRepeated || literal.fields[earlier].name == init.name;
    }
    if (isRepeated) {
        emitAt(value,
               "field " + quoted(init.name) + " is set more than once in " + quoted(literal.name));
    }
    const FieldSig* field = recordField(literal.name, init.name);
    if (field == nullptr) {
        emitAt(value, quoted(literal.name) + " has no field " + quoted(init.name));
        inferExpr(init.value, TyId{});
        return;
    }
    const TyId wanted = field->type;
    const TyId actual = inferExpr(init.value, wanted);
    if (!isCompatible(wanted, actual)) {
        const std::string head =
            "field " + quoted(init.name) + " of " + quoted(literal.name) + " expects ";
        emitAt(value, typesMessage(head, wanted, ", got ", actual));
    }
}

TyId Checker::inferRecordLiteral(const Expr& expr, const RecordLiteral& literal) {
    const std::string& name = literal.name;
    const GlobalEntry* global = resolved_.global(name);
    if (global == nullptr || global->kind != GlobalKind::Record) {
        for (const FieldInit& init : literal.fields) {
            inferExpr(init.value, TyId{});
        }
        emitAt(expr, global == nullptr ? "unknown record " + quoted(name)
                                       : quoted(name) + " is not a record");
        return unknown();
    }
    for (std::size_t i = 0; i < literal.fields.size(); ++i) {
        checkFieldInit(literal, i);
    }
    const RecordSig* sig = sigs_.record(name);
    if (sig == nullptr) {
        return types_.named(name);
    }
    for (const FieldSig& field : sig->fields) {
        bool isSupplied = false;
        for (const FieldInit& init : literal.fields) {
            isSupplied = isSupplied || init.name == field.name;
        }
        if (!isSupplied) {
            emitAt(expr, "record literal for " + quoted(name) + " is missing field " +
                             quoted(field.name));
        }
    }
    return types_.named(name);
}

TyId Checker::inferSome(const Expr& expr, const WrapExpr& wrap, TyId want) {
    if (want.isSet() && kindOf(want) == TyKind::Option) {
        const TyId slot = types_.node(want).args[0];
        const TyId inner = inferExpr(wrap.inner, slot);
        if (!isCompatible(slot, inner)) {
            emitAt(expr, typesMessage("'Some' carries ", inner, " where ", slot));
        }
        return want;
    }
    const TyId inner = inferExpr(wrap.inner, TyId{});
    return types_.option(inner);
}

// Ok and Err name one side of a Result, the expected type supplies the other
TyId Checker::inferOkErr(const Expr& expr, const WrapExpr& wrap, TyId want) {
    const bool isOk = wrap.kind == WrapKind::Ok;
    if (want.isSet() && kindOf(want) == TyKind::Result) {
        const TyId slot = types_.node(want).args[isOk ? 0 : 1];
        const TyId inner = inferExpr(wrap.inner, slot);
        if (!isCompatible(slot, inner)) {
            emitAt(expr,
                   typesMessage(isOk ? "'Ok' carries " : "'Err' carries ", inner, " where ", slot));
        }
        return want;
    }
    const TyId inner = inferExpr(wrap.inner, TyId{});
    if (isOk) {
        emitAt(expr, "cannot infer the error type of 'Ok'; annotate the binding");
        return types_.result(inner, unknown());
    }
    emitAt(expr, "cannot infer the value type of 'Err'; annotate the binding");
    return types_.result(unknown(), inner);
}

TyId Checker::inferWrap(const Expr& expr, const WrapExpr& wrap, TyId want) {
    switch (wrap.kind) {
        case WrapKind::Some: return inferSome(expr, wrap, want);
        case WrapKind::Mut: return inferExpr(wrap.inner, want);
        default: return inferOkErr(expr, wrap, want);
    }
}

TyId Checker::inferNone(const Expr& expr, TyId want) {
    if (want.isSet() && kindOf(want) == TyKind::Option) {
        return want;
    }
    emitAt(expr, "cannot infer the type of 'None'; annotate the binding");
    return unknown();
}

// One value handed to a generic constructor, held to the slot it fills
void Checker::checkCtorArg(const Argument& arg, std::size_t index, TyId want,
                           std::string_view ctor) {
    const Expr& value = ast_.expr(arg.value);
    if (arg.label) {
        emitAt(value, std::string{labelFault});
    }
    const TyId actual = inferExpr(arg.value, want);
    if (want.isSet() && !isCompatible(want, actual)) {
        const std::string head =
            argumentHead(index, " of the " + std::string{ctor} + " constructor expects ");
        emitAt(value, typesMessage(head, want, ", got ", actual));
    }
}

// List<T>(v, ...) holds its values, Option<T>() is None and Option<T>(v) is
// Some(v), Result<T, E>(v) takes one Ok or Err, and Tuple<A, B>(a, b) takes
// one value per slot. The annotation fixes the type, the values are held to
// it
TyId Checker::inferGenericNew(const Expr& expr, const GenericNewExpr& ctor) {
    const TyId type = lowering_.lower(ctor.type, expr.lo, expr.hi);
    const TyNode node = types_.node(type);
    const std::size_t count = ctor.args.size();
    const auto slotFor = [&](std::size_t index) {
        switch (node.kind) {
            case TyKind::List:
            case TyKind::Option: return node.args[0];
            case TyKind::Result: return type;
            case TyKind::Tuple: return index < node.args.size() ? node.args[index] : TyId{};
            default: return TyId{};
        }
    };
    const std::string name{node.kind == TyKind::List     ? "List"
                           : node.kind == TyKind::Option ? "Option"
                           : node.kind == TyKind::Result ? "Result"
                                                         : "Tuple"};
    const auto countFault = [&](std::string_view takes) {
        emitAt(expr, "the " + name + " constructor takes " + std::string{takes} + ", got " +
                         countText(static_cast<std::int64_t>(count)));
    };
    if (node.kind == TyKind::Option && count > 1) {
        countFault("at most one value");
    } else if (node.kind == TyKind::Result && count != 1) {
        countFault("one Ok or Err value");
    } else if (node.kind == TyKind::Tuple && count != node.args.size()) {
        countFault(countText(static_cast<std::int64_t>(node.args.size())) + " values");
    }
    for (std::size_t i = 0; i < count; ++i) {
        checkCtorArg(ctor.args[i], i, slotFor(i), name);
    }
    return type;
}

}  // namespace geas
