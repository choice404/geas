// The type checker. It runs only over a program the resolver found nothing
// wrong with, so every name in reach is unique and every signature lookup
// lands on the declaration it was built from. One Checker rides through
// every body: the tables the earlier passes built, the contract and return
// type of the body being walked, the loop depth for break and continue, and
// whether the body is a course or an amends value, the two places with
// rules of their own. Scopes are a stack of maps from a local's name to its
// type and mutability. The member functions are spread over the check*.cpp
// files by area
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.hpp"
#include "checked.hpp"
#include "diagnostics.hpp"
#include "loader.hpp"
#include "quote.hpp"
#include "resolve.hpp"
#include "sigs.hpp"
#include "types.hpp"

namespace geas {

// A local binding, its type and whether it was declared let mut
struct Binding {
    TyId type;
    bool isMutable;
};

// What the arms of one match cover so far
struct Cover {
    bool hasCatchAll = false;
    bool hasTrue = false;
    bool hasFalse = false;
    bool hasSome = false;
    bool hasNone = false;
    bool hasOk = false;
    bool hasErr = false;
    std::unordered_set<std::string> variants;
};

// One Store call with the schema it names already resolved
struct StoreCall {
    const Expr* expr;
    const CallExpr* call;
    const SchemaDecl* schema;
    const RecordSig* row;
    std::string operation;
    TyId rowType;
    TyId keyType;
    TyId errorType;
};

// The declaration a term list trails, and which terms that kind accepts
struct TermOwner {
    std::string_view kind;
    std::string label;
    bool takesWithin;
    bool takesAfter;
    bool takesRetry;
    TyId ret;
};

class Checker {
public:
    Checker(const Program& program, const Resolved& resolved, const Sigs& sigs, TypeArena& types,
            TypeLowering& lowering, std::vector<Diagnostic>& diagnostics);

    /**
     * Checks every contract of the program in declaration order.
     * @returns {CheckTables} - the side tables code generation reads, faults land in the
     * diagnostics vector the checker was built with
     */
    CheckTables run() &&;

private:
    // Shared state and helpers, checker.cpp
    void emit(std::int64_t lo, std::int64_t hi, std::string message);
    void emitAt(const Expr& expr, std::string message);
    void emitInBody(std::string message);
    std::string typesMessage(std::string_view head, TyId first, std::string_view middle,
                             TyId second) const;
    TyId scalar(TyKind kind);
    TyId unknown();
    TyKind kindOf(TyId type) const;
    bool isCompatible(TyId a, TyId b) const;
    void pushScope();
    void popScope();
    void declare(const std::string& name, TyId type, bool isMutable);
    const Binding* lookup(std::string_view name) const;
    ExprId idOf(const Expr& expr) const;
    void markUse(const Expr& expr, ExprUse use);
    const CallableSig& pledgeSig(const PledgeDecl& pledge) const;
    const CallableSig& clauseSig(const ClauseDecl& clause) const;
    const CallableSig& signalSig(const SignalDecl& signal) const;
    const CallableSig& remedySig(const RemedyDecl& remedy) const;
    TyId vowType(const VowDecl& vow);
    const ContractDecl* contractNamed(std::string_view name) const;
    int packageOf(const ContractDecl& contract) const;
    bool hasCourse(const ContractDecl& contract) const;

    // Contracts and bodies, checkrun.cpp
    void checkContract(const ContractDecl& contract);
    void checkMember(const ContractDecl& contract, const Member& member);
    void checkIncorporate(const ContractDecl& contract, const IncorporateDecl& incorporate);
    std::string provisionalCanon(const ProvisionalDecl& provisional, std::string_view name) const;
    bool isConstantLiteral(ExprId id) const;
    void checkVow(const VowDecl& vow);
    void checkBody(const std::vector<Field>& params, const std::vector<TyId>& paramTypes, TyId ret,
                   BlockId body, std::int64_t lo, std::int64_t hi);
    void checkPledgeBody(const PledgeDecl& pledge);
    void checkClauseBody(const ClauseDecl& clause);
    bool hasSchema(const ContractDecl& contract) const;
    void checkSubcontract(const ContractDecl& contract, const SubcontractDecl& sub);

