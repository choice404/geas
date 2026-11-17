// The differ behind diffAmends, internal to amenddiff.cpp and amendmap.cpp.
// One AmendDiffer walks one amends block: it files the block's items, judges
// each rename, retire, and latch against the two shapes, then maps every
// entry of the amended version onto this one, faulting each difference no
// item covers. amenddiff.cpp holds the items, amendmap.cpp the mapping
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "amenddiff.hpp"

namespace geas {

inline constexpr std::int32_t droppedEntry = -1;

enum class EntryKind { Vow, Pledge, Signal, Sub, Remedy };

// One named entry of a shape, its kind and its index in that kind's table
struct Entry {
    EntryKind kind;
    std::int32_t index;
};

/**
 * The word a diagnostic names an entry kind with.
 * @params kind - the kind
 * @returns {std::string_view} - vow, pledge, signal, subcontract, or remedy
 */
std::string_view kindWord(EntryKind kind);

/**
 * A table position as a map entry.
 * @params index - the position
 * @returns {std::int32_t} - the same position
 */
std::int32_t entryIndex(std::size_t index);

/**
 * Every named entry of a shape by name. An anonymous subcontract has no name to file.
 * @params shape - the shape
 * @returns {std::unordered_map<std::string, Entry>} - each name's kind and index
 */
std::unordered_map<std::string, Entry> entriesOf(const RecordedShape& shape);

class AmendDiffer {
public:
    AmendDiffer(const RecordedShape& from, const AmendTarget& to, TypeArena& types)
        : from_{from}, to_{to}, types_{types}, fromEntries_{entriesOf(from)},
          toEntries_{entriesOf(to.shape)} {}

    AmendDiff run(const AmendsDecl& amends) &&;

private:
    void collect(const AmendsDecl& amends);
    void collectRename(const AmendRename& rename);
    void checkRename(const AmendRename& rename);
    bool hasSameType(const AmendRename& rename, const Entry& old, const Entry& next);
    bool hasSameVowType(const AmendRename& rename, const Entry& old, const Entry& next);
    bool hasSameItemType(const AmendRename& rename, const Entry& old, const Entry& next);
    void checkRetire(const std::string& name);
    void checkLatch(const AmendLatch& latch);
    void mapVows();
    void faultVowType(const RecordedVow& vow, const std::string& type,
                      const std::optional<std::string>& changed);
    void checkNewVows();
    void mapItems();
    std::int32_t mapItem(const RecordedItem& item);
    void faultItemType(const RecordedItem& item, const std::string& value,
                       const std::optional<std::string>& changed);
    void mapSubs();
    void mapRemedies();
    std::int32_t mapRemedy(const RecordedRemedy& remedy);
    std::int32_t coverGone(EntryKind kind, const std::string& name);
    std::string spell(const std::string& code);
    std::optional<std::string> changedLayoutOf(const std::string& code) const;
    std::string layoutChange(const std::string& name) const;
    std::string fromVersion() const { return "version " + std::to_string(from_.version); }
    std::string toVersion() const { return "version " + std::to_string(to_.shape.version); }
    void fault(std::string message) { faults_.push_back(std::move(message)); }

    const RecordedShape& from_;
    const AmendTarget& to_;
    TypeArena& types_;
    std::unordered_map<std::string, Entry> fromEntries_;
    std::unordered_map<std::string, Entry> toEntries_;
    std::unordered_set<std::string> vowSets_;
    std::unordered_map<std::string, std::string> renamedTo_;
    std::unordered_map<std::string, std::string> renamedFrom_;
    std::unordered_set<std::string> retired_;
    std::unordered_set<std::string> latched_;
    std::unordered_set<std::string> coveredOld_;
    std::unordered_set<std::string> renameTargets_;
    std::size_t restarts_ = 0;
    AmendPlan plan_;
    std::vector<std::string> faults_;
};

}  // namespace geas
