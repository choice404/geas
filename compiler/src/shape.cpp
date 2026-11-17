// Contract shapes, see shape.hpp
#include "shape.hpp"

#include <array>
#include <charconv>
#include <set>
#include <unordered_set>
#include <utility>

#include "hash.hpp"
#include "layout.hpp"

namespace geas {

namespace {

constexpr std::int64_t defaultVersion = 1;
constexpr int looseOrdinal = -1;

// Room for the shortest exact spelling of any double
constexpr std::size_t floatBufferSize = 32;

// Joins names with the separator the shape string uses inside a term
std::string joinNames(const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) {
            out.push_back('+');
        }
        out.append(names[i]);
    }
    return out;
}

// A backoff multiplier in its shortest exact spelling, so 2.0 and 2.00 hash
// alike. A spelling that does not parse is kept as written
std::string canonicalFloat(const std::string& spelling) {
    std::string digits;
    for (const char c : spelling) {
        if (c != '_') {
            digits.push_back(c);
        }
    }
    double value = 0.0;
    const char* end = digits.data() + digits.size();
    const auto parsed = std::from_chars(digits.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return spelling;
    }
    std::array<char, floatBufferSize> buffer{};
    const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string{buffer.data(), written.ptr};
}

std::string retrySpelling(const RetryTerm& retry) {
    std::string out = "retry=" + std::to_string(retry.count);
    if (retry.everyMilliseconds) {
        out.append("/every=" + std::to_string(*retry.everyMilliseconds));
    }
    if (retry.backoff) {
        out.append("/backoff=" + canonicalFloat(*retry.backoff));
    }
    if (retry.capMilliseconds) {
        out.append("/cap=" + std::to_string(*retry.capMilliseconds));
    }
    if (!retry.on.empty()) {
        out.append("/on=" + joinNames(retry.on));
    }
    return out;
}

// The terms segment of one declaration, empty when it carries no term
std::string termsSegment(std::string_view kind, std::string_view name,
                         const std::vector<Term>& terms) {
    if (terms.empty()) {
        return "";
    }
    std::string out = "terms " + std::string{kind} + " " + std::string{name} + ":";
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        if (const auto* within = std::get_if<WithinTerm>(&terms[i].kind)) {
            out.append("within=" + std::to_string(within->milliseconds));
        } else if (const auto* after = std::get_if<AfterTerm>(&terms[i].kind)) {
            out.append("after=" + joinNames(after->names));
        } else {
            out.append(retrySpelling(std::get<RetryTerm>(terms[i].kind)));
        }
    }
    out.push_back(';');
    return out;
}

template <typename Map, typename Key>
CallableSig sigOr(const Map& map, const Key* key) {
    const auto found = map.find(key);
    return found == map.end() ? CallableSig{} : found->second;
}

class ShapeBuilder {
public:
    ShapeBuilder(const Ast& ast, const Sigs& sigs, const TypeArena& types,
                 const CheckTables& tables)
        : ast_{ast}, sigs_{sigs}, types_{types}, tables_{tables}, layouts_{ast, sigs, types} {}

    ContractShape build(const ContractDecl& contract);

private:
    void collect(const ContractDecl& contract, ContractShape& shape);
    void collectSub(const SubcontractDecl& sub, ContractShape& shape);
    void addPledge(const PledgeDecl& pledge, int subOrdinal, ContractShape& shape);
    void addRemedy(const RemedyDecl& remedy, ContractShape& shape);
    void collectAtoms(ReqId id, ContractShape& shape, std::unordered_set<std::string>& seen);
    std::uint64_t signatureHash(const std::string& signature, const CallableSig& sig) const;
    std::vector<TypeLayout> contractLayouts(const ContractShape& shape) const;
    std::string shapeString(const ContractShape& shape) const;
    std::string classicSegments(const ContractShape& shape) const;
    std::string workflowSegments(const ContractShape& shape) const;
    std::string termSegments(const ContractShape& shape) const;

    const Ast& ast_;
    const Sigs& sigs_;
    const TypeArena& types_;
    const CheckTables& tables_;
    LayoutIndex layouts_;
};

// The hash a mangled name carries: the signature spelling, then the layout
// of every declared type the signature reaches, so a signature that reaches
// none hashes as its spelling alone
std::uint64_t ShapeBuilder::signatureHash(const std::string& signature,
                                          const CallableSig& sig) const {
    std::set<std::string> names;
    layouts_.reachSignature(sig, names);
    return fnv1a64(signature + layoutSegments(layouts_.layoutsOf(names)));
}

// The layouts of every declared type the boundary reaches: the vows, the
// pledge and signal signatures, and the remedy signatures
std::vector<TypeLayout> ShapeBuilder::contractLayouts(const ContractShape& shape) const {
    std::set<std::string> names;
    for (const VowDecl* vow : shape.vows) {
        const auto sig = sigs_.vows.find(vow);
        if (sig != sigs_.vows.end()) {
            layouts_.reach(sig->second.type, names);
        }
    }
    for (const PledgeShape& pledge : shape.pledges) {
        layouts_.reachSignature(sigOr(sigs_.pledges, pledge.decl), names);
    }
    for (const SignalShape& signal : shape.signals) {
        layouts_.reachSignature(sigOr(sigs_.signals, signal.decl), names);
    }
    for (const RemedyShape& remedy : shape.remedies) {
        layouts_.reachSignature(sigOr(sigs_.remedies, remedy.decl), names);
    }
    return layouts_.layoutsOf(names);
}

