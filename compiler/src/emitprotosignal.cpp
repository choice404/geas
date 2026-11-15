// The signals of one contract on the gRPC surface. Each signal is its own
// unary rpc, Signal then the signal's CamelCase name, taking the instance id
// the session stream issued and the signal's parameters in declaration
// order, and answering the shared SignalReply. A contract without signals
// adds nothing, so its .proto keeps the text it always had
#include "emitproto.hpp"
#include "emittext.hpp"

namespace geas {

namespace {

// The rpc spelling of a signal, prefixed so it never shadows a pledge's
std::string signalRPCName(std::string_view signal) {
    return joined("Signal", camelCase(signal));
}

}  // namespace

// One request message per signal, its parameter lines built before the
// header so a parameter's type definition lands above the message using it
void ProtoEmitter::emitSignalRequests(const ContractShape& shape, std::string_view prefix) {
    for (const SignalShape& signal : shape.signals) {
        const SignalDecl& decl = *signal.decl;
        const CallableSig sig = lookup_.signalSig(decl);
        const std::string rpc = signalRPCName(decl.name);
        for (const PledgeShape& pledge : shape.pledges) {
            if (camelCase(pledge.decl->name) == rpc) {
                lookup_.fault(decl.lo, decl.hi,
                              joined("codegen: a signal's rpc spelling collides with a pledge's: ",
                                     rpc));
            }
        }
        std::vector<std::string> lines;
        for (std::size_t i = 0; i < decl.params.size() && i < sig.params.size(); ++i) {
            std::string text = fieldLine(decl.params[i].name, sig.params[i],
                                         static_cast<std::int64_t>(i) + 2, decl.lo, decl.hi);
            if (!text.empty()) {
                lines.push_back(std::move(text));
            }
        }
        top(joined("// signal ", decl.name, ", delivered into a live instance."));
        top(joined("message ", prefix, rpc, "Request {"));
        top("  uint64 instance_id = 1;");
        for (const std::string& text : lines) {
            top(text);
        }
        top("}");
        top("");
    }
}

void ProtoEmitter::emitSignalRpcs(const ContractShape& shape, std::string_view prefix) {
    if (shape.signals.empty()) {
        return;
    }
    top("  // A signal is an outside event the host delivers: no body runs, the");
    top("  // first delivery wins, and a refused delivery is the call's status.");
    for (const SignalShape& signal : shape.signals) {
        const std::string rpc = signalRPCName(signal.decl->name);
        top(joined("  rpc ", rpc, "(", prefix, rpc, "Request) returns (SignalReply);"));
    }
}

}  // namespace geas
