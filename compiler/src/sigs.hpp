// The signature tables the type checker reads, built from the resolved
// program before any body is walked. Every declared type annotation is
// lowered into the checked type arena with the grammar's type rules enforced
// at the annotation: type arguments belong to the builtin composites alone
// and arrive in the right number, Map keys stay in the keyable set, a user
// named type takes no arguments, and a pledge returns Result, Option, or
// Unit. A contract type and a pledge type are rejected at every boundary
// position, pledge, signal, and remedy signatures, vow types, record and
// variant fields, and provisional clause signatures; clause signatures and
// let annotations keep them. A name no declaration owns lowers to an opaque
// named type
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ast.hpp"
#include "diagnostics.hpp"
#include "loader.hpp"
#include "resolve.hpp"
#include "types.hpp"

namespace geas {

struct FieldSig {
    std::string name;
    TyId type;
};

struct RecordSig {
    std::vector<FieldSig> fields;
};

// A variant's payload and the sum that owns it
struct VariantSig {
    std::vector<FieldSig> fields;
    std::string owner;
};

struct VowSig {
    TyId type;
    bool hasInit;
};

// The lowered signature of a pledge, clause, signal, or remedy. A signal
// returns Unit
struct CallableSig {
    std::vector<TyId> params;
    TyId ret;
};

struct Sigs {
    std::unordered_map<std::string, RecordSig> records;
    std::unordered_map<std::string, VariantSig> variants;
    std::unordered_map<const VowDecl*, VowSig> vows;
    std::unordered_map<const PledgeDecl*, CallableSig> pledges;
    std::unordered_map<const ClauseDecl*, CallableSig> clauses;
    std::unordered_map<const ClauseSig*, CallableSig> provisionalClauses;
    std::unordered_map<const SignalDecl*, CallableSig> signals;
    std::unordered_map<const RemedyDecl*, CallableSig> remedies;

    // The fields of a record or a schema row by name, null when none has it
    const RecordSig* record(std::string_view name) const;

    // The payload of a variant by name, null when no sum declares it
    const VariantSig* variant(std::string_view name) const;
};

// Lowers surface type annotations into the arena. isABIPosition is set while
// a boundary signature lowers and currentPackage rides along for the
// internal contract rule, both owned by whoever is driving the lowering
class TypeLowering {
public:
    TypeLowering(const Ast& ast, const Resolved& resolved, TypeArena& types,
                 std::vector<Diagnostic>& diagnostics);

    /**
     * Lowers one annotation, reporting every rule the annotation breaks
     * against the span given, since a type node carries none of its own.
     * @params type - the annotation, lo - the span start, hi - the span end
     * @returns {TyId} - the checked type, unknown after a fault
     */
    TyId lower(TypeId type, std::int64_t lo, std::int64_t hi);

    /**
     * Lowers a parameter list, each type against the same span.
     * @params params - the declared parameters, lo - the span start, hi - the span end
     * @returns {std::vector<TyId>} - one checked type per parameter, in order
     */
    std::vector<TyId> lowerParams(const std::vector<Field>& params, std::int64_t lo,
                                  std::int64_t hi);

    bool isABIPosition = false;
    int currentPackage = 0;

private:
    TyId lowerNamed(const NamedType& named, std::int64_t lo, std::int64_t hi);
    TyId lowerBuiltin(const NamedType& named, std::int64_t lo, std::int64_t hi);
    TyId lowerMap(const NamedType& named, std::int64_t lo, std::int64_t hi);
    TyId lowerPair(const NamedType& named, std::int64_t lo, std::int64_t hi);
    TyId lowerDeclared(const NamedType& named, std::int64_t lo, std::int64_t hi);
    TyId lowerContract(const NamedType& named, const GlobalEntry& global, std::int64_t lo,
                       std::int64_t hi);
    void emit(std::int64_t lo, std::int64_t hi, std::string message);

    const Ast& ast_;
    const Resolved& resolved_;
    TypeArena& types_;
    std::vector<Diagnostic>& diagnostics_;
};

/**
 * Builds every signature table in declaration order, reporting the
 * annotation faults as it lowers.
 * @params program - the loaded program, types - the arena, lowering - the annotation lowering to
 * drive, diagnostics - where faults land
 * @returns {Sigs} - the lowered tables
 */
Sigs buildSigs(const Program& program, TypeArena& types, TypeLowering& lowering,
               std::vector<Diagnostic>& diagnostics);

/**
 * The canonical spelling of a signature, (P,P)->R, the string a signature
 * comparison, a sig hash, and a shape hash all read.
 * @params types - the arena, sig - the lowered signature
 * @returns {std::string} - the spelling
 */
std::string signatureCanon(const TypeArena& types, const CallableSig& sig);

}  // namespace geas
