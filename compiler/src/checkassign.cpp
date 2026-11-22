// The assignment rule. The right side of an assignment, and any index the
// target reads on its way down, may not itself assign the binding the
// target is rooted in. Ordinary code finds the target's slot before it
// evaluates the right side and a course finds it after, since a course
// re-reads its frame once a step resumes, so a right side that replaced
// the container under the target would land the value in a different place
// in each. With the rule in place the order is never observable, in a
// course or out of one. A binding shadowed by a let, a for variable, or a
// pattern inside the right side is another binding and does not count
#include "checker.hpp"

#include "astwalk.hpp"
#include "overloaded.hpp"

namespace geas {

namespace {

// The binding an lvalue chain is rooted in, empty when the root is no name
std::string_view rootName(const Ast& ast, ExprId target) {
    const Expr* root = &ast.expr(target);
    while (true) {
        if (const auto* member = std::get_if<MemberExpr>(&root->kind)) {
            root = &ast.expr(member->base);
        } else if (const auto* index = std::get_if<IndexExpr>(&root->kind)) {
            root = &ast.expr(index->base);
        } else {
            break;
        }
    }
    const auto* ident = std::get_if<Identifier>(&root->kind);
    return ident == nullptr ? std::string_view{} : std::string_view{ident->name};
}

// Whether a pattern binds the name anywhere inside it
bool patternBinds(const Ast& ast, PatternId id, std::string_view name) {
    return std::visit(Overloaded{
        [&](const BindPattern& bind) { return bind.name == name; },
        [&](const WrapPattern& wrap) { return patternBinds(ast, wrap.inner, name); },
        [&](const VariantPattern& variant) {
            for (const PatternId field : variant.fields) {
                if (patternBinds(ast, field, name)) {
                    return true;
                }
            }
            return false;
        },
        [](const auto&) { return false; },
    }, ast.pattern(id));
}

bool exprAssigns(const Ast& ast, ExprId id, std::string_view name);
bool blockAssigns(const Ast& ast, BlockId id, std::string_view name);

// One arm body, skipped whole when its pattern shadows the name
bool armAssigns(const Ast& ast, std::optional<PatternId> pattern, const ArmBody& body,
                std::string_view name) {
    if (pattern && patternBinds(ast, *pattern, name)) {
        return false;
    }
    if (const auto* block = std::get_if<BlockId>(&body)) {
        return blockAssigns(ast, *block, name);
    }
    return exprAssigns(ast, std::get<ExprId>(body), name);
}

bool matchAssigns(const Ast& ast, const MatchExpr& match, std::string_view name) {
    if (exprAssigns(ast, match.scrutinee, name)) {
        return true;
    }
    for (const MatchArm& arm : match.arms) {
        if (armAssigns(ast, arm.pattern, arm.body, name)) {
            return true;
        }
    }
    return false;
}

bool selectAssigns(const Ast& ast, const SelectExpr& select, std::string_view name) {
    for (const SelectArm& arm : select.arms) {
        const auto* awaited = std::get_if<AwaitArm>(&arm);
        const bool isAssigned = awaited != nullptr
            ? armAssigns(ast, awaited->binding, awaited->body, name)
            : armAssigns(ast, std::nullopt, std::get<TimeoutArm>(arm).body, name);
        if (isAssigned) {
            return true;
        }
    }
    return false;
}

// Whether an assignment to the name sits anywhere under an expression
bool exprAssigns(const Ast& ast, ExprId id, std::string_view name) {
    const ExprKind& kind = ast.expr(id).kind;
    if (const auto* match = std::get_if<MatchExpr>(&kind)) {
        return matchAssigns(ast, *match, name);
    }
    if (const auto* select = std::get_if<SelectExpr>(&kind)) {
        return selectAssigns(ast, *select, name);
    }
    for (const ExprId child : exprChildren(ast, id).exprs) {
        if (exprAssigns(ast, child, name)) {
            return true;
        }
    }
    return false;
}

bool stmtAssigns(const Ast& ast, StmtId id, std::string_view name) {
    const Stmt& statement = ast.stmt(id);
    if (const auto* assign = std::get_if<AssignStmt>(&statement)) {
        if (rootName(ast, assign->target) == name) {
            return true;
        }
    }
    if (const auto* loop = std::get_if<ForStmt>(&statement)) {
        return exprAssigns(ast, loop->iterable, name) ||
               (loop->variable != name && blockAssigns(ast, loop->body, name));
    }
    const NodeChildren children = stmtChildren(ast, id);
    for (const ExprId child : children.exprs) {
        if (exprAssigns(ast, child, name)) {
            return true;
        }
    }
    for (const BlockId block : children.blocks) {
        if (blockAssigns(ast, block, name)) {
            return true;
        }
    }
    return false;
}

// A let of the name shadows it for the rest of the block, after its own
// initializer has been read
bool blockAssigns(const Ast& ast, BlockId id, std::string_view name) {
    for (const StmtId statement : ast.block(id).stmts) {
        if (stmtAssigns(ast, statement, name)) {
            return true;
        }
        const auto* let = std::get_if<LetStmt>(&ast.stmt(statement));
        if (let != nullptr && let->name == name) {
            return false;
        }
    }
    return false;
}

}  // namespace

/**
 * Refuses an assignment whose right side, or an index its target reads, assigns the binding
 * the target is rooted in.
 * @params assign - the assignment statement
 */
void Checker::checkAssignOrder(const AssignStmt& assign) {
    const std::string_view root = rootName(ast_, assign.target);
    if (root.empty()) {
        return;
    }
    if (!exprAssigns(ast_, assign.value, root) && !exprAssigns(ast_, assign.target, root)) {
        return;
    }
    emitAt(ast_.expr(assign.value),
           "this assignment to " + quoted(root) + " assigns " + quoted(root) +
               " again while its right side or target index is evaluated; bind that value "
               "with 'let' first");
}

}  // namespace geas
