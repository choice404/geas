// The dump of patterns, match and select arms, blocks, and statements
#include "astdumper.hpp"

#include <variant>

namespace geas {

void AstDumper::renderPattern(PatternId id, int level) {
    std::visit(Overloaded{
                   [&](const WildcardPattern&) { write("Wildcard"); },
                   [&](const LiteralPattern& p) {
                       write("Lit(");
                       renderExpr(p.literal, level);
                       write(")");
                   },
                   [&](const BindPattern& p) {
                       write("Bind(");
                       quoted(p.name);
                       write(")");
                   },
                   [&](const VariantPattern& p) { renderVariantPattern(p, level); },
                   [&](const WrapPattern& p) {
                       openWrap(wrapKindName(p.kind), level);
                       renderPattern(p.inner, level + 1);
                       closeWrap(level);
                   },
                   [&](const NonePattern&) { write("None"); },
               },
               ast_.pattern(id));
}

// Variant("x", [ fields ]), the bracket opening a line even when empty
void AstDumper::renderVariantPattern(const VariantPattern& variant, int level) {
    write("Variant(");
    quoted(variant.name);
    write(", [\n");
    for (const PatternId field : variant.fields) {
        indent(level + 1);
        renderPattern(field, level + 1);
        endField();
    }
    indent(level);
    write("])");
}

void AstDumper::renderArmBody(const ArmBody& body, int level) {
    if (const auto* block = std::get_if<BlockId>(&body)) {
        renderBlock(*block, level);
        return;
    }
    renderExpr(std::get<ExprId>(body), level);
}

void AstDumper::renderMatch(const MatchExpr& match, int level) {
    openStruct("Match");
    key(level + 1, "scrutinee");
    renderExpr(match.scrutinee, level + 1);
    endField();
    key(level + 1, "arms");
    list(match.arms, level + 1, [&](const MatchArm& arm, int inner) {
        openStruct("Arm");
        key(inner + 1, "pat");
        renderPattern(arm.pattern, inner + 1);
        endField();
        key(inner + 1, "body");
        renderArmBody(arm.body, inner + 1);
        endField();
        closeStruct(inner);
    });
    endField();
    closeStruct(level);
}

void AstDumper::renderSelect(const SelectExpr& select, int level) {
    openStruct("Select");
    key(level + 1, "arms");
    list(select.arms, level + 1,
         [&](const SelectArm& arm, int inner) { renderSelectArm(arm, inner); });
    endField();
    closeStruct(level);
}

// AwaitArm { signal, bind, body } or TimeoutArm { after, body }
void AstDumper::renderSelectArm(const SelectArm& arm, int level) {
    if (const auto* await = std::get_if<AwaitArm>(&arm)) {
        openStruct("AwaitArm");
        key(level + 1, "signal");
        quoted(await->signal);
        endField();
        key(level + 1, "bind");
        if (await->binding) {
            renderPattern(*await->binding, level + 1);
        } else {
            write("None");
        }
        endField();
        key(level + 1, "body");
        renderArmBody(await->body, level + 1);
        endField();
        closeStruct(level);
        return;
    }
    const auto& timeout = std::get<TimeoutArm>(arm);
    openStruct("TimeoutArm");
    key(level + 1, "after");
    renderDuration(timeout.milliseconds);
    endField();
    key(level + 1, "body");
    renderArmBody(timeout.body, level + 1);
    endField();
    closeStruct(level);
}

void AstDumper::renderBlock(BlockId id, int level) {
    openStruct("Block");
    key(level + 1, "stmts");
    list(ast_.block(id).stmts, level + 1,
         [&](StmtId stmt, int inner) { renderStmt(stmt, inner); });
    endField();
    closeStruct(level);
}

void AstDumper::renderStmt(StmtId id, int level) {
    std::visit(Overloaded{
                   [&](const LetStmt& s) { renderLet(s, level); },
                   [&](const AssignStmt& s) { renderPair("Assign", s.target, s.value, level); },
                   [&](const ReturnStmt& s) { renderReturn(s, level); },
                   [&](const IfStmt& s) { renderIf(s, level); },
                   [&](const WhileStmt& s) { renderWhile(s, level); },
                   [&](const ForStmt& s) { renderFor(s, level); },
                   [&](const BreakStmt&) { write("Break"); },
                   [&](const ContinueStmt&) { write("Continue"); },
                   [&](const ExprStmt& s) { renderWrapped("Expr", s.expr, level); },
                   [&](const SleepStmt& s) { renderWrapped("Sleep", s.duration, level); },
                   [&](const TogetherStmt& s) { renderTogether(s, level); },
               },
               ast_.stmt(id));
}

// Return(None) for a bare return, else a wrapper over the value
void AstDumper::renderReturn(const ReturnStmt& returnStmt, int level) {
    if (!returnStmt.value) {
        write("Return(None)");
        return;
    }
    renderWrapped("Return", *returnStmt.value, level);
}

void AstDumper::renderTogether(const TogetherStmt& together, int level) {
    openStruct("Together");
    key(level + 1, "body");
    renderBlock(together.body, level + 1);
    endField();
    closeStruct(level);
}

void AstDumper::renderLet(const LetStmt& let, int level) {
    openStruct("Let");
    key(level + 1, "mutable");
    boolean(let.isMutable);
    endField();
    key(level + 1, "name");
    quoted(let.name);
    endField();
    key(level + 1, "ty");
    if (let.type) {
        renderType(*let.type, level + 1);
    } else {
        write("None");
    }
    endField();
    key(level + 1, "value");
    renderExpr(let.value, level + 1);
    endField();
    closeStruct(level);
}

void AstDumper::renderIf(const IfStmt& ifStmt, int level) {
    openStruct("If");
    key(level + 1, "cond");
    renderExpr(ifStmt.condition, level + 1);
    endField();
    key(level + 1, "then");
    renderBlock(ifStmt.thenBlock, level + 1);
    endField();
    key(level + 1, "els");
    renderOptionalBlock(ifStmt.elseBlock, level + 1);
    endField();
    closeStruct(level);
}

void AstDumper::renderWhile(const WhileStmt& whileStmt, int level) {
    openStruct("While");
    key(level + 1, "cond");
    renderExpr(whileStmt.condition, level + 1);
    endField();
    key(level + 1, "body");
    renderBlock(whileStmt.body, level + 1);
    endField();
    closeStruct(level);
}

void AstDumper::renderFor(const ForStmt& forStmt, int level) {
    openStruct("For");
    key(level + 1, "var");
    quoted(forStmt.variable);
    endField();
    key(level + 1, "iter");
    renderExpr(forStmt.iterable, level + 1);
    endField();
    key(level + 1, "body");
    renderBlock(forStmt.body, level + 1);
    endField();
    closeStruct(level);
}

}  // namespace geas
