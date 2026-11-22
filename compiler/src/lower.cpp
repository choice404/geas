// Annotation lowering, see sigs.hpp
#include "sigs.hpp"

#include <array>
#include <optional>
#include <utility>

#include "quote.hpp"

namespace geas {

namespace {

// The builtin scalars by their surface names
constexpr auto scalarNames = std::to_array<std::pair<std::string_view, TyKind>>({
    {"Int", TyKind::Int},
    {"UInt", TyKind::UInt},
    {"Float", TyKind::Float},
    {"Bool", TyKind::Bool},
    {"Byte", TyKind::Byte},
    {"Char", TyKind::Char},
    {"String", TyKind::String},
    {"Bytes", TyKind::Bytes},
    {"Unit", TyKind::Unit},
    {"Duration", TyKind::Duration},
    {"Time", TyKind::Time},
});

// The scalar a builtin name spells, nullopt for any other name
std::optional<TyKind> scalarKind(std::string_view name) {
    for (const auto& [spelling, kind] : scalarNames) {
        if (spelling == name) {
            return kind;
        }
    }
    return std::nullopt;
}

// Duration, Time, and Bytes allow declarations to take precedence over the builtin
bool isShadowableScalar(TyKind kind) {
    return kind == TyKind::Duration || kind == TyKind::Time || kind == TyKind::Bytes;
}

// The one type name the compiler brings in without a declaration behind
// it: StoreError, the Err side of every Store answer. Every other name a
// type annotation spells has to be declared
constexpr std::string_view storeErrorName = "StoreError";

// Whether a lowered type may key a Map, the unknown recovery type passing
bool isMapKeyable(TyKind kind) {
    switch (kind) {
        case TyKind::Int:
        case TyKind::UInt:
        case TyKind::Bool:
        case TyKind::Byte:
        case TyKind::Char:
        case TyKind::String:
        case TyKind::Unknown: return true;
        default: return false;
    }
}

}  // namespace

TypeLowering::TypeLowering(const Ast& ast, const Resolved& resolved, TypeArena& types,
                           std::vector<Diagnostic>& diagnostics)
    : ast_{ast}, resolved_{resolved}, types_{types}, diagnostics_{diagnostics} {}

void TypeLowering::emit(std::int64_t lo, std::int64_t hi, std::string message) {
    diagnostics_.push_back(Diagnostic{lo, hi, std::move(message)});
}

TyId TypeLowering::lower(TypeId type, std::int64_t lo, std::int64_t hi) {
    if (type.index < 0) {
        return types_.unknown();
    }
    const Type& node = ast_.type(type);
    if (const auto* named = std::get_if<NamedType>(&node)) {
        return lowerNamed(*named, lo, hi);
    }
    // The pledge type pledge(T, ...) -> R parses, so the spelling stays
    // reserved, but pledge values are left out of this version of the
    // language, so the type is refused wherever it appears
    emit(lo, hi, std::string{pledgeValueFault});
    return types_.unknown();
}

std::vector<TyId> TypeLowering::lowerParams(const std::vector<Field>& params, std::int64_t lo,
                                            std::int64_t hi) {
    std::vector<TyId> lowered;
    for (const Field& param : params) {
        lowered.push_back(lower(param.type, lo, hi));
    }
    return lowered;
}

TyId TypeLowering::lowerNamed(const NamedType& named, std::int64_t lo, std::int64_t hi) {
    const std::string& name = named.name;
    if (name.empty()) {
        return types_.unknown();
    }
    const std::optional<TyKind> scalar = scalarKind(name);
    const bool isShadowed =
        scalar && isShadowableScalar(*scalar) && resolved_.global(name) != nullptr;
    if (scalar && !isShadowed) {
        if (!named.args.empty()) {
            emit(lo, hi, "type " + quoted(name) + " takes no type arguments");
        }
        return types_.scalar(*scalar);
    }
    const bool isComposite =
        name == "List" || name == "Option" || name == "Map" || name == "Result" || name == "Tuple";
    return isComposite ? lowerBuiltin(named, lo, hi) : lowerDeclared(named, lo, hi);
}

// The builtin composites, each with its own argument count. A wrong count
// reports once and lowers none of the arguments
TyId TypeLowering::lowerBuiltin(const NamedType& named, std::int64_t lo, std::int64_t hi) {
    const std::string& name = named.name;
    const std::size_t count = named.args.size();
    const auto refuse = [&](std::string_view message) {
        emit(lo, hi, std::string{message});
        return types_.unknown();
    };
    if (name == "List") {
        return count == 1 ? types_.list(lower(named.args[0], lo, hi))
                          : refuse("List takes one type argument");
    }
    if (name == "Option") {
        return count == 1 ? types_.option(lower(named.args[0], lo, hi))
                          : refuse("Option takes one type argument");
    }
    if (name == "Map") {
        return count == 2 ? lowerMap(named, lo, hi) : refuse("Map takes two type arguments");
    }
    if (name == "Result") {
        return count == 2 ? lowerPair(named, lo, hi) : refuse("Result takes two type arguments");
    }
    if (count < 1) {
        return refuse("Tuple takes at least one type argument");
    }
    std::vector<TyId> elements;
    for (const TypeId element : named.args) {
        elements.push_back(lower(element, lo, hi));
    }
    return types_.tuple(std::move(elements));
}

TyId TypeLowering::lowerMap(const NamedType& named, std::int64_t lo, std::int64_t hi) {
    const TyId key = lower(named.args[0], lo, hi);
    const TyId value = lower(named.args[1], lo, hi);
    if (!isMapKeyable(types_.kindOf(key))) {
        emit(lo, hi, "a Map key must be Int, UInt, Bool, Byte, Char, or String");
    }
    return types_.map(key, value);
}

// Result<T, E>, the Ok side lowered before the Err side
TyId TypeLowering::lowerPair(const NamedType& named, std::int64_t lo, std::int64_t hi) {
    const TyId ok = lower(named.args[0], lo, hi);
    const TyId err = lower(named.args[1], lo, hi);
    return types_.result(ok, err);
}

// A user named type: a contract, which only some positions may name, a name
// that is not a type at all, a record or a sum, or StoreError, which the
// compiler brings in. A name nothing declares is a fault
TyId TypeLowering::lowerDeclared(const NamedType& named, std::int64_t lo, std::int64_t hi) {
    const std::string& name = named.name;
    if (!named.args.empty()) {
        emit(lo, hi, quoted(name) + " takes no type arguments");
    }
    const GlobalEntry* global = resolved_.global(name);
    if (global == nullptr && name == storeErrorName) {
        return types_.named(name);
    }
    if (global == nullptr) {
        emit(lo, hi, "unknown type " + quoted(name));
        return types_.unknown();
    }
    if (global->kind == GlobalKind::Contract) {
        return lowerContract(named, *global, lo, hi);
    }
    if (global->kind == GlobalKind::Variant) {
        emit(lo, hi, quoted(name) + " is a variant, not a type");
        return types_.unknown();
    }
    if (global->kind == GlobalKind::Provisional) {
        emit(lo, hi, quoted(name) + " is a provisional clause, not a type");
        return types_.unknown();
    }
    return types_.named(name);
}

// A signed instance has no tagged representation the boundary could carry,
// so a contract type is refused wherever a value crosses it, and an internal
// contract is nameable only inside its own package
TyId TypeLowering::lowerContract(const NamedType& named, const GlobalEntry& global, std::int64_t lo,
                                 std::int64_t hi) {
    const std::string& name = named.name;
    if (isABIPosition) {
        emit(lo, hi, "contract type " + quoted(name) + " cannot cross the contract boundary");
        return types_.unknown();
    }
    const ContractDecl* contract = contractAt(ast_, global.declIndex);
    if (contract != nullptr && contract->isInternal && global.package != currentPackage) {
        emit(lo, hi, "internal contract " + quoted(name) + " is not visible outside its package");
        return types_.unknown();
    }
    return types_.contract(name);
}

}  // namespace geas
