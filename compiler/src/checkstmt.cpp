// Statements and blocks. A block opens a scope, its statements check in
// order, and the scope closes with it. A block can also stand where a value
// is needed, a match arm written as a block: its value is the type of its
// final expression statement, and Unit when the block ends any other way.
// Assignment enforces the mutability rule as written: the target is a let
// mut binding, a field of one, or an element of one, and a vow is never
// assignable. Its right side may not assign that binding again, see
// checkassign.cpp
#include "checker.hpp"

#include "overloaded.hpp"

namespace geas {

void Checker::checkCondition(ExprId id, std::string_view what) {
    const TyId boolType = scalar(TyKind::Bool);
    const TyId actual = inferExpr(id, boolType);
    if (!isCompatible(actual, boolType)) {
        emitAt(ast_.expr(id), "a" + std::string{what} + " condition must be 'Bool', got " +
                                  quoted(types_.render(actual)));
    }
}

// The root of an lvalue chain must be a let mut binding, a vow is never one
void Checker::checkAssignable(ExprId id) {
    const Expr* root = &ast_.expr(id);
    while (true) {
        if (const auto* member = std::get_if<MemberExpr>(&root->kind)) {
            root = &ast_.expr(member->base);
        } else if (const auto* index = std::get_if<IndexExpr>(&root->kind)) {
            root = &ast_.expr(index->base);
        } else {
            break;
        }
    }
    const auto* ident = std::get_if<Identifier>(&root->kind);
    if (ident == nullptr) {
        emitAt(*root, "this expression cannot be assigned");
        return;
    }
    if (ident->name.empty()) {
        return;
    }
    if (const Binding* binding = lookup(ident->name)) {
        if (!binding->isMutable) {
            emitAt(*root, "cannot assign to the immutable binding " + quoted(ident->name) +
                              "; declare it 'let mut'");
        }
        return;
    }
    if (contract_ == nullptr) {
        return;
    }
    const MemberEntry* member = resolved_.member(contract_, ident->name);
    if (member != nullptr && member->kind() == MemberKind::Vow) {
        emitAt(*root,
               "vow " + quoted(ident->name) + " is never assignable; copy it with 'mut(...)'");
    }
}

void Checker::checkLet(const LetStmt& let) {
    const Expr& value = ast_.expr(let.value);
    TyId annotation;
    if (let.type) {
        annotation = lowering_.lower(*let.type, value.lo, value.hi);
    }
    const TyId actual = inferExpr(let.value, annotation);
    TyId bound = actual;
    if (annotation.isSet()) {
        if (!isCompatible(annotation, actual)) {
            emitAt(value, typesMessage("the let binding is annotated ", annotation,
                                       " but the value has type ", actual));
        }
        bound = annotation;
    }
    declare(let.name, bound, let.isMutable);
}

// The element type an index reaches in assignment position, where a Map
// index names the value slot itself and not the Option a read yields
TyId Checker::lvalueIndexType(const Expr& expr, const IndexExpr& index) {
    const TyId base = lvalueType(index.base);
    const TyNode node = types_.node(base);
    if (node.kind == TyKind::Map) {
        const TyId key = inferExpr(index.index, node.args[0]);
        if (!isCompatible(node.args[0], key)) {
            emitAt(expr, typesMessage("this map is keyed by ", node.args[0], ", got ", key));
        }
        return node.args[1];
    }
    const TyId intType = scalar(TyKind::Int);
    const TyId position = inferExpr(index.index, intType);
    if (node.kind == TyKind::List) {
        if (!isCompatible(position, intType)) {
            emitAt(expr, typesMessage("a list index must be ", intType, ", got ", position));
        }
        return node.args[0];
    }
    if (node.kind == TyKind::Unknown) {
        return unknown();
    }
    emitAt(expr, "cannot assign through an index on " + quoted(types_.render(base)) +
                     "; only a List or a Map is indexed");
    return unknown();
}

TyId Checker::lvalueMemberType(const Expr& expr, const MemberExpr& member) {
    const TyId base = lvalueType(member.base);
    const TyKind kind = kindOf(base);
    if (kind == TyKind::Named) {
        const std::string typeName{types_.nameOf(base)};
        const GlobalEntry* global = resolved_.global(typeName);
        if (global != nullptr && global->kind == GlobalKind::Record) {
            if (const FieldSig* field = recordField(typeName, member.name)) {
                return field->type;
            }
            emitAt(expr, quoted(typeName) + " has no field " + quoted(member.name));
            return unknown();
        }
    }
    if (kind == TyKind::Unknown) {
        return unknown();
    }
    emitAt(expr, "cannot assign through a member of type " + quoted(types_.render(base)));
    return unknown();
}

// The type an lvalue chain resolves to, walked in assignment position.
// Record fields and list elements type exactly as their reads do
TyId Checker::lvalueType(ExprId id) {
    const Expr& expr = ast_.expr(id);
    if (const auto* index = std::get_if<IndexExpr>(&expr.kind)) {
        return lvalueIndexType(expr, *index);
    }
    if (const auto* member = std::get_if<MemberExpr>(&expr.kind)) {
        return lvalueMemberType(expr, *member);
    }
    // The root of the chain: a binding or a vow, the value read's business
    return inferExpr(id, TyId{});
}

// An index target walks its base as an lvalue, so the container may sit at
// the end of any chain of fields and elements. On a Map the statement is the
// insert or update form, the target type is the value type
void Checker::checkIndexAssign(const AssignStmt& assign, const Expr& target,
                               const IndexExpr& index) {
    const TyId base = lvalueType(index.base);
    const TyNode node = types_.node(base);
    checkAssignable(assign.target);
    const Expr& value = ast_.expr(assign.value);
    if (node.kind == TyKind::Map) {
        const TyId key = inferExpr(index.index, node.args[0]);
        if (!isCompatible(node.args[0], key)) {
            emitAt(target, typesMessage("this map is keyed by ", node.args[0], ", got ", key));
        }
        const TyId actual = inferExpr(assign.value, node.args[1]);
        if (!isCompatible(node.args[1], actual)) {
            emitAt(value, typesMessage("this map holds values of type ", node.args[1],
                                       " but the value has type ", actual));
        }
        return;
    }
    const TyId intType = scalar(TyKind::Int);
    const TyId position = inferExpr(index.index, intType);
    TyId element = unknown();
    if (node.kind == TyKind::List) {
        if (!isCompatible(position, intType)) {
            emitAt(target, typesMessage("a list index must be ", intType, ", got ", position));
        }
        element = node.args[0];
    } else if (node.kind != TyKind::Unknown) {
        emitAt(target, "cannot assign through an index on " + quoted(types_.render(base)) +
                           "; only a List or a Map is indexed");
    }
    checkAssignedValue(assign.value, element);
}

// The right side of an assignment against the type of its target
void Checker::checkAssignedValue(ExprId value, TyId target) {
    const TyId actual = inferExpr(value, target);
    if (!isCompatible(target, actual)) {
        emitAt(ast_.expr(value), typesMessage("the assignment target has type ", target,
                                              " but the value has type ", actual));
    }
}

void Checker::checkAssign(const AssignStmt& assign) {
    checkAssignOrder(assign);
    const Expr& target = ast_.expr(assign.target);
    if (const auto* index = std::get_if<IndexExpr>(&target.kind)) {
        checkIndexAssign(assign, target, *index);
        return;
    }
    const bool isMember = std::holds_alternative<MemberExpr>(target.kind);
    const TyId targetType = isMember ? lvalueType(assign.target) : inferExpr(assign.target, TyId{});
    checkAssignable(assign.target);
    checkAssignedValue(assign.value, targetType);
}

void Checker::checkReturn(const ReturnStmt& statement) {
    const TyId ret = returnType_.isSet() ? returnType_ : unknown();
    const TyKind kind = kindOf(ret);
    if (!statement.value) {
        if (kind != TyKind::Unit && kind != TyKind::Unknown) {
            emitInBody("a bare return in a body that returns " + quoted(types_.render(ret)));
        }
        return;
    }
    if (isInCourse_) {
        inferExpr(*statement.value, TyId{});
        emitAt(ast_.expr(*statement.value), "a course returns no value; write a bare 'return'");
        return;
    }
    const TyId actual = inferExpr(*statement.value, ret);
    if (!isCompatible(ret, actual)) {
        emitAt(ast_.expr(*statement.value),
               typesMessage("the return value has type ", actual, " but the body returns ", ret));
    }
}

// A for over a List binds its element, a for over a Map walks its keys in
// insertion order and binds the key type
void Checker::checkFor(const ForStmt& loop) {
    const TyId iterable = inferExpr(loop.iterable, TyId{});
    const TyNode node = types_.node(iterable);
    TyId element = unknown();
    if (node.kind == TyKind::List || node.kind == TyKind::Map) {
        element = node.args[0];
    } else if (node.kind != TyKind::Unknown) {
        emitAt(ast_.expr(loop.iterable),
               "for needs a List or a Map value, got " + quoted(types_.render(iterable)));
    }
    pushScope();
    declare(loop.variable, element, false);
    ++loopDepth_;
    checkBlock(loop.body);
    --loopDepth_;
    popScope();
}

void Checker::checkWhile(const WhileStmt& loop) {
    checkCondition(loop.condition, " while");
    ++loopDepth_;
    checkBlock(loop.body);
    --loopDepth_;
}

void Checker::checkIf(const IfStmt& statement) {
    checkCondition(statement.condition, "n if");
    checkBlock(statement.thenBlock);
    if (statement.elseBlock) {
        checkBlock(*statement.elseBlock);
    }
}

void Checker::checkLoopExit(std::string_view keyword) {
    if (loopDepth_ == 0) {
        emitInBody(quoted(keyword) + " appears outside a loop");
    }
}

void Checker::checkStmt(StmtId id) {
    std::visit(Overloaded{
                   [&](const LetStmt& let) { checkLet(let); },
                   [&](const AssignStmt& assign) { checkAssign(assign); },
                   [&](const ReturnStmt& statement) { checkReturn(statement); },
                   [&](const IfStmt& statement) { checkIf(statement); },
                   [&](const WhileStmt& loop) { checkWhile(loop); },
                   [&](const ForStmt& loop) { checkFor(loop); },
                   [&](const BreakStmt&) { checkLoopExit("break"); },
                   [&](const ContinueStmt&) { checkLoopExit("continue"); },
                   [&](const ExprStmt& statement) { inferExpr(statement.expr, TyId{}); },
                   [&](const SleepStmt& sleep) { checkSleep(sleep); },
                   [&](const TogetherStmt& together) { checkTogether(together); },
               },
               ast_.stmt(id));
}

void Checker::checkBlock(BlockId id) {
    pushScope();
    for (const StmtId statement : ast_.block(id).stmts) {
        checkStmt(statement);
    }
    popScope();
}

// A block in value position, a match or select arm body: the value is the
// final expression statement's, and Unit when the block ends any other way
TyId Checker::blockValue(BlockId id, TyId want) {
    const std::vector<StmtId>& statements = ast_.block(id).stmts;
    pushScope();
    TyId value = scalar(TyKind::Unit);
    for (std::size_t i = 0; i < statements.size(); ++i) {
        const auto* last = std::get_if<ExprStmt>(&ast_.stmt(statements[i]));
        if (i + 1 == statements.size() && last != nullptr) {
            value = inferExpr(last->expr, want);
        } else {
            checkStmt(statements[i]);
        }
    }
    popScope();
    return value;
}

}  // namespace geas
