// The direct children of one body node, read only. An expression's children
// are its operands and the blocks of its match and select arms; a
// statement's are the expressions and blocks it holds. The checker's path
// rules walk a body through these, so every rule sees the same tree
#pragma once

#include <vector>

#include "ast.hpp"

namespace geas {

// The expressions and blocks one node holds directly, in source order
struct NodeChildren {
    std::vector<ExprId> exprs;
    std::vector<BlockId> blocks;
};

/**
 * The direct children of one expression.
 * @params ast - the tree, id - the expression
 * @returns {NodeChildren} - its operand expressions and the blocks of its arms
 */
NodeChildren exprChildren(const Ast& ast, ExprId id);

/**
 * The direct children of one statement.
 * @params ast - the tree, id - the statement
 * @returns {NodeChildren} - the expressions and blocks it holds
 */
NodeChildren stmtChildren(const Ast& ast, StmtId id);

}  // namespace geas
