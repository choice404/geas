# geas

Geas is a statically typed language for durable obligations. You write a contract, and the contract is the whole process: its steps, their deadlines, the order they run in, how they retry, what undoes each one when things go wrong, and the policy that decides when the whole thing counts as kept, partly kept, or broken. A contract can take seconds or a week to keep, it can be partially fulfilled, it can break, it can make amends, and it can be parked on one machine and picked back up on another. The checker proves the policy is consistent before anything runs, and the runtime holds every term while it does.

A geas, in Celtic myth, is a binding obligation laid on a person: keep it and it protects you, break it and the consequences find you. That is the whole design of this language, so the name is not a metaphor, it is a description. A contract that can be partially fulfilled, broken, remedied, and carried across a crash is that idea taken literally. Say it "gesh" or "gee-as", both are in circulation and the compiler answers to either.

The compiler for Geas is `geas`, and source files for Geas end in `.geas`. The compiler is written by hand in C++23 and emits C, which links against a small runtime written in C, `libgeasrt`, a shared library with a stable C ABI. So any language that can load a C library can be a party to a contract: it can sign one, fulfill its steps, deliver events into it, and read where it stands, with the runtime owning every allocation that crosses the line. Across processes the same contracts speak gRPC, emitted straight from the source. Interop is how every language takes part, it is not the point of the language, the obligation is.

## Workflows, sagas, and why a language

So the easiest way to explain what Geas is for is an online order. You validate the card, charge it, reserve the stock, ship the box, and wait for the delivery scan. Each step might hit a different service written in a different language, any of them can fail, and some of them take days. If the server restarts on day two, the process has to pick up where it left off, not start over and not forget that the card was already charged.

That kind of process is called a workflow. Now when a workflow crosses services you cannot wrap it in one database transaction, so the standard pattern is a saga: every step that changes something gets a compensating step that undoes it, a refund for a charge, a release for a reservation, and when the saga fails you run the compensations newest first. Think of it like a trip you booked piece by piece, the flight, the hotel, the car; if the flight gets cancelled you do not just stop, you go back and cancel the car and then the hotel, in the reverse order you booked them.

The tools people reach for today keep the important parts of that, what counts as done, what counts as partly done, what gets undone, buried in imperative code or in JSON. Geas makes those parts the language. A contract declares its steps as pledges, its deadlines and ordering and retries as terms, its compensations as remedies, its outside events as signals, and its success and failure as a requirements block written in boolean logic, so the shape of the process is something the checker can read and the runtime can enforce instead of something you reconstruct from the code later.

## Core ideas

- Everything is a contract. The contract is the unit of the language, not the function, and a running workflow is a signed contract.
- Obligations have terms. A deadline is `within 30s`, an ordering is `after authorize`, a retry policy is `retry 3 every 2s backoff 2.0`, and a contract's whole lifetime is `[term: 7d]`. The runtime holds them, the code does not have to.
- Partial is a real state. A contract reports exactly which steps landed, which are pending, and which broke, mid flight and from any language.
- Breaking has consequences. A remedy undoes a step that landed, and when a contract breaks its remedies run newest first, each handed the value its step produced.
- Nothing is implicit. Ownership is always clear, visibility is always stated, and every fallible pledge returns a `Result` or an `Option`.
- Errors are values. No exceptions, and nothing throws across a language boundary. A missed deadline is a lifecycle fact, never a value of your own error type.

## A taste of the language

Here is the order saga, the whole contract from `skeleton/saga.geas` with its comments trimmed. The records it names, `Card`, `AuthId`, `Receipt`, `Reservation`, and `Tracking`, are declared beside it in that file.

