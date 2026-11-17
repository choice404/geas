# A Geas Tutorial

Geas is a language for durable obligations. You write a contract once, its
steps, its deadlines, its remedies, and the policy that decides when it is
kept, and any language that can call a C library can sign it, fulfill its
pledges, deliver events into it, and read where it stands, with the runtime
holding every term and every allocation that crosses the line. This tutorial
walks the whole language surface, drives one contract from C and from Python
with no generated bindings on either side, and then walks an order saga
through its deadlines and its remedies, first host driven and then engine
driven, the contract running its own course.

The bridge, contracts served over gRPC to other processes, is a real part of
Geas and is out of scope here on purpose. Everything below is the in
process story: the language, and two languages meeting through it in one
address space. The bridge is [docs/bridge.md](bridge.md) when you want it.

Every example in this tutorial is a real file under `skeleton/` or `lib/`, and
every command is a real `make` target. Nothing here is pseudocode.

## Before you start

You need a C++23 compiler, gcc or clang, and CMake to build the compiler, and
`cc` to compile the emitted C and the runtime. Build everything and run the
walking skeleton:

```sh
make smoke
```

That compiles the runtime into `libgeasrt.so`, builds `geas` with CMake from
`compiler/` into `target/bin/geas`, compiles a contract module, and runs a C
host that signs it and fulfills a pledge end to end. `make compiler` builds the
compiler on its own, and `make compiler SANITIZE=ON` builds it under the
address and undefined behavior sanitizers into `target/bin-asan/geas`.

The compiler is `geas`, source files end in `.geas`, and a compiled module ends
in `.geas.so`. Everything lands under `target/`. `geas check file.geas` runs the
whole front end without building anything, which is the fastest way to try an
example out.

## 1. Your first contract

A contract is the unit of Geas the way a function is the unit of most
languages. Here is the whole of `skeleton/hello.geas`:

```text
GreetError is either Empty

contract Greeter {
    vow prefix: String = "hello, "

    pledge greet(name: String) -> Result<String, GreetError> {
        return Ok(prefix + name)
    }

    pledge shout(name: String) -> Result<String, GreetError>
}
```

Three ideas are already here.

A **vow** is an immutable field. `prefix` is locked the moment the contract is
signed and never changes for the life of that signature. A vow may carry a
default, and a host may override it at sign, but nothing inside the contract can
assign to it.

A **pledge** is a callable commitment with a declared return type. `greet` has a
body and returns a `Result`. Every fallible pledge returns a `Result` or an
`Option`, never a bare value and never an exception, because errors in Geas
are values that cross the boundary like any other.

`shout` is an **abstract pledge**: a commitment with no body. The contract
cannot be signed until a host binds an implementation for it, which is how a
foreign language plugs its own behavior into a Geas contract. We come back
to it in the interop section.

`String + String` concatenates, and `Ok(...)` wraps a success. `GreetError is
either Empty` declares a sum type with a single variant, the error slot of the
`Result`.

## 2. The lifecycle

A contract moves through six states, and the whole runtime is built around this
progression:

```text
Unsigned -> Signed -> Fulfilled
                   -> Partial
                   -> Remedying -> Broken
                   -> Broken                (no remedies due)
```

`Remedying` belongs to the saga half of the language and gets its own section
at the end; a contract with no remedies goes straight from its break to
`Broken`.

Signing validates the contract's shape, locks its vows, and activates it.
Fulfilling a pledge runs it and records the outcome. A pledge latches on its
first `Ok`: once fulfilled it stays fulfilled, and an `Err` before any `Ok`
latches it broken with the error kept readable. Breaking a contract tears it
down and reclaims every allocation the runtime made for it. A contract that got
partway reports exactly which pledges landed, which are pending, and which
broke, with the errors attached.

You never call these transitions from inside `.geas` code for your own contract;
a host drives them across the boundary. That is the next half of the tutorial.
First, the rest of the language.

## 3. Data: records and sums

Geas has two ways to shape your own data.

A **record** is a product, a fixed set of named fields:

```text
DemoError is either BadMood(reason: String) or Empty
```

