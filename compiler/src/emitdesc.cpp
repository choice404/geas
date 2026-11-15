// The descriptor tables of one contract: the schema columns, the vows with
// their defaults, the pledges with their mangled names, the subcontract
// flag bytes, and the contract descriptor that points at all of them. A
// table the contract has no entries for is not emitted and the descriptor
// references NULL in its place
#include "emitc.hpp"
#include "emittext.hpp"
#include "hash.hpp"

namespace geas {

namespace {

constexpr std::string_view noDefaultText = "{ .ty = GEAS_TY_UNIT, .tag = 0, .as = { .i = 0 } }";

std::string countOf(std::size_t count) {
    return decimal(static_cast<std::int64_t>(count));
}

// A table's symbol when it has entries, NULL when it was not emitted
std::string tableRef(std::string_view contract, std::string_view suffix, std::int64_t count) {
    return count > 0 ? joined("k_", contract, suffix) : std::string{"NULL"};
}

// The number a scalar literal carries, a Bool as 0 or 1 and a Char as its
// scalar
std::int64_t literalNumber(const Expr& literal) {
    if (const auto* integer = std::get_if<IntLiteral>(&literal.kind)) {
        return integer->value;
    }
    if (const auto* boolean = std::get_if<BoolLiteral>(&literal.kind)) {
        return boolean->value ? 1 : 0;
    }
    if (const auto* character = std::get_if<CharLiteral>(&literal.kind)) {
        return character->scalar;
    }
    if (const auto* duration = std::get_if<DurationLiteral>(&literal.kind)) {
        return duration->milliseconds;
    }
    return 0;
}

}  // namespace

// Emits one GeasSchemaCol array per schema, its columns in declaration
// order at their scalar tags, then the GeasSchemaDesc array the contract
// descriptor points at. The row shape is read back from the schema's record
// signature, the one the row record and the shape hash share
void CEmitter::emitSchemas(const ContractShape& shape) {
    if (shape.schemas.empty()) {
        return;
    }
    const std::string& contract = shape.decl->name;
    for (const SchemaDecl* schema : shape.schemas) {
        top(joined("static const GeasSchemaCol k_", contract, "_", schema->name, "_cols[] = {"));
        const RecordSig* row = lookup_.sigs().record(schema->name);
        for (std::size_t i = 0; row != nullptr && i < row->fields.size(); ++i) {
            const FieldSig& column = row->fields[i];
            top(joined("    { \"", column.name, "\", ", lookup_.tagOf(column.type), " },"));
        }
        top("};");
        top("");
    }
    top(joined("static const GeasSchemaDesc k_", contract, "_schemas[] = {"));
    for (const SchemaDecl* schema : shape.schemas) {
        const RecordSig* row = lookup_.sigs().record(schema->name);
        const std::size_t columns = row == nullptr ? 0 : row->fields.size();
        top(joined("    { \"", schema->name, "\", ", countOf(columns), ", k_", contract, "_",
                   schema->name, "_cols },"));
    }
    top("};");
    top("");
}

// The vow default initializer text for the descriptor table
std::string CEmitter::vowDefaultText(const VowDecl& vow, TyId type) const {
    const Expr* literal = &ast().expr(*vow.init);
    bool isNegated = false;
    if (const auto* unary = std::get_if<UnaryExpr>(&literal->kind)) {
        isNegated = true;
        literal = &ast().expr(unary->operand);
    }
    std::string value;
    if (kindOf(type) == TyKind::String) {
        const auto* text = std::get_if<StringLiteral>(&literal->kind);
        const CString lowered = cString(text == nullptr ? std::string_view{} : text->body);
        value = joined(".s = { (uint8_t*)", lowered.literal, ", ", decimal(lowered.length), " }");
    } else if (kindOf(type) == TyKind::Float) {
        const auto* number = std::get_if<FloatLiteral>(&literal->kind);
        const std::string spelling =
            number == nullptr ? decimal(literalNumber(*literal)) : number->spelling;
        value = joined(".f = ", isNegated ? "-" : "", spelling);
    } else {
        const std::int64_t number = literalNumber(*literal);
        value =
            joined(".", lookup_.unionFieldOf(type), " = ", decimal(isNegated ? -number : number));
    }
    return joined("{ .ty = ", lookup_.tagOf(type), ", .tag = 0, .as = { ", value, " } }");
}

// Emits the vow descriptor table of one contract. A vow whose type is not
// one union field or a String is outside what a descriptor can carry
void CEmitter::emitVows(const ContractShape& shape) {
    if (shape.vows.empty()) {
        return;
    }
    top(joined("static const GeasVowDesc k_", shape.decl->name, "_vows[] = {"));
    for (const VowDecl* vow : shape.vows) {
        const TyId type = lookup_.vowType(*vow);
        const bool isCarriable =
            !lookup_.unionFieldOf(type).empty() || kindOf(type) == TyKind::String;
        if (!isCarriable) {
            lookup_.unsupported(vow->lo, vow->hi, "a vow of this type in a compiled module");
            continue;
        }
        const std::string init =
            vow->init ? vowDefaultText(*vow, type) : std::string{noDefaultText};
        top(joined("    { \"", vow->name, "\", ", lookup_.tagOf(type), ", ", vow->init ? "1" : "0",
                   ", ", init, " },"));
    }
    top("};");
    top("");
}

// The pledge table, every pledge body or not in declaration order. An
// abstract pledge carries NULL where a compiled one carries its thunk
void CEmitter::emitPledgeTable(const ContractShape& shape) {
    if (shape.pledges.empty() && shape.signals.empty()) {
        return;
    }
    top(joined("static const GeasPledgeDesc k_", shape.decl->name, "_pledges[] = {"));
    for (const PledgeShape& pledge : shape.pledges) {
        const std::string_view thunk =
            pledge.decl->body ? std::string_view{pledge.mangled} : "NULL";
        top(joined("    { \"", pledge.decl->name, "\", \"", pledge.mangled, "\", ",
                   countOf(pledge.decl->params.size()), ", ", thunk, ", ",
                   decimal(pledge.subOrdinal), " },"));
    }
    // A signal's row: nothing runs and nothing binds, so no thunk and no
    // mangled name, and its kind byte marks it
    for (const SignalShape& signal : shape.signals) {
        top(joined("    { \"", signal.decl->name, "\", NULL, ", countOf(signal.decl->params.size()),
                   ", NULL, ", decimal(signal.subOrdinal), " },"));
    }
    top("};");
    top("");
}

// The subcontract flag bytes, one per subcontract in declaration order. The
// array is emitted only when a subcontract wears the transactional modifier,
// so a contract without one references NULL
std::string CEmitter::emitSubFlags(const ContractShape& shape) {
    bool hasTransactional = false;
    for (const SubcontractDecl* sub : shape.subs) {
        hasTransactional = hasTransactional || sub->isTransactional;
    }
    if (!hasTransactional) {
        return "NULL";
    }
    top(joined("static const uint8_t k_", shape.decl->name, "_sub_flags[] = {"));
    for (const SubcontractDecl* sub : shape.subs) {
        top(joined("    ", sub->isTransactional ? "GEAS_SUB_TRANSACTIONAL" : "0", ","));
    }
    top("};");
    top("");
    return joined("k_", shape.decl->name, "_sub_flags");
}

void CEmitter::emitDescriptor(const ContractShape& shape, const RequirementCounts& counts,
                              std::string_view subFlagsRef, const std::vector<std::string>& tail) {
    const std::string& name = shape.decl->name;
    const auto pledges = static_cast<std::int64_t>(shape.pledges.size() + shape.signals.size());
    const auto vows = static_cast<std::int64_t>(shape.vows.size());
    const auto schemas = static_cast<std::int64_t>(shape.schemas.size());
    top(joined("static const GeasContractDesc k_", name, " = {"));
    top(joined("    \"", name, "\", 0x", hashHex16(shape.shapeHash), "ULL,"));
    top(joined("    ", decimal(shape.version), ", ", decimal(pledges), ", ",
               tableRef(name, "_pledges", pledges), ","));
    top(joined("    ", decimal(vows), ", ", tableRef(name, "_vows", vows), ","));
    top(joined("    ", decimal(counts.subs), ", ", tableRef(name, "_subs", counts.subs), ", ",
               decimal(counts.atoms), ","));
    top(joined("    ", tableRef(name, "_atoms", counts.atoms), ", ", counts.hasBlock ? "1" : "0",
               ","));
    top(joined("    ", decimal(counts.fulfill), ", ", decimal(counts.partial), ", ",
               decimal(counts.breakLine), ","));
    top(joined("    ", tableRef(name, "_rf", counts.fulfill), ", ",
               tableRef(name, "_rp", counts.partial), ", ", tableRef(name, "_rb", counts.breakLine),
               ","));
    top(joined("    ", decimal(schemas), ", ", tableRef(name, "_schemas", schemas), ","));
    top(joined("    ", subFlagsRef, ","));
    for (const std::string& text : tail) {
        top(text);
    }
    top("};");
    top("");
}

}  // namespace geas
