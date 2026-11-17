// The module C emitter. Given a checked program it emits one C translation
// unit: a static function per clause, a static thunk per compiled pledge
// body in the uniform frame, the vow, pledge, schema, and requirements
// tables, one contract descriptor per public contract with its shape hash
// and version baked in, and the single exported geas_module_register. The
// emitted C includes geas/geas.h and nothing else defines a layout.
//
// One CEmitter rides through the module: the finished file, the body of the
// thunk being lowered, a scope stack binding a local's name to the C
// variable that holds it, and the counter fresh temporaries are named from.
// Every expression lowers to a fresh GeasValue temporary and hands back its
// name beside its checked type, so the parent reads fields off a value whose
// shape it knows. The codegen goldens pin the text byte for byte, so the
// order temporaries are named in is part of the contract.
// The member functions are spread over the emit*.cpp files by area
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "check.hpp"
#include "diagnostics.hpp"
#include "emitlookup.hpp"

namespace geas {

// One lowered expression, the C temporary that carries the value and its
// checked type. A local binding is the same pair
struct CValue {
    std::string name;
    TyId type;
};

// The resolved storage slot of an assignment target, a C GeasValue* aimed at
// the exact memory the assignment writes
struct LSlot {
    std::string pointer;
    TyId type;
};

// One hoisted pattern binding: the source name, the C local that carries the
// matched value, and its checked type
struct PatternBind {
    std::string name;
    std::string variable;
    TyId type;
};

// A string literal lowered to C, the quoted spelling and the byte length
struct CString {
    std::string literal;
    std::int64_t length;
};

// What the contract descriptor needs to reference the requirements arrays
struct RequirementCounts {
    std::int64_t subs = 0;
    std::int64_t atoms = 0;
    bool hasBlock = false;
    std::int64_t fulfill = 0;
    std::int64_t partial = 0;
    std::int64_t breakLine = 0;
};

// One step of a postfix requirements program
struct RequirementOp {
    int op;
    std::int64_t atom;
};

// One comparison of a Store predicate with every negation already folded
// into its operator
struct StoreTerm {
    BinaryOp op;
    ExprId column;
    ExprId value;
};

// A Store predicate in disjunctive normal form, an OR of AND groups
using StoreGroups = std::vector<std::vector<StoreTerm>>;

// One Store call being lowered: the row shape, the descriptor entry it
// names, and the temporaries its result and status land in
struct StoreOp {
    const Expr* expr;
    const CallExpr* call;
    const RecordSig* row;
    std::string schemaRef;
    std::string out;
    std::string status;
    TyId rowType;
    TyId keyType;
    TyId errorType;
};

/**
 * The C name of a compiled clause. Clauses are contract private, so the
 * function is static and the name needs no hash or version.
 * @params contract - the contract name, clause - the clause name
 * @returns {std::string} - the function name
 */
std::string clauseFunctionName(std::string_view contract, std::string_view clause);

/**
 * Whether a contract declares any of the workflow surface its descriptor
 * carries as the tail after sub_flags: a signal, a remedy, a term on any
 * pledge or subcontract, or the term attribute.
 * @params shape - the contract
 * @returns {bool} - true when the descriptor needs the tail
 */
bool hasWorkflowTail(const ContractShape& shape);

class CEmitter {
public:
    CEmitter(CheckedProgram& checked, std::vector<Diagnostic>& diagnostics);

    /**
     * Emits the whole module.
     * @returns {std::string} - the translation unit, to be thrown away when faults landed in the
     * diagnostics vector the emitter was built with
     */
    std::string emitModule() &&;

private:
    // Shared state and helpers, emitc.cpp
    const Ast& ast() const { return lookup_.ast(); }
    TypeArena& types() const { return lookup_.types(); }
    TyKind kindOf(TyId type) const { return lookup_.kindOf(type); }
    TyNode nodeOf(TyId type) const;
    TyId unknown() const { return types().unknown(); }
    std::string freshName();
    void line(std::string_view text);
    void top(std::string_view text);
    void pushScope();
    void popScope();
    void declare(const std::string& name, std::string variable, TyId type);
    const CValue* lookupLocal(std::string_view name) const;
    CValue badValue() const;
    std::string newValue();
    void setTag(std::string_view value, std::string_view tag);
    void returnUnlessOk(std::string_view status);
    void unsupportedAt(const Expr& expr, std::string_view what);
    bool isComposite(TyId type) const;
    CValue copyOut(const CValue& value);
    CString cString(std::string_view body) const;
    void beginBody(TyId ret, std::int64_t lo, std::int64_t hi);
    void endBody();

