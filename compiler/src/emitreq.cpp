// The requirements surface of one contract. The descriptor grows three
// pieces here: the subs table naming every subcontract in declaration
// order, the atom table, and one postfix program per policy line. A
// contract that wrote a requirements block gets its lines serialized from
// the same tree the satisfiability check walked, atoms in the same first
// appearance order. A contract without a block gets the grammar's defaults
// synthesized: fulfill when every subcontract and every loose pledge and
// signal is fulfilled, partial when at least one subcontract is, break when
// everything is broken. A line the source did not write emits no array;
// length zero is the runtime's constant false
#include <algorithm>
#include <limits>

#include "emitc.hpp"
#include "emittext.hpp"

namespace geas {

namespace {

constexpr int opAtom = 0;
constexpr int opNot = 1;
constexpr int opAnd = 2;
constexpr int opOr = 3;
constexpr int atomFulfilled = 0;
constexpr int atomBroken = 1;
constexpr std::int64_t noIndex = -1;
constexpr std::int64_t maxAtomIndex = std::numeric_limits<std::uint8_t>::max();
constexpr std::int64_t defaultAtomKinds = 2;

std::string_view opName(int op) {
    switch (op) {
        case opAtom: return "GEAS_REQ_ATOM";
        case opNot: return "GEAS_REQ_NOT";
        case opAnd: return "GEAS_REQ_AND";
        default: return "GEAS_REQ_OR";
    }
}

std::string_view atomKindName(int kind) {
    return kind == atomBroken ? "GEAS_ATOM_BROKEN" : "GEAS_ATOM_FULFILLED";
}

// A chain program over a run of atom indices: AND or OR folded left, the
// postfix spelling atom0 atom1 op atom2 op and so on
std::vector<RequirementOp> chainOf(const std::vector<std::int64_t>& atoms, int op) {
    std::vector<RequirementOp> ops;
    for (std::size_t i = 0; i < atoms.size(); ++i) {
        ops.push_back(RequirementOp{opAtom, atoms[i]});
        if (i > 0) {
            ops.push_back(RequirementOp{op, 0});
        }
    }
    return ops;
}

std::int64_t sizeOf(const std::vector<RequirementOp>& ops) {
    return static_cast<std::int64_t>(ops.size());
}

// The name on one pledges table row, a pledge's or, past every pledge, a
// signal's
std::string_view rowName(const ContractShape& shape, std::int64_t index) {
    const auto row = static_cast<std::size_t>(index);
    if (row < shape.pledges.size()) {
        return shape.pledges[row].decl->name;
    }
    return shape.signals[row - shape.pledges.size()].decl->name;
}

}  // namespace

// The whole requirements surface of one contract: the subs table, then the
// source block serialized or the defaults synthesized
RequirementCounts CEmitter::emitRequirements(const ContractShape& shape) {
    emitSubNames(shape);
    if (shape.requirements != nullptr) {
        return emitSourceBlock(shape);
    }
    return emitDefaultBlock(shape);
}

void CEmitter::emitSubNames(const ContractShape& shape) {
    if (shape.subs.empty()) {
        return;
    }
    top(joined("static const char* const ", contractStem(shape.decl->name), "_subs[] = {"));
    for (const SubcontractDecl* sub : shape.subs) {
        top(sub->name ? joined("    \"", *sub->name, "\",") : std::string{"    NULL,"});
    }
    top("};");
    top("");
}

// One emitted atom row: the item's name as a C literal or NULL for an
// anonymous subcontract, the latch the atom tests, and the indices
void CEmitter::emitAtomRow(std::optional<std::string_view> name, int kind, std::int64_t sub,
                           std::int64_t pledge) {
    const std::string spelled = name ? joined("\"", *name, "\"") : std::string{"NULL"};
    top(joined("    { ", spelled, ", ", atomKindName(kind), ", ", decimal(sub), ", ",
               decimal(pledge), " },"));
}

// One emitted postfix program, nothing for an empty one
void CEmitter::emitOps(std::string_view tag, const std::vector<RequirementOp>& ops) {
    if (ops.empty()) {
        return;
    }
    top(joined("static const GeasReqOp ", contractStem(shape_->decl->name), "_", tag, "[] = {"));
    for (const RequirementOp& op : ops) {
        top(joined("    { ", opName(op.op), ", ", decimal(op.atom), " },"));
    }
    top("};");
    top("");
}

// The postfix program of one source line, a post order walk of the tree.
// Atom indices come from the shape's first appearance order, the same
// order the satisfiability check saw
void CEmitter::buildPostfix(ReqId id, const ContractShape& shape,
                            std::vector<RequirementOp>& ops) const {
    const ReqNode& node = ast().req(id);
    if (const auto* atom = std::get_if<ReqAtom>(&node.kind)) {
        const auto& atoms = shape.requirementAtoms;
        const auto found = std::find(atoms.begin(), atoms.end(), atom->name);
        const auto index = found == atoms.end() ? 0 : found - atoms.begin();
        ops.push_back(RequirementOp{opAtom, static_cast<std::int64_t>(index)});
    } else if (const auto* negation = std::get_if<ReqNot>(&node.kind)) {
        buildPostfix(negation->operand, shape, ops);
        ops.push_back(RequirementOp{opNot, 0});
    } else if (const auto* conjunction = std::get_if<ReqAnd>(&node.kind)) {
        buildPostfix(conjunction->lhs, shape, ops);
        buildPostfix(conjunction->rhs, shape, ops);
        ops.push_back(RequirementOp{opAnd, 0});
    } else if (const auto* disjunction = std::get_if<ReqOr>(&node.kind)) {
        buildPostfix(disjunction->lhs, shape, ops);
        buildPostfix(disjunction->rhs, shape, ops);
        ops.push_back(RequirementOp{opOr, 0});
    }
}

// Resolves one source atom to its descriptor indices and emits its row: the
// sub ordinal for a named subcontract, the flattened pledge index for a
// loose pledge. The resolver validated every atom, so a miss here is a
// compiler fault worth a build error, never silently wrong indices
void CEmitter::emitSourceAtom(const ContractShape& shape, const std::string& name) {
    const MemberEntry* member = lookup_.resolved().member(shape.decl, name);
    std::int64_t sub = noIndex;
    std::int64_t pledge = noIndex;
    if (member != nullptr && member->kind() == MemberKind::Subcontract) {
        const auto found = std::find(shape.subs.begin(), shape.subs.end(), member->subcontract());
        sub = found == shape.subs.end() ? noIndex : found - shape.subs.begin();
    }
    const bool isLoosePledge =
        member != nullptr && member->kind() == MemberKind::Pledge && member->owner == nullptr;
    for (std::size_t i = 0; isLoosePledge && i < shape.pledges.size(); ++i) {
        if (shape.pledges[i].decl == member->pledge()) {
            pledge = static_cast<std::int64_t>(i);
            break;
        }
    }
    // A loose signal is an atom on its pledges table row, after every pledge
    const bool isLooseSignal =
        member != nullptr && member->kind() == MemberKind::Signal && member->owner == nullptr;
    for (std::size_t i = 0; isLooseSignal && i < shape.signals.size(); ++i) {
        if (shape.signals[i].decl == member->signal()) {
            pledge = static_cast<std::int64_t>(shape.pledges.size() + i);
            break;
        }
    }
    if (sub == noIndex && pledge == noIndex) {
        lookup_.fault(member == nullptr ? 0 : member->lo, member == nullptr ? 0 : member->hi,
                      joined("codegen: requirements atom '", name, "' did not resolve to an item"));
    }
    emitAtomRow(name, atomFulfilled, sub, pledge);
}

// The explicit path: the source block serialized. Emits the atom table and
// up to three programs, and fills the counts
RequirementCounts CEmitter::emitSourceBlock(const ContractShape& shape) {
    const auto atoms = static_cast<std::int64_t>(shape.requirementAtoms.size());
    if (atoms > 0) {
        top(joined("static const GeasReqAtom ", contractStem(shape.decl->name), "_atoms[] = {"));
        for (const std::string& name : shape.requirementAtoms) {
            emitSourceAtom(shape, name);
        }
        top("};");
        top("");
    }
    const RequirementsDecl& block = *shape.requirements;
    std::vector<RequirementOp> fulfill;
    std::vector<RequirementOp> partial;
    std::vector<RequirementOp> breakLine;
    if (block.fulfill) {
        buildPostfix(*block.fulfill, shape, fulfill);
    }
    if (block.partial) {
        buildPostfix(*block.partial, shape, partial);
    }
    if (block.breakLine) {
        buildPostfix(*block.breakLine, shape, breakLine);
    }
    emitOps("rf", fulfill);
    emitOps("rp", partial);
    emitOps("rb", breakLine);
    return RequirementCounts{static_cast<std::int64_t>(shape.subs.size()),
                             atoms,
                             true,
                             sizeOf(fulfill),
                             sizeOf(partial),
                             sizeOf(breakLine)};
}

// The default atom table: one fulfilled atom per item, subcontracts then
// loose pledges in declaration order, then the same items again as broken
// atoms
void CEmitter::emitDefaultAtoms(const ContractShape& shape,
                                const std::vector<std::int64_t>& loose) {
    top(joined("static const GeasReqAtom ", contractStem(shape.decl->name), "_atoms[] = {"));
    for (const int kind : {atomFulfilled, atomBroken}) {
        for (std::size_t i = 0; i < shape.subs.size(); ++i) {
            const SubcontractDecl* sub = shape.subs[i];
            const auto name =
                sub->name ? std::optional<std::string_view>{*sub->name} : std::nullopt;
            emitAtomRow(name, kind, static_cast<std::int64_t>(i), noIndex);
        }
        for (const std::int64_t index : loose) {
            emitAtomRow(rowName(shape, index), kind, noIndex, index);
        }
    }
    top("};");
    top("");
}

// The default path: the grammar's policy synthesized when the source wrote
// no block. fulfill is the AND chain over the fulfilled atoms, partial the
// OR chain over the fulfilled subcontract atoms, break the AND chain over
// the broken atoms; a contract with no items emits no arrays
RequirementCounts CEmitter::emitDefaultBlock(const ContractShape& shape) {
    const auto subs = static_cast<std::int64_t>(shape.subs.size());
    std::vector<std::int64_t> loose;
    for (std::size_t i = 0; i < shape.pledges.size(); ++i) {
        if (shape.pledges[i].subOrdinal < 0) {
            loose.push_back(static_cast<std::int64_t>(i));
        }
    }
    for (std::size_t i = 0; i < shape.signals.size(); ++i) {
        if (shape.signals[i].subOrdinal < 0) {
            loose.push_back(static_cast<std::int64_t>(shape.pledges.size() + i));
        }
    }
    const std::int64_t items = subs + static_cast<std::int64_t>(loose.size());
    // Larger defaults use the runtime's structural policy over the item tables
    if (items == 0 || items * defaultAtomKinds > maxAtomIndex + 1) {
        return RequirementCounts{subs, 0, false, 0, 0, 0};
    }
    emitDefaultAtoms(shape, loose);
    // Fulfilled items sit at 0 to items - 1, broken ones right after
    std::vector<std::int64_t> fulfilled;
    std::vector<std::int64_t> fulfilledSubs;
    std::vector<std::int64_t> broken;
    for (std::int64_t i = 0; i < items; ++i) {
        fulfilled.push_back(i);
        broken.push_back(items + i);
        if (i < subs) {
            fulfilledSubs.push_back(i);
        }
    }
    const std::vector<RequirementOp> fulfill = chainOf(fulfilled, opAnd);
    const std::vector<RequirementOp> partial = chainOf(fulfilledSubs, opOr);
    const std::vector<RequirementOp> breakLine = chainOf(broken, opAnd);
    emitOps("rf", fulfill);
    emitOps("rp", partial);
    emitOps("rb", breakLine);
    return RequirementCounts{
        subs, items * defaultAtomKinds, false, sizeOf(fulfill), sizeOf(partial), sizeOf(breakLine)};
}

}  // namespace geas
