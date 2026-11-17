// Calls. A call checks its arguments against exactly one signature: a
// clause called by bare name inside its own contract, a sum variant
// constructor, a first class pledge value, or, inside a course, one of the
// contract's own pledges by bare name. Calls on a contract name or a signed
// instance live in checkinstance.cpp, Store calls in checkstore.cpp.
// Everything else is not callable and says so
#include "checker.hpp"

namespace geas {

namespace {

std::string arityFault(std::string_view head, std::size_t takes, std::size_t got) {
    return std::string{head} + countText(static_cast<std::int64_t>(takes)) + " arguments, got " +
           countText(static_cast<std::int64_t>(got));
}

}  // namespace

// Infers every argument with no expectation, the recovery walk after the
// callee itself already faulted
void Checker::walkArgs(const std::vector<Argument>& args) {
    for (const Argument& arg : args) {
        inferExpr(arg.value, TyId{});
    }
}

// Positional arguments against a lowered parameter list. Labels belong to
// sign alone, the count must match, and every argument type must be exact
void Checker::checkArgs(const std::vector<TyId>& params, const CallExpr& call, const Expr& expr,
                        std::string_view word, std::string_view name) {
    if (call.args.size() != params.size()) {
        const std::string head = std::string{word} + " " + quoted(name) + " takes ";
        emitAt(expr, arityFault(head, params.size(), call.args.size()));
    }
    for (std::size_t i = 0; i < call.args.size(); ++i) {
        const Argument& arg = call.args[i];
        const Expr& value = ast_.expr(arg.value);
        if (arg.label) {
            emitAt(value, std::string{labelFault});
        }
        const TyId wanted = i < params.size() ? params[i] : TyId{};
        const TyId actual = inferExpr(arg.value, wanted);
        if (wanted.isSet() && !isCompatible(wanted, actual)) {
            const std::string head = argumentHead(i, " of " + quoted(name) + " expects ");
            emitAt(value, typesMessage(head, wanted, ", got ", actual));
        }
    }
}

// Two positional String arguments, the shape park and resume share: the dsn
// naming the database and the key naming the row
void Checker::checkTwoStrings(const Expr& expr, const CallExpr& call, std::string_view name) {
    constexpr std::size_t expectedCount = 2;
    if (call.args.size() != expectedCount) {
        emitAt(expr, quoted(name) + " takes a dsn and a key, got " +
                         countText(static_cast<std::int64_t>(call.args.size())) + " arguments");
    }
    const TyId stringType = scalar(TyKind::String);
    for (std::size_t i = 0; i < call.args.size(); ++i) {
        const Argument& arg = call.args[i];
        const Expr& value = ast_.expr(arg.value);
        if (arg.label) {
            emitAt(value, std::string{labelFault});
        }
        const TyId actual = inferExpr(arg.value, stringType);
        if (!isCompatible(stringType, actual)) {
            const std::string head = argumentHead(i, " of " + quoted(name) + " expects ");
            emitAt(value, typesMessage(head, stringType, ", got ", actual));
        }
    }
}

// A call whose callee typed as a first class pledge value
TyId Checker::callPledgeValue(const Expr& expr, const CallExpr& call, TyId callee) {
    const TyNode node = types_.node(callee);
    if (call.args.size() != node.args.size()) {
        emitAt(expr, arityFault("the pledge value takes ", node.args.size(), call.args.size()));
    }
    for (std::size_t i = 0; i < call.args.size(); ++i) {
        const Argument& arg = call.args[i];
        const Expr& value = ast_.expr(arg.value);
        if (arg.label) {
            emitAt(value, std::string{labelFault});
        }
        const TyId wanted = i < node.args.size() ? node.args[i] : TyId{};
        const TyId actual = inferExpr(arg.value, wanted);
        if (wanted.isSet() && !isCompatible(wanted, actual)) {
            const std::string head = argumentHead(i, " of the pledge value expects ");
            emitAt(value, typesMessage(head, wanted, ", got ", actual));
        }
    }
    markUse(expr, ExprUse::PledgeValueCall);
    return node.ret;
}

// The builtin len(x), the length of a List, Map, or Bytes as an Int. It is reserved in callee
// position only, so a binding may still be named len
TyId Checker::checkLen(const Expr& expr, const CallExpr& call) {
    const TyId intType = scalar(TyKind::Int);
    markUse(expr, ExprUse::LenCall);
    if (call.args.size() != 1) {
        walkArgs(call.args);
        emitAt(expr, "len takes one value, got " +
                         countText(static_cast<std::int64_t>(call.args.size())));
        return intType;
    }
    const Argument& arg = call.args.front();
    const Expr& value = ast_.expr(arg.value);
    if (arg.label) {
        emitAt(value, std::string{labelFault});
    }
    const TyId actual = inferExpr(arg.value, TyId{});
    const TyKind kind = kindOf(actual);
    if (kind != TyKind::List && kind != TyKind::Map && kind != TyKind::Bytes &&
        kind != TyKind::Unknown) {
        emitAt(value, "len takes a List, Map, or Bytes, got " + quoted(types_.render(actual)));
    }
    return intType;
}

// Bytes conversions reserve a bare callee name and leave ordinary bindings alone
TyId Checker::checkBytesConversion(const Expr& expr, const CallExpr& call,
                                    std::string_view name) {
    const TyId bytesType = scalar(TyKind::Bytes);
    const TyId byteList = types_.list(scalar(TyKind::Byte));
    const TyId result = name == "bytes" ? bytesType
        : name == "text" ? types_.option(scalar(TyKind::String)) : byteList;
    if (call.args.size() != 1) {
        walkArgs(call.args);
        emitAt(expr, std::string{name} + " takes one value, got " +
                         countText(static_cast<std::int64_t>(call.args.size())));
        return result;
    }
    const Argument& arg = call.args.front();
    const Expr& value = ast_.expr(arg.value);
    if (arg.label) {
        emitAt(value, std::string{labelFault});
    }
    const TyId expected = name == "bytes" ? byteList : bytesType;
    const TyId actual = inferExpr(arg.value, expected);
    const bool isAccepted = name == "bytes"
        ? isCompatible(actual, byteList) || kindOf(actual) == TyKind::String
        : isCompatible(actual, bytesType);
    if (!isAccepted) {
        const std::string wanted = name == "bytes" ? "String or List<Byte>" : "Bytes";
        emitAt(value, std::string{name} + " takes " + wanted + ", got " +
                          quoted(types_.render(actual)));
    }
    return result;
}

// A sum variant constructed with its payload
TyId Checker::callVariant(const Expr& expr, const CallExpr& call, const std::string& name) {
    const VariantSig* variant = sigs_.variant(name);
    if (variant == nullptr) {
        walkArgs(call.args);
        return unknown();
    }
    const std::vector<FieldSig>& fields = variant->fields;
    if (call.args.size() != fields.size()) {
        emitAt(expr,
               arityFault("variant " + quoted(name) + " takes ", fields.size(), call.args.size()));
    }
    for (std::size_t i = 0; i < call.args.size(); ++i) {
        const Argument& arg = call.args[i];
        const Expr& value = ast_.expr(arg.value);
        const bool hasField = i < fields.size();
        if (arg.label && (!hasField || *arg.label != fields[i].name)) {
            emitAt(value, "the label " + quoted(*arg.label) +
                              " does not name this field of variant " + quoted(name));
        }
        const TyId wanted = hasField ? fields[i].type : TyId{};
        const TyId actual = inferExpr(arg.value, wanted);
        if (wanted.isSet() && !isCompatible(wanted, actual)) {
            const std::string head = argumentHead(i, " of variant " + quoted(name) + " expects ");
            emitAt(value, typesMessage(head, wanted, ", got ", actual));
        }
    }
    markUse(expr, ExprUse::VariantCall);
    return types_.named(variant->owner);
}

// A bare name called inside a contract that one of its members claims,
// nullopt when the call falls through to the globals
std::optional<TyId> Checker::callMemberName(const Expr& expr, const CallExpr& call,
                                            const std::string& name) {
    const MemberEntry* member = resolved_.member(contract_, name);
    const MemberKind kind = member == nullptr ? MemberKind::Vow : member->kind();
    if (member != nullptr && kind == MemberKind::Clause) {
        const CallableSig& sig = clauseSig(*member->clause());
        checkArgs(sig.params, call, expr, "clause", name);
        markUse(expr, ExprUse::ClauseCall);
        return sig.ret;
    }
    if (isInCourse_) {
        if (const PledgeDecl* pledge = resolved_.findPledge(contract_, name)) {
            const CallableSig& sig = pledgeSig(*pledge);
            checkArgs(sig.params, call, expr, "pledge", name);
            markUse(expr, ExprUse::CoursePledgeCall);
            return sig.ret;
        }
    }
    if (member == nullptr || kind == MemberKind::Vow || kind == MemberKind::Subcontract) {
        return std::nullopt;
    }
    walkArgs(call.args);
    if (kind == MemberKind::Pledge) {
        emitAt(expr, "pledge " + quoted(name) + " is fulfilled through a signed instance");
    } else if (kind == MemberKind::Signal) {
        emitAt(expr, "signal " + quoted(name) +
                         " is delivered through a signed instance; a course awaits it");
    } else {
        emitAt(expr,
               "remedy " + quoted(name) + " runs when the contract breaks, it is never called");
    }
    return unknown();
}

// A call through a bare identifier that no local binds
TyId Checker::callIdent(const Expr& expr, const CallExpr& call, const Identifier& callee) {
    const std::string& name = callee.name;
    if (name.empty()) {
        walkArgs(call.args);
        return unknown();
    }
    if (name == "len") {
        return checkLen(expr, call);
    }
    if (name == "bytes" || name == "text" || name == "byte_list") {
        return checkBytesConversion(expr, call, name);
    }
    if (contract_ != nullptr) {
        if (const std::optional<TyId> answer = callMemberName(expr, call, name)) {
            return *answer;
        }
    }
    const GlobalEntry* global = resolved_.global(name);
    if (global != nullptr && global->kind == GlobalKind::Variant) {
        return callVariant(expr, call, name);
    }
    walkArgs(call.args);
    if (global == nullptr) {
        if (name == "now") {
            return checkNow(expr, call);
        }
        emitAt(expr, "unknown name " + quoted(name));
        return unknown();
    }
    emitAt(expr, notCallableFault(name, global->kind));
    return unknown();
}

// Why a declared global that is no variant cannot be called
std::string Checker::notCallableFault(const std::string& name, GlobalKind kind) const {
    switch (kind) {
        case GlobalKind::Contract:
            return quoted(name) + " is a contract; sign it with '" + name + ".sign(...)'";
        case GlobalKind::Record:
            return quoted(name) + " is a record; construct it with '" + name + " { ... }'";
        case GlobalKind::Sum: return quoted(name) + " is a type, not a value";
        default: return quoted(name) + " is a provisional clause, not a value";
    }
}

// A call through anything that is not a name: only a pledge value is
// callable
TyId Checker::callValue(const Expr& expr, const CallExpr& call, TyId callee) {
    if (kindOf(callee) == TyKind::Pledge) {
        return callPledgeValue(expr, call, callee);
    }
    walkArgs(call.args);
    if (kindOf(callee) != TyKind::Unknown) {
        emitAt(expr, "this expression is not callable");
    }
    return unknown();
}

TyId Checker::callMember(const Expr& expr, const CallExpr& call, const Expr& callee,
                         const MemberExpr& member) {
    if (isProcessNamespace(member.base)) {
        return checkProcessCall(expr, call, member.name);
    }
    if (isStoreNamespace(member.base)) {
        return checkStoreOp(expr, call, member.name);
    }
    if (const ContractDecl* target = staticContract(member.base)) {
        return callStatic(expr, call, *target, member.name);
    }
    const TyId base = inferExpr(member.base, TyId{});
    if (kindOf(base) == TyKind::Contract) {
        return callOnInstance(expr, call, base, member.name);
    }
    const TyId calleeType = memberWithBase(callee, member, base);
    tables_.exprTypes[static_cast<std::size_t>(call.callee.index)] = calleeType;
    return callValue(expr, call, calleeType);
}

TyId Checker::inferCall(const Expr& expr, const CallExpr& call) {
    const Expr& callee = ast_.expr(call.callee);
    const auto* ident = std::get_if<Identifier>(&callee.kind);
    if (ident != nullptr && lookup(ident->name) == nullptr) {
        return callIdent(expr, call, *ident);
    }
    if (const auto* member = std::get_if<MemberExpr>(&callee.kind)) {
        return callMember(expr, call, callee, *member);
    }
    const TyId calleeType = inferExpr(call.callee, TyId{});
    return callValue(expr, call, calleeType);
}

bool Checker::isProcessNamespace(ExprId base) const {
    const auto* identifier = std::get_if<Identifier>(&ast_.expr(base).kind);
    return identifier != nullptr && identifier->name == "Process" &&
           lookup(identifier->name) == nullptr && resolved_.global(identifier->name) == nullptr;
}

// The runtime reads fields and variants in declaration order
bool Checker::hasProcessTypes() {
    const RecordSig* command = sigs_.record("Command");
    const TyId stringType = scalar(TyKind::String);
    const std::vector<FieldSig> fields = {{"program", stringType},
        {"args", types_.list(stringType)}, {"stdout_path", stringType}, {"stderr_path", stringType}};
    if (command == nullptr || command->fields.size() != fields.size()) {
        return false;
    }
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (command->fields[i].name != fields[i].name || command->fields[i].type != fields[i].type) {
            return false;
        }
    }
    return hasProcessErrorType();
}