A **sum**, written `is either`, is a choice between variants, each of which may
carry a payload. `DemoError` above is a sum with two variants, one carrying a
`String` and one carrying nothing. You build a variant by naming it,
`BadMood("asked to fail")` or `Empty`, and you take it apart with `match`.

Two sums are built in and everywhere. `Option<T>` is `Some(T)` or `None`.
`Result<T, E>` is `Ok(T)` or `Err(E)`. They are how a pledge says "a value or
nothing" and "a value or a failure", and the type checker holds you to handling
both arms.

## 4. Inside a pledge body

A pledge body is ordinary imperative code. Here is `Main.run` from
`skeleton/main_demo.geas`, which counts its arguments:

```text
pledge run(args: List<String>) -> Result<Int, DemoError> {
    let mut n = 0
    for a in args {
        if a == "fail" {
            return Err(BadMood("asked to fail"))
        }
        n = n + 1
    }
    return Ok(n)
}
```

The pieces:

- `let` binds an immutable name, `let mut` a reassignable one. `n` is `mut`
  because the loop grows it.
- `if` and `else` are the conditional. A record literal is banned in an `if`
  head so the brace is never ambiguous, a rule worth knowing before it surprises
  you.
- `for x in list` walks a list, `while cond { }` loops on a condition, and
  `break` and `continue` do what you expect.
- `return` leaves the pledge with a value of its declared type.

The two operators that make Geas feel like Geas are `match` and `?`.

**`match`** is an exhaustive expression. It must cover every variant, and the
checker rejects it if it does not:

```text
match r {
    Ok(v) -> {
        let gate = IntGate.sign()
        let d = gate.double_pos(v)?
        gate.break()
        return Ok(d)
    }
    Err(_) -> {
        return Err(-1)
    }
}
```

Each arm binds the payload it matches, `v` for the `Ok`, `_` to ignore the
`Err`. A `match` is an expression, so it can produce a value directly, and a
block is a value too: the last expression in a `{ }` is what it evaluates to.

**`?`** is propagation. `gate.double_pos(v)?` runs the pledge, and if it returns
`Err`, the whole enclosing pledge returns that `Err` immediately; on `Ok` it
unwraps to the inner value. The error types have to agree: `?` only works when
the callee's error type is the enclosing pledge's error type, which is why the
example around it hand translates errors when they do not match.

One safety rule to know: the logical operators short circuit, so a bounds check
like `i < n && xs[i] == 0` never evaluates `xs[i]` when `i < n` is false, and an
out of range index returns a type error rather than faulting.

## 5. Clauses and traits

A **clause** is an internal method. It never leaves its contract, has no place
in the lifecycle, and exists so pledges can share logic. From
`skeleton/std_user.geas`:

```text
clause compare(a: Int, b: Int) -> Int {
    if a < b { return -1 }
    if a > b { return 1 }
    return 0
}
```

A **provisional clause** is Geas's trait: a named set of clause signatures a
contract can promise to satisfy. A contract `incorporate`s one and implements
its clauses:

```text
contract StdUser {
    incorporate Comparable
    incorporate Loggable

    clause compare(a: Int, b: Int) -> Int { ... }
    clause log_line(msg: String) -> String { ... }
}
```

Now `StdUser` is `Comparable`, and a pledge can sort through its own comparator:

```text
pledge sort3(a: Int, b: Int, c: Int) -> Result<List<Int>, Int> {
    let mut xs = [a, b, c]
    if compare(xs[0], xs[1]) > 0 {
        let t = xs[0]
        xs[0] = xs[1]
        xs[1] = t
    }
    // two more passes
    return Ok(xs)
}
```

`xs[0] = xs[1]` is an index assignment, and it writes in place. Composites carry
value semantics everywhere else: a `let` of a list, an assignment, and a
composite read all copy, so mutating one binding is never visible through
another. The one place a write lands in place is an assignment target like
`xs[0]`.

## 6. Subcontracts and the fulfillment policy

A **subcontract** groups pledges, and the **requirements** block writes the
contract's success and failure as boolean logic over those groups. This is the
heart of the language, and `skeleton/payment.geas` shows it whole:

