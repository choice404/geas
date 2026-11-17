// The type table of one contract, see typetable.hpp
#include "typetable.hpp"

#include <set>
#include <unordered_map>

#include "emittext.hpp"

namespace geas {

namespace {

constexpr std::string_view tagPrefix = "GEAS_TY_";
constexpr std::string_view recordTag = "GEAS_TY_RECORD";
constexpr std::string_view sumTag = "GEAS_TY_SUM";

// What the table can say about a type: it spells it, a name inside it has
// no layout, or a contract instance or a pledge value sits inside it, which
// the checker keeps out of every boundary position
enum class Spelling { Known, Opaque, Unbound };

Spelling worse(Spelling a, Spelling b) {
    return static_cast<int>(a) > static_cast<int>(b) ? a : b;
}

class TypeTableBuilder {
public:
    explicit TypeTableBuilder(const EmitLookup& lookup) : lookup_{lookup} {}

    ContractTypes run(const ContractShape& shape) &&;

private:
    std::uint32_t slotOf(TyId type, std::int64_t lo, std::int64_t hi);
    TypeTableItem itemOf(const CallableSig& sig, bool isSignal, std::int64_t lo, std::int64_t hi);
    TypeTableItem remedyItemOf(const RemedyShape& remedy);
    Spelling spellingOf(TyId type, std::set<std::string>& seen) const;
    Spelling namedSpelling(TyId type, std::set<std::string>& seen) const;
    Spelling fieldsSpelling(const std::vector<FieldSig>& fields, std::set<std::string>& seen) const;
    const RecordSig* recordOf(TyId type) const;
    std::string keyOf(TyId type) const;
    std::uint32_t indexOf(TyId type);
    std::uint32_t reserve(std::string key, std::string_view tag, std::string name);
    std::vector<std::uint32_t> indicesOf(const std::vector<TyId>& types);
    std::vector<std::uint32_t> fieldIndices(const std::vector<FieldSig>& fields);
    void fillSum(std::uint32_t index, const SumDecl& sum);