```text
PayError is either Declined(reason: String) or Timeout or Gateway(code: Int)
StockError is either OutOfStock(sku: String) or Unavailable
ShipError is either NoCarrier or Refused(reason: String)

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

    pledge reserve(sku: String, hold: Duration) -> Result<Reservation, StockError> {
        if sku == "" {
            return Err(OutOfStock(sku))
        }
        let total = hold + grace
        if total > 1d {
            return Err(Unavailable)
        }
        return Ok(Reservation { id: "R-" + sku, sku: sku, hold: total })
    }

    pledge ship(res: Reservation) -> Result<Tracking, ShipError>
        after Payment, reserve

    signal delivered(tracking: Tracking) after ship within 3d

    remedy refund for capture(receipt: Receipt) -> Result<Unit, PayError>
        retry 2 every 1m

    remedy release for reserve(res: Reservation) -> Result<Unit, StockError> {
        if res.id != "R-" + res.sku || res.hold < grace {
            return Err(Unavailable)
        }
        return Ok(released())
    }

    clause released() -> Unit {
        return
    }

    requirements {
        fulfill: Payment && reserve && ship && delivered
        partial: Payment || reserve
        break: !Payment || !reserve || !ship || !delivered
    }
}
```

A `vow` is an immutable field locked when the contract is signed, and `grace` is a `Duration`, written `30m`, which is just milliseconds once it crosses the boundary. A `pledge` is a step with a declared return type; `reserve` has a body written in Geas, while `authorize`, `capture`, and `ship` have none, so the host binds them, since a payment gateway and a carrier are the host's to call. A `subcontract` groups steps so the policy can name them as one. The terms trail a signature one per line: `authorize` has thirty seconds and three retries on a `Timeout` or a `Gateway` hiccup, `capture` waits on `authorize`, and `ship` waits on the whole `Payment` subcontract and on `reserve`. A `signal` is an outside event, here the delivery scan, and it has three days from the moment `ship` landed. A `remedy` undoes the step it names, handed that step's `Ok` value. And the `requirements` block writes the whole policy as boolean logic, which the checker enumerates to prove `fulfill` and `break` can never hold at once.

So when any of it breaks, by an `Err`, by a missed deadline, or by the week running out, every step that already landed is undone through its remedy, newest first: `release` gets the reservation `reserve` produced, then `refund` gets the receipt `capture` produced.

That contract leaves the order to its host. Give it a `course` and it runs itself, this is the heart of `skeleton/order_course.geas`, the same saga with its steps written out:

```text
course {
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

Every call there is a durable step held to the same terms, `capture` and `reserve` run at the same time, and a parcel quiet for two days gets chased and waited on again.

## The lifecycle

A contract moves through six states.

```text
Unsigned -> Signed -> Fulfilled
                   -> Partial
                   -> Remedying -> Broken
                   -> Broken                (no remedies due)
