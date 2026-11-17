// The workflow grammar: terms trailing a signature, remedies, signals, the
// course, and amends blocks. A term keyword reads across a newline, so each
// term can sit on its own line under its signature, while the contextual
// retry options every, backoff, cap, and on stay on the retry's line. Which
// declaration takes which terms is the checker's business, the parser
// accepts any of them anywhere terms may go
#include "parser.hpp"

#include <utility>

#include "duration.hpp"
#include "lexdump.hpp"
#include "size.hpp"

namespace geas {

namespace {

// The state a latch item names, or nullopt for any other word
std::optional<LatchState> latchStateNamed(std::string_view word) {
    if (word == "fulfilled") {
        return LatchState::Fulfilled;
    }
    if (word == "pending") {
        return LatchState::Pending;
    }
    if (word == "broken") {
        return LatchState::Broken;
    }
    return std::nullopt;
}

}  // namespace

std::vector<Term> Parser::parseTerms() {
    std::vector<Term> terms;
    while (!isHalted()) {
        if (at(TokenKind::KwWithin)) {
            terms.push_back(parseWithin());
        } else if (at(TokenKind::KwAfter)) {
            terms.push_back(parseAfter());
        } else if (at(TokenKind::KwRetry)) {
            terms.push_back(parseRetry());
        } else {
            break;
        }
    }
    return terms;
}

// within DURATION
Term Parser::parseWithin() {
    const std::int64_t lo = current().lo;
    bump();
    const std::int64_t milliseconds = expectDuration("a duration after 'within'");
    return Term{lo, previousHi(), WithinTerm{milliseconds}};
}

// after IDENT { "," IDENT }
Term Parser::parseAfter() {
    const std::int64_t lo = current().lo;
    bump();
    std::vector<std::string> names =
        parseNameList("a pledge, subcontract, or signal name after 'after'");
    return Term{lo, previousHi(), AfterTerm{std::move(names)}};
}

// retry INT [ every DURATION ] [ backoff FLOAT ] [ cap DURATION ] [ on IDENT, ... ]
Term Parser::parseRetry() {
    const std::int64_t lo = current().lo;
    bump();
    RetryTerm retry{0, std::nullopt, std::nullopt, std::nullopt, {}};
    if (at(TokenKind::Int) || at(TokenKind::Size)) {
        retry.isSizeCount = at(TokenKind::Size);
        retry.count = retry.isSizeCount ? parseSize(tokenText(current())).value_or(0)
                                       : intValue(text_, current().lo, current().hi);
        bump();
    } else {
        error(expectedFound("the number of retries after 'retry'"));
    }
    parseRetryOptions(retry);
    return Term{lo, previousHi(), std::move(retry)};
}

// The optional retry clauses, in grammar order and on the retry's line
void Parser::parseRetryOptions(RetryTerm& retry) {
    const auto atOption = [&](std::string_view word) {
        return atIdentText(word) && !isNewlineTerminator();
    };
    if (atOption("every")) {
        bump();
        retry.everyMilliseconds = expectDuration("a duration after 'every'");
    }
    if (atOption("backoff")) {
        bump();
        if (at(TokenKind::Float)) {
            retry.backoff = std::string{tokenText(current())};
            bump();
        } else {
            error(expectedFound("a float multiplier after 'backoff'"));
            skipStrayValue();
        }
    }
    if (atOption("cap")) {
        bump();
        retry.capMilliseconds = expectDuration("a duration after 'cap'");
    }
    if (atOption("on")) {
        bump();
        retry.on = parseNameList("an error variant name after 'on'");
    }
}

// IDENT { "," IDENT }, or the message and an empty list when no name opens it
std::vector<std::string> Parser::parseNameList(std::string_view what) {
    std::vector<std::string> names;
    if (!at(TokenKind::Ident)) {
        error(expectedFound(what));
        return names;
    }
    names.push_back(ident());
    while (eat(TokenKind::Comma)) {
        names.push_back(ident());
    }
    return names;
}

// A duration literal's milliseconds, or 0 and a fault. A stray value on the
// same line, a bare number or a name, is stepped over so it reports once
std::int64_t Parser::expectDuration(std::string_view what) {
    if (!at(TokenKind::Duration)) {
        error(expectedFound(what));
        skipStrayValue();
        return 0;
    }
    const std::int64_t milliseconds = parseDuration(tokenText(current())).value_or(0);
    bump();
    return milliseconds;
}

// Steps over an integer, a size, a float, or a name sitting on the current line, the
// recovery after a term option took the wrong kind of value
void Parser::skipStrayValue() {
    const bool isValue = at(TokenKind::Int) || at(TokenKind::Size) ||
                         at(TokenKind::Float) || at(TokenKind::Ident);
    if (isValue && !isNewlineTerminator()) {
        bump();
    }
}

// remedy IDENT for IDENT "(" [ Param ] ")" "->" Type Terms [ Attrs ] [ Block ]
RemedyDecl Parser::parseRemedy() {
    const std::int64_t lo = current().lo;
    bump();
    std::string name = ident();
    std::string target = parseRemedyTarget();
    const std::int64_t hi = previousHi();
    std::optional<Field> param = parseRemedyParam();
    const TypeId ret = parseReturnType("a remedy declares its return type with '->'");
    std::vector<Term> terms = parseTerms();
    std::vector<Attr> attrs = parseAttrsOnLine();
    const std::optional<BlockId> body = parseBodyOnLine();
    return RemedyDecl{std::move(name),  std::move(target), lo,   hi, std::move(param), ret,
                      std::move(terms), std::move(attrs),  body};
}

// for IDENT. Without the for, a name right before the parameter list is
// still taken as the target so the rest of the remedy parses cleanly
std::string Parser::parseRemedyTarget() {
    if (eat(TokenKind::KwFor)) {
        return ident();
    }
    error(expectedFound("'for' and the pledge this remedy undoes"));
    if (at(TokenKind::Ident) && nextIs(TokenKind::LParen)) {
        return ident();
    }
    return "";
}

// The single parameter, the Ok value of the target pledge
std::optional<Field> Parser::parseRemedyParam() {
    std::vector<Field> params = parseParams();
    if (params.size() > 1) {
        errorSpan(previousHi() - 1, previousHi(),
                  "a remedy takes at most one parameter, the Ok value of its target");
    }
    if (params.empty()) {
        return std::nullopt;
    }
    return std::move(params.front());
}

// signal IDENT "(" [ Params ] ")" Terms. A body is reported, then parsed and
// discarded so the parse resumes after it
SignalDecl Parser::parseSignal() {
    const std::int64_t lo = current().lo;
    bump();
    std::string name = ident();
    const std::int64_t hi = previousHi();
    std::vector<Field> params = parseParams();
    std::vector<Term> terms = parseTerms();
    std::vector<Attr> attrs = parseAttrsOnLine();
    if (at(TokenKind::LBrace) && !hasNewlineHere()) {
        error("a signal has no body; the host delivers it into the instance");
        parseBlock();
    }
    return SignalDecl{std::move(name), lo, hi, std::move(params), std::move(terms),
                      std::move(attrs)};
}

// course Block
CourseDecl Parser::parseCourse() {
    const std::int64_t lo = current().lo;
    bump();
    const BlockId body = parseBlock();
    return CourseDecl{lo, previousHi(), body};
}

// amends INT "{" { AmendItem NL } "}"
AmendsDecl Parser::parseAmends() {
    const std::int64_t lo = current().lo;
    bump();
    std::int64_t version = 0;
    const bool isSizeVersion = at(TokenKind::Size);
    if (at(TokenKind::Int) || at(TokenKind::Size)) {
        version = isSizeVersion ? parseSize(tokenText(current())).value_or(0)
                                : intValue(text_, current().lo, current().hi);
        bump();
    } else {
        error(expectedFound("the version number this block amends from"));
    }
    const std::int64_t hi = previousHi();
    std::vector<AmendItem> items;
    expect(TokenKind::LBrace, "'{' to open the amends block");
    while (!at(TokenKind::RBrace) && !atEnd()) {
        const std::size_t before = pos_;
        parseAmendItem(items);
        expectSeparator("expected a newline after the amends item");
        guardProgress(before, "unexpected token in the amends block");
    }
    expectClose("the amends block is not closed; expected '}' before end of input");
    return AmendsDecl{lo, hi, version, std::move(items), isSizeVersion};
}

// One item, an unknown one reported and its line skipped
void Parser::parseAmendItem(std::vector<AmendItem>& items) {
    if (at(TokenKind::KwVow)) {
        items.push_back(parseAmendVow());
        return;
    }
    if (atIdentText("rename")) {
        items.push_back(parseAmendRename());
        return;
    }
    if (atIdentText("retire")) {
        bump();
        items.emplace_back(AmendRetire{ident()});
        return;
    }
    if (atIdentText("latch")) {
        items.push_back(parseAmendLatch());
        return;
    }
    if (atIdentText("restart")) {
        parseAmendRestart(items);
        return;
    }
    error(expectedFound(
        "an amends item, 'vow', 'rename', 'retire', 'latch', or 'restart course'"));
    skipRestOfLine();
}

// restart course. restart is a keyword inside an amends block alone, and
// course is the only thing it restarts
void Parser::parseAmendRestart(std::vector<AmendItem>& items) {
    bump();
    if (at(TokenKind::KwCourse)) {
        bump();
        items.emplace_back(AmendRestart{});
        return;
    }
    error(expectedFound("'course' after 'restart'"));
    skipRestOfLine();
}

// vow IDENT "=" Expr
AmendItem Parser::parseAmendVow() {
    bump();
    std::string name = ident();
    expect(TokenKind::Assign, "'=' between the vow name and its value");
    const ExprId value = parseExpr();
    return AmendVow{std::move(name), value};
}

// rename IDENT "->" IDENT
AmendItem Parser::parseAmendRename() {
    bump();
    std::string from = ident();
    expect(TokenKind::Arrow, "'->' between the old name and the new one");
    std::string to = ident();
    return AmendRename{std::move(from), std::move(to)};
}

// latch IDENT as ( fulfilled | pending | broken ). A bad state is reported
// and stepped over, the item keeps pending as its placeholder
AmendItem Parser::parseAmendLatch() {
    bump();
    std::string name = ident();
    if (atIdentText("as")) {
        bump();
    } else {
        error(expectedFound("'as' after the latched name"));
    }
    const std::optional<LatchState> state =
        at(TokenKind::Ident) ? latchStateNamed(tokenText(current())) : std::nullopt;
    if (state) {
        bump();
        return AmendLatch{std::move(name), *state};
    }
    error(expectedFound("a latch state, 'fulfilled', 'pending', or 'broken'"));
    if (!at(TokenKind::RBrace) && !isNewlineTerminator()) {
        bump();
    }
    return AmendLatch{std::move(name), LatchState::Pending};
}

}  // namespace geas
