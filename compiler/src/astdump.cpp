// The declaration level of the parse dump: imports, records, sums,
// provisional clauses, contracts, and the carried over contract members.
// A pledge or subcontract prints its terms, and a subcontract its signals,
// only when it has some, so a file without workflow syntax dumps exactly as
// it always did
#include "astdumper.hpp"

#include <variant>

namespace geas {

void AstDumper::renderImports() {
    list(ast_.imports, 1, [&](const Import& import, int) {
        write("Import(");
        quoted(import.path);
        write(")");
    });
}

void AstDumper::renderDecl(const Decl& decl, int level) {
    std::visit(Overloaded{
                   [&](const RecordDecl& record) { renderRecord(record, level); },
                   [&](const SumDecl& sum) { renderSum(sum, level); },
                   [&](const ProvisionalDecl& decl) { renderProvisional(decl, level); },
                   [&](const ContractDecl& contract) { renderContract(contract, level); },
               },
               decl);
}

void AstDumper::renderFields(const std::vector<Field>& fields, int level) {
    list(fields, level, [&](const Field& field, int inner) { renderField(field, inner); });
}

void AstDumper::renderField(const Field& field, int level) {
    openStruct("Field");
    key(level + 1, "name");
    quoted(field.name);
    endField();
    key(level + 1, "ty");
    renderType(field.type, level + 1);
    endField();
    closeStruct(level);
}

void AstDumper::renderAttrs(const std::vector<Attr>& attrs, int level) {
    list(attrs, level, [&](const Attr& attr, int inner) {
        openStruct("Attr");
        key(inner + 1, "key");
        quoted(attr.key);
        endField();
        key(inner + 1, "value");
        renderExpr(attr.value, inner + 1);
        endField();
        closeStruct(inner);
    });
}

void AstDumper::renderRecord(const RecordDecl& record, int level) {
    openStruct("Record");
    key(level + 1, "name");
    quoted(record.name);
    endField();
    key(level + 1, "fields");
    renderFields(record.fields, level + 1);
    endField();
    closeStruct(level);
}

void AstDumper::renderSum(const SumDecl& sum, int level) {
    openStruct("Sum");
    key(level + 1, "name");
    quoted(sum.name);
    endField();
    key(level + 1, "variants");
    list(sum.variants, level + 1, [&](const Variant& variant, int inner) {
        openStruct("Variant");
        key(inner + 1, "name");
        quoted(variant.name);
        endField();
        key(inner + 1, "fields");
        renderFields(variant.fields, inner + 1);
        endField();
        closeStruct(inner);
    });
    endField();
    closeStruct(level);
}

void AstDumper::renderProvisional(const ProvisionalDecl& provisional, int level) {
    openStruct("Provisional");
    key(level + 1, "internal");
    boolean(provisional.isInternal);
    endField();
    key(level + 1, "name");
    quoted(provisional.name);
    endField();
    key(level + 1, "sigs");
    list(provisional.sigs, level + 1, [&](const ClauseSig& sig, int inner) {
        openStruct("ClauseSig");
        key(inner + 1, "name");
        quoted(sig.name);
        endField();
        key(inner + 1, "params");
        renderFields(sig.params, inner + 1);
        endField();
        key(inner + 1, "ret");
        renderType(sig.ret, inner + 1);
        endField();
        closeStruct(inner);
    });
    endField();
    closeStruct(level);
}

void AstDumper::renderContract(const ContractDecl& contract, int level) {
    openStruct("Contract");
    key(level + 1, "internal");
    boolean(contract.isInternal);
    endField();
    key(level + 1, "name");
    quoted(contract.name);
    endField();
    key(level + 1, "attrs");
    renderAttrs(contract.attrs, level + 1);
    endField();
    key(level + 1, "members");
    list(contract.members, level + 1,
         [&](const Member& member, int inner) { renderMember(member, inner); });
    endField();
    closeStruct(level);
}

void AstDumper::renderMember(const Member& member, int level) {
    std::visit(Overloaded{
                   [&](const IncorporateDecl& incorporate) {
                       write("Incorporate(");
                       quoted(incorporate.name);
                       write(")");
                   },
                   [&](const VowDecl& vow) { renderVow(vow, level); },
                   [&](const PledgeDecl& pledge) { renderPledge(pledge, level); },
                   [&](const ClauseDecl& clause) { renderClause(clause, level); },
                   [&](const SubcontractDecl& sub) { renderSubcontract(sub, level); },
                   [&](const SchemaDecl& schema) { renderSchema(schema, level); },
                   [&](const RequirementsDecl& req) { renderRequirements(req, level); },
                   [&](const RemedyDecl& remedy) { renderRemedy(remedy, level); },
                   [&](const SignalDecl& signal) { renderSignal(signal, level); },
                   [&](const CourseDecl& course) { renderCourse(course, level); },
                   [&](const AmendsDecl& amends) { renderAmends(amends, level); },
               },
               member);
}

void AstDumper::renderVow(const VowDecl& vow, int level) {
    openStruct("Vow");
    key(level + 1, "name");
    quoted(vow.name);
    endField();
    key(level + 1, "ty");
    renderType(vow.type, level + 1);
    endField();
    key(level + 1, "init");
    if (vow.init) {
        renderExpr(*vow.init, level + 1);
    } else {
        write("None");
    }
    endField();
    closeStruct(level);
}

void AstDumper::renderPledge(const PledgeDecl& pledge, int level) {
    openStruct("Pledge");
    key(level + 1, "name");
    quoted(pledge.name);
    endField();
    key(level + 1, "params");
    renderFields(pledge.params, level + 1);
    endField();
    key(level + 1, "ret");
    renderType(pledge.ret, level + 1);
    endField();
    if (!pledge.terms.empty()) {
        key(level + 1, "terms");
        renderTerms(pledge.terms, level + 1);
        endField();
    }
    key(level + 1, "attrs");
    renderAttrs(pledge.attrs, level + 1);
    endField();
    key(level + 1, "body");
    renderOptionalBlock(pledge.body, level + 1);
    endField();
    closeStruct(level);
}

void AstDumper::renderClause(const ClauseDecl& clause, int level) {
    openStruct("Clause");
    key(level + 1, "name");
    quoted(clause.name);
    endField();
    key(level + 1, "params");
    renderFields(clause.params, level + 1);
    endField();
    key(level + 1, "ret");
    renderType(clause.ret, level + 1);
    endField();
    key(level + 1, "body");
    renderBlock(clause.body, level + 1);
    endField();
    closeStruct(level);
}

}  // namespace geas
