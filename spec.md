# Geas Language Specification

## Status

This is the language reference for Geas, a statically typed language for
durable obligations. A contract declares its steps, their deadlines, ordering,
and retries, the remedies that undo what landed, the outside events it waits
on, and the policy that decides when the whole is fulfilled, partially
fulfilled, or broken. Any language that can load a C library signs and drives
that contract across the C ABI, and a shared runtime holds every term while it
does. This document describes the language as it stands today: contracts with
vows, pledges, clauses, subcontracts, and a requirements policy, durations and
times, terms, remedies, and signals, records and sums, the full statement and
expression core with exhaustive match and error propagation, value semantics
with one named exception for a signed instance, a package aware module system,
a first standard library, and the host surface that lets any language take
part with no generated bindings. Where this spec states a rule, the rule is the
one `geas` enforces and the runtime holds.

Geas runs in layers that share one lifecycle. The core language and the in
process runtime carry the contract itself. The workflow surface, terms,
remedies, signals, and the contract term, runs in two modes: host driven, the
host calling the steps and the runtime enforcing the rules, and engine driven,
a contract that carries its own `course`, which the runtime runs step by step.
The store layer backs a contract with a database and parks an instance into a
row another process can resume, and the bridge serves the same contracts over
gRPC. Typed amendment carries a parked instance from one version of its
contract to the next, checked at build time against the recorded shape of the
older version and applied by the runtime at the resume. The compiler is `geas`, written by hand
in C++23 and emitting C, source files end in `.geas`, a compiled module ends in
`.geas.so`, and the runtime is a shared C library, `libgeasrt`.

---

## Table of Contents

