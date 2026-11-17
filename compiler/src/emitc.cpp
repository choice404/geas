// The shared state of the C emitter and its small toolbox, see emitc.hpp
#include "emitc.hpp"

#include <utility>

#include "emittext.hpp"
#include "utf8.hpp"

namespace geas {

namespace {

constexpr std::string_view indentUnit = "    ";
constexpr int hexBase = 16;
constexpr int octalDigitBits = 3;
constexpr unsigned octalDigitMask = 7;
constexpr unsigned char firstPrintable = 32;
constexpr unsigned char lastPrintable = 126;

// Appends one byte of a C string literal in a spelling every C compiler
// reads back exactly: printable ASCII as itself, quote and backslash
// escaped, and everything else as a three digit octal escape, which cannot
// swallow a digit that follows the way a hex escape can
void pushCByte(std::string& out, unsigned char byte) {
    if (byte == '"' || byte == '\\') {
        out.push_back('\\');
        out.push_back(static_cast<char>(byte));
        return;
    }
    if (byte >= firstPrintable && byte <= lastPrintable) {
        out.push_back(static_cast<char>(byte));
        return;
    }
    out.push_back('\\');
    for (int digit = 2; digit >= 0; --digit) {
        const unsigned value = (byte >> (octalDigitBits * digit)) & octalDigitMask;
        out.push_back(static_cast<char>('0' + value));
    }
}

// The marker bits of the UTF-8 lead and continuation bytes
constexpr std::int64_t twoByteLimit = 0x800;
constexpr std::int64_t threeByteLimit = 0x10000;
constexpr int continuationBits = 6;
constexpr std::int64_t continuationMask = 0x3F;
constexpr std::int64_t continuationLead = 0x80;
constexpr std::int64_t twoByteLead = 0xC0;
constexpr std::int64_t threeByteLead = 0xE0;
constexpr std::int64_t fourByteLead = 0xF0;
constexpr std::int64_t asciiLimit = 0x80;

// Appends the continuation bytes of a scalar, count of them, high bits first
void pushContinuations(std::string& out, std::int64_t scalar, int count) {
    for (int i = count - 1; i >= 0; --i) {
        const std::int64_t bits = (scalar >> (continuationBits * i)) & continuationMask;
        out.push_back(static_cast<char>(continuationLead + bits));
    }
}

// Appends the UTF-8 bytes of one scalar
void pushUTF8(std::string& out, std::int64_t scalar) {
    if (scalar < asciiLimit) {
        out.push_back(static_cast<char>(scalar));
        return;
    }
    const int count = scalar < twoByteLimit ? 1 : scalar < threeByteLimit ? 2 : 3;
    const std::int64_t lead = count == 1 ? twoByteLead : count == 2 ? threeByteLead : fourByteLead;
    out.push_back(static_cast<char>(lead + (scalar >> (continuationBits * count))));
    pushContinuations(out, scalar, count);
}

// Decodes a \u{hex} escape whose brace sits at pos, appending the scalar as
// UTF-8 and returning the offset of the closing brace
std::size_t decodeUnicode(std::string_view body, std::size_t pos, std::string& out) {
    std::int64_t scalar = 0;
    std::size_t cursor = pos + 1;
    while (cursor < body.size() && body[cursor] != '}') {
        scalar = scalar * hexBase + hexValue(byteAt(body, static_cast<std::int64_t>(cursor)));
        ++cursor;
    }
    pushUTF8(out, scalar);
    return cursor;
}

// Decodes the canonical string body the parser kept: printable ASCII raw,
// backslash and quote escaped, everything else as \u{hex}. Returns the raw
// UTF-8 bytes of the value
std::string decodeBody(std::string_view body) {
    std::string out;
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '\\' || i + 1 >= body.size()) {
            out.push_back(body[i]);
            continue;
        }
        ++i;
        switch (body[i]) {
            case 'u': i = decodeUnicode(body, i + 1, out); break;
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case '0': out.push_back('\0'); break;
            default: out.push_back(body[i]); break;
        }
    }
    return out;
}

}  // namespace

std::string contractStem(std::string_view contract) {
    return joined("k", decimal(static_cast<std::int64_t>(contract.size())), "_", contract);
}

std::string clauseFunctionName(std::string_view contract, std::string_view clause) {
    return joined("geas_clause", decimal(static_cast<std::int64_t>(contract.size())), "_",
                  contract, "_", clause);
}

CEmitter::CEmitter(CheckedProgram& checked, std::vector<Diagnostic>& diagnostics)
    : lookup_{checked, diagnostics},
      lowering_{checked.program.ast, checked.resolved, checked.types, scratch_} {}

