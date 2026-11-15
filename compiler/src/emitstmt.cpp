// Lowering statements and blocks to C. A block opens a scope on both sides
// of the fence at once, a scope frame in the emitter beside the C variables
// it binds, so a geas binding lives exactly as long as the C variable that
// carries it. let makes a fresh copy per binding so a later assignment
// never writes through an aliased name, while and for lower onto real C
// loops so break and continue map one to one, and return writes the value
// through the out parameter and reports GEAS_OK, the one exit convention
// every thunk shares
#include <utility>

#include "emitc.hpp"
#include "emittext.hpp"
#include "overloaded.hpp"

namespace geas {

void CEmitter::genBlock(BlockId id) {
    pushScope();
    for (const StmtId stmt : ast().block(id).stmts) {
        genStmt(stmt);
    }
    popScope();
}

// A block in value position, a match arm written as a block: the statements
// run in order and a final expression statement's value lands in dest.
// Returns that value's checked type, unset when the block ends any other
// way and dest keeps what it held
TyId CEmitter::genBlockInto(BlockId id, TyId want, std::string_view dest) {
    const std::vector<StmtId>& stmts = ast().block(id).stmts;
    pushScope();
    TyId type;
    for (std::size_t i = 0; i < stmts.size(); ++i) {
        const auto* last = std::get_if<ExprStmt>(&ast().stmt(stmts[i]));
        if (i + 1 != stmts.size() || last == nullptr) {
            genStmt(stmts[i]);
            continue;
        }
        const CValue value = genExpr(last->expr, want);
        line(joined(dest, " = ", value.name, ";"));
        type = value.type;
    }
    popScope();
    return type;
}

void CEmitter::genStmt(StmtId id) {
    const Stmt& stmt = ast().stmt(id);
    std::visit(Overloaded{
                   [&](const LetStmt& s) { genLet(s); },
                   [&](const AssignStmt& s) { genAssign(s); },
                   [&](const ReturnStmt& s) { genReturn(s); },
                   [&](const IfStmt& s) { genIf(s); },
                   [&](const WhileStmt& s) { genWhile(s); },
                   [&](const ForStmt& s) { genFor(s); },
                   [&](const BreakStmt&) { line("break;"); },
                   [&](const ContinueStmt&) { line("continue;"); },
                   [&](const ExprStmt& s) { genExpr(s.expr, TyId{}); },
                   [&](const SleepStmt&) { genWorkflowStmt(stmt); },
                   [&](const TogetherStmt&) { genWorkflowStmt(stmt); },
               },
               stmt);
}

void CEmitter::genLet(const LetStmt& let) {
    TyId annotation;
    if (let.type) {
        const Expr& value = ast().expr(let.value);
        annotation = lowering_.lower(*let.type, value.lo, value.hi);
    }
    const CValue value = genExpr(let.value, annotation);
    // Value semantics: a composite initializer is deep copied so the binding
    // owns memory no other binding aliases
    const CValue copy = copyOut(value);
    // The binding gets its own C variable, never the initializer's, so an
    // assignment to it leaves the initializer alone
    std::string variable = freshName();
    line(joined("GeasValue ", variable, " = ", copy.name, ";"));
    declare(let.name, std::move(variable), annotation.isSet() ? annotation : copy.type);
}

void CEmitter::genReturn(const ReturnStmt& ret) {
    if (!ret.value) {
        line("out->ty = GEAS_TY_UNIT;");
        line("return GEAS_OK;");
        return;
    }
    const CValue value = genExpr(*ret.value, returnType_);
    line(joined("*out = ", value.name, ";"));
    line("return GEAS_OK;");
}

void CEmitter::genIf(const IfStmt& branch) {
    const CValue condition = genExpr(branch.condition, types().scalar(TyKind::Bool));
    line(joined("if (", condition.name, ".as.b) {"));
    ++indent_;
    genBlock(branch.thenBlock);
    --indent_;
    if (branch.elseBlock) {
        line("} else {");
        ++indent_;
        genBlock(*branch.elseBlock);
        --indent_;
    }
    line("}");
}

// while lowers onto an unconditional C loop with the condition re-evaluated
// at the top of every pass, since the condition may take several statements
// to compute
void CEmitter::genWhile(const WhileStmt& loop) {
    line("for (;;) {");
    ++indent_;
    const CValue condition = genExpr(loop.condition, types().scalar(TyKind::Bool));
    line(joined("if (!", condition.name, ".as.b) break;"));
    genBlock(loop.body);
    --indent_;
    line("}");
}

// for over a List is a counted C loop over the element array. Over a Map it
// steps the flat [k0,v0,k1,v1,...] store two slots at a time, the entry
// count snapshotted before the loop so a key the body inserts does not join
// the current pass. The loop variable is a struct copy of the slot
void CEmitter::genFor(const ForStmt& loop) {
    const CValue iterable = genExpr(loop.iterable, TyId{});
    const TyNode node = nodeOf(iterable.type);
    if (node.kind == TyKind::Map) {
        const std::string length = freshName();
        line(joined("uint64_t ", length, " = ", iterable.name, ".as.list.len;"));
        const std::string index = freshName();
        line(joined("for (uint64_t ", index, " = 0; ", index, " < ", length, "; ", index,
                    " += 2) {"));
        genForBody(loop, iterable.name, index, node.args[0]);
        return;
    }
    if (node.kind != TyKind::List) {
        lookup_.unsupported(spanLo_, spanHi_, "for over this type in a pledge body");
        return;
    }
    const std::string index = freshName();
    line(joined("for (uint64_t ", index, " = 0; ", index, " < ", iterable.name, ".as.list.len; ",
                index, "++) {"));
    genForBody(loop, iterable.name, index, node.args[0]);
}

// The loop body shared by both for forms, the loop variable bound to a copy
// of the slot the index names
void CEmitter::genForBody(const ForStmt& loop, std::string_view iterable, std::string_view index,
                          TyId element) {
    ++indent_;
    std::string variable = freshName();
    line(joined("GeasValue ", variable, " = ((const GeasValue*)", iterable, ".as.list.data)[",
                index, "];"));
    pushScope();
    declare(loop.variable, std::move(variable), element);
    genBlock(loop.body);
    popScope();
    --indent_;
    line("}");
}

}  // namespace geas
