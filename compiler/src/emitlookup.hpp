// What every emitter asks of a checked program: where a named type was
// declared, which slot a record field or a sum variant sits in, the ABI tag
// and the union field of a type, and the lowered signature of a member. One
// EmitLookup rides through an emitter with the fault vector code generation
// errors land in. A construct an emitter does not lower is a build error
// here, never silently wrong output
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "check.hpp"
#include "diagnostics.hpp"

namespace geas {

// A record field resolved by name, the slot the ABI stores it in and its type
struct FieldSlot {
    std::int64_t index;
    TyId type;
};

// A variant resolved by name: its declaration index within the owning sum,
// which is the ABI tag a sum value carries, and its payload
struct VariantSlot {
    std::int64_t index;
    const VariantSig* sig;
};

class EmitLookup {
public:
    EmitLookup(CheckedProgram& checked, std::vector<Diagnostic>& diagnostics);

    const Ast& ast() const { return checked_.program.ast; }
    const Resolved& resolved() const { return checked_.resolved; }
    const Sigs& sigs() const { return checked_.sigs; }
    TypeArena& types() const { return checked_.types; }
    const CheckTables& tables() const { return checked_.tables; }

    // Records a build fault over a span
    void fault(std::int64_t lo, std::int64_t hi, std::string message) const;

    // The named build error for a construct outside the lowered subset
    void unsupported(std::int64_t lo, std::int64_t hi, std::string_view what) const;

    bool hasFaults() const { return !diagnostics_.empty(); }

    /**
     * The contracts a module publishes, in declaration order. An internal
     * contract stays inside its package, so no emitter names it.
     * @returns {std::vector<const ContractShape*>} - the shapes of the public contracts
     */
    std::vector<const ContractShape*> publicContracts() const;

    // The package of the file that declared a contract
    int packageOf(const ContractDecl& contract) const;

    // Reports a version attribute that is not an integer literal
    void checkVersionAttr(const ContractDecl& contract) const;

    // The kind of a checked type, Unknown for an unset id
    TyKind kindOf(TyId type) const;

    // The global kind behind a named type, nullopt for any other type or a
    // name nothing declares
    std::optional<GlobalKind> namedKind(TyId type) const;

    // The contract behind an instance type, null when the program declares none
    const ContractDecl* contractOf(TyId type) const;

    // The sum a named type was declared as, null for anything else
    const SumDecl* sumOf(TyId type) const;

    // A field of a record or a schema row by name
    std::optional<FieldSlot> fieldOf(std::string_view record, std::string_view field) const;

    // A variant by name, with its tag in the owning sum
    std::optional<VariantSlot> variantOf(std::string_view name) const;

    /**
     * The GeasTypeTag spelling of a checked type.
     * @params type - the checked type
     * @returns {std::string_view} - the tag, empty for a shape the ABI does not tag one to one
     */
    std::string_view tagOf(TyId type) const;

    /**
     * The GeasValue union field a scalar of this type lives in.
     * @params type - the checked type
     * @returns {std::string_view} - the field, empty for a type that is not one union field
     */
    std::string_view unionFieldOf(TyId type) const;

    // The lowered signatures, unknown typed for a declaration the tables miss
    TyId vowType(const VowDecl& vow) const;
    CallableSig pledgeSig(const PledgeDecl& pledge) const;
    CallableSig clauseSig(const ClauseDecl& clause) const;
    CallableSig remedySig(const RemedyDecl& remedy) const;
    CallableSig signalSig(const SignalDecl& signal) const;

private:
    CheckedProgram& checked_;
    std::vector<Diagnostic>& diagnostics_;
};

}  // namespace geas
