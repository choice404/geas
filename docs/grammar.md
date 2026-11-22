# The Geas Grammar

This is the normative grammar for Geas. It covers the lexical structure, the type grammar, every declaration form including the workflow surface, terms, remedies, signals, the course, and amendment, and the statement and expression core that clause, pledge, remedy, and course bodies are written in. Where the grammar alone cannot pin a rule down, a semantic note next to the production carries the rest. The parser and the checker are held to this document, and a change to the surface lands here first.

The workflow surface runs in both of its modes today. Durations, terms, remedies, signals, and the contract term are parsed, checked, emitted, and enforced by the runtime for a host driven contract, and the course, with `await`, `select`, `sleep`, `together`, and `now()`, is lowered to a resumable state machine the runtime drives for an engine driven one. The amends block is checked against the recorded shape of the version it amends and emitted into the module, so a parked instance resumes as a later version of its contract.

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
- An attribute list `[...]`, and the body `{` of a pledge, a clause, or a remedy, open on the line their declaration ends on, which for a pledge with terms is the line of its last term. A pledge whose `{` sits on the next line has no body as far as the parser is concerned, so the brace is refused as a stray member, and a clause, which always has a body, says it is missing one. Every other `{`, the one that opens a record, a schema, a contract, a provisional clause, a subcontract, a `requirements` block, a `course`, an `amends` block, or the block of an `if`, `else`, `while`, `for`, or `match`, may sit on the line after its head as well as on the same one.

Nesting depth is capped at 500. The parser counts every block, bracket, and parenthesis it opens, every operator a chain of binary operators links, since each one nests the expression tree a level deeper, and every call, index, or member suffix after a primary, so a long `a + b + c + ...` chain counts against the cap the way deep parentheses do. A parser that hits the ceiling reports a diagnostic and stops rather than hanging or overflowing its stack, and no later walk of the tree ever recurses past it.

## Comments

```text
// line comment, to end of line
/* block comment, no nesting */
```

## Tokens

The lexer produces exactly these kinds, each carrying its source span and the line break mark described above.

| Class | Kinds |
| ----- | ----- |
| Names and literals | `IDENT`, `INT`, `SIZE`, `FLOAT`, `DURATION`, `STRING`, `CHAR` |
| Grouping | `(` `)` `[` `]` `{` `}` |
| Punctuation | `,` `.` `:` `?` `->` `_` `=` |
| Arithmetic | `+` `-` `*` `/` `%` |
| Logic and comparison | `!` `&&` `\|\|` `==` `!=` `<` `<=` `>` `>=` |
| Keywords | one kind per hard keyword, below |
| End | `EOF` |

The lexer takes the longest token that fits: `->` over `-`, `==` over `=`, `!=` over `!`, `<=` over `<`, `>=` over `>`, and `ms` over `m` inside a duration. A lone `&` or `|` is a lex error, there are no bitwise operators. There is no shift operator, so `>>` is never a token and `List<List<Int>>` closes on two `>` tokens. `_` alone is its own token; `_x` is an `IDENT`.

Lex errors are reported with their span and then skipped past, so one bad character does not end the file: an unterminated string, char, or block comment, a raw line break inside a string, an invalid escape, a char literal holding zero or several scalar values, invalid UTF-8, an integer or size above the `Int` maximum, a size suffix followed by identifier characters, a duration with units out of order, repeated, or unknown, and a stray character.

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
- `rename`, `retire`, `latch`, `restart`, `as`, and `prior` belong to an `amends` block, `fulfilled`, `pending`, and `broken` follow `latch NAME as`, and `course` follows `restart`.
- `version`, `term`, `abi`, and `symbol` are builtin attribute keys.
- `attribute` opens an attribute declaration when followed by an identifier at the top level, and `on` separates its type from its target. Both remain ordinary identifiers elsewhere.
- `sign`, `resume`, `status`, `park`, and `partial` are builtin contract methods.
- `List`, `Map`, `Option`, `Result`, and `Tuple` open type arguments, see Expressions.
- `len`, `bytes`, `text`, and `byte_list` are builtins in callee position, and `now` is a builtin in callee position inside a course.

One carve out: after `.` any keyword is accepted as a member name. That is what lets `payment.break()` parse while `break` stays a hard keyword for loop control.

## Literals

```text
INT      ::= decimal | "0x" hex | "0b" binary        // underscores allowed as separators
SIZE     ::= decimal ( "K" | "M" | "G" | "T" )
FLOAT    ::= digits "." digits [ ("e"|"E") ["+"|"-"] digits ]
DURATION ::= DurPart { DurPart }
DurPart  ::= digits ( "d" | "h" | "m" | "s" | "ms" )
STRING   ::= '"' { char | escape } '"'
CHAR     ::= "'" ( char | escape ) "'"
BOOL     ::= "true" | "false"
escape   ::= "\n" | "\t" | "\r" | "\0" | "\\" | "\"" | "\'" | "\u{" hex "}"
```

