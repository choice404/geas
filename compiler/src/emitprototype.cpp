// How a Geas type crosses the gRPC bridge. A scalar is a proto3 scalar, a
// declared record is a message, a declared sum is an enum when every arm is
// bare and a message with a oneof when an arm carries fields, Unit is the
// empty message, and Option, List, Map, and Tuple are wrapper messages
// named from their element words, IntOption, IntList, StringIntMap,
// IntStringTuple. A definition is emitted the first time a type is needed
// and never again. A type outside the bridged set is a named build error
#include "emitproto.hpp"
#include "emittext.hpp"

namespace geas {

namespace {

// The proto scalar a Map key crosses as. Geas allows six key types and
// every one has a proto scalar
std::string_view mapKeyScalar(TyKind kind) {
    switch (kind) {
        case TyKind::Int: return "int64";
        case TyKind::UInt: return "uint64";
        case TyKind::Bool: return "bool";
        case TyKind::Byte:
        case TyKind::Char: return "uint32";
        case TyKind::String: return "string";
        default: return "";
    }
}

// The word a Map key contributes to a wrapper name
std::string_view mapKeyWord(TyKind kind) {
    switch (kind) {
        case TyKind::Int: return "Int";
        case TyKind::UInt: return "Uint";
        case TyKind::Bool: return "Bool";
        case TyKind::Byte: return "Byte";
        case TyKind::Char: return "Char";
        case TyKind::String: return "String";
        default: return "";
    }
}

std::string_view scalarWord(TyKind kind) {
    switch (kind) {
        case TyKind::Int: return "Int";
        case TyKind::Duration: return "Duration";
        case TyKind::Time: return "Time";
        case TyKind::UInt: return "Uint";
        case TyKind::Float: return "Float";
        case TyKind::Bool: return "Bool";
        case TyKind::String: return "String";
        case TyKind::Bytes: return "Bytes";
        case TyKind::Unit: return "Unit";
        default: return "";
    }
}

}  // namespace

// The proto3 scalar spelling a Geas type crosses the bridge as, empty for a
// type that is not a bridged scalar
std::string_view ProtoEmitter::scalarOf(TyId type) const {
    switch (lookup_.kindOf(type)) {
        // Duration and Time cross as int64, milliseconds and epoch milliseconds
        case TyKind::Int:
        case TyKind::Duration:
        case TyKind::Time: return "int64";
        case TyKind::UInt: return "uint64";
        case TyKind::Float: return "double";
        case TyKind::Bool: return "bool";
        case TyKind::String: return "string";
        case TyKind::Bytes: return "bytes";
        default: return "";
    }
}

std::string ProtoEmitter::tupleWord(const TyNode& node) const {
    std::string out;
    for (const TyId element : node.args) {
        const std::string word = wordOf(element);
        if (word.empty()) {
            return "";
        }
        out.append(word);
    }
    return joined(out, "Tuple");
}

// The Geas spelling that names a result message arm or a wrapper, empty
// for a type the bridge does not name
std::string ProtoEmitter::wordOf(TyId type) const {
    const TyNode node = lookup_.types().node(type);
    const auto wrapped = [&](std::string_view suffix) {
        const std::string inner = wordOf(node.args[0]);
        return inner.empty() ? std::string{} : joined(inner, suffix);
    };
    switch (node.kind) {
        case TyKind::Option: return wrapped("Option");
        case TyKind::List: return wrapped("List");
        case TyKind::Tuple: return tupleWord(node);
        case TyKind::Map: {
            const std::string_view key = mapKeyWord(lookup_.kindOf(node.args[0]));
            const std::string value = wordOf(node.args[1]);
            return key.empty() || value.empty() ? std::string{} : joined(key, value, "Map");
        }
        case TyKind::Named: {
            const std::optional<GlobalKind> kind = lookup_.namedKind(type);
            const bool isDeclared = kind == GlobalKind::Record || kind == GlobalKind::Sum;
            return isDeclared ? node.name : std::string{};
        }
        default: return std::string{scalarWord(node.kind)};
    }
}

// The rpc return message name of one pledge, spelled without emitting or
// recording anything: a declared Result names its oneof message, a declared
// Option names its wrapper. Empty for a return outside the bridged set
std::string ProtoEmitter::returnName(TyId ret) const {
    const TyNode node = lookup_.types().node(ret);
    if (node.kind == TyKind::Bytes) {
        return "BytesValue";
    }
    if (node.kind == TyKind::Option) {
        return wordOf(ret);
    }
    if (node.kind != TyKind::Result) {
        return "";
    }
    const std::string ok = wordOf(node.args[0]);
    const std::string err = wordOf(node.args[1]);
    return ok.empty() || err.empty() ? std::string{} : joined(ok, err, "Result");
}

// Whether a named type is a sum whose every arm is bare, the shape that
// crosses as a proto enum and whose fields track presence with optional
bool ProtoEmitter::isPlainSum(TyId type) const {
    const SumDecl* sum = lookup_.sumOf(type);
    if (sum == nullptr) {
        return false;
    }
    for (const Variant& variant : sum->variants) {
        if (!variant.fields.empty()) {
            return false;
        }
    }
    return true;
}

// Whether proto tracks an Option's presence by the optional label rather
// than by the message that carries it
bool ProtoEmitter::tracksPresence(TyId inner) const {
    return !scalarOf(inner).empty() || isPlainSum(inner);
}

std::string ProtoEmitter::ensureUnit() {
    if (!isSeen("Unit")) {
        seen_.emplace_back("Unit");
        top("// The Ok of a pledge that answers with nothing; the message is the value.");
        top("message Unit {}");
        top("");
    }
    return "Unit";
}

// The proto spelling of a type in a field or oneof arm position, with the
// definition it needs emitted once per file. Empty, and a named build
// error, for a type outside the bridged set
std::string ProtoEmitter::ensureType(TyId type, std::int64_t lo, std::int64_t hi) {
    const std::string_view scalar = scalarOf(type);
    if (!scalar.empty()) {
        return std::string{scalar};
    }
    const TyKind kind = lookup_.kindOf(type);
    if (kind == TyKind::Unit) {
        return ensureUnit();
    }
    if (lookup_.namedKind(type) == GlobalKind::Record) {
        return ensureRecord(lookup_.types().node(type).name, lo, hi);
    }
    if (const SumDecl* sum = lookup_.sumOf(type)) {
        return ensureSum(*sum, lo, hi);
    }
    const std::string word = wordOf(type);
    const bool isWrapper = kind == TyKind::Option || kind == TyKind::List || kind == TyKind::Map ||
                           kind == TyKind::Tuple;
    if (isWrapper && !word.empty() && isSeen(word)) {
        return word;
    }
    if (isWrapper && !word.empty()) {
        switch (kind) {
            case TyKind::Option: return ensureOption(type, lo, hi);
            case TyKind::List: return ensureList(type, lo, hi);
            case TyKind::Map: return ensureMap(type, lo, hi);
            default: return ensureTuple(type, lo, hi);
        }
    }
    lookup_.unsupported(lo, hi,
                        joined("a type on the gRPC surface outside the bridged set: ",
                               lookup_.types().render(type)));
    return "";
}

// Option<T> as a message: an unset value is None
std::string ProtoEmitter::ensureOption(TyId type, std::int64_t lo, std::int64_t hi) {
    const TyId inner = lookup_.types().node(type).args[0];
    const std::string word = wordOf(type);
    const std::string spelling = ensureType(inner, lo, hi);
    if (spelling.empty()) {
        return "";
    }
    seen_.push_back(word);
    top(joined("// Option<", wordOf(inner), "> as a message: an unset value is None."));
    top(joined("message ", word, " {"));
    top(joined(tracksPresence(inner) ? "  optional " : "  ", spelling, " value = 1;"));
    top("}");
    top("");
    return word;
}

// List<T> as a message, so a list can ride a oneof arm
std::string ProtoEmitter::ensureList(TyId type, std::int64_t lo, std::int64_t hi) {
    const TyId element = lookup_.types().node(type).args[0];
    const std::string word = wordOf(type);
    const std::string spelling = ensureType(element, lo, hi);
    if (spelling.empty()) {
        return "";
    }
    seen_.push_back(word);
    top(joined("// List<", wordOf(element), "> as a message, so a list can ride a oneof arm."));
    top(joined("message ", word, " {"));
    top(joined("  repeated ", spelling, " values = 1;"));
    top("}");
    top("");
    return word;
}

// A map as the ordered entry list it actually is: Geas pins map iteration
// as insertion ordered and proto's own map type carries no order
std::string ProtoEmitter::ensureMap(TyId type, std::int64_t lo, std::int64_t hi) {
    const TyNode node = lookup_.types().node(type);
    const std::string word = wordOf(type);
    const std::string_view key = mapKeyScalar(lookup_.kindOf(node.args[0]));
    const std::string value = ensureType(node.args[1], lo, hi);
    if (key.empty() || value.empty()) {
        return "";
    }
    const std::string entry = joined(word, "Entry");
    seen_.push_back(word);
    seen_.push_back(entry);
    top(joined("// The ", lookup_.types().render(type),
               " wrapper: Geas pins map iteration as insertion"));
    top("// ordered and proto's own map type carries no order, so the bridge spells");
    top("// a map as the ordered entry list it actually is.");
    top(joined("message ", entry, " {"));
    top(joined("  ", key, " key = 1;"));
    top(joined("  ", value, " value = 2;"));
    top("}");
    top(joined("message ", word, " {"));
    top(joined("  repeated ", entry, " entries = 1;"));
    top("}");
    top("");
    return word;
}

// A tuple as a message, each element a numbered field
std::string ProtoEmitter::ensureTuple(TyId type, std::int64_t lo, std::int64_t hi) {
    const TyNode node = lookup_.types().node(type);
    const std::string word = wordOf(type);
    std::vector<std::string> lines;
    bool isOk = true;
    for (std::size_t i = 0; i < node.args.size(); ++i) {
        const std::string spelling = ensureType(node.args[i], lo, hi);
        isOk = isOk && !spelling.empty();
        const std::string number = decimal(static_cast<std::int64_t>(i) + 1);
        lines.push_back(joined("  ", spelling, " item", number, " = ", number, ";"));
    }
    if (!isOk) {
        return "";
    }
    seen_.push_back(word);
    top(joined("// The ", lookup_.types().render(type),
               " as a message, each element a numbered field."));
    top(joined("message ", word, " {"));
    for (const std::string& text : lines) {
        top(text);
    }
    top("}");
    top("");
    return word;
}

}  // namespace geas
