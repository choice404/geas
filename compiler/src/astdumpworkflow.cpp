// The dump of the workflow declarations: terms, remedies, signals, the
// course, and amends blocks. Durations print as Duration(ms), the same
// shape a duration literal takes in an expression, and absent options as None
#include "astdumper.hpp"

#include <string>
#include <variant>

namespace geas {

namespace {

std::string_view latchStateName(LatchState state) {
    switch (state) {
        case LatchState::Fulfilled: return "Fulfilled";
        case LatchState::Pending: return "Pending";
        case LatchState::Broken: return "Broken";
    }
    return "Pending";
}

}  // namespace

void AstDumper::renderTerms(const std::vector<Term>& terms, int level) {
    list(terms, level, [&](const Term& term, int inner) { renderTerm(term, inner); });
}

// Within(Duration(ms)), After([ names ]), or Retry { ... }
void AstDumper::renderTerm(const Term& term, int level) {
    std::visit(Overloaded{
                   [&](const WithinTerm& within) {
                       write("Within(");
                       renderDuration(within.milliseconds);
                       write(")");
                   },
                   [&](const AfterTerm& after) {
                       write("After(");
                       renderNames(after.names, level);
                       write(")");
                   },
                   [&](const RetryTerm& retry) { renderRetry(retry, level); },
               },
               term.kind);
}

void AstDumper::renderRetry(const RetryTerm& retry, int level) {
    openStruct("Retry");
    key(level + 1, "count");
    write(retry.isSizeCount ? "Size(" + std::to_string(retry.count) + ")"
                            : std::to_string(retry.count));
    endField();
    key(level + 1, "every");
    renderOptionalDuration(retry.everyMilliseconds);
    endField();
    key(level + 1, "backoff");
    if (retry.backoff) {
        write("Float(");
        quoted(*retry.backoff);
        write(")");
    } else {
        write("None");
    }
    endField();
    key(level + 1, "cap");
    renderOptionalDuration(retry.capMilliseconds);
    endField();
    key(level + 1, "on");
    renderNames(retry.on, level + 1);
    endField();
    closeStruct(level);
}

void AstDumper::renderNames(const std::vector<std::string>& names, int level) {
    list(names, level, [&](const std::string& name, int) { quoted(name); });
}

void AstDumper::renderDuration(std::int64_t milliseconds) {
    write("Duration(");
    write(std::to_string(milliseconds));
    write(")");
}

void AstDumper::renderOptionalDuration(const std::optional<std::int64_t>& milliseconds) {
    if (milliseconds) {
        renderDuration(*milliseconds);
    } else {
        write("None");
    }
}

void AstDumper::renderRemedy(const RemedyDecl& remedy, int level) {
    openStruct("Remedy");
    key(level + 1, "name");
    quoted(remedy.name);
    endField();
    key(level + 1, "target");
    quoted(remedy.target);
    endField();
    key(level + 1, "param");
    if (remedy.param) {
        renderField(*remedy.param, level + 1);
    } else {
        write("None");
    }
    endField();
    key(level + 1, "ret");
    renderType(remedy.ret, level + 1);
    endField();
    key(level + 1, "terms");
    renderTerms(remedy.terms, level + 1);
    endField();
    key(level + 1, "attrs");
    renderAttrs(remedy.attrs, level + 1);
    endField();
    key(level + 1, "body");
    renderOptionalBlock(remedy.body, level + 1);
    endField();
    closeStruct(level);
}

void AstDumper::renderSignal(const SignalDecl& signal, int level) {
    openStruct("Signal");
    key(level + 1, "name");
    quoted(signal.name);
    endField();
    key(level + 1, "params");
    renderFields(signal.params, level + 1);
    endField();
    key(level + 1, "terms");
    renderTerms(signal.terms, level + 1);
    endField();
    if (!signal.attrs.empty()) {
        key(level + 1, "attrs");
        renderAttrs(signal.attrs, level + 1);
        endField();
    }
    closeStruct(level);
}

void AstDumper::renderCourse(const CourseDecl& course, int level) {
    openStruct("Course");
    key(level + 1, "body");
    renderBlock(course.body, level + 1);
    endField();
    closeStruct(level);
}

void AstDumper::renderAmends(const AmendsDecl& amends, int level) {
    openStruct("Amends");
    key(level + 1, "version");
    write(amends.isSizeVersion ? "Size(" + std::to_string(amends.version) + ")"
                              : std::to_string(amends.version));
    endField();
    key(level + 1, "items");
    list(amends.items, level + 1,
         [&](const AmendItem& item, int inner) { renderAmendItem(item, inner); });
    endField();
    closeStruct(level);
}

// AmendVow { name, value }, Rename { from, to }, Retire { name }, Latch { name, state },
// and RestartCourse, a leaf
void AstDumper::renderAmendItem(const AmendItem& item, int level) {
    if (std::holds_alternative<AmendRestart>(item)) {
        write("RestartCourse");
        return;
    }
    const auto namedStruct = [&](std::string_view kind, std::string_view nameKey,
                                 const std::string& name) {
        openStruct(kind);
        key(level + 1, nameKey);
        quoted(name);
        endField();
    };
    std::visit(Overloaded{
                   [&](const AmendVow& vow) {
                       namedStruct("AmendVow", "name", vow.name);
                       key(level + 1, "value");
                       renderExpr(vow.value, level + 1);
                   },
                   [&](const AmendRename& rename) {
                       namedStruct("Rename", "from", rename.from);
                       key(level + 1, "to");
                       quoted(rename.to);
                   },
                   [&](const AmendRetire& retire) { namedStruct("Retire", "name", retire.name); },
                   [&](const AmendLatch& latch) {
                       namedStruct("Latch", "name", latch.name);
                       key(level + 1, "state");
                       write(latchStateName(latch.state));
                   },
                   [](const AmendRestart&) {},
               },
               item);
    if (!std::holds_alternative<AmendRetire>(item)) {
        endField();
    }
    closeStruct(level);
}

}  // namespace geas
