// Remedies in a compiled module: a thunk per remedy with a body, in the
// uniform frame a pledge thunk uses with the target's Ok value in slot 0
// when the remedy takes it, then the remedy table in declaration order. An
// abstract remedy carries NULL where a compiled one carries its thunk and
// binds by "Contract.remedy" or its mangled name, the way an abstract pledge
// does. The descriptor's workflow tail closes the file's view of a contract
#include "emitc.hpp"
#include "attributes.hpp"
#include "emittext.hpp"

namespace geas {

namespace {

std::string countOf(std::size_t count) {
    return decimal(static_cast<std::int64_t>(count));
}

}  // namespace

// The parameter checks and bindings of one frame, the argument count first
void CEmitter::emitFrameChecks(const std::vector<Field>& params, const CallableSig& sig) {
    line(joined("if (nargs != ", countOf(params.size()), ") return GEAS_ERR_TYPE;"));
    for (std::size_t i = 0; i < params.size(); ++i) {
        const TyId type = i < sig.params.size() ? sig.params[i] : unknown();
        const std::string_view tag = lookup_.tagOf(type);
        const std::string index = countOf(i);
        if (!tag.empty()) {
            line(joined("if (args[", index, "].ty != ", tag, ") return GEAS_ERR_TYPE;"));
        }
        declare(params[i].name, joined("args[", index, "]"), type);
    }
}

// Emits one remedy thunk. A body that runs off its end reports Unit, which
// the remedy pass reads as success the way it reads an Ok
void CEmitter::emitRemedyThunk(const RemedyShape& remedy) {
    const RemedyDecl& decl = *remedy.decl;
    const CallableSig sig = lookup_.remedySig(decl);
    beginBody(sig.ret, decl.lo, decl.hi);
    bool hasBindingAttrs = false;
    for (const Attr& attr : decl.attrs) {
        hasBindingAttrs = hasBindingAttrs || isBuiltinAttr(attr.key, AttrTarget::Remedy);
    }
    if (hasBindingAttrs) {
        lookup_.unsupported(decl.lo, decl.hi, "remedy attributes in a compiled module");
    }
    std::vector<Field> params;
    if (decl.param) {
        params.push_back(*decl.param);
    }
    pushScope();
    emitFrameChecks(params, sig);
    genBlock(*decl.body);
    popScope();
    top(joined("static GeasStatus ", remedy.mangled,
               "(void* ctx, const GeasValue* args, size_t nargs,"));
    top("                           GeasValue* out) {");
    top("    GeasContract* cc = (GeasContract*)ctx;");
    top("    (void)cc;");
    top("    (void)args;");
    endBody();
}

// The pledges table index of the pledge a remedy undoes
std::int64_t CEmitter::remedyTargetIndex(const ContractShape& shape,
                                         const RemedyShape& remedy) const {
    for (std::size_t i = 0; i < shape.pledges.size(); ++i) {
        if (shape.pledges[i].decl == remedy.target) {
            return static_cast<std::int64_t>(i);
        }
    }
    lookup_.fault(remedy.decl->lo, remedy.decl->hi,
                  joined("codegen: remedy '", remedy.decl->name, "' names no pledge of the table"));
    return 0;
}

// Whether the remedy is handed its target's Ok value: it declared the
// parameter and the target answers a Result whose Ok carries a value
bool CEmitter::remedyTakesValue(const RemedyShape& remedy) const {
    return geas::remedyTakesValue(lookup_, remedy);
}

// The remedy table, NULL when the contract declares no remedy. The arrays
// each remedy's terms point at land first
std::string CEmitter::emitRemedyTable(const ContractShape& shape) {
    if (shape.remedies.empty()) {
        return "NULL";
    }
    std::vector<std::string> rows;
    for (std::size_t i = 0; i < shape.remedies.size(); ++i) {
        const RemedyShape& remedy = shape.remedies[i];
        const RemedyDecl& decl = *remedy.decl;
        const std::string terms =
            termEntry(shape, decl.terms, joined("remedy_", countOf(i)), decl.lo, decl.hi);
        const std::string_view thunk = decl.body ? std::string_view{remedy.mangled} : "NULL";
        rows.push_back(joined("    { \"", decl.name, "\", \"", remedy.mangled, "\", ",
                              decimal(remedyTargetIndex(shape, remedy)), ", ",
                              remedyTakesValue(remedy) ? "1" : "0", ", ", thunk, ", ", terms,
                              " },"));
    }
    top(joined("static const GeasRemedyDesc ", contractStem(shape.decl->name), "_remedies[] = {"));
    for (const std::string& row : rows) {
        top(row);
    }
    top("};");
    top("");
    return joined(contractStem(shape.decl->name), "_remedies");
}

// Every workflow table of one contract and the descriptor lines that point
// at them, empty for a contract outside the workflow surface
std::vector<std::string> CEmitter::emitWorkflowTail(const ContractShape& shape) {
    if (!hasWorkflowTail(shape)) {
        return {};
    }
    const std::string kinds = emitKinds(shape);
    const std::string terms = emitPledgeTerms(shape);
    const std::string subTerms = emitSubTerms(shape);
    const std::string remedies = emitRemedyTable(shape);
    const std::int64_t termMilliseconds = shape.termMilliseconds.value_or(0);
    return {
        joined("    ", kinds, ", ", terms, ", ", subTerms, ","),
        joined("    ", countOf(shape.remedies.size()), ", ", remedies, ","),
        joined("    ", decimal(termMilliseconds), ","),
    };
}

}  // namespace geas
