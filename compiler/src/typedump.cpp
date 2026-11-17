// The listing behind geas dump-types, see typetable.hpp. One block per
// contract that is not internal:
//
//   // types Name: N nodes
//   0 RECORD Node(1, 2)          a node, its index, tag, name, and kids
//   3 SUM Tree                   a sum, then one line per variant
//     Leaf
//     Branch(3, 3)
//   pledge walk(3) -> 4          an item, its parameter and return indices
//   signal moved(1) -> none      none is GEAS_NO_TYPE
//   vow limit 1
//   remedy undo(0) -> 5
#include "emittext.hpp"
#include "typetable.hpp"

namespace geas {

namespace {

std::string indexText(std::uint32_t index) {
    return index == noTypeIndex ? std::string{"none"} : decimal(index);
}

// Indices in parentheses, empty parentheses for none
std::string parenthesized(const std::vector<std::uint32_t>& indices) {
    std::string out = "(";
    for (std::size_t i = 0; i < indices.size(); ++i) {
        out.append(i > 0 ? ", " : "");
        out.append(indexText(indices[i]));
    }
    return out + ")";
}

void appendNode(std::string& out, std::size_t index, const TypeTableNode& node) {
    out.append(joined(decimal(static_cast<std::int64_t>(index)), " ", shortTagName(node.tag)));
    if (!node.name.empty()) {
        out.append(joined(" ", node.name));
    }
    if (!node.kids.empty()) {
        out.append(parenthesized(node.kids));
    }
    out.push_back('\n');
    for (const TypeTableVariant& variant : node.variants) {
        out.append(joined("  ", variant.name));
        if (!variant.fields.empty()) {
            out.append(parenthesized(variant.fields));
        }
        out.push_back('\n');
    }
}

void appendItem(std::string& out, std::string_view kind, std::string_view name,
                const TypeTableItem& item) {
    out.append(joined(kind, " ", name, parenthesized(item.params), " -> ", indexText(item.ret),
                      "\n"));
}

void appendItems(std::string& out, const ContractShape& shape, const ContractTypes& types) {
    for (std::size_t i = 0; i < shape.pledges.size(); ++i) {
        appendItem(out, "pledge", shape.pledges[i].decl->name, types.items[i]);
    }
    for (std::size_t i = 0; i < shape.signals.size(); ++i) {
        appendItem(out, "signal", shape.signals[i].decl->name,
                   types.items[shape.pledges.size() + i]);
    }
    for (std::size_t i = 0; i < shape.vows.size(); ++i) {
        out.append(joined("vow ", shape.vows[i]->name, " ", indexText(types.vows[i]), "\n"));
    }
    for (std::size_t i = 0; i < shape.remedies.size(); ++i) {
        appendItem(out, "remedy", shape.remedies[i].decl->name, types.remedies[i]);
    }
}

}  // namespace

std::expected<std::string, std::vector<Diagnostic>> dumpTypes(CheckedProgram& checked) {
    std::vector<Diagnostic> faults;
    const EmitLookup lookup{checked, faults};
    std::string out;
    for (const ContractShape* shape : lookup.publicContracts()) {
        const ContractTypes types = buildContractTypes(lookup, *shape);
        out.append(joined("// types ", shape->decl->name, ": ",
                          decimal(static_cast<std::int64_t>(types.nodes.size())), " nodes\n"));
        for (std::size_t i = 0; i < types.nodes.size(); ++i) {
            appendNode(out, i, types.nodes[i]);
        }
        appendItems(out, *shape, types);
    }
    if (!faults.empty()) {
        return std::unexpected(std::move(faults));
    }
    return out;
}

}  // namespace geas
