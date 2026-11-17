// Attributes, transactions, and remedies. The term attribute is a
// contract's total lifetime and belongs nowhere else. A transactional
// subcontract holds no signal and no pledge with a retry. A remedy undoes
// one pledge of its own contract, takes that pledge's Ok value or nothing,
// and answers Result<Unit, E> or Unit
#include "checker.hpp"

namespace geas {

namespace {

constexpr std::string_view termKey = "term";

}  // namespace

// The term attribute, a duration on a contract, a lifetime longer than nothing
void Checker::checkContractAttrs(const ContractDecl& contract) {
    for (const Attr& attr : contract.attrs) {
        if (attr.key != termKey) {
            continue;
        }
        const Expr& value = ast_.expr(attr.value);
        const auto* duration = std::get_if<DurationLiteral>(&value.kind);
        if (duration == nullptr || duration->milliseconds <= 0) {
            emitAt(value, "the term attribute must be a duration greater than zero");
        }
    }
}

// A transactional episode is all or nothing under one open transaction, so
// a retry, which would run a failed attempt's writes and the next one's
// inside it with the instance lock let go between them, and a signal, which
// arrives from outside any transaction, both stay out of one
void Checker::refuseInTransaction(const SubcontractDecl& sub, const SubMember& member,
                                  const std::string& label) {
    if (!sub.isTransactional) {
        return;
    }
    const std::string where = sub.name ? "the transactional subcontract " + quoted(*sub.name)
                                       : "a transactional subcontract";
    if (const auto* signal = std::get_if<SignalDecl>(&member)) {
        emit(signal->lo, signal->hi,
             "signal " + quoted(signal->name) + " cannot sit in " + where +
                 "; a signal arrives from outside, never inside an open transaction");
        return;
    }
    for (const Term& term : std::get<PledgeDecl>(member).terms) {
        if (std::holds_alternative<RetryTerm>(term.kind)) {
            emit(term.lo, term.hi,
                 label + " sits in " + where +
                     " and cannot carry a retry; a transactional episode is all or nothing");
        }
    }
}

void Checker::checkRemedies(const ContractDecl& contract) {
    std::unordered_map<const PledgeDecl*, const RemedyDecl*> taken;
    for (const Member& member : contract.members) {
        if (const auto* remedy = std::get_if<RemedyDecl>(&member)) {
            checkRemedy(contract, *remedy, taken);
        }
    }
}

// One remedy: what it returns, the pledge it targets, and that no other
// remedy already undoes that pledge
void Checker::checkRemedy(const ContractDecl& contract, const RemedyDecl& remedy,
                          std::unordered_map<const PledgeDecl*, const RemedyDecl*>& taken) {
    const std::string label = "remedy " + quoted(remedy.name);
    const auto fault = [&](std::string message) { emit(remedy.lo, remedy.hi, std::move(message)); };
    const TyNode ret = types_.node(remedySig(remedy).ret);
    const bool isUnitResult =
        ret.kind == TyKind::Result &&
        (kindOf(ret.args[0]) == TyKind::Unit || kindOf(ret.args[0]) == TyKind::Unknown);
    if (ret.kind != TyKind::Unit && ret.kind != TyKind::Unknown && !isUnitResult) {
        fault(label + " must return Result<Unit, E> or Unit");
    }
    if (remedy.target.empty()) {
        return;
    }
    const MemberEntry* entry = resolved_.reachable(&contract, remedy.target);
    if (entry == nullptr) {
        return fault(label + " names an unknown pledge " + quoted(remedy.target));
    }
    if (entry->kind() != MemberKind::Pledge) {
        return fault(label + " targets " + quoted(remedy.target) + ", which is a " +
                     std::string{memberKindWord(entry->kind())} + ", not a pledge");
    }
    const PledgeDecl* target = entry->pledge();
    tables_.remedyTargets.emplace(&remedy, target);
    const auto [slot, isFirst] = taken.emplace(target, &remedy);
    if (!isFirst) {
        fault("pledge " + quoted(remedy.target) + " already has the remedy " +
              quoted(slot->second->name) + ", " + quoted(remedy.name) + " is a second one");
    }
    checkRemedyParam(remedy, *target);
}

// The parameter receives the pledge's Ok value, so its type is the T of the
// target's Result<T, E>, or Bytes for a bare buffer return. Unit, an Option, or a
// Result of Unit, leaves the remedy no parameter to take
void Checker::checkRemedyParam(const RemedyDecl& remedy, const PledgeDecl& target) {
    if (!remedy.param) {
        return;
    }
    const std::string label = "remedy " + quoted(remedy.name);
    const TyNode yielded = types_.node(pledgeSig(target).ret);
    if (yielded.kind == TyKind::Unknown) {
        return;
    }
    const bool hasValue = yielded.kind == TyKind::Bytes ||
        (yielded.kind == TyKind::Result && kindOf(yielded.args[0]) != TyKind::Unit);
    if (!hasValue) {
        emit(remedy.lo, remedy.hi,
             label + " cannot take a parameter; pledge " + quoted(target.name) +
                 " yields no Ok value");
        return;
    }
    const TyId payload = yielded.kind == TyKind::Bytes ? scalar(TyKind::Bytes) : yielded.args[0];
    const std::vector<TyId>& params = remedySig(remedy).params;
    if (params.empty() || isCompatible(payload, params.front())) {
        return;
    }
    emit(remedy.lo, remedy.hi,
         typesMessage("the parameter of " + label + " has type ", params.front(),
                      " but pledge " + quoted(target.name) + " yields ", payload));
}

// A remedy with a body checks like a pledge body, one with none is abstract
// and bound the way an abstract pledge is
void Checker::checkRemedyBody(const RemedyDecl& remedy) {
    if (!remedy.body) {
        return;
    }
    std::vector<Field> params;
    if (remedy.param) {
        params.push_back(*remedy.param);
    }
    const CallableSig& sig = remedySig(remedy);
    const TyId ret = sig.ret.isSet() ? sig.ret : unknown();
    checkBody(params, sig.params, ret, *remedy.body, remedy.lo, remedy.hi);
}

}  // namespace geas
