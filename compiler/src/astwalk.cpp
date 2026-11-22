// The direct children of one body node, see astwalk.hpp
#include "astwalk.hpp"

#include "overloaded.hpp"

namespace geas {

namespace {

void addArguments(NodeChildren& children, const std::vector<Argument>& arguments) {
    for (const Argument& argument : arguments) {
        children.exprs.push_back(argument.value);
    }
}

// An arm body is an expression or a block, filed where it belongs
void addArmBody(NodeChildren& children, const ArmBody& body) {
    if (const auto* expr = std::get_if<ExprId>(&body)) {
        children.exprs.push_back(*expr);
    } else {
        children.blocks.push_back(std::get<BlockId>(body));
    }
}

void addMatch(NodeChildren& children, const MatchExpr& match) {
    children.exprs.push_back(match.scrutinee);
    for (const MatchArm& arm : match.arms) {
        addArmBody(children, arm.body);
    }
}

void addSelect(NodeChildren& children, const SelectExpr& select) {
    for (const SelectArm& arm : select.arms) {
        if (const auto* awaited = std::get_if<AwaitArm>(&arm)) {
            addArmBody(children, awaited->body);
        } else {
            addArmBody(children, std::get<TimeoutArm>(arm).body);
        }
    }
}

}  // namespace

NodeChildren exprChildren(const Ast& ast, ExprId id) {
    NodeChildren children;
    std::visit(Overloaded{
        [&](const UnaryExpr& node) { children.exprs.push_back(node.operand); },
        [&](const BinaryExpr& node) { children.exprs = {node.lhs, node.rhs}; },
        [&](const CallExpr& node) {
            children.exprs.push_back(node.callee);
            addArguments(children, node.args);
        },
        [&](const IndexExpr& node) { children.exprs = {node.base, node.index}; },
        [&](const MemberExpr& node) { children.exprs.push_back(node.base); },
        [&](const QuestionExpr& node) { children.exprs.push_back(node.operand); },
        [&](const WrapExpr& node) { children.exprs.push_back(node.inner); },
        [&](const TupleExpr& node) { children.exprs = node.elements; },
        [&](const ListExpr& node) { children.exprs = node.elements; },
        [&](const RecordLiteral& node) {
            for (const FieldInit& field : node.fields) {
                children.exprs.push_back(field.value);
            }
        },
        [&](const GenericNewExpr& node) { addArguments(children, node.args); },
        [&](const MatchExpr& node) { addMatch(children, node); },
        [&](const SelectExpr& node) { addSelect(children, node); },
        [](const auto&) {},
    }, ast.expr(id).kind);
    return children;
}

NodeChildren stmtChildren(const Ast& ast, StmtId id) {
    NodeChildren children;
    std::visit(Overloaded{
        [&](const LetStmt& node) { children.exprs.push_back(node.value); },
        [&](const AssignStmt& node) { children.exprs = {node.target, node.value}; },
        [&](const ReturnStmt& node) {
            if (node.value) {
                children.exprs.push_back(*node.value);
            }
        },
        [&](const IfStmt& node) {
            children.exprs.push_back(node.condition);
            children.blocks.push_back(node.thenBlock);
            if (node.elseBlock) {
                children.blocks.push_back(*node.elseBlock);
            }
        },
        [&](const WhileStmt& node) {
            children.exprs.push_back(node.condition);
            children.blocks.push_back(node.body);
        },
        [&](const ForStmt& node) {
            children.exprs.push_back(node.iterable);
            children.blocks.push_back(node.body);
        },
        [&](const ExprStmt& node) { children.exprs.push_back(node.expr); },
        [&](const SleepStmt& node) { children.exprs.push_back(node.duration); },
        [&](const TogetherStmt& node) { children.blocks.push_back(node.body); },
        [](const auto&) {},
    }, ast.stmt(id));
    return children;
}

}  // namespace geas
