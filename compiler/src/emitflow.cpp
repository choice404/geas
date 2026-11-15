// The workflow tail of one contract's descriptor: the kind bytes that mark a
// signal's row of the pledges table, the terms of every pledge, signal, and
// subcontract with the after and retry on arrays they point at, the remedy
// table, and the contract's own term. A contract that declares none of the
// workflow surface emits none of it and its descriptor stops at sub_flags,
// so the C it compiles to is the C it always compiled to.
//
// Signals ride the pledges table after every pledge, in declaration order,
// so a pledge keeps the index it always had and a signal's index is the
// pledge count plus its own ordinal
#include "emitc.hpp"
#include "emittext.hpp"

namespace geas {

namespace {

std::string countOf(std::size_t count) {
    return decimal(static_cast<std::int64_t>(count));
}

// A float spelling as C reads it, the underscores the lexer allows dropped
std::string floatText(const std::string& spelling) {
    std::string out;
    for (const char c : spelling) {
        if (c != '_') {
            out.push_back(c);
        }
    }
    return out;
}

// The three terms one declaration may trail, picked out of its list
struct TermPick {
    const WithinTerm* within = nullptr;
    const Term* after = nullptr;
    const RetryTerm* retry = nullptr;
};

TermPick pickTerms(const std::vector<Term>& terms) {
    TermPick pick;
    for (const Term& term : terms) {
        if (const auto* within = std::get_if<WithinTerm>(&term.kind)) {
            pick.within = within;
        } else if (const auto* retry = std::get_if<RetryTerm>(&term.kind)) {
            pick.retry = retry;
        } else {
            pick.after = &term;
        }
    }
    return pick;
}

}  // namespace

bool hasWorkflowTail(const ContractShape& shape) {
    if (!shape.signals.empty() || !shape.remedies.empty() || shape.termMilliseconds) {
        return true;
    }
    for (const PledgeShape& pledge : shape.pledges) {
        if (!pledge.decl->terms.empty()) {
            return true;
        }
    }
    for (const SubcontractDecl* sub : shape.subs) {
        if (!sub->terms.empty()) {
            return true;
        }
    }
    return false;
}

// The pledges table index of an after target: a pledge by its row, a signal
// after every pledge, a subcontract by its ordinal in the subs table
std::string CEmitter::itemRefText(const ContractShape& shape, const OrderTarget& target) const {
    if (const auto* sub = std::get_if<const SubcontractDecl*>(&target)) {
        for (std::size_t i = 0; i < shape.subs.size(); ++i) {
            if (shape.subs[i] == *sub) {
                return joined("{ GEAS_REF_SUB, ", countOf(i), " }");
            }
        }
    }
    if (const auto* pledge = std::get_if<const PledgeDecl*>(&target)) {
        for (std::size_t i = 0; i < shape.pledges.size(); ++i) {
            if (shape.pledges[i].decl == *pledge) {
                return joined("{ GEAS_REF_PLEDGE, ", countOf(i), " }");
            }
        }
    }
    if (const auto* signal = std::get_if<const SignalDecl*>(&target)) {
        for (std::size_t i = 0; i < shape.signals.size(); ++i) {
            if (shape.signals[i].decl == *signal) {
                return joined("{ GEAS_REF_PLEDGE, ", countOf(shape.pledges.size() + i), " }");
            }
        }
    }
    return "";
}

// Emits the after array of one term set and returns its symbol, NULL for a
// set that waits on nothing
std::string CEmitter::emitAfterArray(const ContractShape& shape, const Term* after,
                                     std::string_view symbol, std::size_t& count) {
    count = 0;
    const auto targets = after == nullptr ? lookup_.tables().afterTargets.end()
                                          : lookup_.tables().afterTargets.find(after);
    if (targets == lookup_.tables().afterTargets.end() || targets->second.empty()) {
        return "NULL";
    }
    top(joined("static const GeasItemRef ", symbol, "[] = {"));
    for (const OrderTarget& target : targets->second) {
        const std::string ref = itemRefText(shape, target);
        if (ref.empty()) {
            lookup_.fault(after->lo, after->hi,
                          "codegen: an after target did not resolve to a descriptor index");
            continue;
        }
        top(joined("    ", ref, ","));
        ++count;
    }
    top("};");
    top("");
    return std::string{symbol};
}

// Emits the retry on array of one retry term, each listed variant as its
// declaration index in the Err sum, and returns its symbol
std::string CEmitter::emitRetryOnArray(const RetryTerm& retry, std::string_view symbol,
                                       std::int64_t lo, std::int64_t hi) {
    if (retry.on.empty()) {
        return "NULL";
    }
    top(joined("static const uint32_t ", symbol, "[] = {"));
    for (const std::string& name : retry.on) {
        const std::optional<VariantSlot> slot = lookup_.variantOf(name);
        if (!slot) {
            lookup_.fault(lo, hi, joined("codegen: the retry on variant '", name,
                                         "' has no declaration index"));
            continue;
        }
        top(joined("    ", decimal(slot->index), ","));
    }
    top("};");
    top("");
    return std::string{symbol};
}

// The retry fields of one GeasTermDesc, the on array emitted first
void CEmitter::appendRetryFields(const RetryTerm& retry, std::string_view onSymbol,
                                 std::int64_t lo, std::int64_t hi,
                                 std::vector<std::string>& fields) {
    fields.push_back(joined(".retry_count = ", decimal(retry.count)));
    if (retry.everyMilliseconds) {
        fields.push_back(joined(".retry_every_ms = ", decimal(*retry.everyMilliseconds)));
    }
    if (retry.backoff) {
        fields.push_back(joined(".retry_backoff = ", floatText(*retry.backoff)));
    }
    if (retry.capMilliseconds) {
        fields.push_back(joined(".retry_cap_ms = ", decimal(*retry.capMilliseconds)));
    }
    const std::string on = emitRetryOnArray(retry, onSymbol, lo, hi);
    if (!retry.on.empty()) {
        fields.push_back(
            joined(".nretry_on = ", countOf(retry.on.size()), ", .retry_on = ", on));
    }
}

/**
 * Emits the arrays one declaration's terms point at and spells its
 * GeasTermDesc initializer.
 * @params shape - the contract, terms - the declaration's terms, stem - the suffix that names its
 * arrays, lo - the span start, hi - the span end
 * @returns {std::string} - the initializer, { 0 } for a declaration with no terms
 */
std::string CEmitter::termEntry(const ContractShape& shape, const std::vector<Term>& terms,
                                std::string_view stem, std::int64_t lo, std::int64_t hi) {
    const TermPick pick = pickTerms(terms);
    const std::string& name = shape.decl->name;
    std::vector<std::string> fields;
    if (pick.within != nullptr) {
        fields.push_back(joined(".within_ms = ", decimal(pick.within->milliseconds)));
    }
    std::size_t afterCount = 0;
    const std::string after =
        emitAfterArray(shape, pick.after, joined("k_", name, "_after_", stem), afterCount);
    if (afterCount > 0) {
        fields.push_back(joined(".nafter = ", countOf(afterCount), ", .after = ", after));
    }
    if (pick.retry != nullptr) {
        appendRetryFields(*pick.retry, joined("k_", name, "_retry_on_", stem), lo, hi, fields);
    }
    if (fields.empty()) {
        return "{ 0 }";
    }
    std::string out = "{ ";
    for (std::size_t i = 0; i < fields.size(); ++i) {
        out.append(i > 0 ? ", " : "");
        out.append(fields[i]);
    }
    return out + " }";
}

// The kind byte per pledges table row, NULL when no row is a signal
std::string CEmitter::emitKinds(const ContractShape& shape) {
    if (shape.signals.empty()) {
        return "NULL";
    }
    top(joined("static const uint8_t k_", shape.decl->name, "_kinds[] = {"));
    for (std::size_t i = 0; i < shape.pledges.size(); ++i) {
        top("    GEAS_KIND_PLEDGE,");
    }
    for (std::size_t i = 0; i < shape.signals.size(); ++i) {
        top("    GEAS_KIND_SIGNAL,");
    }
    top("};");
    top("");
    return joined("k_", shape.decl->name, "_kinds");
}

// The terms per pledges table row, NULL when no pledge or signal has one.
// The arrays every entry points at land first, then the full length table
std::string CEmitter::emitPledgeTerms(const ContractShape& shape) {
    bool hasTerms = false;
    for (const PledgeShape& pledge : shape.pledges) {
        hasTerms = hasTerms || !pledge.decl->terms.empty();
    }
    for (const SignalShape& signal : shape.signals) {
        hasTerms = hasTerms || !signal.decl->terms.empty();
    }
    if (!hasTerms) {
        return "NULL";
    }
    std::vector<std::string> entries;
    for (std::size_t i = 0; i < shape.pledges.size(); ++i) {
        const PledgeDecl& pledge = *shape.pledges[i].decl;
        entries.push_back(termEntry(shape, pledge.terms, countOf(i), pledge.lo, pledge.hi));
    }
    for (std::size_t i = 0; i < shape.signals.size(); ++i) {
        const SignalDecl& signal = *shape.signals[i].decl;
        entries.push_back(termEntry(shape, signal.terms, countOf(shape.pledges.size() + i),
                                    signal.lo, signal.hi));
    }
    top(joined("static const GeasTermDesc k_", shape.decl->name, "_terms[] = {"));
    for (const std::string& entry : entries) {
        top(joined("    ", entry, ","));
    }
    top("};");
    top("");
    return joined("k_", shape.decl->name, "_terms");
}

// The terms per subcontract, NULL when none carries one
std::string CEmitter::emitSubTerms(const ContractShape& shape) {
    bool hasTerms = false;
    for (const SubcontractDecl* sub : shape.subs) {
        hasTerms = hasTerms || !sub->terms.empty();
    }
    if (!hasTerms) {
        return "NULL";
    }
    std::vector<std::string> entries;
    for (std::size_t i = 0; i < shape.subs.size(); ++i) {
        const SubcontractDecl& sub = *shape.subs[i];
        entries.push_back(termEntry(shape, sub.terms, joined("sub_", countOf(i)), sub.lo, sub.hi));
    }
    top(joined("static const GeasTermDesc k_", shape.decl->name, "_sub_terms[] = {"));
    for (const std::string& entry : entries) {
        top(joined("    ", entry, ","));
    }
    top("};");
    top("");
    return joined("k_", shape.decl->name, "_sub_terms");
}

}  // namespace geas
