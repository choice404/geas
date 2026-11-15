// The .proto emitter behind geas emit-proto. It spells every public
// contract as its own typed service, one rpc per pledge with the contract's
// argument and result types, so a consumer gets them from protoc alone. The
// signing rpc is the surface's one load bearing decision: Session takes
// what sign takes and answers with a stream whose first event is the
// signature, and the stream stays open for the instance's life. The pledge
// rpcs are unary and typed, keyed by the instance id the stream issued.
//
// Message names carry no contract prefix while the file declares one public
// contract; a file with several prefixes every contract scoped message with
// its contract's name so the shared file stays unambiguous. Each type
// definition is emitted once per file, tracked by name in seen_. The member
// functions are spread over emitproto.cpp, emitprototype.cpp,
// emitprotodecl.cpp, and emitprotomsg.cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "check.hpp"
#include "diagnostics.hpp"
#include "emitlookup.hpp"

namespace geas {

/**
 * snake_case to CamelCase, the rpc spelling of a pledge name.
 * @params name - the pledge name
 * @returns {std::string} - the CamelCase spelling
 */
std::string camelCase(std::string_view name);

class ProtoEmitter {
public:
    ProtoEmitter(CheckedProgram& checked, std::vector<Diagnostic>& diagnostics,
                 std::string_view stem);

    /**
     * Emits the whole .proto.
     * @returns {std::string} - the file, to be thrown away when faults landed in the diagnostics
     * vector the emitter was built with
     */
    std::string emit() &&;

private:
    // The file frame and the services, emitproto.cpp
    void top(std::string_view text);
    bool isSeen(std::string_view name) const;
    void emitService(const ContractShape& shape);
    std::string prefixOf(const ContractShape& shape) const;
    std::string requestName(std::string_view prefix, std::string_view camel) const;

    // Types on the wire, emitprototype.cpp
    std::string_view scalarOf(TyId type) const;
    std::string wordOf(TyId type) const;
    std::string tupleWord(const TyNode& node) const;
    std::string returnName(TyId ret) const;
    bool isPlainSum(TyId type) const;
    bool tracksPresence(TyId inner) const;
    std::string ensureType(TyId type, std::int64_t lo, std::int64_t hi);
    std::string ensureUnit();
    std::string ensureOption(TyId type, std::int64_t lo, std::int64_t hi);
    std::string ensureList(TyId type, std::int64_t lo, std::int64_t hi);
    std::string ensureMap(TyId type, std::int64_t lo, std::int64_t hi);
    std::string ensureTuple(TyId type, std::int64_t lo, std::int64_t hi);

    // Declared records and sums, emitprotodecl.cpp
    std::string ensureRecord(std::string_view name, std::int64_t lo, std::int64_t hi);
    std::string ensureSum(const SumDecl& sum, std::int64_t lo, std::int64_t hi);
    void emitPlainSum(const SumDecl& sum);
    bool emitArmMessages(const SumDecl& sum, std::int64_t lo, std::int64_t hi);
    std::string fieldLine(std::string_view name, TyId type, std::int64_t number, std::int64_t lo,
                          std::int64_t hi);
    bool buildFieldLines(const std::vector<FieldSig>& fields, std::int64_t lo, std::int64_t hi,
                         std::vector<std::string>& lines);

    // One contract's messages, emitprotomsg.cpp
    void emitContractMessages(const ContractShape& shape);
    void emitSignRequest(const ContractShape& shape, std::string_view prefix);
    void emitSigned(const ContractShape& shape, std::string_view prefix);
    void emitSessionMessages(std::string_view prefix);
    std::string ensureReturn(const PledgeDecl& pledge, TyId ret);
    void emitRequestMessage(const PledgeDecl& pledge, std::string_view prefix);

    // The signal rpcs and their requests, emitprotosignal.cpp
    void emitSignalRequests(const ContractShape& shape, std::string_view prefix);
    void emitSignalRpcs(const ContractShape& shape, std::string_view prefix);

    EmitLookup lookup_;
    std::string stem_;
    std::string out_;
    std::vector<std::string> seen_;
    bool hasManyContracts_ = false;
};

}  // namespace geas
