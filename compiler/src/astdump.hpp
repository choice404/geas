// The parse dump, the whole tree as nested text, the format the
// tests/parse goldens pin
#pragma once

#include <string>

#include "ast.hpp"

namespace geas {

/**
 * Renders a parsed file as the golden dump text, no trailing newline.
 * @params ast - the tree to render
 * @returns {std::string} - the dump, starting with "File {"
 */
std::string dumpAst(const Ast& ast);

}  // namespace geas