    // The module and its contracts, emitmodule.cpp
    void emitContract(const ContractShape& shape);
    void emitClausePrototypes(const std::vector<const ClauseDecl*>& clauses);
    void emitClause(const ClauseDecl& clause);
    void emitThunk(const PledgeShape& pledge);
    void emitThunkFrame(const PledgeDecl& pledge, const CallableSig& sig);
    void emitRegistrar(const std::vector<const ContractShape*>& contracts);

    // The descriptor tables, emitdesc.cpp
    void emitSchemas(const ContractShape& shape);
    void emitVows(const ContractShape& shape);
    std::string vowDefaultText(const VowDecl& vow, TyId type) const;
    void emitPledgeTable(const ContractShape& shape);
    std::string emitSubFlags(const ContractShape& shape);
    void emitAttrTail(const ContractShape& shape, std::vector<std::string>& tail);
    std::string attrValueText(ExprId value) const;
    void appendAttrEntries(const std::vector<Attr>& attrs, AttrTarget target, std::size_t index,
                           std::vector<std::string>& entries) const;
    void emitDescriptor(const ContractShape& shape, const RequirementCounts& counts,
                        std::string_view subFlagsRef, const std::vector<std::string>& tail);

    // The workflow tail, emitflow.cpp and emitremedy.cpp
    std::string itemRefText(const ContractShape& shape, const OrderTarget& target) const;
    std::string emitAfterArray(const ContractShape& shape, const Term* after,
                               std::string_view symbol, std::size_t& count);
    std::string emitRetryOnArray(const RetryTerm& retry, std::string_view symbol, std::int64_t lo,
                                 std::int64_t hi);
    void appendRetryFields(const RetryTerm& retry, std::string_view onSymbol, std::int64_t lo,
                           std::int64_t hi, std::vector<std::string>& fields);
    std::string termEntry(const ContractShape& shape, const std::vector<Term>& terms,
                          std::string_view stem, std::int64_t lo, std::int64_t hi);
    std::string emitKinds(const ContractShape& shape);
    std::string emitPledgeTerms(const ContractShape& shape);
    std::string emitSubTerms(const ContractShape& shape);
    void emitFrameChecks(const std::vector<Field>& params, const CallableSig& sig);
    void emitRemedyThunk(const RemedyShape& remedy);
    std::int64_t remedyTargetIndex(const ContractShape& shape, const RemedyShape& remedy) const;
    bool remedyTakesValue(const RemedyShape& remedy) const;
    std::string emitRemedyTable(const ContractShape& shape);
    std::vector<std::string> emitWorkflowTail(const ContractShape& shape);

    // The requirements surface, emitreq.cpp
    RequirementCounts emitRequirements(const ContractShape& shape);
    void emitSubNames(const ContractShape& shape);
    RequirementCounts emitSourceBlock(const ContractShape& shape);
    RequirementCounts emitDefaultBlock(const ContractShape& shape);
    void emitDefaultAtoms(const ContractShape& shape, const std::vector<std::int64_t>& loose);
    void emitAtomRow(std::optional<std::string_view> name, int kind, std::int64_t sub,
                     std::int64_t pledge);
    void emitSourceAtom(const ContractShape& shape, const std::string& name);
    void emitOps(std::string_view tag, const std::vector<RequirementOp>& ops);
    void buildPostfix(ReqId id, const ContractShape& shape, std::vector<RequirementOp>& ops) const;

    // Statements and blocks, emitstmt.cpp
    void genBlock(BlockId id);
    TyId genBlockInto(BlockId id, TyId want, std::string_view dest);
    void genStmt(StmtId id);
    void genLet(const LetStmt& let);
    void genReturn(const ReturnStmt& ret);
    void genIf(const IfStmt& branch);
    void genWhile(const WhileStmt& loop);
    void genFor(const ForStmt& loop);
    void genForBody(const ForStmt& loop, std::string_view iterable, std::string_view index,
                    TyId element);