An `INT` or `SIZE` literal types as `Int` unless the expected type at its position is `UInt` or `Byte` and the value fits. Its magnitude must fit 64 bits signed, a lex error otherwise, save the magnitude of the least `Int`, `9223372036854775808`, which is legal only as the operand of a unary minus right before it, `-9223372036854775808`, and spells the least `Int` there; the least `Duration`, `-9223372036854775808ms`, and the least size, `-8388608T`, follow the same rule. A `FLOAT` literal types as `Float` and stands for the double nearest the decimal it spells, underscores ignored, so `1_000.5` and `1000.5` are one value; a literal whose value is past the largest finite double, `1.0e309`, is a lex error. `CHAR` holds one Unicode scalar value. There is no implicit numeric conversion anywhere, widening included, and there is no conversion syntax either: no cast, no `as`, and no call named after a type, so a value of one numeric type never becomes another inside the language. A host that needs a value in another width hands it in at that width.

A `SIZE` is one token, `64M`, `512K`, `1G`, with no whitespace before its uppercase suffix. The suffix counts bytes in binary multiples: `K` is 1024, `M` is 1024 squared, `G` is 1024 cubed, and `T` is 1024 to the fourth power. A size is an integer spelling, legal wherever an integer literal is legal, including attributes, vow defaults, patterns, retry counts, and amendment versions. Unary minus applies the same way, so `-512K` is an `Int` value of -524288. The decoded value must fit `Int` before negation; `64Mb`, `1Gx`, and a suffix followed by any other identifier character are lex errors. The lex and parse dumps retain `Size(bytes)`, while emitted C carries the plain integer. Where a production below calls for `INT`, `SIZE` is accepted too.

A `DURATION` is one token, `30s`, `250ms`, `1h30m`, `7d`, with no whitespace inside it, and types as `Duration`. Its units run from largest to smallest, `d h m s ms`, each at most once, so `30m1h` and `1h1h` are lex errors. After the digits of a decimal literal the lexer looks at what follows: a `.` and a digit make a `FLOAT`, `K`, `M`, `G`, or `T` opens a `SIZE`, and a duration unit opens a `DURATION`. Other letter runs are lex errors, so `5mx`, `3days`, and lowercase `5k` are refused rather than split into a number and a name. Case matters: `5m` is five minutes and `5M` is five mebibytes. A literal with a `0x` or `0b` prefix takes neither suffix, and a float takes neither suffix, so `0x1d` is an `INT` while `0x1K`, `0b1M`, and `1.5G` are lex errors.

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
| Bytes    | arbitrary bytes, pointer plus byte length, no terminator |
| Duration | 64 bit signed milliseconds  |
| Time     | 64 bit signed milliseconds since the Unix epoch |

## Types

```text
Type       ::= NamedType | TupleType
NamedType  ::= IDENT [ "<" Type { "," Type } ">" ]
TupleType  ::= "Tuple" "<" Type { "," Type } ">"
```

The builtin named types are `Int`, `UInt`, `Float`, `Bool`, `Byte`, `Char`, `String`, `Bytes`, `Unit`, `Duration`, `Time`, `List<T>`, `Map<K, V>`, `Option<T>`, `Result<T, E>`, and `Tuple<...>`. Type arguments belong to the builtin composites only. A user declaration does not take type parameters. Every instantiation is monomorphized, and only instantiations reachable from a pledge signature get an ABI descriptor.

`Bytes` holds arbitrary bytes, including zeros and invalid UTF-8, in one buffer. A user declaration named `Bytes` wins over the builtin, the rule `Duration` and `Time` follow. It crosses pledge and signal parameters and pledge returns, and fits inside `Result`, `Option`, records, sum payloads, lists, map values, tuples, and local bindings. A `Bytes` vow has no default and must be supplied at sign. `Bytes` is neither a map key nor a schema column.

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

Every other type name has to be declared: a record, a sum, a schema's row, or a contract. A name nothing declares is a compile error wherever an annotation spells it, `unknown type 'Strng'`, so a typo never turns into a type that takes any value. The one name the compiler brings in with no declaration behind it is `StoreError`, the `Err` side of every Store answer.

A contract name used as a type denotes a signed instance, and an instance never crosses the boundary. A contract type is therefore rejected anywhere a value would cross it: a pledge, remedy, or signal parameter or return, a vow, a record or sum field, and a provisional clause signature, at any nesting depth. Clause parameters, clause returns, and local bindings may carry contract types freely, since they never leave the module.

## Program structure