```

`sign()` validates the contract shape, locks the vows, starts the clock on its terms, and activates it. Fulfilling a pledge runs it and latches the outcome: fulfilled on its first `Ok`, broken on an `Err` before that, or broken by a lapse when its deadline passes first. Delivering a signal latches it fulfilled. After every latch the policy runs, break first, then fulfill, then partial, and moves the contract. When the contract breaks, by its policy, by its term running out, or by an explicit `break()`, it reads `Remedying` while its due remedies run and `Broken` once they are done. A remedy that fails is recorded and does not stop the others. A fulfilled contract met its terms, so it never runs a remedy. At any moment the partial surface reports exactly which items landed, which are pending, which broke, which of those broke by a lapse, and how every remedy went.

## Two ways to drive a contract

Geas has two modes, and they share everything on the outside: the same requirements, terms, remedies, and lifecycle, so a contract can start in one and grow into the other without changing its shape.

Host driven is the mode of a contract with no `course`. A host signs the contract and calls its pledges in whatever order it likes, and the runtime enforces the terms: a step called before what it waits on answers a state error and never runs, a missed deadline latches the step broken, retries happen inside the runtime with their delays waited out, and remedies run on break. The host stays in charge of the order, and the contract stays in charge of the rules. `tests/runtime/test_saga.c` and `interop/python/demo_saga.py` each walk the order saga this way, from C and from Python.

Engine driven is the mode of a contract with a `course`, like the one in the taste above. So the compiler lowers the course to a resumable state machine, and the runtime runs it from the moment the contract signs: each pledge call is a step that runs through the same terms a host call gets, `await` waits on a signal, `select` takes whichever of several awaits or a timeout is ready first, `sleep` pauses, and `together` runs its calls at the same time on the runtime's pool. While a course waits no thread waits with it, it is a step number and a few values on the instance, and a signal or a deadline enters it again right at that step. Armed with a store, the instance writes its row at every step, so a process can die anywhere and a fresh one resumes the course at the step it stopped on, with steps at least once, and a row from a course that has since changed is refused rather than run against the wrong steps. The host's job shrinks to binding the bodies the contract leaves abstract and handing signals in, a host call of one of its pledges is a state error. `tests/runtime/test_course_saga.c`, `interop/python/demo_course.py`, and `interop/dusk/demo_course.dusk` each drive `skeleton/order_course.geas` this way.

The course ending is not the contract's state, that stays with the requirements. A step that answers `Err` ends the course at its `?`, and it is the break line that breaks the contract and runs the remedies, exactly as in host driven mode.

## Amendment

A saga waiting three days on a parcel will see a deploy or two, so a contract says how an instance parked under an older version comes back as this one. `skeleton/order_course_v2.geas` is the order saga at version 2, ship renamed dispatch, a notify step added, chase gone, the order counted in cents, and a course changed to match, and its `amends 1` block covers every one of those differences:

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

`geas record skeleton/order_course.geas` writes the shape of version 1 to `skeleton/shapes/OrderCourse.v1.shape`, and the checker diffs that recorded shape against version 2, so a block that misses a difference, or covers one that is not there, fails the build. The runtime that loads version 2 resumes a version 1 row through the block: the vow is worked out from `prior.amount`, ship's latch and tracking carry over to dispatch, the course starts again at step 0 with every latch where it was, and from then on it is an ordinary version 2 instance. `tests/runtime/test_amend_saga.c` and `interop/python/demo_amend.py` park a version 1 saga and finish it under version 2.

## How it works

The compiler reads a `.geas` file, checks it, and emits plain C that includes one header, `geas/geas.h`, then links it with cc into a loadable `.geas.so`. That module carries its contracts as descriptor tables: every pledge and its signature, every vow and its default, the requirements as a table the checker and the runtime both read so the two can never disagree, and the workflow tail, the terms, the signals, the remedies, and the contract term.

The runtime registers those descriptors in the iname table, a queryable registry of every contract and pledge signature with the type signature hash and the version baked into each mangled name, so a version mismatch is a descriptive error at link time instead of silent corruption at run time. A C program, or anything that can load a C library, looks up a contract, signs it, binds the steps it owns, fulfills pledges, delivers signals, and reads the partial surface, all through a handful of runtime calls; `geas_pledge_bind_user` lets one body carry a user pointer for many bindings, futures poll and release with `geas_future_poll` and `geas_future_release`, a running body sees a pending break through `geas_cancel_requested`, and `geas_runtime_set_event_hook` observes every lifecycle transition in order after the instance lock is released.

Time runs through a clock the host can replace. Every call that touches an instance checks its deadlines first, and a host that wants a lapse, a `sleep`, or a `select` timeout the moment it falls due either calls `geas_runtime_tick` from whatever scheduler it already has or starts the runtime's own ticker thread with `geas_runtime_start_ticker`. Swap the clock for a fake one and a three day deadline becomes a number a test moves, which is exactly how the saga gates run.

An instance's durable state fits in one store row: the vows, the latches, the error payloads, the transactional fates, and a versioned workflow column holding eligibility stamps, lapses, the `Ok` values the remedies will need, signal payloads, retry progress, remedy progress, and for a course its frame, the step it stands at and every value it bound. `geas_instance_park` writes it, `geas_contract_set_autopark` has the runtime write it at every step and every latch, including during a pledge retry wait or a remedy pass, `geas_instance_resume` stands it back up in another process with the same retry budget and only the remedies not yet started, and a deadline keeps counting from where it started across the park, since the clock is wall time.

## Next to other tools

Durable execution is a well trodden space, and the tools in it are good at what they do. Temporal and Restate run workflows durably with big, mature ecosystems; the success and compensation logic lives in the workflow code, and versioning a running workflow goes through their own patching rules. AWS Step Functions is managed and visual, with the state machine written as JSON and tied to AWS. BPMN engines model business processes as diagrams. A hand rolled saga gives you total control and asks you to build timeouts, partial state, and compensation yourself.

Where Geas differs is what it makes declarative. The requirements block is the success, partial, and failure policy as checked boolean logic over steps, subcontracts, and signals. Partial fulfillment is a first class state any host can read mid flight. Amendment between versions is typed, a mapping the checker holds you to. And any language that loads a `.so` or speaks gRPC can be a party. Geas is also young, a single repository with one runtime, so it is not a replacement for a managed platform, it is a different take on where the rules should live.

## Installing

On Arch, the [geas](https://aur.archlinux.org/packages/geas) package on the AUR builds from the latest tagged release:

```sh
paru -S geas   # or yay -S geas
```

From a checkout you need a C++23 compiler, gcc or clang, CMake, cc, and make. `make compiler` builds `geas` with CMake into `target/bin/geas`, and `make runtime` builds `libgeasrt.so` into `target/geas-out/`. `make install` lays the toolchain out under a prefix: the `geas` binary, `libgeasrt.so`, the C ABI headers, and the std modules. The default prefix is `/usr/local`.

```sh
make compiler runtime
sudo make install                   # PREFIX=/usr/local
make install PREFIX=$HOME/.local    # a user install
make DESTDIR=./stage install        # a staged install for packaging
```

`make compiler SANITIZE=ON` builds the same compiler under the address and undefined behavior sanitizers into `target/bin-asan/geas` instead, so the plain binary is never silently swapped for the instrumented one.

An installed `geas` needs no environment variables. It probes for a repository checkout first and falls back to the system paths; `GEAS_ROOT` and `GEAS_HOME` still override both for out of tree development.

## Usage

New to Geas, start with [docs/tutorial.md](docs/tutorial.md): it walks the whole language, drives one contract from C and from Python with no generated bindings, and then walks the order saga, host driven, engine driven, and across a deploy from one version to the next. [spec.md](spec.md) is the language reference, [docs/grammar.md](docs/grammar.md) is the normative grammar, [docs/abi.md](docs/abi.md) is the C surface a host compiles against, and [CHANGELOG.md](CHANGELOG.md) carries the history version by version.

The whole pipeline runs end to end, and one target runs every gate:

```sh
# build everything and run the end to end check
make smoke

