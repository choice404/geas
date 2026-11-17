// The checked type arena, see types.hpp
#include "types.hpp"

#include <utility>

namespace geas {

namespace {

// Each leaf has its own canonical code. Bytes takes X, distinct from String
// and every earlier spelling, so types without Bytes keep their canonical text
std::string_view leafCode(TyKind kind) {
    switch (kind) {
        case TyKind::Int: return "I";
        case TyKind::UInt: return "U";
        case TyKind::Float: return "F";
        case TyKind::Bool: return "B";
        case TyKind::Byte: return "Y";
        case TyKind::Char: return "C";
        case TyKind::String: return "S";
        case TyKind::Bytes: return "X";
        case TyKind::Unit: return "V";
        case TyKind::Duration: return "D";
        case TyKind::Time: return "W";
        default: return "?";
    }
}

// The surface name of a leaf type, empty for a composite
std::string_view leafName(TyKind kind) {
    switch (kind) {
        case TyKind::Int: return "Int";
        case TyKind::UInt: return "UInt";
        case TyKind::Float: return "Float";
        case TyKind::Bool: return "Bool";
        case TyKind::Byte: return "Byte";
        case TyKind::Char: return "Char";
        case TyKind::String: return "String";
        case TyKind::Bytes: return "Bytes";
        case TyKind::Unit: return "Unit";
        case TyKind::Duration: return "Duration";
        case TyKind::Time: return "Time";
        case TyKind::Unknown: return "_";
        default: return "";
    }
}

}  // namespace

TyId TypeArena::intern(TyNode node) {
    std::string spelling = canonOf(node);
    const auto found = ids_.find(spelling);
    if (found != ids_.end()) {
        return found->second;
    }
    const TyId id{static_cast<std::int32_t>(nodes_.size())};
    nodes_.push_back(std::move(node));
    canons_.push_back(spelling);
    ids_.emplace(std::move(spelling), id);
    return id;
}

const TyNode& TypeArena::node(TyId id) const {
    return nodes_[static_cast<std::size_t>(id.index)];
}

TyKind TypeArena::kindOf(TyId id) const {
    return node(id).kind;
}

std::string_view TypeArena::canon(TyId id) const {
    return canons_[static_cast<std::size_t>(id.index)];
}

std::string_view TypeArena::nameOf(TyId id) const {
    const TyNode& type = node(id);
    if (type.kind == TyKind::Named || type.kind == TyKind::Contract) {
        return type.name;
    }
    return "";
}

// The spelling is injective per shape, so string equality is node equality
std::string TypeArena::canonOf(const TyNode& type) const {
    const auto joined = [&](std::string_view open) {
        std::string out{open};
        for (std::size_t i = 0; i < type.args.size(); ++i) {
            if (i > 0) {
                out.push_back(',');
            }
            out.append(canon(type.args[i]));
        }
        out.push_back(')');
        return out;
    };
    switch (type.kind) {
        case TyKind::List: return joined("L(");
        case TyKind::Option: return joined("O(");
        case TyKind::Map: return joined("M(");
        case TyKind::Result: return joined("R(");
        case TyKind::Tuple: return joined("T(");
        case TyKind::Pledge: return joined("P(") + "->" + std::string{canon(type.ret)};
        case TyKind::Named: return "N" + type.name;
        case TyKind::Contract: return "K" + type.name;
        default: return std::string{leafCode(type.kind)};
    }
}

std::string TypeArena::render(TyId id) const {
    std::string out;
    renderInto(out, id);
    return out;
}

void TypeArena::renderList(std::string& out, const std::vector<TyId>& items) const {
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            out.append(", ");
        }
        renderInto(out, items[i]);
    }
}

void TypeArena::renderInto(std::string& out, TyId id) const {
    const TyNode& type = node(id);
    const auto generic = [&](std::string_view name) {
        out.append(name);
        out.push_back('<');
        renderList(out, type.args);
        out.push_back('>');
    };
    switch (type.kind) {
        case TyKind::List: return generic("List");
        case TyKind::Map: return generic("Map");
        case TyKind::Option: return generic("Option");
        case TyKind::Result: return generic("Result");
        case TyKind::Tuple: return generic("Tuple");
        case TyKind::Pledge:
            out.append("pledge(");
            renderList(out, type.args);
            out.append(") -> ");
            return renderInto(out, type.ret);
        case TyKind::Named:
        case TyKind::Contract: out.append(type.name); return;
        default: out.append(leafName(type.kind)); return;
    }
}

bool TypeArena::areAllCompatible(const std::vector<TyId>& a, const std::vector<TyId>& b) const {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!isCompatible(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

bool TypeArena::isCompatible(TyId a, TyId b) const {
    if (a == b) {
        return true;
    }
    const TyNode& left = node(a);
    const TyNode& right = node(b);
    if (left.kind == TyKind::Unknown || right.kind == TyKind::Unknown) {
        return true;
    }
    if (left.kind != right.kind) {
        return false;
    }
    switch (left.kind) {
        case TyKind::List:
        case TyKind::Option:
        case TyKind::Map:
        case TyKind::Result:
        case TyKind::Tuple: return areAllCompatible(left.args, right.args);
        case TyKind::Pledge:
            return areAllCompatible(left.args, right.args) && isCompatible(left.ret, right.ret);
        default: return false;
    }
}

TyId TypeArena::scalar(TyKind kind) {
    return intern(TyNode{kind, "", {}, {}});
}

TyId TypeArena::unknown() {
    return scalar(TyKind::Unknown);
}

TyId TypeArena::list(TyId element) {
    return intern(TyNode{TyKind::List, "", {element}, {}});
}

TyId TypeArena::option(TyId inner) {
    return intern(TyNode{TyKind::Option, "", {inner}, {}});
}

TyId TypeArena::map(TyId key, TyId value) {
    return intern(TyNode{TyKind::Map, "", {key, value}, {}});
}

TyId TypeArena::result(TyId ok, TyId err) {
    return intern(TyNode{TyKind::Result, "", {ok, err}, {}});
}

TyId TypeArena::tuple(std::vector<TyId> elements) {
    return intern(TyNode{TyKind::Tuple, "", std::move(elements), {}});
}

TyId TypeArena::pledge(std::vector<TyId> params, TyId ret) {
    return intern(TyNode{TyKind::Pledge, "", std::move(params), ret});
}

TyId TypeArena::named(std::string_view name) {
    return intern(TyNode{TyKind::Named, std::string{name}, {}, {}});
}

TyId TypeArena::contract(std::string_view name) {
    return intern(TyNode{TyKind::Contract, std::string{name}, {}, {}});
}

}  // namespace geas
