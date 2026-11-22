// The unread local pass. A lowered thunk, clause, or remedy body declares a
// GeasValue temporary for every value it computes, and some of them are
// never read: the Unit answer of a call made as a statement, a binding the
// source names and never uses, a match whose value is dropped. The emitted C
// compiles under -Wall -Wextra with no warning, so each such temporary gets
// a (void) line right after its declaration and every other line stays as
// it was
#pragma once

#include <string>
#include <string_view>

namespace geas {

/**
 * Marks every GeasValue temporary of one lowered body that nothing reads.
 * @params body - The lowered body text, one statement per line
 * @returns {std::string} - The same body with a (void) line after the
 *     declaration of each temporary no other line reads
 */
std::string markUnreadLocals(std::string_view body);

}  // namespace geas
