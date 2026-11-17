// The front end driver, see check.hpp
#include "check.hpp"

#include <utility>

#include "checker.hpp"
#include "hash.hpp"
#include "reqsolve.hpp"

namespace geas {

namespace {

std::vector<std::string> renderAll(const Program& program,
                                   const std::vector<Diagnostic>& diagnostics) {
    std::vector<std::string> rendered;
    for (const Diagnostic& diagnostic : diagnostics) {
        rendered.push_back(renderProgramDiagnostic(program, diagnostic));
    }
    return rendered;
}

}  // namespace

std::expected<CheckedProgram, std::vector<std::string>> checkProgram(std::string_view rootPath) {
    CheckedProgram checked;
    checked.program = loadProgram(rootPath);
    if (!checked.program.isOk) {
        return std::unexpected(std::move(checked.program.loadErrors));
    }
    checked.resolved = resolveProgram(checked.program);
    if (!checked.resolved.diagnostics.empty()) {
        return std::unexpected(renderAll(checked.program, checked.resolved.diagnostics));
    }
    std::vector<Diagnostic> diagnostics;
    TypeLowering lowering{checked.program.ast, checked.resolved, checked.types, diagnostics};
    checked.sigs = buildSigs(checked.program, checked.types, lowering, diagnostics);
    checked.tables = Checker{checked.program, checked.resolved, checked.sigs,
                             checked.types,   lowering,         diagnostics}
                         .run();
    solveRequirements(checked.program.ast, diagnostics);
    if (!diagnostics.empty()) {
        return std::unexpected(renderAll(checked.program, diagnostics));
    }
    checked.contracts =
        buildContractShapes(checked.program, checked.sigs, checked.types, checked.tables);
    const auto lowered = lowerCourses(checked);
    if (!lowered) {
        return std::unexpected(std::vector<std::string>{lowered.error()});
    }
    return checked;
}

std::string dumpShapes(const CheckedProgram& checked) {
    std::string out;
    for (const ContractShape& contract : checked.contracts) {
        if (contract.decl->isInternal) {
            continue;
        }
        const std::string& name = contract.decl->name;
        out.append("// shape " + name + " v" + std::to_string(contract.version) + ": " +
                   contract.shape + "\n");
        out.append("#define GEAS_HASH_" + name + " 0x" + hashHex16(contract.shapeHash) + "ULL\n");
        for (const PledgeShape& pledge : contract.pledges) {
            out.append("#define GEAS_MANGLED_" + name + "_" + pledge.decl->name + " \"" +
                       pledge.mangled + "\"\n");
        }
        for (const RemedyShape& remedy : contract.remedies) {
            out.append("#define GEAS_MANGLED_" + name + "_" + remedy.decl->name + " \"" +
                       remedy.mangled + "\"\n");
        }
    }
    return out;
}

}  // namespace geas
