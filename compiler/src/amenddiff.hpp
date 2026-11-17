// The coverage diff of one amends block. An amends N block inside a contract
// at version M says how an instance parked under N resumes as M, and every
// difference between the recorded shape of N and the shape of M has to be
// covered: a vow whose type changed or that M adds without a default is set
// with vow NAME = expr, a pledge, signal, subcontract, or remedy M lacks is
// renamed or retired, and an item M adds may be latched. A type is the same
// in both versions when its code is and so is the layout of every declared
// record and sum the code reaches, see layout.hpp. An item no
// difference calls for is a fault too. The course is the one difference the
// diff leaves to courseFault, since its hash is known only once the course lowers.
//
// What comes back is the plan code generation lowers onto the descriptor's
// amends table: per entry of N, the index of the entry of M it lands on, -1
// when it is dropped, and the latch seeds for the items M adds
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "amendshape.hpp"
#include "ast.hpp"
#include "types.hpp"

namespace geas {

// A latch seed: the index of an item M adds in M's pledges table, pledges
// then signals, and the state it starts in
struct AmendSeed {
    std::int32_t item;
    LatchState state;
};

struct AmendPlan {
    RecordedShape from;
    std::vector<std::int32_t> vowMap;
    std::vector<std::int32_t> itemMap;
    std::vector<std::int32_t> subMap;
    std::vector<std::int32_t> remedyMap;
    std::vector<AmendSeed> seeds;
    std::vector<TypeLayout> toLayouts;
    bool isCourseRestarted = false;
};

// The version being compiled, its recorded form and which of its vows carry a default
struct AmendTarget {
    const RecordedShape& shape;
    const std::vector<bool>& vowDefaults;
};

struct AmendDiff {
    AmendPlan plan;
    std::vector<std::string> faults;
};

/**
 * Diffs the recorded shape of the amended version against the version being
 * compiled and holds the block's items to the differences.
 * @params from - the recorded shape of version N, to - version M, amends - the block,
 * types - the arena type codes render through
 * @returns {AmendDiff} - the plan, and every fault in the order found
 */
AmendDiff diffAmends(const RecordedShape& from, const AmendTarget& to, const AmendsDecl& amends,
                     TypeArena& types);

/**
 * Holds restart course to what the two courses call for: required when the
 * course hashes differ or only M has a course, refused otherwise.
 * @params plan - the block's plan, courseHash - the hash of M's lowered course, 0 when it has
 * none, version - M's version
 * @returns {std::optional<std::string>} - the fault, nullopt when the block agrees
 */
std::optional<std::string> courseFault(const AmendPlan& plan, std::uint64_t courseHash,
                                       std::int64_t version);

}  // namespace geas
