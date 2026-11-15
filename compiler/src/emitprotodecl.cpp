// The declared types on the gRPC surface: a record as a message with its
// fields in declaration order, a sum as an enum when every arm is bare and
// as a message with a oneof over one payload message per arm when an arm
// carries fields, and the field line both are built from
#include "emitproto.hpp"
#include "emittext.hpp"

namespace geas {

namespace {

// CamelCase to SCREAMING_SNAKE, the enum value spelling proto reserves for
// its shared value namespace
std::string screamingSnake(std::string_view name) {
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c >= 'A' && c <= 'Z' && i > 0) {
            out.push_back('_');
        }
        out.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
    }
    return out;
}

// CamelCase to snake_case, the oneof field spelling of a sum's arm
std::string snakeCase(std::string_view name) {
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool isUpper = c >= 'A' && c <= 'Z';
        if (isUpper && i > 0) {
            out.push_back('_');
        }
        out.push_back(isUpper ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

}  // namespace

// One message field: the label a container needs, the spelling, the name,
// and the number. A List field is repeated in place, an Option field is
// optional where proto tracks presence by label and plain where a message
// carries presence itself. Empty when the type is refused
std::string ProtoEmitter::fieldLine(std::string_view name, TyId type, std::int64_t number,
                                    std::int64_t lo, std::int64_t hi) {
    const TyNode node = lookup_.types().node(type);
    std::string_view label = "  ";
    TyId carried = type;
    if (node.kind == TyKind::List) {
        label = "  repeated ";
        carried = node.args[0];
    } else if (node.kind == TyKind::Option) {
        carried = node.args[0];
    }
    const std::string spelling = ensureType(carried, lo, hi);
    if (spelling.empty()) {
        return "";
    }
    if (node.kind == TyKind::Option && tracksPresence(carried)) {
        label = "  optional ";
    }
    return joined(label, spelling, " ", name, " = ", decimal(number), ";");
}

// Builds the field lines of a record or a sum arm, ensuring each field's
// type on the way. Returns whether every field was bridged
bool ProtoEmitter::buildFieldLines(const std::vector<FieldSig>& fields, std::int64_t lo,
                                   std::int64_t hi, std::vector<std::string>& lines) {
    bool isOk = true;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        lines.push_back(
            fieldLine(fields[i].name, fields[i].type, static_cast<std::int64_t>(i) + 1, lo, hi));
        isOk = isOk && !lines.back().empty();
    }
    return isOk;
}

// A declared record as a message, its fields in declaration order, emitted
// once per file under its own name
std::string ProtoEmitter::ensureRecord(std::string_view name, std::int64_t lo, std::int64_t hi) {
    const std::string record{name};
    if (isSeen(record)) {
        return record;
    }
    const RecordSig* sig = lookup_.sigs().record(record);
    if (sig == nullptr) {
        lookup_.unsupported(lo, hi, joined("an undeclared record on the gRPC surface: ", record));
        return "";
    }
    seen_.push_back(record);
    std::vector<std::string> lines;
    if (!buildFieldLines(sig->fields, lo, hi, lines)) {
        return "";
    }
    top(joined("// The declared record ", record, ", fields in declaration order."));
    top(joined("message ", record, " {"));
    for (const std::string& text : lines) {
        top(text);
    }
    top("}");
    top("");
    return record;
}

// Every arm bare: the sum crosses as an enum numbered in declared arm
// order, the ABI's own tag space
void ProtoEmitter::emitPlainSum(const SumDecl& sum) {
    top(joined("// The declared sum ", sum.name, ": every arm is bare, so it crosses as an"));
    top("// enum whose numbering is the declared arm order, the ABI's own tag space.");
    top(joined("enum ", sum.name, " {"));
    const std::string prefix = screamingSnake(sum.name);
    for (std::size_t i = 0; i < sum.variants.size(); ++i) {
        top(joined("  ", prefix, "_", screamingSnake(sum.variants[i].name), " = ",
                   decimal(static_cast<std::int64_t>(i)), ";"));
    }
    top("}");
    top("");
}

// One payload message per arm of a sum that carries fields. Returns whether
// every arm's fields were bridged
bool ProtoEmitter::emitArmMessages(const SumDecl& sum, std::int64_t lo, std::int64_t hi) {
    bool isOk = true;
    for (const Variant& variant : sum.variants) {
        const std::string message = joined(sum.name, variant.name);
        if (isSeen(message)) {
            continue;
        }
        seen_.push_back(message);
        const VariantSig* sig = lookup_.sigs().variant(variant.name);
        std::vector<std::string> lines;
        if (sig != nullptr) {
            isOk = buildFieldLines(sig->fields, lo, hi, lines) && isOk;
        }
        top(joined("message ", message, lines.empty() ? " {}" : " {"));
        for (const std::string& text : lines) {
            top(text);
        }
        if (!lines.empty()) {
            top("}");
        }
        top("");
    }
    return isOk;
}

// A declared sum, emitted once per file. An arm with fields makes the sum a
// message whose oneof carries one message per arm, the payload messages
// first, the oneof numbered in the same arm order
std::string ProtoEmitter::ensureSum(const SumDecl& sum, std::int64_t lo, std::int64_t hi) {
    if (isSeen(sum.name)) {
        return sum.name;
    }
    bool isPlain = true;
    for (const Variant& variant : sum.variants) {
        isPlain = isPlain && variant.fields.empty();
    }
    seen_.push_back(sum.name);
    if (isPlain) {
        emitPlainSum(sum);
        return sum.name;
    }
    if (!emitArmMessages(sum, lo, hi)) {
        return "";
    }
    top(joined("// The declared sum ", sum.name, ": an arm carries fields, so it crosses as a"));
    top("// message whose oneof numbering is the declared arm order, the ABI's tag space.");
    top(joined("message ", sum.name, " {"));
    top("  oneof arm {");
    for (std::size_t i = 0; i < sum.variants.size(); ++i) {
        const std::string& arm = sum.variants[i].name;
        top(joined("    ", sum.name, arm, " ", snakeCase(arm), " = ",
                   decimal(static_cast<std::int64_t>(i) + 1), ";"));
    }
    top("  }");
    top("}");
    top("");
    return sum.name;
}

}  // namespace geas
