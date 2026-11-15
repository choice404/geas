# The Geas Grammar

This is the normative grammar for Geas. It covers the lexical structure, the type grammar, every declaration form including the workflow surface, terms, remedies, signals, the course, and amendment, and the statement and expression core that clause, pledge, remedy, and course bodies are written in. Where the grammar alone cannot pin a rule down, a semantic note next to the production carries the rest. The parser and the checker are held to this document, and a change to the surface lands here first.

Most of the workflow surface runs today in the host driven mode: durations, terms, remedies, signals, and the contract term are parsed, checked, emitted, and enforced by the runtime. The course and amends blocks, along with `await`, `select`, `sleep`, `together`, and `now()`, are parsed and checked in full, and their emission is the next step on the roadmap; until it lands, `geas build` refuses a program that uses them with `geas: workflow constructs are not emitted yet: <what>`. Their sections say so at the top.

## Notation

Productions are written in EBNF.

```text
::=     is defined as
|       alternation
{ x }   zero or more
[ x ]   optional
( x )   grouping
"x"     literal terminal
NL      a line break that ends something, see below
```

Nonterminals are CamelCase. Token classes are UPPERCASE.

## Source files and line breaks

A source file is UTF-8 and ends in `.geas`. A string literal that is not valid UTF-8 is a compile error, not a silent replacement.

Statements and declarations are terminated by a line break. There are no semicolons. `NL` in the productions below means one or more line breaks, with comment only lines counting as blank.

So the way the line break works is simple once you see where it lives. The lexer never emits a newline token; it marks every token with whether a line break came before it, and the parser decides, at each spot where something could end, whether that mark ends it. The rules the parser holds:

- Inside `(` or `[` a line break ends nothing, so a long signature, an argument list, or a list literal wraps freely. A `{` resets that, so a block inside an argument list holds statements again and its line breaks terminate.
- A line that ends in a token that still needs its right side, a binary operator, `=`, `->`, `,`, `.`, or `:`, carries on to the next line, because the parser reads that right side wherever it sits.
- A line that starts with a binary operator does not continue the line above; the expression above ended at the line break. Break a long expression after the operator, never before it.
- A few tokens continue the previous line even at its start: the term keywords `within`, `after`, and `retry` under a signature, `or` in a sum declaration, and `else` after an `if` block. That is what lets terms stack one per line under a pledge and lets `}` and `else {` sit on separate lines.
- The options of a `retry` term, `every`, `backoff`, `cap`, and `on`, stay on the `retry`'s own line.
- An attribute list `[...]` and a body `{` open on the line their declaration ends on, which for a pledge with terms is the line of its last term. A `{` on a line of its own is a stray block, not a body.

Nesting depth is capped. A parser that hits the ceiling reports a diagnostic and stops rather than hanging or overflowing its stack.

## Comments

```text
// line comment, to end of line
/* block comment, no nesting */
```

## Tokens

The lexer produces exactly these kinds, each carrying its source span and the line break mark described above.

| Class | Kinds |
| ----- | ----- |
| Names and literals | `IDENT`, `INT`, `FLOAT`, `DURATION`, `STRING`, `CHAR` |
| Grouping | `(` `)` `[` `]` `{` `}` |
| Punctuation | `,` `.` `:` `?` `->` `_` `=` |
| Arithmetic | `+` `-` `*` `/` `%` |
| Logic and comparison | `!` `&&` `\|\|` `==` `!=` `<` `<=` `>` `>=` |
| Keywords | one kind per hard keyword, below |
| End | `EOF` |

The lexer takes the longest token that fits: `->` over `-`, `==` over `=`, `!=` over `!`, `<=` over `<`, `>=` over `>`, and `ms` over `m` inside a duration. A lone `&` or `|` is a lex error, there are no bitwise operators. There is no shift operator, so `>>` is never a token and `List<List<Int>>` closes on two `>` tokens. `_` alone is its own token; `_x` is an `IDENT`.

Lex errors are reported with their span and then skipped past, so one bad character does not end the file: an unterminated string, char, or block comment, a raw line break inside a string, an invalid escape, a char literal holding zero or several scalar values, invalid UTF-8, an integer above the `Int` maximum, a duration with units out of order, repeated, or unknown, and a stray character.

## Identifiers and keywords

```text
IDENT ::= (letter | "_") { letter | digit | "_" }
```

Hard keywords, reserved everywhere, the twenty nine the language has always had:

```text
contract    vow         pledge      clause      provisional
subcontract requirements incorporate internal   record
import      let         mut         return      if
else        match       while       for         in
break       continue    is          either      or
true        false       schema      transactional
```

and the eleven the workflow surface adds:

```text
remedy      signal      course      await       sleep
together    select      amends      within      after
retry
```

`after`, `select`, and `retry` are common words, and being hard keywords they cannot name a binding, a field, or a pledge; a program that used one as a name picks another.

Contextual keywords, ordinary identifiers except in the positions named:

