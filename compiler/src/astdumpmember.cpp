// The dump of subcontracts, schemas, and the requirements block with its
// boolean formulas
#include "astdumper.hpp"

#include <variant>

namespace geas {

void AstDumper::renderSubcontract(const SubcontractDecl& subcontract, int level) {
    openStruct("Subcontract");
    key(level + 1, "name");
    if (subcontract.name) {
        quoted(*subcontract.name);
    } else {
        write("None");
    }
    endField();
    if (subcontract.isTransactional) {
        key(level + 1, "transactional");
        write("true");
        endField();
    }
    if (!subcontract.terms.empty()) {
        key(level + 1, "terms");
        renderTerms(subcontract.terms, level + 1);
        endField();
    }
    if (!subcontract.attrs.empty()) {
        key(level + 1, "attrs");
        renderAttrs(subcontract.attrs, level + 1);
        endField();
    }
    renderSubMembers(subcontract.members, level);
    closeStruct(level);
}

// The pledges, then the signals when there are any, each in source order
void AstDumper::renderSubMembers(const std::vector<SubMember>& members, int level) {
    std::vector<const PledgeDecl*> pledges;
    std::vector<const SignalDecl*> signals;
    for (const SubMember& member : members) {
        if (const auto* pledge = std::get_if<PledgeDecl>(&member)) {
            pledges.push_back(pledge);
        } else {
            signals.push_back(&std::get<SignalDecl>(member));
        }
    }
    key(level + 1, "pledges");
    list(pledges, level + 1,
         [&](const PledgeDecl* pledge, int inner) { renderPledge(*pledge, inner); });
    endField();
    if (signals.empty()) {
        return;
    }
    key(level + 1, "signals");
    list(signals, level + 1,
         [&](const SignalDecl* signal, int inner) { renderSignal(*signal, inner); });
    endField();
}

void AstDumper::renderSchema(const SchemaDecl& schema, int level) {
    openStruct("Schema");
    key(level + 1, "name");
    quoted(schema.name);
    endField();
    key(level + 1, "cols");
    renderFields(schema.columns, level + 1);
    endField();
    closeStruct(level);
}

void AstDumper::renderRequirements(const RequirementsDecl& requirements, int level) {
    openStruct("Requirements");
    key(level + 1, "fulfill");
    renderReqSlot(requirements.fulfill, level + 1);
    endField();
    key(level + 1, "partial");
    renderReqSlot(requirements.partial, level + 1);
    endField();
    key(level + 1, "break");
    renderReqSlot(requirements.breakLine, level + 1);
    endField();
    closeStruct(level);
}

void AstDumper::renderReqSlot(const std::optional<ReqId>& slot, int level) {
    if (slot) {
        renderReq(*slot, level);
    } else {
        write("None");
    }
}

// Atom("x") on one line, Not, And, and Or as wrappers over their operands
void AstDumper::renderReq(ReqId id, int level) {
    std::visit(Overloaded{
                   [&](const ReqAtom& atom) {
                       write("Atom(");
                       quoted(atom.name);
                       write(")");
                   },
                   [&](const ReqNot& negation) {
                       openWrap("Not", level);
                       renderReq(negation.operand, level + 1);
                       closeWrap(level);
                   },
                   [&](const ReqAnd& conjunction) {
                       openWrap("And", level);
                       renderReq(conjunction.lhs, level + 1);
                       endField();
                       indent(level + 1);
                       renderReq(conjunction.rhs, level + 1);
                       closeWrap(level);
                   },
                   [&](const ReqOr& disjunction) {
                       openWrap("Or", level);
                       renderReq(disjunction.lhs, level + 1);
                       endField();
                       indent(level + 1);
                       renderReq(disjunction.rhs, level + 1);
                       closeWrap(level);
                   },
               },
               ast_.req(id).kind);
}

void AstDumper::renderOptionalBlock(const std::optional<BlockId>& body, int level) {
    if (body) {
        renderBlock(*body, level);
    } else {
        write("None");
    }
}

}  // namespace geas