    // Assignment and the lvalue walk, emitassign.cpp
    void genAssign(const AssignStmt& assign);
    void genIndexAssign(const AssignStmt& assign, const Expr& target, const IndexExpr& index);
    std::optional<LSlot> genLvalue(ExprId id);
    std::optional<LSlot> genMemberSlot(const Expr& expr, const MemberExpr& member);
    std::optional<LSlot> genIndexSlot(const Expr& expr, const IndexExpr& index);
    std::string genListSlot(std::string_view base, ExprId index);

    // Expressions, emitexpr.cpp
    CValue genExpr(ExprId id, TyId want);
    CValue genInt(const IntLiteral& literal, TyId want);
    CValue genScalar(std::string_view tag, std::string_view field, std::string_view text,
                     TyKind kind);
    CValue genString(const StringLiteral& literal);
    CValue genIdentifier(const Expr& expr, const Identifier& identifier);
    CValue genVowRead(const VowDecl& vow);
    CValue genUnary(const Expr& expr, const UnaryExpr& unary);
    CValue genBinary(const Expr& expr, const BinaryExpr& binary, TyId want);
    CValue genShortCircuit(const BinaryExpr& binary);
    CValue genEquality(const Expr& expr, const BinaryExpr& binary, const CValue& lhs,
                       const CValue& rhs, TyId operand);
    CValue genCompare(const BinaryExpr& binary, const CValue& lhs, const CValue& rhs,
                      std::string_view field);
    CValue genConcat(const CValue& lhs, const CValue& rhs);
    CValue genMapNew(const Expr& expr, const MapNewExpr& ctor);

    // Constructors and reads, emitvalue.cpp
    CValue genWrap(const WrapExpr& wrap, TyId want);
    CValue genSome(const WrapExpr& wrap, TyId want);
    CValue genOkErr(const WrapExpr& wrap, TyId want);
    CValue genBoxed(const CValue& payload, std::string_view tag, int variant, TyId type);
    CValue genNone(TyId want);
    CValue genList(const ListExpr& list, TyId want);
    CValue genTuple(const TupleExpr& tuple, TyId want);
    CValue genRecord(const Expr& expr, const RecordLiteral& record);
    std::string genFieldArray(std::size_t count);
    void adoptFieldArray(std::string_view value, std::string_view array, std::size_t count);
    CValue genVariant(const Expr& expr, std::string_view name, const std::vector<Argument>& args);
    CValue genMember(const Expr& expr, const MemberExpr& member);
    CValue genInstanceVow(const Expr& expr, const MemberExpr& member, const CValue& base);
    CValue genIndex(const Expr& expr, const IndexExpr& index);
    CValue genMapIndex(const CValue& base, const IndexExpr& index);
    CValue genQuestion(const Expr& expr, const QuestionExpr& question);

    // Calls, emitcall.cpp
    CValue genCall(const Expr& expr, const CallExpr& call);
    std::optional<CValue> genNamedCall(const Expr& expr, const CallExpr& call,
                                       const Identifier& callee);
    std::optional<CValue> genStaticCall(const Expr& expr, const CallExpr& call,
                                        const MemberExpr& callee);
    CValue genInstanceCall(const Expr& expr, const CallExpr& call, const MemberExpr& callee,
                           const CValue& base);
    std::vector<CValue> genArgs(const std::vector<TyId>& params, const std::vector<Argument>& args);
    std::string genValueArray(const std::vector<CValue>& values);
    CValue genLen(const CallExpr& call);
    CValue genBytesConversion(const CallExpr& call, std::string_view name);
    CValue genProcessCall(const CallExpr& call);
    CValue genBytesIndex(const CValue& base, const CValue& position);
    CValue genClauseCall(const ClauseDecl& clause, const CallExpr& call);
    CValue genSign(const Expr& expr, const CallExpr& call, const ContractDecl& contract);
    std::string genVowBindings(const CallExpr& call, const ContractDecl& contract);
    CValue genResume(const Expr& expr, const CallExpr& call, const ContractDecl& contract);
    CValue genInstanceHandle(std::string_view instance, const ContractDecl& contract);
    CValue genPledgeCall(const Expr& expr, const CallExpr& call, const MemberExpr& callee,
                         const CValue& base);
    CValue genBreakCall(const CValue& base);
    CValue genStatusCall(const CValue& base);
    CValue genParkCall(const CallExpr& call, const CValue& base);
    CValue genPartialCall(const CValue& base);