```text
File        ::= { Import NL } { TopDecl NL }
Import      ::= "import" IDENT { "." IDENT }
TopDecl     ::= RecordDecl | SumDecl | ProvClause | ContractDecl | AttributeDecl
```

A file is a module. A directory is a package. `import std.math` brings a module into scope by its dotted path. `internal` on a contract or provisional clause keeps it inside its package and out of the descriptor table.

## Attributes

```text
AttributeDecl ::= [ "internal" ] "attribute" IDENT ":" AttrType "on" AttrTarget
AttrType      ::= "Int" | "Float" | "Bool" | "String" | "Duration"
AttrTarget    ::= "contract" | "subcontract" | "pledge" | "signal" | "remedy"
Attrs         ::= "[" IDENT ":" AttrValue { "," IDENT ":" AttrValue } [ "," ] "]"
AttrValue     ::= [ "-" ] ( INT | SIZE | FLOAT | DURATION ) | BOOL | STRING
```

A trailing comma before the closing `]` is accepted, so `[version: 2,]` and `[version: 2]` are the same list.

An attribute declaration occupies one line and follows the module rules: declarations in loaded modules are visible across their package and through `import`, and `internal` keeps one inside its package. Attribute names have one namespace per target. A name may be declared on several targets, but a second declaration for the same target is an error, and a declaration cannot shadow a builtin key on a target that owns that key.

An attribute list trails a declaration header, after every term and before the opening body brace when there is one. Its `[` opens on the line the signature or last term ends on, and the body brace opens on the line the list ends on. A subcontract puts its list after its name, optional `transactional`, and terms, immediately before `{`. A signal puts its list after its parameters and terms, with no body. The same positions apply to a pledge or signal inside a subcontract.

A list accepts the builtin keys of its target and declared attributes in scope for that target. Unknown keys, a key declared for another target, a repeated key in one list, and a value of the wrong type are compile errors. Values use the usual literal rules, so a size is an `Int`, a duration is a `Duration`, and there is no numeric conversion. Unary minus applies to numbers and durations. Declared attributes are host settings, outside both the shape hash and mangled names, so changing a setting leaves a parked instance compatible.

```text
attribute program: String on pledge
attribute region: Int on pledge
attribute queue: String on contract

CopyError is either Failed(code: Int)

contract Job [queue: "batch"] {
    pledge copy(target: String) -> Result<Unit, CopyError> [program: "/bin/copy", region: 64M]
}
```

The builtin keys:

- `version: INT` on a contract, from 1 through 4294967295. Defaults to 1. Baked into every mangled name the contract produces.
- `term: DURATION` on a contract, its whole lifetime counted from sign. The duration must be greater than zero, and this builtin key belongs on a contract only. A pledge or remedy needs its own declared attribute to use the name `term`. When the term runs out while the contract is Signed or Partial, the contract breaks the way a matched break line breaks it, remedies included; a contract already Fulfilled met its terms and is left alone.
- `abi: STRING` and `symbol: STRING` on a pledge or a remedy with no body. Binds it to a function the host process exports under that symbol. `"c"` is the only ABI, the symbol is a C identifier that does not start with `geas_`, `__geas_`, or `GEAS_`, the prefixes the runtime and the emitted code own, and the two keys always appear together. A pledge or a remedy with a body takes neither, and a signal, a subcontract, or a contract takes neither.

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

SubcontractDecl ::= "subcontract" [ IDENT ] [ "transactional" ] Terms [ Attrs ]
                    "{" NL { SubMember NL } "}"
