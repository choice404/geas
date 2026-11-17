// The amends table of one contract's descriptor. Each amends N block lowers
// to one GeasAmendDesc: the names of N's vows, items, subcontracts, and
// remedies in N's order, one map per table from N's index to this version's
// or -1, the latch seeds of the items this version adds, whether the course
// restarts, and the hashes the park row has to carry. A block with vow
// items gets a vows function that evaluates each value with prior.NAME read
// from N's vows and writes an instance owned copy into this version's slot.
// A contract without an amends block emits nothing here, and its
// descriptor names no amends field
#include "emitc.hpp"
#include "emittext.hpp"
#include "hash.hpp"
#include "typecode.hpp"

namespace geas {

namespace {

constexpr std::string_view nullText = "NULL";

std::string countOf(std::size_t count) {
    return decimal(static_cast<std::int64_t>(count));
}

// The runtime's name for the state a latch seed starts in
std::string_view seedStateName(LatchState state) {
    switch (state) {
        case LatchState::Pending: return "GEAS_AMEND_PENDING";
        case LatchState::Fulfilled: return "GEAS_AMEND_FULFILLED";
        case LatchState::Broken: return "GEAS_AMEND_BROKEN";
    }
    return "GEAS_AMEND_PENDING";
}

std::vector<std::string> itemNames(const RecordedShape& shape) {
    std::vector<std::string> names;
    for (const RecordedItem& item : shape.items) {
        names.push_back(item.name);
    }
    return names;
}

std::vector<std::string> vowNames(const RecordedShape& shape) {
    std::vector<std::string> names;
    for (const RecordedVow& vow : shape.vows) {
        names.push_back(vow.name);
    }
    return names;
}

std::vector<std::string> remedyNames(const RecordedShape& shape) {
    std::vector<std::string> names;
    for (const RecordedRemedy& remedy : shape.remedies) {
        names.push_back(remedy.name);
    }
    return names;
}

std::int32_t vowIndex(const ContractShape& shape, std::string_view name) {
    for (std::size_t i = 0; i < shape.vows.size(); ++i) {
        if (shape.vows[i]->name == name) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

}  // namespace

// A table of old names, an anonymous subcontract as a NULL entry
std::string CEmitter::emitAmendNames(std::string_view symbol,
                                     const std::vector<std::string>& names) {
    if (names.empty()) {
        return std::string{nullText};
    }
    top(joined("static const char* const ", symbol, "[] = {"));
    for (const std::string& name : names) {
        top(name.empty() ? std::string{"    NULL,"} : joined("    \"", name, "\","));
    }
    top("};");
    top("");
    return std::string{symbol};
}

std::string CEmitter::emitAmendMap(std::string_view symbol, const std::vector<std::int32_t>& map) {
    if (map.empty()) {
        return std::string{nullText};
    }
    top(joined("static const int32_t ", symbol, "[] = {"));
    for (const std::int32_t index : map) {
        top(joined("    ", decimal(index), ","));
    }
    top("};");
    top("");
    return std::string{symbol};
}

std::string CEmitter::emitAmendLatches(std::string_view symbol, const AmendPlan& plan) {
    if (plan.seeds.empty()) {
        return std::string{nullText};
    }
    top(joined("static const GeasAmendLatch ", symbol, "[] = {"));
    for (const AmendSeed& seed : plan.seeds) {
        top(joined("    { ", decimal(seed.item), ", ", seedStateName(seed.state), " },"));
    }
    top("};");
    top("");
    return std::string{symbol};
}

// The vows function of one block: the counts it was built against, then
// each vow item in source order, its value lowered and deep copied into this
// version's slot. Every vow of this version is in scope under its own name
// as its slot, already holding the carried or default value
std::string CEmitter::emitAmendVows(const ContractShape& shape, const AmendsDecl& amends,
                                    const AmendPlan& plan, std::string_view symbol) {
    beginBody(TyId{}, amends.lo, amends.hi);
    amend_ = &plan;
    pushScope();
    for (std::size_t i = 0; i < shape.vows.size(); ++i) {
        declare(shape.vows[i]->name, joined("vows[", countOf(i), "]"),
                lookup_.vowType(*shape.vows[i]));
    }
    bool hasItem = false;
    for (const AmendItem& item : amends.items) {
        if (const auto* vow = std::get_if<AmendVow>(&item)) {
            genAmendVow(shape, *vow);
            hasItem = true;
        }
    }
    popScope();
    amend_ = nullptr;
    if (!hasItem) {
        body_.clear();
        return std::string{nullText};
    }
    emitAmendVowsFrame(shape, plan, symbol);
    out_.append(body_);
    body_.clear();
    top("    return GEAS_OK;");
    top("}");
    top("");
    return std::string{symbol};
}

void CEmitter::emitAmendVowsFrame(const ContractShape& shape, const AmendPlan& plan,
                                  std::string_view symbol) {
    const std::string head = joined("static GeasStatus ", symbol, "(");
    top(joined(head, "void* ctx, const GeasValue* prior_vows, size_t nprior,"));
    top(joined(std::string(head.size(), ' '), "GeasValue* vows, size_t nvows) {"));
    top("    GeasContract* cc = (GeasContract*)ctx;");
    top("    (void)cc;");
    top("    (void)prior_vows;");
    top(joined("    if (nprior != ", countOf(plan.from.vows.size()), " || nvows != ",
               countOf(shape.vows.size()), ") return GEAS_ERR_TYPE;"));
}

// One vow item: the value, then an instance owned copy in the vow's slot
void CEmitter::genAmendVow(const ContractShape& shape, const AmendVow& item) {
    const std::int32_t index = vowIndex(shape, item.name);
    if (index < 0) {
        return;
    }
    const VowDecl& vow = *shape.vows[static_cast<std::size_t>(index)];
    const CValue value = genExpr(item.value, lookup_.vowType(vow));
    const std::string status = freshName();
    line(joined("GeasStatus ", status, " = geas_value_deep_copy(cc, &", value.name, ", &vows[",
                decimal(index), "]);"));
    returnUnlessOk(status);
}

// prior.NAME inside a vows function, a copy of the slot of N's vow
std::optional<CValue> CEmitter::genPriorRead(const Expr& expr, const MemberExpr& member) {
    if (amend_ == nullptr) {
        return std::nullopt;
    }
    const auto index = static_cast<std::size_t>(&expr - ast().exprs.data());
    if (lookup_.tables().exprUses[index] != ExprUse::PriorVow) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < amend_->from.vows.size(); ++i) {
        if (amend_->from.vows[i].name != member.name) {
            continue;
        }
        const std::optional<TyId> type = typeFromCode(types(), amend_->from.vows[i].type);
        std::string value = freshName();
        line(joined("GeasValue ", value, " = prior_vows[", countOf(i), "];"));
        return CValue{std::move(value), type.value_or(unknown())};
    }
    return std::nullopt;
}

// The GeasAmendDesc of one block, its tables emitted first
std::string CEmitter::emitAmendEntry(const ContractShape& shape, const AmendsDecl& amends,
                                     const AmendPlan& plan) {
    const std::string stem =
        joined(contractStem(shape.decl->name), "_amend", decimal(amends.version));
    const RecordedShape& from = plan.from;
    const std::string vowsFn = emitAmendVows(shape, amends, plan, joined(stem, "_vows"));
    const std::string vows = emitAmendNames(joined(stem, "_old_vows"), vowNames(from));
    const std::string items = emitAmendNames(joined(stem, "_old_items"), itemNames(from));
    const std::string subs = emitAmendNames(joined(stem, "_old_subs"), from.subs);
    const std::string remedies = emitAmendNames(joined(stem, "_old_remedies"), remedyNames(from));
    const std::string vowMap = emitAmendMap(joined(stem, "_vow_map"), plan.vowMap);
    const std::string itemMap = emitAmendMap(joined(stem, "_item_map"), plan.itemMap);
    const std::string subMap = emitAmendMap(joined(stem, "_sub_map"), plan.subMap);
    const std::string remedyMap = emitAmendMap(joined(stem, "_remedy_map"), plan.remedyMap);
    const std::string latches = emitAmendLatches(joined(stem, "_latches"), plan);
    return joined("    {\n        .from_version = ", decimal(from.version),
                  ", .restart_course = ", plan.isCourseRestarted ? "1" : "0",
                  ",\n        .from_shape_hash = 0x", hashHex16(from.shapeHash),
                  "ULL, .from_course_hash = 0x", hashHex16(from.courseHash),
                  "ULL,\n        .nold_vows = ", countOf(from.vows.size()), ", .old_vows = ", vows,
                  ",\n        .nold_items = ", countOf(from.items.size()), ", .old_items = ", items,
                  ",\n        .nold_subs = ", countOf(from.subs.size()), ", .old_subs = ", subs,
                  ",\n        .nold_remedies = ", countOf(from.remedies.size()),
                  ", .old_remedies = ", remedies, ",\n        .vow_map = ", vowMap,
                  ", .item_map = ", itemMap, ",\n        .sub_map = ", subMap,
                  ", .remedy_map = ", remedyMap, ",\n        .nlatches = ",
                  countOf(plan.seeds.size()), ", .latches = ", latches,
                  ",\n        .vows_fn = ", vowsFn, ",\n    },");
}

/**
 * Emits the amends table of one contract and every table it points at.
 * @params shape - the contract
 * @returns {std::string} - the descriptor line naming the table, empty when the contract has no
 * amends block
 */
std::string CEmitter::emitAmends(const ContractShape& shape) {
    std::vector<std::string> entries;
    for (const Member& member : shape.decl->members) {
        const auto* amends = std::get_if<AmendsDecl>(&member);
        if (amends == nullptr) {
            continue;
        }
        const auto plan = lookup_.tables().amends.find(amends);
        if (plan != lookup_.tables().amends.end()) {
            entries.push_back(emitAmendEntry(shape, *amends, plan->second));
        }
    }
    if (entries.empty()) {
        return "";
    }
    const std::string symbol = joined(contractStem(shape.decl->name), "_amends");
    top(joined("static const GeasAmendDesc ", symbol, "[] = {"));
    for (const std::string& entry : entries) {
        top(entry);
    }
    top("};");
    top("");
    return joined("    .namends = ", countOf(entries.size()), ", .amends = ", symbol, ",");
}

}  // namespace geas
