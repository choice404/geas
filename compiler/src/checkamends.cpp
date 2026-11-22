// Amends blocks. An amends N block names an older version of its contract
// and says how an instance parked under N resumes as this version. The
// shape of N comes from the file geas record wrote beside the source, and
// the block is held to the diff between that shape and this version's, see
// amenddiff.hpp. A vow item's value reads N's vows through prior, typed
// when N's type is a scalar or one this version can still spell with the
// same layout
#include "checker.hpp"
#include "layout.hpp"
#include "shape.hpp"
#include "typecode.hpp"

namespace geas {

namespace {

constexpr std::int64_t firstVersion = 1;
constexpr std::string_view priorName = "prior";

bool isScalarKind(TyKind kind) {
    switch (kind) {
        case TyKind::Int:
        case TyKind::UInt:
        case TyKind::Float:
        case TyKind::Bool:
        case TyKind::Byte:
        case TyKind::Char:
        case TyKind::String:
        case TyKind::Bytes:
        case TyKind::Unit:
        case TyKind::Duration:
        case TyKind::Time: return true;
        default: return false;
    }
}

std::string versionText(std::int64_t version) {
    return "version " + std::to_string(version);
}

}  // namespace

// An amends block names a version at least 1 and older than this contract's own,
// and each older version is amended once. An internal contract never reaches
// the descriptor table, so no parked instance of it resumes through a block
void Checker::checkAmendsVersions(const ContractDecl& contract) {
    const std::int64_t version = contractVersion(ast_, contract);
    std::unordered_set<std::int64_t> seen;
    for (const Member& member : contract.members) {
        const auto* amends = std::get_if<AmendsDecl>(&member);
        if (amends == nullptr) {
            continue;
        }
        const std::string head = "amends " + countText(amends->version);
        const bool isInRange = amends->version >= firstVersion && amends->version < version;
        if (amends->version < firstVersion) {
            emit(amends->lo, amends->hi, head + " names no version; versions start at 1");
        } else if (amends->version >= version) {
            emit(amends->lo, amends->hi,
                 head + " must name a version older than contract " + quoted(contract.name) +
                     ", which is at version " + countText(version));
        }
        const bool isFirst = seen.insert(amends->version).second;
        if (!isFirst) {
            emit(amends->lo, amends->hi,
                 "duplicate " + head + " in contract " + quoted(contract.name));
        }
        if (contract.isInternal) {
            emit(amends->lo, amends->hi,
                 "contract " + quoted(contract.name) +
                     " is internal and never parks under a version of its own, so it takes no "
                     "amends block");
        } else if (isInRange && isFirst) {
            amendable_.insert(amends);
        }
    }
}

// shapes/<Contract>.v<N>.shape beside the file that declares the contract
std::string Checker::recordedPathOf(const ContractDecl& contract, std::int64_t version) const {
    std::string_view source = program_.files.empty() ? std::string_view{"."}
                                                     : std::string_view{program_.files[0].path};
    for (const LoadedFile& file : program_.files) {
        if (contract.lo >= file.base && contract.lo <= file.base + file.length) {
            source = file.path;
            break;
        }
    }
    return recordedShapePath(directoryOfPath(source), contract.name, version);
}

// The recorded shape of the version a block amends, faulted when the file is
// missing, unreadable, or records another contract or version
std::optional<RecordedShape> Checker::loadAmendShape(const ContractDecl& contract,
                                                     const AmendsDecl& amends) {
    const std::string path = recordedPathOf(contract, amends.version);
    const std::string head = "amends " + countText(amends.version);
    if (!isRecordedShapeFile(path)) {
        emit(amends.lo, amends.hi,
             head + " needs the recorded shape of " + quoted(contract.name) + " " +
                 versionText(amends.version) + " at " + quoted(path) +
                 "; run geas record on the source of " + versionText(amends.version));
        return std::nullopt;
    }
    auto shape = readRecordedShape(path);
    if (!shape) {
        emit(amends.lo, amends.hi,
             head + " cannot use the recorded shape " + quoted(path) + ": " + shape.error());
        return std::nullopt;
    }
    if (shape->contract != contract.name || shape->version != amends.version) {
        emit(amends.lo, amends.hi,
             head + " finds the recorded shape of " + quoted(shape->contract) + " " +
                 versionText(shape->version) + " at " + quoted(path) + ", not " +
                 quoted(contract.name) + " " + versionText(amends.version));
        return std::nullopt;
    }
    return std::move(*shape);
}

// Diffs the recorded shape against this version's and files the plan
void Checker::diffAmendShape(const ContractDecl& contract, const AmendsDecl& amends,
                             RecordedShape from) {
    const ContractShape shape = buildContractShape(ast_, sigs_, types_, tables_, contract);
    const RecordedShape to = recordShape(shape, sigs_, types_, 0);
    std::vector<bool> defaults;
    for (const VowDecl* vow : shape.vows) {
        defaults.push_back(vow->init.has_value());
    }
    AmendDiff diff = diffAmends(from, AmendTarget{to, defaults}, amends, types_);
    for (std::string& fault : diff.faults) {
        emit(amends.lo, amends.hi, std::move(fault));
    }
    tables_.amends.insert_or_assign(&amends, std::move(diff.plan));
}

void Checker::checkAmends(const ContractDecl& contract, const AmendsDecl& amends) {
    if (amendable_.contains(&amends)) {
        if (std::optional<RecordedShape> from = loadAmendShape(contract, amends)) {
            diffAmendShape(contract, amends, std::move(*from));
        }
    }
    const auto plan = tables_.amends.find(&amends);
    amendPlan_ = plan == tables_.amends.end() ? nullptr : &plan->second;
    amendUnsetVows_ = amendPlan_ == nullptr ? std::unordered_set<std::string>{}
                                            : unsetAmendVows(contract, *amendPlan_);
    for (const AmendItem& item : amends.items) {
        if (const auto* vow = std::get_if<AmendVow>(&item)) {
            checkAmendVow(contract, amends, *vow);
            // A vow item sets its vow for every item after it
            amendUnsetVows_.erase(vow->name);
        } else {
            checkAmendName(contract, amends, item);
        }
    }
    amendUnsetVows_.clear();
    amendPlan_ = nullptr;
}

// The vows of this version a block has to set before a vow item reads them:
// every vow no vow of the amended version carries into and with no default
std::unordered_set<std::string> Checker::unsetAmendVows(const ContractDecl& contract,
                                                        const AmendPlan& plan) const {
    const ContractShape shape = buildContractShape(ast_, sigs_, types_, tables_, contract);
    std::vector<bool> isCarried(shape.vows.size(), false);
    for (const std::int32_t target : plan.vowMap) {
        if (target >= 0 && static_cast<std::size_t>(target) < isCarried.size()) {
            isCarried[static_cast<std::size_t>(target)] = true;
        }
    }
    std::unordered_set<std::string> names;
    for (std::size_t i = 0; i < shape.vows.size(); ++i) {
        if (!isCarried[i] && !shape.vows[i]->init.has_value()) {
            names.insert(shape.vows[i]->name);
        }
    }
    return names;
}

// A vow read inside a vow item reads the slot as the block has filled it so
// far, so a vow only this block sets reads as nothing until an earlier item
// set it
void Checker::checkAmendVowRead(const Expr& expr, const std::string& name) {
    if (!isInAmends_ || !amendUnsetVows_.contains(name) || amendPlan_ == nullptr) {
        return;
    }
    const std::string from = versionText(amendPlan_->from.version);
    emitAt(expr, "vow " + quoted(name) + " is read before this amends " +
                     countText(amendPlan_->from.version) + " block sets it; it is not carried "
                     "from " + from + " and has no default, so only a vow item after the one "
                     "that sets it may read it");
}

// An amends vow, vow NAME = expr, supplies a vow this version has. The older
// instance's vows are read through prior
void Checker::checkAmendVow(const ContractDecl& contract, const AmendsDecl& amends,
                            const AmendVow& item) {
    const MemberEntry* entry = resolved_.member(&contract, item.name);
    const bool isVow = entry != nullptr && entry->kind() == MemberKind::Vow;
    if (!isVow && !item.name.empty()) {
        emit(amends.lo, amends.hi,
             "amends vow " + quoted(item.name) + " is not a vow of contract " +
                 quoted(contract.name));
    }
    const TyId declared = isVow ? vowType(*entry->vow()) : TyId{};
    bodyLo_ = amends.lo;
    bodyHi_ = amends.hi;
    isInAmends_ = true;
    amendVowType_ = declared;
    const TyId actual = inferExpr(item.value, declared);
    isInAmends_ = false;
    amendVowType_ = TyId{};
    if (declared.isSet() && !isCompatible(declared, actual)) {
        emitAt(ast_.expr(item.value), typesMessage("amends vow " + quoted(item.name) + " has type ",
                                                   actual, " but the vow is ", declared));
    }
}

// The rename, retire, and latch items, held to what this version alone can
// verify; what they mean against the older version is the diff's to judge
void Checker::checkAmendName(const ContractDecl& contract, const AmendsDecl& amends,
                             const AmendItem& item) {
    const auto fault = [&](std::string message) {
        emit(amends.lo, amends.hi, std::move(message) + " of contract " + quoted(contract.name));
    };
    const auto kindOfName = [&](const std::string& name) -> std::optional<MemberKind> {
        const MemberEntry* entry = resolved_.reachable(&contract, name);
        return entry == nullptr ? std::nullopt : std::optional<MemberKind>{entry->kind()};
    };
    if (const auto* rename = std::get_if<AmendRename>(&item)) {
        const std::optional<MemberKind> kind = kindOfName(rename->to);
        const bool isAllowed = kind && *kind != MemberKind::Clause;
        if (!isAllowed && !rename->to.empty()) {
            fault("rename target " + quoted(rename->to) +
                  " is not a pledge, a subcontract, a vow, a signal, or a remedy");
        }
    } else if (const auto* latch = std::get_if<AmendLatch>(&item)) {
        const std::optional<MemberKind> kind = kindOfName(latch->name);
        const bool isAllowed = kind && (*kind == MemberKind::Pledge || *kind == MemberKind::Signal);
        if (!isAllowed && !latch->name.empty()) {
            fault("latch names " + quoted(latch->name) + ", which is not a pledge or a signal");
        }
        checkFulfilledSeed(contract, amends, *latch);
    } else if (const auto* retire = std::get_if<AmendRetire>(&item)) {
        if (kindOfName(retire->name)) {
            fault("retire names " + quoted(retire->name) + ", which is still a member");
        }
    }
}

// The type of the value an item hands whatever reads it once it is
// fulfilled: a pledge's Ok value, the payload of an Option, or a bare
// buffer, and a signal's payload. Unset when the item carries none
TyId Checker::seededValueType(const MemberEntry& entry) {
    if (entry.kind() == MemberKind::Signal) {
        const TyId payload = signalPayload(*entry.signal());
        return kindOf(payload) == TyKind::Unit ? TyId{} : payload;
    }
    if (entry.kind() != MemberKind::Pledge) {
        return TyId{};
    }
    const TyNode yielded = types_.node(pledgeSig(*entry.pledge()).ret);
    if (yielded.kind == TyKind::Bytes) {
        return scalar(TyKind::Bytes);
    }
    const bool isWrapped = yielded.kind == TyKind::Result || yielded.kind == TyKind::Option;
    if (!isWrapped || kindOf(yielded.args[0]) == TyKind::Unit ||
        kindOf(yielded.args[0]) == TyKind::Unknown) {
        return TyId{};
    }
    return yielded.args[0];
}

// A seed of fulfilled latches an item with no value, so it is refused for
// an item that carries one: a course awaiting the signal or reading the
// pledge, or a remedy taking the pledge's Ok value, would have nothing to
// be handed. Such an item starts pending instead
void Checker::checkFulfilledSeed(const ContractDecl& contract, const AmendsDecl& amends,
                                 const AmendLatch& latch) {
    if (latch.state != LatchState::Fulfilled) {
        return;
    }
    const MemberEntry* entry = resolved_.reachable(&contract, latch.name);
    if (entry == nullptr) {
        return;
    }
    const TyId value = seededValueType(*entry);
    if (!value.isSet()) {
        return;
    }
    const std::string carries = entry->kind() == MemberKind::Signal
                                    ? "signal " + quoted(latch.name) + " carries a payload of type "
                                    : "pledge " + quoted(latch.name) + " yields a value of type ";
    emit(amends.lo, amends.hi,
         "latch " + latch.name + " as fulfilled seeds no value, but " + carries +
             quoted(types_.render(value)) + " that whatever reads it later needs; latch " +
             quoted(latch.name) + " as pending");
}

// Whether a member access reads through prior: the bare name prior inside an
// amends value, with no local, member, or global of that name in the way
bool Checker::isPriorBase(ExprId base) const {
    const auto* ident = std::get_if<Identifier>(&ast_.expr(base).kind);
    if (!isInAmends_ || ident == nullptr || ident->name != priorName) {
        return false;
    }
    return lookup(priorName) == nullptr && resolved_.member(contract_, ident->name) == nullptr &&
           resolved_.global(ident->name) == nullptr;
}

// prior.NAME, the value of vow NAME in the version the block amends
TyId Checker::inferPriorRead(const Expr& expr, const MemberExpr& member) {
    markUse(ast_.expr(member.base), ExprUse::PriorInstance);
    if (amendPlan_ == nullptr) {
        return unknown();
    }
    for (const RecordedVow& vow : amendPlan_->from.vows) {
        if (vow.name == member.name) {
            return priorVowType(expr, member, vow);
        }
    }
    emitAt(expr, versionText(amendPlan_->from.version) + " has no vow " + quoted(member.name) +
                     " to read through prior");
    return unknown();
}

// The type a prior read takes: a scalar of N stands as itself, and any
// other type of N only when it is the type of this version's vow of the
// same name or of the vow being set, the types this version still spells,
// and every declared type it reaches kept its layout
TyId Checker::priorVowType(const Expr& expr, const MemberExpr& member, const RecordedVow& vow) {
    const std::string from = versionText(amendPlan_->from.version);
    const std::optional<TyId> type = typeFromCode(types_, vow.type);
    if (!type) {
        emitAt(expr, "prior." + member.name + " has the type code " + quoted(vow.type) + " in " +
                         from + ", which this compiler cannot read");
        return unknown();
    }
    markUse(expr, ExprUse::PriorVow);
    if (isScalarKind(kindOf(*type))) {
        return *type;
    }
    const MemberEntry* same = resolved_.member(contract_, member.name);
    const bool isSameNamedVow = same != nullptr && same->kind() == MemberKind::Vow &&
                                vowType(*same->vow()) == *type;
    if (*type == amendVowType_ || isSameNamedVow) {
        return priorLayoutType(expr, member, vow, *type);
    }
    emitAt(expr, "prior." + member.name + " is " + quoted(types_.render(*type)) + " in " + from +
                     ", a type " + versionText(contractVersion(ast_, *contract_)) +
                     " no longer declares; prior reads a scalar, or a type equal to the vow "
                     "being set or to this version's vow " + quoted(member.name));
    return unknown();
}

// A composite prior read keeps its type only when the layout of every
// declared record and sum it reaches is the same in both versions
TyId Checker::priorLayoutType(const Expr& expr, const MemberExpr& member, const RecordedVow& vow,
                              TyId type) {
    const std::optional<std::string> changed =
        changedLayout(vow.type, amendPlan_->from.layouts, amendPlan_->toLayouts);
    if (!changed) {
        return type;
    }
    const std::string from = versionText(amendPlan_->from.version);
    const std::string to = versionText(contractVersion(ast_, *contract_));
    emitAt(expr, "prior." + member.name + " is " + quoted(types_.render(type)) + " in " + from +
                     ", but the layout of " + quoted(*changed) + " changed between " + from +
                     " and " + to + "; prior reads a declared type only when its layout is the "
                     "same in both versions");
    return unknown();
}

}  // namespace geas
