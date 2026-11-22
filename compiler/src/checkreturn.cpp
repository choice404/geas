// The return rule. A pledge, clause, or remedy whose return type is not
// Unit has to end every path through its body with a return, since there is
// no value a body could fall off its end with. A path ends when it reaches a
// return, an if whose two branches both end, a match whose every arm ends,
// or a while true loop no break leaves, since nothing after such a loop is
// reachable. A for loop may run zero times, so it never ends a path. A
// course returns nothing and is never held to this
#include "astwalk.hpp"
#include "checker.hpp"

#include "overloaded.hpp"

namespace geas {

namespace {

bool blockEnds(const Ast& ast, BlockId id);

// A match ends a path when every one of its arms does
bool exprEnds(const Ast& ast, ExprId id) {
    const auto* match = std::get_if<MatchExpr>(&ast.expr(id).kind);
    if (match == nullptr || match->arms.empty()) {
        return false;
    }
    for (const MatchArm& arm : match->arms) {
        const auto* block = std::get_if<BlockId>(&arm.body);
        const bool isEnded = block != nullptr ? blockEnds(ast, *block)
                                              : exprEnds(ast, std::get<ExprId>(arm.body));
        if (!isEnded) {
            return false;
        }
    }
    return true;
}

bool blockBreaks(const Ast& ast, BlockId id);

// Whether a break anywhere under an expression leaves the loop being
// scanned, through the blocks of match and select arms
bool exprBreaks(const Ast& ast, ExprId id) {
    const NodeChildren children = exprChildren(ast, id);
    for (const ExprId child : children.exprs) {
        if (exprBreaks(ast, child)) {
            return true;
        }
    }
    for (const BlockId block : children.blocks) {
        if (blockBreaks(ast, block)) {
            return true;
        }
    }
    return false;
}

// A break inside a nested loop's body leaves that loop, not this one, but a
// break in the nested loop's condition or iterable still leaves this one
bool stmtBreaks(const Ast& ast, StmtId id) {
    const Stmt& statement = ast.stmt(id);
    if (std::holds_alternative<BreakStmt>(statement)) {
        return true;
    }
    const NodeChildren children = stmtChildren(ast, id);
    for (const ExprId child : children.exprs) {
        if (exprBreaks(ast, child)) {
            return true;
        }
    }
    const bool isLoop =
        std::holds_alternative<WhileStmt>(statement) || std::holds_alternative<ForStmt>(statement);
    if (isLoop) {
        return false;
    }
    for (const BlockId block : children.blocks) {
        if (blockBreaks(ast, block)) {
            return true;
        }
    }
    return false;
}

bool blockBreaks(const Ast& ast, BlockId id) {
    for (const StmtId statement : ast.block(id).stmts) {
        if (stmtBreaks(ast, statement)) {
            return true;
        }
    }
    return false;
}

// A while true loop no break leaves never falls through to what follows
bool isEndless(const Ast& ast, const WhileStmt& loop) {
    const auto* condition = std::get_if<BoolLiteral>(&ast.expr(loop.condition).kind);
    return condition != nullptr && condition->value && !blockBreaks(ast, loop.body);
}

bool stmtEnds(const Ast& ast, StmtId id) {
    return std::visit(Overloaded{
        [](const ReturnStmt&) { return true; },
        [&](const IfStmt& node) {
            return node.elseBlock && blockEnds(ast, node.thenBlock) &&
                   blockEnds(ast, *node.elseBlock);
        },
        [&](const WhileStmt& node) { return isEndless(ast, node); },
        [&](const ExprStmt& node) { return exprEnds(ast, node.expr); },
        [&](const LetStmt& node) { return exprEnds(ast, node.value); },
        [](const auto&) { return false; },
    }, ast.stmt(id));
}

bool blockEnds(const Ast& ast, BlockId id) {
    for (const StmtId statement : ast.block(id).stmts) {
        if (stmtEnds(ast, statement)) {
            return true;
        }
    }
    return false;
}

}  // namespace

/**
 * Holds a body whose return type is not Unit to a return on every path.
 * @params name - the pledge, clause, or remedy name, ret - its return type, body - its block,
 * lo and hi - the span a fault lands on
 */
void Checker::checkEveryPathReturns(std::string_view name, TyId ret, BlockId body,
                                    std::int64_t lo, std::int64_t hi) {
    const TyKind kind = kindOf(ret);
    if (kind == TyKind::Unit || kind == TyKind::Unknown) {
        return;
    }
    if (blockEnds(ast_, body)) {
        return;
    }
    emit(lo, hi, "not every path of " + quoted(name) + " returns a value");
}

}  // namespace geas