```text
contract PaymentService {
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

Read the policy out loud. The contract is **fulfilled** when Validation and
Processing and notify_user all land. It is **partially fulfilled** when either
Validation or Processing lands. It **breaks** when none of the three has landed
and something has failed. A subcontract counts as satisfied when its own pledges
have latched.

The runtime evaluates this policy after every outcome, in the priority break,
then fulfill, then partial, and moves the contract's state automatically. When
the break line holds, the contract breaks itself and keeps its heap alive so the
error payloads stay readable until an explicit break reclaims them. The checker
proves at compile time that no state satisfies both fulfill and break at once,
so a policy that contradicts itself never ships.

The payoff is the partial surface: at any moment a host can ask which
subcontracts and pledges are fulfilled, which are pending, and which broke, and
read the latched errors by name. We watch that happen from Python below.

## 7. Calling a contract from a contract

A pledge body can sign another contract, fulfill it, and break it, which is how
Geas composes. From `StdUser.compute`:

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

`MathOps.sign(epsilon: 0.5)` signs another contract with a vow override,
`ops.abs(x)` fulfills a pledge on it through method syntax, and `ops.break()`
tears it down. Notice the instance breaks before `r` is read: that is safe
because a cross contract call copies the result home before the callee dies. A
signed instance is the one value in the language with reference semantics,
copies of the handle alias the same instance and equality is identity, and it is
also the one value that can never cross the C boundary, only its results can.

## 8. A standalone program

Everything so far is a library other languages drive. Geas is also a language
you can compile straight to an executable. Declare one `Main` contract with a
`run` pledge over `List<String>` returning `Result<Int, E>`:

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

`build --bin` links it into a real program. `Ok(n)` becomes the exit code, and
an `Err` renders itself to stderr and exits 1:

```sh
make test-bin
./target/geas-out/main_demo a b c ; echo $?   # counts its args, exits 3
./target/geas-out/main_demo fail              # takes the Err path, exits 1
```

## 9. The standard library

A first standard library lives under `lib/std`: `math` with overflow checked
pledges, `strings` and `collections` shaped to what the language can express,
common `errors`, and the `traits` that hold the provisional clauses like
`Comparable` and `Loggable`. You pull a module in with `import`:

```text
import std.math
import std.collections
import std.traits
import std.errors
```

Imports resolve beside your root file and then under `GEAS_HOME`, every file
parses into one shared tree, and cycles and duplicate imports are named errors.
A module lends another its record and sum shapes, its error sums for the `E`
slot, its provisional clauses, and its contracts themselves. `make test-std`
builds a module that imports four std modules and drives them from C.

That is the language. Now the reason it exists.

## 10. The boundary

Here is how any language gets to be a party to a contract.

A compiled `.geas.so` module carries **descriptors**, tables that spell every
contract, every pledge and its signature, and every vow and its default. When
the runtime loads the module, it registers all of that in the **iname table**, a
sorted registry keyed by a **mangled name** that bakes in the contract, the
pledge, a 64 bit hash of the type signature, and a version. A host looks a
contract up by that name, and because the name carries the shape hash, a version
mismatch is a clean error at discovery time instead of silent corruption at call
time.

Every value that crosses the boundary is an `GeasValue`, a small tagged union
whose layout is fixed and documented in [docs/abi.md](abi.md). Integers, floats,
strings, lists, tuples, records, sums, options, and results all have a pinned
representation. The one iron rule is ownership: the runtime owns every heap
allocation that crosses the line. Arguments are copied onto the instance on the
way in, results are owned by the instance and die when it breaks, and a value
passed by reference is copied in on entry and written back on return, never held
by Geas past the call. A host never has to free what Geas gave it, and
Geas never holds a pointer into host memory after a call returns.

A host drives a contract through a handful of runtime calls: initialize the
runtime, load a module, sign a contract, fulfill its pledges, read the partial
surface, break it. Those calls are the same in every language, because they are
just C. Let us make two languages speak them.

## 11. Interop with C

`skeleton/host.c` is a foreign program. It knows nothing about `geas`; it links
`libgeasrt` and speaks the ABI header `geas/geas.h`. The shape of a host is always
the same five moves.

**Initialize and load.**

```c
GeasRuntime* rt = NULL;
geas_runtime_init(NULL, &rt);
geas_module_load(rt, "target/geas-out/libhello.geas.so");
```

**Sign.** A host builds an argument as an `GeasValue` and hands the runtime a
value it owns; the runtime never keeps it:

```c
GeasContract* c = NULL;
geas_contract_sign(rt, "Greeter", NULL, 0, 0, &c);
```

Pass a vow override to change a locked field at sign, exactly what
`MathOps.sign(epsilon: 0.5)` did from inside the language:

```c
GeasVowBinding prefix;
prefix.name = "prefix";
prefix.value = str_arg("hey, ");
geas_contract_sign(rt, "Greeter", &prefix, 1, 0, &c3);
```

**Fulfill.** A pledge runs through a future: `geas_pledge_fulfill` returns a
handle and `geas_future_wait` collects the outcome exactly once. The synchronous
form fuses the two:

```c
GeasValue out;
GeasValue name = str_arg("world");
geas_pledge_fulfill_sync(c, "greet", &name, 1, NULL, 0, &out);
// out is Ok("hello, world")
```

The future is not decoration. A wait blocks until the outcome exists, and the
runtime never promised where the body runs, on the caller's thread, on a worker
pool, or, in the network layer, across a socket. Writing to the future is how
the host stays the same when the answer's origin changes.

**Bind an abstract pledge.** `Greeter.shout` had no body. The host supplies one,
written in the same uniform thunk frame every compiled pledge uses, and only
then can the contract sign:

```c
static GeasStatus host_shout(void* ctx, const GeasValue* args, size_t nargs,
                            GeasValue* out) {
    GeasContract* c = (GeasContract*)ctx;
    // uppercase args[0], write the result into *out as Ok(...)
    return GEAS_OK;
}

