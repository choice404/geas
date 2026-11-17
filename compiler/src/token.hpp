// The lexed token and the tables that name its kind. A token is a flat value,
// its kind, its half open byte span, and whether a line break came before it.
// There is no newline token, the flag rides on the next token and the parser
// decides when it ends a statement. A literal's payload is rebuilt from the
// span when it is needed, which keeps the token small and free of owned heap
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace geas {

// Every kind the lexer produces, the names follow section 1.2 of the grammar
enum class TokenKind {
    Eof,

    // Names and literals
    Ident,
    Int,
    Size,
    Float,
    Duration,
    String,
    Char,

    // Punctuation and operators
    LParen,
    RParen,
    LBracket,
    RBracket,
    LBrace,
    RBrace,
    Comma,
    Dot,
    Colon,
    Question,
    Arrow,
    Underscore,
    Assign,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Bang,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    AndAnd,
    OrOr,

    // Hard keywords, carried over
    KwContract,
    KwVow,
    KwPledge,
    KwClause,
    KwProvisional,
    KwSubcontract,
    KwRequirements,
    KwIncorporate,
    KwInternal,
    KwRecord,
    KwImport,
    KwSchema,
    KwTransactional,
    KwTrue,
    KwFalse,
    KwLet,
    KwMut,
    KwReturn,
    KwIf,
    KwElse,
    KwMatch,
    KwWhile,
    KwFor,
    KwIn,
    KwBreak,
    KwContinue,
    KwIs,
    KwEither,
    KwOr,

    // Hard keywords, new for workflows
    KwRemedy,
    KwSignal,
    KwCourse,
    KwAwait,
    KwSleep,
    KwTogether,
    KwSelect,
    KwAmends,
    KwWithin,
    KwAfter,
    KwRetry,
};

// A lexed token over the byte span [lo, hi)
struct Token {
    TokenKind kind;
    std::int64_t lo;
    std::int64_t hi;
    bool hasNewlineBefore;
};

/**
 * The hard keyword a word spells, true and false included.
 * @params word - the scanned identifier text
 * @returns {std::optional<TokenKind>} - the keyword kind, or nullopt for a plain identifier
 */
std::optional<TokenKind> keywordKind(std::string_view word);

/**
 * Whether a kind is one of the hard keywords, true and false included.
 * @params kind - the kind to test
 * @returns {bool} - true for a keyword kind
 */
bool isKeyword(TokenKind kind);

/**
 * The name the lex dump prints for a fixed kind: "Arrow" for an operator,
 * "Let" for a keyword. Literal kinds render from their span instead and get
 * an empty name here.
 * @params kind - the kind to name
 * @returns {std::string_view} - a static name, empty for a literal kind
 */
std::string_view kindDumpName(TokenKind kind);

}  // namespace geas
