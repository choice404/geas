// The C header emitter behind geas emit-header. It renders one header a
// foreign C host compiles against: an include guard from the root file's
// stem, one GEAS_HASH_{Contract} define carrying the shape hash, and one
// GEAS_MANGLED_{Contract}_{name} define per pledge and remedy carrying the exact
// mangled string the module registered, each under a comment spelling the
// signature the way the surface language wrote it. The hashes and mangled
// names come from the same contract shapes the module emitter reads, so a
// header and the module it describes can never disagree. The mangled names
// carry each name's length and never collide, but a macro name joins the
// contract and the item with a bare underscore, so two pairs can spell one
// macro, contract A with pledge b_c and contract A_b with pledge c; a header
// that would define one macro twice is an error naming both
#include <cctype>
#include <unordered_map>

#include "codegen.hpp"
#include "emitlookup.hpp"
#include "emittext.hpp"
#include "hash.hpp"

namespace geas {

namespace {

// The include guard spelled from the root file's stem: GEAS_HEADER_ then
// the stem uppercased, with anything outside the identifier set as
// underscore, then the hash of the stem itself in hex, so two stems that
// spell the same identifier, a-b and a_b, still guard apart
std::string guardOf(std::string_view stem) {
    std::string out{"GEAS_HEADER_"};
    for (const char c : stem) {
        const auto byte = static_cast<unsigned char>(c);
        const bool isIdentifier = byte < 0x80 && std::isalnum(byte) != 0;
        out.push_back(isIdentifier ? static_cast<char>(std::toupper(byte)) : '_');
    }
    out.push_back('_');
    for (const char digit : hashHex16(fnv1a64(stem))) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(digit))));
    }
    return out;
}

// The human readable signature of one pledge or remedy, the surface
// spelling: (name: Type, ...) -> Ret
std::string signatureText(const EmitLookup& lookup, const std::vector<Field>& params,
                          const CallableSig& sig) {
    std::string out{"("};
    for (std::size_t i = 0; i < params.size(); ++i) {
        out.append(i > 0 ? ", " : "");
        out.append(joined(params[i].name, ": "));
        if (i < sig.params.size()) {
            out.append(lookup.types().render(sig.params[i]));
        }
    }
    out.append(joined(") -> ", lookup.types().render(sig.ret)));
    return out;
}

void line(std::string& out, std::string_view text) {
    out.append(text);
    out.push_back('\n');
}

// Every macro name the header defines so far, each with the item it names
class MacroNames {
public:
    explicit MacroNames(std::vector<Diagnostic>& faults) : faults_{faults} {}

    /**
     * Files one macro name, recording an error when another item already took it.
     * @params macro - the macro name, item - how a diagnostic names the item, lo and hi - its span
     */
    void claim(const std::string& macro, std::string item, std::int64_t lo, std::int64_t hi) {
        const auto [found, isNew] = owners_.emplace(macro, item);
        if (isNew) {
            return;
        }
        faults_.push_back(Diagnostic{lo, hi,
                                     joined("geas: the header macro ", macro, " would name both ",
                                            found->second, " and ", item,
                                            "; rename one of them so each macro names one item")});
    }

private:
    std::vector<Diagnostic>& faults_;
    std::unordered_map<std::string, std::string> owners_;
};

// The comment naming a contract with its version and vow table
void emitContractComment(std::string& out, const EmitLookup& lookup, const ContractShape& shape) {
    const std::string head =
        joined("/* contract ", shape.decl->name, ", version ", decimal(shape.version));
    if (shape.vows.empty()) {
        line(out, joined(head, " */"));
        return;
    }
    line(out, head);
    for (const VowDecl* vow : shape.vows) {
        line(out, joined(" * vow ", vow->name, ": ", lookup.types().render(lookup.vowType(*vow)),
                         vow->init ? " (default)" : " (no default)"));
    }
    line(out, " */");
}

// One contract's block: its comment, the shape hash define, then a
// commented define per pledge
void emitContractBlock(std::string& out, const EmitLookup& lookup, const ContractShape& shape,
                       MacroNames& macros) {
    const std::string& name = shape.decl->name;
    lookup.checkVersionAttr(*shape.decl);
    emitContractComment(out, lookup, shape);
    const std::string hashMacro = joined("GEAS_HASH_", name);
    macros.claim(hashMacro, joined("contract ", name), shape.decl->lo, shape.decl->hi);
    line(out, joined("#define ", hashMacro, " 0x", hashHex16(shape.shapeHash), "ULL"));
    line(out, "");
    for (const PledgeShape& pledge : shape.pledges) {
        const PledgeDecl& decl = *pledge.decl;
        const std::string text = signatureText(lookup, decl.params, lookup.pledgeSig(decl));
        const std::string macro = joined("GEAS_MANGLED_", name, "_", decl.name);
        macros.claim(macro, joined("pledge ", decl.name, " of contract ", name), decl.lo, decl.hi);
        line(out, joined("/* pledge ", decl.name, text, decl.body ? " */" : ", abstract */"));
        line(out, joined("#define ", macro, " \"", pledge.mangled, "\""));
        line(out, "");
    }
    // A remedy binds the way a pledge does, so a host gets its mangled name
    // beside the pledges'
    for (const RemedyShape& remedy : shape.remedies) {
        const RemedyDecl& decl = *remedy.decl;
        std::vector<Field> params;
        if (decl.param) {
            params.push_back(*decl.param);
        }
        const std::string text = signatureText(lookup, params, lookup.remedySig(decl));
        const std::string macro = joined("GEAS_MANGLED_", name, "_", decl.name);
        macros.claim(macro, joined("remedy ", decl.name, " of contract ", name), decl.lo, decl.hi);
        line(out, joined("/* remedy ", decl.name, " for ", decl.target, text,
                         decl.body ? " */" : ", abstract */"));
        line(out, joined("#define ", macro, " \"", remedy.mangled, "\""));
        line(out, "");
    }
}

}  // namespace

std::expected<std::string, std::vector<Diagnostic>> emitHeader(CheckedProgram& checked,
                                                               std::string_view stem) {
    std::vector<Diagnostic> faults;
    const EmitLookup lookup{checked, faults};
    const std::string guard = guardOf(stem);
    std::string out;
    line(out, joined("/* ", stem, ".geas.h: generated by geas emit-header, do not edit."));
    line(out, " * Every layout these names describe comes from geas/geas_abi.h; include");
    line(out, " * that header for the value and descriptor structs. */");
    line(out, joined("#ifndef ", guard));
    line(out, joined("#define ", guard));
    line(out, "");
    MacroNames macros{faults};
    for (const ContractShape* shape : lookup.publicContracts()) {
        emitContractBlock(out, lookup, *shape, macros);
    }
    line(out, joined("#endif /* ", guard, " */"));
    if (!faults.empty()) {
        return std::unexpected(std::move(faults));
    }
    return out;
}

}  // namespace geas