geas_pledge_bind(rt, "Greeter.shout", host_shout);
```

**By reference and write back.** A pledge can take an argument by reference and
hand data back through the host's own pointer. The host owns the storage, the
runtime copies the value in at fulfill, the body mutates its instance owned slot,
and the write back lands in host memory only at delivery, while the host is
still blocked in the call:

```c
GeasString by_ref;
by_ref.ptr = (uint8_t*)"whisper";
by_ref.len = 7;
GeasRef ref;
memset(&ref, 0, sizeof(ref));
ref.host_ptr = &by_ref;
ref.ty = GEAS_TY_STRING;
geas_pledge_fulfill_sync(c, "shout", NULL, 0, &ref, 1, &out);
// by_ref now holds "WHISPER", written back into host storage
```

**Break.** Tearing the contract down reclaims everything, and every later
fulfillment on it is a clean state error:

```c
geas_contract_break(c);
```

Run the whole C walk, sign, bind, fulfill, by reference write back, futures,
discovery, and break, under the address and leak sanitizers:

```sh
make smoke-asan
```

**Resolving names without hardcoding them.** A host can spell mangled names by
hand, but `geas` will generate the header for you:

```sh
target/bin/geas emit-header skeleton/hello.geas
```

That writes the shape hash and every mangled pledge name as C defines, read from
the same tables the module emitter hashes, so a host resolves and signs against
generated names instead of string literals.

## 12. Interop with Python

The claim Geas makes is that any language which can load a C library can
speak a contract, with no code generated for the binding. The repository proves
it with Python and nothing but `ctypes`. `interop/python/geas.py` is written
against [docs/abi.md](abi.md) alone, no header parsed and no stubs generated, and
`interop/python/demo_payment.py` drives the payment contract with it.

The same five moves, now in Python:

```python
from geas import Runtime, Ok, Err

with Runtime(OUT / "libgeasrt.so") as rt:
    rt.load(OUT / "libpayment.geas.so")
    rt.bind("PaymentService.charge", charge)   # a Python function is the pledge
    rt.freeze()

    c1 = rt.sign("PaymentService", vows={"currency": "EUR"})
    assert c1.vow("currency") == "EUR"
```

The abstract `charge` pledge is implemented by an ordinary Python function,
called back from the runtime when a pledge fulfills:

```python
def charge(inst, args):
    card, amount = args
    if amount > 0:
        return Ok(True)
    return Err(41)
```

Now walk the fulfillment policy from Python and watch the partial surface move.
Validation lands, and the contract goes partial:

```python
c1.fulfill_sync("validate_card", "4111 1111")
c1.fulfill("validate_amount", 25.0).wait()