- `fulfill` and `partial` are labels inside a `requirements` block.
- `every`, `backoff`, `cap`, and `on` are options inside a `retry` term.
- `as` and `timeout` belong to `select` arms.
- `rename`, `retire`, `latch`, `as`, and `prior` belong to an `amends` block, and `fulfilled`, `pending`, and `broken` follow `latch NAME as`.
- `version`, `term`, `abi`, and `symbol` are attribute keys.
- `sign`, `resume`, `status`, `park`, and `partial` are builtin contract methods.
- `List`, `Map`, `Option`, `Result`, and `Tuple` open type arguments, see Expressions.
- `len` is a builtin in callee position, and `now` is a builtin in callee position inside a course.

One carve out: after `.` any keyword is accepted as a member name. That is what lets `payment.break()` parse while `break` stays a hard keyword for loop control.

## Literals

```text
INT      ::= decimal | "0x" hex | "0b" binary        // underscores allowed as separators
FLOAT    ::= digits "." digits [ ("e"|"E") ["+"|"-"] digits ]
DURATION ::= DurPart { DurPart }
DurPart  ::= digits ( "d" | "h" | "m" | "s" | "ms" )
STRING   ::= '"' { char | escape } '"'
CHAR     ::= "'" ( char | escape ) "'"
BOOL     ::= "true" | "false"
escape   ::= "\n" | "\t" | "\r" | "\0" | "\\" | "\"" | "\'" | "\u{" hex "}"
```

An `INT` literal types as `Int` unless the expected type at its position is `UInt` or `Byte` and the value fits. A `FLOAT` literal types as `Float`. `CHAR` holds one Unicode scalar value. There is no implicit numeric conversion anywhere, widening included.

A `DURATION` is one token, `30s`, `250ms`, `1h30m`, `7d`, with no whitespace inside it, and types as `Duration`. Its units run from largest to smallest, `d h m s ms`, each at most once, so `30m1h` and `1h1h` are lex errors. After the digits of a decimal literal the lexer looks at what follows: a `.` and a digit make a `FLOAT`, a run of letters that is exactly a unit makes a `DURATION`, any other letter run is a lex error, so `5mx` and `3days` are refused rather than split into a number and a name, and anything else leaves an `INT`. A literal with a `0x` or `0b` prefix is never a duration, so `0x1d` is an `INT`.

A raw line break inside a string literal is an error; a string that needs one writes `\n`. An integer literal above the `Int` maximum is a lex error, so the largest `UInt` values are not writable as literals yet. A `\u{...}` escape naming a surrogate or a value above `0x10FFFF` is an error.

The fixed representations, identical on every target:

| Type     | Representation              |
| -------- | --------------------------- |
| Int      | 64 bit signed               |
| UInt     | 64 bit unsigned             |
| Float    | 64 bit IEEE 754             |
| Bool     | 1 byte, 0 or 1              |
| Byte     | 8 bit unsigned              |
| Char     | 32 bit Unicode scalar value |
| String   | UTF-8, pointer plus byte length, no terminator |
| Duration | 64 bit signed milliseconds  |
| Time     | 64 bit signed milliseconds since the Unix epoch |

## Types

```text
Type       ::= NamedType | TupleType | PledgeType
NamedType  ::= IDENT [ "<" Type { "," Type } ">" ]
TupleType  ::= "Tuple" "<" Type { "," Type } ">"
PledgeType ::= "pledge" "(" [ Type { "," Type } ] ")" "->" Type
```

The builtin named types are `Int`, `UInt`, `Float`, `Bool`, `Byte`, `Char`, `String`, `Unit`, `Duration`, `Time`, `List<T>`, `Map<K, V>`, `Option<T>`, `Result<T, E>`, and `Tuple<...>`. Type arguments belong to the builtin composites only. A user declaration does not take type parameters. Every instantiation is monomorphized, and only instantiations reachable from a pledge signature get an ABI descriptor.

`Map<K, V>` keys are restricted to `Int`, `UInt`, `Bool`, `Byte`, `Char`, and `String`.

`Duration` and `Time` are scalars of their own to the checker and plain milliseconds to everything past it: both cross the boundary as an `Int` count of milliseconds, a `Duration` a span and a `Time` a point since the Unix epoch, which is what the runtime clock answers. They hash and mangle apart from `Int`, so a pledge that takes a `Duration` and one that takes an `Int` are different shapes even though their frames look the same. Their arithmetic is closed and small:

| Expression | Type |
| ---------- | ---- |
| `Duration + Duration`, `Duration - Duration` | `Duration` |
| `Duration * Int` | `Duration` |
| `-Duration` | `Duration` |
| `Time + Duration`, `Time - Duration` | `Time` |
| `Time - Time` | `Duration` |

Two `Duration` values or two `Time` values compare with `==`, `!=`, `<`, `<=`, `>`, and `>=`. Every other pairing is a type error: `Duration * Float`, `Duration + Int`, `Int * Duration`, `Time + Time`, and comparing a `Duration` with a `Time` among them. A `Time` comes from outside, a vow, a parameter, a signal payload, or `now()` inside a course; there is no `Time` literal.

