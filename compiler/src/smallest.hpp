// The smallest value of a course slot's type. The runtime starts every slot
// of a course frame at the smallest value of its type when an instance
// signs, so the compiler holds each slot type to the lines the runtime's
// registrar holds: the type has a finite value at all, its smallest value
// nests no deeper than a value may, and it holds no more nodes than one slot
// may start with.
//
// Both measures are least fixed points over the declared records and sums a
// slot type reaches, computed the way the runtime computes them. The height
// of a type is the fewest levels any finite value of it nests: 1 for a
// scalar, a String, Bytes, Unit, an empty List or Map, and a None; one more
// than the deepest field for a Tuple and a record; one more than the arm that
// nests least for a Result, Ok on a tie; one more than the variant whose
// fields nest least for a sum, the first of equals. The node count of the
// smallest value is 1 for each of the same leaves, and 1 plus the nodes of
// every kid the value carries otherwise: every field of a Tuple and a
// record, the fields of the variant the height picks for a sum, and the arm
// the height picks for a Result, the very value the runtime builds. Every
// name starts with no finite value and each pass recomputes every name from
// the values of the pass before, so a name only ever comes down; a smallest
// value never holds a name twice down one path, so one pass per name
// reaches the fixed point and the pass after it changes nothing. Node
// counts saturate one past the limit, so a type whose smallest value is
// exponential in its declarations costs no more than any other
#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "emitlookup.hpp"

namespace geas {

// The most nodes the smallest value of a course slot's type may hold. The
// runtime registrar refuses a contract past it, and this value has to equal
// GEAS_SLOT_MIN_NODES_MAX in runtime/include/geas/geas_abi.h
inline constexpr std::uint64_t slotMinNodesMax = 4096;

// The deepest a value may nest, counted in values from the outermost to the
// innermost. This value has to equal GEAS_VALUE_MAX_DEPTH in
// runtime/include/geas/geas_abi.h
inline constexpr std::uint64_t valueMaxDepth = 256;

// The height and the node count of a type with no finite value
inline constexpr std::uint64_t noFiniteValue = std::numeric_limits<std::uint64_t>::max();

// The measures of one type's smallest value. nodes saturates at
// slotMinNodesMax + 1, so any count past the limit reads as that one
struct SmallestValue {
    std::uint64_t height = noFiniteValue;
    std::uint64_t nodes = noFiniteValue;

    bool isFinite() const { return height != noFiniteValue; }
};

// The fixed points over every declared record and sum a set of root types
// reaches, built once and then read for any type over those names
class SmallestValues {
public:
    /**
     * Settles the height and node count of every record and sum the roots reach.
     * @params lookup - the program lookups, roots - the slot types to measure
     */
    SmallestValues(const EmitLookup& lookup, const std::vector<TyId>& roots);

    /**
     * The measures of one type's smallest value.
     * @params type - a root, or any type over the names the roots reach
     * @returns {SmallestValue} - its height and node count, both noFiniteValue when it has none
     */
    SmallestValue of(TyId type) const;

private:
    void reach(TyId type);
    void reachFields(const std::vector<FieldSig>& fields);
    void settleHeights();
    void settleNodes();
    std::uint64_t heightOf(TyId type) const;
    std::uint64_t nodesOf(TyId type) const;
    std::uint64_t declaredHeight(TyId type) const;
    std::uint64_t declaredNodes(TyId type) const;
    std::uint64_t deepestField(const std::vector<FieldSig>& fields) const;
    std::uint64_t fieldNodes(const std::vector<FieldSig>& fields) const;
    const std::vector<FieldSig>* leastVariant(const SumDecl& sum) const;
    const RecordSig* recordOf(TyId type) const;
    const std::vector<FieldSig>* payloadOf(const Variant& variant, const SumDecl& sum) const;

    const EmitLookup& lookup_;
    std::unordered_map<std::string, std::size_t> indices_;
    std::vector<TyId> declared_;
    std::vector<std::uint64_t> heights_;
    std::vector<std::uint64_t> nodes_;
};

}  // namespace geas