    // Statements and blocks, checkstmt.cpp
    void checkBlock(BlockId id);
    TyId blockValue(BlockId id, TyId want);
    void checkStmt(StmtId id);
    void checkCondition(ExprId id, std::string_view what);
    void checkLet(const LetStmt& let);
    void checkAssign(const AssignStmt& assign);
    void checkIndexAssign(const AssignStmt& assign, const Expr& target, const IndexExpr& index);
    void checkAssignedValue(ExprId value, TyId target);
    void checkAssignable(ExprId id);
    TyId lvalueType(ExprId id);
    TyId lvalueIndexType(const Expr& expr, const IndexExpr& index);
    TyId lvalueMemberType(const Expr& expr, const MemberExpr& member);
    void checkReturn(const ReturnStmt& statement);
    void checkIf(const IfStmt& statement);
    void checkWhile(const WhileStmt& loop);
    void checkFor(const ForStmt& loop);
    void checkLoopExit(std::string_view keyword);

    // Expressions, checkexpr.cpp
    TyId inferExpr(ExprId id, TyId want);
    TyId inferKind(const Expr& expr, TyId want);
    bool isNumeric(TyId type) const;
    bool isTemporal(TyId type) const;
    TyId inferInt(const Expr& expr, const IntLiteral& literal, TyId want);
    TyId inferIdent(const Expr& expr, const Identifier& ident);
    TyId inferMemberIdent(const Expr& expr, const std::string& name, const MemberEntry& member);
    TyId inferGlobalIdent(const Expr& expr, const std::string& name);
    TyId inferUndeclaredIdent(const Expr& expr, const std::string& name);
    TyId inferVariantIdent(const Expr& expr, const std::string& name);
    TyId inferUnary(const Expr& expr, const UnaryExpr& unary);
    TyId inferBinary(const Expr& expr, const BinaryExpr& binary, TyId want);
    TyId inferLogical(const Expr& expr, const BinaryExpr& binary);
    TyId inferEquality(const Expr& expr, const BinaryExpr& binary);
    TyId inferComparison(const Expr& expr, const BinaryExpr& binary);
    TyId inferArithmetic(const Expr& expr, const BinaryExpr& binary, TyId want);
    TyId inferTemporal(const Expr& expr, const BinaryExpr& binary, TyId left);
    TyId inferQuestion(const Expr& expr, const QuestionExpr& question);
    TyId inferIndex(const Expr& expr, const IndexExpr& index);
    TyId inferTuple(const TupleExpr& tuple, TyId want);
    TyId inferList(const ListExpr& list, TyId want);

    // Constructors, checkctor.cpp
    const FieldSig* recordField(std::string_view record, std::string_view field) const;
    void checkFieldInit(const RecordLiteral& literal, std::size_t index);
    TyId inferRecordLiteral(const Expr& expr, const RecordLiteral& literal);
    TyId inferWrap(const Expr& expr, const WrapExpr& wrap, TyId want);
    TyId inferSome(const Expr& expr, const WrapExpr& wrap, TyId want);
    TyId inferOkErr(const Expr& expr, const WrapExpr& wrap, TyId want);
    TyId inferNone(const Expr& expr, TyId want);
    void checkCtorArg(const Argument& arg, std::size_t index, TyId want, std::string_view ctor);
    TyId inferGenericNew(const Expr& expr, const GenericNewExpr& ctor);

