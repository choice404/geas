// The call expression. The lowered callables are the len builtin, a sum
// variant constructor, a bare clause of the enclosing contract, sign and
// resume on a contract name, the Store namespace, and the pledge, break,
// status, park, and partial operations on a signed instance, and a signal
// delivered through one. An instance value is the internal GEAS_TY_INSTANCE
// handle in as.box, the one exception to value semantics: a let or a bind
// copies the handle, not the contract.
// A call through a first class pledge value stays a named build error
#include "emitc.hpp"
#include "emittext.hpp"

namespace geas {

namespace {

std::string countOf(std::size_t count) {
    return decimal(static_cast<std::int64_t>(count));
}

}  // namespace

CValue CEmitter::genCall(const Expr& expr, const CallExpr& call) {
    const Expr& callee = ast().expr(call.callee);
    if (const auto* identifier = std::get_if<Identifier>(&callee.kind)) {
        if (std::optional<CValue> value = genNamedCall(expr, call, *identifier)) {
            return *value;
        }
    }
    if (const auto* member = std::get_if<MemberExpr>(&callee.kind)) {
        if (std::optional<CValue> value = genStaticCall(expr, call, *member)) {
            return *value;
        }
        const CValue base = genExpr(member->base, TyId{});
        if (kindOf(base.type) == TyKind::Contract) {
            return genInstanceCall(expr, call, *member, base);
        }
    }
    unsupportedAt(expr, "a call in a pledge body");
    return badValue();
}

// A call through a bare name no local shadows: len, a clause of the
// enclosing contract, or a variant constructor
std::optional<CValue> CEmitter::genNamedCall(const Expr& expr, const CallExpr& call,
                                             const Identifier& callee) {
    if (lookupLocal(callee.name) != nullptr) {
        return std::nullopt;
    }
    if (callee.name == "len") {
        return genLen(call);
    }
    const MemberEntry* member = lookup_.resolved().member(shape_->decl, callee.name);
    if (member != nullptr && member->kind() == MemberKind::Clause) {
        return genClauseCall(*member->clause(), call);
    }
    const GlobalEntry* global = lookup_.resolved().global(callee.name);
    if (global != nullptr && global->kind == GlobalKind::Variant) {
        return genVariant(expr, callee.name, call.args);
    }
    return std::nullopt;
}

// A call on a bare name that is not a value: sign or resume on a contract
// name, or an operation of the Store namespace
std::optional<CValue> CEmitter::genStaticCall(const Expr& expr, const CallExpr& call,
                                              const MemberExpr& callee) {
    const auto* base = std::get_if<Identifier>(&ast().expr(callee.base).kind);
    if (base == nullptr || lookupLocal(base->name) != nullptr) {
        return std::nullopt;
    }
    const GlobalEntry* global = lookup_.resolved().global(base->name);
    if (global == nullptr || global->kind != GlobalKind::Contract) {
        if (base->name == "Store") {
            return genStoreOp(expr, call, callee.name);
        }
        return std::nullopt;
    }
    const ContractDecl* contract = contractAt(ast(), global->declIndex);
    if (contract != nullptr && callee.name == "sign") {
        return genSign(expr, call, *contract);
    }
    if (contract != nullptr && callee.name == "resume") {
        return genResume(expr, call, *contract);
    }
    unsupportedAt(expr, "this call on a contract name in a pledge body");
    return badValue();
}

CValue CEmitter::genInstanceCall(const Expr& expr, const CallExpr& call, const MemberExpr& callee,
                                 const CValue& base) {
    if (callee.name == "break") {
        return genBreakCall(base);
    }
    if (callee.name == "status") {
        return genStatusCall(base);
    }
    if (callee.name == "park") {
        return genParkCall(call, base);
    }
    if (callee.name == "partial") {
        return genPartialCall(base);
    }
    const ContractDecl* contract = lookup_.contractOf(base.type);
    if (const SignalDecl* signal = lookup_.resolved().findSignal(contract, callee.name)) {
        return genSignalCall(call, *signal, base);
    }
    return genPledgeCall(expr, call, callee, base);
}

// Lowers a positional argument list against the checked parameter types of
// the callee, one value per argument, in source order
std::vector<CValue> CEmitter::genArgs(const std::vector<TyId>& params,
                                      const std::vector<Argument>& args) {
    std::vector<CValue> values;
    for (std::size_t i = 0; i < args.size(); ++i) {
        values.push_back(genExpr(args[i].value, i < params.size() ? params[i] : TyId{}));
    }
    return values;
}

// Gathers lowered argument values into one stack GeasValue array and
// returns its name, or NULL for an empty list
std::string CEmitter::genValueArray(const std::vector<CValue>& values) {
    if (values.empty()) {
        return "NULL";
    }
    std::string array = freshName();
    std::string text = joined("GeasValue ", array, "[", countOf(values.size()), "] = { ");
    for (std::size_t i = 0; i < values.size(); ++i) {
        text.append(i > 0 ? ", " : "");
        text.append(values[i].name);
    }
    text.append(" };");
    line(text);
    return array;
}

// len(x): the length of a List or a Map read out as an Int. A List answers
// its element count straight off the header; a Map answers half the slot
// count of the flat [k0,v0,...] store the runtime keeps
CValue CEmitter::genLen(const CallExpr& call) {
    const CValue subject = genExpr(call.args.front().value, TyId{});
    const bool isMap = kindOf(subject.type) == TyKind::Map;
    std::string value = newValue();
    setTag(value, "GEAS_TY_INT");
    line(isMap ? joined(value, ".as.i = (int64_t)(", subject.name, ".as.list.len / 2);")
               : joined(value, ".as.i = (int64_t)", subject.name, ".as.list.len;"));
    return CValue{std::move(value), types().scalar(TyKind::Int)};
}

// A bare clause call. The arguments land in one stack array and the static
// clause function runs in the thunk's exit convention: a status that is not
// GEAS_OK exits the enclosing body with it and the value arrives through
// the out slot
CValue CEmitter::genClauseCall(const ClauseDecl& clause, const CallExpr& call) {
    const CallableSig sig = lookup_.clauseSig(clause);
    const std::vector<CValue> values = genArgs(sig.params, call.args);
    const std::string array = genValueArray(values);
    std::string result = newValue();
    const std::string status = freshName();
    line(joined("GeasStatus ", status, " = ", clauseFunctionName(shape_->decl->name, clause.name),
                "(cc, ", array, ", &", result, ");"));
    returnUnlessOk(status);
    return CValue{std::move(result), sig.ret.isSet() ? sig.ret : unknown()};
}

// The named vow overrides of a sign call as one GeasVowBinding array, each
// value lowered against its vow's checked type. Returns the array's name,
// NULL when the call overrides nothing
std::string CEmitter::genVowBindings(const CallExpr& call, const ContractDecl& contract) {
    if (call.args.empty()) {
        return "NULL";
    }
    std::string array = freshName();
    line(joined("GeasVowBinding ", array, "[", countOf(call.args.size()), "];"));
    for (std::size_t i = 0; i < call.args.size(); ++i) {
        const std::string label = call.args[i].label.value_or("");
        const MemberEntry* member = lookup_.resolved().member(&contract, label);
        const bool isVow = member != nullptr && member->kind() == MemberKind::Vow;
        const CValue value =
            genExpr(call.args[i].value, isVow ? lookup_.vowType(*member->vow()) : TyId{});
        const std::string slot = joined(array, "[", countOf(i), "]");
        line(joined(slot, ".name = \"", label, "\";"));
        line(joined(slot, ".value = ", value.name, ";"));
    }
    return array;
}

// The instance handle a sign or a resume answers with
CValue CEmitter::genInstanceHandle(std::string_view instance, const ContractDecl& contract) {
    std::string value = newValue();
    setTag(value, "GEAS_TY_INSTANCE");
    line(joined(value, ".as.box = ", instance, ";"));
    return CValue{std::move(value), types().contract(contract.name)};
}

// Contract.sign(named vow overrides). The sign runs against the runtime the
// enclosing instance rides. The expected hash is 0 on purpose: the callee's
// descriptor was emitted from this same checked tree, so the name alone
// identifies it. An internal contract emits no descriptor and cannot be
// signed by name, so signing one from a body is a build error
CValue CEmitter::genSign(const Expr& expr, const CallExpr& call, const ContractDecl& contract) {
    if (contract.isInternal) {
        unsupportedAt(
            expr, joined("signing the internal contract '", contract.name, "' in a pledge body"));
        return badValue();
    }
    const std::string bindings = genVowBindings(call, contract);
    const std::string instance = freshName();
    line(joined("GeasContract* ", instance, " = NULL;"));
    const std::string status = freshName();
    line(joined("GeasStatus ", status, " = geas_contract_sign(geas_instance_runtime(cc), \"",
                contract.name, "\", ", bindings, ", ", countOf(call.args.size()), ", 0, &",
                instance, ");"));
    returnUnlessOk(status);
    return genInstanceHandle(instance, contract);
}

// Contract.resume(dsn, key): the parked row stood back up on this runtime,
// the resumed instance the value the way sign's instance is
CValue CEmitter::genResume(const Expr& expr, const CallExpr& call, const ContractDecl& contract) {
    if (contract.isInternal) {
        unsupportedAt(
            expr, joined("resuming the internal contract '", contract.name, "' in a pledge body"));
        return badValue();
    }
    const TyId text = types().scalar(TyKind::String);
    const CValue dsn = genExpr(call.args[0].value, text);
    const CValue key = genExpr(call.args[1].value, text);
    const std::string instance = freshName();
    line(joined("GeasContract* ", instance, " = NULL;"));
    const std::string status = freshName();
    line(joined("GeasStatus ", status, " = geas_instance_resume_v(geas_instance_runtime(cc), &",
                dsn.name, ", &", key.name, ", 0, &", instance, ");"));
    returnUnlessOk(status);
    return genInstanceHandle(instance, contract);
}

// instance.pledge(args), the cross contract fulfillment. The arguments ride
// one synchronous fulfill against the callee instance, and the value, which
// the callee instance owns, is deep copied onto the enclosing instance at
// once, so it outlives the callee's break
CValue CEmitter::genPledgeCall(const Expr& expr, const CallExpr& call, const MemberExpr& callee,
                               const CValue& base) {
    const ContractDecl* contract = lookup_.contractOf(base.type);
    const PledgeDecl* pledge = lookup_.resolved().findPledge(contract, callee.name);
    if (contract == nullptr || pledge == nullptr) {
        unsupportedAt(expr,
                      joined("the call '", callee.name, "' on this instance in a pledge body"));
        return badValue();
    }
    const CallableSig sig = lookup_.pledgeSig(*pledge);
    const std::vector<CValue> values = genArgs(sig.params, call.args);
    const std::string array = genValueArray(values);
    const std::string result = newValue();
    const std::string status = freshName();
    line(joined("GeasStatus ", status, " = geas_pledge_fulfill_sync((GeasContract*)", base.name,
                ".as.box, \"", callee.name, "\", ", array, ", ", countOf(values.size()),
                ", NULL, 0, &", result, ");"));
    returnUnlessOk(status);
    std::string home = newValue();
    const std::string copyStatus = freshName();
    line(joined("GeasStatus ", copyStatus, " = geas_value_deep_copy(cc, &", result, ", &", home,
                ");"));
    returnUnlessOk(copyStatus);
    return CValue{std::move(home), sig.ret.isSet() ? sig.ret : unknown()};
}

// instance.break(). The value is Unit, the zeroed fresh temporary
CValue CEmitter::genBreakCall(const CValue& base) {
    const std::string status = freshName();
    line(joined("GeasStatus ", status, " = geas_contract_break((GeasContract*)", base.name,
                ".as.box);"));
    returnUnlessOk(status);
    return CValue{newValue(), types().scalar(TyKind::Unit)};
}

// instance.status(). The runtime hands the state's canonical spelling as a
// String borrowing static bytes, so nothing allocates and nothing frees
CValue CEmitter::genStatusCall(const CValue& base) {
    std::string value = freshName();
    line(joined("GeasValue ", value, " = geas_state_value((const GeasContract*)", base.name,
                ".as.box);"));
    return CValue{std::move(value), types().scalar(TyKind::String)};
}

// instance.park(dsn, key). The two String values ride the value argument
// spelling of park and the answer is Unit
CValue CEmitter::genParkCall(const CallExpr& call, const CValue& base) {
    const TyId text = types().scalar(TyKind::String);
    const CValue dsn = genExpr(call.args[0].value, text);
    const CValue key = genExpr(call.args[1].value, text);
    const std::string status = freshName();
    line(joined("GeasStatus ", status, " = geas_instance_park_v((GeasContract*)", base.name,
                ".as.box, &", dsn.name, ", &", key.name, ");"));
    returnUnlessOk(status);
    return CValue{newValue(), types().scalar(TyKind::Unit)};
}

// instance.partial(). The runtime folds the instance's requirements into a
// PartialResult record allocated on the enclosing instance
CValue CEmitter::genPartialCall(const CValue& base) {
    std::string value = newValue();
    const std::string status = freshName();
    line(joined("GeasStatus ", status, " = geas_partial_value((GeasContract*)", base.name,
                ".as.box, cc, &", value, ");"));
    returnUnlessOk(status);
    return CValue{std::move(value), types().named("PartialResult")};
}

}  // namespace geas
