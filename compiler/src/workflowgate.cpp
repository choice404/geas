// The workflow scan, see workflowgate.hpp
#include "workflowgate.hpp"

#include <algorithm>

#include "emittext.hpp"

namespace geas {

std::vector<std::string> findWorkflowConstructs(const CheckedProgram& checked) {
    std::vector<std::string> found;
    for (const Decl& decl : checked.program.ast.decls) {
        const auto* contract = std::get_if<ContractDecl>(&decl);
        if (contract == nullptr) {
            continue;
        }
        // One finding per contract, however many amends blocks it carries
        const bool hasAmends = std::any_of(contract->members.begin(), contract->members.end(),
            [](const Member& member) { return std::holds_alternative<AmendsDecl>(member); });
        if (hasAmends) {
            found.push_back(joined("an amends block in contract '", contract->name, "'"));
        }
    }
    return found;
}

}  // namespace geas