SubMember       ::= PledgeDecl | SignalDecl
```

Semantic notes.

- A pledge return type must be `Result<T, E>`, `Option<T>`, `Unit`, or `Bytes`. A bare `Bytes` return latches fulfilled on delivery.
- A pledge with a `Block` is implemented in Geas. A pledge without one is abstract: it must be bound to a foreign symbol through `[abi: ..., symbol: ...]` or bound by the host through the runtime before the contract signs. Signing with an unbound pledge is an error.
- A vow initializer is a literal of the vow type, a unary minus in front of a number or a duration included, `vow retries: Int = -1` or `vow grace: Duration = 30m`; anything else, `1 + 2` or another vow's name, is `vow 'NAME' initializer must be a constant literal`. A vow without an initializer must be supplied at sign time.
- A vow's type is one the contract descriptor carries as a single value: `Int`, `UInt`, `Float`, `Bool`, `Byte`, `Char`, `String`, `Bytes`, `Duration`, or `Time`. A vow of a record, a sum, a `List`, a `Map`, an `Option`, a `Result`, or `Unit` checks, and `geas build` refuses it with `codegen: not yet supported: a vow of this type in a compiled module`; carrying composite vows in the descriptor is a later step on the roadmap.
- Clauses are not first class. A clause is callable by bare name only inside its own contract.
- Subcontracts do not nest. An anonymous subcontract cannot be named in a `requirements` block.
- A subcontract holds at least one pledge or signal. It is fulfilled when every member inside it is, so an empty one would never be, and the checker refuses it.
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
- **`within`** is a deadline counted from eligibility, a duration greater than zero; `within 0s` would read as no deadline at all, so it is a compile error. An item still pending when it passes lapses: it latches broken with no `Err` payload, because the pledge's own error type has no say over a deadline, and the runtime marks the latch as a lapse. A lapse is the cause of a latch, never the status of a call. A body started before the deadline lands its outcome even if it returns after it. A subcontract's `within` lapses every member inside it that has not latched.
- **`retry`** is the number of attempts after the first, at least 1 and at most 1,000,000. The delay before retry number n, counting from 0, is `every` times `backoff` to the power n, held at `cap` when one is given; `every` and `cap` are durations greater than zero, since a zero one would read as no delay or no cap, and `backoff` is a `FLOAT` literal of at least `1.0`, written with its decimal point, `2.0` and never `2`. Without `on`, only a lifecycle failure retries, a lost worker or a lapsed attempt, never a business `Err`. With `on`, the pledge's return must be a `Result` whose error type is a declared sum, and the listed names must be distinct variants of it; an `Err` carrying one of them retries too. A remedy's `retry` follows the same rules over the remedy's own return.
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
SignalDecl ::= "signal" IDENT "(" [ Params ] ")" Terms [ Attrs ]
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
RequirementsBlock ::= "requirements" "{" NL { ReqLine NL } "}"
ReqLine  ::= ( "fulfill" | "partial" | "break" ) ":" ReqExpr

ReqExpr  ::= ReqOr
ReqOr    ::= ReqAnd { "||" ReqAnd }
ReqAnd   ::= ReqNot { "&&" ReqNot }
ReqNot   ::= [ "!" ] ReqAtom
ReqAtom  ::= IDENT | "(" ReqExpr ")"
```

The three lines come in any order, each at most once, and any of them may be left out; a second `fulfill` line is `duplicate 'fulfill' line in the requirements block`. A line a written block leaves out never matches, it does not fall back to its default, so a block with no `break` line is a contract no latch breaks, and only its term or an explicit break ends it. Writing them fulfill, partial, break is the convention every example follows. A `ReqAtom` names a named subcontract, a pledge declared outside any subcontract, or a signal declared outside any subcontract. A remedy is never an atom. A bare name means that item is fulfilled, `!` negates it. A written requirements block names at most 64 distinct atoms across its three lines.

A pledge latches. It becomes fulfilled on its first `Ok` and broken on an `Err` that lands before any `Ok`, or on a lapse. Later calls still run and still return their results to the caller, but the latched state never changes, so contract state never regresses. A subcontract is fulfilled when every member inside it is, and broken when every member inside it is.

The policy is evaluated after every latch, in priority order `break`, then `fulfill`, then `partial`, and the first line that matches sets the contract state. The `break` line arms only once at least one item has latched broken; before that it never matches, whatever its expression says. Without this rule a policy like `break: !Validation && !Processing` would tear the contract down after its very first successful fulfillment, when nothing has broken and nothing is yet fulfilled. A state that satisfies both `fulfill` and `partial` is therefore fine, `fulfill` wins. The checker rejects a fulfill or break line that can never hold, and a `fulfill` and a `break` that can be true at the same time, checked by satisfiability search with unit propagation and atom branching, and the diagnostic names a witness.

When the block is absent the defaults apply: `fulfill` when every subcontract, loose pledge, and loose signal is fulfilled, `partial` when at least one subcontract is, `break` when everything is broken. The 64 atom cap applies only to written blocks. Synthesized defaults work through 255 items, including loose pledges and signals, without that cap. When the fulfilled and broken atom rows would exceed the byte sized descriptor index, the runtime evaluates the same default directly over the subcontract and pledge tables; it keeps the same fulfill, partial, and break rules.

## Course

The course is emitted and run: `geas build` lowers it to a resumable C function, and the runtime drives that function on its pool from the moment the contract signs.

```text
CourseDecl ::= "course" Block
```

The course is the contract's orchestration, the order in which the engine drives its own pledges. A contract has at most one. With a course the contract is engine driven: the engine runs the course from sign, each pledge call inside it is a durable step dispatched to its binding, every step's result is kept in the course's frame, and a parked instance resumes at the step it stopped on with that frame. Without one the contract is host driven, the host calling the pledges in any order the terms allow.

```text
course {
    let window = plan(hold)?
    let auth = authorize(Card { number: card })?
    together {
        let receipt = capture(auth)?
        let held = reserve(sku, window)?
    }
    ship(held)?
    await delivered
}
```