A `PledgeType` is the type of a first class pledge value. The value is always bound to a signed contract instance, so calling one fulfills that pledge on that instance.

A contract name used as a type denotes a signed instance, and an instance never crosses the boundary. A contract type is therefore rejected anywhere a value would cross it: a pledge, remedy, or signal parameter or return, a vow, a record or sum field, and a provisional clause signature, at any nesting depth. Clause parameters, clause returns, and local bindings may carry contract types freely, since they never leave the module.

## Program structure

```text
File        ::= { Import NL } { TopDecl NL }
Import      ::= "import" IDENT { "." IDENT }
TopDecl     ::= RecordDecl | SumDecl | ProvClause | ContractDecl
```

A file is a module. A directory is a package. `import std.math` brings a module into scope by its dotted path. `internal` on a contract or provisional clause keeps it inside its package and out of the descriptor table.

## Attributes

```text
Attrs ::= "[" IDENT ":" Literal { "," IDENT ":" Literal } "]"
```

An attribute list trails a declaration header, after any terms and on the same line. Unknown keys are compile errors. The known keys:

- `version: INT` on a contract. Defaults to 1. Baked into every mangled name the contract produces.
- `term: DURATION` on a contract, its whole lifetime counted from sign. The duration must be greater than zero, and the key belongs on a contract only, a pledge or remedy carrying it is an error. When the term runs out while the contract is Signed or Partial, the contract breaks the way a matched break line breaks it, remedies included; a contract already Fulfilled met its terms and is left alone.
- `abi: STRING` and `symbol: STRING` on a pledge or a remedy. Binds it to a foreign symbol. `"c"` is the only ABI.

## Data declarations

```text
RecordDecl ::= "record" IDENT "{" NL { FieldDecl NL } "}"
FieldDecl  ::= IDENT ":" Type

SumDecl    ::= IDENT "is" "either" Variant { "or" Variant }
Variant    ::= IDENT [ "(" FieldDecl { "," FieldDecl } ")" ]
```

A `record` is plain data with no lifecycle, copyable across the ABI. A sum declaration reads the way the builtin ones do, and since `or` continues a line, a long one wraps with each variant on its own line:

```text
record Card {
    number: String
    expiry: String
}

LogLevel is either Debug or Info or Error

PayError is either Declined(reason: String)
    or Timeout
    or Gateway(code: Int)
```

Both get a deterministic C compatible layout, a tag plus a union for sums, the tag being the variant's declaration index. Error types are ordinary sums used in the `E` slot of `Result`.

## Provisional clauses

```text
ProvClause ::= [ "internal" ] "provisional" "clause" IDENT "{" NL { ClauseSig NL } "}"
ClauseSig  ::= "clause" IDENT "(" [ Params ] ")" "->" Type
```

A provisional clause is a clause template. A contract that incorporates one must implement every signature exactly. Two incorporated provisional clauses contributing the same clause name is a compile error at the `incorporate` site.

## Contracts

```text
ContractDecl ::= [ "internal" ] "contract" IDENT [ Attrs ] "{" NL { Member NL } "}"
Member       ::= Incorporate | VowDecl | SchemaDecl | PledgeDecl | RemedyDecl
               | SignalDecl | ClauseDecl | SubcontractDecl | RequirementsBlock
               | CourseDecl | AmendsDecl

Incorporate  ::= "incorporate" IDENT

VowDecl      ::= "vow" IDENT ":" Type [ "=" Expr ]

SchemaDecl   ::= "schema" IDENT "{" NL { ColumnDecl NL } "}"
ColumnDecl   ::= IDENT ":" Type

PledgeDecl   ::= "pledge" IDENT "(" [ Params ] ")" "->" Type Terms [ Attrs ] [ Block ]

ClauseDecl   ::= "clause" IDENT "(" [ Params ] ")" "->" Type Block

Params       ::= Param { "," Param }
Param        ::= IDENT ":" Type

SubcontractDecl ::= "subcontract" [ IDENT ] [ "transactional" ] Terms
                    "{" NL { SubMember NL } "}"
SubMember       ::= PledgeDecl | SignalDecl
```

Semantic notes.

