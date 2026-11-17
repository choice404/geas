// One contract's message block of the .proto: the sign surface, the
// signature, the session event, the resume call, each distinct result
// message once per file, and one request message per pledge
#include "emitproto.hpp"
#include "emittext.hpp"
#include "hash.hpp"

namespace geas {

namespace {

constexpr std::string_view fallbackVowScalar = "int64";
constexpr std::string_view defaultNote = ";  // the vow; unset takes the declared default";
constexpr std::string_view noDefaultNote =
    ";  // the vow; no default is declared, so unset refuses at sign";

// The field numbers that trail the vows in the Signed message
constexpr std::int64_t signedHashOffset = 2;
constexpr std::int64_t signedAtOffset = 3;
constexpr std::int64_t signedTokenOffset = 4;

// Whether a pledge's rpc spelling takes a name the session surface owns
bool isSessionName(std::string_view camel) {
    return camel == "Session" || camel == "Resume" || camel == "GetPartial" || camel == "Break";
}

}  // namespace

void ProtoEmitter::emitContractMessages(const ContractShape& shape) {
    const std::string prefix = prefixOf(shape);
    lookup_.checkVersionAttr(*shape.decl);
    top(joined("// contract ", shape.decl->name, ", version ", decimal(shape.version), ", shape 0x",
               hashHex16(shape.shapeHash), "."));
    emitSignRequest(shape, prefix);
    emitSigned(shape, prefix);
    emitSessionMessages(prefix);
    // Each pledge's return, each distinct shape once per file. The
    // contract's own Err is a value on an OK rpc, never a transport error.
    // A course runs its contract's pledges itself and no client calls one,
    // so an engine driven contract carries neither their returns nor their
    // requests
    if (shape.course == nullptr) {
        for (const PledgeShape& pledge : shape.pledges) {
            ensureReturn(*pledge.decl, lookup_.pledgeSig(*pledge.decl).ret);
        }
        for (const PledgeShape& pledge : shape.pledges) {
            emitRequestMessage(*pledge.decl, prefix);
        }
    }
    emitSignalRequests(shape, prefix);
}

// The sign call carries the vow overrides and the shape check, the two
// things geas_contract_sign takes beside the contract name
void ProtoEmitter::emitSignRequest(const ContractShape& shape, std::string_view prefix) {
    top("// The sign surface: one optional field per vow, and the shape check,");
    top("// where 0 skips it and the wrapper's pinned hash enforces this build.");
    top(joined("message ", prefix, "SignRequest {"));
    for (std::size_t i = 0; i < shape.vows.size(); ++i) {
        const VowDecl& vow = *shape.vows[i];
        std::string_view scalar = scalarOf(lookup_.vowType(vow));
        if (scalar.empty()) {
            lookup_.unsupported(vow.lo, vow.hi, "a vow on the gRPC surface outside the scalar set");
            scalar = fallbackVowScalar;
        }
        top(joined("  optional ", scalar, " ", vow.name, " = ",
                   decimal(static_cast<std::int64_t>(i) + 1),
                   vow.init ? defaultNote : noDefaultNote));
    }
    top(joined("  uint64 expected_hash = ",
               decimal(static_cast<std::int64_t>(shape.vows.size()) + 1), ";"));
    top("}");
    top("");
}

// The signature as the instance carries it: the id the pledge rpcs are
// keyed by, the vows that actually landed, and the hash and timestamp
void ProtoEmitter::emitSigned(const ContractShape& shape, std::string_view prefix) {
    const auto vows = static_cast<std::int64_t>(shape.vows.size());
    top("// The signature: the id the pledge rpcs are keyed by, the effective vows");
    top("// after defaults and overrides, and the hash and timestamp an in process");
    top("// host reads off the handle.");
    top(joined("message ", prefix, "Signed {"));
    top("  uint64 instance_id = 1;");
    for (std::int64_t i = 0; i < vows; ++i) {
        const VowDecl& vow = *shape.vows[static_cast<std::size_t>(i)];
        const std::string_view scalar = scalarOf(lookup_.vowType(vow));
        top(joined("  ", scalar.empty() ? fallbackVowScalar : scalar, " ", vow.name, " = ",
                   decimal(i + 2), ";"));
    }
    top(joined("  uint64 shape_hash = ", decimal(vows + signedHashOffset), ";"));
    top(joined("  int64 signed_at = ", decimal(vows + signedAtOffset), ";"));
    top(joined("  string park_token = ", decimal(vows + signedTokenOffset),
               ";  // the resume key a park enabled server issues; empty when parking is off"));
    top("}");
    top("");
}

// The session event and the resume call, the same for every contract but
// for the prefix
void ProtoEmitter::emitSessionMessages(std::string_view prefix) {
    top("// What the session stream carries. Signed is the first event; the stream's");
    top("// other job is holding the session open, and anything a server later needs");
    top("// to push arrives as another arm.");
    top(joined("message ", prefix, "SessionEvent {"));
    top("  oneof event {");
    top(joined("    ", prefix, "Signed signed = 1;"));
    top("  }");
    top("}");
    top("");
    top("// The resume call: the token names a parked row on the server's park");
    top("// store, and a 0 expected_hash skips the shape check the way sign's does.");
    top(joined("message ", prefix, "ResumeRequest {"));
    top("  string park_token = 1;");
    top("  uint64 expected_hash = 2;");
    top("}");
    top("");
}

// The rpc return of one pledge, its message emitted as needed: a declared
// Result becomes its oneof message once per distinct shape, arms ensured
// first so a container arm rides as its wrapper; a declared Option becomes
// its wrapper directly. Anything else is a named build error
std::string ProtoEmitter::ensureReturn(const PledgeDecl& pledge, TyId ret) {
    const TyNode node = lookup_.types().node(ret);
    if (node.kind == TyKind::Bytes) {
        const std::string name = returnName(ret);
        if (!isSeen(name)) {
            seen_.push_back(name);
            top("message BytesValue { bytes value = 1; }");
            top("");
        }
        return name;
    }
    if (node.kind == TyKind::Option) {
        return ensureType(ret, pledge.lo, pledge.hi);
    }
    if (node.kind != TyKind::Result) {
        lookup_.unsupported(pledge.lo, pledge.hi,
                            "a pledge on the gRPC surface returning outside Result and Option");
        return "";
    }
    const std::string ok = ensureType(node.args[0], pledge.lo, pledge.hi);
    const std::string err = ensureType(node.args[1], pledge.lo, pledge.hi);
    const std::string name = returnName(ret);
    if (ok.empty() || err.empty() || name.empty() || isSeen(name)) {
        return ok.empty() || err.empty() ? std::string{} : name;
    }
    seen_.push_back(name);
    top(joined("// Result<", lookup_.types().render(node.args[0]), ", ",
               lookup_.types().render(node.args[1]),
               ">: the contract's own Err is a value, not a"));
    top("// transport error.");
    top(joined("message ", name, " {"));
    top("  oneof result {");
    top(joined("    ", ok, " ok = 1;"));
    top(joined("    ", err, " err = 2;"));
    top("  }");
    top("}");
    top("");
    return name;
}

// One request message per pledge: the instance id the session issued, then
// the pledge's own parameters in declaration order, the lines built before
// the header so a parameter's type definition lands above the message that
// uses it
void ProtoEmitter::emitRequestMessage(const PledgeDecl& pledge, std::string_view prefix) {
    const CallableSig sig = lookup_.pledgeSig(pledge);
    const std::string camel = camelCase(pledge.name);
    if (isSessionName(camel)) {
        lookup_.fault(
            pledge.lo, pledge.hi,
            joined("codegen: a pledge's rpc spelling collides with the session surface: ", camel));
    }
    std::vector<std::string> lines;
    for (std::size_t i = 0; i < pledge.params.size() && i < sig.params.size(); ++i) {
        std::string text = fieldLine(pledge.params[i].name, sig.params[i],
                                     static_cast<std::int64_t>(i) + 2, pledge.lo, pledge.hi);
        if (!text.empty()) {
            lines.push_back(std::move(text));
        }
    }
    top(joined("message ", requestName(prefix, camel), " {"));
    top("  uint64 instance_id = 1;");
    for (const std::string& text : lines) {
        top(text);
    }
    top("}");
    top("");
}

}  // namespace geas
