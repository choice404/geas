// The body level constructs of the workflow surface. A duration literal is
// an Int of milliseconds on the wire, a generic constructor builds the value
// its annotation names, and a signal delivered through an instance rides
// geas_signal. The course forms, await, select, now(), sleep, and together,
// lower only inside a course, through the course emitter in emitcourse.cpp;
// the checker holds them there, and the two seams below record a named
// build error should one ever reach a pledge or clause body
#include "emitc.hpp"
#include "emittext.hpp"

namespace geas {

namespace {

constexpr int tagSome = 1;

}  // namespace

CValue CEmitter::genWorkflowExpr(const Expr& expr) {
    if (const auto* duration = std::get_if<DurationLiteral>(&expr.kind)) {
        return genScalar("GEAS_TY_INT", "i", decimal(duration->milliseconds), TyKind::Duration);
    }
    if (const auto* ctor = std::get_if<GenericNewExpr>(&expr.kind)) {
        return genGenericNew(expr, *ctor);
    }
    unsupportedAt(expr, "a workflow expression in a pledge body");
    return badValue();
}

void CEmitter::genWorkflowStmt(const Stmt&) {
    lookup_.unsupported(spanLo_, spanHi_, "a workflow statement in a pledge body");
}

// List<T>(v, ...) is the list literal of its values, Option<T>() is None and
// Option<T>(v) is Some(v), Result<T, E>(r) is r itself, and Tuple<A, B>(a, b)
// is the tuple literal, each typed by the annotation rather than the values
CValue CEmitter::genGenericNew(const Expr& expr, const GenericNewExpr& ctor) {
    const TyId type = lowering_.lower(ctor.type, expr.lo, expr.hi);
    const TyNode node = nodeOf(type);
    std::vector<ExprId> values;
    for (const Argument& arg : ctor.args) {
        values.push_back(arg.value);
    }
    switch (node.kind) {
        case TyKind::List: return genList(ListExpr{values}, type);
        case TyKind::Tuple: return genTuple(TupleExpr{values}, type);
        case TyKind::Result: {
            const CValue inner = genExpr(values.front(), type);
            return CValue{inner.name, type};
        }
        case TyKind::Option: {
            if (values.empty()) {
                return genNone(type);
            }
            const CValue payload = genExpr(values.front(), node.args[0]);
            return genBoxed(payload, "GEAS_TY_OPTION", tagSome, type);
        }
        default: break;
    }
    unsupportedAt(expr, "a generic constructor of this type");
    return badValue();
}

// instance.signal(args): the event delivered into the callee instance, Unit
// as the value, and a refusal, a signal not yet eligible or already lapsed,
// a contract no longer live, faulting the body like any lifecycle call
CValue CEmitter::genSignalCall(const CallExpr& call, const SignalDecl& signal,
                               const CValue& base) {
    const CallableSig sig = lookup_.signalSig(signal);
    const std::vector<CValue> values = genArgs(sig.params, call.args);
    const std::string array = genValueArray(values);
    const std::string status = freshName();
    line(joined("GeasStatus ", status, " = geas_signal((GeasContract*)", base.name, ".as.box, \"",
                signal.name, "\", ", array, ", ", decimal(static_cast<std::int64_t>(values.size())),
                ");"));
    returnUnlessOk(status);
    return CValue{newValue(), types().scalar(TyKind::Unit)};
}

}  // namespace geas