bool Checker::hasProcessErrorType() {
    const GlobalEntry* error = resolved_.global("ExitError");
    if (error == nullptr || error->kind != GlobalKind::Sum) {
        return false;
    }
    const auto& sum = std::get<SumDecl>(ast_.decls[static_cast<std::size_t>(error->declIndex)]);
    const std::vector<std::string> names = {"Exited", "Signaled", "SpawnFailed"};
    const std::vector<FieldSig> payloads = {{"code", scalar(TyKind::Int)},
        {"number", scalar(TyKind::Int)}, {"reason", scalar(TyKind::String)}};
    if (sum.variants.size() != names.size()) {
        return false;
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        const VariantSig* variant = sigs_.variant(names[i]);
        if (sum.variants[i].name != names[i] || variant == nullptr || variant->fields.size() != 1 ||
            variant->fields[0].name != payloads[i].name || variant->fields[0].type != payloads[i].type) {
            return false;
        }
    }
    return true;
}

TyId Checker::checkProcessCall(const Expr& expr, const CallExpr& call,
                              const std::string& operation) {
    if (operation != "run") {
        walkArgs(call.args);
        emitAt(expr, "Process has no operation " + quoted(operation));
        return unknown();
    }
    if (!hasProcessTypes()) {
        walkArgs(call.args);
        emitAt(expr, "Process.run requires Command and ExitError from std.process");
        return unknown();
    }
    checkArgs({types_.named("Command")}, call, expr, "operation", "Process.run");
    return types_.result(scalar(TyKind::Unit), types_.named("ExitError"));
}

}  // namespace geas