    // Calls, checkcall.cpp
    void walkArgs(const std::vector<Argument>& args);
    void checkArgs(const std::vector<TyId>& params, const CallExpr& call, const Expr& expr,
                   std::string_view word, std::string_view name);
    void checkTwoStrings(const Expr& expr, const CallExpr& call, std::string_view name);
    TyId callPledgeValue(const Expr& expr, const CallExpr& call, TyId callee);
    TyId checkLen(const Expr& expr, const CallExpr& call);
    TyId callVariant(const Expr& expr, const CallExpr& call, const std::string& name);
    std::optional<TyId> callMemberName(const Expr& expr, const CallExpr& call,
                                       const std::string& name);
    TyId callIdent(const Expr& expr, const CallExpr& call, const Identifier& callee);
    std::string notCallableFault(const std::string& name, GlobalKind kind) const;
    TyId callValue(const Expr& expr, const CallExpr& call, TyId callee);
    TyId callMember(const Expr& expr, const CallExpr& call, const Expr& callee,
                    const MemberExpr& member);
    TyId inferCall(const Expr& expr, const CallExpr& call);

    // Contract names, signed instances, and member access, checkinstance.cpp
    void checkSignArg(const ContractDecl& target, const Argument& arg,
                      std::unordered_set<std::string>& supplied);
    TyId checkSign(const Expr& expr, const CallExpr& call, const ContractDecl& target);
    void checkInternalSign(const Expr& expr, const ContractDecl& target);
    TyId callStatic(const Expr& expr, const CallExpr& call, const ContractDecl& target,
                    const std::string& member);
    std::optional<TyId> callInstanceBuiltin(const Expr& expr, const CallExpr& call,
                                            const std::string& member);
    void refuseCoursePledge(const Expr& expr, const ContractDecl& target, std::string_view pledge);
    TyId callOnInstance(const Expr& expr, const CallExpr& call, TyId base,
                        const std::string& member);
    std::string noPledgeFault(const ContractDecl& target, const MemberEntry* entry,
                              std::string_view name) const;
    TyId instanceMember(const Expr& expr, const MemberExpr& member, TyId base);
    TyId memberWithBase(const Expr& expr, const MemberExpr& member, TyId base);
    const ContractDecl* staticContract(ExprId base) const;
    TyId inferMember(const Expr& expr, const MemberExpr& member);

    // Store operations, checkstore.cpp
    bool isStoreNamespace(ExprId base) const;
    const SchemaDecl* findSchema(std::string_view name) const;
    TyId columnType(const StoreCall& store, std::string_view name) const;
    void emitNoColumn(const StoreCall& store, const Expr& at, std::string_view column);
    std::string_view querySuffixName(ExprId id) const;
    void checkQueryTerm(const StoreCall& store, const BinaryExpr& comparison);
    void checkQueryPredicate(const StoreCall& store, ExprId id);
    void checkQueryOrder(const StoreCall& store, ExprId id);
    void checkQueryLimit(ExprId id);
    void checkStoreArg(const StoreCall& store, std::size_t index, TyId want, std::string_view what);
    TyId storeColumnArg(const StoreCall& store, bool needsNumber);
    bool isOrderSuffix(ExprId id) const;
    bool checkOrderedQuery(const StoreCall& store);
    void checkBoundedQuery(const StoreCall& store);
    TyId checkStoreQuery(const StoreCall& store);
    TyId checkStoreAggregate(const StoreCall& store);
    TyId checkStoreWrite(const StoreCall& store);
    const SchemaDecl* storeSchema(const Expr& expr, const CallExpr& call,
                                  const std::string& operation);
    TyId checkStoreOp(const Expr& expr, const CallExpr& call, const std::string& operation);

