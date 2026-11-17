// The body level nodes of the syntax tree: types, expressions, patterns,
// statements, blocks, and requirements formulas. Each recursive shape lives
// in its own arena inside Ast, see ast.hpp, and is named by a small typed
// index, so the tree is flat and frees in one go. Names and literal payloads
// are owned strings, so the tree outlives the source buffer
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace geas {

// A typed index into one of the arenas, the tag keeps an ExprId from being
// passed where a TypeId belongs
template <typename Tag>
struct NodeId {
    std::int32_t index = -1;
};

using ExprId = NodeId<struct ExprTag>;
using StmtId = NodeId<struct StmtTag>;
using TypeId = NodeId<struct TypeTag>;
using PatternId = NodeId<struct PatternTag>;
using BlockId = NodeId<struct BlockTag>;
using ReqId = NodeId<struct ReqTag>;

// Types

struct NamedType {
    std::string name;
    std::vector<TypeId> args;
};

// The first class pledge type, pledge(T, ...) -> R
struct PledgeType {
    std::vector<TypeId> params;
    TypeId ret;
};

using Type = std::variant<NamedType, PledgeType>;

// Expressions

enum class UnaryOp { Neg, Not };

enum class BinaryOp { Add, Sub, Mul, Div, Mod, Eq, Ne, Lt, Le, Gt, Ge, And, Or };

// The one argument syntactic constructors, Some(e), Ok(e), Err(e), mut(e)
enum class WrapKind { Some, Ok, Err, Mut };

struct IntLiteral {
    std::int64_t value;
    bool isSize = false;
};

// A float keeps its spelling so the dump never depends on float formatting
struct FloatLiteral {
    std::string spelling;
};

// The canonical re escaped body, quotes stripped
struct StringLiteral {
    std::string body;
};

struct CharLiteral {
    std::int64_t scalar;
};

struct BoolLiteral {
    bool value;
};

struct DurationLiteral {
    std::int64_t milliseconds;
};

struct Identifier {
    std::string name;
};

struct UnaryExpr {
    UnaryOp op;
    ExprId operand;
};

struct BinaryExpr {
    BinaryOp op;
    ExprId lhs;
    ExprId rhs;
};

// A call argument, the [IDENT ":"] Expr form
struct Argument {
    std::optional<std::string> label;
    ExprId value;
};

struct CallExpr {
    ExprId callee;
    std::vector<Argument> args;
};

struct IndexExpr {
    ExprId base;
    ExprId index;
};

struct MemberExpr {
    ExprId base;
    std::string name;
};

struct QuestionExpr {
    ExprId operand;
};

struct TupleExpr {
    std::vector<ExprId> elements;
};

struct ListExpr {
    std::vector<ExprId> elements;
};

struct FieldInit {
    std::string name;
    ExprId value;
};

struct RecordLiteral {
    std::string name;
    std::vector<FieldInit> fields;
};

// The body of a match or select arm, an expression or a block
using ArmBody = std::variant<ExprId, BlockId>;

struct MatchArm {
    PatternId pattern;
    ArmBody body;
};

struct MatchExpr {
    ExprId scrutinee;
    std::vector<MatchArm> arms;
};

struct WrapExpr {
    WrapKind kind;
    ExprId inner;
};

struct NoneExpr {};

// Map<K, V>(), the empty map constructor
struct MapNewExpr {
    TypeId type;
};

// List<T>(..), Option<T>(..), Result<T, E>(..), Tuple<..>(..)
struct GenericNewExpr {
    TypeId type;
    std::vector<Argument> args;
};

// await sig [within DURATION]
struct AwaitExpr {
    std::string signal;
    std::optional<std::int64_t> withinMilliseconds;
};

// await sig [as Pattern] -> body
struct AwaitArm {
    std::string signal;
    std::optional<PatternId> binding;
    ArmBody body;
};

// timeout DURATION -> body
struct TimeoutArm {
    std::int64_t milliseconds;
    ArmBody body;
};

using SelectArm = std::variant<AwaitArm, TimeoutArm>;

struct SelectExpr {
    std::vector<SelectArm> arms;
};

using ExprKind = std::variant<IntLiteral, FloatLiteral, StringLiteral, CharLiteral, BoolLiteral,
                              DurationLiteral, Identifier, UnaryExpr, BinaryExpr, CallExpr,
                              IndexExpr, MemberExpr, QuestionExpr, TupleExpr, ListExpr,
                              RecordLiteral, MatchExpr, WrapExpr, NoneExpr, MapNewExpr,
                              GenericNewExpr, AwaitExpr, SelectExpr>;

// An expression over the byte span [lo, hi)
struct Expr {
    std::int64_t lo;
    std::int64_t hi;
    ExprKind kind;
};

// Patterns

struct WildcardPattern {};

struct LiteralPattern {
    ExprId literal;
};

struct BindPattern {
    std::string name;
};

struct VariantPattern {
    std::string name;
    std::vector<PatternId> fields;
};

// Some(p), Ok(p), Err(p); Mut never appears here
struct WrapPattern {
    WrapKind kind;
    PatternId inner;
};

struct NonePattern {};

using Pattern = std::variant<WildcardPattern, LiteralPattern, BindPattern, VariantPattern,
                             WrapPattern, NonePattern>;

// Statements

struct LetStmt {
    bool isMutable;
    std::string name;
    std::optional<TypeId> type;
    ExprId value;
};

struct AssignStmt {
    ExprId target;
    ExprId value;
};

struct ReturnStmt {
    std::optional<ExprId> value;
};

// An else if chain is an else block holding one if statement
struct IfStmt {
    ExprId condition;
    BlockId thenBlock;
    std::optional<BlockId> elseBlock;
};

struct WhileStmt {
    ExprId condition;
    BlockId body;
};

struct ForStmt {
    std::string variable;
    ExprId iterable;
    BlockId body;
};

struct BreakStmt {};

struct ContinueStmt {};

struct ExprStmt {
    ExprId expr;
};

struct SleepStmt {
    ExprId duration;
};

struct TogetherStmt {
    BlockId body;
};

using Stmt = std::variant<LetStmt, AssignStmt, ReturnStmt, IfStmt, WhileStmt, ForStmt, BreakStmt,
                          ContinueStmt, ExprStmt, SleepStmt, TogetherStmt>;

struct Block {
    std::vector<StmtId> stmts;
};

// Requirements formulas

struct ReqAtom {
    std::string name;
};

struct ReqNot {
    ReqId operand;
};

struct ReqAnd {
    ReqId lhs;
    ReqId rhs;
};

struct ReqOr {
    ReqId lhs;
    ReqId rhs;
};

struct ReqNode {
    std::int64_t lo;
    std::int64_t hi;
    std::variant<ReqAtom, ReqNot, ReqAnd, ReqOr> kind;
};

}  // namespace geas