TyNode CEmitter::nodeOf(TyId type) const {
    return type.isSet() ? types().node(type) : TyNode{};
}

// A fresh thunk local temporary name, t0, t1, and so on
std::string CEmitter::freshName() {
    return joined("t", decimal(nextTemporary_++));
}

// Pushes one line of the current body at the current indent
void CEmitter::line(std::string_view text) {
    for (int i = 0; i < indent_ + 1; ++i) {
        body_.append(indentUnit);
    }
    body_.append(text);
    body_.push_back('\n');
}

// Pushes one line of the finished file, no indent
void CEmitter::top(std::string_view text) {
    out_.append(text);
    out_.push_back('\n');
}

void CEmitter::pushScope() {
    scopes_.emplace_back();
}

void CEmitter::popScope() {
    scopes_.pop_back();
}

void CEmitter::declare(const std::string& name, std::string variable, TyId type) {
    scopes_.back().insert_or_assign(name, CValue{std::move(variable), type});
}

const CValue* CEmitter::lookupLocal(std::string_view name) const {
    const std::string key{name};
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        const auto found = scope->find(key);
        if (found != scope->end()) {
            return &found->second;
        }
    }
    return nullptr;
}

CValue CEmitter::badValue() const {
    return CValue{"bad", unknown()};
}

// Opens a fresh zeroed GeasValue temporary and returns its name
std::string CEmitter::newValue() {
    std::string value = freshName();
    line(joined("GeasValue ", value, " = {0};"));
    return value;
}

void CEmitter::setTag(std::string_view value, std::string_view tag) {
    line(joined(value, ".ty = ", tag, ";"));
}

// The exit every runtime helper shares: a status that is not GEAS_OK leaves
// the enclosing body with it
void CEmitter::returnUnlessOk(std::string_view status) {
    line(joined("if (", status, " != GEAS_OK) ", failExit(status)));
}

// The statement that leaves the enclosing function on a failed status: a
// thunk or clause returns it, a course records it in its frame and stops
std::string CEmitter::failExit(std::string_view status) const {
    if (course_ != nullptr) {
        return joined("{ f->status = ", status, "; return GEAS_COURSE_STOPPED; }");
    }
    return joined("return ", status, ";");
}

void CEmitter::unsupportedAt(const Expr& expr, std::string_view what) {
    lookup_.unsupported(expr.lo, expr.hi, what);
}

// Whether a checked type is a composite whose value is more than one union
// field. These are the values that carry a data pointer or a box, so a bare
// struct assignment aliases them and a copy has to walk the whole shape
bool CEmitter::isComposite(TyId type) const {
    switch (kindOf(type)) {
        case TyKind::Named:
        case TyKind::List:
        case TyKind::Map:
        case TyKind::Tuple:
        case TyKind::Option:
        case TyKind::Result: return true;
        default: return false;
    }
}

// Gives a value its own instance owned storage so no other binding can see a
// mutation through it. A scalar or a string rides through untouched and a
// composite is deep copied into a fresh temporary
CValue CEmitter::copyOut(const CValue& value) {
    if (!isComposite(value.type)) {
        return value;
    }
    std::string copy = newValue();
    const std::string status = freshName();
    line(joined("GeasStatus ", status, " = geas_value_deep_copy(cc, &", value.name, ", &", copy,
                ");"));
    returnUnlessOk(status);
    return CValue{std::move(copy), value.type};
}

// Lowers a canonical string body to a C string literal and its length
CString CEmitter::cString(std::string_view body) const {
    const std::string raw = decodeBody(body);
    std::string literal{"\""};
    for (const char byte : raw) {
        pushCByte(literal, static_cast<unsigned char>(byte));
    }
    literal.push_back('"');
    return CString{std::move(literal), static_cast<std::int64_t>(raw.size())};
}

// Starts a fresh thunk or clause body
void CEmitter::beginBody(TyId ret, std::int64_t lo, std::int64_t hi) {
    body_.clear();
    scopes_.clear();
    nextTemporary_ = 0;
    indent_ = 0;
    returnType_ = ret;
    spanLo_ = lo;
    spanHi_ = hi;
}

// Lands the lowered body in the file with the tail every body shares: one
// that runs off its end reports Unit
void CEmitter::endBody() {
    out_.append(body_);
    body_.clear();
    top("    out->ty = GEAS_TY_UNIT;");
    top("    return GEAS_OK;");
    top("}");
    top("");
}

}  // namespace geas
