// The layouts of the declared records and sums a contract boundary reaches.
// A value that crosses the boundary or parks in a vow or a latch is stored
// by the layout of its type, so the shape string and every signature hash
// carry those layouts beside the canonical codes, which name a declared type
// and nothing more.
//
// A type is reached from a root type through its own fields and variant
// payloads and through the type arguments of List, Map, Option, Result, and
// Tuple. Each reached type is spelled once as a segment, in name order:
//
//   type Name{field:code,field:code};           a record, fields in declaration order
//   type Name=Variant(field:code)|Variant;      a sum, variants in declaration order
//
// A variant with no payload is its bare name. A type reached a second time,
// a recursive one included, is not walked again. A name no declaration owns
// is opaque and has no layout to spell. The recorded side reads the same
// spellings back from a recorded shape, so an older version's layouts and
// this version's compare as text
#pragma once

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "amendshape.hpp"
#include "ast.hpp"
#include "sigs.hpp"
#include "types.hpp"

namespace geas {

class LayoutIndex {
public:
    LayoutIndex(const Ast& ast, const Sigs& sigs, const TypeArena& types);

    /**
     * Adds every declared record and sum one type reaches to a name set.
     * @params type - the root type, names - the set the reached names land in
     */
    void reach(TyId type, std::set<std::string>& names) const;

    /**
     * Adds every declared record and sum a signature reaches to a name set.
     * @params sig - the lowered signature, names - the set the reached names land in
     */
    void reachSignature(const CallableSig& sig, std::set<std::string>& names) const;

    /**
     * The layouts of a set of reached names, in name order.
     * @params names - names reach filled
     * @returns {std::vector<TypeLayout>} - one layout per name
     */
    std::vector<TypeLayout> layoutsOf(const std::set<std::string>& names) const;

private:
    void reachNamed(const std::string& name, std::set<std::string>& names) const;
    void reachFields(const std::vector<FieldSig>& fields, std::set<std::string>& names) const;
    std::string recordDefinition(const RecordSig& row) const;
    std::string sumDefinition(const SumDecl& sum) const;
    std::string fieldList(const std::vector<FieldSig>& fields) const;

    const Sigs& sigs_;
    const TypeArena& types_;
    std::unordered_map<std::string, const RecordDecl*> records_;
    std::unordered_map<std::string, const SumDecl*> sums_;
};

/**
 * The first declared type a canonical code reaches whose layout differs
 * between two recorded layout sets, walked through the first set.
 * @params code - a canonical type code or a parenthesized code list, from - the layouts of the
 * older version, to - the layouts of the newer one
 * @returns {std::optional<std::string>} - the name of the changed type, nullopt when every layout
 * the code reaches agrees
 */
std::optional<std::string> changedLayout(std::string_view code,
                                         const std::vector<TypeLayout>& from,
                                         const std::vector<TypeLayout>& to);

}  // namespace geas
