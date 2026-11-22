// The smallest value of a course slot's type, see smallest.hpp
#include "smallest.hpp"

#include <algorithm>

namespace geas {

namespace {

// The node count past which every count reads the same
constexpr std::uint64_t nodeCeiling = slotMinNodesMax + 1;

// One level more than inner, or no finite value
std::uint64_t oneAbove(std::uint64_t inner) {
    return inner == noFiniteValue ? noFiniteValue : inner + 1;
}

// The sum of two node counts, saturating at the ceiling
std::uint64_t addNodes(std::uint64_t a, std::uint64_t b) {
    if (a == noFiniteValue || b == noFiniteValue) {
        return noFiniteValue;
    }
    return std::min(a + b, nodeCeiling);
}

}  // namespace

SmallestValues::SmallestValues(const EmitLookup& lookup, const std::vector<TyId>& roots)
    : lookup_{lookup} {
    for (const TyId root : roots) {
        reach(root);
    }
    heights_.assign(declared_.size(), noFiniteValue);
    nodes_.assign(declared_.size(), noFiniteValue);
    settleHeights();
    settleNodes();
}

SmallestValue SmallestValues::of(TyId type) const {
    return SmallestValue{heightOf(type), nodesOf(type)};
}

// Every record and sum a type reaches through a Tuple, a Result, or a field.
// A List, a Map, and an Option stop the walk, since their smallest value is
// empty or None whatever they hold
void SmallestValues::reach(TyId type) {
    const TyNode node = lookup_.types().node(type);
    if (node.kind == TyKind::Tuple || node.kind == TyKind::Result) {
        for (const TyId arg : node.args) {
            reach(arg);
        }
        return;
    }
    if (node.kind != TyKind::Named || indices_.contains(node.name)) {
        return;
    }
    const RecordSig* record = recordOf(type);
    const SumDecl* sum = lookup_.sumOf(type);
    if (record == nullptr && sum == nullptr) {
        return;
    }
    indices_.emplace(node.name, declared_.size());
    declared_.push_back(type);
    if (record != nullptr) {
        reachFields(record->fields);
        return;
    }
    for (const Variant& variant : sum->variants) {
        if (const std::vector<FieldSig>* payload = payloadOf(variant, *sum)) {
            reachFields(*payload);
        }
    }
}

void SmallestValues::reachFields(const std::vector<FieldSig>& fields) {
    for (const FieldSig& field : fields) {
        reach(field.type);
    }
}

// Each pass recomputes every name from the heights of the pass before; one
// pass per name settles them all, and the pass after changes nothing
void SmallestValues::settleHeights() {
    bool isChanged = true;
    for (std::size_t pass = 0; isChanged && pass <= declared_.size(); ++pass) {
        std::vector<std::uint64_t> next(declared_.size());
        for (std::size_t i = 0; i < declared_.size(); ++i) {
            next[i] = declaredHeight(declared_[i]);
        }
        isChanged = next != heights_;
        heights_ = std::move(next);
    }
}

// The node counts settle the same way once the heights are fixed, since a
// Result takes the arm its heights pick
void SmallestValues::settleNodes() {
    bool isChanged = true;
    for (std::size_t pass = 0; isChanged && pass <= declared_.size(); ++pass) {
        std::vector<std::uint64_t> next(declared_.size());
        for (std::size_t i = 0; i < declared_.size(); ++i) {
            const bool isFinite = heights_[i] != noFiniteValue;
            next[i] = isFinite ? declaredNodes(declared_[i]) : noFiniteValue;
        }
        isChanged = next != nodes_;
        nodes_ = std::move(next);
    }
}

// The height of any type from the current heights of the names it reaches
std::uint64_t SmallestValues::heightOf(TyId type) const {
    const TyNode node = lookup_.types().node(type);
    switch (node.kind) {
        case TyKind::Tuple: {
            std::uint64_t deepest = 0;
            for (const TyId arg : node.args) {
                const std::uint64_t height = heightOf(arg);
                if (height == noFiniteValue) {
                    return noFiniteValue;
                }
                deepest = std::max(deepest, height);
            }
            return deepest + 1;
        }
        case TyKind::Result:
            return oneAbove(std::min(heightOf(node.args[0]), heightOf(node.args[1])));
        case TyKind::Named: {
            const auto found = indices_.find(node.name);
            return found == indices_.end() ? 1 : heights_[found->second];
        }
        default: return 1;
    }
}

// The node count of any type's smallest value from the current counts
std::uint64_t SmallestValues::nodesOf(TyId type) const {
    const TyNode node = lookup_.types().node(type);
    switch (node.kind) {
        case TyKind::Tuple: {
            std::uint64_t count = 1;
            for (const TyId arg : node.args) {
                count = addNodes(count, nodesOf(arg));
            }
            return count;
        }
        case TyKind::Result: {
            // The arm that nests least, Ok on a tie, the one the runtime builds
            const bool isErr = heightOf(node.args[1]) < heightOf(node.args[0]);
            return addNodes(1, nodesOf(node.args[isErr ? 1 : 0]));
        }
        case TyKind::Named: {
            const auto found = indices_.find(node.name);
            return found == indices_.end() ? 1 : nodes_[found->second];
        }
        default: return 1;
    }
}

// A record nests one level above its deepest field, a sum one above the
// variant whose fields nest least, a variant with no payload counting 0
std::uint64_t SmallestValues::declaredHeight(TyId type) const {
    if (const RecordSig* record = recordOf(type)) {
        return oneAbove(deepestField(record->fields));
    }
    const SumDecl& sum = *lookup_.sumOf(type);
    std::uint64_t least = noFiniteValue;
    for (const Variant& variant : sum.variants) {
        const std::vector<FieldSig>* payload = payloadOf(variant, sum);
        least = std::min(least, payload == nullptr ? 0 : deepestField(*payload));
    }
    return oneAbove(least);
}

// A record counts itself and every field, a sum itself and the fields of the
// variant its height picks, the one the runtime builds
std::uint64_t SmallestValues::declaredNodes(TyId type) const {
    if (const RecordSig* record = recordOf(type)) {
        return addNodes(1, fieldNodes(record->fields));
    }
    const SumDecl& sum = *lookup_.sumOf(type);
    const std::vector<FieldSig>* chosen = leastVariant(sum);
    return addNodes(1, chosen == nullptr ? 0 : fieldNodes(*chosen));
}

// The payload of the variant whose fields nest least, the first of equals,
// null when that variant carries none
const std::vector<FieldSig>* SmallestValues::leastVariant(const SumDecl& sum) const {
    const std::vector<FieldSig>* chosen = nullptr;
    std::uint64_t least = noFiniteValue;
    for (const Variant& variant : sum.variants) {
        const std::vector<FieldSig>* payload = payloadOf(variant, sum);
        const std::uint64_t height = payload == nullptr ? 0 : deepestField(*payload);
        if (height < least) {
            least = height;
            chosen = payload;
        }
    }
    return chosen;
}

// The height of the deepest field, 0 for none, noFiniteValue when any has none
std::uint64_t SmallestValues::deepestField(const std::vector<FieldSig>& fields) const {
    std::uint64_t deepest = 0;
    for (const FieldSig& field : fields) {
        const std::uint64_t height = heightOf(field.type);
        if (height == noFiniteValue) {
            return noFiniteValue;
        }
        deepest = std::max(deepest, height);
    }
    return deepest;
}

// The nodes of every field together, 0 for none
std::uint64_t SmallestValues::fieldNodes(const std::vector<FieldSig>& fields) const {
    std::uint64_t count = 0;
    for (const FieldSig& field : fields) {
        count = addNodes(count, nodesOf(field.type));
    }
    return count;
}

// The fields of a declared record or a schema row, null for anything else
const RecordSig* SmallestValues::recordOf(TyId type) const {
    if (lookup_.namedKind(type) != GlobalKind::Record) {
        return nullptr;
    }
    return lookup_.sigs().record(lookup_.types().nameOf(type));
}

// The payload fields of one variant of a sum, null when it carries none
const std::vector<FieldSig>* SmallestValues::payloadOf(const Variant& variant,
                                                      const SumDecl& sum) const {
    const VariantSig* payload = lookup_.sigs().variant(variant.name);
    if (payload == nullptr || payload->owner != sum.name) {
        return nullptr;
    }
    return &payload->fields;
}

}  // namespace geas
