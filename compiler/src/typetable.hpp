// The type table of one contract, the deep description of every value its
// boundary carries. The runtime holds an argument, a return, a signal
// payload, a vow binding, and a remedy's value to the declared type all the
// way down by walking this table, so it spells each type the way the value
// layout stores it.
//
// The walk starts at the vows in declaration order, then each pledge's
// parameters and return, then each signal's parameters, then each remedy's
// parameter and return, the order the descriptor tables list them in. A
// type takes its index the first time the walk meets it, before its kids are
// walked, so a recursive type refers back to its own index. Every distinct
// type appears once, distinct as a conforming value sees it: Duration, Time,
// and Int share one node.
//
// Each node carries the tag a conforming value has: Duration and Time are
// Int, a declared record is a Record whose kids are its fields in
// declaration order, a schema row is the Record of its columns, and a
// declared sum is a Sum whose variants list their payload fields. A contract
// instance and a first class pledge never reach the table, the checker keeps
// both out of every boundary position. A name no declaration owns has no
// layout the table could spell, so a slot whose type reaches one carries
// noTypeIndex and its value is held to the structural facts alone
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "emitlookup.hpp"
#include "shape.hpp"

namespace geas {

// The index a slot carries when its type has no spelling in the table, the
// GEAS_NO_TYPE of the ABI, and the return of every signal
inline constexpr std::uint32_t noTypeIndex = 0xFFFFFFFFu;

// One variant of a sum: its name and the table index of each payload field
struct TypeTableVariant {
    std::string name;
    std::vector<std::uint32_t> fields;
};

// One node of the table. tag is the GeasTypeTag spelling, name the record or
// sum name and empty otherwise, variants filled for a sum alone
struct TypeTableNode {
    std::string_view tag;
    std::vector<std::uint32_t> kids;
    std::string name;
    std::vector<TypeTableVariant> variants;
};

// The parameter indices and the return index of one item
struct TypeTableItem {
    std::vector<std::uint32_t> params;
    std::uint32_t ret = noTypeIndex;
};

// The whole table of one contract. items lists the pledges then the
// signals, the pledges table's order; remedies holds the 0 or 1 parameter
// the remedy thunk is handed and the remedy's return
struct ContractTypes {
    std::vector<TypeTableNode> nodes;
    std::vector<TypeTableItem> items;
    std::vector<std::uint32_t> vows;
    std::vector<TypeTableItem> remedies;
};

/**
 * Whether a remedy is handed its target's Ok value: it declared the
 * parameter and the target answers a Result whose Ok carries a value.
 * @params lookup - the program lookups, remedy - the remedy
 * @returns {bool} - true when the remedy thunk takes one argument
 */
bool remedyTakesValue(const EmitLookup& lookup, const RemedyShape& remedy);

/**
 * Builds the type table of one contract. A contract instance or pledge type
 * reached at the boundary is a build fault recorded through the lookup.
 * @params lookup - the program lookups, shape - the contract
 * @returns {ContractTypes} - the table, every index in range or noTypeIndex
 */
ContractTypes buildContractTypes(const EmitLookup& lookup, const ContractShape& shape);

/**
 * The debug listing behind geas dump-types: for every contract that is not
 * internal, each node of its type table with its index, tag, name, kids, and
 * variants, then the parameter and return indices of every pledge, signal,
 * and remedy and the index of every vow.
 * @params checked - a checked program
 * @returns {std::expected<std::string, std::vector<Diagnostic>>} - the listing, or the build
 * faults a table met
 */
std::expected<std::string, std::vector<Diagnostic>> dumpTypes(CheckedProgram& checked);

/**
 * The short name of a node's tag, its GeasTypeTag spelling without the
 * GEAS_TY_ prefix.
 * @params tag - the GeasTypeTag spelling
 * @returns {std::string_view} - the short name
 */
std::string_view shortTagName(std::string_view tag);

}  // namespace geas