# every functional gate the repository carries
make all
```

`make smoke` builds the runtime shared library `libgeasrt.so` from `runtime/`, the `geas` binary from `compiler/`, the compiled contract module `libhello.geas.so` from `skeleton/hello.geas`, and the C host from `skeleton/host.c`, then runs the host, which loads the module, signs `Greeter`, fulfills `greet("world")`, checks the result is `Ok("hello, world")`, exercises the error paths, and breaks the contract. Everything lands under `target/`.

`geas` checks programs, builds modules and standalone executables, and writes the C header and the gRPC surface a foreign host builds against:

```sh
target/bin/geas version
target/bin/geas check skeleton/saga.geas
target/bin/geas build skeleton/hello.geas
target/bin/geas build --bin skeleton/main_demo.geas
target/bin/geas emit-header skeleton/hello.geas
target/bin/geas emit-proto skeleton/saga.geas
target/bin/geas record skeleton/order_course.geas
```

`record` writes the shape of every public contract in a file to `shapes/<Contract>.v<version>.shape` beside it, the recorded shape a later version's `amends` block is checked against.

`build` reads the source, emits the module C into `target/geas-out/`, and links it with cc into a loadable `.geas.so`. Each module names `libgeasrt.so.0` as its runtime dependency, so a host may link the runtime plainly without preloading it with `RTLD_GLOBAL`. Hosts that already preload `libgeasrt.so` globally keep using the same runtime through its symlink to `libgeasrt.so.0`. Run it from the repository root, or set `GEAS_ROOT` to a checkout and run `geas` from anywhere: `GEAS_ROOT` points cc at the runtime headers and `libgeasrt`, and `GEAS_HOME` at the standard library for imports.

The front end has its own golden suite: `tests/run.sh` drives the lex, parse, check, and shape goldens, every accepted program beside its expected dump and every rejected one beside the diagnostics it must produce, and `compiler/tests/run_codegen.sh` pins the emitted C.

### The workflow gates

The saga is where the language's point shows, so it has the most gates. Each one runs on a fake clock, so a deadline is a number the test moves and a retry delay is an entry in a list the test reads.

```sh
# the order saga from C: the happy path, an early step, a retried authorize,
# a failed ship and its remedies, a lapsed signal, the week running out,
# and a park and resume in the middle of it all
make test-saga

