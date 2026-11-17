// The constructors and the reads: the Option and Result wrappers, list,
// tuple, and record literals, sum variants, record field reads, list and
// map indexing, and the propagation operator. A boxed payload lands in an
// instance owned box, a record or a variant payload in an instance owned
// field array. A read yields a value, so a composite lifted out of a field
// or a slot comes out as its own copy and mutating it never writes back
#include "emitc.hpp"
#include "emittext.hpp"

namespace geas {

namespace {

constexpr int tagSome = 1;
constexpr int tagOk = 0;
constexpr int tagErr = 1;

std::string countOf(std::size_t count) {
    return decimal(static_cast<std::int64_t>(count));
}

}  // namespace

CValue CEmitter::genWrap(const WrapExpr& wrap, TyId want) {
    switch (wrap.kind) {
        case WrapKind::Some: return genSome(wrap, want);
        case WrapKind::Mut: return genExpr(wrap.inner, want);
        default: return genOkErr(wrap, want);
    }
}

// A boxed constructor: the payload lands in an instance owned box and the
// wrapper carries the ABI tag, Option 0 None 1 Some, Result 0 Ok 1 Err
CValue CEmitter::genBoxed(const CValue& payload, std::string_view tag, int variant, TyId type) {
    const std::string box = freshName();
    line(joined("GeasValue* ", box, " = geas_box(cc);"));
    line(joined("if (!", box, ") ", failExit("GEAS_ERR_OOM")));
    line(joined("*", box, " = ", payload.name, ";"));
    std::string value = newValue();
    setTag(value, tag);
    line(joined(value, ".tag = ", decimal(variant), ";"));
    line(joined(value, ".as.box = ", box, ";"));
    return CValue{std::move(value), type};
}

CValue CEmitter::genSome(const WrapExpr& wrap, TyId want) {
    const TyNode wanted = nodeOf(want);
    const TyId inner = wanted.kind == TyKind::Option ? wanted.args[0] : TyId{};
    const CValue payload = genExpr(wrap.inner, inner);
    return genBoxed(payload, "GEAS_TY_OPTION", tagSome, types().option(payload.type));
}

CValue CEmitter::genOkErr(const WrapExpr& wrap, TyId want) {
    const bool isOk = wrap.kind == WrapKind::Ok;
    const TyNode wanted = nodeOf(want);
    const bool hasResult = wanted.kind == TyKind::Result;
    const TyId inner = hasResult ? wanted.args[isOk ? 0 : 1] : TyId{};
    const CValue payload = genExpr(wrap.inner, inner);
    TyId type = want;
    if (!hasResult) {
        type = isOk ? types().result(payload.type, unknown())
                    : types().result(unknown(), payload.type);
    }
    return genBoxed(payload, "GEAS_TY_RESULT", isOk ? tagOk : tagErr, type);
}

CValue CEmitter::genNone(TyId want) {
    const TyId type = kindOf(want) == TyKind::Option ? want : types().option(unknown());
    std::string value = newValue();
    setTag(value, "GEAS_TY_OPTION");
    return CValue{std::move(value), type};
}

// A list literal: the elements lower in source order, then the list is built
// with the element tag of the checked element type and each value pushed
CValue CEmitter::genList(const ListExpr& list, TyId want) {
    const TyNode wanted = nodeOf(want);
    TyId element = wanted.kind == TyKind::List ? wanted.args[0] : TyId{};
    std::vector<CValue> values;
    for (const ExprId item : list.elements) {
        values.push_back(genExpr(item, element));
        if (!element.isSet()) {
            element = values.back().type;
        }
    }
    if (!element.isSet()) {
        element = unknown();
    }
    // An empty literal with no expectation has no element tag to declare;
    // Unit stands in, and nothing is ever pushed against it
    const std::string_view elementTag = lookup_.tagOf(element);
    const std::string_view tag = elementTag.empty() ? "GEAS_TY_UNIT" : elementTag;
    std::string value = newValue();
    line(joined("if (geas_list_new(cc, ", tag, ", ", countOf(list.elements.size()), ", &", value,
                ") != GEAS_OK) ", failExit("GEAS_ERR_OOM")));
    for (const CValue& item : values) {
        line(joined("if (geas_list_push(cc, &", value, ", &", item.name,
                    ") != GEAS_OK) ", failExit("GEAS_ERR_OOM")));
    }
    return CValue{std::move(value), types().list(element)};
}

// A tuple literal: elements in source order, then a fresh tuple whose slots
// take the values one by one
CValue CEmitter::genTuple(const TupleExpr& tuple, TyId want) {
    const TyNode wanted = nodeOf(want);
    const bool hasShape =
        wanted.kind == TyKind::Tuple && wanted.args.size() == tuple.elements.size();
    std::vector<CValue> values;
    std::vector<TyId> elementTypes;
    for (std::size_t i = 0; i < tuple.elements.size(); ++i) {
        values.push_back(genExpr(tuple.elements[i], hasShape ? wanted.args[i] : TyId{}));
        elementTypes.push_back(values.back().type);
    }
    std::string value = newValue();
    line(joined("if (geas_tuple_new(cc, ", countOf(tuple.elements.size()), ", &", value,
                ") != GEAS_OK) ", failExit("GEAS_ERR_OOM")));
    for (std::size_t i = 0; i < values.size(); ++i) {
        line(joined("((GeasValue*)", value, ".as.list.data)[", countOf(i), "] = ", values[i].name,
                    ";"));
    }
    return CValue{std::move(value), types().tuple(std::move(elementTypes))};
}

// Opens the instance owned field array a record or a variant payload fills
std::string CEmitter::genFieldArray(std::size_t count) {
    std::string array = freshName();
    line(joined("GeasValue* ", array, " = (GeasValue*)geas_bytes(cc, ", countOf(count),
                " * sizeof(GeasValue));"));
    line(joined("if (!", array, ") ", failExit("GEAS_ERR_OOM")));
    return array;
}

// Hands a filled field array to the value that owns it
void CEmitter::adoptFieldArray(std::string_view value, std::string_view array, std::size_t count) {
    line(joined(value, ".as.list.data = ", array, ";"));
    line(joined(value, ".as.list.len = ", countOf(count), ";"));
    line(joined(value, ".as.list.cap = ", countOf(count), ";"));
}

// A record literal. The slots fill in declaration order, each value lowered
// against its declared field type
CValue CEmitter::genRecord(const Expr& expr, const RecordLiteral& record) {
    const RecordSig* sig = lookup_.sigs().record(record.name);
    if (sig == nullptr) {
        unsupportedAt(expr, joined("the record literal '", record.name, "' in a pledge body"));
        return badValue();
    }
    const std::size_t count = sig->fields.size();
    const std::string array = count > 0 ? genFieldArray(count) : std::string{};
    for (std::size_t i = 0; i < count; ++i) {
        const FieldSig& field = sig->fields[i];
        const FieldInit* init = nullptr;
        for (const FieldInit& candidate : record.fields) {
            init = candidate.name == field.name ? &candidate : init;
        }
        if (init == nullptr) {
            lookup_.fault(expr.lo, expr.hi,
                          joined("codegen: record literal is missing field '", field.name, "'"));
            continue;
        }
        const CValue value = genExpr(init->value, field.type);
        line(joined(array, "[", countOf(i), "] = ", value.name, ";"));
    }
    std::string value = newValue();
    setTag(value, "GEAS_TY_RECORD");
    if (count > 0) {
        adoptFieldArray(value, array, count);
    }
    return CValue{std::move(value), types().named(record.name)};
}

// A sum variant constructor, the bare form and the payload carrying call
// form alike. The value's tag is the variant's declaration index and the
// payload rides the list arm as an instance owned field array
CValue CEmitter::genVariant(const Expr& expr, std::string_view name,
                            const std::vector<Argument>& args) {
    const std::optional<VariantSlot> variant = lookup_.variantOf(name);
    if (!variant) {
        unsupportedAt(expr, joined("the constructor '", name, "' in a pledge body"));
        return badValue();
    }
    const std::vector<FieldSig>& fields = variant->sig->fields;
    const std::string array = fields.empty() ? std::string{} : genFieldArray(fields.size());
    for (std::size_t i = 0; !fields.empty() && i < args.size(); ++i) {
        const bool hasSlot = i < fields.size();
        const CValue value = genExpr(args[i].value, hasSlot ? fields[i].type : TyId{});
        if (hasSlot) {
            line(joined(array, "[", countOf(i), "] = ", value.name, ";"));
        }
    }
    std::string value = newValue();
    setTag(value, "GEAS_TY_SUM");
    line(joined(value, ".tag = ", decimal(variant->index), ";"));
    if (!fields.empty()) {
        adoptFieldArray(value, array, fields.size());
    }
    return CValue{std::move(value), types().named(variant->sig->owner)};
}

// A member read: a record field lifts out of its slot by declaration index,
// a vow reads through the instance that carries it. A pledge read off an
// instance, the first class pledge value, has no lowering
CValue CEmitter::genMember(const Expr& expr, const MemberExpr& member) {
    if (std::optional<CValue> prior = genPriorRead(expr, member)) {
        return *prior;
    }
    const CValue base = genExpr(member.base, TyId{});
    std::optional<FieldSlot> field;
    if (lookup_.namedKind(base.type) == GlobalKind::Record) {
        field = lookup_.fieldOf(types().nameOf(base.type), member.name);
    }
    if (field) {
        std::string value = freshName();
        line(joined("GeasValue ", value, " = ((const GeasValue*)", base.name, ".as.list.data)[",
                    decimal(field->index), "];"));
        return copyOut(CValue{std::move(value), field->type});
    }
    if (kindOf(base.type) == TyKind::Contract) {
        return genInstanceVow(expr, member, base);
    }
    unsupportedAt(expr, joined("member access '.", member.name, "' in a pledge body"));
    return badValue();
}

// A vow read through an instance: geas_vow_ref borrows the locked value,
// and the read deep copies it home so the value survives the callee's break
CValue CEmitter::genInstanceVow(const Expr& expr, const MemberExpr& member, const CValue& base) {
    const ContractDecl* contract = lookup_.contractOf(base.type);
    const MemberEntry* entry =
        contract == nullptr ? nullptr : lookup_.resolved().member(contract, member.name);
    if (entry == nullptr || entry->kind() != MemberKind::Vow) {
        unsupportedAt(expr, "a first class pledge value in a pledge body");
        return badValue();
    }
    const std::string borrowed = freshName();
    line(joined("const GeasValue* ", borrowed, " = geas_vow_ref((GeasContract*)", base.name,
                ".as.box, \"", member.name, "\");"));
    line(joined("if (!", borrowed, ") ", failExit("GEAS_ERR_NAME")));
    std::string home = newValue();
    const std::string status = freshName();
    line(
        joined("GeasStatus ", status, " = geas_value_deep_copy(cc, ", borrowed, ", &", home, ");"));
    returnUnlessOk(status);
    return CValue{std::move(home), lookup_.vowType(*entry->vow())};
}

// A map index read: geas_map_get borrows the value slot on a hit, and the
// result is Option<V>, Some around a deep copy of the slot, None on a miss
CValue CEmitter::genMapIndex(const CValue& base, const IndexExpr& index) {
    const TyNode node = nodeOf(base.type);
    const CValue key = genExpr(index.index, node.args[0]);
    const std::string borrowed = freshName();
    line(joined("const GeasValue* ", borrowed, " = NULL;"));
    const std::string found = freshName();
    line(joined("int ", found, " = geas_map_get(&", base.name, ", &", key.name, ", &", borrowed,
                ");"));
    std::string value = newValue();
    setTag(value, "GEAS_TY_OPTION");
    line(joined("if (", found, ") {"));
    ++indent_;
    const std::string box = freshName();
    line(joined("GeasValue* ", box, " = geas_box(cc);"));
    line(joined("if (!", box, ") ", failExit("GEAS_ERR_OOM")));
    const std::string status = freshName();
    line(joined("GeasStatus ", status, " = geas_value_deep_copy(cc, ", borrowed, ", ", box, ");"));
    returnUnlessOk(status);
    line(joined(value, ".tag = 1;"));
    line(joined(value, ".as.box = ", box, ";"));
    --indent_;
    line("}");
    return CValue{std::move(value), types().option(node.args[1])};
}

// A list index read, bounds checked through geas_list_get: an index outside
// the list, negative included through the unsigned cast, is a clean
// GEAS_ERR_TYPE from the pledge. A map index read answers an Option
CValue CEmitter::genIndex(const Expr& expr, const IndexExpr& index) {
    const CValue base = genExpr(index.base, TyId{});
    const TyNode node = nodeOf(base.type);
    if (node.kind == TyKind::Map) {
        return genMapIndex(base, index);
    }
    const CValue position = genExpr(index.index, types().scalar(TyKind::Int));
    if (node.kind == TyKind::Bytes) {
        return genBytesIndex(base, position);
    }
    if (node.kind != TyKind::List) {
        unsupportedAt(expr, "indexing this type in a pledge body");
        return badValue();
    }
    const std::string slot = freshName();
    line(joined("const GeasValue* ", slot, " = geas_list_get(&", base.name, ", (uint64_t)",
                position.name, ".as.i);"));
    line(joined("if (!", slot, ") ", failExit("GEAS_ERR_TYPE")));
    std::string value = freshName();
    line(joined("GeasValue ", value, " = *", slot, ";"));
    return copyOut(CValue{std::move(value), node.args[0]});
}

// Bytes use the List index rule, including the unsigned rejection of negatives
CValue CEmitter::genBytesIndex(const CValue& base, const CValue& position) {
    line(joined("if ((uint64_t)", position.name, ".as.i >= ", base.name,
                ".as.s.len) ", failExit("GEAS_ERR_TYPE")));
    std::string value = newValue();
    setTag(value, "GEAS_TY_BYTE");
    line(joined(value, ".as.b = ", base.name, ".as.s.ptr[(uint64_t)", position.name, ".as.i];"));
    return CValue{std::move(value), types().scalar(TyKind::Byte)};
}

// The propagation operator. On the failure tag, None or Err, the whole value
// returns through the out parameter; on success the payload lifts out of
// the box
CValue CEmitter::genQuestion(const Expr& expr, const QuestionExpr& question) {
    const CValue operand = genExpr(question.operand, TyId{});
    const TyNode node = nodeOf(operand.type);
    if (node.kind != TyKind::Result && node.kind != TyKind::Option) {
        unsupportedAt(expr, "'?' on this type");
        return badValue();
    }
    const std::string_view failTag = node.kind == TyKind::Result ? "1" : "0";
    // A course has no caller to hand the failure to, so it ends there
    if (course_ != nullptr) {
        line(joined("if (", operand.name, ".tag == ", failTag, ") return GEAS_COURSE_DONE;"));
    } else {
        line(joined("if (", operand.name, ".tag == ", failTag, ") { *out = ", operand.name,
                    "; return GEAS_OK; }"));
    }
    std::string value = freshName();
    line(joined("GeasValue ", value, " = *(const GeasValue*)", operand.name, ".as.box;"));
    return CValue{std::move(value), node.args[0]};
}

}  // namespace geas