void ShapeBuilder::addPledge(const PledgeDecl& pledge, int subOrdinal, ContractShape& shape) {
    const CallableSig sig = sigOr(sigs_.pledges, &pledge);
    std::string signature = signatureCanon(types_, sig);
    const std::uint64_t sigHash = signatureHash(signature, sig);
    std::string mangled = mangleSymbol(shape.decl->name, pledge.name, sigHash, shape.version);
    shape.pledges.push_back(
        PledgeShape{&pledge, subOrdinal, std::move(signature), sigHash, std::move(mangled)});
}

void ShapeBuilder::addRemedy(const RemedyDecl& remedy, ContractShape& shape) {
    const CallableSig sig = sigOr(sigs_.remedies, &remedy);
    std::string signature = signatureCanon(types_, sig);
    const std::uint64_t sigHash = signatureHash(signature, sig);
    std::string mangled = mangleSymbol(shape.decl->name, remedy.name, sigHash, shape.version);
    const auto target = tables_.remedyTargets.find(&remedy);
    const PledgeDecl* pledge = target == tables_.remedyTargets.end() ? nullptr : target->second;
    shape.remedies.push_back(
        RemedyShape{&remedy, pledge, std::move(signature), sigHash, std::move(mangled)});
}

void ShapeBuilder::collectSub(const SubcontractDecl& sub, ContractShape& shape) {
    const int ordinal = static_cast<int>(shape.subs.size());
    shape.subs.push_back(&sub);
    for (const SubMember& member : sub.members) {
        if (const auto* pledge = std::get_if<PledgeDecl>(&member)) {
            addPledge(*pledge, ordinal, shape);
        } else if (const auto* signal = std::get_if<SignalDecl>(&member)) {
            shape.signals.push_back(SignalShape{signal, ordinal});
        }
    }
}

// The atoms of one requirements line in first appearance order, the order
// the satisfiability check and the runtime evaluator both index by
void ShapeBuilder::collectAtoms(ReqId id, ContractShape& shape,
                                std::unordered_set<std::string>& seen) {
    const ReqNode& node = ast_.req(id);
    if (const auto* atom = std::get_if<ReqAtom>(&node.kind)) {
        if (seen.insert(atom->name).second) {
            shape.requirementAtoms.push_back(atom->name);
        }
    } else if (const auto* negation = std::get_if<ReqNot>(&node.kind)) {
        collectAtoms(negation->operand, shape, seen);
    } else if (const auto* conjunction = std::get_if<ReqAnd>(&node.kind)) {
        collectAtoms(conjunction->lhs, shape, seen);
        collectAtoms(conjunction->rhs, shape, seen);
    } else if (const auto* disjunction = std::get_if<ReqOr>(&node.kind)) {
        collectAtoms(disjunction->lhs, shape, seen);
        collectAtoms(disjunction->rhs, shape, seen);
    }
}

void ShapeBuilder::collect(const ContractDecl& contract, ContractShape& shape) {
    for (const Member& member : contract.members) {
        if (const auto* vow = std::get_if<VowDecl>(&member)) {
            shape.vows.push_back(vow);
        } else if (const auto* pledge = std::get_if<PledgeDecl>(&member)) {
            addPledge(*pledge, looseOrdinal, shape);
        } else if (const auto* sub = std::get_if<SubcontractDecl>(&member)) {
            collectSub(*sub, shape);
        } else if (const auto* signal = std::get_if<SignalDecl>(&member)) {
            shape.signals.push_back(SignalShape{signal, looseOrdinal});
        } else if (const auto* remedy = std::get_if<RemedyDecl>(&member)) {
            addRemedy(*remedy, shape);
        } else if (const auto* schema = std::get_if<SchemaDecl>(&member)) {
            shape.schemas.push_back(schema);
        } else if (const auto* requirements = std::get_if<RequirementsDecl>(&member)) {
            if (shape.requirements == nullptr) {
                shape.requirements = requirements;
            }
        } else if (const auto* course = std::get_if<CourseDecl>(&member)) {
            if (shape.course == nullptr) {
                shape.course = course;
            }
        }
    }
}

