// The mapping half of the amends diff: every entry of the amended version
// lands on an entry of this one or is dropped, and a difference no item
// covers is a fault, see amenddiffer.hpp
#include "amenddiffer.hpp"

#include "quote.hpp"

namespace geas {

// Each vow of N lands on M's vow of the same name and type, on a rename's
// new name, or nowhere. A vow whose type or layout changed has to be set
void AmendDiffer::mapVows() {
    for (const RecordedVow& vow : from_.vows) {
        const auto same = toEntries_.find(vow.name);
        const auto renamed = renamedTo_.find(vow.name);
        std::int32_t target = droppedEntry;
        if (same != toEntries_.end() && same->second.kind == EntryKind::Vow) {
            const auto index = static_cast<std::size_t>(same->second.index);
            const std::string& type = to_.shape.vows[index].type;
            const std::optional<std::string> changed = changedLayoutOf(vow.type);
            if (type == vow.type && !changed) {
                target = same->second.index;
            } else if (!vowSets_.contains(vow.name)) {
                faultVowType(vow, type, changed);
            }
        } else if (renamed != renamedTo_.end()) {
            target = toEntries_.at(renamed->second).index;
        }
        plan_.vowMap.push_back(target);
    }
}

// A vow of N whose type M changed, or whose type kept its code while the
// layout of a declared type it reaches changed
void AmendDiffer::faultVowType(const RecordedVow& vow, const std::string& type,
                               const std::optional<std::string>& changed) {
    const std::string supply = "; supply it with vow " + vow.name + " = expr";
    if (type == vow.type && changed) {
        return fault("vow " + quoted(vow.name) + " is " + spell(vow.type) +
                     " in both versions, but " + layoutChange(*changed) + supply);
    }
    fault("vow " + quoted(vow.name) + " is " + spell(vow.type) + " in " + fromVersion() + " and " +
          spell(type) + " in " + toVersion() + supply);
}

// A vow M adds takes its default, or a rename's value, or has to be set
void AmendDiffer::checkNewVows() {
    for (std::size_t i = 0; i < to_.shape.vows.size(); ++i) {
        const std::string& name = to_.shape.vows[i].name;
        const bool isCovered = fromEntries_.contains(name) || renameTargets_.contains(name) ||
                               vowSets_.contains(name) || to_.vowDefaults[i];
        if (!isCovered) {
            fault("vow " + quoted(name) + " is new in " + toVersion() +
                  " and has no default; supply it with vow " + name + " = expr");
        }
    }
}

// An entry of N that M no longer has under its name: a rename's new index,
// dropped by a retire, or a fault when no item covers it
std::int32_t AmendDiffer::coverGone(EntryKind kind, const std::string& name) {
    if (const auto renamed = renamedTo_.find(name); renamed != renamedTo_.end()) {
        return toEntries_.at(renamed->second).index;
    }
    if (!coveredOld_.contains(name)) {
        fault(std::string{kindWord(kind)} + " " + quoted(name) + " of " + fromVersion() +
              " is not in " + toVersion() + "; cover it with rename " + name +
              " -> NEW or retire " + name);
    }
    return droppedEntry;
}

// A pledge or signal M has under the same name keeps its latch when it is
// the same kind storing the same type; otherwise its stored state cannot carry
std::int32_t AmendDiffer::mapItem(const RecordedItem& item) {
    const EntryKind kind = item.isSignal ? EntryKind::Signal : EntryKind::Pledge;
    const auto same = toEntries_.find(item.name);
    if (same == toEntries_.end()) {
        return coverGone(kind, item.name);
    }
    const std::string word{kindWord(kind)};
    if (same->second.kind != kind) {
        fault(word + " " + quoted(item.name) + " of " + fromVersion() + " is a " +
              std::string{kindWord(same->second.kind)} + " in " + toVersion() +
              "; declare the new one under a new name and retire " + quoted(item.name));
        return droppedEntry;
    }
    const std::string& value = to_.shape.items[static_cast<std::size_t>(same->second.index)].value;
    const std::optional<std::string> changed = changedLayoutOf(item.value);
    if (value == item.value && !changed) {
        return same->second.index;
    }
    faultItemType(item, value, changed);
    return droppedEntry;
}

// A pledge or signal of N whose stored type M changed, or whose stored type
// kept its code while the layout of a declared type it reaches changed
void AmendDiffer::faultItemType(const RecordedItem& item, const std::string& value,
                                const std::optional<std::string>& changed) {
    const std::string word{kindWord(item.isSignal ? EntryKind::Signal : EntryKind::Pledge)};
    const std::string head = word + " " + quoted(item.name) +
                             (item.isSignal ? " carries " : " yields ") + spell(item.value);
    const std::string retire = "; its stored value cannot carry, so retire " + quoted(item.name) +
                               " and declare the " + word + " under a new name";
    if (value == item.value && changed) {
        return fault(head + " in both versions, but " + layoutChange(*changed) + retire);
    }
    fault(head + " in " + fromVersion() + " and " + spell(value) + " in " + toVersion() + retire);
}

void AmendDiffer::mapItems() {
    for (const RecordedItem& item : from_.items) {
        plan_.itemMap.push_back(mapItem(item));
    }
}

// A named subcontract maps by name like any entry; the anonymous ones of N
// pair with the anonymous ones of M in order, and one past M's count is dropped
void AmendDiffer::mapSubs() {
    std::vector<std::int32_t> anonymous;
    for (std::size_t i = 0; i < to_.shape.subs.size(); ++i) {
        if (to_.shape.subs[i].empty()) {
            anonymous.push_back(entryIndex(i));
        }
    }
    std::size_t nextAnonymous = 0;
    for (const std::string& sub : from_.subs) {
        if (sub.empty()) {
            const bool hasPair = nextAnonymous < anonymous.size();
            plan_.subMap.push_back(hasPair ? anonymous[nextAnonymous++] : droppedEntry);
            continue;
        }
        const auto same = toEntries_.find(sub);
        const bool isSame = same != toEntries_.end() && same->second.kind == EntryKind::Sub;
        plan_.subMap.push_back(isSame ? same->second.index : coverGone(EntryKind::Sub, sub));
    }
}

// A remedy that ran stays ran under its name in M, so long as it still
// undoes the same pledge, followed through any rename of that pledge
std::int32_t AmendDiffer::mapRemedy(const RecordedRemedy& remedy) {
    const auto same = toEntries_.find(remedy.name);
    const bool isSame = same != toEntries_.end() && same->second.kind == EntryKind::Remedy;
    const std::int32_t target =
        isSame ? same->second.index : coverGone(EntryKind::Remedy, remedy.name);
    if (target == droppedEntry) {
        return droppedEntry;
    }
    const auto renamed = renamedTo_.find(remedy.target);
    const std::string& was = renamed == renamedTo_.end() ? remedy.target : renamed->second;
    const RecordedRemedy& next = to_.shape.remedies[static_cast<std::size_t>(target)];
    if (next.target == was) {
        return target;
    }
    fault("remedy " + quoted(remedy.name) + " undoes " + quoted(remedy.target) + " in " +
          fromVersion() + " and " + quoted(next.target) + " in " + toVersion() + "; retire " +
          quoted(remedy.name) + " and declare the remedy under a new name");
    return droppedEntry;
}

void AmendDiffer::mapRemedies() {
    for (const RecordedRemedy& remedy : from_.remedies) {
        plan_.remedyMap.push_back(mapRemedy(remedy));
    }
}

}  // namespace geas