That is a trimmed version of the course in `skeleton/order_course.geas`, which plans the hold, authorizes the card, captures and reserves at once, ships, and waits on the delivery scan.

Semantic notes.

- Inside a course the contract's own pledges, subcontract members included, are called by bare name, the way clauses are; outside a course a bare name call to a pledge is an error, a pledge being fulfilled through a signed instance.
- `await`, `select`, `sleep`, `together`, and `now()` appear inside a course and nowhere else.
- A course returns no value; `return` in a course is bare.
- A `?` that fires ends the course. The pledge it unwrapped has already latched, so the requirements decide where the contract stands, not the course.
- A contract with a course is driven by it, so another body cannot fulfill its pledges through an instance; delivering its signals through an instance stays legal, since that is how the outside reaches an engine driven contract.
- `now()` takes no arguments and answers the current `Time`, read once from the runtime clock where it runs. Its value lives in the course frame like any binding, so a resumed course sees the time it read before the park, never a new one.
- A `Main` contract cannot carry a course. The module builds, and `geas build --bin` refuses it, since the entry wrapper fulfills `run` itself while a course would be running the contract's pledges.

#### Suspending operations

A suspending operation is one the course can stop at and come back to: a call of the contract's own pledge by bare name, `await`, `select`, `sleep`, and `together`. Each one is a step. The engine saves the course at every step boundary, so a parked or crashed instance resumes at the step it stopped on with every binding it had made, and nothing before that step runs again. A step is at least once: a course that was inside a pledge call when its row was last written runs that call again after a resume.

- A suspending operation stands in statement position: an expression statement, a `let` initializer, the right side of an assignment, a `?` on any of those, or the head of an `if`, a `match`, or a `while`. One found deeper in an expression, a pledge call as an argument of another call for instance, is hoisted by the compiler into a hidden binding just before the statement, and the operands to its left are evaluated and kept first, so evaluation order is the order the source reads.
- The right operand of `&&` and `||` cannot hold one. Hoisting it would run it before the left operand decided whether it runs at all, so the checker refuses it and says to bind it with `let` first.
- A clause called from a course is ordinary code and holds no suspending operation; only the course itself suspends.
- A course binding never has a contract instance type, directly or inside a record, a list, an option, or any other composite, and neither does a hidden temporary the compiler makes. A frame must be able to park, and an instance handle does not survive a park, so signing another contract and holding its instance in a course is refused.
- Every binding and every hidden temporary of a course lives in a slot of the frame, and a slot starts at the smallest value of its type when the instance signs: zero, empty, `None`, a record of smallest fields, the variant of a sum whose fields nest least, the first on a tie, and the arm of a `Result` that nests least, `Ok` on a tie. So a slot's type has a finite value, and its smallest value nests at most 256 levels and holds at most 4096 values. A record that holds itself with no `Option` or `List` between has no finite value and one that holds the next record twice passes the count in a dozen levels, and `geas build` refuses a course that holds either, saying to hold the value behind an `Option` or a `List`, whose smallest value is one value however large the type inside.
- The values a course makes, the copies its bindings take, and every step's result live on the instance's heap until the instance is broken. A loop in a course that runs many times grows that heap with each pass, so a long running loop belongs in a pledge, or in a contract of its own, rather than in the course.

## Amendment

A contract keeps changing after it ships, and an instance parked under an older version still has to finish. An `amends N` block inside a contract at version M says how an instance parked under version N resumes as version M, and `geas build` emits it into the module, so the runtime that loads version M picks up a version N row, applies the block, and carries on with an ordinary version M instance.

```text
AmendsDecl ::= "amends" INT "{" NL { AmendItem NL } "}"
AmendItem  ::= "vow" IDENT "=" Expr
             | "rename" IDENT "->" IDENT
             | "retire" IDENT
             | "latch" IDENT "as" LatchState
             | "restart" "course"
LatchState ::= "fulfilled" | "pending" | "broken"
```

The block from `skeleton/order_course_v2.geas`, version 2 of the engine driven order saga, which renames ship, adds a notify step, drops chase, counts the order in cents, and changes the course:

```text
amends 1 {
    // Version 1 counted the order in whole units
    vow cents = prior.amount * 100
    rename ship -> dispatch
    retire chase
    latch notify as pending
    restart course
}
```

#### Recorded shapes

The checker compiles version M, so it needs version N's shape from somewhere other than the source being compiled, and that is the recorded shape. `geas record FILE` writes `shapes/<Contract>.v<version>.shape` beside the file for every public contract in it, creating `shapes/` when it is missing:

```text
$ geas record skeleton/order_course.geas
[geas] recorded skeleton/shapes/OrderCourse.v1.shape
```