// The segments every contract has: the contract name, then each vow,
// pledge, and schema under the canonical type spellings
std::string ShapeBuilder::classicSegments(const ContractShape& shape) const {
    std::string out = "contract " + shape.decl->name + ";";
    for (const VowDecl* vow : shape.vows) {
        const auto sig = sigs_.vows.find(vow);
        const std::string_view type =
            sig == sigs_.vows.end() ? std::string_view{} : types_.canon(sig->second.type);
        out.append("vow " + vow->name + ":" + std::string{type} + ";");
    }
    for (const PledgeShape& pledge : shape.pledges) {
        out.append("pledge " + pledge.decl->name + ":" + pledge.signature + ";");
    }
    for (const SchemaDecl* schema : shape.schemas) {
        out.append("schema " + schema->name + ":");
        const RecordSig* row = sigs_.record(schema->name);
        const std::size_t count = row == nullptr ? 0 : row->fields.size();
        for (std::size_t i = 0; i < count; ++i) {
            if (i > 0) {
                out.push_back(',');
            }
            out.append(row->fields[i].name + ":" + std::string{types_.canon(row->fields[i].type)});
        }
        out.push_back(';');
    }
    return out;
}

// One terms segment per declaration that carries terms. An anonymous
// subcontract is named by its ordinal, the only stable name it has
std::string ShapeBuilder::termSegments(const ContractShape& shape) const {
    std::string out;
    for (const PledgeShape& pledge : shape.pledges) {
        out.append(termsSegment("pledge", pledge.decl->name, pledge.decl->terms));
    }
    for (std::size_t i = 0; i < shape.subs.size(); ++i) {
        const SubcontractDecl* sub = shape.subs[i];
        const std::string name = sub->name ? *sub->name : "#" + std::to_string(i);
        out.append(termsSegment("subcontract", name, sub->terms));
    }
    for (const SignalShape& signal : shape.signals) {
        out.append(termsSegment("signal", signal.decl->name, signal.decl->terms));
    }
    for (const RemedyShape& remedy : shape.remedies) {
        out.append(termsSegment("remedy", remedy.decl->name, remedy.decl->terms));
    }
    return out;
}

// The segments the workflow surface adds, each present only when the
// contract has the construct, so nothing is appended for a contract without
std::string ShapeBuilder::workflowSegments(const ContractShape& shape) const {
    std::string out;
    for (const SignalShape& signal : shape.signals) {
        const CallableSig sig = sigOr(sigs_.signals, signal.decl);
        out.append("signal " + signal.decl->name + ":" + signatureCanon(types_, sig) + ";");
    }
    for (const RemedyShape& remedy : shape.remedies) {
        out.append("remedy " + remedy.decl->name + ":" + remedy.decl->target + ":" +
                   remedy.signature + ";");
    }
    out.append(termSegments(shape));
    if (shape.course != nullptr) {
        out.append("course;");
    }
    if (shape.termMilliseconds) {
        out.append("term " + std::to_string(*shape.termMilliseconds) + ";");
    }
    return out;
}

// The layout segments go last, so a contract that reaches no declared
// record or sum keeps the string it had before layouts were spelled
std::string ShapeBuilder::shapeString(const ContractShape& shape) const {
    return classicSegments(shape) + workflowSegments(shape) + layoutSegments(shape.layouts);
}

ContractShape ShapeBuilder::build(const ContractDecl& contract) {
    ContractShape shape{};
    shape.decl = &contract;
    shape.version = contractVersion(ast_, contract);
    shape.termMilliseconds = contractTerm(ast_, contract);
    collect(contract, shape);
    if (shape.requirements != nullptr) {
        std::unordered_set<std::string> seen;
        const RequirementsDecl& block = *shape.requirements;
        for (const std::optional<ReqId>& line : {block.fulfill, block.partial, block.breakLine}) {
            if (line) {
                collectAtoms(*line, shape, seen);
            }
        }
    }
    shape.layouts = contractLayouts(shape);
    shape.shape = shapeString(shape);
    shape.shapeHash = fnv1a64(shape.shape);
    return shape;
}

}  // namespace

std::int64_t contractVersion(const Ast& ast, const ContractDecl& contract) {
    for (const Attr& attr : contract.attrs) {
        if (attr.key != "version") {
            continue;
        }
        // TODO: a version that is not an integer literal is a build fault,
        // reported when code generation lands
        if (const auto* literal = std::get_if<IntLiteral>(&ast.expr(attr.value).kind)) {
            return literal->value;
        }
    }
    return defaultVersion;
}

std::optional<std::int64_t> contractTerm(const Ast& ast, const ContractDecl& contract) {
    for (const Attr& attr : contract.attrs) {
        if (attr.key != "term") {
            continue;
        }
        if (const auto* literal = std::get_if<DurationLiteral>(&ast.expr(attr.value).kind)) {
            return literal->milliseconds;
        }
    }
    return std::nullopt;
}

ContractShape buildContractShape(const Ast& ast, const Sigs& sigs, const TypeArena& types,
                                 const CheckTables& tables, const ContractDecl& contract) {
    return ShapeBuilder{ast, sigs, types, tables}.build(contract);
}

std::vector<ContractShape> buildContractShapes(const Program& program, const Sigs& sigs,
                                               const TypeArena& types, const CheckTables& tables) {
    ShapeBuilder builder{program.ast, sigs, types, tables};
    std::vector<ContractShape> shapes;
    for (const Decl& decl : program.ast.decls) {
        if (const auto* contract = std::get_if<ContractDecl>(&decl)) {
            shapes.push_back(builder.build(*contract));
        }
    }
    return shapes;
}

}  // namespace geas
