// The signature table builder, see sigs.hpp
#include "sigs.hpp"

#include <unordered_set>
#include <utility>

#include "quote.hpp"

namespace geas {

namespace {

// Whether a lowered type is one of the seven scalars a store column may hold
bool isScalarColumn(TyKind kind) {
    switch (kind) {
        case TyKind::Int:
        case TyKind::UInt:
        case TyKind::Float:
        case TyKind::Bool:
        case TyKind::Byte:
        case TyKind::Char:
        case TyKind::String:
        case TyKind::Unknown: return true;
        default: return false;
    }
}

class SigBuilder {
public:
    SigBuilder(const Program& program, TypeArena& types, TypeLowering& lowering,
               std::vector<Diagnostic>& diagnostics)
        : program_{program}, types_{types}, lowering_{lowering}, diagnostics_{diagnostics} {}

    Sigs run() &&;

private:
    void emit(std::int64_t lo, std::int64_t hi, std::string message);
    std::vector<FieldSig> lowerFields(const std::vector<Field>& fields, std::int64_t lo,
                                      std::int64_t hi);
    CallableSig lowerBoundary(const std::vector<Field>& params, TypeId ret, std::int64_t lo,
                              std::int64_t hi);
    void collectRecord(const RecordDecl& record);
    void collectSum(const SumDecl& sum);
    void collectProvisional(const ProvisionalDecl& provisional);
    void collectContract(const ContractDecl& contract);
    void collectSchema(const SchemaDecl& schema);
    void collectPledge(const PledgeDecl& pledge);
    void collectSignal(const SignalDecl& signal);
    void collectRemedy(const RemedyDecl& remedy);
    void collectSub(const SubcontractDecl& sub);