The file is plain text and stable across runs: the shape string and its hash, the course hash, the names the park row is positional over (vows, pledges then signals, subcontracts, remedies) each with its canonical type, and the layout of every declared record and sum the boundary reaches. Recording the same shape again is a no op, and a file that already records a different shape of the same version is refused unless `--force` is given, so a shape that shipped cannot quietly drift. The recorded files belong in the repository next to the source, the way `skeleton/shapes` holds both versions of the order saga.

Compiling a contract with `amends N` loads `shapes/<Contract>.v<N>.shape` from beside the source that declares the contract. A missing file is a compile error that names the path and says to run `geas record` on the source of version N.

#### Coverage

The checker diffs the recorded shape of N against M, and every difference has to be covered by exactly one item, so a resume the build cannot account for is a compile error and never a surprise in production.

- **Vows.** A vow N and M both have, under the same name and the same type, carries its value. A vow M adds takes its default when it has one, otherwise it needs `vow NAME = Expr`. A vow whose type changed needs `vow NAME = Expr` too. A vow N has and M lacks is dropped with no item, or mapped onto a vow of M with `rename OLD -> NEW` when the type agrees; a `retire` of a vow is an error, since there is nothing to retire.
- **Pledges and signals.** An item N and M both have under the same name, of the same kind, carries its latch, its `Err` payload, its stored `Ok` value or signal payload, its eligibility, its lapse, and its retry progress, even when its parameters or its error type changed. An item of N that M lacks needs `rename OLD -> NEW` or `retire OLD`. An item whose stored type changed, the `T` of a pledge's `Result<T, E>` or a signal's payload, cannot carry what it stored: it is retired and declared under a new name, and the error says so. A pledge of N that is a signal in M, or the other way round, is the same story. An item M adds starts pending, or in the state `latch NAME as STATE` gives it, and `latch` names only an item M adds.
- **Subcontracts and remedies.** Renamed with `rename` or dropped with `retire` the same way. A remedy that already ran stays ran under its new name, and a remedy that undoes another pledge in M than it did in N is retired and declared under a new name. A remedy M adds for a pledge that already latched fulfilled is due like any other when the contract breaks.
- **The course.** When N and M both have a course and the course hashes differ, or only M has one, the block holds `restart course`: the resumed instance enters M's course at step 0 with a fresh frame, and since every step is at least once, a step whose pledge already latched runs its body again, its result goes to the course, and its latch, its stored `Ok` value, and its place in the remedy order stay the ones it had. When the hashes agree the frame carries over and the course goes on at the step it parked at. When only N had a course its frame is dropped with no item.
- **Nothing extra.** An item no difference calls for is an error: a `retire` or a `rename` of a name N never had, a `rename` onto a name M lacks or onto a name that is already carried, a `latch` on an item N already had, a second item for one name, a `vow NAME = Expr` beside a `rename OLD -> NAME` among them, and a `restart course` where the course did not change.
- **Layout is type.** A canonical type code names a declared record or sum and nothing more, so the comparison reads the layouts too: a vow, a pledge's stored `Ok` value, or a signal's payload whose declared type kept its name but changed its fields or its variants counts as a changed type, and the error names the type whose layout moved.
- **A fulfilled seed has no value.** `latch NAME as fulfilled` latches the item with no value behind it, so it is a compile error for an item that carries one: a pledge whose `Ok` value or `Option` payload is anything but `Unit`, a pledge returning bare `Bytes`, and a signal with a payload. Whatever reads that value later, a course calling the pledge or awaiting the signal, or a remedy taking the `Ok` value, would have nothing to be handed. Latch such an item `pending`; a pledge of `Result<Unit, E>` or `Unit` and a signal with no parameters may start fulfilled.
- Terms, requirements, the contract's own term, and attributes change freely and need no item. M's apply from the resume on, with the eligibility stamps and the signing time the row kept.

#### Reading the older version

Inside `vow NAME = Expr`, `prior.NAME` reads vow NAME of the parked instance, as version N signed it. `prior` alone is not a value, only `prior.NAME` is. A `prior` read is typed when N's type is a scalar, or when it equals the type of M's vow of the same name or of the vow being set and every declared record and sum it reaches kept its layout, the types M can still spell; any other read is an error that says which type M no longer declares.

A vow item also reads M's own vows by bare name, as the block has filled them so far: a vow carried from N or with a default reads at once, and a vow only the block sets reads in the items after the one that sets it. Reading such a vow earlier, or inside its own item, is a compile error.

Semantic notes.

