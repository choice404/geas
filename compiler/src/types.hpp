// The checked type arena. Every checked type is one hash consed node, so two
// mentions of the same type share one id and comparing types is comparing
// ids. Consing goes through the canonical spelling of a node, and that same
// spelling is what signature strings and shape hashes are built from, so it
// is frozen for a type once a release carries it
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace geas {

enum class TyKind {
    Int,
    UInt,
    Float,
    Bool,
    Byte,
    Char,
    String,
    Unit,
    Duration,
    Time,
    List,
    Map,
    Option,
    Result,
    Tuple,
    Pledge,
    Named,
    Contract,
    Unknown,
};

// The id of a checked type, unset when a position expects nothing
struct TyId {
    std::int32_t index = -1;

    bool isSet() const { return index >= 0; }
    bool operator==(const TyId&) const = default;
};

// One checked type. List and Option keep their element in args[0], Map and
// Result keep two, Tuple keeps all of them, and a Pledge keeps its params in
// args and its return in ret. Named and Contract carry the name
struct TyNode {
    TyKind kind = TyKind::Unknown;
    std::string name;
    std::vector<TyId> args;
    TyId ret;
};

class TypeArena {
public:
    /**
     * Interns a node whose children are already interned.
     * @params node - the type to intern
     * @returns {TyId} - the id of the first node with the same canonical spelling
     */
    TyId intern(TyNode node);

    // The node behind an id. The reference dies with the next intern, so a
    // caller that goes on to build types copies the node first
    const TyNode& node(TyId id) const;
    TyKind kindOf(TyId id) const;

    /**
     * The canonical spelling of a type, the text shape hashes eat.
     * @params id - the type
     * @returns {std::string_view} - the spelling, owned by the arena and good until the next intern
     */
    std::string_view canon(TyId id) const;

    /**
     * The surface spelling of a type, for diagnostics.
     * @params id - the type
     * @returns {std::string} - the spelling as the source would write it
     */
    std::string render(TyId id) const;

    /**
     * Whether one type may stand where the other is expected. Equal types
     * share an id, so the work left is letting the unknown recovery type
     * stand anywhere, inside a composite included.
     * @params a - one type, b - the other
     * @returns {bool} - true when they agree
     */
    bool isCompatible(TyId a, TyId b) const;

    // The name behind a named or contract type, empty for any other shape.
    // Like node, the view dies with the next intern
    std::string_view nameOf(TyId id) const;

    // The makers, one per shape, each interning its node
    TyId scalar(TyKind kind);
    TyId unknown();
    TyId list(TyId element);
    TyId option(TyId inner);
    TyId map(TyId key, TyId value);
    TyId result(TyId ok, TyId err);
    TyId tuple(std::vector<TyId> elements);
    TyId pledge(std::vector<TyId> params, TyId ret);
    TyId named(std::string_view name);
    TyId contract(std::string_view name);

private:
    std::string canonOf(const TyNode& node) const;
    void renderInto(std::string& out, TyId id) const;
    void renderList(std::string& out, const std::vector<TyId>& items) const;
    bool areAllCompatible(const std::vector<TyId>& a, const std::vector<TyId>& b) const;

    std::vector<TyNode> nodes_;
    std::vector<std::string> canons_;
    std::unordered_map<std::string, TyId> ids_;
};

}  // namespace geas
