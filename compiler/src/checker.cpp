// The checker's shared state and helpers, see checker.hpp
#include "checker.hpp"

#include <utility>

namespace geas {

namespace {

// What a lookup hands back when a table has no entry, which only happens on
// a tree the parser already faulted
const CallableSig emptySig{};

template <typename Map, typename Key>
const CallableSig& sigIn(const Map& map, const Key* key) {
    const auto found = map.find(key);
    return found == map.end() ? emptySig : found->second;
}

}  // namespace

Checker::Checker(const Program& program, const Resolved& resolved, const Sigs& sigs,
                 TypeArena& types, TypeLowering& lowering, std::vector<Diagnostic>& diagnostics)
    : program_{program},
      ast_{program.ast},
      resolved_{resolved},
      sigs_{sigs},
      types_{types},
      lowering_{lowering},
      diagnostics_{diagnostics} {}

// Every contract's members check in declaration order
CheckTables Checker::run() && {
    tables_.exprTypes.assign(ast_.exprs.size(), TyId{});
    tables_.exprUses.assign(ast_.exprs.size(), ExprUse::None);
    tables_.patternTypes.assign(ast_.patterns.size(), TyId{});
    for (const Decl& decl : ast_.decls) {
        if (const auto* contract = std::get_if<ContractDecl>(&decl)) {
            checkContract(*contract);
        }
    }
    return std::move(tables_);
}

void Checker::emit(std::int64_t lo, std::int64_t hi, std::string message) {
    diagnostics_.push_back(Diagnostic{lo, hi, std::move(message)});
}

void Checker::emitAt(const Expr& expr, std::string message) {
    emit(expr.lo, expr.hi, std::move(message));
}

// Statements carry no spans, so a fault with no expression to point at
// lands on the enclosing body
void Checker::emitInBody(std::string message) {
    emit(bodyLo_, bodyHi_, std::move(message));
}

// A two type message, head 'first' middle 'second'
std::string Checker::typesMessage(std::string_view head, TyId first, std::string_view middle,
                                  TyId second) const {
    std::string out{head};
    out.append(quoted(types_.render(first)));
    out.append(middle);
    out.append(quoted(types_.render(second)));
    return out;
}

TyId Checker::scalar(TyKind kind) {
    return types_.scalar(kind);
}

TyId Checker::unknown() {
    return types_.unknown();
}

TyKind Checker::kindOf(TyId type) const {
    return types_.kindOf(type);
}

bool Checker::isCompatible(TyId a, TyId b) const {
    return types_.isCompatible(a, b);
}

void Checker::pushScope() {
    scopes_.emplace_back();
}

void Checker::popScope() {
    scopes_.pop_back();
}

// Declares a binding in the innermost scope, replacing any shadowed name
void Checker::declare(const std::string& name, TyId type, bool isMutable) {
    if (isInCourse_ && hasContractType(type)) {
        emitInBody("course local " + quoted(name) + " cannot hold a contract instance, even inside "
                   "a composite; signing another contract and holding its instance in a course "
                   "is not allowed");
    }
    scopes_.back().insert_or_assign(name, Binding{type, isMutable});
}

// The binding a name resolves to, innermost scope first
const Binding* Checker::lookup(std::string_view name) const {
    const std::string key{name};
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        const auto found = scope->find(key);
        if (found != scope->end()) {
            return &found->second;
        }
    }
    return nullptr;
}

ExprId Checker::idOf(const Expr& expr) const {
    return ExprId{static_cast<std::int32_t>(&expr - ast_.exprs.data())};
}

void Checker::markUse(const Expr& expr, ExprUse use) {
    tables_.exprUses[static_cast<std::size_t>(idOf(expr).index)] = use;
}

const CallableSig& Checker::pledgeSig(const PledgeDecl& pledge) const {
    return sigIn(sigs_.pledges, &pledge);
}

const CallableSig& Checker::clauseSig(const ClauseDecl& clause) const {
    return sigIn(sigs_.clauses, &clause);
}

const CallableSig& Checker::signalSig(const SignalDecl& signal) const {
    return sigIn(sigs_.signals, &signal);
}

const CallableSig& Checker::remedySig(const RemedyDecl& remedy) const {
    return sigIn(sigs_.remedies, &remedy);
}

TyId Checker::vowType(const VowDecl& vow) {
    const auto found = sigs_.vows.find(&vow);
    return found == sigs_.vows.end() ? unknown() : found->second.type;
}

// The contract a name declares, null when the name is anything else
const ContractDecl* Checker::contractNamed(std::string_view name) const {
    const GlobalEntry* global = resolved_.global(name);
    if (global == nullptr || global->kind != GlobalKind::Contract) {
        return nullptr;
    }
    return contractAt(ast_, global->declIndex);
}

int Checker::packageOf(const ContractDecl& contract) const {
    const auto found = resolved_.contracts.find(&contract);
    return found == resolved_.contracts.end() ? -1 : found->second.package;
}

bool Checker::hasCourse(const ContractDecl& contract) const {
    for (const Member& member : contract.members) {
        if (std::holds_alternative<CourseDecl>(member)) {
            return true;
        }
    }
    return false;
}

}  // namespace geas
