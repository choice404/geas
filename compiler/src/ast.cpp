// The arena helpers of the syntax tree, see ast.hpp
#include "ast.hpp"

#include <utility>

namespace geas {

namespace {

// Pushes a node and returns its slot as a typed id
template <typename Id, typename Node>
Id pushNode(std::vector<Node>& arena, Node node) {
    const auto index = static_cast<std::int32_t>(arena.size());
    arena.push_back(std::move(node));
    return Id{index};
}

template <typename Node, typename Id>
const Node& nodeAt(const std::vector<Node>& arena, Id id) {
    return arena[static_cast<std::size_t>(id.index)];
}

}  // namespace

ExprId Ast::addExpr(Expr node) {
    return pushNode<ExprId>(exprs, std::move(node));
}

StmtId Ast::addStmt(Stmt node) {
    return pushNode<StmtId>(stmts, std::move(node));
}

TypeId Ast::addType(Type node) {
    return pushNode<TypeId>(types, std::move(node));
}

PatternId Ast::addPattern(Pattern node) {
    return pushNode<PatternId>(patterns, std::move(node));
}

BlockId Ast::addBlock(Block node) {
    return pushNode<BlockId>(blocks, std::move(node));
}

ReqId Ast::addReq(ReqNode node) {
    return pushNode<ReqId>(reqs, std::move(node));
}

const Expr& Ast::expr(ExprId id) const {
    return nodeAt(exprs, id);
}

const Stmt& Ast::stmt(StmtId id) const {
    return nodeAt(stmts, id);
}

const Type& Ast::type(TypeId id) const {
    return nodeAt(types, id);
}

const Pattern& Ast::pattern(PatternId id) const {
    return nodeAt(patterns, id);
}

const Block& Ast::block(BlockId id) const {
    return nodeAt(blocks, id);
}

const ReqNode& Ast::req(ReqId id) const {
    return nodeAt(reqs, id);
}

}  // namespace geas