- A pledge return type must be `Result<T, E>`, `Option<T>`, or `Unit`. Nothing else.
- A pledge with a `Block` is implemented in Geas. A pledge without one is abstract: it must be bound to a foreign symbol through `[abi: ..., symbol: ...]` or bound by the host through the runtime before the contract signs. Signing with an unbound pledge is an error.
- A vow initializer must be a constant expression of the vow type. A vow without an initializer must be supplied at sign time.
- Clauses are not first class. A clause is callable by bare name only inside its own contract.
- Subcontracts do not nest. An anonymous subcontract cannot be named in a `requirements` block.
- A `schema` names a table and its columns for a store-backed contract. A column type is one of the seven scalars a flat row holds, `Int`, `UInt`, `Float`, `Bool`, `Byte`, `Char`, or `String`; a composite column type is a type error. The first column is the schema's primary key. A contract may declare more than one schema, one per table it touches, and the schema is vow shaped: it joins the shape hash and locks at sign. A contract that declares at least one schema is store-backed and must carry a `dsn` vow, or the sign fails unbound.
- The `transactional` modifier runs a subcontract's pledges as one all-or-nothing episode and is allowed only on a subcontract in a store-backed contract. Its pledges commit together when the subcontract completes and roll back on the first `Err` or at a break before the commit; a subcontract without the modifier groups for the policy alone, its pledges autocommit. A transactional subcontract holds no signal and no pledge with a `retry`: a signal arrives from outside and never belongs inside an open transaction, and a retry would run a failed attempt's writes and the next attempt's inside one episode, so both are compile errors rather than given a meaning.
- Names are unique across a contract's pledges, signals, remedies, vows, schemas, clauses, and named subcontracts.

## Terms

```text
Terms    ::= { Term }
Term     ::= Deadline | Ordering | Retry

Deadline ::= "within" DURATION
Ordering ::= "after" IDENT { "," IDENT }
Retry    ::= "retry" INT [ "every" DURATION ] [ "backoff" FLOAT ]
                         [ "cap" DURATION ] [ "on" IDENT { "," IDENT } ]
```

Terms trail a signature, and since a term keyword continues the line above, each one may sit on its own line under it:

```text
pledge authorize(card: Card) -> Result<AuthId, PayError>
    within 30s
    retry 3 every 2s backoff 2.0 cap 30s on Timeout, Gateway
```

Which declaration takes which terms, each at most once:

| Declaration | `within` | `after` | `retry` |
| ----------- | -------- | ------- | ------- |
| pledge      | yes      | yes     | yes     |
| subcontract | yes      | yes     | no      |
| signal      | yes      | yes     | no      |
| remedy      | yes      | no      | yes     |

Semantic notes.

- **Eligibility.** Every pledge, signal, and subcontract becomes eligible at some moment. With no `after` that moment is the sign. With an `after` it is the moment every item it names has latched fulfilled, a named subcontract counting when every member inside it has. An `Err` or a lapse never satisfies an `after`, so an item waiting on something that broke never becomes eligible. A subcontract's own `after` and `within` cover its members: a member is eligible once its subcontract is and its own `after` has landed too.
- **`after`** names pledges, subcontracts, or signals of the same contract, never a vow, a clause, or a remedy. An item cannot be ordered after itself, a member after its own subcontract, or a subcontract after one of its own members, and the checker rejects a cycle through the `after` graph and names the path. Fulfilling or delivering an item before it is eligible is a state error and runs nothing.
- **`within`** is a deadline counted from eligibility. An item still pending when it passes lapses: it latches broken with no `Err` payload, because the pledge's own error type has no say over a deadline, and the runtime marks the latch as a lapse. A lapse is the cause of a latch, never the status of a call. A body started before the deadline lands its outcome even if it returns after it. A subcontract's `within` lapses every member inside it that has not latched.
- **`retry`** is the number of attempts after the first, at least 1. The delay before retry number n, counting from 0, is `every` times `backoff` to the power n, held at `cap` when one is given; `backoff` is a `FLOAT` literal of at least `1.0`, written with its decimal point, `2.0` and never `2`. Without `on`, only a lifecycle failure retries, a lost worker or a lapsed attempt, never a business `Err`. With `on`, the pledge's return must be a `Result` whose error type is a declared sum, and the listed names must be distinct variants of it; an `Err` carrying one of them retries too. A remedy's `retry` follows the same rules over the remedy's own return.
- The options of a `retry` stay on its line and come in the order the production gives, `every`, `backoff`, `cap`, `on`, each optional.

## Remedies

```text
RemedyDecl ::= "remedy" IDENT "for" IDENT "(" [ Param ] ")" "->" Type
               Terms [ Attrs ] [ Block ]
```

A remedy undoes a pledge that landed, the compensation in a saga. When the contract breaks, every pledge that latched fulfilled and carries a remedy gets it run, newest latch first.

```text
remedy refund for capture(receipt: Receipt) -> Result<Unit, PayError>
    retry 2 every 1m
```

Semantic notes.

- The target after `for` names a pledge of the same contract, never a signal or another remedy, and a pledge has at most one remedy.
- The parameter, when present, receives the target's `Ok` value, so its type must equal `T` of the target's `Result<T, E>`. A target returning `Unit` or `Option<T>` yields no `Ok` value and its remedy takes no parameter. The parameter may also be left off for a target that does yield one, when the remedy has no use for the value.
- A remedy returns `Result<Unit, E>` or `Unit`. It succeeds when it answers anything but an `Err`.
- A remedy is bound the ways a pledge is: a Geas body, a foreign symbol through `[abi: ..., symbol: ...]`, or a host binding under `"Contract.remedy"`. An abstract remedy nothing binds refuses the sign.
- A remedy is never a requirements atom, never an item of the partial surface, and never called through an instance.
- While remedies run the contract reads `Remedying`, and `Broken` once the last one returns. A remedy that fails, by an `Err` its `retry` does not cover, by running out of retries, or by its `within`, is recorded on the partial surface and does not stop the others. A remedy's `within` counts from the moment it starts.
- A pledge inside a transactional subcontract whose episode never committed has no remedy due, since the rollback already undid it.

