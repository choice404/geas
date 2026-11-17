// Enumeration is the independent oracle for small requirements formulas
#include "reqselftest.hpp"

#include <algorithm>
#include <cstdio>
#include <expected>
#include <random>

#include "lexer.hpp"
#include "parser.hpp"
#include "reqsat.hpp"
#include "source.hpp"

namespace geas {

namespace {

constexpr std::size_t enumerationAtomLimit = 16;
constexpr std::uint32_t randomSeed = 0x6EA564;
constexpr std::size_t randomCaseCount = 600;
constexpr std::uint32_t randomAtomLimit = 12;
constexpr std::uint32_t randomDepth = 5;
constexpr std::uint32_t randomForms = 4;

bool evaluate(const Ast& ast, ReqId id, const RequirementAtoms& atoms, std::uint64_t mask) {
    const ReqNode& node = ast.req(id);
    if (const auto* atom = std::get_if<ReqAtom>(&node.kind)) {
        const auto index = std::find(atoms.begin(), atoms.end(), atom->name) - atoms.begin();
        return ((mask >> index) & 1U) != 0;
    }
    if (const auto* negation = std::get_if<ReqNot>(&node.kind)) {
        return !evaluate(ast, negation->operand, atoms, mask);
    }
    if (const auto* conjunction = std::get_if<ReqAnd>(&node.kind)) {
        return evaluate(ast, conjunction->lhs, atoms, mask) &&
               evaluate(ast, conjunction->rhs, atoms, mask);
    }
    const auto& disjunction = std::get<ReqOr>(node.kind);
    return evaluate(ast, disjunction.lhs, atoms, mask) ||
           evaluate(ast, disjunction.rhs, atoms, mask);
}

RequirementVerdict enumerate(const Ast& ast, const RequirementsDecl& block,
                             const RequirementAtoms& atoms) {
    RequirementVerdict verdict;
    const std::uint64_t total = std::uint64_t{1} << atoms.size();
    for (std::uint64_t mask = 0; mask < total; ++mask) {
        const bool isFulfilled = block.fulfill && evaluate(ast, *block.fulfill, atoms, mask);
        const bool isBroken = block.breakLine && evaluate(ast, *block.breakLine, atoms, mask);
        verdict.canFulfill = verdict.canFulfill || isFulfilled;
        verdict.canBreak = verdict.canBreak || isBroken;
        if (isFulfilled && isBroken && !verdict.both) {
            verdict.both = mask;
        }
    }
    return verdict;
}

bool doesAgree(const Ast& ast, const RequirementsDecl& block, const RequirementAtoms& atoms) {
    return searchRequirements(ast, block, atoms) == enumerate(ast, block, atoms);
}

ReqId randomFormula(Ast& ast, std::mt19937& random, std::uint32_t atomCount,
                    std::uint32_t depth) {
    const auto form = random() % randomForms;
    if (depth == 0 || form == 0) {
        const std::string name = "atom" + std::to_string(random() % atomCount);
        return ast.addReq(ReqNode{0, 0, ReqAtom{name}});
    }
    const ReqId lhs = randomFormula(ast, random, atomCount, depth - 1);
    if (form == 1) {
        return ast.addReq(ReqNode{0, 0, ReqNot{lhs}});
    }
    const ReqId rhs = randomFormula(ast, random, atomCount, depth - 1);
    if (form == 2) {
        return ast.addReq(ReqNode{0, 0, ReqAnd{lhs, rhs}});
    }
    return ast.addReq(ReqNode{0, 0, ReqOr{lhs, rhs}});
}

// Some pairs share a tree or its complement; absent lines remain false
RequirementsDecl randomBlock(Ast& ast, std::mt19937& random, std::uint32_t atomCount) {
    RequirementsDecl block{0, 0, randomFormula(ast, random, atomCount, randomDepth),
                           randomFormula(ast, random, atomCount, randomDepth),
                           randomFormula(ast, random, atomCount, randomDepth)};
    switch (random() % randomForms) {
        case 0: block.breakLine = block.fulfill; break;
        case 1: block.breakLine = ast.addReq(ReqNode{0, 0, ReqNot{*block.fulfill}}); break;
        case 2: block.fulfill.reset(); break;
        default: break;
    }
    return block;
}

bool checkRandom() {
    std::mt19937 random{randomSeed};
    for (std::size_t index = 0; index < randomCaseCount; ++index) {
        Ast ast;
        const std::uint32_t atomCount = 1 + random() % randomAtomLimit;
        const RequirementsDecl block = randomBlock(ast, random, atomCount);
        if (!doesAgree(ast, block, collectRequirementAtoms(ast, block))) {
            std::fprintf(stderr, "requirements self test disagrees at seed %u, case %zu\n",
                         randomSeed, index);
            return false;
        }
    }
    return true;
}

std::expected<std::size_t, std::string> checkFile(std::string_view path) {
    const auto source = loadSource(path);
    if (!source) {
        return std::unexpected(std::string{sourceErrorMessage(source.error())});
    }
    const LexResult lexed = lex(source->text);
    const ParseResult parsed = parse(source->text, lexed.tokens);
    std::size_t count = 0;
    for (const Decl& decl : parsed.ast.decls) {
        const auto* contract = std::get_if<ContractDecl>(&decl);
        if (contract == nullptr) {
            continue;
        }
        for (const Member& member : contract->members) {
            const auto* block = std::get_if<RequirementsDecl>(&member);
            if (block == nullptr) {
                continue;
            }
            const RequirementAtoms atoms = collectRequirementAtoms(parsed.ast, *block);
            if (atoms.size() > enumerationAtomLimit) {
                continue;
            }
            if (!doesAgree(parsed.ast, *block, atoms)) {
                return std::unexpected("disagreement in contract " + contract->name);
            }
            ++count;
        }
    }
    return count;
}

}  // namespace

int runRequirementsSelfTest(std::span<char*> paths) {
    if (!checkRandom()) {
        return 1;
    }
    std::size_t blockCount = 0;
    for (const char* path : paths) {
        const auto checked = checkFile(path);
        if (!checked) {
            std::fprintf(stderr, "requirements self test: %s: %s\n", path,
                         checked.error().c_str());
            return 1;
        }
        blockCount += *checked;
    }
    std::fprintf(stderr, "[requirements] %zu random formulas and %zu source blocks agree\n",
                 randomCaseCount, blockCount);
    return 0;
}

}  // namespace geas