1. [Core Philosophy](#core-philosophy)
2. [Source Files, Imports, Packages](#source-files-imports-packages)
3. [Contracts](#contracts)
4. [Terms](#terms)
5. [Remedies](#remedies)
6. [Signals](#signals)
7. [The Lifecycle](#the-lifecycle)
8. [The Requirements Policy](#the-requirements-policy)
9. [Driving a Contract](#driving-a-contract)
10. [Amendment](#amendment)
11. [Type System](#type-system)
12. [Statements and Expressions](#statements-and-expressions)
13. [Provisional Clauses](#provisional-clauses)
14. [Cross Contract Calls](#cross-contract-calls)
15. [Standalone Programs](#standalone-programs)
16. [The Standard Library](#the-standard-library)
17. [Interop and the ABI](#interop-and-the-abi)
18. [The Store Layer](#the-store-layer)
19. [The Bridge](#the-bridge)

---

## Core Philosophy

- Everything is a contract. The contract is the unit of the language, not the
  function. A program is a set of contracts other languages sign and fulfill.
- Nothing is implicit. Ownership is always clear, visibility is always stated,
  numeric widths never convert on their own, and every fallible pledge returns a
  `Result` or an `Option`.
- Errors are values. There are no exceptions, and nothing throws across a
  language boundary. A failure is a value the caller matches on.
- Obligations have terms. Deadlines, ordering, retries, and a contract's whole
  lifetime are declarations the runtime enforces, not code a host has to get
  right, and a missed deadline is a lifecycle fact, never a value of the
  pledge's own error type.
- Breaking has consequences. A remedy undoes a step that landed, and a contract
  that breaks runs its remedies newest first, each handed the value its step
  produced.
- Any language can be a party. The runtime owns every heap allocation that
  crosses a boundary, data passes by value in both directions, and a reference
  is copied on entry and written back on return, never held by Geas.
- The runtime enforces the terms. A contract's shape, its vows, its fulfillment
  policy, its terms, and its version are checked at the boundary, and every
  value that crosses it is checked against its declared type all the way down,
  so a mismatch is a descriptive error and never silent corruption.

---

## Source Files, Imports, Packages

A source file is a list of top level declarations: contracts, records, sums,
provisional clauses, and imports. A file is a module, and a directory of files
is a package.

An `import` names a module by a dotted path:

```text
import std.math
import std.collections
```

A dotted path resolves beside the root file first and then under `GEAS_HOME`. A
directory is a package and a file is a module within it. Every imported file
parses into one shared tree, with each file's spans mapped back to its own
source so a diagnostic names the right place. A cycle in the import graph and a
duplicate import are named errors, and two builds of one source emit byte
identical module C.

Visibility is by package. A declaration is exported from its package by default
and visible to any importer. A declaration marked internal to its package is
invisible outside its own directory: an internal contract or provisional clause
cannot be incorporated, signed, or named in a type annotation from another
package, and an internal contract never reaches the descriptor tables or the
iname table a foreign host discovers. The boundary holds at every position,
including nested ones, so an internal type cannot leak through a public
signature.

---

## Contracts

A contract is the unit of Geas. It groups the state a signing locks, the
commitments a host fulfills, the internal methods those commitments share, and
the policy that decides when the whole is fulfilled, partially fulfilled, or
broken.

```text
contract PaymentService {
    incorporate Loggable

    vow currency: String = "USD"

    subcontract Validation {
        pledge validate_card(card: String) -> Result<Bool, Int> {
            return Ok(true)
        }
        pledge validate_amount(amount: Float) -> Result<Bool, Int> {
            if amount > 0.0 { return Ok(true) }
            return Err(1)
        }
    }

    subcontract Processing {
        pledge charge(card: String, amount: Float) -> Result<Bool, Int>
    }

    pledge notify_user(ok: Bool) -> Result<Bool, Int> {
        return Ok(true)
    }

    requirements {
        fulfill: Validation && Processing && notify_user
        partial: Validation || Processing
        break: !Validation && !Processing && !notify_user
    }
}
```

### Vows

A `vow` is an immutable field, locked the moment the contract is signed. A vow
carries a type and an optional default:

```text
vow currency: String = "USD"
```

A vow default is a constant of the vow's type. Nothing inside the contract may
assign to a vow, and a host may override a vow at sign, supplying a value of the
declared type. A vow with no default and no override at sign is an unbound
signing and is refused. A pledge body reads a vow by name.

### Pledges

A `pledge` is a callable commitment with a declared return type, and it is a
first class element of the language. Its return type is a `Result`, an `Option`,
`Unit`, or `Bytes`. A bare Bytes return latches fulfilled on delivery:

```text
pledge charge(card: String, amount: Float) -> Result<Bool, Int>
```

A pledge with a body is concrete. A pledge with no body is abstract: the
contract cannot be signed until a host binds an implementation for it, which is
how a foreign language plugs its own behavior into a contract. A pledge is
fulfilled through method syntax on a signed instance, `instance.charge(card,
amount)`, and the runtime also exposes a dynamic by name fulfillment for a
foreign host.

A pledge may take an argument by reference, a trailing declared parameter the
runtime copies in on entry and writes back to the host's pointer on return, so
host memory is touched only while the host is blocked in the call. See
[Interop and the ABI](#interop-and-the-abi).

### Clauses

A `clause` is an internal method. It never leaves its contract, has no place in
the lifecycle, and exists so pledges share logic:

```text
clause compare(a: Int, b: Int) -> Int {
    if a < b { return -1 }
    if a > b { return 1 }
    return 0
}
```

A clause is not a first class value and is contract scoped only. Clauses may
call each other in any order.

### Subcontracts

A `subcontract` groups pledges so a contract can be partially fulfilled and so
the requirements policy can name a group as a unit:

```text
subcontract Validation {
    pledge validate_card(card: String) -> Result<Bool, Int> { ... }
    pledge validate_amount(amount: Float) -> Result<Bool, Int> { ... }
}
```

A subcontract is satisfied when every member inside it has latched fulfilled. A
pledge declared outside any subcontract is a loose pledge and stands on its own
in the policy. A subcontract may also hold signals, and it may carry `within`
and `after` terms that cover every member inside it.

### Attributes

So a host setting is a declared attribute, a literal the host reads from the
contract descriptor before sign or from a signed instance. Its declaration
names the type and the kind of declaration it belongs on:

```text
attribute program: String on pledge
attribute region: Int on pledge
attribute region_max: Int on pledge
attribute threads: Int on pledge
attribute queue: String on contract

contract Job [queue: "batch"] {
    pledge copy(target: String) -> Result<Unit, CopyError> [program: "/bin/copy", region: 64M, region_max: 512M, threads: 1]
}
```

The type is `Int`, `Float`, `Bool`, `String`, or `Duration`, and the target is
`contract`, `subcontract`, `pledge`, `signal`, or `remedy`. Each declaration ends
at its newline. `attribute` and `on` are contextual words, ordinary names
outside that declaration. Imports bring declarations into scope, declarations
are visible across their package, and `internal attribute` keeps one inside
its package. A name may serve several targets, but cannot repeat on one target
or replace a builtin key that target owns.

The list follows the signature and any terms, before the body brace when
there is one. Subcontracts place it after their terms and before `{`, signals
after their parameters and terms. The opening `[` shares the line with what
precedes it. Values are literals checked against the declared type, sizes are
`Int` values in bytes, durations retain their own source type, and numbers and
durations allow unary minus. Unknown keys, wrong targets, wrong types, and a
key repeated in one list are compile errors.

Declared attributes stay outside the shape hash and mangled names. Changing a
job's queue or a step's memory budget leaves a parked instance compatible. The
runtime publishes those values as immutable descriptor data, available until
runtime shutdown, and the host decides how to apply them.

A contract carries an optional version attribute, `[version: N]`, default 1. The
version is baked into every mangled name, so a host that signs against the wrong
version is refused at discovery rather than corrupted at call time.

A contract may also carry a term, `[term: 7d]`, its whole lifetime counted from
sign. When the term runs out while the contract is still Signed or Partial, the
contract breaks the way a matched break line breaks it, remedies included, and
the host can read that the term is what broke it. A contract already Fulfilled
when its term passes met its terms and is left alone.

```text
contract OrderSaga [version: 1, term: 7d] {
    ...
}
```

---

## Terms

Terms are the rules of an obligation in time: when a step may run, how long it
has, and what happens when it fails. They trail a signature, one per line if
you like:

```text
pledge authorize(card: Card) -> Result<AuthId, PayError>
    within 30s
    retry 3 every 2s backoff 2.0 cap 30s on Timeout, Gateway
pledge capture(auth: AuthId) -> Result<Receipt, PayError>
    after authorize
    within 1m
```

A pledge takes all three terms, a subcontract and a signal take `within` and
`after`, and a remedy takes `within` and `retry`, each at most once.

So the idea underneath all three is eligibility. Every item becomes eligible
at some moment, at sign when it waits on nothing, otherwise the moment
everything it waits on has latched fulfilled, and that stamp is written once
and never moves.

**`after`** is ordering. It names pledges, subcontracts, or signals of the same
contract that must latch fulfilled first. Calling a step before it is eligible
answers a state error and its body never runs, so nothing is latched and the
call can be made again once the steps ahead of it land. An `Err` never
satisfies an `after`, so a step waiting on one that broke never becomes
eligible. The checker rejects a cycle and names the path.

**`within`** is a deadline counted from eligibility. A step still pending when
it passes lapses: it latches broken, with no error payload, because the
pledge's own error type has no say over a deadline, and the runtime marks the
latch as a lapse so a host can tell a missed deadline from an `Err`. A lapse
arms the break line like any broken latch. A body started in time lands its
outcome even if it returns late, since the work it did is real. A
subcontract's `within` lapses every member inside it that has not latched.

**`retry`** is the number of attempts after the first. The delay before each
retry starts at `every`, multiplies by `backoff` each time, a float of at
least `1.0`, and stops growing at `cap`. Without `on`, only lifecycle failures
retry, a lost worker or a lapsed attempt, never a business `Err`, because a
declined card is an answer and not an accident. With `on`, the listed variants
of the pledge's error sum retry too. Only the last attempt's outcome is
latched, and a delay is never waited past the step's own deadline. A park
during the delay records the attempts already made and the absolute time
the next attempt is due. Resume keeps that budget, and an early fulfillment
is refused until the due time. The host supplies the pending pledge's
arguments again; a tick alone does not run it.

The runtime owns no timer. Every call that reads or moves an instance applies
the deadlines that have passed first, in deadline order, and a host that wants
a lapse the moment it falls due ticks the runtime from whatever scheduler it
has. The clock itself is replaceable, which is how a test turns a three day
deadline into a number it moves. [docs/abi.md](docs/abi.md) carries the exact
rules under The clock and the tick and Terms.

---

## Remedies

A remedy undoes one pledge that landed, the compensation step of a saga:

```text
remedy refund for capture(receipt: Receipt) -> Result<Unit, PayError>
    retry 2 every 1m
remedy release for reserve(res: Reservation) -> Result<Unit, StockError> {
    if res.id != "R-" + res.sku {
        return Err(Unavailable)
    }
    return Ok(released())
}
```

The target after `for` is a pledge of the same contract, and a pledge has at
most one remedy. The parameter receives the target's `Ok` value, so its type is
the `T` of the target's `Result<T, E>`; a target returning `Unit` or an
`Option` yields no `Ok` value and its remedy takes no parameter, and a remedy
that has no use for the value may leave the parameter off. A remedy returns
`Result<Unit, E>` or `Unit`, and it is bound the ways a pledge is, a Geas
body, a foreign symbol, or a host binding, an unbound one refusing the sign.

When a contract breaks, by its break line, by a lapse that matches it, by its
term running out, or by an explicit break while it is still Signed or Partial,
every pledge that latched fulfilled and carries a remedy gets it run, one at a
time, newest latch first. While they run the contract reads `Remedying`, and
`Broken` once the last one returns. A remedy that fails, by an `Err`, by
running out of retries, or by its own `within`, is recorded on the partial
surface and the pass moves on, so one stuck refund never strands a
reservation. Every remedy runs at most once for the life of a signature, a
park and a resume included. A pledge inside a transactional subcontract whose
episode never committed has nothing to undo, the rollback already did.

A pass can park between remedies, inside a remedy body, or during a remedy
retry wait, including from the host sleep callback. Finished remedies keep
their outcomes, an in flight remedy resumes failed with `GEAS_ERR_STATE`,
and remedies that had not started run on the first tick after resume. The
resumed pass ends Broken, without repeating remedy history or its events.

A remedy is never a requirements atom and never called through an instance; it
belongs to the break, not to the host.

---

## Signals

A signal is an outside event delivered into a live instance, a webhook, an
approval, a delivery scan:

```text
signal delivered(tracking: Tracking) after ship within 3d
```

A signal has no body and no return type. It latches fulfilled the moment it is
delivered, and the first delivery wins: a repeat is accepted and changes
nothing, so a host passing a retried webhook along need not deduplicate first.
The payload is kept on the instance, `Unit` with no parameters, the value with
one, a `Tuple` with several, and it parks and resumes with the instance. A
signal takes `within` and `after` like a pledge, so `delivered` above cannot
arrive before `ship` landed and lapses if three days pass without it.

A loose signal is a requirements atom like a loose pledge; a signal inside a
subcontract counts toward its subcontract. From a host a signal is delivered
with `geas_signal`, from Python with `instance.signal(name, ...)`, and from
inside Geas through a signed instance, `courier.scanned(at)`, a call answering
`Unit` whose refusal faults the calling body. A signal is never fulfilled like
a pledge and a pledge is never delivered like a signal; the two surfaces refuse
each other's names. A transactional subcontract holds no signal, since an
outside event never belongs inside an open transaction.

---

## The Lifecycle

A contract moves through six states:

```text
Unsigned -> Signed -> Fulfilled
                   -> Partial
                   -> Remedying -> Broken
                   -> Broken                (no remedies due)
```

`sign()` validates the contract's shape, locks its vows, and activates it. It
takes named vow overrides and an optional expected shape hash, and it refuses an
abstract pledge that nothing has bound, an override naming no vow, an override of
the wrong type, and a hash that disagrees.

Fulfilling a pledge runs it and records the outcome. A pledge latches on its
first `Ok`: it becomes fulfilled and stays fulfilled, and later calls run and
return results but never change the latched state. An `Err` before any `Ok`
latches the pledge broken with the error payload kept readable, and so does a
lapse, a deadline that passed first, with no payload and the lapse marked.
Delivering a signal latches it fulfilled. Contract state never regresses.

When the contract breaks, by its policy, by its term, or by an explicit
`break()` while it is still Signed or Partial, it reads `Remedying` while its
due remedies run and `Broken` once they are done; with no remedy due it goes
straight to `Broken`. A Fulfilled contract never runs a remedy. `break()` then
tears the contract down and reclaims every allocation the runtime owns for it,
in one walk. Every later fulfillment on a broken contract is a state error.

The partial surface reports, at any moment, which subcontracts, loose pledges,
and loose signals are fulfilled, which are pending, and which are broken, with
the latched errors attached by name, which broken latches are lapses, and how
every remedy went.

### Cancel

`break()` is cancel. When a step body is running on another thread, break waits
for that body to return, never interrupts it, then runs the remedies if the
contract was Signed or Partial and reclaims the instance. The body's outcome
still latches first, so a step that finishes with `Ok` after cancel gets its
remedy and a step whose first outcome is `Err` does not. The last step gets its
remedy too, even if its `Ok` would have fulfilled the whole contract.

A body may poll whether cancel was requested through the host boundary, using
`geas_cancel_requested(ctx)` in C or `inst.cancel_requested()` in a Python bound
callback. The poll also reads true during runtime shutdown. A body that calls
`break()` on its own instance leaves the break pending until the outermost
fulfillment ends, and the poll reads true from that request onward. A retry
delay ends on cancel, the fulfillment reports a state error, and no further
attempt starts. Remedies still run through their own terms.

---

## The Requirements Policy

The `requirements` block writes the contract's fulfillment as boolean logic over
its subcontracts, loose pledges, and loose signals:

```text
requirements {
    fulfill: Validation && Processing && notify_user
    partial: Validation || Processing
    break: !Validation && !Processing && !notify_user
}
```

A bare name means that item is fulfilled. `&&`, `||`, and `!` combine the
conditions. Three lines are defined:

- **fulfill**: the condition under which the contract is fully fulfilled.
- **partial**: the condition under which it is partially fulfilled.
- **break**: the condition under which it breaks.

The runtime evaluates the policy after every outcome, under the instance's lock,
in the priority break, then fulfill, then partial. When the break line holds the
contract transitions to broken by itself, keeping its heap alive so the error
payloads stay readable until an explicit break reclaims them. The break line is
armed only once something has actually broken, so an empty contract does not
break on the vacuous truth of its negations.

The compiler proves at build time that no single state satisfies both the
fulfill and the break line, naming a witness if one exists, and rejects a policy
line no state can satisfy. It searches the boolean trees with unit propagation
and atom branching, keeping the witness in the order the atoms were written.
The requirements block compiles to a shared table the
static checker and the runtime evaluator both read, so the two can never
diverge. When a contract writes no requirements block, the grammar's defaults are
synthesized into the same table form. A remedy is never an atom. A written block
names at most 64 distinct atoms across its three lines. That cap belongs to
written blocks; synthesized defaults work through 255 items. When their
fulfilled and broken atom rows would exceed the byte sized descriptor index,
the runtime reads the same default directly from the subcontract and pledge
tables, with the same fulfillment, partial fulfillment, and break rules.

---

## Driving a Contract

A contract can be driven two ways, and the two share everything on the
outside: the requirements, the terms, the remedies, and the lifecycle. So a
contract can start in one and grow into the other without a host noticing its
shape change.

**Host driven** is the mode of any contract with no `course`. A host signs
the contract and calls its pledges in whatever order it likes, and the runtime
enforces the terms: an early call answers a state error, a missed deadline
latches the step broken, retries happen inside the runtime, and remedies run
on break. This is the right shape for in process glue and for hosts that want
to stay in charge of the order.

**Engine driven** is the mode of a contract that carries a `course` block, its
orchestration written out, and the runtime runs it from the moment the
contract signs. `skeleton/order_course.geas` is the order saga written this
way:

```text
course {
    if amount <= 0 {
        return
    }
    let window = plan(hold)?
    let auth = authorize(Card { number: card })?
    together {
        let receipt = capture(auth)?
        let held = reserve(sku, window)?
    }
    let tracking = ship(held)?
    let shipped = now()
    let parcel = select {
        await delivered as parcel -> parcel
        timeout 2d -> {
            chase(tracking, now() - shipped)?
            await delivered
        }
    }
}
```

So the host's job shrinks to two things, it binds the bodies the contract
leaves abstract and it hands signals in. It never calls a pledge of such a
contract: a host fulfillment answers a state error and its body never runs,
and the checker refuses a Geas body that tries.

*How the course is driven.* The compiler lowers the course to a C function
that is a state machine over a frame, a step number and a set of slots that
hold every binding, every loop's position, and every value that has to
outlive a step. Signing schedules the first entry on the runtime's pool, and a
pool worker runs the function with the instance lock held. Each call of one
of the contract's own pledges is a step and runs inline on that worker
through the same machinery a host fulfillment uses, the eligibility gate, the
terms, the retries, the latch, the policy, the events, and the remedy pass a
break makes due. Between two steps the course is ordinary code.

*Steps are at least once.* After every step the course stores its next step
number, and an armed autopark writes the instance's row right there. A
resumed course enters at the step its row names with the slots its row
carried, so a step that finished before the row was written never runs again,
and a step that was running when the process died runs again from its start.
A body that must not act twice keys its side effect the way any at least once
worker does. The same boundary publishes the step number, so a host reading
the course while it runs sees the step it stands at.

*Suspend and wake.* At an `await`, a `sleep`, a `select`, or a `together`
that cannot finish yet, the function returns to the runtime, the course reads
WAITING, and its worker goes back to the pool. A signal's first delivery, a
tick that finds the wait's deadline due, or the last call of a `together`
finishing schedules the next entry, which jumps straight to the saved step.
A wait's deadline is fixed the first time its step runs and travels with the
row, so neither a re-entry nor a park ever moves it. A deadline falls due only
when something ticks, the host's own timer through `geas_runtime_tick` or the
runtime's ticker thread, and like every deadline it is applied when no body is
in flight on the instance.

*A delivery never stops a course.* The event hook a host installs is called
from the thread that recorded the events, a signal's caller, the ticker, a
worker, and while that thread delivers, the instance is reserved to it. A
course that meets such a delivery at one of its operations, at the end of a
retry delay, at a step boundary, or where an armed autopark writes its row,
waits for it to end and goes on, so a course never halts or is cancelled
over a delivery it had no part in. A host driven fulfillment that meets one
is refused with `GEAS_ERR_STATE` instead, since the host thread may hold a
lock the hook is waiting on, and the host calls again once the delivery
ended, which its own hook is the one to tell it.

*Together.* The calls of a `together` start at once on the pool, each with
the instance lock released, so two bodies really run at the same time, bounded
by the pool's size. A pledge of a transactional subcontract is the exception:
its body keeps the instance lock, since its episode rides the instance's open
store transaction. Once every call has an outcome the course is entered again,
the results land in their bindings, and the `?`s apply in statement order.

*Cancel.* A break the host asks for, an automatic one from a break line or a
lapse, or the contract term running out cancels the course. A waiting course
is dropped at once. A running one is cut off at its next operation, after the
step it is inside returns and latches, which is what a host break waits for by
waiting on the instance lock. A host break during a `together` returns with
the contract Remedying while the calls still run; they finish and latch, and
the last one back runs the remedy pass, so whatever landed is undone with the
rest.

*The course's end is not the contract's state.* A course finishes when it
falls off its end, runs `return`, or meets an `Err` or a `None` at a `?`; it
halts when an operation answers a runtime status, a failed autopark write
among them; and it is cancelled by a break. None of those sets the contract's
state, the requirements decide it from the latches, so a course whose last
step answered `Err` reads finished while the break line it tripped has already
moved the contract to Remedying and then Broken. A course that runs to its end
before a signal it does not wait on arrives leaves the contract Partial, and
the signal can still fulfill it.

*Autopark and resume.* A host arms an instance with a store and a key, and
from then on the runtime writes its row at every step boundary, whenever the
course waits or ends, and at every latch, lapse, state change, and remedy, the
same row `geas_instance_park` writes. A resume in a fresh runtime enters a
waiting or running course at its saved step; a finished, halted, or cancelled
one stays as it was. The row carries a hash of the course, its step count,
its slot count, and the tokens of its source, and a module whose course hash
differs refuses the row with `GEAS_ERR_VERSION`, since a frame laid out for
one course cannot drive another. A course that changes ships under a new
contract version, the job amendment takes on.

*Handing off.* So a park is a checkpoint and the instance stays live where
it is, however moving an obligation to another process wants the opposite,
the old process letting go and the new one taking up, with no remedy run in
between for work the new one is about to finish. The host parks the
instance, or lets autopark write its row, then releases it, which waits out
a running body, cuts the course, rolls back an open episode, and frees the
instance without running a remedy or moving its state, and the other process
claims the row, which resumes it and deletes it in one move so that of two
claimers exactly one gets the instance. A claimed course goes on from the
step its row names. A cancel request on its own raises the flag a body polls
and cuts the course while the instance stays live, which is how a host asks
its bodies to wind down before it releases or breaks. The calls are in
`docs/abi.md` under Handing an instance off.

The full runtime contract, the descriptor fields, the frame, the operations
the emitted function calls, and the host calls that read and wait on a course,
is in `docs/abi.md` under The course.

---

## Amendment

A parked instance can outlive the version of the contract it was signed
under. A saga waiting three days on a delivery scan does not care that a new
module shipped in the meantime, so a contract says how an instance parked
under an older version comes back as this one. Inside the order saga at
version 2, `skeleton/order_course_v2.geas`:

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

`vow` supplies a vow of this version, reading the parked instance's vows
through `prior`, `rename` maps a member of the older version onto one of
this version, `retire` drops a member and whatever it stored, `latch` sets
the starting state of a pledge or signal the older version did not have,
and `restart course` enters a changed course from the top.

*The model.* A park row records the contract version and the shape hash it
was written under. A row whose version and hash are the registered
contract's resumes the way it always did. Otherwise the runtime looks
through the contract's amends table for the block whose version is the
row's and whose recorded shape hash is the row's hash, and when one is
there it applies it; when none is, the resume answers `GEAS_ERR_VERSION`
and the row is left exactly as it was, so a later module that does carry
the block still resumes it. The apply is all or nothing: the row is
decoded against the older version's counts, mapped onto a fresh instance of
this version, and the instance is published only when every step landed, so
a failed apply leaves nothing half built and nothing written. Blocks do not
chain. Each one maps its version straight to the one that carries it, and a
version 3 that wants version 1 rows to resume carries `amends 1` beside
`amends 2`.

*Where the older shape comes from.* The compiler only has the source of the
version it compiles, so the shape of the older version is recorded. `geas
record` writes `shapes/<Contract>.v<N>.shape` beside the source, the shape
string and its hash, the course hash, the names the row is positional over,
and the layout of every declared record and sum the boundary reaches, and a
block is checked against that file. Every difference between the two shapes
has to be covered by exactly one item, and an item no difference calls for is
an error too, which turns a resume nobody planned for into a compile error
instead of a surprise in production. The rules are in `docs/grammar.md`
under Amendment.

*The order of the apply.* The course frame is checked first: a row whose
frame was written by another course than the one the block names is
refused. Then the vows: every vow the two versions share is carried, this
version's defaults fill what is still empty, and the block's `vow` items run
in source order, each reading `prior.NAME` and this version's vows as filled
so far. Then the contract's own sign time, term lapse, and latch counter,
then each item of the older version through the block: its latch, its `Err`
payload, its eligibility stamp, its lapse, its place in the latch order, its
stored `Ok` value or signal payload, and its retry progress held to this
version's retry budget. Then the subcontracts' transactional fates, the
remedy records, the seeds of the items this version adds, and eligibility
under this version's terms. The course frame is kept, dropped, or left for a
fresh start last.

*The course.* When both versions carry the same course, by hash, its frame
carries over and the course goes on at the step it parked at. When the
course changed, or only the new version has one, the block says `restart
course` and the resumed instance enters the course at step 0 with a fresh
frame. Steps are at least once, so a step whose pledge already latched runs
its body again: the course binds what that run returns, while the latch, the
stored `Ok` value a remedy is handed, and the latch order stay the ones the
older version recorded. When only the older version had a course its frame
is dropped and the contract goes on host driven.

*After the apply.* The runtime raises an `amended` event naming the version
left and the version taken, then `resumed`, then, for a restarted course,
`course started`. From then on the instance is an ordinary instance of this
version: its terms and requirements are this version's, its next park
writes a row of this version, and resuming that row is a plain resume.
`geas_contract_amended_from` answers the older version on the instance that
went through a block and 0 on any other. A break after the amendment runs
the remedies of this version over what landed in both, newest latch first,
each handed the `Ok` value its pledge stored, the one the older version
stored for a pledge that latched before the deploy.

*Remedies at the resume.* This version's policy is evaluated once, after
the `amended` and `resumed` events. When it already breaks the instance, a
`latch NAME as broken` seed matching the new break line for one, the
instance reads Remedying and no remedy runs inside the resume; the first
tick, an explicit one or any call that ticks, runs the pass, the same rule a
row parked in the middle of a pass follows. A row parked mid pass keeps the
records of the remedies that ran, under their new names, and its first tick
runs the ones still owed.

*The shape covers layout.* A type code names a declared record or sum and
nothing more, so the shape string spells the layout of every record and sum
the boundary reaches beside the codes, and every pledge and remedy signature
hash covers the layouts its own signature reaches. A record that gained a
field therefore moves the shape hash and the mangled names of the pledges
that carry it, a row written before the change is a row of another shape,
and an amends block treats a vow, a stored value, or a signal payload whose
layout changed as a changed type.

---

## Type System

### Primitive types

| type | representation |
|---|---|
| `Unit` | the empty value |
| `Int` | 64 bit signed |
| `UInt` | 64 bit unsigned |
| `Float` | IEEE 754 double |
| `Bool` | one byte, true or false |
| `Byte` | one byte |
| `Char` | a 32 bit Unicode scalar value |
| `String` | UTF-8 bytes, a pointer and a byte length, no terminator |
| `Bytes` | arbitrary bytes, a pointer and a byte length, no terminator |
| `Duration` | a span of time, 64 bit signed milliseconds |
| `Time` | a point in time, 64 bit signed milliseconds since the Unix epoch |

Numeric widths are fixed and never convert implicitly. A widening or narrowing
is a type error unless written explicitly, and there is no implicit conversion
anywhere in the language, widths included.

### Byte buffers

`Bytes` is a raw buffer, so a host can hand a record across a pledge without
building a `List<Byte>` with one tagged value per byte. Zeros and invalid UTF-8
are ordinary contents. It fits in parameters, returns, vows, records, sum
payloads, and containers; a map value may be Bytes, a map key and a schema
column may not. A declaration named `Bytes` takes precedence over the builtin.

`len(b)` counts bytes as an `Int`, `b[i]` takes an Int and returns a Byte,
with the same bounds fault as List indexing, and `+` joins two buffers into a
new one. Equality compares length and contents. Bytes has no ordering,
index assignment, or direct iteration.

`bytes(s)` copies a String's UTF-8 bytes and `bytes(xs)` packs a List<Byte>.
`byte_list(b)` expands the buffer back to that list. `text(b)` returns
`Option<String>`, None for invalid UTF-8, including overlong encodings and
surrogates, and Some for valid text including embedded zeros. These names
are builtins in callee position, the same rule as `len`. There is no byte
string literal and a Bytes vow has no default, so signing supplies it.

The ABI appends tag 17 and uses the existing string arm without changing a
struct layout. Buffers are instance owned, deep copied at the boundary,
and carried by the value wire codec through park and resume. Their canonical
type code is `X`, so String and Bytes have distinct hashes and a contract
without Bytes keeps its hash and mangled names. Proto uses `bytes`, Go uses
`[]byte`, and the TypeScript session wrapper uses `Uint8Array`.

### Size literals

A size literal is an integer followed immediately by `K`, `M`, `G`, or `T`,
so `64M` is 67108864 bytes and `512K` is 524288 bytes. The multiples are binary,
1024, 1024 squared, 1024 cubed, and 1024 to the fourth power. It types as `Int`,
or as `UInt` or `Byte` when that type is expected and the byte count fits,
the same rule an ordinary integer literal follows. Attributes, vow defaults,
patterns, and unary minus accept it too, and emitted C holds the plain integer.

The suffix is uppercase: `5M` counts five mebibytes, while `5m` remains five
minutes. A size that exceeds the signed 64 bit maximum is a lex error, as is a
suffix followed by more identifier characters, `64Mb` or `1Gx`. Hexadecimal,
binary, and floating point literals take no size suffix. Lowercase `k`, `g`,
and `t` stay invalid numeric suffixes. The lex and parse dumps show `Size(bytes)`
so you can still see which spelling the source used.

### Durations and times

A duration is a literal of its own, `30s`, `250ms`, `1h30m`, `7d`, one token
with its units running from largest to smallest, and it types as `Duration`.
A `Time` has no literal; it comes from a vow, a parameter, a signal payload, or
`now()` inside a course. Their arithmetic is small on purpose: durations add
and subtract, a duration scales by an `Int` on its right, a time shifts by a
duration, and two times subtract into the duration between them. Two durations
or two times compare. `Duration * Float`, `Duration + Int`, and `Time + Time`
are type errors, so a unit mistake is caught where it is written.

To the checker both are types of their own, and they hash and mangle apart from
`Int`. To everything past the checker they are milliseconds: both cross the
boundary as an `Int`, a C host reads `v.as.i`, a Python host gets a plain int,
and a `Time` a host hands in is directly comparable with a deadline the runtime
computes, since the runtime clock answers milliseconds since the epoch too.

### Composite types

- **`List<T>`**: a homogeneous, ordered sequence, written `[a, b, c]`.
- **Tuples**: a fixed heterogeneous group whose elements need not agree in type.
- **The generic constructors**: `List<T>(a, b)`, `Option<T>()` and
  `Option<T>(v)`, `Result<T, E>(Ok(v))`, `Tuple<A, B>(a, b)`, and `Map<K, V>()`
  spell their type and are held to it, which is how a value gets its type where
  nothing else in the expression supplies one.
- **`Map<K, V>`**: an ordered map, constructed `Map<K, V>()`, with a keyable
  scalar key. An index read answers `Option<V>`, `None` on a miss, and an index
  assignment inserts or updates. Entries keep insertion order, and that order is
  what serialization sees, what equality compares, and what `for` walks: a `for`
  over a map binds the key, first inserted first, and the body reads the value
  through the map itself. `len` answers the entry count of a map and the element
  count of a list, the one builtin both collections share.

### Records

A record is a product type, a fixed set of named fields:

```text
record Card {
    number: String
    amount: Float
}
```

A record is built with a brace literal on the type's name, its fields read by
name, and it carries value semantics like every composite.

### Sums

A sum is a choice between variants, each of which may carry a payload, written
`is either`:

```text
DemoError is either BadMood(reason: String) or Empty
```

A variant is built by naming it, `BadMood("asked to fail")` or `Empty`, and
taken apart with `match`. A sum's representation carries the variant's
declaration index as its tag.

### Option and Result

`Option<T>` is `Some(T)` or `None`. `Result<T, E>` is `Ok(T)` or `Err(E)`. They
are how nullability and failure are expressed, and the checker holds a `match`
on either to both arms.

### The contract type at the boundary

A contract type is forbidden at every ABI position: a pledge, remedy, or signal
signature, a vow, a record or variant field, at any nesting depth. A signed instance is a value only
inside a pledge body, never a value that crosses the boundary, so nothing a
foreign host receives can carry an instance it cannot own. See
[Cross Contract Calls](#cross-contract-calls).

A pledge type, `pledge(Int) -> Unit`, is held to the same line. A pledge value
is bound to the instance that made it and means nothing outside that process,
so it is a value only inside the module, in a clause parameter, a clause
return, and a local binding, and the checker refuses it at every position a
contract type is refused.

---

## Statements and Expressions

A pledge body, a clause body, a remedy body, and a subcontract pledge body are
ordinary imperative code, and so is a course, with the course statements on
top. A statement is terminated by a newline at the top group depth,
so a block inside an argument list keeps its statement semantics.

### Bindings

`let` binds an immutable name, `let mut` a reassignable one:

```text
let n = 0
let mut xs = [a, b, c]
```

An unused binding is not silently accepted; the language names deferred and
unsupported constructs rather than accepting them quietly.

### Control flow

- `if` and `else`, with `else if` chaining as ordinary nested `if`s. A bare
  record literal is banned in an `if`, `while`, `for`, or `match` head so the
  brace is never ambiguous.
- `while cond { }` loops on a condition.
- `for x in list { }` walks a list.
- `break` and `continue` inside a loop.
- `return` leaves a pledge or clause with a value of its declared type.

### Match

`match` is an exhaustive expression over sums, `Option`, `Result`, and `Bool`.
It must cover every variant, and the checker rejects it otherwise. Each arm binds
the payload it matches:

```text
match r {
    Ok(v) -> { return Ok(v * 2) }
    Err(_) -> { return Err(-1) }
}
```

### Propagation

`?` propagates an error. `f(x)?` runs a fallible call, returns its `Err` from the
enclosing pledge immediately, and unwraps its `Ok` otherwise. The callee's error
type must be the enclosing return type's error type.

### Blocks as values

A block is an expression, and its value is its last expression, so a `match` arm
or an `if` branch can produce a value directly.

### Operators

The language has an eight level precedence ladder. `String + String`
and `Bytes + Bytes` concatenate. The comparison and equality operators work over the primitive
types. The logical operators `&&` and `||` short circuit: the right operand is
evaluated only when the left does not already decide the result, so a bounds
check like `i < n && xs[i] == 0` never touches `xs[i]` when `i < n` is false.

### Value semantics

Composites carry value semantics at every boundary. A `let` of a composite, an
assignment, and a composite read all deep copy, so mutation through one binding
is never visible through another. The one place a write lands in place is an
assignment target, `xs[0] = y` or `record.field = y` or `map[k] = y`, which walks
to the named slot and writes there while the right side still copies. An out of
range list index or a missing map key mid chain is a clean type error, never a
fault, and the fault checks run before the right side evaluates.

The single exception to value semantics is a signed instance, described next.

---

## Provisional Clauses

A provisional clause is Geas's trait: a named set of clause signatures a
contract can promise to satisfy.

```text
provisional clause Comparable {
    clause compare(a: Int, b: Int) -> Int
}
```

A contract `incorporate`s a provisional clause and implements its clauses:

```text
contract StdUser {
    incorporate Comparable
    incorporate Loggable

    clause compare(a: Int, b: Int) -> Int { ... }
    clause log_line(msg: String) -> String { ... }
}
```

An incorporated signature must match its clause implementation exactly. Once
incorporated, the contract is of that provisional clause, and its pledges use the
implemented clauses like any internal method.

---

## Cross Contract Calls

A pledge body can sign another contract, fulfill its pledges, and break it, which
is how contracts compose:

```text
pledge compute(x: Int) -> Result<Int, Int> {
    let ops = MathOps.sign(epsilon: 0.5)
    let r = ops.abs(x)
    ops.break()
    match r {
        Ok(v) -> {
            let gate = IntGate.sign()
            let d = gate.double_pos(v)?
            gate.break()
            return Ok(d)
        }
        Err(_) -> { return Err(-1) }
    }
}
```

`MathOps.sign(epsilon: 0.5)` signs with a vow override, `ops.abs(x)` fulfills a
pledge through method syntax, and `ops.break()` tears it down. A cross contract
call copies the callee's result home before use, so the result outlives the
callee breaking, which is why `r` is safe to read after `ops.break()`.

A signed instance is the one value in the language with reference semantics: a
copy of the handle aliases the same instance, and equality is identity. It is
also the one value that can never cross the C boundary; only its results can. A
fulfillment issued from inside another fulfillment runs inline on the same
worker, so nested calls cannot starve the pool.

---

## Standalone Programs

Geas is a language you can compile straight to an executable. A program
declares exactly one `Main` contract with a `run` pledge over `List<String>`
returning `Result<Int, E>`:

```text
contract Main {
    vow greeting: String = "counted"

    pledge run(args: List<String>) -> Result<Int, DemoError> {
        let mut n = 0
        for a in args {
            if a == "fail" { return Err(BadMood("asked to fail")) }
            n = n + 1
        }
        return Ok(n)
    }
}
```

`geas build --bin` links it into a real program: the generated wrapper signs
`Main`, hands `argv` to `run` as a `List<String>`, and maps the result onto the
process exit code. `Ok(n)` becomes the exit code, and an `Err` renders itself to
stderr and exits 1. A `--bin` module still emits every contract in the file, not
`Main` alone.

---

## The Standard Library

A first standard library lives under `lib/std` and is pulled in with `import`:

- **`std.math`**: overflow checked arithmetic pledges and an epsilon vow.
- **`std.strings`** and **`std.collections`**: string and list helpers
  shaped to what the language expresses today.
- **`std.errors`**: common error sums for the `E` slot.
- **`std.traits`**: the provisional clauses, `Comparable`, `Loggable`, and
  `Serializable`.
- **`std.process`**: a program step, `Process.run(Command)` returning
  `Result<Unit, ExitError>` on the caller's instance.

A module lends another its record and sum shapes, its error sums, its provisional
clauses, and its contracts themselves. A pledge in one module can sign a contract
declared in another, fulfill its pledges, and break it.

### Program steps

`import std.process` brings in `Command`, with `program: String`,
`args: List<String>`, `stdout_path: String`, and `stderr_path: String`, and
`ExitError`, either `Exited(code: Int)`, `Signaled(number: Int)`, or
`SpawnFailed(reason: String)`. `Process.run(command)` is a builtin namespace
operation like `Store`, callable from a pledge, clause, or remedy. It runs on
the caller's instance so cancellation and the result share that instance's
lifetime, with no helper contract to sign or host binding to supply.

The program is a path, relative to the process cwd when it is not absolute.
There is no PATH search or shell expansion, and `args` excludes the program
name. The child inherits the environment and reads stdin from `/dev/null`.
An empty output path inherits that stream, any other path is opened for append,
created with mode 0644 subject to the host's umask. Exit zero returns `Ok(Unit)`,
a nonzero exit returns `Exited`, a terminating signal returns `Signaled`, and
a launch failure returns `SpawnFailed` with a readable reason. Interior NUL
bytes in the program, arguments, or output paths are launch failures.

The runtime uses `posix_spawn` and its file actions, so it runs no application
code in a forked child of the threaded host. The closefrom spawn action closes
every descriptor above 2, including descriptors another host thread opened.
This helper requires the GNU closefrom spawn action available in glibc 2.34
and later. The wait retries `waitpid` on EINTR and polls the caller's atomic
cancellation flag every 10 ms, which a breaker publishes before taking the
instance lock. Cancellation sends SIGTERM, allows a 200 ms grace period, then
sends SIGKILL if the child remains alive. The result names the signal that
ended the child, and every launched child is reaped before the helper returns.

---

## Interop and the ABI

This is how every language takes part. A compiled `.geas.so` module carries
descriptor tables that spell every contract, every pledge and its signature, and
every vow and its default. When the runtime loads the module it registers all of
it in the iname table, and a foreign host drives a contract through a handful of
C calls with no binding generated for it.

### The iname table

The iname table is a sorted registry of every contract and pledge, keyed by a
mangled name that bakes in the contract, the pledge, a 64 bit FNV-1a hash of the
type signature, and the version. A host looks a contract up by that name, and
because the name carries the shape hash, a version mismatch is a clean error at
discovery time. The table supports lookup by exact mangled name, ordered
enumeration, and a canonical one line per entry dump. `geas_runtime_freeze` makes
the table immutable: after it, loading, registering, and binding refuse, while
signing and fulfilling continue.

The mangled name has the form
`__geas_ash_{contract}_{symbol}_{sighash}_v{n}`, and `geas emit-header` writes the
shape hash and every mangled name as C defines so a host resolves against
generated names instead of string literals.

### Values

Every value that crosses the boundary is an `GeasValue`, a tagged union whose
layout is fixed and documented. Integers, floats, strings, lists, tuples, maps,
records, sums, options, and results each have a pinned representation. A string
is a fat value, a pointer and a byte length, UTF-8, no terminator. A list, a
tuple, a record, and a map all ride one array arm with different rules.

### The checked boundary

Every value that crosses into a contract is checked to the bottom against its
declared type: a fulfillment's arguments, by value and by reference, a body's
answer, a signal's payload, a vow at sign, a remedy's argument and answer, and
the values a course call carries. The check reaches every element of a list,
every field of a record and a variant, every key and value of a map, and the
payload of every `Option` and `Result`, so a `Bool` that is not 0 or 1, a
`Char` that is not a Unicode scalar value, a `String` that is not UTF-8, a
record a field short, a sum tag past its variants, a map with a key twice, or
a list whose elements are not its element type never reaches the code that
would read it. A value that does not conform is `GEAS_ERR_TYPE` and no body
runs, nothing latches, and nothing of it is kept. A body's answer that does not
conform fails that attempt with `GEAS_ERR_TYPE`, retried like any failed
attempt and never latched, and a course whose call meets it halts with that
status. A value nests at most 256 levels at any boundary, and anything that
conforms can always be parked.

The same check runs on a park row. A resume, a claim, and an amend hold the
vows, the Ok values, the Err payloads, and the signal payloads a row brings
back to the contract's declared types the same way, so a row edited in the
store is `GEAS_ERR_TYPE` with no instance built, and a claim that refuses one
leaves the row where it was. The ABI lays out every rule, see
[docs/abi.md](docs/abi.md).

### Ownership

The runtime owns every heap allocation that crosses the line. Arguments are deep
copied onto the instance on entry, on the caller's thread, so a host buffer
rewritten after the call changes nothing. Results are owned by the instance and
die at its break. A host never frees what Geas returns, and Geas never
holds a pointer into host memory after a call returns.

### The pledge frame

Every pledge crosses the boundary in one uniform frame, compiled bodies and host
bound implementations alike:

```c
GeasStatus (*GeasPledgeFn)(void* ctx, const GeasValue* args, size_t nargs,
                         GeasValue* out);
```

`ctx` is the signed instance the fulfillment runs against, and its allocation
helpers make everything the pledge builds instance owned. The runtime marshals
the frame and dispatches, and the bridge carries the same frame across processes.

### Terms, signals, and remedies at the boundary

The workflow surface rides the same descriptors as one more tail, so a module
compiled before it loads unchanged and a contract that declares none of it
behaves exactly as it did. A host replaces the clock with
`geas_runtime_set_clock` and drives lapses with `geas_runtime_tick` or
`geas_instance_tick`, delivers a signal with `geas_signal` and reads its payload
with `geas_signal_payload`, binds an abstract remedy with the same
`geas_pledge_bind` a pledge uses, and reads lapses and remedy outcomes off the
partial surface with `geas_partial_lapsed`, `geas_contract_term_lapsed`,
`geas_remedy_state`, and the `geas_partial_remedy_*` calls. The contract state
gains `GEAS_REMEDYING` at the end of its enum, so every earlier number stands.

### Futures

Fulfillment is a future. `geas_pledge_fulfill` returns a handle and
`geas_future_wait` collects the outcome exactly once; a synchronous form fuses the
two. The ABI never promises where the body runs, on the caller's thread, on a
worker pool, or across a socket, so a host that waits on a future is unaffected
when the origin of the answer changes.

### Threading

Fulfillments run on a worker pool sized by the runtime configuration. Each
instance is serialized behind one recursive mutex covering the thunk run, the
latch, and every arena allocation, so fulfillments against one instance serialize
while distinct instances run in parallel. The lock order is instance, then
runtime, then future. A break racing an in flight fulfillment resolves
deterministically: the pledge delivered before the break wins, or the break
forfeits the future to a state error, and nothing touches freed memory.

### By reference and write back

A host can pass an argument by reference and receive data back through its own
pointer. The runtime copies the value in at fulfill, the body mutates its
instance owned slot, and the write back lands in host memory only at delivery,
while the host is still blocked in the call, through a default for scalars and
strings or a caller supplied callback.

### Binding an abstract pledge

A host implements an abstract pledge by binding a function of the uniform frame
over its name. The binding is resolved into a fixed dispatch table at sign, beats
a compiled body, and satisfies the abstract pledge so the contract can sign.

### Two languages, one contract

The repository proves the claim with C and with Python. `skeleton/host.c` links
`libgeasrt` and speaks the ABI header. `interop/python/geas/` is a ctypes
binding written against the ABI reference alone, no header parsed and no stubs
generated. Both hosts sign the same contracts, override the same vows, bind the
same abstract pledges, and read the same partial surface, and both walk the
order saga on a clock of their own, `tests/runtime/test_saga.c` from C and
`interop/python/demo_saga.py` from Python. The contract is the shared
artifact, and every language meets at it.

---

## The Store Layer

The store layer backs a contract with a database. The surface below is the
language's view of it, and [docs/database.md](docs/database.md) is the normative
design.

A store backed contract declares the shape of a table as a schema, beside its
vows:

```text
contract Ledger {
    vow dsn: String = "file:ledger.db"

    schema Accounts {
        id: Int
        balance: Float
        owner: String
    }

    subcontract Transfer transactional {
        pledge debit(id: Int, amount: Float) -> Result<Unit, LedgerError>
        pledge credit(id: Int, amount: Float) -> Result<Unit, LedgerError>
    }
}
```

### Schema as vows

A `schema` names a table and its columns, each a scalar type, and it is vow
shaped: immutable, locked at sign, and validated at sign. When the contract
signs, the runtime reconciles every schema with the live database on the bound
`dsn`: an absent table is created to match, a present table is validated column
for column, and a divergent table fails the sign. The schema joins the contract's
shape hash, so a changed column fails a stale host's sign the way a changed
pledge does. A schema also stands up a record, so a row a pledge reads is an
ordinary record. Column types are the seven scalars, `Int`, `UInt`, `Float`,
`Bool`, `Byte`, `Char`, and `String`; a composite column is a type error.

### The store surface

A pledge body reads and writes through the standard library store module,
naming a schema in its own contract:

```text
pledge debit(id: Int, amount: Float) -> Result<Unit, LedgerError> {
    match Store.find(Accounts, id) {
        Err(_) -> { return Err(NoSuchAccount(id)) }
        Ok(None) -> { return Err(NoSuchAccount(id)) }
        Ok(Some(row)) -> {
            Store.update(Accounts, id, Accounts { balance: row.balance - amount })
            return Ok(Unit)
        }
    }
}
```

`Store.find`, `Store.insert`, `Store.update`, and `Store.delete` name a schema
and bind every value positionally, never by concatenation, so a value carrying
SQL is a value and nothing else. The first column of a schema is its primary key.
A backend failure that the runtime cannot complete, a lost connection, a full
disk, a refused constraint, is the status `GEAS_ERR_STORE` delivered through the
fulfillment; a contract's own rules stay values in its own error type.
`Store.query`, `Store.count`, `Store.sum`, `Store.min`, `Store.max`, and
`Store.avg` read sets through one predicate grammar, listed in
[docs/database.md](docs/database.md).

### Transactions as subcontracts

A `transactional` subcontract runs its pledges as one all or nothing episode. The
transaction opens on the first fulfillment of a pledge in the subcontract,
buffers its writes, and commits the moment the subcontract completes, its last
pledge latching `Ok`, or rolls back the instant any pledge in it returns `Err` or
the contract breaks first. Because a transaction is one episode and not a latch, a
pledge in a transactional subcontract is fulfilled at most once, and a repeat call
to a resolved one is a state error. Loose pledges outside a transactional
subcontract run in autocommit, each store operation its own implicit transaction.
The `transactional` modifier is allowed only on a subcontract in a store backed
contract.

### The lifecycle over storage

`sign` opens the connection and reconciles the schema; `break` rolls back any
open transaction, closes the connection, and reclaims the heap, in that order, so
no uncommitted write survives a break and no connection outlives its instance.
An automatic break, whether its break line matched, its term ran out, or a
deadline lapsed, rolls back every open episode before remedies begin, releasing
the database locks while keeping the heap, errors, and partial surface readable.

A transactional subcontract holds no signal and no pledge with a `retry`, since
a retried attempt would run inside the same open episode as the attempt it
replaced. And when a contract breaks, a pledge whose transactional episode never
committed has no remedy due, because the rollback already undid it.

### Parking

`instance.park(dsn, key)` writes the instance's durable state into one row of
the runtime's own table behind `dsn`, and `Contract.resume(dsn, key)` stands it
back up, in this process or another. The row carries the vows, the latches and
their error payloads, the transactional fates, and a versioned workflow column
holding the eligibility stamps, the lapses, the `Ok` values the remedies will
need, the signal payloads, the retry attempts and due times, and the remedy
progress, so a deadline keeps counting
across the park and a break after a resume runs the same remedies in the same
order it would have before. An automatically Broken instance can park after
its open episodes have rolled back and resumes Broken with its errors. An
explicit `break()` reclaims the heap, so that instance can no longer park.

A retry wait and a fulfillment driving a remedy pass can park with their
futures still waiting. Park is a snapshot: the original operations continue
if the host carries on, and their waits return their ordinary status and
value. In particular, a failing pledge that triggered remedies still returns
its `Err` with `GEAS_OK` after the original pass finishes. A shutdown joins
workers, so the host wakes its own sleep callbacks. A running pledge body,
a queued fulfillment, and a completed but unwaited receipt still refuse a
park. Workflow blob version 3 adds the retry fields; version 2 and rows
without a workflow blob retain their earlier resume rules.

Steps are at least once. A host that dies in the middle of a step resumes from
its last park with that pledge still pending and runs it again, so a step body
should be safe to run twice or check its own progress before repeating work.
Parking records what has latched, it does not make the body's outside effects
and the park row one transaction. Cancel waits for a live body to return, but a
host that dies cannot wait for it.

---

## The Bridge

Across processes the same contracts speak gRPC, and Geas keeps no wire of
its own. `geas emit-proto` writes the wire surface, one typed rpc per pledge
and a signing stream whose lifetime is the instance's, plus the session
wrappers protoc cannot write; a server is ordinary host code holding the
runtime over the C ABI, and a client in any gRPC language builds from the
emitted artifacts with stock tooling. A pledge's `Err` crosses as a value on
an OK rpc, a Geas status crosses as a gRPC code, the shape hash pins both
ends to one contract, and a park store lets a session survive its stream, its
client, and its server. Every signal is its own typed `Signal` rpc on the
emitted surface, with a helper in each session wrapper, and serving those rpcs
from the reference bridge server is next on the roadmap. The bridge adds no language surface; it is the same
contracts, one emitted file wider. The canonical little endian value encoding
remains the park row's format, so a parked value round trips exactly.
[docs/bridge.md](docs/bridge.md) is the normative surface.