## Signals

```text
SignalDecl ::= "signal" IDENT "(" [ Params ] ")" Terms
```

A signal is an outside event delivered into a live instance, a webhook, an approval, a delivery scan. It has no body and no return type.

```text
signal delivered(tracking: Tracking) after ship within 3d
```

Semantic notes.

- A signal latches fulfilled when it is delivered, and the first delivery wins: a repeat is accepted and changes nothing, the payload stays the first one's. Its payload is `Unit` with no parameters, the value itself with one, and a `Tuple` of the values with several.
- A loose signal, one declared outside any subcontract, is a requirements atom like a loose pledge. A signal inside a subcontract counts toward its subcontract, and the subcontract is the name the requirements use; naming the inner signal directly is an error.
- From Geas, a signal is delivered through a signed instance, `courier.scanned(at)`, a call answering `Unit`. A refused delivery, an early one, a lapsed one, or one into a contract that is not live, faults the calling body with the runtime's status. A signal is not a value, cannot be called by bare name, and is never fulfilled like a pledge. From a host it is `geas_signal`.
- A signal parameter follows the boundary rules every pledge parameter follows.

## Requirements

```text
RequirementsBlock ::= "requirements" "{" NL
                        [ "fulfill" ":" ReqExpr NL ]
                        [ "partial" ":" ReqExpr NL ]
                        [ "break"   ":" ReqExpr NL ]
                      "}"

ReqExpr  ::= ReqOr
ReqOr    ::= ReqAnd { "||" ReqAnd }
ReqAnd   ::= ReqNot { "&&" ReqNot }
ReqNot   ::= [ "!" ] ReqAtom
ReqAtom  ::= IDENT | "(" ReqExpr ")"
```

A `ReqAtom` names a named subcontract, a pledge declared outside any subcontract, or a signal declared outside any subcontract. A remedy is never an atom. A bare name means that item is fulfilled, `!` negates it. At most 16 atoms per contract.

A pledge latches. It becomes fulfilled on its first `Ok` and broken on an `Err` that lands before any `Ok`, or on a lapse. Later calls still run and still return their results to the caller, but the latched state never changes, so contract state never regresses. A subcontract is fulfilled when every member inside it is, and broken when every member inside it is.

The policy is evaluated after every latch, in priority order `break`, then `fulfill`, then `partial`, and the first line that matches sets the contract state. The `break` line arms only once at least one item has latched broken; before that it never matches, whatever its expression says. Without this rule a policy like `break: !Validation && !Processing` would tear the contract down after its very first successful fulfillment, when nothing has broken and nothing is yet fulfilled. A state that satisfies both `fulfill` and `partial` is therefore fine, `fulfill` wins. The one static rejection is a `fulfill` and a `break` that can be true at the same time, checked by enumerating the atom assignments, and the diagnostic names a witness.

When the block is absent the defaults apply: `fulfill` when every subcontract, loose pledge, and loose signal is fulfilled, `partial` when at least one subcontract is, `break` when everything is broken.

## Course

The course parses and checks in full today; its emission, the engine driven mode, is the next step on the roadmap, and `geas build` refuses a contract with a course until then.

```text
CourseDecl ::= "course" Block
```

The course is the contract's orchestration, the order in which the engine drives its own pledges. A contract has at most one. With a course the contract is engine driven: the engine runs the course from sign, each pledge call inside it is a durable step dispatched to its binding, every result is journaled, and a parked instance resumes at the step it stopped on. Without one the contract is host driven, the host calling the pledges in any order the terms allow.

```text
course {
    let auth = authorize(order.card)?
    together {
        let receipt = capture(auth)?
        let res = reserve(order.sku, 1h)?
    }
    ship(res)?
    await delivered
}
```

Semantic notes.

- Inside a course the contract's own pledges, subcontract members included, are called by bare name, the way clauses are; outside a course a bare name call to a pledge is an error, a pledge being fulfilled through a signed instance.
- `await`, `select`, `sleep`, `together`, and `now()` appear inside a course and nowhere else.
- A course returns no value; `return` in a course is bare.
- A `?` that fires ends the course. The pledge it unwrapped has already latched, so the requirements decide where the contract stands, not the course.
- A contract with a course is driven by it, so another body cannot fulfill its pledges through an instance; delivering its signals through an instance stays legal, since that is how the outside reaches an engine driven contract.
- `now()` takes no arguments and answers the current `Time`. Like every non deterministic value in a course, it is journaled on first run and replayed on resume.