p = c1.partial()
# p.fulfilled == ["Validation"], p.pending == ["Processing", "notify_user"]
```

Drive `charge` and `notify_user`, and the policy declares the contract
fulfilled. Or send a bad amount, and the break line fires by itself:

```python
c2 = rt.sign("PaymentService")
out = c2.fulfill_sync("charge", "4111 1111", -2.0)
# out == Err(41), and c2 broke automatically
p = c2.partial()
# p.broken == ["Processing"], p.errors == [("charge", 41)]
```

By reference works from Python too, the write back landing back in a host owned
object:

```python
g = rt.sign("Greeter")
ref = StringRef("whisper")
out = g.fulfill_sync("shout", refs=[ref])
# out == Ok("WHISPER"), and ref.value == "WHISPER"
```

Run it:

```sh
make test-python
```

Read the two demos side by side and the point lands. The C host and the Python
host drive the same contracts, sign the same vows, bind the same abstract
pledges, and read the same partial surface. Neither generated a line of binding
code. The contract is the shared artifact, and every language meets at it.

## 13. A saga, host driven

So far every contract has been quick: sign, fulfill, read, break. Now for the
kind of contract Geas is really for, one that takes days, waits on the outside
world, and has to clean up after itself when it fails. `skeleton/saga.geas` is
an online order: authorize the card, capture the charge, reserve the stock,
ship the box, and wait for the delivery scan.

Read the whole file once, it is short. The part worth slowing down on is the
contract header and the steps:

```text
contract OrderSaga [version: 1, term: 7d] {
    vow amount: Int = 100
    vow grace: Duration = 30m

    subcontract Payment {
        pledge authorize(card: Card) -> Result<AuthId, PayError>
            within 30s
            retry 3 every 2s backoff 2.0 cap 30s on Timeout, Gateway
        pledge capture(auth: AuthId) -> Result<Receipt, PayError>
            after authorize
            within 1m
    }

    pledge ship(res: Reservation) -> Result<Tracking, ShipError>
        after Payment, reserve

    signal delivered(tracking: Tracking) after ship within 3d

    remedy refund for capture(receipt: Receipt) -> Result<Unit, PayError>
        retry 2 every 1m
}
```

Those trailing lines are **terms**. `within 30s` is a deadline, counted from
the moment the step became eligible. `after authorize` is ordering, `capture`
cannot run until `authorize` latched fulfilled. `retry 3 every 2s backoff 2.0
cap 30s on Timeout, Gateway` is the retry policy: up to three more attempts,
waiting 2s, then 4s, then 8s, never more than 30s, and only for a `Timeout` or a
`Gateway` error, because a declined card is an answer and not an accident.
`[term: 7d]` is the contract's whole lifetime. `30m` and `7d` are duration
literals, type `Duration`, which cross the boundary as plain milliseconds.

`delivered` is a **signal**, an outside event with no body. The carrier's
webhook delivers it, and it has three days from the moment `ship` landed.

`refund` is a **remedy**, the step that undoes `capture`. If the saga breaks,
by an `Err` its policy cannot live with, by a deadline passing, or by the week
running out, every step that landed gets its remedy run, newest first, each
handed the `Ok` value its step produced. `reserve` and its remedy `release` are
written in Geas right in the file, so you can see a remedy body check the value
it was handed before it lets the stock go.

This contract has no `course`, so it is **host driven**: the host calls the
steps in whatever order it likes, and the runtime enforces the terms. Calling
`ship` before `Payment` and `reserve` landed is a state error and the body never
runs; that is the whole point, the order lives in the contract and not in the
host's discipline.

Now a step that just launches a program can use `std.process` directly. After
`import std.process`, put this pledge in the contract, and the host calls it
like every other step:

```text
pledge copy(target: String) -> Result<Unit, ExitError> {
    return Process.run(Command {
        program: "/bin/cp", args: [target, target + ".copy"],
        stdout_path: "", stderr_path: ""
    })
}
```

The path runs with those exact arguments, an argument containing spaces stays
one argument, and the two empty paths inherit stdout and stderr. Exit zero is
`Ok`, other outcomes are `Exited`, `Signaled`, or `SpawnFailed`. The same call
works inside a clause or a remedy, so an undo program can live beside its step.
Breaking the instance requests SIGTERM and then SIGKILL after 200 ms if needed,
and the runtime waits for the child before it reclaims the instance.

### From C

For a dusk host, `interop/dusk` carries the same runtime surface through a thin
C shim, and its demo binds dusk pledge and remedy handlers to the order saga.

`tests/runtime/test_saga.c` drives every road the saga can take. The host binds
the four abstract steps, `authorize`, `capture`, `ship`, and `refund`, the same
way `Greeter.shout` was bound, and points the runtime at a fake clock so a
three day deadline is a number the test moves:

```c
static int64_t fake_now(void* user) {
    (void)user;
    return (int64_t)atomic_load(&g_now_ms);
}