    // The Store operations, emitstore.cpp and emitstorequery.cpp
    CValue genStoreOp(const Expr& expr, const CallExpr& call, std::string_view operation);
    std::optional<StoreOp> openStoreOp(const Expr& expr, const CallExpr& call);
    CValue finishStoreOp(const StoreOp& op, std::string_view callText, TyId okType);
    CValue genStoreKeyed(const StoreOp& op, std::string_view primitive, bool takesRow, TyId okType);
    CValue genStoreInsert(const StoreOp& op);
    CValue genStoreCount(const StoreOp& op);
    CValue genStoreSum(const StoreOp& op);
    CValue genStoreAggregate(const StoreOp& op, std::string_view operation);
    CValue genStoreQuery(const StoreOp& op);
    CValue genStoreWhere(const StoreOp& op);
    CValue genStoreOrdered(const StoreOp& op, std::string_view direction, ExprId column);
    CValue genStorePage(const StoreOp& op);
    CValue genStoreEq(const StoreOp& op);
    void genStoreSelect(const StoreOp& op, const StoreGroups& groups, std::string_view orderColumn,
                        std::string_view direction, std::string_view limitRef);
    std::optional<FieldSlot> columnOf(const StoreOp& op, ExprId column) const;
    StoreGroups predicateGroups(ExprId id, bool isNegated) const;
    std::string genStoreTerm(const StoreOp& op, const StoreTerm& term);
    std::string genTermArray(const StoreOp& op, const std::vector<StoreTerm>& terms);
    void emitInitArray(std::string_view declaration, std::string_view array,
                       const std::vector<std::string>& items);
    std::string genGroupArrays(const StoreOp& op, const StoreGroups& groups, std::string& lengths);

    // Match, emitmatch.cpp
    CValue genMatch(const MatchExpr& match, TyId want);
    TyId genArms(const MatchExpr& match, const CValue& scrutinee, std::string_view result,
                 std::size_t index, TyId want);
    TyId genArmBody(const MatchArm& arm, const std::vector<PatternBind>& binds,
                    std::string_view result, TyId want);
    bool isVariantBind(std::string_view name, TyId scrutinee) const;
    bool isIrrefutable(PatternId id, TyId scrutinee) const;
    void collectBinds(PatternId id, TyId scrutinee, std::vector<PatternBind>& binds);
    std::size_t genPatternTest(PatternId id, std::string_view value, TyId type,
                               const std::vector<PatternBind>& binds, std::size_t cursor);
    std::size_t genWrapTest(const WrapPattern& wrap, std::string_view value, TyId type,
                            const std::vector<PatternBind>& binds, std::size_t cursor);
    std::size_t genVariantTest(const VariantPattern& pattern, std::string_view value,
                               const std::vector<PatternBind>& binds, std::size_t cursor);
    void genLiteralTest(const LiteralPattern& pattern, std::string_view value, TyId type);
    TyId wrapInner(WrapKind kind, TyId type) const;

    // The seam the workflow constructs land on, emitworkflow.cpp
    CValue genWorkflowExpr(const Expr& expr);
    void genWorkflowStmt(const Stmt& stmt);
    CValue genGenericNew(const Expr& expr, const GenericNewExpr& ctor);
    CValue genSignalCall(const CallExpr& call, const SignalDecl& signal, const CValue& base);

    EmitLookup lookup_;
    std::vector<Diagnostic> scratch_;
    TypeLowering lowering_;
    std::string out_;
    std::string body_;
    std::vector<std::unordered_map<std::string, CValue>> scopes_;
    std::int64_t nextTemporary_ = 0;
    int indent_ = 0;
    const ContractShape* shape_ = nullptr;
    TyId returnType_;
    std::int64_t spanLo_ = 0;
    std::int64_t spanHi_ = 0;
};

}  // namespace geas