# the same saga from Python, a Python clock and Python steps
make test-saga-python

# the order saga engine driven: the course on its own, a failed step, a
# retry, the select timing out into a chase, a lapse, the together running
# both calls at once, a kill at every step and inside every body resumed in
# a fresh runtime, a changed course refused, the host breaking it waiting,
# running, and mid together, and the week running out
make test-course-saga

# the same course from Python
make test-course-python

# a version 1 saga parked and finished under version 2: mid course with the
# course restarted, a step failing after the deploy, a second park, a kill at
# every step boundary, a module without the block and a drifted row refused,
# and the host driven saga amended too
make test-amend-saga

# the same deploy from Python
make test-amend-python

# the runtime surface underneath, one gate each, all under ASan and UBSan
make test-terms test-remedy test-signal test-workflow-park

# the same four under ThreadSanitizer
make tsan-workflow
```

`make test-terms` covers ordering, deadlines counted from eligibility, a subcontract's deadline, the retry count and delay sequence and cap and `on` filter, and the contract term. `make test-remedy` covers newest latch first, `Remedying` seen from the body and from a second thread, a failing remedy recorded without stopping the rest, and a host break racing the pass. `make test-signal` covers delivery, payloads, the first delivery winning, and a signal as an atom and an `after` target. `make test-workflow-park` carries all of that across a park into a fresh runtime, including a remedy pass parked in the middle.

### Two languages, one contract

The runtime binds from anything that can load a C library, and the repository proves it with Python and dusk. `interop/python/geas.py` is a ctypes binding written against [docs/abi.md](docs/abi.md) alone, no header parsed, no code generated, and exposes event delivery through `rt.on_event`. `interop/python/demo_payment.py` walks the payment contract: it binds a Python function over the abstract `charge` pledge, signs with a vow override, drives the partial, fulfilled, and broken paths, and takes a by reference argument back through the write back. `interop/python/demo_saga.py` points the runtime at a Python clock, binds Python callables over the saga's steps, and watches the remedies run, and `interop/python/demo_course.py` lets the course run the same saga, then parks it at the `select` and finishes it in a fresh runtime. `interop/dusk` carries the same host surface through a thin C shim, with pledge and remedy handlers bound onto the runtime's worker threads.

```sh
make test-python
make test-saga-python
```

### Across processes

The same contracts run across processes, and the wire is gRPC. Geas does not carry a transport of its own: the compiler emits the wire surface and the whole gRPC ecosystem is the transport. `geas emit-proto` writes `<stem>.proto`, where every pledge is its own typed rpc, every signal its own `Signal` rpc, and signing is a stream whose lifetime is the instance's, plus the session wrappers protoc cannot write, `<stem>_session.go` for Go and `<stem>_session.ts` for Node over `@grpc/proto-loader`. A server is ordinary host code holding the runtime through the C ABI and serving the emitted surface; a client in any gRPC language builds from the emitted artifacts with stock tooling and never learns Geas exists. A pledge's `Err` crosses as a value on an OK rpc, a Geas status crosses as a gRPC code, and the shape hash pins both ends to one contract. [docs/bridge.md](docs/bridge.md) is the normative surface.

```sh
# the payment contract served over gRPC, driven from Python, Go, and Node
make test-grpc-bridge
make test-grpc-go
make test-grpc-node

# park, kill the server, resume on a fresh one, finish the contract
make test-grpc-resume