## Amendment

Amends blocks parse and check today, against what the amending version alone can verify; loading the older version's shape and its emission come next on the roadmap, and `geas build` refuses a contract with an amends block until then.

```text
AmendsDecl ::= "amends" INT "{" NL { AmendItem NL } "}"
AmendItem  ::= "vow" IDENT "=" Expr
             | "rename" IDENT "->" IDENT
             | "retire" IDENT
             | "latch" IDENT "as" LatchState
LatchState ::= "fulfilled" | "pending" | "broken"
```

`amends N` inside a contract at version M says how an instance parked under version N resumes as version M. The design is that the checker diffs the shape of N against M and every difference must be covered by an item, so a resume the build cannot account for is a compile error rather than a surprise in production. Blocks chain: a contract at version 3 may carry `amends 1` and `amends 2`.

```text
amends 1 {
    vow dsn = prior.store_url
    rename dispatch -> ship
    retire legacy
    latch delivered as pending
}
```

Semantic notes, as the checker holds them today.

- `N` is at least 1 and below the contract's own version, and each version is amended at most once.
- `vow NAME = Expr` supplies a vow of this version, and the expression is held to that vow's type. Inside it `prior` reads the parked instance's vows, `prior.store_url`; until the older shape is loaded, a `prior` member is taken at any type.
- `rename OLD -> NEW` maps an old member name onto a pledge, subcontract, vow, or signal of this version, never a clause or a remedy.
- `retire NAME` drops an old member and discards its latched state; it must name something this version no longer has.
- `latch NAME as STATE` sets the starting state of a pledge or signal of this version, `fulfilled`, `pending`, or `broken`.

## Statements

```text
Block        ::= "{" NL { Stmt NL } "}"
Stmt         ::= LetStmt | AssignStmt | ReturnStmt | IfStmt | MatchExpr
               | WhileStmt | ForStmt | SleepStmt | TogetherStmt
               | "break" | "continue" | Expr

LetStmt      ::= "let" [ "mut" ] IDENT [ ":" Type ] "=" Expr
AssignStmt   ::= Lvalue "=" Expr
Lvalue       ::= IDENT { "." IDENT | "[" Expr "]" }
ReturnStmt   ::= "return" [ Expr ]
IfStmt       ::= "if" Expr Block { "else" "if" Expr Block } [ "else" Block ]
WhileStmt    ::= "while" Expr Block
ForStmt      ::= "for" IDENT "in" Expr Block
SleepStmt    ::= "sleep" Expr
TogetherStmt ::= "together" Block
```

The parser reads an assignment as an expression statement followed by `=` and the checker holds its left side to a place, which keeps the start of a statement decidable on one token.

Bindings are immutable unless declared `let mut`. Assignment targets a `let mut` binding, a field of one, or an element of one. There are no compound assignment operators. `break` and `continue` are loop control only, so they appear inside a `while` or `for` body.

`for` walks a `List` or a `Map`. Over a `List<T>` the loop variable binds each element in order, type `T`. Over a `Map<K, V>` it binds each key in insertion order, type `K`, and the body reads a value back with `m[key]` where it needs one. The entry count is fixed when the loop begins, so a key the body inserts is not visited on the current pass, while updating the value of a key already present is seen through a later read, since an update moves the value and not the key's place.

`sleep` and `together` are course statements. `sleep d` takes a `Duration` and pauses the course durably, so a parked instance wakes on time. `together { ... }` starts every statement inside it concurrently and ends when all of them have; bindings made inside are visible after the block, and a `?` that fires inside cancels the siblings that have not latched.

A block used as a value takes the type of its final expression statement, and `Unit` when it ends with anything else. Match arms must agree on one type wherever the match sits, so a statement position match with mixed arms ends each arm in a statement or returns.

`+` on two `String` values concatenates into a new `String`. That is the one operator with a non numeric arm; a `String` never meets a numeric operand.

`&&` and `||` short circuit: the right operand is not evaluated when the left already decides the answer, so a bounds guard like `i < n && xs[i] == 0` never touches `xs[i]` out of range.

Every value has value semantics. A `let`, an assignment, an argument, and a return each carry an independent copy, composites included; mutating a record or list through one binding is never visible through another. The copy is cheap by assumption, the same assumption the boundary makes.

The one exception is a signed instance, which is a reference handle: `let b = a` binds both names to the same instance, equality on instances is identity, and there is no way to copy a contract. A pledge called on another contract inside a body runs a real fulfillment on that instance, and its result is copied home before it is used, so the value survives the callee breaking.

Vows are never assignable. `mut(expr)` is the builtin that produces a mutable copy of a vow value; the original never changes.

## Expressions

Precedence, tightest first.