GeasRuntimeConfig cfg = { .max_threads = 2 };
geas_runtime_init(&cfg, &rt);
geas_runtime_set_clock(rt, fake_now, fake_sleep, NULL);
geas_module_load(rt, "target/geas-out/libsaga.geas.so");
geas_pledge_bind(rt, "OrderSaga.authorize", fn_authorize);
geas_pledge_bind(rt, "OrderSaga.capture", fn_capture);
geas_pledge_bind(rt, "OrderSaga.ship", fn_ship);
geas_pledge_bind(rt, "OrderSaga.refund", fn_refund);
geas_runtime_freeze(rt);
```

When several bindings share one host body but carry different host state,
`geas_pledge_bind_user` binds each one with the user pointer that body reads.

A remedy binds with the same call a pledge does. The fake sleep moves the clock
by whatever it is asked to wait and logs it, so a retry costs no real time and
the test can read the delays back: two timeouts from `authorize` show up as
`2000` then `4000`.

A signal is delivered with `geas_signal`, no body runs, the latch lands, and
the policy moves the contract:

```c
GeasValue fields[2] = { str_val("ACME"), str_val("T-1") };
GeasValue tracking = record_of(fields, 2);
geas_signal(c, "delivered", &tracking, 1);
// geas_contract_state(c) == GEAS_FULFILLED
```

Now the break. The host's carrier refuses, `ship` answers `Err(NoCarrier)`,
the break line `!ship` holds, and the remedies run before the fulfillment
returns: `release` first, since `reserve` landed last, then `refund`, handed
the receipt `capture` produced. Inside the refund body the saga reads
`GEAS_REMEDYING`, and once the pass ends it reads `GEAS_BROKEN`:

```c
geas_contract_state(c) == GEAS_BROKEN;
geas_remedy_state(c, "release") == GEAS_REMEDY_DONE;
geas_remedy_state(c, "refund") == GEAS_REMEDY_DONE;
geas_partial_remedy_count(c) == 0;   // no remedy failed
```

A deadline needs no call at all to pass, but the runtime only notices on the
next call that touches the instance, or on a tick. Move the fake clock three
days past `ship` and tick, and `delivered` lapses, which breaks the saga and
runs the remedies the same way:

```c
clock_advance(3 * DAY_MS);
geas_instance_tick(c);
geas_partial_lapsed(c, "delivered") == 1;   // a lapse, not an Err
```

A real host calls `geas_runtime_tick(rt)` from whatever timer it already has
when it wants a lapse the moment it falls due. And since the saga's whole point
is surviving days, the gate also parks it mid flight with
`geas_instance_park(c, dsn, "saga-1")`, shuts the runtime down, stands a fresh
one up an hour later, resumes, and finishes the saga there; the eligibility
stamps, the `Ok` values the remedies will need, and every latch cross with it.

```sh
make test-saga
```

### From Python

`interop/python/demo_saga.py` walks the same saga with the same binding as
before. The clock is two Python callables, and the steps are Python methods:

```python
rt = Runtime(OUT / "libgeasrt.so")
rt.set_clock(clock.now, clock.sleep)
rt.load(OUT / "libsaga.geas.so")
rt.bind("OrderSaga.authorize", host.authorize)
rt.bind("OrderSaga.capture", host.capture)
rt.bind("OrderSaga.ship", host.ship)
rt.bind("OrderSaga.refund", host.refund)
rt.freeze()

