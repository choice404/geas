// Canonical type codes, the spelling the type arena interns by and shape
// hashes are taken over, read back into the arena. A recorded shape keeps
// the types of an older version as codes, and reading one back lets the
// checker compare it to a type of this version by id and render it the way
// every other diagnostic renders types
#pragma once

#include <optional>
#include <string_view>

#include "types.hpp"

namespace geas {

/**
 * Interns the type a canonical type code spells.
 * @params types - the arena to intern into, code - the canonical code
 * @returns {std::optional<TyId>} - the type, nullopt when the code does not parse
 */
std::optional<TyId> typeFromCode(TypeArena& types, std::string_view code);

}  // namespace geas