# two replicas, one park store: kill one mid session, resume on the other,
# then race both for a single token and demand exactly one winner
make test-grpc-failover
```

A session survives more than its connection. A server started with a park store writes an instance down when its stream ends and a `Resume` call stands it back up under the token the signature carried, one shot, and when two replicas race the same token the row's delete is the claim, so exactly one wins.

### Backed by a database

A contract declares the table shape it needs as a `schema`, a vow like any other that locks at sign, binds a live database through a `dsn` vow, and its pledges read and write rows through the store standard library. A group of writes that must all land or all vanish is a `transactional` subcontract, the unit the language already had for all or nothing. The store is invisible across the C ABI, so a host drives a store backed contract with the same calls it always used. The reference backend is SQLite, vendored and compiled into `libgeasrt`, so the store gates are hermetic. [docs/database.md](docs/database.md) is the normative store design.

```sh
make test-store test-store-txn test-store-python
```

### A whole program

[examples/supervisor](examples/supervisor) is a small process supervisor whose service state machine is a contract instance, one per run: Signed is starting, Partial is running, Fulfilled is a clean exit, Broken is a crash. The host binds the spawn and the health pass over abstract pledges, the run record lives on the store, and a supervisor restart is a park and a resume.

```sh
make test-supervisor test-supervisor-watch
```

### Standard library

Import the modules under `lib/std` where a contract needs them:

- `std.math`, `std.strings`, and `std.collections` provide arithmetic, string, and list helpers.
- `std.errors` carries common error sums, and `std.traits` carries provisional clauses.
- `std.store` provides the `Store` operations over a contract's schemas.
- `std.process` provides `Process.run(Command)`, a program step returning `Result<Unit, ExitError>`, with output redirection and cancellation. The program is a path, arguments stay separate, and exit zero is `Ok`.

### A standalone program

Geas compiles straight to an executable too. A program declares one `Main` contract with a `run(args: List<String>) -> Result<Int, E>` pledge, and `build --bin` links it: `Ok(n)` becomes the exit code, `Err` prints itself to stderr and exits 1.

```sh
make test-bin
./target/geas-out/main_demo a b c; echo $?   # counts its args, exits 3
```

## Status

What runs today, end to end, host driven and engine driven: contracts with vows, pledges, clauses, subcontracts, and requirements policies, records and sums, match, loops, propagation, imports with package visibility, and a standard library under `lib/std`. `Duration` and `Time`, with duration literals like `1h30m`, and size literals like `64M`, `512K`, `1G`, and `2T`, binary byte counts typed as `Int`. Declared attributes, typed settings named at the top level and carried by contracts, subcontracts, pledges, signals, and remedies for a host to read before or after sign. `Bytes`, an arbitrary byte buffer with String's layout and no UTF-8 rule, across the runtime, park, proto, Go, TypeScript, and Python wherever String goes except map keys and schema columns. A written requirements block holds up to 64 distinct atoms, checked by satisfiability search so fulfill and break cannot both hold, while a contract with no block synthesizes its default for up to 255 items. Terms, `within`, `after`, and `retry ... every ... backoff ... cap ... on`, on pledges, subcontracts, signals, and remedies, plus the contract `term`. Remedies and the `Remedying` state. The `course`, `await`, `select`, `sleep`, `together`, and `now()`, lowered to a resumable state machine the runtime drives on its pool, with autopark writing the row at every step, resume entering at the saved step, the course hash refusing a row from a changed course, a ticker thread, and the course read and waited on from C, Python, and dusk and over the bridge with a `GetCourse` rpc. Amendment, `amends` blocks checked against the recorded shape of the version they amend and applied by the runtime at the resume, the shape hash and every signature hash covering the layout of each record and sum the boundary reaches. Signals, delivered from C, from Python, and from another contract's body, with a typed `Signal` rpc per signal on the emitted gRPC surface. The clock and the tick, the partial surface with its lapses and remedy outcomes, and a park row that carries all of it across processes. The C runtime enforces the lifecycle with per pledge latching, user-bound host bodies, pollable and releasable futures, cancellation a body can read, and the out pointer helpers `geas_string_copy_into`, `geas_map_new_into`, and `geas_state_value_into`, runs fulfillments on a worker pool, owns every allocation an instance makes, and reclaims it all at break. Golden and compile fail suites, sanitizer gates, thread sanitizer gates, a determinism gate, store gates, and bridge gates hold the line on every build.

The roadmap, next:

- Signals on the reference bridge server: `emit-proto` already writes a `Signal` rpc per signal and a helper in each session wrapper, and the Python reference server in `interop/grpc` serves them next.

## Requirements

- A C++23 compiler, gcc or clang, and CMake, to build `geas`.
- cc, to compile the emitted C and the runtime.
- make.
- python3, for the Python and bridge gates, and Go and Node for their bridge clients.

## License

MIT, see [LICENSE](LICENSE).
