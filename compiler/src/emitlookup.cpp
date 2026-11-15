// The lookups the emitters share, see emitlookup.hpp
#include "emitlookup.hpp"

#include <utility>

#include "emittext.hpp"

namespace geas {

EmitLookup::EmitLookup(CheckedProgram& checked, std::vector<Diagnostic>& diagnostics)
    : checked_{checked}, diagnostics_{diagnostics} {}

void EmitLookup::fault(std::int64_t lo, std::int64_t hi, std::string message) const {
    diagnostics_.push_back(Diagnostic{lo, hi, std::move(message)});
}

void EmitLookup::unsupported(std::int64_t lo, std::int64_t hi, std::string_view what) const {
    fault(lo, hi, joined("codegen: not yet supported: ", what));
}

std::vector<const ContractShape*> EmitLookup::publicContracts() const {
    std::vector<const ContractShape*> shapes;
    for (const ContractShape& shape : checked_.contracts) {
        if (!shape.decl->isInternal) {
            shapes.push_back(&shape);
        }
    }
    return shapes;
}

int EmitLookup::packageOf(const ContractDecl& contract) const {
    const GlobalEntry* global = resolved().global(contract.name);
    return global == nullptr ? 0 : global->package;
}

void EmitLookup::checkVersionAttr(const ContractDecl& contract) const {
    for (const Attr& attr : contract.attrs) {
        if (attr.key != "version") {
            continue;
        }
        const Expr& value = ast().expr(attr.value);
        if (std::holds_alternative<IntLiteral>(value.kind)) {
            return;
        }
        fault(value.lo, value.hi, "codegen: the version attribute must be an integer literal");
    }
}

TyKind EmitLookup::kindOf(TyId type) const {
    return type.isSet() ? types().kindOf(type) : TyKind::Unknown;
}

std::optional<GlobalKind> EmitLookup::namedKind(TyId type) const {
    if (kindOf(type) != TyKind::Named) {
        return std::nullopt;
    }
    const GlobalEntry* global = resolved().global(types().nameOf(type));
    if (global == nullptr) {
        return std::nullopt;
    }
    return global->kind;
}

const ContractDecl* EmitLookup::contractOf(TyId type) const {
    if (kindOf(type) != TyKind::Contract) {
        return nullptr;
    }
    const GlobalEntry* global = resolved().global(types().nameOf(type));
    if (global == nullptr || global->kind != GlobalKind::Contract) {
        return nullptr;
    }
    return contractAt(ast(), global->declIndex);
}

const SumDecl* EmitLookup::sumOf(TyId type) const {
    if (namedKind(type) != GlobalKind::Sum) {
        return nullptr;
    }
    const GlobalEntry* global = resolved().global(types().nameOf(type));
    return std::get_if<SumDecl>(&ast().decls[static_cast<std::size_t>(global->declIndex)]);
}

std::optional<FieldSlot> EmitLookup::fieldOf(std::string_view record,
                                             std::string_view field) const {
    const RecordSig* sig = sigs().record(record);
    if (sig == nullptr) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < sig->fields.size(); ++i) {
        if (sig->fields[i].name == field) {
            return FieldSlot{static_cast<std::int64_t>(i), sig->fields[i].type};
        }
    }
    return std::nullopt;
}

std::optional<VariantSlot> EmitLookup::variantOf(std::string_view name) const {
    const VariantSig* sig = sigs().variant(name);
    if (sig == nullptr) {
        return std::nullopt;
    }
    const GlobalEntry* owner = resolved().global(sig->owner);
    if (owner == nullptr || owner->kind != GlobalKind::Sum) {
        return std::nullopt;
    }
    const auto* sum =
        std::get_if<SumDecl>(&ast().decls[static_cast<std::size_t>(owner->declIndex)]);
    if (sum == nullptr) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < sum->variants.size(); ++i) {
        if (sum->variants[i].name == name) {
            return VariantSlot{static_cast<std::int64_t>(i), sig};
        }
    }
    return std::nullopt;
}

std::string_view EmitLookup::tagOf(TyId type) const {
    switch (kindOf(type)) {
        // Duration and Time cross as Int, milliseconds and epoch milliseconds
        case TyKind::Int:
        case TyKind::Duration:
        case TyKind::Time: return "GEAS_TY_INT";
        case TyKind::UInt: return "GEAS_TY_UINT";
        case TyKind::Float: return "GEAS_TY_FLOAT";
        case TyKind::Bool: return "GEAS_TY_BOOL";
        case TyKind::Byte: return "GEAS_TY_BYTE";
        case TyKind::Char: return "GEAS_TY_CHAR";
        case TyKind::String: return "GEAS_TY_STRING";
        case TyKind::Unit: return "GEAS_TY_UNIT";
        case TyKind::List: return "GEAS_TY_LIST";
        case TyKind::Map: return "GEAS_TY_MAP";
        case TyKind::Tuple: return "GEAS_TY_TUPLE";
        case TyKind::Option: return "GEAS_TY_OPTION";
        case TyKind::Result: return "GEAS_TY_RESULT";
        case TyKind::Contract: return "GEAS_TY_INSTANCE";
        default: break;
    }
    const std::optional<GlobalKind> kind = namedKind(type);
    if (kind == GlobalKind::Record) {
        return "GEAS_TY_RECORD";
    }
    if (kind == GlobalKind::Sum) {
        return "GEAS_TY_SUM";
    }
    return "";
}

std::string_view EmitLookup::unionFieldOf(TyId type) const {
    switch (kindOf(type)) {
        case TyKind::Int:
        case TyKind::Duration:
        case TyKind::Time: return "i";
        case TyKind::UInt: return "u";
        case TyKind::Float: return "f";
        case TyKind::Bool: return "b";
        case TyKind::Byte: return "b";
        case TyKind::Char: return "ch";
        default: return "";
    }
}

TyId EmitLookup::vowType(const VowDecl& vow) const {
    const auto found = sigs().vows.find(&vow);
    return found == sigs().vows.end() ? types().unknown() : found->second.type;
}

CallableSig EmitLookup::pledgeSig(const PledgeDecl& pledge) const {
    const auto found = sigs().pledges.find(&pledge);
    return found == sigs().pledges.end() ? CallableSig{{}, types().unknown()} : found->second;
}

CallableSig EmitLookup::clauseSig(const ClauseDecl& clause) const {
    const auto found = sigs().clauses.find(&clause);
    return found == sigs().clauses.end() ? CallableSig{{}, types().unknown()} : found->second;
}

CallableSig EmitLookup::remedySig(const RemedyDecl& remedy) const {
    const auto found = sigs().remedies.find(&remedy);
    return found == sigs().remedies.end() ? CallableSig{{}, types().unknown()} : found->second;
}

CallableSig EmitLookup::signalSig(const SignalDecl& signal) const {
    const auto found = sigs().signals.find(&signal);
    return found == sigs().signals.end() ? CallableSig{{}, types().unknown()} : found->second;
}

}  // namespace geas