    const EmitLookup& lookup_;
    ContractTypes out_;
    std::unordered_map<std::string, std::uint32_t> indices_;
};

// Vows, pledges, signals, remedies: the fixed walk order the indices follow
ContractTypes TypeTableBuilder::run(const ContractShape& shape) && {
    for (const VowDecl* vow : shape.vows) {
        out_.vows.push_back(slotOf(lookup_.vowType(*vow), vow->lo, vow->hi));
    }
    for (const PledgeShape& pledge : shape.pledges) {
        const PledgeDecl& decl = *pledge.decl;
        out_.items.push_back(itemOf(lookup_.pledgeSig(decl), false, decl.lo, decl.hi));
    }
    for (const SignalShape& signal : shape.signals) {
        const SignalDecl& decl = *signal.decl;
        out_.items.push_back(itemOf(lookup_.signalSig(decl), true, decl.lo, decl.hi));
    }
    for (const RemedyShape& remedy : shape.remedies) {
        out_.remedies.push_back(remedyItemOf(remedy));
    }
    return std::move(out_);
}

// The index one slot carries: the type's own, or noTypeIndex when the type
// reaches a name with no layout. An instance or a pledge type is a build fault
std::uint32_t TypeTableBuilder::slotOf(TyId type, std::int64_t lo, std::int64_t hi) {
    std::set<std::string> seen;
    const Spelling spelling = spellingOf(type, seen);
    if (spelling == Spelling::Unbound) {
        lookup_.fault(lo, hi, "codegen: a contract instance or pledge type cannot reach the type table");
        return noTypeIndex;
    }
    if (spelling == Spelling::Opaque) {
        return noTypeIndex;
    }
    return indexOf(type);
}

// A signal's return is no value, so it carries noTypeIndex
TypeTableItem TypeTableBuilder::itemOf(const CallableSig& sig, bool isSignal, std::int64_t lo,
                                       std::int64_t hi) {
    TypeTableItem item;
    for (const TyId param : sig.params) {
        item.params.push_back(slotOf(param, lo, hi));
    }
    item.ret = isSignal ? noTypeIndex : slotOf(sig.ret, lo, hi);
    return item;
}

// A remedy is handed its target's Ok value when it takes one at all
TypeTableItem TypeTableBuilder::remedyItemOf(const RemedyShape& remedy) {
    const RemedyDecl& decl = *remedy.decl;
    TypeTableItem item;
    if (remedyTakesValue(lookup_, remedy)) {
        const TyId targetRet = lookup_.pledgeSig(*remedy.target).ret;
        const TyId okType = lookup_.types().node(targetRet).args[0];
        item.params.push_back(slotOf(okType, decl.lo, decl.hi));
    }
    item.ret = slotOf(lookup_.remedySig(decl).ret, decl.lo, decl.hi);
    return item;
}

Spelling TypeTableBuilder::spellingOf(TyId type, std::set<std::string>& seen) const {
    switch (lookup_.kindOf(type)) {
        case TyKind::Unknown: return Spelling::Opaque;
        case TyKind::Contract:
        case TyKind::Pledge: return Spelling::Unbound;
        case TyKind::Named: return namedSpelling(type, seen);
        case TyKind::List:
        case TyKind::Option:
        case TyKind::Map:
        case TyKind::Result:
        case TyKind::Tuple: break;
        default: return Spelling::Known;
    }
    Spelling spelling = Spelling::Known;
    const std::vector<TyId> args = lookup_.types().node(type).args;
    for (const TyId arg : args) {
        spelling = worse(spelling, spellingOf(arg, seen));
    }
    return spelling;
}

// A name met again is already being judged further up the walk, so it adds
// nothing; a name neither a record, a schema row, nor a sum is opaque
Spelling TypeTableBuilder::namedSpelling(TyId type, std::set<std::string>& seen) const {
    const std::string name{lookup_.types().nameOf(type)};
    if (!seen.insert(name).second) {
        return Spelling::Known;
    }
    if (const RecordSig* record = recordOf(type)) {
        return fieldsSpelling(record->fields, seen);
    }
    const SumDecl* sum = lookup_.sumOf(type);
    if (sum == nullptr) {
        return Spelling::Opaque;
    }
    Spelling spelling = Spelling::Known;
    for (const Variant& variant : sum->variants) {
        const VariantSig* payload = lookup_.sigs().variant(variant.name);
        if (payload != nullptr && payload->owner == name) {
            spelling = worse(spelling, fieldsSpelling(payload->fields, seen));
        }
    }
    return spelling;
}

Spelling TypeTableBuilder::fieldsSpelling(const std::vector<FieldSig>& fields,
                                          std::set<std::string>& seen) const {
    Spelling spelling = Spelling::Known;
    for (const FieldSig& field : fields) {
        spelling = worse(spelling, spellingOf(field.type, seen));
    }
    return spelling;
}

// The fields of a declared record or a schema row, null for anything else
const RecordSig* TypeTableBuilder::recordOf(TyId type) const {
    if (lookup_.namedKind(type) != GlobalKind::Record) {
        return nullptr;
    }
    return lookup_.sigs().record(lookup_.types().nameOf(type));
}

// The identity a node is shared under: the type as a conforming value sees
// it. Duration and Time are Int, and a declared name stands for its
// declaration, so two types the runtime cannot tell apart share one node
std::string TypeTableBuilder::keyOf(TyId type) const {
    const TyNode& node = lookup_.types().node(type);
    if (node.kind == TyKind::Named) {
        return joined("N", node.name, ";");
    }
    std::string key{lookup_.tagOf(type)};
    if (node.args.empty()) {
        return key;
    }
    const std::vector<TyId> args = node.args;
    key.push_back('(');
    for (const TyId arg : args) {
        key.append(keyOf(arg));
        key.push_back(',');
    }
    return key + ")";
}

// The index of a type the table spells, assigned before its kids are walked
// so a recursive type finds its own index on the way back
std::uint32_t TypeTableBuilder::indexOf(TyId type) {
    std::string key = keyOf(type);
    const auto found = indices_.find(key);
    if (found != indices_.end()) {
        return found->second;
    }
    const TyNode node = lookup_.types().node(type);
    if (const RecordSig* record = recordOf(type)) {
        const std::uint32_t index = reserve(std::move(key), recordTag, node.name);
        out_.nodes[index].kids = fieldIndices(record->fields);
        return index;
    }
    if (const SumDecl* sum = lookup_.sumOf(type)) {
        const std::uint32_t index = reserve(std::move(key), sumTag, node.name);
        fillSum(index, *sum);
        return index;
    }
    const std::uint32_t index = reserve(std::move(key), lookup_.tagOf(type), "");
    out_.nodes[index].kids = indicesOf(node.args);
    return index;
}

std::uint32_t TypeTableBuilder::reserve(std::string key, std::string_view tag, std::string name) {
    const auto index = static_cast<std::uint32_t>(out_.nodes.size());
    out_.nodes.push_back(TypeTableNode{tag, {}, std::move(name), {}});
    indices_.emplace(std::move(key), index);
    return index;
}

std::vector<std::uint32_t> TypeTableBuilder::indicesOf(const std::vector<TyId>& types) {
    std::vector<std::uint32_t> indices;
    for (const TyId type : types) {
        indices.push_back(indexOf(type));
    }
    return indices;
}

std::vector<std::uint32_t> TypeTableBuilder::fieldIndices(const std::vector<FieldSig>& fields) {
    std::vector<std::uint32_t> indices;
    for (const FieldSig& field : fields) {
        indices.push_back(indexOf(field.type));
    }
    return indices;
}

// Variants in declaration order, the order their tags are numbered in. The
// node is reached through its index each time, since walking a payload may
// grow the table under it
void TypeTableBuilder::fillSum(std::uint32_t index, const SumDecl& sum) {
    for (const Variant& variant : sum.variants) {
        const VariantSig* payload = lookup_.sigs().variant(variant.name);
        const bool hasPayload = payload != nullptr && payload->owner == sum.name;
        std::vector<std::uint32_t> fields =
            hasPayload ? fieldIndices(payload->fields) : std::vector<std::uint32_t>{};
        out_.nodes[index].variants.push_back(TypeTableVariant{variant.name, std::move(fields)});
    }
}

}  // namespace

bool remedyTakesValue(const EmitLookup& lookup, const RemedyShape& remedy) {
    if (!remedy.decl->param || remedy.target == nullptr) {
        return false;
    }
    const TyId ret = lookup.pledgeSig(*remedy.target).ret;
    if (lookup.kindOf(ret) != TyKind::Result) {
        return false;
    }
    return lookup.kindOf(lookup.types().node(ret).args[0]) != TyKind::Unit;
}

ContractTypes buildContractTypes(const EmitLookup& lookup, const ContractShape& shape) {
    return TypeTableBuilder{lookup}.run(shape);
}

std::string_view shortTagName(std::string_view tag) {
    if (tag.starts_with(tagPrefix)) {
        tag.remove_prefix(tagPrefix.size());
    }
    return tag;
}

}  // namespace geas