| Level | Operators                            | Associativity |
| ----- | ------------------------------------ | ------------- |
| 1     | call `()`, index `[]`, member `.`, propagate `?` | left |
| 2     | unary `!`, unary `-`                 | right         |
| 3     | `*` `/` `%`                          | left          |
| 4     | `+` `-`                              | left          |
| 5     | `<` `<=` `>` `>=`                    | left          |
| 6     | `==` `!=`                            | left          |
| 7     | `&&`                                 | left          |
| 8     | `\|\|`                               | left          |

```text
Expr        ::= see the ladder above
Primary     ::= Literal | IDENT | "(" Expr ")" | TupleLit | ListLit
              | MatchExpr | CtorExpr | GenericCtor | AwaitExpr | SelectExpr
              | "mut" "(" Expr ")"

TupleLit    ::= "(" Expr "," Expr { "," Expr } ")"
ListLit     ::= "[" [ Expr { "," Expr } ] "]"

CtorExpr    ::= IDENT "{" [ IDENT ":" Expr { "," IDENT ":" Expr } ] "}"   // record
              | IDENT [ "(" Args ")" ]                                    // Some, Ok, Err, variant, call

GenericCtor ::= ( "List" | "Map" | "Option" | "Result" | "Tuple" )
                "<" Type { "," Type } ">" "(" [ Args ] ")"
```

`Some`, `None`, `Ok`, `Err`, a sum variant, and a call all parse as a name with an optional argument list, and the checker resolves which one a name is. `Ok`, `Err`, and `None` lean on the expected type for the side they do not carry, and when nothing supplies it the binding needs an annotation.

`<` after `List`, `Map`, `Option`, `Result`, or `Tuple` opens type arguments, and after any other name it is less than; the decision is on the name, one token of lookahead. The generic constructors spell their type and are held to it: `List<T>(a, b, c)` holds its values, `Map<K, V>()` is an empty map, `Option<T>()` is `None` and `Option<T>(v)` is `Some(v)`, `Result<T, E>(v)` takes exactly one `Ok` or `Err`, and `Tuple<A, B>(a, b)` takes one value per slot.

There is no map literal. A map starts empty from its constructor and grows through index assignment:

```text
let mut ages = Map<String, Int>()
ages["ada"] = 36
let hit = ages["ada"]      // Option<Int>, Some(36)
let miss = ages["bob"]     // None
```

Indexing a map reads an `Option<V>`, `None` on a missing key, so a miss is a value and never a fault; `m[k] = v` inserts or updates. Entries keep insertion order, which is the order any serialization sees and the order `for k in m` walks the keys. Removal from a map is not in the language yet.

`len(x)` is a builtin call, the length of a `List` or a `Map` as an `Int`: a list answers its element count and a map its entry count. It is reserved in callee position only, so a binding may still be named `len`; `len` applied to any other type, or to other than one argument, is an error. There is no method form.

An assignment target is a place, not a value, so the lvalue walk differs from reading in two ways. A map index in an assignment target names the value slot directly and types as `V`, not `Option<V>`: in final position `m[k] = v` inserts or updates and never faults, while a missing key in the middle of a chain, `m[k].field = v`, is a fault, the same status an out of range list index reports, because a place that does not exist cannot hang a `None` anywhere. And writing through a chain, `b.xs[0] = 99`, mutates the named binding in place; the copies value semantics make on reads never apply to the target being assigned.

A record literal is ambiguous where a block can follow, so two rules keep the parse deterministic. A bare record literal is not allowed in the head expression of `if`, `while`, `for`, or `match`; wrap it in parentheses to opt back in. And the `{` of a record literal must sit on the same line as the name, since a line ending before `{` reads as the name alone with the brace opening a block.

### Calls and contract operations

```text
CallExpr   ::= Expr "(" [ Args ] ")"
MemberExpr ::= Expr "." IDENT
Args       ::= Arg { "," Arg }
Arg        ::= [ IDENT ":" ] Expr
```

A named argument needs two tokens of lookahead, a name and then `:`.

Fulfilling a pledge is a method call on a signed instance, and it type checks against the pledge signature directly:

```text
let payment = PaymentService.sign(currency: "EUR")
let result = payment.validate_card(my_card)
```

