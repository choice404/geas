// The recorded form of a contract shape, see recordShape in shape.hpp
#include "shape.hpp"

namespace geas {

namespace {

template <typename Map, typename Key>
CallableSig sigOf(const Map& map, const Key* key) {
    const auto found = map.find(key);
    return found == map.end() ? CallableSig{} : found->second;
}

// The canonical code of a type, the unknown code for a type never lowered,
// which only a faulted tree leaves behind
std::string codeOf(const TypeArena& types, TyId type) {
    constexpr std::string_view unknownCode = "?";
    return type.isSet() ? std::string{types.canon(type)} : std::string{unknownCode};
}

// What a fulfilled pledge stores: the T of a Result<T, E>, otherwise the
// whole return, an Option, Unit, or Bytes
std::string okCode(const TypeArena& types, TyId ret) {
    if (!ret.isSet() || types.kindOf(ret) != TyKind::Result) {
        return codeOf(types, ret);
    }
    return codeOf(types, types.node(ret).args[0]);
}

// A signal's payload, its parameter types as one parenthesized list
std::string payloadCode(const TypeArena& types, const CallableSig& sig) {
    std::string out{"("};
    for (std::size_t i = 0; i < sig.params.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        out.append(codeOf(types, sig.params[i]));
    }
    out.push_back(')');
    return out;
}

void recordItems(const ContractShape& shape, const Sigs& sigs, const TypeArena& types,
                 RecordedShape& recorded) {
    for (const PledgeShape& pledge : shape.pledges) {
        const CallableSig sig = sigOf(sigs.pledges, pledge.decl);
        recorded.items.push_back(
            RecordedItem{pledge.decl->name, false, pledge.signature, okCode(types, sig.ret)});
    }
    for (const SignalShape& signal : shape.signals) {
        const CallableSig sig = sigOf(sigs.signals, signal.decl);
        recorded.items.push_back(
            RecordedItem{signal.decl->name, true, "", payloadCode(types, sig)});
    }
}

}  // namespace

RecordedShape recordShape(const ContractShape& shape, const Sigs& sigs, const TypeArena& types,
                          std::uint64_t courseHash) {
    RecordedShape recorded;
    recorded.contract = shape.decl->name;
    recorded.version = shape.version;
    recorded.shapeHash = shape.shapeHash;
    recorded.courseHash = courseHash;
    recorded.shape = shape.shape;
    for (const VowDecl* vow : shape.vows) {
        const auto sig = sigs.vows.find(vow);
        const TyId type = sig == sigs.vows.end() ? TyId{} : sig->second.type;
        recorded.vows.push_back(RecordedVow{vow->name, codeOf(types, type)});
    }
    recordItems(shape, sigs, types, recorded);
    for (const SubcontractDecl* sub : shape.subs) {
        recorded.subs.push_back(sub->name.value_or(""));
    }
    for (const RemedyShape& remedy : shape.remedies) {
        recorded.remedies.push_back(RecordedRemedy{remedy.decl->name, remedy.decl->target});
    }
    recorded.layouts = shape.layouts;
    return recorded;
}

}  // namespace geas