saga = rt.sign("OrderSaga")
```

The happy path answers one `Timeout` from `authorize`, which the retry term
absorbs, `clock.sleeps == [2000]`, then reserves through the compiled Geas
body, ships, and delivers:

```python
shipped = saga.fulfill_sync("ship", Record(*res))
saga.signal("delivered", Record(*shipped.value))
# saga.state() == GEAS_FULFILLED
```

An early delivery raises a `GeasError` with `GEAS_ERR_STATE`, because
`delivered` waits on `ship`. The break path has the carrier refuse and reads
the remedies back from Python:

```python
shipped = saga.fulfill_sync("ship", Record(*res))
# shipped is an Err carrying NoCarrier, and saga.state() == GEAS_BROKEN
saga.remedy_state("release") == GEAS_REMEDY_DONE
saga.remedy_failures() == []
saga.lapsed("ship") == False   # ship broke by its Err, not by a deadline
```

```sh
make test-saga-python
```

Two hosts, two clocks, one saga, and the rules never moved out of the
contract.

## 14. A saga, engine driven

So the saga above works, however look at what the host had to do, it called
`authorize`, then `capture`, then `reserve`, then `ship`, in the right order,
and the contract only checked that it did. The order lived in the contract as
rules and in the host as code, twice. Now the engine driven mode puts it in one
place: the contract carries a `course` block, the orchestration written out,
and the runtime runs it from the moment the contract signs.
`skeleton/order_course.geas` is the same order with its course:

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

Read it like any function, because that is what it is, with two differences.
Every call of one of the contract's own pledges is a **step**: it runs through
the same terms a host call gets, the `within`, the `after`, the retries, the
latch, and once it returns the course remembers where it is, so a saga parked
and resumed in another process picks up at the step it stopped on with every
binding it had made. And a few statements **suspend**: `await` waits for a
signal and hands back its payload, `select` takes whichever arm is ready first,
an await or a timeout, `sleep` pauses, and `together` runs its calls at the
same time and finishes when every one has. While the course waits, no thread
waits with it, the course is just a step number and some values on the
instance.

`plan` is a Geas body, the hold plus the grace vow, and its `?` ends the course
if the warehouse would not hold that long. `capture` and `reserve` really run
at once, each on its own pool worker. The `select` gives the parcel two days,
and if it is quiet the course calls `chase` with how long it has been out and
keeps waiting; the `delivered` signal's own three days still decide whether it
ever lands.

Now the one thing that trips people up. The course ending is not the contract's
state. When `ship` answers `Err(NoCarrier)`, the `?` ends the course, it reads
finished, and it is the requirements that read the latch, see `!ship`, break
the contract, and run the remedies, newest first, the same way the host driven
saga did. A course is the order of the steps, the requirements are still the
judge.

### From C

The host binds the bodies the contract leaves abstract, signs, and that is it,
there is no step to call:

```c
geas_pledge_bind(rt, "OrderCourse.authorize", fn_authorize);
geas_pledge_bind(rt, "OrderCourse.capture", fn_capture);
geas_pledge_bind(rt, "OrderCourse.reserve", fn_reserve);
geas_pledge_bind(rt, "OrderCourse.ship", fn_ship);
geas_pledge_bind(rt, "OrderCourse.chase", fn_chase);
geas_pledge_bind(rt, "OrderCourse.refund", fn_refund);
geas_runtime_freeze(rt);
geas_runtime_start_ticker(rt, 5);

