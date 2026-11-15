// The geas syntax tree. The body level nodes and their typed ids are in
// astnode.hpp; this header adds the declarations, which are plain values in
// source order, and the Ast that owns every arena. A node is added through
// the add helpers, which hand back the id of the new slot
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "astnode.hpp"

namespace geas {

// A name and type pair, shared by record fields, variant fields, params,
// and schema columns, which are all the same FieldDecl production
struct Field {
    std::string name;
    TypeId type;
};

// An attribute key and its literal value
struct Attr {
    std::string key;
    ExprId value;
};

// Terms trailing a signature

struct WithinTerm {
    std::int64_t milliseconds;
};

struct AfterTerm {
    std::vector<std::string> names;
};

// retry INT [every D] [backoff F] [cap D] [on IDENT, ...]; backoff keeps the
// float spelling like every other float
struct RetryTerm {
    std::int64_t count;
    std::optional<std::int64_t> everyMilliseconds;
    std::optional<std::string> backoff;
    std::optional<std::int64_t> capMilliseconds;
    std::vector<std::string> on;
};

struct Term {
    std::int64_t lo;
    std::int64_t hi;
    std::variant<WithinTerm, AfterTerm, RetryTerm> kind;
};

// Declarations

struct Import {
    std::string path;
    std::int64_t lo;
    std::int64_t hi;
};

struct RecordDecl {
    std::string name;
    std::int64_t lo;
    std::int64_t hi;
    std::vector<Field> fields;
};

struct Variant {
    std::string name;
    std::vector<Field> fields;
};

struct SumDecl {
    std::string name;
    std::int64_t lo;
    std::int64_t hi;
    std::vector<Variant> variants;
};

struct ClauseSig {
    std::string name;
    std::vector<Field> params;
    TypeId ret;
};

struct ProvisionalDecl {
    bool isInternal;
    std::string name;
    std::int64_t lo;
    std::int64_t hi;
    std::vector<ClauseSig> sigs;
};

struct IncorporateDecl {
    std::string name;
    std::int64_t lo;
    std::int64_t hi;
};

struct VowDecl {
    std::string name;
    std::int64_t lo;
    std::int64_t hi;
    TypeId type;
    std::optional<ExprId> init;
};

struct PledgeDecl {
    std::string name;
    std::int64_t lo;
    std::int64_t hi;
    std::vector<Field> params;
    TypeId ret;
    std::vector<Term> terms;
    std::vector<Attr> attrs;
    std::optional<BlockId> body;
};

struct ClauseDecl {
    std::string name;
    std::int64_t lo;
    std::int64_t hi;
    std::vector<Field> params;
    TypeId ret;
    BlockId body;
};

// signal NAME(params) Terms, an outside event delivered into an instance
struct SignalDecl {
    std::string name;
    std::int64_t lo;
    std::int64_t hi;
    std::vector<Field> params;
    std::vector<Term> terms;
};

using SubMember = std::variant<PledgeDecl, SignalDecl>;

struct SubcontractDecl {
    std::optional<std::string> name;
    std::int64_t lo;
    std::int64_t hi;
    bool isTransactional;
    std::vector<Term> terms;
    std::vector<SubMember> members;
};

struct SchemaDecl {
    std::string name;
    std::int64_t lo;
    std::int64_t hi;
    std::vector<Field> columns;
};

struct RequirementsDecl {
    std::int64_t lo;
    std::int64_t hi;
    std::optional<ReqId> fulfill;
    std::optional<ReqId> partial;
    std::optional<ReqId> breakLine;
};

// remedy NAME for TARGET([param]) -> Type Terms [Attrs] [Block]
struct RemedyDecl {
    std::string name;
    std::string target;
    std::int64_t lo;
    std::int64_t hi;
    std::optional<Field> param;
    TypeId ret;
    std::vector<Term> terms;
    std::vector<Attr> attrs;
    std::optional<BlockId> body;
};

struct CourseDecl {
    std::int64_t lo;
    std::int64_t hi;
    BlockId body;
};

// The items of an amends block

struct AmendVow {
    std::string name;
    ExprId value;
};

struct AmendRename {
    std::string from;
    std::string to;
};

struct AmendRetire {
    std::string name;
};

enum class LatchState { Fulfilled, Pending, Broken };

struct AmendLatch {
    std::string name;
    LatchState state;
};

using AmendItem = std::variant<AmendVow, AmendRename, AmendRetire, AmendLatch>;

struct AmendsDecl {
    std::int64_t lo;
    std::int64_t hi;
    std::int64_t version;
    std::vector<AmendItem> items;
};

using Member = std::variant<IncorporateDecl, VowDecl, PledgeDecl, ClauseDecl, SubcontractDecl,
                            SchemaDecl, RequirementsDecl, RemedyDecl, SignalDecl, CourseDecl,
                            AmendsDecl>;

struct ContractDecl {
    bool isInternal;
    std::string name;
    std::int64_t lo;
    std::int64_t hi;
    std::vector<Attr> attrs;
    std::vector<Member> members;
};

using Decl = std::variant<RecordDecl, SumDecl, ProvisionalDecl, ContractDecl>;

// A parsed file and the arenas its ids point into
struct Ast {
    std::vector<Expr> exprs;
    std::vector<Stmt> stmts;
    std::vector<Type> types;
    std::vector<Pattern> patterns;
    std::vector<Block> blocks;
    std::vector<ReqNode> reqs;
    std::vector<Import> imports;
    std::vector<Decl> decls;

    ExprId addExpr(Expr expr);
    StmtId addStmt(Stmt stmt);
    TypeId addType(Type type);
    PatternId addPattern(Pattern pattern);
    BlockId addBlock(Block block);
    ReqId addReq(ReqNode req);

    const Expr& expr(ExprId id) const;
    const Stmt& stmt(StmtId id) const;
    const Type& type(TypeId id) const;
    const Pattern& pattern(PatternId id) const;
    const Block& block(BlockId id) const;
    const ReqNode& req(ReqId id) const;
};

}  // namespace geas