- `N` is at least 1 and below the contract's own version, and each version is amended at most once.
- Blocks do not chain. Each one maps N straight to the version that carries it, so a contract at version 3 that wants version 1 rows to resume carries `amends 1` as well as `amends 2`, each checked against its own recorded shape.
- `rename OLD -> NEW` maps a member of N onto a pledge, signal, subcontract, vow, or remedy of M of the same kind, never a clause.
- An `internal` contract never parks under a version of its own, so it takes no amends block.
- `restart` is a contextual keyword, a keyword only at the head of an item.
- A resumed instance that went through a block reads amended from version N on the C surface and the runtime raises an `amended` event before `resumed`; after that it is an ordinary version M instance, and its next park writes a version M row.

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

Bindings are immutable unless declared `let mut`. Assignment targets a `let mut` binding, a field of one, or an element of one. There are no compound assignment operators.

The right side of an assignment, and any index its target reads, may not assign the binding the target is rooted in, so `values[0] = match flag { ... }` with an arm that assigns `values` is a compile error. Ordinary code finds the target's slot before it runs the right side and a course finds it after, since a course reads its frame again once a step resumes, and this rule is what keeps the two from ever landing a value in different places. Bind the value with `let` first. A binding of the same name that a `let`, a `for` variable, or a pattern shadows inside the right side is another binding and does not count.

A pledge, clause, or remedy whose return type is not `Unit` ends every path through its body with a `return`, since there is no value to fall off the end with; one that can reach its closing brace is `not every path of 'NAME' returns a value`. A path ends at a `return`, at an `if` with an `else` whose branches both end, at a `match` whose every arm ends, and at a `while true` no `break` leaves. A `for` may run zero times, so it never ends a path. A `Unit` body may fall off its end, and a course returns nothing and is never held to this. `break` and `continue` are loop control only, so they appear inside a `while` or `for` body.

`for` walks a `List` or a `Map`. Over a `List<T>` the loop variable binds each element in order, type `T`. Over a `Map<K, V>` it binds each key in insertion order, type `K`, and the body reads a value back with `m[key]` where it needs one. The entry count is fixed when the loop begins, so a key the body inserts is not visited on the current pass, while updating the value of a key already present is seen through a later read, since an update moves the value and not the key's place.

`sleep` and `together` are course statements. `sleep d` takes a `Duration` and pauses the course durably: the deadline is fixed the first time the step runs, and a parked instance keeps it, so it wakes on time.

`together { ... }` runs the contract's own pledges at the same time, as one step. Its block holds only single call statements of those pledges, in one of four forms, `name(args)`, `name(args)?`, `let x = name(args)`, and `let x = name(args)?`; anything else inside it, another statement, a nested block, a sleep, a return, is refused. The arguments are evaluated in order before any call starts, so an argument cannot read a binding made in the same block, and an argument cannot hold a suspending operation of its own; bind either with `let` before the block. Once every call has an outcome the `?`s apply in statement order, the first `Err` or `None` ending the course, and the bindings are visible after the block. A call that already latched before a `?` fires keeps its latch, and its remedy is due if the contract breaks.

A block used as a value takes the type of its final expression statement, and `Unit` when it ends with anything else. Match arms must agree on one type wherever the match sits, so a statement position match with mixed arms ends each arm in a statement or returns. `if` is a statement and never a value, `let x = if c { 1 } else { 2 }` does not parse; a value that depends on a condition comes from a `match` on the `Bool`, its arms `true -> 1` and `false -> 2`, or from a `let mut` the branches assign.

Integer arithmetic is checked. A `+`, `-`, `*`, or unary `-` on `Int`, `UInt`, `Byte`, `Duration`, or `Time` whose result leaves the range of its type, a `UInt` or `Byte` going below zero included, a `/` or `%` by zero, and the least `Int` divided by or taken modulo `-1` each fault the fulfillment with `GEAS_ERR_TYPE`, the status an out of range list index reports, and inside a course the fault stops the course the same way. Literal operands get no special treatment, `9223372036854775807 + 1` faults when it runs like any other sum. `Float` follows IEEE and never faults, and it has no `%`, so `%` on `Float` is a compile error.

`Unit` in value position is the one value of type `Unit`, so `return Ok(Unit)` is how a `Result<Unit, E>` body says it succeeded. It compares like any value, `Unit == Unit` is `true` and `Unit != Unit` is `false`. A binding or a declaration named `Unit` claims the name first.

`+` on two `String` values concatenates into a new `String`, and on two `Bytes` values into a new `Bytes`. The operands must have the same type. `Bytes` supports `==` and `!=`, comparing length then contents, and has no ordering comparisons.

`<`, `<=`, `>`, and `>=` order two operands of one numeric type, `Int`, `UInt`, `Float`, or `Byte`, or two `Duration` values or two `Time` values, and nothing else: `Char`, `String`, `Bool`, and `Bytes` have no ordering, and comparing two of them is `operator '<' needs matching numeric operands`. `==` and `!=` take two operands of any one type, composites compared structurally and an instance by identity.

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