- `Contract.sign(...)` takes named vow overrides. Every vow without a default must appear. It returns the signed instance.
- `Contract.resume(dsn, key)` takes two positional `String` arguments and stands a parked instance back up, the signed instance the answer the way sign's is. A key nobody parked, a contract the runtime does not register, and a version or shape skew each fault the enclosing body with the runtime's own status.
- `instance.pledge_name(args)` fulfills that pledge and returns exactly the pledge's declared type. A pledge that is not yet eligible under its `after`, or has lapsed, faults the body with the state status.
- `instance.signal_name(args)` delivers that signal and returns `Unit`; a refusal faults the body.
- `instance.pledge_name` with no call is a first class pledge value of the matching `PledgeType`, bound to that instance.
- `instance.vow_name` reads a locked vow through the instance, typed as the vow's declared type, an independent copy the way every read is. A vow is read, never called.
- `instance.break()` tears the contract down and returns `Unit`. A contract still Signed or Partial runs its due remedies first. Every later fulfillment on it returns the broken contract error.
- `instance.status()` returns the state's canonical spelling as a `String`: `"Unsigned"`, `"Signed"`, `"Fulfilled"`, `"Partial"`, `"Remedying"`, or `"Broken"`.
- `instance.park(dsn, key)` writes the instance's durable state into the store behind `dsn` under `key` and returns `Unit`; parking is a write, not an ending, and the instance stays live. A park mid walk, an open transactional episode, an unwaited future, or a retry waiting out its delay, faults the body, the same refusals the C surface answers.
- `instance.partial()` returns the `PartialResult`, a builtin record every program carries without declaring it: `state: String`, `fulfilled: List<String>`, `pending: List<String>`, `broken: List<String>`, the name lists in the runtime's insertion order. It is an ordinary record value, its fields read like any record's, and the copy is independent the way every read is. The `Err` payloads, the lapse flags, and the remedy outcomes stay on the C surface, because their types vary per item and a record field carries one. Declaring your own `PartialResult` collides with the builtin and is an error.
- A clause is called by bare name, `format_message(msg)`, and only from inside its own contract. Inside a course, so is a pledge of the same contract.

The member spellings `sign`, `resume`, `break`, `status`, `park`, and `partial` belong to the lifecycle; a pledge under one of those names is unreachable through an instance and its own spelling should step aside.

There is no dynamic `fulfill(name, args)` form in the language. Dispatch by name lives in the runtime C API for foreign hosts, where nothing is statically checked anyway.

### Propagation

`expr?` unwraps an `Option` or a `Result`. On `Some` or `Ok` it yields the inner value. On `None` or `Err` it returns from the enclosing pledge, clause, or remedy immediately with that `None` or `Err`, and inside a course it ends the course. The enclosing return type must match, `Option` propagates only inside an `Option` returning body, `Err` only where the error types agree; a course, which returns nothing, takes either.

### Match

```text
MatchExpr ::= "match" Expr "{" NL { MatchArm NL } "}"
MatchArm  ::= Pattern "->" ( Expr | Block )
Pattern   ::= Literal
            | "-" ( INT | FLOAT | DURATION )
            | "_"
            | IDENT                                   // binding
            | IDENT "(" Pattern { "," Pattern } ")"   // variant with payload
            | "Some" "(" Pattern ")" | "None"
            | "Ok" "(" Pattern ")" | "Err" "(" Pattern ")"
```

`match` is an expression. Arms are checked for exhaustiveness over sums, `Option`, `Result`, and `Bool`. A `_` or binding arm covers the rest. Non exhaustive matching over an open type like `Int` or `Duration` requires a final `_` arm.

### Await and select

These are course expressions, parsed and checked today and emitted with the course.

```text
AwaitExpr  ::= "await" IDENT [ "within" DURATION ]
SelectExpr ::= "select" "{" NL { SelectArm NL } "}"
SelectArm  ::= "await" IDENT [ "as" Pattern ] "->" ( Expr | Block )
             | "timeout" DURATION "->" ( Expr | Block )
```

`await sig` waits for a signal of the same contract and yields its payload: `Unit` for no parameters, the value for one, a `Tuple` for several. `await sig within 24h` yields an `Option` of the payload, `None` when the time runs out, and the signal does not latch broken for it. `select` waits on several things and takes the first arm that is ready: an `await` arm binds its payload with `as`, a `timeout` arm fires after its duration, and the arms agree on one type the way match arms do.

## The entry point

A standalone program declares exactly one `Main`:

```text
contract Main {
    pledge run(args: List<String>) -> Result<Int, E>
}
```

`run` takes exactly one `List<String>` parameter, returns `Result<Int, E>` for any error type `E` the program declares, and must carry a body. The emitted executable signs `Main`, fulfills `run`, maps `Ok(n)` to exit code n truncated to its low eight bits, and maps `Err` to a rendered diagnostic on stderr and exit code 1.

## Lifecycle states

```text
Unsigned -> Signed -> Fulfilled
                   -> Partial
                   -> Remedying -> Broken
                   -> Broken                (no remedies due)
```

A contract signs into Signed. The policy moves it to Partial or Fulfilled as items latch. A matched break line, a lapse that matches it, the contract `term` running out, or an explicit break on a contract still Signed or Partial moves it to Remedying while its due remedies run, and to Broken when they are done; with no remedy due it goes straight to Broken. A Fulfilled contract never runs a remedy.

## What the grammar deliberately leaves out

Kept out so the surface stays small. Each one has a reserved spelling or a stated path, so adding it later breaks nothing.

- Generic user declarations. Type parameters stay on the builtin composites.
- Map literals, map removal, compound assignment, ranges, and a `for` over numeric ranges.
- A `Time` literal. A point in time comes from the host or from `now()`.
- An async fulfillment surface in the language. The runtime API is shaped for it, the language keeps the synchronous call form.
- Provisional clauses requiring pledges. Today they require clauses only, which makes them internal code sharing, not a public interface.
