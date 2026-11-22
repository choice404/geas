// The type table of one contract's descriptor, see typetable.hpp for what it
// holds. The kid and payload index arrays land first, then the variant
// tables of the sums, then the node table, then the parameter arrays and the
// item, vow, and remedy tables, and last the course slot table. Every
// contract with a pledge, a signal, a vow, a remedy, or a course slot
// carries the table; one with none of them emits no array
// and names ntypes 0 alone, so its descriptor still reads as carrying no
// table and still ends on a designated field, every field it leaves out
// zero filled by name rather than by position
#include "emitc.hpp"
#include "emittext.hpp"

namespace geas {

namespace {

constexpr std::string_view nullText = "NULL";
constexpr std::string_view noTypeText = "GEAS_NO_TYPE";

std::string countOf(std::size_t count) {
    return decimal(static_cast<std::int64_t>(count));
}

std::string indexText(std::uint32_t index) {
    return index == noTypeIndex ? std::string{noTypeText} : decimal(index);
}

// Indices as one brace list, { 0, 1, 2 }
std::string indexList(const std::vector<std::uint32_t>& indices) {
    std::string out = "{ ";
    for (std::size_t i = 0; i < indices.size(); ++i) {
        out.append(i > 0 ? ", " : "");
        out.append(indexText(indices[i]));
    }
    return out + " }";
}

bool hasTypedItems(const ContractShape& shape, const CheckTables& tables) {
    const auto course = tables.courses.find(shape.decl);
    const bool hasSlots = course != tables.courses.end() && !course->second.slots.empty();
    return !shape.pledges.empty() || !shape.signals.empty() || !shape.vows.empty() ||
           !shape.remedies.empty() || hasSlots;
}

}  // namespace

// One uint32_t array per node with kids, named by the node's index
void CEmitter::emitTypeKids(std::string_view stem, const ContractTypes& types) {
    bool hasArray = false;
    for (std::size_t i = 0; i < types.nodes.size(); ++i) {
        const TypeTableNode& node = types.nodes[i];
        if (node.kids.empty()) {
            continue;
        }
        top(joined("static const uint32_t ", stem, "_type_kids_", countOf(i), "[] = ",
                   indexList(node.kids), ";"));
        hasArray = true;
    }
    if (hasArray) {
        top("");
    }
}

/**
 * Emits the payload arrays and the variant table of one sum node.
 * @params stem - the contract's symbol stem, index - the node's index, node - the sum node
 * @returns {std::string} - the variant table's symbol, NULL for a node with no variants
 */
std::string CEmitter::emitTypeVariants(std::string_view stem, std::size_t index,
                                       const TypeTableNode& node) {
    if (node.variants.empty()) {
        return std::string{nullText};
    }
    const std::string nodeStem = joined(stem, "_type_fields_", countOf(index), "_");
    std::vector<std::string> rows;
    for (std::size_t v = 0; v < node.variants.size(); ++v) {
        const TypeTableVariant& variant = node.variants[v];
        std::string fields{nullText};
        if (!variant.fields.empty()) {
            fields = joined(nodeStem, countOf(v));
            top(joined("static const uint32_t ", fields, "[] = ", indexList(variant.fields), ";"));
        }
        rows.push_back(joined("    { \"", variant.name, "\", ", countOf(variant.fields.size()),
                              ", ", fields, " },"));
    }
    const std::string symbol = joined(stem, "_type_variants_", countOf(index));
    top(joined("static const GeasTypeVariant ", symbol, "[] = {"));
    for (const std::string& row : rows) {
        top(row);
    }
    top("};");
    top("");
    return symbol;
}

// The node table, each row tag, kid count, kids, name, variant count, variants
std::string CEmitter::emitTypeNodes(std::string_view stem, const ContractTypes& types) {
    if (types.nodes.empty()) {
        return std::string{nullText};
    }
    emitTypeKids(stem, types);
    std::vector<std::string> rows;
    for (std::size_t i = 0; i < types.nodes.size(); ++i) {
        const TypeTableNode& node = types.nodes[i];
        const std::string variants = emitTypeVariants(stem, i, node);
        const std::string kids =
            node.kids.empty() ? std::string{nullText} : joined(stem, "_type_kids_", countOf(i));
        const std::string name =
            node.name.empty() ? std::string{nullText} : joined("\"", node.name, "\"");
        rows.push_back(joined("    { ", node.tag, ", ", countOf(node.kids.size()), ", ", kids,
                              ", ", name, ", ", countOf(node.variants.size()), ", ", variants,
                              " },"));
    }
    const std::string symbol = joined(stem, "_types");
    top(joined("static const GeasTypeNode ", symbol, "[] = {"));
    for (const std::string& row : rows) {
        top(row);
    }
    top("};");
    top("");
    return symbol;
}

/**
 * Emits one GeasItemTypes table and the parameter arrays it points at.
 * @params stem - the stem its symbols are named from, items - its entries in order
 * @returns {std::string} - the table's symbol, NULL for no entries
 */
std::string CEmitter::emitItemTypes(std::string_view stem,
                                    const std::vector<TypeTableItem>& items) {
    if (items.empty()) {
        return std::string{nullText};
    }
    std::vector<std::string> rows;
    bool hasParams = false;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const TypeTableItem& item = items[i];
        std::string params{nullText};
        if (!item.params.empty()) {
            params = joined(stem, "_params_", countOf(i));
            top(joined("static const uint32_t ", params, "[] = ", indexList(item.params), ";"));
            hasParams = true;
        }
        rows.push_back(joined("    { ", countOf(item.params.size()), ", ", params, ", ",
                              indexText(item.ret), " },"));
    }
    if (hasParams) {
        top("");
    }
    const std::string symbol = joined(stem, "_types");
    top(joined("static const GeasItemTypes ", symbol, "[] = {"));
    for (const std::string& row : rows) {
        top(row);
    }
    top("};");
    top("");
    return symbol;
}

std::string CEmitter::emitVowTypes(std::string_view symbol, const std::vector<std::uint32_t>& vows) {
    if (vows.empty()) {
        return std::string{nullText};
    }
    top(joined("static const uint32_t ", symbol, "[] = ", indexList(vows), ";"));
    top("");
    return std::string{symbol};
}

/**
 * Emits the type table of one contract and every array it points at.
 * @params shape - the contract
 * @returns {std::string} - the descriptor lines naming the tables, ntypes 0 alone for a contract
 * with no pledge, signal, vow, remedy, or course slot
 */
std::string CEmitter::emitTypeTables(const ContractShape& shape) {
    if (!hasTypedItems(shape, lookup_.tables())) {
        return "    .ntypes = 0,";
    }
    const ContractTypes types = buildContractTypes(lookup_, shape);
    const std::string stem = contractStem(shape.decl->name);
    const std::string nodes = emitTypeNodes(stem, types);
    const std::string items = emitItemTypes(joined(stem, "_item"), types.items);
    const std::string vows = emitVowTypes(joined(stem, "_vow_types"), types.vows);
    const std::string remedies = emitItemTypes(joined(stem, "_remedy"), types.remedies);
    const std::string slots = emitVowTypes(joined(stem, "_course_slot_types"), types.courseSlots);
    const std::string slotLine =
        types.courseSlots.empty() ? "" : joined("\n    .course_slot_types = ", slots, ",");
    return joined("    .ntypes = ", countOf(types.nodes.size()), ", .types = ", nodes,
                  ",\n    .item_types = ", items, ", .vow_types = ", vows,
                  ",\n    .remedy_types = ", remedies, ",", slotLine);
}

}  // namespace geas