`len(x)` is a builtin call, the length of a `List`, `Map`, or `Bytes` as an `Int`: a list answers its element count, a map its entry count, and Bytes its byte count. It is reserved in callee position only, so a binding may still be named `len`; `len` applied to any other type, or to other than one argument, is an error. There is no method form.

`b[i]` on Bytes takes an `Int` index and yields a `Byte`. It follows the List index rule: zero is the first byte, and a negative index or an index at or beyond `len(b)` faults the fulfillment with `GEAS_ERR_TYPE`. Bytes has no index assignment or iteration syntax; `byte_list(b)` supplies a list where either is needed.

The byte conversions are builtin calls with one positional argument, reserved in callee position the way `len` is:

| Call | Result |
| --- | --- |
| `bytes(s)` for a `String` | `Bytes`, the string's UTF-8 bytes |
| `bytes(xs)` for a `List<Byte>` | `Bytes`, one byte per element |
| `text(b)` for `Bytes` | `Option<String>`, `None` when UTF-8 is invalid |
| `byte_list(b)` for `Bytes` | `List<Byte>`, one element per byte |

Each conversion builds an independent value. `text` accepts embedded zeros and rejects truncated sequences, overlong encodings, surrogates, and values above U+10FFFF. There is no byte string literal, no implicit String conversion, and no Bytes vow default in this release. Build a buffer with `bytes("text")`, `bytes([0, 255])`, or a host value.

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
            | "-" ( INT | SIZE | FLOAT | DURATION )
            | "_"
            | IDENT                                   // binding
            | IDENT "(" Pattern { "," Pattern } ")"   // variant with payload
            | "Some" "(" Pattern ")" | "None"
            | "Ok" "(" Pattern ")" | "Err" "(" Pattern ")"
```

`match` is an expression. Arms are checked for exhaustiveness over sums, `Option`, `Result`, and `Bool`. A `_` or binding arm covers the rest. Non exhaustive matching over an open type like `Int` or `Duration` requires a final `_` arm.

Coverage reads one level deep. An arm covers its case, `Some`, `Err`, or a variant, only when everything nested inside its pattern is a name or `_`; an arm with a refutable pattern nested inside, `Some(Ok(x))`, `Some(0)`, or `Gateway(500)`, covers nothing toward exhaustiveness, so a match over such arms still needs an arm that takes the case whole, or a `_` or binding arm. The rule only ever asks for an arm that turns out unreachable, it never lets a value through unmatched.

### Await and select

These are course expressions, emitted with the course.

```text
AwaitExpr  ::= "await" IDENT [ "within" DURATION ]
SelectExpr ::= "select" "{" NL { SelectArm NL } "}"
SelectArm  ::= "await" IDENT [ "as" Pattern ] "->" ( Expr | Block )
             | "timeout" DURATION "->" ( Expr | Block )
```

`await sig` waits for a signal of the same contract and yields its payload: `Unit` for no parameters, the value for one, a `Tuple` for several. A signal delivered before the course reaches the `await` is not lost, the await answers at once with the payload the first delivery latched. `await sig within 24h` yields an `Option` of the payload, `None` when the time runs out, and the signal does not latch broken for it. An `await` on a signal whose own `within` already lapsed can never be answered: with `within` it yields `None` at once, and without one the course ends, cancelled when the lapse broke the contract and halted otherwise.

`select` holds at least one arm, an empty `select {}` is a compile error, and it waits on several things and takes the first arm that is ready, in source order when more than one is: an `await` arm binds its payload with `as`, a `timeout` arm fires after its duration, counted from the moment the course reached the select, and the arms agree on one type the way match arms do. The `as` pattern is irrefutable, a name or `_`, since a select binds the payload of the arm it chose and has no next arm to fall to; a pattern that could fail to match, `Some(x)`, a variant, a literal, is a compile error, and the payload is matched inside the arm instead.

```text
let parcel = select {
    await delivered as parcel -> parcel
    timeout 2d -> {
        chase(tracking, now() - shipped)?
        await delivered
    }
}
```

## The entry point

A standalone program declares exactly one `Main`:

```text
UsageError is either NoArgs

contract Main {
    pledge run(args: List<String>) -> Result<Int, UsageError> {
        if len(args) == 0 {
            return Err(NoArgs)
        }
        return Ok(len(args))
    }
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
- First class pledge values. The type `pledge(T, ...) -> R` and a bare `instance.pledge_name` with no call both parse, so the spelling stays reserved, but the checker refuses either wherever it appears with `pledge values are not part of this version of the language`. Call the pledge on its instance instead.