GeasContract* c = NULL;
geas_contract_sign(rt, "OrderCourse", NULL, 0, 0, &c);
```

The ticker is a runtime thread that ticks every 5 milliseconds, which is what
lets the `select`'s two days and the signal's three fall due with nobody
calling in; a host with its own timer calls `geas_runtime_tick` from it
instead, and that is what the gate does, over a fake clock it moves by hand.
To see where the course is, read it, or wait for it to stop running:

```c
GeasCourseInfo info = { .size = sizeof(info) };
geas_course_wait(c, 5000, &info);
// info.state == GEAS_COURSE_WAITING, info.step == 5, the select
// info.wait_due_ms is the moment the 2d timeout falls due
```

The course passes through WAITING at the `together` on its way, while
`capture` and `reserve` run, so a host that wants the `select` in particular
waits again until the step reads 5; `geas_course_info` reads the same thing
without blocking. A wait that runs out is no error either, it answers
`GEAS_OK` with `info.state` still `GEAS_COURSE_RUNNING` and `info.step` the
step the course is on right now, since every step boundary publishes the step,
so a host polling with a short timeout reads the state to tell a slow course
from one at rest. The one wait that is refused is a wait from inside a body or
an event hook, that answers `GEAS_ERR_STATE` at once, the course could be
waiting on that very caller.

Now the thing is the hook you install is called from whichever thread recorded
the events, the ticker included, and while it runs the instance is reserved to
that thread. The course does not care, a course that runs into somebody else's
delivery, coming out of a retry delay or at its next step, waits for it to end
and carries on, it never halts over a hook it had nothing to do with.

A host fulfillment of a course's pledge answers `GEAS_ERR_STATE` and the body
never runs, the course owns the order now. The host still hands signals in:

```c
GeasValue fields[2] = { str_val("ACME"), str_val("T-1") };
GeasValue tracking = record_of(fields, 2);
geas_signal(c, "delivered", &tracking, 1);
geas_course_wait(c, 5000, &info);
// info.state == GEAS_COURSE_FINISHED, geas_contract_state(c) == GEAS_FULFILLED
```

For a saga that has to live through a restart, arm it once with a store and a
key, and the runtime writes its row at every step from then on:

```c
geas_contract_set_autopark(c, dsn, "order-1");
```

Kill the process anywhere and resume the row in a fresh one with
`geas_instance_resume`, and the course comes back at the step the row named. A
step that finished before the row was written never runs again, and the step
that was running when the process died runs again from its start, so steps are
at least once and a body that must not act twice keys its side effect, the way
any worker does. The gate kills the saga at every step boundary and inside
every host body and checks exactly that.

The row also remembers which course wrote it. A module whose course changed,
even one duration in a `timeout`, refuses the row with `GEAS_ERR_VERSION`,
since a step number from one course means nothing to another, so a course that
changes ships as a new contract version.

```sh
make test-course-saga
```

### From Python

`interop/python/demo_course.py` walks the same saga. The binding grows a few
calls, `start_ticker`, `course_wait`, `course_info`, and `set_autopark`, and the
rest is what you already know:

```python
rt.bind("OrderCourse.ship", host.ship)
rt.bind("OrderCourse.chase", host.chase)
rt.bind("OrderCourse.refund", host.refund)
rt.freeze()
rt.start_ticker(5)

saga = rt.sign("OrderCourse")
info = saga.course_wait(5000)
# CourseInfo(state=waiting, step=5, ...) once the together is behind it
saga.signal("delivered", Record("ACME", "T-1"))
info = saga.course_wait(5000)
# info.state == GEAS_COURSE_FINISHED, saga.state() == GEAS_FULFILLED
```

A `course_wait` that runs out raises nothing, it hands back the `CourseInfo`
with `state` running and `step` where the course is, and only a wait from a
body or a hook raises `GeasError` with `GEAS_ERR_STATE`.

The demo also has the carrier refuse, so the course finishes at the `ship`
step while the saga reads Broken with `refund` handed the receipt, and it parks
the saga at the `select`, shuts the runtime down, resumes the row in a fresh
one, and finishes it there with no host body run twice.

```sh
make test-course-python
```

Same saga, same rules, and now the order lives in exactly one place.

## Where to go next

- [docs/grammar.md](grammar.md) is the normative language grammar, every rule
  this tutorial showed and the ones it did not.
- [docs/abi.md](abi.md) is the ABI a foreign host compiles against, the value
  layouts, the thunk frame, ownership, mangling, the hash, and the workflow
  surface: the clock and the tick, terms, signals, remedies, the course, and
  the park row.
- [docs/bridge.md](bridge.md) is the layer this tutorial skipped: the same
  contracts served over gRPC, the wire surface emitted by the compiler and
  driven from any language with stock tooling.

The fastest way to learn the rest is to read `skeleton/` beside `docs/abi.md`
and change something. Every file there is compiled and driven by a gate, so a
broken change fails loudly and a working one runs.