    const Program& program_;
    TypeArena& types_;
    TypeLowering& lowering_;
    std::vector<Diagnostic>& diagnostics_;
    Sigs sigs_;
};

void SigBuilder::emit(std::int64_t lo, std::int64_t hi, std::string message) {
    diagnostics_.push_back(Diagnostic{lo, hi, std::move(message)});
}

// Lowers a field list at a boundary position
std::vector<FieldSig> SigBuilder::lowerFields(const std::vector<Field>& fields, std::int64_t lo,
                                              std::int64_t hi) {
    std::vector<FieldSig> lowered;
    lowering_.isABIPosition = true;
    for (const Field& field : fields) {
        lowered.push_back(FieldSig{field.name, lowering_.lower(field.type, lo, hi)});
    }
    lowering_.isABIPosition = false;
    return lowered;
}

// Lowers a signature whose values cross the contract boundary
CallableSig SigBuilder::lowerBoundary(const std::vector<Field>& params, TypeId ret, std::int64_t lo,
                                      std::int64_t hi) {
    lowering_.isABIPosition = true;
    std::vector<TyId> lowered = lowering_.lowerParams(params, lo, hi);
    const TyId loweredRet = lowering_.lower(ret, lo, hi);
    lowering_.isABIPosition = false;
    return CallableSig{std::move(lowered), loweredRet};
}

void SigBuilder::collectRecord(const RecordDecl& record) {
    if (sigs_.records.contains(record.name)) {
        return;
    }
    sigs_.records.emplace(record.name, RecordSig{lowerFields(record.fields, record.lo, record.hi)});
}

void SigBuilder::collectSum(const SumDecl& sum) {
    for (const Variant& variant : sum.variants) {
        if (sigs_.variants.contains(variant.name)) {
            continue;
        }
        sigs_.variants.emplace(variant.name,
                               VariantSig{lowerFields(variant.fields, sum.lo, sum.hi), sum.name});
    }
}

void SigBuilder::collectProvisional(const ProvisionalDecl& provisional) {
    // A repeated signature name keeps its first lowering, the one lookups read
    std::unordered_set<std::string> seen;
    for (const ClauseSig& sig : provisional.sigs) {
        if (!seen.insert(sig.name).second) {
            continue;
        }
        sigs_.provisionalClauses.emplace(
            &sig, lowerBoundary(sig.params, sig.ret, provisional.lo, provisional.hi));
    }
}

// A schema's row record: its columns lower the way a record's fields do, and
// each is held to the seven scalars a flat row can store
void SigBuilder::collectSchema(const SchemaDecl& schema) {
    if (sigs_.records.contains(schema.name)) {
        return;
    }
    std::vector<FieldSig> columns;
    lowering_.isABIPosition = true;
    for (const Field& column : schema.columns) {
        const TyId type = lowering_.lower(column.type, schema.lo, schema.hi);
        if (!isScalarColumn(types_.kindOf(type))) {
            emit(schema.lo, schema.hi,
                 "schema column " + quoted(column.name) +
                     " must be a scalar type (Int, UInt, Float, Bool, Byte, Char, or String)");
        }
        columns.push_back(FieldSig{column.name, type});
    }
    lowering_.isABIPosition = false;
    sigs_.records.emplace(schema.name, RecordSig{std::move(columns)});
}

// The pledge return rule: Result, Option, or Unit, unknown passing after an
// earlier fault in the annotation
void SigBuilder::collectPledge(const PledgeDecl& pledge) {
    CallableSig sig = lowerBoundary(pledge.params, pledge.ret, pledge.lo, pledge.hi);
    const TyKind kind = types_.kindOf(sig.ret);
    const bool isAllowed = kind == TyKind::Result || kind == TyKind::Option ||
                           kind == TyKind::Unit || kind == TyKind::Unknown;
    if (!isAllowed) {
        emit(pledge.lo, pledge.hi,
             "pledge " + quoted(pledge.name) + " must return Result, Option, or Unit");
    }
    sigs_.pledges.emplace(&pledge, std::move(sig));
}

void SigBuilder::collectSignal(const SignalDecl& signal) {
    lowering_.isABIPosition = true;
    std::vector<TyId> params = lowering_.lowerParams(signal.params, signal.lo, signal.hi);
    lowering_.isABIPosition = false;
    sigs_.signals.emplace(&signal, CallableSig{std::move(params), types_.scalar(TyKind::Unit)});
}

void SigBuilder::collectRemedy(const RemedyDecl& remedy) {
    std::vector<Field> params;
    if (remedy.param) {
        params.push_back(*remedy.param);
    }
    sigs_.remedies.emplace(&remedy, lowerBoundary(params, remedy.ret, remedy.lo, remedy.hi));
}

void SigBuilder::collectSub(const SubcontractDecl& sub) {
    for (const SubMember& member : sub.members) {
        if (const auto* pledge = std::get_if<PledgeDecl>(&member)) {
            collectPledge(*pledge);
        } else if (const auto* signal = std::get_if<SignalDecl>(&member)) {
            collectSignal(*signal);
        }
    }
}

void SigBuilder::collectContract(const ContractDecl& contract) {
    for (const Member& member : contract.members) {
        if (const auto* schema = std::get_if<SchemaDecl>(&member)) {
            collectSchema(*schema);
        } else if (const auto* vow = std::get_if<VowDecl>(&member)) {
            lowering_.isABIPosition = true;
            const TyId type = lowering_.lower(vow->type, vow->lo, vow->hi);
            lowering_.isABIPosition = false;
            sigs_.vows.emplace(vow, VowSig{type, vow->init.has_value()});
        } else if (const auto* pledge = std::get_if<PledgeDecl>(&member)) {
            collectPledge(*pledge);
        } else if (const auto* clause = std::get_if<ClauseDecl>(&member)) {
            std::vector<TyId> params =
                lowering_.lowerParams(clause->params, clause->lo, clause->hi);
            const TyId ret = lowering_.lower(clause->ret, clause->lo, clause->hi);
            sigs_.clauses.emplace(clause, CallableSig{std::move(params), ret});
        } else if (const auto* sub = std::get_if<SubcontractDecl>(&member)) {
            collectSub(*sub);
        } else if (const auto* signal = std::get_if<SignalDecl>(&member)) {
            collectSignal(*signal);
        } else if (const auto* remedy = std::get_if<RemedyDecl>(&member)) {
            collectRemedy(*remedy);
        }
    }
}

// Records, sums, provisional clauses, and contract members, all in
// declaration order so the faults read back in source order
Sigs SigBuilder::run() && {
    const Ast& ast = program_.ast;
    for (std::size_t i = 0; i < ast.decls.size(); ++i) {
        lowering_.currentPackage = program_.declPackages[i];
        const Decl& decl = ast.decls[i];
        if (const auto* record = std::get_if<RecordDecl>(&decl)) {
            collectRecord(*record);
        } else if (const auto* sum = std::get_if<SumDecl>(&decl)) {
            collectSum(*sum);
        } else if (const auto* provisional = std::get_if<ProvisionalDecl>(&decl)) {
            collectProvisional(*provisional);
        } else if (const auto* contract = std::get_if<ContractDecl>(&decl)) {
            collectContract(*contract);
        }
    }
    return std::move(sigs_);
}

}  // namespace

const RecordSig* Sigs::record(std::string_view name) const {
    const auto found = records.find(std::string{name});
    return found == records.end() ? nullptr : &found->second;
}

const VariantSig* Sigs::variant(std::string_view name) const {
    const auto found = variants.find(std::string{name});
    return found == variants.end() ? nullptr : &found->second;
}

Sigs buildSigs(const Program& program, TypeArena& types, TypeLowering& lowering,
               std::vector<Diagnostic>& diagnostics) {
    return SigBuilder{program, types, lowering, diagnostics}.run();
}

std::string signatureCanon(const TypeArena& types, const CallableSig& sig) {
    std::string out{"("};
    for (std::size_t i = 0; i < sig.params.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        out.append(types.canon(sig.params[i]));
    }
    out.append(")->");
    out.append(types.canon(sig.ret));
    return out;
}

}  // namespace geas