    // Match and patterns, checkmatch.cpp
    bool isSumType(TyId type) const;
    bool isVariantOf(TyId type, std::string_view name) const;
    bool bindPattern(PatternId id, TyId type, const Expr& at);
    bool bindName(const BindPattern& bind, TyId type, const Expr& at);
    void bindWrap(const WrapPattern& wrap, TyId type, const Expr& at);
    void bindVariant(const VariantPattern& pattern, TyId type, const Expr& at);
    bool hasIrrefutableFields(const Pattern& pattern) const;
    void coverPattern(PatternId id, TyId scrutinee, Cover& cover) const;
    void checkExhaustive(TyId scrutinee, const Cover& cover, const Expr& at);
    void checkSumCovered(TyId scrutinee, const Cover& cover, const Expr& at);
    TyId checkArmBody(const ArmBody& body, TyId want, const Expr& whole, std::string_view what);
    TyId checkMatch(const Expr& expr, const MatchExpr& match, TyId want);

    // The course and what only a course may hold, checkcourse.cpp
    void checkCourse(const CourseDecl& course);
    TyId signalPayload(const SignalDecl& signal);
    const SignalDecl* awaitedSignal(const Expr& expr, const std::string& name);
    TyId inferAwait(const Expr& expr, const AwaitExpr& await);
    TyId checkSelect(const Expr& expr, const SelectExpr& select, TyId want);
    void checkSleep(const SleepStmt& sleep);
    void checkTogether(const TogetherStmt& together);
    TyId checkNow(const Expr& expr, const CallExpr& call);

    // Terms and ordering, checkterms.cpp
    void checkWorkflowRules(const ContractDecl& contract);
    void checkTermList(const std::vector<Term>& terms, const TermOwner& owner);
    void checkRetry(const Term& term, const RetryTerm& retry, const TermOwner& owner);
    void checkRetryOn(const Term& term, const RetryTerm& retry, const TermOwner& owner);
    void resolveAfter(const ContractDecl& contract, const SubcontractDecl* enclosing,
                      const std::vector<Term>& terms, OrderTarget self, const std::string& label);
    void fileAfterTarget(const ContractDecl& contract, const SubcontractDecl* enclosing,
                         const Term& term, const std::string& name, OrderTarget self,
                         const std::string& label, std::vector<OrderTarget>& targets);
    std::optional<OrderTarget> afterTarget(const ContractDecl& contract,
                                           const SubcontractDecl* enclosing, const Term& term,
                                           const std::string& name);
    void refuseInTransaction(const SubcontractDecl& sub, const SubMember& member,
                             const std::string& label);
    void checkSubTerms(const ContractDecl& contract, const SubcontractDecl& sub);
    void checkOrdering(const ContractDecl& contract);

    // Attributes, remedies, and amends, checkworkflow.cpp
    void checkContractAttrs(const ContractDecl& contract);
    void refuseTermAttr(const std::vector<Attr>& attrs, const std::string& label);
    void checkRemedies(const ContractDecl& contract);
    void checkRemedy(const ContractDecl& contract, const RemedyDecl& remedy,
                     std::unordered_map<const PledgeDecl*, const RemedyDecl*>& taken);
    void checkRemedyParam(const RemedyDecl& remedy, const PledgeDecl& target);
    void checkRemedyBody(const RemedyDecl& remedy);
    void checkAmendsVersions(const ContractDecl& contract);
    void checkAmends(const ContractDecl& contract, const AmendsDecl& amends);
    void checkAmendVow(const ContractDecl& contract, const AmendsDecl& amends,
                       const AmendVow& item);
    void checkAmendName(const ContractDecl& contract, const AmendsDecl& amends,
                        const AmendItem& item);

    const Ast& ast_;
    const Resolved& resolved_;
    const Sigs& sigs_;
    TypeArena& types_;
    TypeLowering& lowering_;
    std::vector<Diagnostic>& diagnostics_;
    CheckTables tables_;
    std::vector<std::unordered_map<std::string, Binding>> scopes_;
    const ContractDecl* contract_ = nullptr;
    TyId returnType_;
    std::int64_t bodyLo_ = 0;
    std::int64_t bodyHi_ = 0;
    int loopDepth_ = 0;
    bool isInCourse_ = false;
    bool isInAmends_ = false;
};

}  // namespace geas
