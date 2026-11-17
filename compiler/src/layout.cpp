// The layouts a contract boundary reaches, the live side, see layout.hpp
#include "layout.hpp"

namespace geas {

namespace {

// The code a field spells when its type never lowered, which only a faulted
// tree leaves behind
constexpr std::string_view unknownCode = "?";

}  // namespace

LayoutIndex::LayoutIndex(const Ast& ast, const Sigs& sigs, const TypeArena& types)
    : sigs_{sigs}, types_{types} {
    for (const Decl& decl : ast.decls) {
        if (const auto* record = std::get_if<RecordDecl>(&decl)) {
            records_.emplace(record->name, record);
        } else if (const auto* sum = std::get_if<SumDecl>(&decl)) {
            sums_.emplace(sum->name, sum);
        }
    }
}

void LayoutIndex::reach(TyId type, std::set<std::string>& names) const {
    if (!type.isSet()) {
        return;
    }
    const TyNode& node = types_.node(type);
    switch (node.kind) {
        case TyKind::Named: return reachNamed(node.name, names);
        case TyKind::List:
        case TyKind::Option:
        case TyKind::Map:
        case TyKind::Result:
        case TyKind::Tuple:
            for (const TyId arg : node.args) {
                reach(arg, names);
            }
            return;
        case TyKind::Pledge:
            for (const TyId arg : node.args) {
                reach(arg, names);
            }
            return reach(node.ret, names);
        default: return;
    }
}

void LayoutIndex::reachSignature(const CallableSig& sig, std::set<std::string>& names) const {
    for (const TyId param : sig.params) {
        reach(param, names);
    }
    reach(sig.ret, names);
}

// A declared record or sum lands in the set once and its fields are walked
// then; an opaque name has no layout and nothing to walk
void LayoutIndex::reachNamed(const std::string& name, std::set<std::string>& names) const {
    if (records_.contains(name)) {
        const RecordSig* row = sigs_.record(name);
        if (row != nullptr && names.insert(name).second) {
            reachFields(row->fields, names);
        }
        return;
    }
    const auto sum = sums_.find(name);
    if (sum == sums_.end() || !names.insert(name).second) {
        return;
    }
    for (const Variant& variant : sum->second->variants) {
        const VariantSig* payload = sigs_.variant(variant.name);
        if (payload != nullptr && payload->owner == name) {
            reachFields(payload->fields, names);
        }
    }
}

void LayoutIndex::reachFields(const std::vector<FieldSig>& fields,
                              std::set<std::string>& names) const {
    for (const FieldSig& field : fields) {
        reach(field.type, names);
    }
}

// The fields as name:code pairs, comma separated
std::string LayoutIndex::fieldList(const std::vector<FieldSig>& fields) const {
    std::string out;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        const TyId type = fields[i].type;
        out.append(fields[i].name + ":");
        out.append(type.isSet() ? types_.canon(type) : unknownCode);
    }
    return out;
}

std::string LayoutIndex::recordDefinition(const RecordSig& row) const {
    return "{" + fieldList(row.fields) + "}";
}

// Variants in declaration order, the order their tags are numbered in
std::string LayoutIndex::sumDefinition(const SumDecl& sum) const {
    std::string out{"="};
    for (std::size_t i = 0; i < sum.variants.size(); ++i) {
        if (i > 0) {
            out.push_back('|');
        }
        const Variant& variant = sum.variants[i];
        out.append(variant.name);
        const VariantSig* payload = sigs_.variant(variant.name);
        const bool hasPayload =
            payload != nullptr && payload->owner == sum.name && !payload->fields.empty();
        if (hasPayload) {
            out.append("(" + fieldList(payload->fields) + ")");
        }
    }
    return out;
}

std::vector<TypeLayout> LayoutIndex::layoutsOf(const std::set<std::string>& names) const {
    std::vector<TypeLayout> layouts;
    for (const std::string& name : names) {
        if (records_.contains(name)) {
            const RecordSig* row = sigs_.record(name);
            layouts.push_back(TypeLayout{name, row == nullptr ? "{}" : recordDefinition(*row)});
            continue;
        }
        const auto sum = sums_.find(name);
        if (sum != sums_.end()) {
            layouts.push_back(TypeLayout{name, sumDefinition(*sum->second)});
        }
    }
    return layouts;
}

}  // namespace geas
