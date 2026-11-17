// Attribute names live in one namespace per declaration target
#pragma once

#include "resolve.hpp"

namespace geas {

/**
 * Names the declaration an attribute belongs on
 * @params target - the declaration kind
 * @returns {std::string_view} - the source spelling
 */
std::string_view attrTargetWord(AttrTarget target);

/**
 * Identifies keys whose meaning the language defines on this target
 * @params key - the key spelling, target - the declaration kind
 * @returns {bool} - whether the key is builtin here
 */
bool isBuiltinAttr(std::string_view key, AttrTarget target);

/**
 * Collects declarations and validates their names and scalar types
 * @params program - the loaded modules, resolved - the tables and diagnostics
 * @returns {void} - declarations are recorded in resolved
 */
void resolveAttributes(const Program& program, Resolved& resolved);

}  // namespace geas
