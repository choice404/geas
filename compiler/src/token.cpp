// The keyword and kind name tables, see token.hpp
#include "token.hpp"

#include <array>
#include <utility>

namespace geas {

namespace {

// A keyword's source spelling, its kind, and the name the dump prints
struct KeywordEntry {
    std::string_view spelling;
    TokenKind kind;
    std::string_view dumpName;
};

constexpr std::array keywordTable{
    KeywordEntry{"contract", TokenKind::KwContract, "Contract"},
    KeywordEntry{"vow", TokenKind::KwVow, "Vow"},
    KeywordEntry{"pledge", TokenKind::KwPledge, "Pledge"},
    KeywordEntry{"clause", TokenKind::KwClause, "Clause"},
    KeywordEntry{"provisional", TokenKind::KwProvisional, "Provisional"},
    KeywordEntry{"subcontract", TokenKind::KwSubcontract, "Subcontract"},
    KeywordEntry{"requirements", TokenKind::KwRequirements, "Requirements"},
    KeywordEntry{"incorporate", TokenKind::KwIncorporate, "Incorporate"},
    KeywordEntry{"internal", TokenKind::KwInternal, "Internal"},
    KeywordEntry{"record", TokenKind::KwRecord, "Record"},
    KeywordEntry{"import", TokenKind::KwImport, "Import"},
    KeywordEntry{"schema", TokenKind::KwSchema, "Schema"},
    KeywordEntry{"transactional", TokenKind::KwTransactional, "Transactional"},
    KeywordEntry{"true", TokenKind::KwTrue, "True"},
    KeywordEntry{"false", TokenKind::KwFalse, "False"},
    KeywordEntry{"let", TokenKind::KwLet, "Let"},
    KeywordEntry{"mut", TokenKind::KwMut, "Mut"},
    KeywordEntry{"return", TokenKind::KwReturn, "Return"},
    KeywordEntry{"if", TokenKind::KwIf, "If"},
    KeywordEntry{"else", TokenKind::KwElse, "Else"},
    KeywordEntry{"match", TokenKind::KwMatch, "Match"},
    KeywordEntry{"while", TokenKind::KwWhile, "While"},
    KeywordEntry{"for", TokenKind::KwFor, "For"},
    KeywordEntry{"in", TokenKind::KwIn, "In"},
    KeywordEntry{"break", TokenKind::KwBreak, "Break"},
    KeywordEntry{"continue", TokenKind::KwContinue, "Continue"},
    KeywordEntry{"is", TokenKind::KwIs, "Is"},
    KeywordEntry{"either", TokenKind::KwEither, "Either"},
    KeywordEntry{"or", TokenKind::KwOr, "Or"},
    KeywordEntry{"remedy", TokenKind::KwRemedy, "Remedy"},
    KeywordEntry{"signal", TokenKind::KwSignal, "Signal"},
    KeywordEntry{"course", TokenKind::KwCourse, "Course"},
    KeywordEntry{"await", TokenKind::KwAwait, "Await"},
    KeywordEntry{"sleep", TokenKind::KwSleep, "Sleep"},
    KeywordEntry{"together", TokenKind::KwTogether, "Together"},
    KeywordEntry{"select", TokenKind::KwSelect, "Select"},
    KeywordEntry{"amends", TokenKind::KwAmends, "Amends"},
    KeywordEntry{"within", TokenKind::KwWithin, "Within"},
    KeywordEntry{"after", TokenKind::KwAfter, "After"},
    KeywordEntry{"retry", TokenKind::KwRetry, "Retry"},
};

// A fixed operator or punctuation kind and its dump name
struct SymbolName {
    TokenKind kind;
    std::string_view dumpName;
};

constexpr std::array symbolNames{
    SymbolName{TokenKind::Eof, "Eof"},
    SymbolName{TokenKind::LParen, "LParen"},
    SymbolName{TokenKind::RParen, "RParen"},
    SymbolName{TokenKind::LBracket, "LBracket"},
    SymbolName{TokenKind::RBracket, "RBracket"},
    SymbolName{TokenKind::LBrace, "LBrace"},
    SymbolName{TokenKind::RBrace, "RBrace"},
    SymbolName{TokenKind::Comma, "Comma"},
    SymbolName{TokenKind::Dot, "Dot"},
    SymbolName{TokenKind::Colon, "Colon"},
    SymbolName{TokenKind::Question, "Question"},
    SymbolName{TokenKind::Arrow, "Arrow"},
    SymbolName{TokenKind::Underscore, "Underscore"},
    SymbolName{TokenKind::Assign, "Assign"},
    SymbolName{TokenKind::Plus, "Plus"},
    SymbolName{TokenKind::Minus, "Minus"},
    SymbolName{TokenKind::Star, "Star"},
    SymbolName{TokenKind::Slash, "Slash"},
    SymbolName{TokenKind::Percent, "Percent"},
    SymbolName{TokenKind::Bang, "Bang"},
    SymbolName{TokenKind::Eq, "EqEq"},
    SymbolName{TokenKind::Ne, "Ne"},
    SymbolName{TokenKind::Lt, "Lt"},
    SymbolName{TokenKind::Le, "Le"},
    SymbolName{TokenKind::Gt, "Gt"},
    SymbolName{TokenKind::Ge, "Ge"},
    SymbolName{TokenKind::AndAnd, "AndAnd"},
    SymbolName{TokenKind::OrOr, "OrOr"},
};

}  // namespace

std::optional<TokenKind> keywordKind(std::string_view word) {
    for (const KeywordEntry& entry : keywordTable) {
        if (entry.spelling == word) {
            return entry.kind;
        }
    }
    return std::nullopt;
}

bool isKeyword(TokenKind kind) {
    return std::to_underlying(kind) >= std::to_underlying(TokenKind::KwContract);
}

std::string_view kindDumpName(TokenKind kind) {
    for (const KeywordEntry& entry : keywordTable) {
        if (entry.kind == kind) {
            return entry.dumpName;
        }
    }
    for (const SymbolName& symbol : symbolNames) {
        if (symbol.kind == kind) {
            return symbol.dumpName;
        }
    }
    return {};
}

}  // namespace geas
