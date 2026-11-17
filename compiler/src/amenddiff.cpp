// The coverage diff of one amends block, see amenddiff.hpp, and the items
// of the block, see amenddiffer.hpp
#include "amenddiffer.hpp"

#include "layout.hpp"
#include "overloaded.hpp"
#include "quote.hpp"
#include "typecode.hpp"

namespace geas {

std::string_view kindWord(EntryKind kind) {
    switch (kind) {
        case EntryKind::Vow: return "vow";
        case EntryKind::Pledge: return "pledge";
        case EntryKind::Signal: return "signal";
        case EntryKind::Sub: return "subcontract";
        case EntryKind::Remedy: return "remedy";
    }
    return "member";
}

std::int32_t entryIndex(std::size_t index) {
    return static_cast<std::int32_t>(index);
}

// Every named entry of a shape by name. An anonymous subcontract has no name to file
std::unordered_map<std::string, Entry> entriesOf(const RecordedShape& shape) {
    std::unordered_map<std::string, Entry> entries;
    for (std::size_t i = 0; i < shape.vows.size(); ++i) {
        entries.emplace(shape.vows[i].name, Entry{EntryKind::Vow, entryIndex(i)});
    }
    for (std::size_t i = 0; i < shape.items.size(); ++i) {
        const EntryKind kind = shape.items[i].isSignal ? EntryKind::Signal : EntryKind::Pledge;
        entries.emplace(shape.items[i].name, Entry{kind, entryIndex(i)});
    }
    for (std::size_t i = 0; i < shape.subs.size(); ++i) {
        if (!shape.subs[i].empty()) {
            entries.emplace(shape.subs[i], Entry{EntryKind::Sub, entryIndex(i)});
        }
    }
    for (std::size_t i = 0; i < shape.remedies.size(); ++i) {
        entries.emplace(shape.remedies[i].name, Entry{EntryKind::Remedy, entryIndex(i)});
    }
    return entries;
}

// The surface spelling of a recorded code, the code itself when it does not
// parse. A signal's payload is a bare parenthesized list, spelled as one
std::string AmendDiffer::spell(const std::string& code) {
    constexpr std::string_view tupleHead = "T";
    const bool isPayload = code.starts_with('(');
    const std::optional<TyId> type =
        typeFromCode(types_, isPayload ? std::string{tupleHead} + code : code);
    if (!type) {
        return quoted(code);
    }
    if (!isPayload) {
        return quoted(types_.render(*type));
    }
    const std::vector<TyId> params = types_.node(*type).args;
    std::string out{"("};
    for (std::size_t i = 0; i < params.size(); ++i) {
        out.append(i > 0 ? ", " : "");
        out.append(types_.render(params[i]));
    }
    return quoted(out + ")");
}

// The first declared type a code of N reaches whose layout M changed
std::optional<std::string> AmendDiffer::changedLayoutOf(const std::string& code) const {
    return changedLayout(code, from_.layouts, to_.shape.layouts);
}

std::string AmendDiffer::layoutChange(const std::string& name) const {
    return "the layout of " + quoted(name) + " changed between " + fromVersion() + " and " +
           toVersion();
}

AmendDiff AmendDiffer::run(const AmendsDecl& amends) && {
    plan_.from = from_;
    plan_.toLayouts = to_.shape.layouts;
    collect(amends);
    mapVows();
    checkNewVows();
    mapItems();
    mapSubs();
    mapRemedies();
    plan_.isCourseRestarted = restarts_ > 0;
    return AmendDiff{std::move(plan_), std::move(faults_)};
}

// Files every item, faulting a second item over the same name. Latches go
// last, so a latch on a rename's new name is caught wherever it sits
void AmendDiffer::collect(const AmendsDecl& amends) {
    for (const AmendItem& item : amends.items) {
        std::visit(Overloaded{
                       [&](const AmendVow& vow) {
                           if (!vowSets_.insert(vow.name).second) {
                               fault("amends vow " + quoted(vow.name) + " is set twice");
                           }
                       },
                       [&](const AmendRename& rename) { collectRename(rename); },
                       [&](const AmendRetire& retire) { checkRetire(retire.name); },
                       [](const AmendLatch&) {},
                       [&](const AmendRestart&) {
                           if (++restarts_ > 1) {
                               fault("restart course is written twice");
                           }
                       },
                   },
                   item);
    }
    for (const AmendItem& item : amends.items) {
        if (const auto* latch = std::get_if<AmendLatch>(&item)) {
            checkLatch(*latch);
        }
    }
}

void AmendDiffer::collectRename(const AmendRename& rename) {
    const std::string head = "rename " + quoted(rename.from) + " -> " + quoted(rename.to);
    if (!coveredOld_.insert(rename.from).second) {
        return fault(head + ": " + quoted(rename.from) + " is already renamed or retired");
    }
    if (!renameTargets_.insert(rename.to).second) {
        return fault(head + ": another rename already lands on " + quoted(rename.to));
    }
    checkRename(rename);
}

// A rename maps an entry N had and M lacks onto one M adds, the same kind
// and the same type
void AmendDiffer::checkRename(const AmendRename& rename) {
    const std::string head = "rename " + quoted(rename.from) + " -> " + quoted(rename.to);
    const auto old = fromEntries_.find(rename.from);
    const auto next = toEntries_.find(rename.to);
    if (old == fromEntries_.end()) {
        return fault("rename names " + quoted(rename.from) + ", which " + fromVersion() +
                     " never had");
    }
    if (next == toEntries_.end()) {
        return;
    }
    if (toEntries_.contains(rename.from)) {
        return fault(head + ": " + toVersion() + " still has " + quoted(rename.from));
    }
    if (fromEntries_.contains(rename.to)) {
        return fault(head + ": " + quoted(rename.to) + " is already carried from " +
                     fromVersion());
    }
    if (old->second.kind != next->second.kind) {
        return fault(head + ": " + quoted(rename.from) + " is a " +
                     std::string{kindWord(old->second.kind)} + " in " + fromVersion() + " and " +
                     quoted(rename.to) + " is a " + std::string{kindWord(next->second.kind)});
    }
    if (hasSameType(rename, old->second, next->second)) {
        renamedTo_.emplace(rename.from, rename.to);
        renamedFrom_.emplace(rename.to, rename.from);
    }
}

// A renamed vow keeps its type and a renamed pledge or signal the type of
// what it stores; a remedy's target is held when remedies map
bool AmendDiffer::hasSameType(const AmendRename& rename, const Entry& old, const Entry& next) {
    if (old.kind == EntryKind::Vow) {
        return hasSameVowType(rename, old, next);
    }
    if (old.kind != EntryKind::Pledge && old.kind != EntryKind::Signal) {
        return true;
    }
    return hasSameItemType(rename, old, next);
}

// The same code and the same layout of every declared type it reaches
bool AmendDiffer::hasSameVowType(const AmendRename& rename, const Entry& old, const Entry& next) {
    const std::string head = "rename " + quoted(rename.from) + " -> " + quoted(rename.to);
    const std::string& was = from_.vows[static_cast<std::size_t>(old.index)].type;
    const std::string& is = to_.shape.vows[static_cast<std::size_t>(next.index)].type;
    const std::optional<std::string> changed = changedLayoutOf(was);
    const std::string set = "; set " + quoted(rename.to) + " with vow " + rename.to + " = expr";
    if (was == is && !changed) {
        return true;
    }
    if (was == is) {
        fault(head + ": the vow is " + spell(was) + " in both versions, but " +
              layoutChange(*changed) + set);
        return false;
    }
    fault(head + ": the vow is " + spell(was) + " in " + fromVersion() + " and " + spell(is) +
          " in " + toVersion() + set + ", reading prior." + rename.from);
    return false;
}

bool AmendDiffer::hasSameItemType(const AmendRename& rename, const Entry& old, const Entry& next) {
    const std::string head = "rename " + quoted(rename.from) + " -> " + quoted(rename.to);
    const std::string& was = from_.items[static_cast<std::size_t>(old.index)].value;
    const std::string& is = to_.shape.items[static_cast<std::size_t>(next.index)].value;
    const std::optional<std::string> changed = changedLayoutOf(was);
    if (was == is && !changed) {
        return true;
    }
    const std::string stores = old.kind == EntryKind::Pledge ? " yields " : " carries ";
    const std::string retire = " so retire " + quoted(rename.from) + " instead";
    if (was == is) {
        fault(head + ": " + quoted(rename.from) + stores + spell(was) + " in both versions, but " +
              layoutChange(*changed) + "; a stored value cannot change layout," + retire);
        return false;
    }
    fault(head + ": " + quoted(rename.from) + stores + spell(was) + " in " + fromVersion() +
          " and " + quoted(rename.to) + stores + spell(is) +
          "; a stored value cannot change type," + retire);
    return false;
}

// A retire drops a pledge, signal, subcontract, or remedy N had and M
// lacks. A retire of a name M still has is the checker's to report
void AmendDiffer::checkRetire(const std::string& name) {
    if (!coveredOld_.insert(name).second) {
        return fault("retire names " + quoted(name) + ", which is already renamed or retired");
    }
    if (toEntries_.contains(name)) {
        return;
    }
    const auto old = fromEntries_.find(name);
    if (old == fromEntries_.end()) {
        return fault("retire names " + quoted(name) + ", which " + fromVersion() + " never had");
    }
    if (old->second.kind == EntryKind::Vow) {
        return fault("retire names the vow " + quoted(name) + "; a vow " + toVersion() +
                     " lacks is dropped with no item");
    }
    retired_.insert(name);
}

// A latch seeds an item M adds. Latching a name M lacks is the checker's to report
void AmendDiffer::checkLatch(const AmendLatch& latch) {
    const auto next = toEntries_.find(latch.name);
    const bool isItem = next != toEntries_.end() && (next->second.kind == EntryKind::Pledge ||
                                                     next->second.kind == EntryKind::Signal);
    if (!isItem) {
        return;
    }
    if (!latched_.insert(latch.name).second) {
        return fault("latch names " + quoted(latch.name) + " twice");
    }
    const auto old = fromEntries_.find(latch.name);
    if (old != fromEntries_.end() && old->second.kind != EntryKind::Vow) {
        return fault("latch names " + quoted(latch.name) + ", which " + fromVersion() +
                     " already had; its latch carries over");
    }
    if (const auto renamed = renamedFrom_.find(latch.name); renamed != renamedFrom_.end()) {
        return fault("latch names " + quoted(latch.name) + ", which takes the latch of " +
                     quoted(renamed->second) + " from " + fromVersion());
    }
    plan_.seeds.push_back(AmendSeed{next->second.index, latch.state});
}

AmendDiff diffAmends(const RecordedShape& from, const AmendTarget& to, const AmendsDecl& amends,
                     TypeArena& types) {
    return AmendDiffer{from, to, types}.run(amends);
}

}  // namespace geas
