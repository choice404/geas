// The workflow scan, see workflowgate.hpp
#include "workflowgate.hpp"

#include <algorithm>

#include "emittext.hpp"
#include "overloaded.hpp"

namespace geas {

namespace {

// Collects descriptions, each kept once
class Findings {
public:
    void add(std::string what) {
        if (std::find(found_.begin(), found_.end(), what) == found_.end()) {
            found_.push_back(std::move(what));
        }
    }

    std::vector<std::string> take() { return std::move(found_); }

private:
    std::vector<std::string> found_;
};

void scanMember(const ContractDecl& contract, const Member& member, Findings& findings) {
    if (std::holds_alternative<CourseDecl>(member)) {
        findings.add(joined("the course of contract '", contract.name, "'"));
    } else if (std::holds_alternative<AmendsDecl>(member)) {
        findings.add(joined("an amends block in contract '", contract.name, "'"));
    }
}

void scanContracts(const Ast& ast, Findings& findings) {
    for (const Decl& decl : ast.decls) {
        const auto* contract = std::get_if<ContractDecl>(&decl);
        if (contract == nullptr) {
            continue;
        }
        for (const Member& member : contract->members) {
            scanMember(*contract, member, findings);
        }
    }
}

void scanExprs(const CheckedProgram& checked, Findings& findings) {
    const std::vector<Expr>& exprs = checked.program.ast.exprs;
    const std::vector<ExprUse>& uses = checked.tables.exprUses;
    for (std::size_t i = 0; i < exprs.size(); ++i) {
        std::visit(Overloaded{
                       [&](const AwaitExpr&) { findings.add("await"); },
                       [&](const SelectExpr&) { findings.add("select"); },
                       [&](const auto&) {},
                   },
                   exprs[i].kind);
        const ExprUse use = i < uses.size() ? uses[i] : ExprUse::None;
        if (use == ExprUse::NowCall) {
            findings.add("now()");
        }
    }
}

void scanStmts(const Ast& ast, Findings& findings) {
    for (const Stmt& stmt : ast.stmts) {
        if (std::holds_alternative<SleepStmt>(stmt)) {
            findings.add("sleep");
        } else if (std::holds_alternative<TogetherStmt>(stmt)) {
            findings.add("together");
        }
    }
}

}  // namespace

std::vector<std::string> findWorkflowConstructs(const CheckedProgram& checked) {
    Findings findings;
    scanContracts(checked.program.ast, findings);
    scanExprs(checked, findings);
    scanStmts(checked.program.ast, findings);
    return findings.take();
}

}  // namespace geas
