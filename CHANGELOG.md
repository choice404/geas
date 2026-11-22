# Changelog

Every released version of Geas, newest first. The language was named
Ashford through v0.6.8; entries below the rename keep the names they
shipped with, because a changelog that rewrites its own past is not one.

## [v1.0.1] the shelf

Geas now ships the way a stable language is installed and gated. The gates run
in tiers sized to how often they are run, the runtime is compiled once per
flavour so every gate runs beside every other one, a fulfillment that ends its
own instance publishes its receipt in the same order every other outcome
keeps, and the toolchain lands on Arch as three packages over one release
tarball. Nothing in the frozen surface moves, so every module, host, and
parked row built against 1.0.0 runs unchanged.

- run the gates in four tiers: `make quick`, the front end and codegen goldens,
  the smoke walk, the language lowering gate, and the host driven saga, in
  about fifteen seconds; `make check`, which `make all` is the same target as,
  every test the repository carries exactly once, each TSan gate one round and
  each fuzz pass a short count, safe under `-j`, about twenty five seconds as
  `make -j24 check` from a built compiler, well under a minute from nothing but
  the compiler, and a little over two minutes serial; `make soak`, the twenty
  and thirty round TSan loops, the full fuzz counts, and the hundred thousand
  round reuse case, every round its own make target and `SOAK_ROUNDS` setting
  the rounds of every repeated test at once, about a minute; and
  `make check-net`, the gRPC bridge and supervisor gates, each skipping clean
  where grpcio, Go, or Node is absent, in about ten seconds
- compile the runtime once per sanitizer flavour into `target/geas-out/rt/`, an
  archive per flavour each gate links whole, so a gate compiles only its own
  test and a rerun rebuilds only what a change reached
- run the cases of `tests/run.sh` and `compiler/tests/run_codegen.sh` side by
  side, reported in order, as the `test-golden` and `test-codegen` gates
- publish the receipt of a fulfillment whose own body broke or released its
  instance once the RECLAIMED or RELEASED event that ending recorded is
  delivered, holding the `GEAS_ERR_STATE` the forfeit gave it, the order every
  other outcome keeps, so a wait that returns has seen that event
- drop a deadline a course entry finds already past and owned by no wait the
  course stands at, a row brought back with a stray one for instance, and end
  the run there rather than entering the course again for it
- package the toolchain for Arch as `geas-lang`, built from the tagged source
  and taking over from the `geas` package, `geas-lang-git`, built from the head
  of the repository, and `geas-lang-bin`, installed from the release tarball
- cut `geas-lang-<version>-x86_64-linux.tar.gz` and its `.sha256` with
  `packaging/release.sh`, and attach both to the GitHub release of every
  version tag from `.github/workflows/release.yml`

## [v1.0.0] the kept word

Geas is stable. The language the spec and the grammar describe, the C surface
of `geas.h` and `geas_abi.h`, and the formats a host or a database holds on to,
the mangled names, the shape and course hashes, the wire codec, the park row,
and the recorded shape, are the ones every 1.x release keeps, as the Stability
section of `docs/abi.md` sets out, so a module, a host, and a parked row built
against 1.0.0 go on working under every later 1.x compiler and runtime. This is
the language and the runtime that promise is made about: every path a body
takes ends in a value or in a status the host can read, every value the runtime
keeps has a type and a size it can vouch for, and every number a reader would
want to recompute is written down.

- freeze the surface named in the Stability section of `docs/abi.md`, the
  limits `GEAS_VALUE_MAX_DEPTH`, `GEAS_VALUE_MAX_NODES`, and
  `GEAS_SLOT_MIN_NODES_MAX` with their values, so it grows only by new
  descriptor tails, new enum values at the end, and new calls beside the old
- require every path through a pledge, clause, or remedy that returns a value
  to end in a `return`, so no body falls off its closing brace
- stop a body on integer overflow, on a division or remainder by zero, and on
  the least `Int` over `-1`, with the status an out of range index reports,
  leaving the host, its other calls, and every other instance as they were,
  while `Float` follows IEEE 754 and takes no `%`
- refuse a type name nothing declares, a zero `within` or `term`, an empty
  subcontract, and an empty `select`
- spell the one `Unit` value `Unit` and compare it like any value, write the
  least `Int`, `Duration`, and size right after a unary minus, hold `version`
  to 1 through 4294967295, and read a `Float` literal by its value, so
  `1_000.5` reaches the module C as `1000.5` and one past the largest double
  fails to lex
- leave pledge values to a later version, refusing a pledge type and a bare
  `instance.pledge` wherever either appears while `GEAS_TY_PLEDGE_REF` keeps
  its number
- seed `latch NAME as fulfilled` only on an item that carries no value
- resolve `[abi: "c", symbol: "..."]` at every sign, resume, and claim, after a
  host binding and a compiled body, with the symbol kept off the `geas_`,
  `__geas_`, and `GEAS_` prefixes
- hash the members of every subcontract, its transactional flag, and the
  written requirements into the shape hash, and lead every segment of a mangled
  name with its length
- take the course hash from the lowered course and the types of its slots, by
  value and by position, so a renamed binding, a reordered record literal, or a
  respelled literal leaves it alone and every change in what the course does
  moves it
- give every course slot its type, start it at the smallest value of that type,
  hold it to that type at resume, claim, and amend, and refuse a slot type
  whose smallest value nests past 256 levels or holds more than 4096 values
- grow the instance, module, descriptor, binding, and iname tables as they
  fill, and add `geas_contract_free`, which hands back an ended instance before
  shutdown once nothing runs on it
- stamp every park row with a `gen` drawn at each write, so a claim takes
  exactly the row it read, and bring an older `geas_park` table forward with
  checked `ALTER TABLE` statements several processes can race on
- hold every value to `GEAS_VALUE_MAX_NODES`, answering `GEAS_ERR_OOM` at a
  boundary, a deep copy, a list push, a map set, the wire encoder, and a park,
  and hold the wire decoder to a budget its own buffer sets
- keep a negative timestamp as the clock reading it is, run every wait of the
  runtime's own on the monotonic clock, and write flow blob version 5 while
  reading 2 through 5
- let a remedy finish its work under a cancel, `geas_cancel_requested`
  answering 0 on the pass's own thread, and leave a retryable outcome unlatched
  under a request, so a cancel never makes final an outcome the pledge meant
  to try again
- run a ref `write_back` callback after the default write backs, with no
  runtime lock held and on a copy of the value, so it may call into the
  runtime, its own instance included
- let a host call wait out another thread's event delivery, fulfill, signal,
  park, break, release, autopark, free, and tick alike, on a condition with
  the instance lock given up, and then run against the instance as it
  stands, while the same calls from inside the hook or from a body stay
  `GEAS_ERR_STATE`; publish a fulfillment's outcome after its own events were
  delivered, so a sync fulfill returns with its events already seen, and
  list every reason `GEAS_ERR_STATE` is answered beside the status table
- shut down in one order every time, the cancel flag, the ticker, the pool,
  then the instances, descriptors, and modules, refusing with one line on
  stderr when called from a body, a hook, a write back, a together body, the
  ticker, or a thread holding an instance lock
- load the runtime into the Python binding with `RTLD_LOCAL`, so its exports
  and its SQLite stay out of the process's global scope
- compile every module, every course, and every golden the codegen suite pins
  under `-Wall -Wextra` with no warning
- hold `docs/abi.md`, the spec, and the grammar to the headers call by call,
  and write the wire codec, the park row, the flow blob, the shape hash, and
  the course hash down closely enough to recompute each from the text, with
  worked examples a build reproduces
- rebuild every module and record every shape again for a contract with a
  course, a subcontract, or a requirements block, since its shape hash or
  course hash differs under 1.0.0, so a row parked for such a contract by
  v0.9.3 or earlier answers `GEAS_ERR_VERSION` here and finishes under the
  release that wrote it, and a shape file of format 1 or 2 is refused until
  `geas record` writes it again

## [v0.9.3] the checked boundary

Geas now holds every value that crosses into its code, or into the runtime's
keeping, to the type the contract declared all the way down. The compiler
describes those types once, the runtime checks them before code runs or state
moves, and the wire codec holds the same nesting line, so a host, a bridge, and
a parked row all meet one boundary and everything it accepts can be kept.

- emit one type table on each contract descriptor tail, naming every distinct
  type once and joining recursive types by index, with each pledge, signal,
  vow, and remedy parameter and return tied to its declared type
- add `geas dump-types`, validate the whole table when a contract registers,
  and leave a descriptor with no table on the tag checks it carried before
- check fulfillment arguments for synchronous, asynchronous, nested, course,
  and together calls, every compiled or bound body answer, signal payload,
  vow at sign, remedy argument and answer, by reference argument, vow computed
  by an amends block, and value decoded at resume, claim, or amendment
- walk every element, record field, variant tag and payload, option and result
  box, map key and value, and tuple position, requiring the declared arity and
  each map key once, Bool as 0 or 1, Char as a Unicode scalar value, String as
  valid UTF-8, and nesting bounded at 256 levels
- answer a value that does not conform with `GEAS_ERR_TYPE`, before a body runs
  or anything is stored, and leave a bound body's malformed answer unlatched
  so the call may be made again while a course halts on the same status
- make deep copy and wire decode answer a status for malformed input, share the
  depth limit between the check and codec, require None to carry no box, and
  permit an Ok or Err of Unit to carry none
- keep pledge types inside clauses and local bindings, and refuse one at every
  pledge, signal, remedy, vow, record, or variant boundary position
- answer a bridge request whose values do not conform with `INVALID_ARGUMENT`,
  while a supplied body that raises or returns a malformed value answers
  `INTERNAL`
- put the contract name's length in every per contract C symbol, so names that
  run together cannot produce the same emitted symbol within a module
- add `skeleton/boundary.geas` and `skeleton/boundary_host.geas`, the
  `test-conform` and `test-boundary` gates, and fuzz loops over both park row
  bytes and wire bytes

## [v0.9.2] the bridge

Geas now carries a contract across the process boundary on the surface the
contract itself declares, the compiler emits that surface, and the reference
bridge serves any compiled module without knowing its names or types ahead of
time. A session holds the obligation while typed calls, signals, partial state,
and courses cross by reflection, a parked row can move it safely between
processes, and Python, Go, and Node clients walk the same payment and order
contracts, so the bridge is ordinary generated gRPC around the same lifecycle.

- replace the handwritten payment proto with each contract's emitted surface,
  and load compiled modules and their `.proto` descriptors when the reference
  server starts
- serve `Session`, typed pledge RPCs, signal RPCs, `GetPartial` with
  `REMEDYING`, `Break`, `Resume`, and `GetCourse`, with abstract bodies supplied
  by a handlers file
- convert messages and values both ways by reflection over the descriptors,
  including nested options, while keeping business errors in typed results and
  lifecycle errors in gRPC statuses
- ask running bodies to stop when a session ends, wait out admitted calls off
  the gRPC completion threads, then park and release the instance or break it,
  without running remedies when a parked copy is released
- bound session admission below the worker count, answer `RESOURCE_EXHAUSTED`
  beyond it, and make `Resume` one claim so exactly one server wins a parked row
- add `geas_contract_request_cancel`, which publishes a cancel request without
  breaking, `geas_contract_release`, which writes the last autopark row and
  lets go without remedies, and `geas_instance_claim`, which resumes and
  removes one row atomically
- add the `released` event and carry cancellation, release, claim, and that
  event through the Python package and dusk bindings
- make `geas_store_query` keep one consistent set of rows when a row is deleted
  between its passes, and move the Python binding to `interop/python/geas/`
  without changing its import surface
- walk payment, the order saga and its signal, and the order course over the
  bridge from Python, Go, and Node, all against the emitted contract surface
- pin emitted output for every program under `skeleton/`, `lib/std/`, and
  `examples/`, refuse a program with no golden, and add the `test-handoff`,
  `test-handoff-python`, `test-grpc-saga`, `test-grpc-course`,
  `test-grpc-saga-go`, `test-grpc-saga-node`, `test-grpc-edge`, and
  `test-grpc-handoff` gates

## [v0.9.1] the amendment

Geas now carries a parked obligation across the versions that have to finish
it, a contract at version M names each earlier version N it accepts, accounts
for every difference in an `amends N` block, and resumes that row as an
ordinary version M instance. Recorded shapes give the checker the version
that shipped, layout enters every hash whose boundary reaches it, and the
runtime applies the whole map or leaves the row untouched, so a deploy can
move the contract forward without pretending its durable past disappeared.

- add `geas record FILE [--force]`, writing format 2 recorded shapes for
  every public contract to `shapes/<Contract>.v<N>.shape` beside its source,
  and have a missing shape name the command that records the earlier version
- diff the recorded shape of N against M, require every difference to be
  covered exactly once, and refuse an amendment item that covers nothing
- carry matching vows, take defaults, or set a changed or added vow with `vow
  NAME = expr`, where `prior.NAME` reads the value the parked version signed
- carry, rename, or retire pledges, signals, subcontracts, and remedies, seed
  an added item with `latch` or leave it pending, and restart a changed course
  with `restart course`
- map every `amends N` block straight from N to the version that carries it,
  without chaining through the versions between them
- put the layout of every record and sum a contract boundary reaches into the
  shape hash and each pledge or remedy signature hash that reaches it, so a
  changed field or variant is a version mismatch and a changed amendment type
- emit `GeasAmendDesc` tables on the contract descriptor tail, with vow, item,
  subcontract, and remedy maps, latch seeds, and a vow function, and validate
  every table when its contract registers
- resume by version and shape hash, applying vows, payloads, latches, stamps,
  remedy records, episode fates, and a kept, dropped, or restarted course
  frame before this version's policy runs, all or nothing
- raise `amended` before `resumed`, add `geas_contract_amended_from`, and
  carry `amended_from` into the Python and dusk bindings
- add `skeleton/order_course_v2.geas` beside the first order course, keep both
  recorded shapes under `skeleton/shapes`, take the skeleton host's mangled
  name from its emitted header, and add the `test-amend`, `test-amend-saga`,
  and `test-amend-python` gates

## [v0.9.0] the course

Geas now runs the obligation as well as holding it, a contract can carry
its own course, leave the abstract bodies with its host, and move from one
durable step to the next on the runtime pool. The compiler turns the course
into one resumable C state machine, the frame and every wait cross a park
in the same row as the contract, and signals, deadlines, parallel calls,
event delivery, and automatic parking bring it back at the step it named,
so the host owns the work at the edges while the contract owns the order
in the middle.

- make a contract with a `course` engine driven, start it at sign and again
  at resume, leave its pledge bodies and signals with the host, and refuse a
  host fulfillment of one of its pledges with `GEAS_ERR_STATE`
- lower every course to one C function with a dispatch switch and resume
  labels, hoist suspending operations into statement position, number its
  steps, put every local in a frame slot, and show the result with
  `geas dump-course`
- run calls of the contract's own pledges as durable steps, add `await` with
  and without `within`, `select` over signals and timeouts, durable `sleep`,
  `now()`, and `together` calls running at once on the worker pool
- suspend a waiting course without holding its worker, wake it on a signal,
  a lapse, or a due deadline at a tick, and add `geas_runtime_start_ticker`
  for a runtime thread that supplies those ticks
- move the workflow blob to version 4 with the course step, frame slots,
  wait, and course hash, resume at that saved step, refuse a changed course
  with `GEAS_ERR_VERSION`, and keep rows from the earlier formats readable
- add `geas_contract_set_autopark`, writing the row at course steps and
  every latch, signal, state change, remedy step, and saved retry for engine
  driven and host driven instances, with steps remaining at least once
- add `geas_course_info` and `geas_course_wait`, join course transitions in
  the ordered event hook, and let a course wait out another thread's event
  delivery instead of stopping on it
- check where a suspending operation may stand, refuse one in the right side
  of `&&` or `||`, restrict a `together` block to own pledge calls, keep
  instance typed values out of the frame, require an irrefutable `as`
  pattern in a select arm, and keep a course off `Main`
- leave pledge RPCs out of the emitted proto for an engine driven contract,
  add `GetCourse`, and carry its read and wait helpers into the Go and
  TypeScript session wrappers
- carry the course surface into Python and dusk, with `demo_course` in
  each, so both hosts bind bodies, deliver signals, read progress, park, and
  resume
- add `skeleton/order_course.geas` and the `test-course`,
  `test-course-park`, `test-course-saga`, and `test-course-python` gates for
  the lowering, runtime, durable saga, and binding surfaces
- keep typed amendment next on the roadmap, with `amends` continuing to
  parse and check while its emitted resume mapping follows the course

## [v0.8.3] the watched job

The runtime now lets a host watch an obligation as it moves, park work while a
retry or remedy pass is still in flight, and launch a program as an ordinary
contract step. Every transition crosses one ordered event hook after the
instance lock is released, live progress rides the park row without granting a
retry or running a remedy twice, and the same host surface reaches Python and
dusk while the runtime keeps failure on its status paths instead of aborting.

- add `geas_runtime_set_event_hook`, delivering every latch, lapse, signal,
  remedy start and outcome, state change, term lapse, park, resume, cancel,
  and reclaim exactly once and in order per instance, on the producing thread
  after the instance lock is released
- let an event hook read its instance without mutating it, count an event that
  cannot be queued, and report that count through `GEAS_EVENT_LOST`, with
  Python exposing the same stream through `rt.on_event`
- park during a pledge retry wait with the attempts and due time carried in
  the row, then resume within the original retry budget, and park during a
  remedy pass, including from a remedy body or host sleep callback, so the
  resumed copy runs only remedies not yet started
- move the workflow blob to version 3 for live retry and remedy progress while
  continuing to resume rows written in the earlier formats
- add `std.process`, where `Process.run(Command)` launches an argv list with no
  shell, returns `Ok` on exit zero, redirects output to files when requested,
  reports `Exited`, `Signaled`, or `SpawnFailed` otherwise, and terminates and
  reaps the child on cancel
- add `skeleton/steps.geas` and the `test-process` gate for program steps in a
  pledge, clause, and remedy
- add the dusk host binding under `interop/dusk`, its value and attribute
  modules, thin C shim, saga demo, and `test-dusk` gate, with host handlers
  running on the runtime worker threads through `geas_pledge_bind_user`
- keep every runtime failure on an error or status path, with no call to abort

## [v0.8.2] the declared setting

Geas carries the settings and the bytes a durable obligation needs without
making either a private host convention: a size is an Int measured in binary
bytes, a declared attribute is a typed setting the module publishes before
or after sign, and Bytes crosses the same boundary as String without asking
raw data to be text. The requirements checker now answers its one hard
question by satisfiability search, so a written policy stays direct while
the compiler proves fulfill and break cannot both hold, and the skeletons
give each surface a small contract a host can start from.

- add size literals: `64M`, `512K`, `1G`, and `2T` are one token, binary
  multiples carried as an `Int` byte count wherever an integer literal may
  stand
- declare host settings at the top level with `attribute NAME: Type on
  TARGET`, importable and internal like other declarations, for contracts,
  subcontracts, pledges, signals, and remedies with Int, Float, Bool,
  String, or Duration values
- check an attribute list against its declaration beside the builtin keys,
  carry the descriptors through the module, and let a host read them before
  or after sign through `geas_attr_count`, `geas_attr_at`,
  `geas_contract_attr`, `geas_item_attr`, and the instance forms, outside
  the shape hash so a setting can change under a parked instance
- add Bytes: an arbitrary byte buffer with String's pointer and length
  layout but no UTF-8 rule, carried through values, frames, park, proto,
  Go, TypeScript, and Python everywhere String goes except map keys and
  schema columns, with `bytes`, `text`, and `byte_list` beside len, index,
  equality, and concatenation
- make the fulfill and break exclusion a satisfiability search, cap a
  written requirements block at 64 distinct atoms, synthesize a default
  policy for up to 255 items, and cross check the search against enumeration
  in the hidden self test
- add `skeleton/jobstep.geas`, `skeleton/jobattrs.geas`, and
  `skeleton/blob.geas`, with `test-attrs` and `test-bytes` holding the new
  host surfaces

## [v0.8.1] the plain host

The host becomes a plain resident of the runtime, one shared body and its
user pointer serving every pledge and remedy it owns, future receipts kept
until the host has polled, waited, and released them, and cancellation a
request a running body can read while break waits for it to finish. The
runtime now carries its own SONAME and every compiled contract names it, so
a host links `libgeasrt` plainly and loading order stops mattering. The
workflow surface closes around an automatic break too: open transactional
episodes roll back before remedies run, only committed work earns one, and
the broken instance can park afterwards, while the spec says plainly that a
step is at least once across a host crash.

- bind a host body with `geas_pledge_bind_user`, giving each pledge or
  remedy its own user pointer held from sign through nested calls, retries,
  and remedies, so one host function can serve many bindings
- give libgeasrt the SONAME `libgeasrt.so.0`, keep `libgeasrt.so` as its
  symlink, install both, and link every contract module against the runtime
  it names, so a host loads the runtime normally and module order does not
  decide which copy answers
- add the out pointer twins `geas_string_copy_into`, `geas_map_new_into`,
  and `geas_state_value_into`, so C hosts can receive a status before they
  use a value
- keep future receipts explicit: `geas_future_poll` reads readiness without
  delivery, `geas_future_release` drops a waited or forfeited receipt, and
  `geas_pledge_fulfill_sync` releases the receipt it creates
- make an automatic break roll back every open transactional episode before
  the remedy pass, leave an uncommitted pledge owing no remedy, and park the
  resulting Broken instance with its readable partial surface
- make `geas_contract_break` wait for a running body, let that body read
  `geas_cancel_requested` and stop early, and wake retry waits on cancel
  before another attempt begins
- state the crash boundary: steps are at least once, so a host crash between
  parks can run a pending step again and its body must make that safe
- extend Python with future `poll` and `release`, and
  `Contract.cancel_requested`, over the same C ABI the binding already uses
- add the `test-bind-user`, `test-soname`, `test-future`, and `test-cancel`
  gates, covering the plain host surface from binding through cancellation

## [v0.8.0] the durable obligation

Geas becomes what its name always said it was, a language for durable
obligations: contracts that take a long time to keep, can be partly kept,
can break, can make amends, and can be parked on one machine and picked
back up on another. A contract now declares its deadlines, its ordering,
and its retries as terms, the steps that undo what landed as remedies,
and the outside events it waits on as signals, and the runtime holds all
of it on a clock the host can replace. The compiler is written in C++23,
by hand, under compiler/, the whole front end and the C emitter in one
place with the new surface built in; the runtime stays C and grows the
workflow tail beside everything it already enforced. The order
saga runs host driven end to end from C and from Python, and the course
and amends blocks, the engine driven mode and typed amendment, already
parse and check so the next release builds on a front end that knows
them.

- write the compiler in C++23 under compiler/, a hand written recursive
  descent parser over a lexer that marks each token with the line break
  before it, a checker in passes, and the C emitter, built with CMake
  through make compiler into target/bin/geas, and make compiler
  SANITIZE=ON for the ASan and UBSan build in target/bin-asan
- pin the front end with goldens: tests/run.sh drives the lex, parse,
  check, and shape suites, every reject beside the diagnostics it must
  produce, and compiler/tests/run_codegen.sh pins the emitted C
- add Duration and Time: duration literals are one token, 30s, 250ms,
  1h30m, 7d, units descending and each at most once, and both types
  cross the boundary as an Int of milliseconds while hashing and
  mangling apart from Int; the arithmetic is closed, a Duration scales by
  an Int, a Time shifts by a Duration, and Time minus Time is a Duration
- add terms: within is a deadline counted from eligibility, after is
  ordering over pledges, subcontracts, and signals with cycles refused,
  and retry carries every, backoff, cap, and on, where only lifecycle
  failures retry unless on names the variants of the pledge's own error
  sum that may
- add the term attribute, a contract's whole lifetime from sign, which
  breaks a Signed or Partial contract the way its break line would when
  it runs out and leaves a Fulfilled one alone
- add remedies: remedy NAME for PLEDGE(param) undoes a pledge that
  landed, handed its Ok value, bound like a pledge as a Geas body, a
  foreign symbol, or a host binding, and run newest latch first when the
  contract breaks, a failing remedy recorded without stopping the rest
- add the Remedying state at the end of GeasContractState, so every
  earlier number stands, read while the remedies run and followed by
  Broken
- add signals: an outside event with no body, a requirements atom when
  loose and an after target anywhere, delivered with geas_signal from a
  host or through an instance from Geas, the first delivery winning and
  its payload parking with the instance
- teach the sum declaration to wrap, or continuing the line above, and
  add the generic constructors List<T>(..), Option<T>(..),
  Result<T, E>(..), and Tuple<..>(..) beside Map<K, V>()
- grow the descriptor with the workflow tail, kinds, terms, remedies,
  and the contract term, as pure additions, and add
  geas_register_contract_sized so a module built against the older
  descriptor loads with no byte past it touched
- give the runtime a clock: geas_runtime_set_clock swaps the time and
  the sleep, every entry point applies passed deadlines first, and
  geas_instance_tick and geas_runtime_tick let a host lapse a deadline
  the moment it falls due from its own scheduler
- report the workflow on the partial surface: geas_partial_lapsed and
  geas_contract_term_lapsed tell a lapse from an Err, and
  geas_remedy_state and the geas_partial_remedy calls read how every
  remedy went
- carry the workflow across a park: the park row gains a flow column, a
  versioned blob of eligibility stamps, lapses, Ok values, signal
  payloads, and remedy progress, brought into an existing table in place,
  and a row written before it still resumes
- add the order saga: skeleton/saga.geas and skeleton/flowdesk.geas,
  driven by make test-saga from C and make test-saga-python from Python,
  with make test-terms, test-remedy, test-signal, and
  test-workflow-park under the sanitizers and tsan-workflow under
  ThreadSanitizer, all on a fake clock
- emit a typed Signal rpc per signal from emit-proto, with a helper in
  the Go and TypeScript session wrappers and REMEDYING in the state enum
  of a file whose contracts carry the workflow surface
- bind it all from Python: geas.py gains set_clock, tick, signal,
  signal_payload, lapsed, term_lapsed, remedy_state, and
  remedy_failures, written against docs/abi.md alone like the rest
- parse and check the course and amends blocks: course, await, select,
  sleep, together, and now() hold every rule, bare name pledge calls,
  course only statements, a contract driven by its course refusing
  instance calls, and amends items checked against the version that
  carries them, while build answers workflow constructs are not emitted
  yet until the engine driven mode lands
- write it down: the grammar, the spec, the tutorial's saga walk, the
  ABI's workflow surface, and a README that says what the language is
  for

## [v0.7.3] the installed toolchain

The toolchain leaves the checkout. `make install` lays geas out the way
a system expects it: the binary on PATH, libgeasrt where the linker
already searches, the headers under include/geas so the emitted C's one
include resolves with no flags, and the std modules under
share/geas/std. The compiler learns to tell the three worlds apart on
its own: an explicit `GEAS_ROOT` leads, a repository checkout is
detected and keeps every path it always used, and anywhere else cc and
ld are left to their own search paths, because an installed toolchain
should need no environment at all. On top of that sits the first
package: `geas` on the AUR, built from the tagged release tarball.

- teach the compiler the installed layout: runtime_include and
  runtime_libdir answer GEAS_ROOT first, the checkout's relative paths
  when runtime/include/geas/geas.h is present, and empty otherwise, and
  the link lines drop -I, -L, and the libdir rpath when empty so the
  system search paths carry the build
- extend the loader's probe chain: an import resolves beside the root,
  then under GEAS_HOME when set, then lib/, then
  /usr/local/share/geas, then /usr/share/geas, so std.errors finds the
  installed std the same way it finds the checkout's
- add make install and uninstall: PREFIX defaults to /usr/local,
  DESTDIR stages for packaging, and the four artifact groups land as
  bin/geas, lib/libgeasrt.so, include/geas, and share/geas/std
- pin -fPIC on the runtime shared library rule itself, so a packager's
  own CFLAGS cannot drop it out from under -shared
- make the binary agree with the release about what version it is:
  version_string answers geas 0.7.3, and the package's check refuses a
  binary that disagrees with pkgver so the two can never drift again
- cut the AUR package: packaging/aur carries the PKGBUILD the AUR copy
  is cut from, gcc is the one runtime dependency because the vendored
  SQLite rides inside libgeasrt, and dusk-lang builds it
- license the language: MIT, the LICENSE file the package installs
- write the release path down: PUBLISH.md walks the AUR flow end to
  end, from the SSH key to the pushed .SRCINFO, with room for the other
  package repositories to follow

## [v0.7.2] the walked map

The map opens to the language that owns it. A `for` over a `Map<K, V>`
binds the key, first inserted first, the same insertion order the wire,
equality, and the bridge already speak, and the body reads the value
through the map itself. `len` arrives beside it, the one builtin both
collections share: the element count of a list, the entry count of a
map. Neither feature touches the runtime, because the storage was
already the ordered pair array the semantics promised; the language
just learns to say what it always kept.

- retire the refusal in the checker: for binds a map's key type where
  it bound a list's element, and the wrong operand diagnostic now names
  both collections
- walk the store the runtime already keeps: the loop snapshots the
  entry count, strides the flat pair array by two, and re reads the
  base each pass, so a key the body inserts lands past the frozen bound
  and is not visited this walk, an updated key keeps its slot and is,
  and the rule is written in the grammar where for lives
- reserve len in callee position only: len(x) answers Int over a List
  or a Map, refuses anything else by name, and a binding named len
  stays legal because the spelling is special only where a call is
- prove it in the gauntlet, in and out: ordered_keys walks b then a
  then c because insertion beats the alphabet, sizes sums both lens,
  and merge_counts walks two maps the C host built and handed across
  the boundary, so the iteration surface holds for a map the instance
  did not build itself
- write it down where the semantics live: the grammar carries the for
  binding and the mutation law, and the spec's Map entry carries the
  walk and the shared builtin

## [v0.7.1] the whole type set

Map and tuple cross the bridge, and the refusal that guarded them
retires with its reason intact. The old build error existed because
proto's own map type carries no order and Geas pins map iteration as
insertion ordered; the answer was never to ship the unordered thing,
it was to spell the ordered thing. A Map crosses as a wrapper message
holding a repeated entry pair, key and value in one small message, so
the order a program built is the order the wire carries, and a tuple
crosses as a wrapper with one numbered item field per position. The
gauntlet, the file whose snapshot pledge was the standing refusal, is
now a golden on the surface and compiles under real protoc.

- spell the map as what it is: StringIntMap holds repeated
  StringIntMapEntry, never proto's map field, because an ordered
  language type does not become an unordered wire type quietly, and
  the wrapper works in every position a map can appear, a field, a
  list element, an argument, a oneof arm
- spell the tuple as a message: IntStringTuple carries item1 and
  item2, the value kind last in the name, the same rule that names
  BoolIntResult
- keep every existing golden byte identical: the five prior skeletons
  and the four session wrappers carry no map or tuple and emitted
  exactly what they emitted before
- prove the surface with the gauntlet: snapshot, tally, merge_counts,
  and coords put Map in return and argument position and Tuple in
  return position, lang.proto joins the goldens, and the bridge gate
  compiles it under grpc_tools protoc so the wrappers are valid
  proto3 by a real compiler's word
- tell the order story where the type set lives: docs/bridge.md and
  the bridge notes carry why the entry list is the honest spelling

## [v0.7.0] the true name

The language is Geas. A geas, in Celtic myth, is a binding obligation
laid on a person: keep it and it protects you, break it and the
consequences find you, which is not a metaphor for this language but a
description of it. Ashford was a placeholder that never earned its
keep; the rename lands now, before anything is published, so nothing
downstream ever carries the old name. Every surface renames in one
sweep and every gate in the repository re proves itself under the new
one.

- rename the whole surface: the compiler is geas, sources end in .geas, the runtime is libgeasrt.so, the headers live under geas/ as geas.h, geas_abi.h, geas_wire.h, and geas_store.h, the C prefix is geas_ and GEAS_, the mangled prefix is __geas_, the park table is geas_park, and GEAS_ROOT replaces ASH_ROOT
- rename the standard library to what it is: lib/std, imported as std.store and the rest, because the language's own name does not belong in its import paths
- rename the hosts with the runtime: the Python binding is geas.py with Geas typed names, the Go module is geasbridge, and the bridge, supervisor, and demo hosts follow
- keep the changelog's past tense honest: entries before this one keep the names they shipped with, and only this entry knows both
- re prove everything: the full suite, the determinism gate, every proto golden regenerated under the new type names, the five bridge gates, and both supervisor gates, all green under the new name
- say what the name means where a reader first meets it: the README carries the geas, the pronunciation, and why the name is a description rather than a metaphor A commit message carries the
short bulleted shape of a change; this file carries the whole of it, the design
notes and the reasons a bullet has no room for. Versions are the `v` tags on the
history, one per milestone.

## [v0.6.8] the empty answer

The aggregates that have no zero learn to say so. `Store.min`,
`Store.max`, and `Store.avg` take the schema, column, and predicate
shape sum takes, the full predicate grammar included, and each answers
an Option: `Some` of the value over a non empty match and `None` over
an empty one, because an empty set has no smallest member, no largest,
and no mean, and the surface says so rather than inventing one. Sum
keeps its zero on purpose; an empty ledger owes nothing, but it has no
cheapest account, and the two answers differ because the questions do.
min and max take any scalar column, since every scalar column compares,
and avg takes a numeric one and always answers Float.

- answer the extremes without an aggregate: min and max lower onto an
  ordered limit one read of the column itself, zero rows None and one
  row the Some, so no SQL NULL is ever decoded and a String extreme
  deep copies from scratch onto the instance before the scratch frees
- answer the mean by count and coalesced sum: the count decides None,
  the coalesce guards the decode, and the divide promotes an Int
  column's total to Float, so again no NULL reaches the type check
- lower all three onto one grouped primitive: ash_store_agg_any carries
  the kind and the column beside the flat terms and group lengths every
  grouped read takes, negation eliminated and disjunction normalized on
  the same shared path
- surface the Option end to end: spread answers max minus min with the
  empty spread an honest zero the pledge chose, and midpoint hands the
  avg's Option straight through its own return type, so the bridge
  carries a None a remote caller can see
- walk both answers in the gate: spread reads 994.0 across the table
  and 0.0 over no rows, midpoint reads 270.8 over five accounts and
  None over none
- keep the surface honest in lib/ashstd/store.ash and docs/database.md:
  three rows join the operation table with the empty set spoken as
  None, and the leaves out list narrows to grouping, an offset, and
  arbitrary SQL

## [v0.6.7] the negated predicate

The predicate learns not, and the database never hears about it. A `!`
may wrap any predicate subtree in any of the three read forms, and the
compiler eliminates every one before lowering: De Morgan swaps the
joiners on the way down, the comparison flips at the leaf, `!(balance <
lo)` becoming `balance >= lo`, and a doubled negation cancels. What runs
is always the same or of and groups the surface already had, so this
milestone adds no runtime primitive and changes no runtime line, and the
elimination even picks the cheaper primitive: the complement of an or of
tails is a plain conjunction band, and it lowers onto the ordered where
call, not the grouped one. The flips are sound in the store's
own terms: every row the language writes carries every column, and a
NULL that reaches a table another way fails a comparison and its flip
alike, so the rewrite never changes which rows answer.

- admit not where the predicate lives: the shared checker recurses
  through the unary, so ! wraps a comparison, a group, or the whole
  tree, and a non predicate under ! still speaks the operand's own
  refusal
- eliminate with a pending flag, not a rebuilt tree: the normalizer
  toggles a negation flag down the walk, mints a node only on the
  negated path, and an untouched subtree answers its original node, so
  the no negation path lowers byte for byte down to temp numbering
- prove the elimination by the primitive it picks: mid_owners asks for
  the band extremes excludes and compiles to a single conjunction group
  on the plain ordered call, and others counts a negated equality on the
  plain where call, neither touching the grouped primitives
- walk the complement in the gate: the middle band answers the three
  owners the tails exclude, the full band answers all five including
  the injection owner string, and others counts three around ada and
  five around nobody
- keep the surface honest in lib/ashstd/store.ash and docs/database.md:
  negation joins the one predicate grammar with its elimination story
  told once, and the leaves out list drops negation

## [v0.6.6] the one predicate

One predicate grammar, everywhere. `Store.count` and `Store.sum` now
take the same comparisons, the same `&&`, and the same `||` that
`Store.query` takes, so the conjunction only rule and its refusal are
gone rather than documented. The same compile time normalization
applies, or of and groups decided before the program runs, and a pure
conjunction keeps its existing lowering byte for byte down to the temp
numbering. The Ledger's notable stops counting rows in the pledge and
asks the store the mixed question directly, and tail_total sums both
tails of the balances in one or, the sum twin of extremes.

- retire the refusal, not just relax it: check_query_pred loses its
  allow_or switch entirely, all six call sites read the same, the "not
  ||" diagnostic and its dead branch are gone, and the fallback message
  says joined with && and ||
- lower the aggregates onto grouped primitives: ash_store_count_any and
  ash_store_sum_any take the flat terms and the group lengths select
  takes, share the grouped WHERE emission with it through one helper,
  and answer through the same wrapped Ok every read form answers
- keep what compiled compiled: a pure conjunction count or sum emits
  the exact call it emitted before, proven against the emitted C down
  to temp numbering, so no shipped aggregate moves primitives
- ask the store the whole question in the skeleton: notable counts its
  mixed group, an owner conjunction or a vip floor, with no fold left,
  and tail_total answers 1004.0 across both tails and the column's zero
  over no rows, both walked in the store gate
- keep the surface honest in lib/ashstd/store.ash and docs/database.md:
  one predicate grammar is stated once for all three read forms, and
  the leaves out list drops disjunction under the aggregates

## [v0.6.5] the either predicate

The predicate learns or. A `Store.query` predicate now joins comparisons
with `&&` and `||`, `&&` binding tighter as everywhere in the language,
and the compiler normalizes the whole tree into an or of and groups
before the program runs: the terms flatten in order, the group lengths
ride beside them, and one prepared statement carries every group
parenthesized with every value still a bound parameter. A pure
conjunction keeps its existing lowering byte for byte, proven against
the emitted C, so nothing already compiled moves. `Store.count` and
`Store.sum` keep their conjunction only rule and their exact refusal for
now; the aggregates learn or when their groups do.

- let or into the checker where and lives: check_query_pred carries an
  allow_or switch, the three query forms allow it, count and sum refuse
  it with the same words they always used, and every other predicate
  diagnostic holds unchanged
- normalize at compile time: disjunction distributes over conjunction
  into or of and groups in codegen, a shared comparison re lowered per
  group because a pure expression re emitted is simpler than a temp
  shared across groups
- lower onto one runtime primitive: ash_store_select takes the flat
  terms, the group lengths, an optional order, and an optional bound
  that still requires the order, builds every group parenthesized from
  the schema descriptor, and answers rows on the instance like every
  read
- walk the tails and the mix in the gate: extremes reads both ends of
  the balances in one ordered or, folding the injection owner string
  through a group untouched, and notable counts a mixed group, an owner
  conjunction or a vip floor, with a row satisfying both disjuncts
  counted once because sql or is row membership, not addition
- keep the surface honest in lib/ashstd/store.ash and docs/database.md:
  the predicate reads comparisons joined with && and ||, count and sum
  say conjunction only, and the leaves out list narrows to negation,
  disjunction under the aggregates, and the rest it names

## [v0.6.4] the summed read

The store answers how much. `Store.sum(S, column, predicate)` sums one
numeric column over the matching rows where the rows live, an Int column
answering `Result<Int, StoreError>` and a Float column
`Result<Float, StoreError>`, any other column type refused at compile
time by name. A sum over no rows is the zero of the column's own type,
never a missing value: an empty ledger owes nothing rather than owing a
question, and the runtime spells that COALESCE so the surface never
meets a NULL. The Ledger's owned_total keeps its signature and its gate
values and loses its fold; owned_above keeps its fold on purpose, the
row walking proof standing beside the aggregate.

- check the sum where the schema lives: exactly a schema, a bare column
  name, and a predicate, the column resolved at compile time and refused
  by name when it is not Int or Float, the predicate on the same checker
  path as every query form
- lower onto one runtime primitive: ash_store_sum_where builds
  COALESCE(SUM(column), zero) from the schema descriptor with the zero
  spelled in the column's own type, binds every predicate value as a
  parameter, decodes the scalar through the scratch allocator freed
  before the call returns, and answers the wrapped Ok every read form
  answers
- let the backend keep its own honesty: SQLite reports integer overflow
  on an Int sum as a step error and it surfaces as the store failure
  status, no special case and no silent wrap
- retire the fold without moving the surface: owned_total sums through
  the store with the same Result<Float, LedgerError> and the same gate
  values, the empty owner still an Ok of zero, now the column's zero
- keep the surface honest in lib/ashstd/store.ash and docs/database.md:
  sum joins the operation table, and the leaves out list narrows to
  average, min, max, and grouping

## [v0.6.3] the counted read

The store answers how many. `Store.count(S, predicate)` takes the same
predicate the query forms take and answers `Result<Int, StoreError>`,
counted where the rows live: SELECT COUNT(*) rides the same prepared
statement discipline, every value a bound parameter, and no row is ever
materialized, so a pledge that only needs the number never pays for the
list. The two counts the codebase already had stop scanning: the Ledger's
rich and the supervisor's crashes keep their signatures and their gate
values and lose their folds, which is the point of an aggregate, the
answer moves to the store and the pledge keeps only the question.

- check the count where the query lives: Store.count takes exactly a
  schema and a predicate, any other arity refused by name, and the
  predicate walks the same checker path as query, so every predicate
  diagnostic holds unchanged
- lower onto one runtime primitive: ash_store_count_where builds the
  count from the schema descriptor and the fixed operator table, decodes
  the single scalar through a scratch allocator freed before the call
  returns, and answers the Ok(Int) Result every read form answers, with
  nothing left on the instance heap but the boxed count
- retire the folds without moving the surface: rich counts through the
  store with the same Result<Int, LedgerError> it always answered, and
  the supervisor's crashes counts its unclean runs the same way, the
  restart budget now the store's own arithmetic
- keep the surface honest in lib/ashstd/store.ash and docs/database.md:
  count joins the operation table with its no materialization promise,
  and the leaves out list narrows to the aggregates it names, sum,
  average, min, max, and grouping

## [v0.6.2] the watched service

The supervisor answers questions from outside its process. With
--grpc PORT it serves a small read only observer surface, two rpcs of its
own and deliberately not the emitted contract surface, because an
observer must not be able to sign, fulfill, or break anything. What
crosses is the diagnosis the contract already keeps: the state name, the
partial name lists, and the crash count off the Runs table, which is the
argument the example exists to make, that a contract's lifecycle is worth
watching because it is the service's own truth and not a status flag the
host remembered to set.

- serve the host's surface, not the contract's: observer.proto declares
  ListServices and GetService, the supervisor serves them beside its poll
  loop when asked and not a line of grpc otherwise, and the emitted
  bridge surface stays unserved on purpose, with the reason written where
  the flag is
- keep the loop honest under readers: one lock guards the service tables
  and only the tables, taken around snapshots and mutations and never
  around a fulfillment or a bind callback, so a remote read can never
  deadlock the runtime against its own host
- read the diagnosis, not a cache: every row is the contract's answer at
  the moment of the call, state_name off the instance, the partial names
  off the partial surface, and the crash count through the same short
  lived observer instance the restart budget uses
- watch it from the outside: watch.py lists the services and details one,
  one line per service and three name lists, and an unknown name is
  NOT_FOUND with a nonzero exit
- gate the whole read: test-supervisor-watch stands two services up, one
  already through a crash and a restart, asserts the list and the detail
  from another process, including the pending crashes pledge the partial
  surface honestly names, and skips clean without grpcio

## [v0.6.1] the bounded read

The query surface takes a bound. A trailing `limit(count)` after the
order caps the ordered form of `Store.query` at `count` rows: the count
is an ordinary Int expression, a literal or a value the pledge computed,
bound into the prepared statement as the last positional parameter like
every other value. A bound without an order is refused at compile time
with its reason in the diagnostic, because the rows a bound cuts are
defined only once an order says which come first; a negative count is
refused at runtime rather than read the way SQLite would read it, as no
bound at all. Disjunction, negation, aggregation, and an offset stay on
the leaves out list with their names on them.

- check the bound where the order lives: limit is a reserved spelling in
  the fourth position and nowhere else, a fourth argument that is not
  limit(count), a count that is not an Int, and a limit in the order's
  place each get a named diagnostic, and the two, three, and ordered
  forms stay byte for byte what they were
- lower onto one runtime primitive: ash_store_query_page appends LIMIT as
  the parameter after the terms, refuses a null, non Int, or negative
  count before the mutex, and never concatenates the count into the text
- prove the count is a value: top_owner bounds with a literal one and
  top_owners bounds with its own Int parameter, the same emitted call
  carrying a temp in one and the argument slot in the other, both walked
  in the store gate against a zero bound, a tight bound, and a bound
  wider than the table
- keep the surface honest in lib/ashstd/store.ash and docs/database.md:
  the bounded form joins the operation table, and the leaves out list
  narrows to what one bounded ordered conjunction cannot spell

## [v0.6.0] the standing service

The first whole program. examples/supervisor is a process supervisor whose
service state machine is a contract instance, one instance to one run, and
the four states carry their operational meaning directly: Signed is
starting, Partial is running, Fulfilled is a clean exit, Broken is a
crash. Nothing in it is new machinery; it is the lifecycle, the abstract
pledges, the store, the query surface, and park and resume, composed into
a tool someone would actually run, which is what a flagship example is
for.

- write the state machine as policy: start and ready are abstract and the
  host binds the spawn and the first health pass, finish records the run
  on the store and latches Err on an unclean exit so the contract breaks
  itself, and the requirements block says Partial is a running service in
  three lines
- count crashes with the query surface: the Runs table is shared across
  every instance on one dsn, crashes() reads the unclean runs with one
  predicate, service == name && clean == false, the name straight from
  the vow, and the restart budget is the store's answer, not a counter in
  the host
- respect the lifecycle from the host: a Broken run refuses further
  fulfillments, so the crash count is read through a short lived observer
  instance that is broken as soon as it has answered, and the rule that
  broke the run holds for the reader too
- make the supervisor restart a park: SIGTERM parks every running
  instance under its service name and leaves the processes standing,
  --resume stands the instances back up and reattaches by pidfile, and
  SIGUSR1 is the operator's clean stop, finish(0, clean) into Fulfilled
- gate the whole story: test-supervisor drives two services, a flaky one
  into its crash budget and a steady one through park, supervisor
  restart, resume under the same pid, and a clean Fulfilled stop

## [v0.5.4] the ordered read

The query surface answers in order. A trailing `asc(column)` or
`desc(column)` on the predicate form of `Store.query` orders the matching
rows by one column before they come back: the column is a bare name the
checker resolves against the schema at compile time, `asc` and `desc` are
reserved spellings in that position and nowhere else, and the order rides
the same prepared statement as the predicate, one ORDER BY on the select
the schema descriptor already builds. An empty set comes back ordered the
same as ever, an Ok of an empty list. Disjunction, negation, aggregation,
and a row limit stay on the leaves out list with their names on them.

- check the order where the schema lives: the third argument dispatches on
  asc or desc by name, a call that is neither, an argument that is not a
  bare column name, and a column the schema does not carry are each
  refused with a named diagnostic, and the equality and predicate forms
  stay byte for byte what they were
- lower onto one runtime primitive: ash_store_query_ordered takes the
  terms of the predicate plus a column index and a direction, appends
  ORDER BY with the name from the compiler owned schema descriptor and the
  direction from a two word table, and binds every value as a parameter,
  never concatenated
- walk both directions in the gate: owners_asc and owners_desc fold the
  owner names over the ordered rows, proven against a table whose balances
  are all distinct so the order is total, and an empty match answers the
  empty string
- keep the surface honest in lib/ashstd/store.ash and docs/database.md:
  the ordered form joins the operation table, asc and desc are named as
  reserved in the third position, and the leaves out list narrows to what
  one ordered conjunction cannot spell

## [v0.5.3] the composed predicate

The query surface composes. `Store.query(S, predicate)` takes comparisons,
`==`, `!=`, `<`, `<=`, `>`, `>=`, joined with `&&`, beside the column and
value equality form v0.5.1 shipped: the left side of every comparison is a
bare column name the checker resolves against the schema at compile time,
never a variable, the right side is an ordinary expression checked against
that column's declared type, and the whole predicate lowers onto one
prepared statement with every value positionally bound. No rows stays an
Ok of an empty list. Ordering, disjunction, negation, and aggregation stay
on the leaves out list with their names on them.

- check the predicate where the schema lives: the checker walks the `&&`
  chain, refuses `||` by name, refuses a left side that is not a column
  name, an unknown column, and a value whose type disagrees with the
  column, each with a named diagnostic, and the three argument equality
  form stays byte for byte what it was
- lower onto one runtime primitive: ash_store_query_where takes an array
  of terms, column index, comparison, and value, builds the select from
  the compiler owned schema descriptor with the operator text from a fixed
  table, and binds every value as a parameter, never concatenated
- prove the left side is a column: the Ledger's owned_above pledge spells
  `owner == owner && balance >= min`, the left owner the schema's column
  and the right owner the pledge's parameter, and the emitted call carries
  the resolved column index beside the bound argument
- walk both shapes in the gate: rich counts rows over one comparison,
  owned_above folds balances over two, both proven in the store gate
  against rows above, below, and on the boundary
- keep the surface honest in lib/ashstd/store.ash and docs/database.md:
  the operation table carries the predicate form beside the equality form,
  and the leaves out list narrows to what conjunction cannot spell

## [v0.5.2] the claimed row

Two servers, one park store, one owner per token. v0.4.0 proved a parked
session outlives its server; this milestone proves it outlives its server
while another replica is already standing, and settles who owns a token
when both reach for it at once. The delete is the claim: Resume stands the
instance up and then deletes its row, the delete reports how many rows it
removed, and only the replica whose delete removed one keeps the instance.
The loser breaks the copy it built and answers NOT_FOUND, the same answer
a spent token earns, because a token another replica claimed is spent from
where the loser stands. No lease, no fencing, no registry: the store file
is the only thing replicas share, and the delete is the only word they
exchange.

- make the delete the cross process claim in the bridge server: Resume
  keeps its order, the runtime resume first and the delete second, so an
  unknown token stays NOT_FOUND and a lying hash stays ABORTED before the
  store is ever touched, and the rowcount now arbitrates between replicas
  that both stood a copy up
- pay the loser's cost honestly: a lost race is one wasted stand up, the
  instance built, broken, and never served, bounded to the narrow window
  where two replicas resume one token at the same moment
- race it for real in the client: --race-resume fires Resume at two
  servers behind one barrier, demands exactly one signed session and one
  NOT_FOUND, and finishes the parked walk on the winner
- gate the whole claim: test-grpc-failover stands two replicas on one
  store, parks on the first, kills it with SIGKILL, resumes on the second,
  then parks again, restarts the dead one, and races both for the token
- write the replica story down where it lives: docs/bridge.md carries the
  claim beside park and resume, interop/grpc/NOTES.md carries the step 3b
  findings, and the README names the failover gate

## [v0.5.1] the bound predicate

The store surface reads sets. `Store.query(S, column, value)` answers every
row whose column equals the value as `Result<List<Row>, StoreError>`, the
column a bare name the checker resolves against the schema at compile time,
the value checked against that column's declared type, and no rows an Ok of
an empty list, never an error. One column, one equality, on purpose: the
predicate a single equality cannot spell is composed in the pledge, and
comparison, conjunction, and ordering stay on the leaves out list with
their names on them.

- resolve the column at compile time: the checker matches the bare name
  against the schema's fields and refuses an unknown column, a non name in
  column position, and a value whose type disagrees, each with a named
  diagnostic; the resolved index rides into the emitted call, so no column
  name is ever looked up at runtime
- lower onto one runtime primitive: ash_store_query_eq builds the select
  from the compiler owned schema descriptor with the value always bound as
  a parameter, never concatenated, and materializes the rows as the
  instance owned list of records every read already answers
- walk the result in the language: the Ledger's owned_total pledge queries
  by owner and folds the balances over the returned rows, record field
  reads on a queried list, proven in the store gate against accounts that
  share an owner and a stranger who owns nothing
- keep the surface honest in lib/ashstd/store.ash and docs/database.md: the
  operation table carries query beside find, and the leaves out list
  narrows to what equality cannot spell

## [v0.5.0] the one wire

Ashford keeps no wire of its own. Inside one process the languages share
memory through libashrt, and across processes they share gRPC, which every
language, proxy, and load balancer already speaks: the compiler emits the
surface, ordinary host code serves it, and stock tooling builds the
clients. The runtime's own TCP transport, the daemon, and the mesh serve
and connect surface retire, and the cross process story is the bridge,
whole: the streaming session, the typed pledge rpcs, the value Err beside
the status code, the pinned shape hash, and the park store that lets a
session survive its stream, its client, and its server.

- retire the transport: mesh.c, ashd.c, net.c, and the internal net headers
  leave the tree, and runtime.c drops its remote proxy path whole, the
  connection struct, the proxy fields on the instance, the remote branches
  in sign, state, break, fulfill, the partial reads, the vow read, and the
  park guard, 1383 lines gone with the threading and lock order untouched
- retire the serve and connect API: AshServer, ash_runtime_connect,
  ash_runtime_connect_all, ash_runtime_serve, and ash_server_stop leave
  ash.h, and the exported surface of libashrt drops from 106 symbols to 64
  with no local host call removed
- hold the ABI lines that are pinned: the AshStatus numbers stand,
  ASH_ERR_NET keeping its slot as a reserved status, and AshRuntimeConfig
  keeps its layout, handshake_ms a reserved field a host leaves zero
- keep the canonical value encoding: wire.c and its golden gate stay, the
  park row's format and the proof it round trips exactly
- retire the network and mesh gates, harnesses, skeletons, and the Python
  connect and serve surface with the transport they drove; the default
  suite tightens to the surfaces the language ships and runs in a third of
  the time
- write the bridge down as the normative cross process surface in
  docs/bridge.md: the emitted artifacts, the session as a stream, business
  beside transport with the status table, the type set, park and resume,
  and what the bridge does not do; the README, the spec, the tutorial, and
  the ABI doc tell the two role story straight

### Notes

The two roles were always the design: a local glue that puts many languages
in one address space with no serialization, and a bridge that puts many
processes on the wire the world already runs. A transport of Ashford's own
sat between those roles and belonged to neither, and every question it
answered, sessions, liveness, affinity, partition, the bridge now answers
better, on infrastructure nobody has to trust twice. What the transport
proved stays proven and stays in the tree where it still earns its keep:
the canonical value encoding is the park format, discovery by mangled name
and shape hash is how every host finds a contract, and the token posture
lives on in the bridge's own policy seam. The pitch is one sentence now:
Ashford is how the languages in one program keep promises to each other,
and how those programs keep the same promises to each other over gRPC.

## [v0.4.0] the session that survives

The bridge spends the parked instance. A park enabled server writes a
session down when its stream ends and stands it back up when the client
returns, so a contract now outlives its connection, its client, and the
server process itself. The gate kills the server between the park and the
resume, and the latch set before the death answers the walk that finishes
after it: the stream's partition trade from step 1b is paid back, and
replica affinity has its answer, any server holding the park store can
resume the session.

- issue a park token beside every signature on a park enabled server,
  Signed gaining park_token, empty when parking is off so a server without
  the flag is byte for byte the step 1b server
- park on the stream's end, not the contract's: a termination without an
  explicit Break parks the instance under its token before the break that
  drops the row, while an explicit Break stays the one true ending, the
  runtime's refusal to park an ended instance taken as the answer it is
- add the Resume rpc: the token stands the instance back up under a fresh
  id on a new stream, one shot, the parked row deleted on success, a spent
  or unknown token NOT_FOUND, a lying hash ABORTED before the row is
  touched, and a resumed session that later drops parks again under the
  same token
- keep the one shot rule bridge policy, not runtime law: the runtime
  writes and reads park rows, and the server's own sqlite delete decides
  who may resume a token and how often
- carry the surface out of the compiler: emit-proto adds park_token to
  Signed, the ResumeRequest message, the Resume rpc beside Session, Resume
  to the reserved rpc spellings, and a resume opener in both the Go and
  TypeScript wrappers, the same handle open answers
- expose park and resume in the Python binding, the ctypes twins of the
  language's own spellings
- gate the whole claim in one file: test-grpc-resume signs on a park
  enabled server, latches a pledge, drops the stream, kills the server,
  stands a fresh one up on the same park store, resumes by token, and
  finishes the contract on the latch that crossed the death, beside the
  old gates proving a parkless server unchanged and the Go and Node walks
  green on the widened surface

### Notes

The design sentence from step 1b closes its loop here: the stream is how a
client says still mine and done, and the park store is how a contract
survives everything the stream cannot. The two roles never blur, because
the park happens exactly where the break used to and the resume is a new
stream taking ownership, so there is still never a moment when an instance
lives without an owner: the owner is the stream, or the row, never neither,
never both. What step 1b called the remaining quarter is now spent in
full: correct lifetimes came from the stream, and affinity and partition
tolerance came from the store, each from the piece of the system whose job
it was.

## [v0.3.9] the instance surface

The lifecycle reads and writes from inside the language. An instance
answers its state, hands back its locked vows, parks itself, and stands
back up, all in the surface syntax, so the walk a C host drives through
five ABI calls is a pledge body's dozen lines. The compiled thunk and the
foreign host now hold the same instrument panel.

- add `instance.status()`: the state's canonical spelling as a String,
  Unsigned, Signed, Fulfilled, Partial, or Broken, borrowed from static
  storage so nothing allocates and nothing frees
- add the vow read through an instance: `instance.vow_name` types as the
  vow's declared type and answers an independent deep copy, so the value
  survives the callee's break the way value semantics promise; a vow is
  read, never called, and calling one says so
- add `instance.park(dsn, key)`: the durable state into the store row from
  inside the language, Unit on success, the C surface's own refusals riding
  the thunk's exit convention, a dead dsn as ASH_ERR_STORE, a walk in the
  air or an open episode as ASH_ERR_STATE
- add `Contract.resume(dsn, key)`: the parked row stood back up on the
  running runtime, the signed instance the answer the way sign's is, an
  unparked key as ASH_ERR_NAME and a skew as ASH_ERR_VERSION; an internal
  contract refuses resume exactly as it refuses sign
- reserve the lifecycle member spellings: sign, resume, break, status,
  park, and partial name the builtins on an instance, and the checker walks
  a pledge under one of those names past them never
- carry the surface on four ABI additions: ash_state_name and
  ash_state_value for the canonical spellings, and the _v forms of park and
  resume taking their dsn and key as String values, the shape a compiled
  body holds, so the emitted C needs no string handling of its own
- add instance.partial(): the whole partial surface as one value, the
  builtin record PartialResult every program carries without declaring it,
  state as the canonical String and fulfilled, pending, and broken as
  List<String> in the runtime's insertion order, its fields reading like
  any record's; the Err payloads stay on the C surface where
  ash_partial_error hands them over, because their types vary per pledge
  and a record field carries one
- inject the builtin record in the loader, after the merge and before the
  resolver, so every later pass treats PartialResult as an ordinary
  declared record with no special case, and a program declaring its own
  collides the way any duplicate does
- build the record in the runtime through ash_partial_value(c, owner,
  out), one consistent snapshot under the instance lock, the state
  borrowing its static spelling and every name copied onto owner so the
  record outlives the instance it read
- gate it in the default suite: the lifecycle skeleton walks sign,
  status(), the vow read, park, break, resume, and the partial panel
  entirely inside the language and answers the stations as strings the
  host asserts, plus the two fault paths, under ASan and LSan
- spell the surface in docs/grammar.md beside sign and break, and the ABI
  additions in docs/abi.md beside the state enum

### Notes

status() answers a String rather than a declared sum on purpose: the five
spellings are stable, they match what every log and every bridge already
prints, and a match over them reads plainly. PartialResult is the
language's first builtin record, and it arrives as a declaration the
loader writes rather than a type the checker special cases, which is why
it cost a page and not a subsystem: resolve, the type checker, codegen,
and even the gRPC emitter handle it exactly as they would a record the
program declared. The park and resume spellings complete the thought
v0.3.8 started: the primitive lived in the ABI, and now the language that
owns the contracts owns their persistence too, so a pledge body can hand
its own instance to tomorrow, and read the whole panel while it decides.

## [v0.3.8] the instance written down

A signature outlives its process. `ash_instance_park` writes an instance's
durable state into one store row, and `ash_instance_resume` stands it back
up against a fresh runtime: the vows, the latches, the Err payloads, and
the transactional fates all cross, so a partial walk resumes where it
stood, a broken record resumes readable, and a store backed contract
reopens its own dsn and reads what it committed. This is the piece the
bridge notes kept for Layer 3: an instance that can be written down is the
answer to affinity, to partition tolerance, and to the orphan as scheduled
work, one primitive under all three.

- write the park row into the runtime's own ash_park table in whatever
  database the caller names, created on first use, one INSERT OR REPLACE
  per key, so a key parked twice holds the later instance
- encode the vow values and the Err payloads with the wire codec's
  canonical form, the same bytes the network trusts, so the park format
  inherits the codec's goldens and a parked value round trips exactly
- replay on resume exactly as sign builds: the contract found by the
  recorded name, the dispatch resolved fresh, the vows decoded onto the new
  instance and checked against their declared types, the latches byte for
  byte, each present payload decoded onto the instance heap the partial
  surface expects, and a store backed contract reopening its dsn vow and
  reconciling its schemas in the same call
- hold the skew line: the recorded version and shape hash must match the
  registered module's, and a nonzero expected_hash must agree, else
  ASH_ERR_VERSION, sign's own rule; an unparked key or an unregistered
  contract is ASH_ERR_NAME; a row that will not decode against the
  descriptor is ASH_ERR_TYPE
- park between walks only: an unwaited future, an open transactional
  episode, a remote proxy, an unsigned instance, and an instance the caller
  already ended with an explicit break each refuse ASH_ERR_STATE, while an
  automatic break parks with its errors readable, which is what an
  automatic break keeps its heap for
- keep a resumed episode closed: TXN_DONE replays as done, so a committed
  transactional subcontract can never run twice however many times the
  instance crosses the table, and a recorded TXN_OPEN is corruption by
  construction, a row park refuses to write
- gate it in the default suite: test-park drives the payment round trip
  through two runtimes to fulfilled, the broken record with its payload,
  the future and transaction refusals, the lying hash, the bare runtime,
  and the Ledger resuming onto its own database, all under ASan and LSan
  to zero leaks
- pin the model in docs/database.md: the parked instance section beside the
  lifecycle it extends

### Notes

The park's one design sentence: a park is a state between walks, never a
snapshot of one mid flight. Everything the row cannot carry, a buffered
transaction, a future in the air, a connection, is refused at park rather
than approximated at resume, so a resumed instance is exactly a signed one
with its latches set and nothing else pretending. The explicit break stays
the one true ending: it reclaims the heap the vows and payloads live on,
so there is nothing left to write down and park says ASH_ERR_STATE, while
the automatic break, whose whole purpose is keeping the errors readable,
parks them readable. The bridge inherits the next step, a Session that
parks on disconnect and resumes on reattach, and the mesh inherits the one
after, an instance that moves between nodes; both are consumers of this
primitive, not new ones.

## [v0.3.7] the second wrapper

The session wrapper claim, that another language's is a morning rather than
a milestone, is now a shipped fact. `ashc emit-proto` writes a TypeScript
session wrapper beside the Go one, and a Node client built from nothing but
the emitted artifacts walks the whole payment lifecycle against the bridge
server. The two consumers make opposite tooling choices on purpose: Go runs
protoc and compiles stubs, Node loads the emitted `.proto` at runtime
through `@grpc/proto-loader` and takes its session surface from the
wrapper's structural interfaces, no protoc step, no generated stub, no
native code, the marketplace-safe shape an editor extension ships.

- emit `<stem>_session.ts` as emit-proto's third artifact: the pinned shape
  hash as a bigint, structural interfaces for the sign surface and the
  signature with the vows typed by their loader spellings, the session
  handle, and the opener that reads the signature off the stream before the
  promise settles
- keep the syntax inside the erasable set, so node runs the wrapper
  directly under type stripping with no compiler in the loop
- import nothing in the wrapper: the one surface it needs from grpc-js, the
  stream a session opens, is spelled structurally
- spell every 64 bit integer as a decimal string and every field in the
  wire's own snake_case, the loader under keepCase, longs as String, and
  oneofs on, with the oneof discriminator carrying which arm a result took
- name the interfaces with the contract's name always, wrapper local and
  independent of the proto prefix rule, so a file of many contracts stays
  unambiguous
- stand the Node consumer up in interop/grpc/node: two npm dependencies,
  the emitted pair under gen, and a client that asserts the Go walk check
  for check, the pinned hash at sign, the value Err as err=41 on an OK rpc,
  the automatic break keeping its payload and the explicit break zeroing
  it, a lying hash refused as ABORTED, a killed child's instance collected
  in milliseconds, and a quiet session resuming on latches set before the
  silence
- gate it beside the others: test-grpc-node runs the same server under the
  Node client, out of the all gate with the other gRPC gates and skipping
  clean where node, npm, or grpcio is absent, and test-proto pins the two
  TypeScript goldens beside the Go and proto ones

### Notes

The wrapper is the counterexample to its own tooling: where the Go path is
emit, generate, compile, link, the Node path is emit and run. Both end at
the same server making the same promises, which is the point of a surface
out of the compiler. The editor middleware assessment in the usability
notes now has its VSCode half proven rather than argued, an extension's
whole diet being two npm packages and two emitted files.

## [v0.3.6] the bridged type set

The declared types cross the bridge. emit-proto covered the scalar walk;
now a contract's records, sums, Unit, Option, and List arrive on the wire
under their own definitions, every skeleton and the whole standard library
emit and pass protoc, and the goldens pin one example of each shape.

- lower a declared record to a message, its fields in declaration order,
  emitted once per file under its own name
- lower a sum by its arms: every arm bare is an enum numbered in declared
  arm order, the ABI's own tag space, its values prefixed with the sum's
  name for proto's shared value namespace; an arm with fields makes the sum
  a message whose oneof carries one message per arm, the payload messages
  first, the oneof numbered in the same arm order
- lower Unit to the empty message, the Ok of a pledge that answers with
  nothing
- lower Option and List by position: repeated and optional in a field, a
  wrapper message on a oneof arm, where proto allows no label; an Option
  over a message tracks presence intrinsically and over a scalar or an enum
  takes the optional label
- take a bare Option as a pledge's return, the shape the stdlib's min_of
  and index_of declare, crossing as the wrapper directly
- reserve two spellings: a pledge named sign keeps its rpc and moves its
  request message alone to SignPledgeRequest, stepping aside from the
  session surface; a pledge whose rpc spelling would take Session,
  GetPartial, or Break is refused by name
- keep Map and tuple as named refusals that write nothing, the gauntlet's
  snapshot pledge standing as the proof in the gate
- name the refused type in the error, the checker's own spelling, so the
  refusal reads as the type it refused rather than a category
- widen the proto gate: goldens for hello's enum Err, ledger's Unit and
  bare sum, main_demo's payload sum and repeated parameter under two
  services, and std_user's four contracts exercising the prefix rule, the
  wrappers, and the reserved spelling, with the multi-contract session
  wrapper pinned beside the payment pair

### Notes

The emitter's dedupe set owns the heap strings it holds; the message pass
borrows the name it gets back and must not free it, which is the kind of
sentence a garbage collected language never writes. The service
pass names shapes through a pure spelling function and emits nothing, so a
refusal is recorded exactly once, in the message pass, and the emitted
service block stays well formed even on a path the caller never writes. The
one type left outside the set with a real user is Map, the gauntlet's
snapshot; proto's own map type carries no deterministic order, and Ashford
pins map iteration as insertion ordered, so a faithful lowering is a
repeated entry message and a decision about whether order survives the
wire, which is Appendix III's map-ordering question and stays open on
purpose.

## [v0.3.5] the emitted surface

The gRPC bridge surface comes out of the compiler. `ashc emit-proto` runs the
same front end as `build` and writes two artifacts a consumer in any gRPC
language builds against with stock tooling: the `.proto` spelling every public
contract as its own typed service, and the Go session wrapper carrying the one
thing protoc cannot write, the stream whose lifetime is the instance's. A Go
client built from nothing but those artifacts walks the whole payment
lifecycle against the Python bridge server, and the suite pins both files as
goldens beside the module C and the header.

- add `ashc emit-proto <file>`: one front end pass, two files under
  `target/ashc-out`, `<stem>.proto` and `<stem>_session.go`, both byte stable
  for a given source
- spell the `.proto` as the streaming session surface: one typed rpc per
  pledge in declaration order, `Session(SignRequest) returns (stream
  SessionEvent)` with the signature as the first event, vows as optional
  fields with the declared-default note, each distinct `Result` as a oneof
  message emitted once, the partial surface as repeated names in the
  runtime's insertion order, and no Debug rpc, because the instance table is
  server internal and nothing shipped exposes it
- pin the shape hash in the wrapper: `shape_string` and `fnv1a64` are the
  same calls the module and header emitters make, so the
  `PaymentServiceShapeHash` constant in the Go source and the hash the
  compiled module registers cannot disagree, and a sign that pins it proves
  the emitted surface and the loaded module describe one contract before the
  first pledge runs
- scope the bridge to the scalar set by name: a vow, a parameter, or a
  `Result` arm outside Int, Uint, Float, Bool, and String is a named build
  error, not a silent hole in the service
- prefix contract scoped messages only when a file declares more than one
  public contract, so the single contract file reads as its service and the
  shared file stays unambiguous
- stand the Go consumer up in `interop/grpc/go`: module `ashbridge`, a client
  that imports protoc's output over the emitted proto plus the emitted
  wrapper and asserts the Python walk check for check, the vow override, the
  pending order, the value Err as err=41 on an OK rpc, the automatic break
  keeping its payload and the explicit break zeroing it, a killed client's
  instance collected in milliseconds, a quiet session resuming after three
  times the old timer, and a lying shape hash refused as ABORTED, the
  bridge's spelling of `ASH_ERR_VERSION`
- gate it twice: `test-proto` diffs both emitted files against pinned goldens
  inside the default suite, and `test-grpc-go` builds the Go client and runs
  it against the bridge server, out of the all gate beside the other gRPC
  gates and skipping clean where go, protoc, or the protoc-gen-go pair is
  absent
- move the compiler onto dusk 1.5: `std.map` is generic over its key now, so
  every `Map<V>` spells `Map<string, V>`, and the map lookups whose key was a
  struct field access hoist it into a typed local the type parameter pins to;
  the Makefile and README name 1.5 as the floor

### Notes

The wire compatibility comment at the top of the hand written
`payment_bridge.proto`, "the shape ashc emits in step 2", is now enforced by
a passing gate: the Go client is generated from the emitted proto, the server
from the hand one, and every call routes because package, service, and field
numbers agree. The language coverage question closes with it. Any language
protoc speaks gets the typed surface from the emitted proto, and the session
wrapper is a per language template of about forty lines with one idea in it;
Go's is written by the compiler, another language's is a morning. What the
bridge still wants, the instance that outlives its connection, affinity, and
partition tolerance, is one piece of work and it is Layer 3's.

## [v0.3.2] the whole suite

Every layer the language carries now answers to one command. `make all` runs the
walking skeleton, the runtime's units, the compiled language, the store, the
network, and the mesh, and finishes in half a minute.

- fold the store's gates into the default suite: the driver units, the ledger, the
  transactional transfer, the failure paths, the crash durability proof, and the
  Python store host
- fold the mesh's gates in beside them: a host serving through the library call, the
  symmetric pair driving both edges at once, and the Python provider a C peer
  consumes
- keep the stress and sanitizer variants of the store, network, and mesh out of the
  default suite and run on their own, the treatment the network's own variants
  already had, since each is minutes of load over ground the functional gate beside
  it covers

### Notes

The suite is one gate per surface rather than every gate that exists, which is what
keeps it a half minute command a build can afford to run every time. The variants
left out, the thread sanitizer over the mesh pair and the network, and the stress
runs over the store, the network, and the three node mesh, are the ones that take
minutes each and are run deliberately.

## [v0.3.1] the mesh

The serve call turned symmetric, and the bridge stands: many processes, each in
any language, every node providing the contracts it implements and consuming the
contracts its peers implement. A project's languages talk through contracts the
way a program's modules talk through calls. The wire stays Layer 2's, no new frame
and no new status.

- make a node symmetric: connect gates on frozen and no server, so a serving
  runtime opens a consume edge past its own freeze while a pure client frozen with
  no server stays refused, the rule that lets two nodes each serve and then connect
  to each other; a node tracks its live servers on the runtime under its lock,
  attached when a serve binds and detached when it stops
- keep a node's offered surface exactly its own: serving once a consume edge has
  merged is `ASH_ERR_STATE`, so a node serves before it connects and never
  re-serves a remote it consumed, one hop to the owner with no relay
- make the provider side any language: add `Runtime.serve(addr, token=None)` to the
  Python binding over the serve call, an opaque server handle with an idempotent
  stop and a context manager, the token never logged; a Python node binds a Python
  charge over the abstract pledge and serves it while a C peer connects and reads
  the `Ok(true)` and `Err(41)` the live Python process computed, the callback
  running on a runtime pool worker as every bound pledge does
- wire a node to its peers in one call with `ash_runtime_connect_all`, a loop over
  connect across an address and token array returning the first failure, still by
  explicit address and with no registry
- add Calculator, a third mesh contract named apart from Greeter and
  PaymentService, its answers Int where the others are String and Bool
- pin the mesh under load: two runtimes each serving one contract and consuming the
  other's with both edges driven at once, and three nodes storming six edges where
  three answer shapes make a misrouted sign fail rather than pass, so a green storm
  is the one hop routing proof; a node dropped mid mesh lands `ASH_ERR_NET` on
  exactly the dead edges' in flight waits with the untouched edges still
  delivering, and a serving node absorbs a connect and drop loop with no leak, all
  clean under ASan, LSan, and ThreadSanitizer
- document the serve surface in the ABI reference beside connect, carry the mesh
  section and its four gates into the README, and keep the mesh design normative
  and matched to the serve call and the connect gate

### Notes

A mutual pair brings its edges up after it serves, not before: each node's connect
wants its peer already serving, so if the freeze closed the consume side too
neither could open its edge. The freeze fixes what a node offers; a consume edge
extends only the remote surface a node never re-serves, so a serving node connects
out past its own freeze and the pair bootstraps. The three node storm's proof rests
on the answer shapes: String, Bool, and Int, so a sign that resolved to the wrong
owner fails on its type rather than passing quietly.

## [v0.3.0] the serve call

Layer 4 opens by making serving a call any host can make. The accept loop the
daemon ran is now a library entry point, so a process that links the runtime is a
node: it serves the contracts it holds and connects to the contracts its peers
hold, both at once, the symmetry the bridge is made of.

- lift the server side into libashrt behind an opaque `AshServer` handle in the
  new `runtime/src/mesh.c`: the connection table, the accept loop, the per
  connection reader and waiter threads, the request dispatch for SIGN, FULFILL,
  BREAK, and PARTIAL_QUERY, and the served dump snapshot
- add `ash_runtime_serve`, which freezes the runtime if it is not, snapshots the
  dump and its hash, binds the address, starts the accept loop on a background
  thread, and returns an `AshServer` at once so the host keeps running
- add `ash_server_stop`, which shuts the listener down, wakes and joins every
  connection thread, breaks the instances they signed, and frees the handle
- give each server its own state, one table and one listener and one stop flag per
  handle guarded by the connection mutex, so two servers in one process share no
  mutable state and cannot collide
- rewrite `ashd` as a thin main over the serve call: parse args, load modules and
  the token, serve, then block on SIGINT and SIGTERM and stop from its own thread;
  the signal policy stays the daemon's, the library carries none
- declare `ash_runtime_serve`, `ash_server_stop`, and `AshServer` in the ABI header
  beside `ash_runtime_connect`, and speak the Layer 2 wire unchanged, no new frame
  and no new status
- add the test-mesh-serve gate: a plain C host serves a module through the library
  call and a client drives Greeter over loopback, no daemon binary in sight, with
  every network gate green over the lifted path and clean under ThreadSanitizer

### Notes

The daemon and an embedded server now run the exact same code, so the two cannot
drift. Serving exposes the frozen iname table, the same one a connecting peer
merges, so a node loads and binds, connects to its peers, freezes, and serves,
with the surface it offers and the surface it consumes both fixed at the one
freeze. A node serves its own registrations only and never re-serves a remote it
consumed, so a sign is one hop to the owner with no relay and no routing. The stop
flag moved from the daemon's `volatile sig_atomic_t`, a signal handler's handoff,
to an `int` under the connection mutex, a thread to thread handoff with a real
happens before.

## [v0.2.5] the store in the second language

Layer 3 closes the way every layer here closes, with a second language driving it
and the reference made normative. The store is invisible to the binding, which is
the point: a contract backed by a database signs and fulfills from Python with the
same calls a local contract does.

- give the Python binding the store layer's one status, `ASH_ERR_STORE` in the
  constant block and the status names, readable in the second language
- add the Python ledger host, the twin of the C one: it signs the compiled Ledger
  against a temp SQLite file with a dsn override, opens accounts and reads their
  balances, rewrites a row, binds an injection string the table survives, and
  drives the transactional Transfer to a commit and a rollback, asserting the
  outcomes the C hosts assert
- hold the business boundary from Python: an overdraft is `Err(Insufficient)` with
  an `ASH_OK` delivery and moves no balance, never a store status
- add test-store-python beside test-python and test-net-python, skipping clean
  where python3 is absent
- record the schema descriptor in the ABI reference, `AshSchemaCol` and
  `AshSchemaDesc` and the schema and sub_flags tail of `AshContractDesc`, with the
  transactional bit
- write the schema block and the transactional subcontract modifier into the
  normative grammar, columns the seven scalars and transactional on a store backed
  subcontract alone
- mark the store design normative, its surface the four keyed operations with a
  set of rows composed from reads
- carry the store story and its gates into the README

### Notes

A row crosses to Python as a record, the `ASH_TY_RECORD` arm the binding already
decodes, its fields the schema's columns in order. The demo reads balances through
fresh, loose, autocommit instances the way the C host does, so a committed transfer
is visible to a later sign and a rolled back one leaves the file as it was. The
store surface is four operations keyed by the primary key, find, insert, update,
and delete; a set of rows or a join is composed in a pledge from several reads, and
a bound predicate query form is a later step on the same surface.

## [v0.2.4] the store under failure

The store's one new failure proven to be exactly what the design says, and the
all or nothing claim proven to survive a crash mid transaction. A pure tests and
gates milestone: every SQLite refusal already maps to `ASH_ERR_STORE`, so this
proves the semantics rather than changing them, and no runtime code moves.

- pin the store failure paths, all `ASH_ERR_STORE` through the wait with the
  database left consistent: a read only connection refuses a loose insert and a
  transactional debit, the debit rolled back; a duplicate primary key is the
  backend's own constraint refusal, a status and not a value; a contended writer
  that loses the file to an open transaction surfaces the status at once rather
  than stalling
- write the business boundary first and red: an overdraft is `Err(Insufficient)`,
  the ledger's own rule as a value with an `ASH_OK` delivery, asserted never once
  `ASH_ERR_STORE`, the line between a store layer and an ORM that swallows domain
  logic
- prove rollback on crash, the proof the layer rests on: a forked child opens a
  transaction, buffers a debit, and is killed dead in it; the reopened file shows
  the buffered debit gone, rolled back on the next open, and a twelve round sign
  and kill loop leaves the file consistent every pass
- stress concurrency: many threads sign one instance per transfer against one
  shared file; the race loser is `ASH_ERR_STORE` and an overdraft a business
  `Err`, but every transfer is whole, so the pool total is conserved to the unit
  however the races fall, clean under ASan and LSan and race free under
  ThreadSanitizer
- keep the new gates out of the default all gate the way the other store and
  daemon gates are, run on their own

### Notes

The contended loser is deterministic and immediate: SQLite returns `SQLITE_BUSY`
without the busy handler in the reader upgrade deadlock case, so the loser is
`ASH_ERR_STORE` at once, never a silent stall past the busy window. The stress ran
into a standing runtime limit worth its own future work: an instance is tracked in
a fixed table freed only at shutdown, and a break keeps the instance struct alive
so a host can still read its partial report, so a broken instance holds its slot
for the runtime's life and a run of signs is capped rather than unbounded. The
stress is sized under the cap and a worker that reaches it stops clean, so
conservation still stands; reclaiming a broken instance's slot is an instance
lifetime redesign left for later.

## [v0.2.3] the all or nothing

The store layer's headline. A transactional subcontract runs its pledges as one
episode that commits or vanishes whole, the language's all or nothing written on
the grouping it already had.

- add the transactional modifier: a lexer keyword, a subcontract flag the parser
  records after the name and before the body, and a type error when it is written
  on a subcontract of a contract that declares no schema and so has no database to
  be durable against
- carry the flag as a byte array parallel to the subs table, appended at the tail
  of the descriptor so every earlier descriptor stays byte identical and the shape
  hash never moves
- open the episode lazily on the first fulfillment of a transactional pledge,
  commit the instant the subcontract completes, and roll back on the first Err, on
  a backend failure, or on a break before the commit
- key the commit on the same subcontract fulfilled predicate the requirements
  evaluator reads, never a parallel counter, so the commit point cannot drift from
  the subcontract's own fulfillment under partial fulfillment
- refuse a pledge whose episode already resolved with ASH_ERR_STATE through the
  wait, since a committed transaction is closed and there is nothing left to join;
  loose pledges stay autocommit, untouched, and the connection stays single
  threaded under the instance lock with no new lock
- give the Ledger a transactional Transfer subcontract and add make test-store-txn:
  a good transfer commits both writes, a failed one rolls the debit back byte
  identical, a resolved pledge re-call is a state error, and a break mid
  transaction leaves no write durable, ASan, LSan, and TSan clean
- teach the parse dump to render a schema member, closing a crash where ashc parse
  walked a schema as a requirements block and ran off the end

### Notes

The episode state is one byte per subcontract on the instance, beside the pledge
latches and under the same lock, moving from none to open on the lazy begin to done
on the one commit or rollback. Because the commit test is the evaluator's own
predicate, a transaction that spans debit and credit commits on whichever pledge
completes the pair, order independent and independent of the overall contract
state. One SQLite connection holds one transaction, so two transactional
subcontracts open at once on one instance is not attempted in v1: while one is
open, a pledge of another is refused ASH_ERR_STORE and stays unopened until the
first resolves, a sound consequence of the single connection model rather than a
corruption. The break order from the store layer's first milestone, rollback then
close then reclaim, already covers a teardown mid transaction, so nothing durable
survives a broken contract.

## [v0.2.2] the schema takes the sign

The first ashc change since Layer 1. A contract can now declare the shape of a
table it operates against, sign against a live database, and read and write rows
through a small store surface, all with the schema locked and checked the way a
vow and a shape hash are.

- add the schema block to ashc: a lexer keyword, a parser node, a row record the
  schema stands up so a pledge reads a row as an ordinary record, the seven
  scalar column rule as a named type error, and the schema folded into the
  contract's shape hash so a changed column fails the sign like a changed pledge
- lower the store surface: Store.find, insert, update, and delete name a schema
  in the same contract and lower onto runtime primitives that bind every value
  positionally and land an Ok result the instance owns
- open and reconcile at sign: a store backed contract binds its dsn vow, opens
  the connection, and reconciles every schema against the live database, create
  if absent and validate column for column if present, ASH_ERR_TYPE on
  divergence and ASH_ERR_UNBOUND with no dsn; break rolls back, closes, then
  reclaims the heap, in that order
- emit the schema descriptor as a pure tail on the contract descriptor, one cols
  array and one schemas array, so every pre store module reads as not store
  backed and nothing else in the table moves
- add the Ledger skeleton and make test-store: sign against a temp file, write a
  row, read it back, update it, prove a missing account is the contract's own
  error and an injection string is a value the table survives, both refusal
  signs, ASan and LSan clean
- fold the store driver and its sqlite into every gate that links the runtime,
  since the runtime now speaks to the store

### Notes

A schema stands up a real record, so a row a pledge reads is an ordinary record
and field access, record literals, and match all reuse the machinery already in
the language. The cost is that a schema name shares the global type namespace, so
a clash reads as a duplicate. `Store` is a compiler namespace that turns magic
only when nothing local or a contract owns the name, so a real contract named
Store keeps its sign path. The first column of a schema is its primary key, the
key find, update, and delete write against. A backend failure is the
`ASH_ERR_STORE` status delivered through the fulfillment, not an `Err` value; the
`Err(StoreError)` arm exists so the surface types and matches exhaustively, and
the primitive builds only the `Ok` arm.

Sign opens the connection on the instance before it is published to the runtime,
so no lock is held across the open and the reconcile, and a sign that fails to
open or reconcile never leaves a half open connection behind. Break holds the
instance lock every fulfillment holds, so the rollback and close never race a
store operation.

Left for the milestones above this one: `Store.query`, transactional
subcontracts, and the partial record form of `update` are S2, and the schema
descriptor structs are documented in the ABI reference at the layer's closing
docs pass. The parse and lex dumps do not yet render a schema member, so nothing
regresses but `ashc parse` omits it from its tree.

## [v0.2.1] the store surface

The first executable piece of Layer 3, and it holds no contract yet. Layer 3
starts where the wire layer started, as a library proven on its own before
anything above it can hide a bug behind it.

- add the `AshStore` vtable in `runtime/include/ash/ash_store.h`: open a
  connection, exec a statement, exec with a bound parameter frame, query rows
  back, and begin, commit, and rollback a transaction, with an `AshStoreAlloc`
  hook so the driver builds rows through a caller supplied allocator and never
  names an `AshContract`
- write the SQLite driver in `runtime/src/store.c`, the one translation unit
  that calls `sqlite3_*`: the scalar type map of `docs/database.md`, Int and
  UInt as one 64 bit integer with the UInt riding by its bit pattern, Float as
  REAL, Bool and Byte and Char as integers of their width, and String as raw
  bytes bound and read as a blob into a column declared TEXT, no UTF-8 check
- bind every parameter positionally through SQLite, never by concatenation, so
  a value carrying SQL is a value and nothing else; a composite parameter or
  column has no place in a flat row and is `ASH_ERR_TYPE` before a byte moves
- keep the driver owning no result heap: a query counts its rows, resets, and
  reads them into one backbone sized once from the hook, the same discipline
  the wire decoder kept over its owner, so there is nothing to leak and one
  break reclaims the lot
- map failure honestly, a backend that fails at what it was told is
  `ASH_ERR_STORE`, an allocation the hook refuses is `ASH_ERR_OOM`, and a
  contract's own rules are never folded into either
- vendor the SQLite amalgamation 3.46.1 under `runtime/third_party`, compiled
  into `libashrt.so` as one object with its own flags and no sanitizer, so the
  store is hermetic and versioned in tree with no system dependency
- gate it with `make test-store-unit` under the address and leak sanitizers: a
  byte golden of a queried row set, a round trip for every scalar with the UInt
  high bit and an embedded NUL both pinned, a transaction that commits and one
  that rolls back, and a negative corpus that refuses a dead dsn, a broken
  statement, a wrong parameter count, and a non scalar value without a crash
- append `ASH_ERR_STORE` to the status enum and the ABI reference ahead of its
  first contract level use, kept out of the `all` gate until the layers above
  it exist to fold it in

### Notes

The `col_names` array rides the query API for S1, which will map a schema's
columns onto a row. The record representation the ABI pins is strictly
positional and carries no names of its own, so the driver takes the array as the
seam and does not fold it into the value; names surface only in the test's
golden dumper. The row allocator is the whole reason the driver is portable
across the milestones: S0 hands it a bump arena the test frees wholesale, and S1
will hand it `ash_bytes` on the signing instance, and the driver cannot tell the
difference. A `sqlite3_busy_timeout` is set at open so a writer that must wait on
another connection surfaces as a store error rather than a silent stall, the
concurrency behavior `docs/database.md` pins for the two instance case that S3
will exercise.

## [v0.2.0] the durable boundary

- open Layer 3 with the store design: a contract backed by a database, its
  schema locked as vows and reconciled against the live table at sign, its
  transactions written as subcontracts that commit when the group completes and
  roll back on any failure or on break
- pin the one new fact storage adds, `ASH_ERR_STORE`, the backend failing the
  runtime and nothing more, with the contract's own rules kept in the contract's
  own error type
- choose the ground: SQLite vendored into the runtime as the reference backend,
  a small store vtable behind it for a second backend later, a stdlib store
  surface the pledge bodies call with parameters and never concatenation, and
  relational shape only for v1
- lay the roadmap for the layer, the store driver proven under raw C before the
  first emitted schema depends on it, the first layer since the language surface
  to move the compiler

## [v0.1.3] the resilience pass

- bound the connect: a non blocking connect and poll capped by the handshake
  timeout, with a send deadline over the handshake writes, both cleared once
  served so a running pledge still blocks as long as it runs; a dead peer now
  fails in the timeout instead of parking on the kernel default
- cover every in flight state across a disconnect: a fulfillment awaiting its
  result, an interrupted round trip, a half read frame, and any call after
  teardown all resolve to a network error through an idempotent shutdown that
  touches no wire
- keep the daemon standing through client death, a dead socket write, and a
  malformed frame, draining an instance's waiters before it breaks and staying
  synced on a payload the codec consumes whole
- add a stress gate: load across connections and threads, disconnect churn while
  serving, and a kill storm with hundreds of fulfillments in flight, every wait
  landing a network error or a clean result, no crash and no leak, clean under
  both sanitizers with a hang failing loud
- pin live reconnect out of v1: a dropped connection is final for its runtime,
  and a reconnect is a fresh one

## [v0.1.2] the remote fulfillment

- serve sign, fulfill, break, and partial from ashd: a sign returns the instance
  and its effective vows, a fulfillment runs on the pool and a detached waiter
  sends its result, and a dead connection breaks the instances it signed
- route the client transparently: sign, fulfill, wait, break, and the partial
  surface follow a remote origin proxy over the wire and the local path
  everywhere else, so the same host code drives a contract local or remote,
  proven by one test asserting identically on both
- carry one reader thread and a writer lock per connection, the request id
  doubling as the future id so every answer routes by id alone
- refuse references over the wire and keep binding local, and deliver a
  disconnect as a network error through every waiting future
- hold the instance lock across every runtime owned write on the remote partial
  path, closing a use after free an external review caught, and reclaim a
  proxy's heap on break so the invariant carries its weight
- correct the wire notes: no argument count crosses, and a fulfillment names a
  pledge the way the local call does

## [v0.1.1] the handshake

- add ashd, a daemon over the runtime that loads modules, freezes, and serves
  their contracts thread per connection, snapshotting the iname dump and its
  hash once at startup
- speak the handshake: a version checked HELLO, a constant time token compare
  that folds length in and never returns early, a HELLO_OK carrying the dump
  hash, and the whole iname table in one INAME_TABLE
- connect a client with ash_runtime_connect, which dials, verifies the received
  table against the HELLO_OK hash, and merges every remote entry into the local
  table under the freeze law, rejecting a name collision without merging anything
- factor the socket plumbing into net.c: full transfer reads and writes that
  retry on EINTR, frame send and receive over the wire codec, and dial and
  listen with keepalive
- gate the handshake with a raw frame client covering the good path, a refused
  token that merges nothing, a forged version, a dead address, and connect after
  freeze, clean under the thread sanitizer
- pin the timeout surface and the argument count handoff in the network design

## [v0.1.0] the wire

- open the network layer with its design: an ashd daemon serves the contracts
  its modules carry, a client connects the same runtime it always linked, and
  every remote contract appears in the local iname table, so the host that moves
  a contract across the network changes no code
- define the wire: a fixed twenty byte frame, thirteen message kinds, and a
  canonical little endian value encoding where decoding and re-encoding any
  payload reproduces it byte for byte
- ship the codec inside the runtime with byte goldens for every representation
  and a negative corpus that sweeps every strict prefix, lies about every
  length, bombs the nesting cap, and puts the two untransportable tags on the
  wire, all refused without a crash
- reserve out of memory for the payload cap alone: wrong magic, an unknown kind,
  a duplicate map key, and a domain breaking scalar are malformed and typed as
  such, encoder and decoder holding the same rules
- append the network status to the ABI ahead of its first use

## [v0.0.15] the map and the place

- give maps their surface: the constructor form with spelled type arguments, an
  index read that answers Option with None on a miss, and an index assignment
  that inserts or updates, over a pinned representation of interleaved pairs in
  insertion order
- deep copy both halves of every entry on the way in and the value on the way
  out, compare maps order sensitively, and render them whole
- teach the parser that Map before an angle bracket opens type arguments in
  expression position rather than a comparison
- fix a silent lost write: an assignment through a chain, a record field's list
  element or a map slot behind a field, wrote to a read copy and vanished;
  targets now lower through an in place lvalue walk at any depth while the right
  side keeps value semantics
- rule the lvalue map index: it names the value slot and types as the value,
  final position never faults, and a missing key mid chain faults like an out of
  range index, all pinned in the grammar
- run the fault checks before the right side evaluates, and pin the whole
  surface with gauntlet pledges, reject twins, and runtime unit tests under the
  sanitizers

## [v0.0.14] the call across the boundary

- call one contract from another inside a pledge body: sign with vow overrides,
  fulfill, and break lower onto the runtime, and a callee's result is copied
  home before use so it outlives the callee breaking
- make a signed instance a reference handle, the one exception to value
  semantics, pinned in the grammar: copies alias, equality is identity, and
  nothing can move one across the boundary
- run a fulfillment issued from a pool worker inline on that worker, so nested
  calls cannot starve the pool, proven by a two worker stress with nested sign,
  fulfill, and break from thunks
- restructure sign into three phases after the thread sanitizer caught it
  holding the runtime lock across a fresh instance mutex; the lock hierarchy is
  now instance, then runtime, then future
- deep copy every vow override at sign, closing a hole an external review
  confirmed where a composite override aliased caller heap into a signed vow and
  dangled after break
- expose the owning runtime of an instance, and gate the whole surface under the
  sanitizers beside the existing suite

## [v0.0.13] the standalone program

- link a real executable from build --bin: exactly one Main contract with a
  bodied run over List<String> returning Result<Int, E>, a generated main that
  signs, marshals argv onto the instance, fulfills, and maps Ok to the exit code
  and Err to a rendered diagnostic
- render any value for diagnostics through ash_value_render, recursive with a
  depth cap and the same sizing protocol as the iname dump
- emit the C header a foreign host compiles against: the shape hash and every
  mangled pledge name as defines, read from the same tables the module emitter
  hashes, pinned against a golden and proven by a host that uses nothing else
- reject the bin builds the grammar forbids, no Main, a missing or abstract run,
  and a wrong signature, each with a named diagnostic
- pin the entry point rules in the grammar and bring the README status up to
  what the toolchain actually does

## [v0.0.12] the second language

- bind the runtime from Python with ctypes against the ABI reference alone, no
  header parsed and no code generated, which is the product claim made runnable
- walk the payment contract from Python end to end: a Python function bound over
  the abstract charge pledge and called back from a pool worker, sign with a vow
  override, the partial, fulfilled, and broken paths with the error payloads
  read back, and a by reference argument written back through the host buffer
- record what the binding taught the ABI reference: a host that opens the
  runtime dynamically must load it into the global scope or every module resolve
  fails, the concrete LP64 layouts for a header less foreign function interface,
  the callback lifetime rule, and the stability of every enum value
- gate the walk behind make test-python beside the other gates

## [v0.0.11] the standard library and the boundary

- load imports for real: dotted paths resolve beside the root then under
  ASH_HOME, every file parses into one shared tree with per file span bases
  mapping diagnostics back to their sources, cycles and duplicate imports are
  named errors, and two builds stay byte identical
- enforce the package boundary: an internal contract or provisional clause is
  invisible outside its directory through incorporate, sign, and type
  annotations alike, and an internal contract never reaches the descriptor
  tables or the iname
- close an unchecked ABI hole an external review confirmed with a repro: a
  contract type in a pledge signature emitted no runtime check at all, so a host
  could pass anything; contract types are now rejected at every boundary
  position at any nesting depth, pinned in the grammar
- lower clauses as static functions sharing the thunk's exit convention, so
  returns and propagation work unchanged and mutual clause calls need no ordering
- write the first ashstd: math with overflow checked pledges and an epsilon vow,
  strings and collections shaped honestly to what the language can express,
  common errors, and the comparable, loggable, and serializable provisional
  clauses
- drive the library end to end from the C host, one module registering three
  contracts, sorting through an incorporated compare clause

## [v0.0.10] the whole body

- lower the rest of the surface: while, for over a list, all three assignment
  targets, break and continue, match in every form with nested patterns and
  payload bindings, record and variant construction, field and index access,
  list and tuple literals, and propagation
- give sums their representation, the variant index as the tag over the shared
  payload arm, and teach the deep copy and the new recursive equality to walk
  records and sums
- short circuit the logical operators by lowering the right arm inside a guard,
  so a bounds check like i < n && xs[i] never touches the index it just refused
- give composites value semantics at every boundary, let, assignment, and
  composite reads all deep copy, so mutation through one binding is never visible
  through another, both rules pinned in the grammar
- return a type error from an out of range index instead of faulting, with the
  outcome never latched
- gauntlet the whole set behind a sanitized gate and fold the new module into
  the determinism check

## [v0.0.9] the fulfillment policy

- replace the counting latch with true per pledge state: first Ok latches
  fulfilled, an Err before any Ok latches broken with its payload kept readable,
  and neither ever changes again
- serialize the requirements block from the exact tree the checker verified,
  atoms in the checker's own enumeration order, with the grammar's defaults
  synthesized into the same postfix form when the block is absent
- evaluate the policy under the instance lock after every outcome, in break,
  fulfill, partial priority, with the break line armed only once something has
  actually broken, a rule now pinned in the grammar
- transition to broken automatically when the break line holds, keeping the
  instance heap alive so the error payloads stay readable until an explicit break
  reclaims them
- expose the partial result: item counts and names over named subcontracts and
  loose pledges, and the latched errors by pledge name
- drive the spec's payment contract end to end through partial, fulfilled, and
  both broken paths under the sanitizers

## [v0.0.8] the iname table

- register every contract and pledge in a sorted runtime table keyed by mangled
  name, inserted with full rollback so a failed registration leaves nothing
  behind
- add freeze: after it, loading, registering, and binding refuse with a state
  error while signing and fulfilling continue, and the table reads are immutable
  from then on
- expose lookup by exact mangled name, ordered enumeration, and a canonical
  dump, one line per entry in mangled order, sized or refused whole
- synthesize the contract level mangled name from the descriptor and carry the
  owning contract's shape hash on every pledge entry, so a discovered entry is
  exactly the pair a host signs under
- walk the host through real discovery, lookup to sign to fulfill, and pin a
  second generation module whose stale mangled names miss at every version stamp
- gate determinism, two builds byte compared, and the table against a golden
  dump with the real hashes

## [v0.0.7] the pool

- run every fulfillment on a worker pool sized by the runtime config, with copy
  in still on the caller's thread and write back still at the wait, so the memory
  model's promises survive real concurrency
- queue tasks through an intrusive link the future itself carries, so enqueueing
  allocates nothing and shutdown drains, joins, and frees single threaded
- serialize each instance behind one recursive mutex covering the thunk run, the
  latch, and every arena allocation, with distinct instances genuinely parallel
  and a documented runtime, instance, future lock order
- resolve the break race deterministically: a running thunk finishes, a queued
  task finds the contract broken and touches nothing, and an unwaited future is
  forfeited to a state error with its pointers cleared before the heap goes
- refcount futures between the receipt and the pool, fixing a use after free the
  thread sanitizer caught on the sync path against a broken instance
- pin the model with a stress gate, hundreds of fulfillments across threads and
  instances plus break hammering, run under the address and thread sanitizers in
  the default build, both silent
- document the threading rules in the ABI reference

## [v0.0.6] the memory model

- deep copy every argument onto the instance at fulfill entry, on the caller's
  thread, so a thunk only ever sees instance owned values and a host buffer
  rewritten after the call changes nothing
- add by reference arguments: a ref occupies a trailing declared parameter as a
  mutable instance owned slot, and its final value is written back to the host
  pointer at delivery only, inside the wait or before the synchronous return, so
  host memory is touched only while the host is blocked in an ash call
- write back through a default for scalars and strings or a caller supplied
  callback, only on a fulfilled outcome, with every slot type checked before
  anything is written
- add ash_pledge_bind: a host implementation bound over the runtime's overlay,
  resolved into a fixed dispatch table at sign, beating a compiled body and
  satisfying an abstract pledge, with an unbound pledge still refusing to sign
- grow the value layer: lists, tuples on the list arm, and a recursive deep copy
  across every representation
- give the skeleton an abstract shout pledge the host binds, prove the write
  back timing on both delivery paths, and pin the value layer with an address and
  leak sanitized unit gate
- extend the ABI reference with the binding and by reference protocols

## [v0.0.5] the real emitter

- retire the canned module: build now runs the whole front end and lowers the
  typed tree, and a construct codegen cannot carry yet is a named build error
  instead of wrong C
- emit per pledge thunks over the uniform frame, descriptor tables with vow
  defaults, and the registrar, all byte stable across rebuilds
- mangle every pledge as __ash_ash_{contract}_{symbol}_{sighash}_v{n} with an
  fnv1a 64 shape hash over the canonical signature strings, cross checked against
  an independent implementation
- store vows on the instance: sign takes named overrides over the declared
  defaults, checked by name and type, with strings deep copied so an instance
  never aliases host memory
- give fulfillment its real shape: fulfill returns a future, wait delivers the
  outcome exactly once, and the synchronous form rides the same path, so hosts
  survive the threading milestone unchanged
- carry the hidden signature on every instance, the shape hash and the signing
  time, and refuse a wrong expected hash at sign
- concatenate String with plus, pinned in the grammar and the checker
- write the ABI reference: value representations, the thunk frame, descriptors,
  ownership, mangling, and the hash algorithm

## [v0.0.4] the checker

- write the type checker in dusk over a hash consed type arena whose canonical
  strings are deterministic, the same strings the signature hashes will eat
- enforce the contract rules: a pledge returns Result, Option, or Unit, a vow
  initializer is a constant of the vow type and never assignable, and an
  incorporated signature matches its clause exactly
- infer expressions with expected type pushdown and no implicit conversion
  anywhere, widths included, over the full operator table
- type sign with named vow overrides, instance pledge calls, first class pledge
  values, and break, and check match exhaustiveness over sums, Option, Result,
  and Bool with the propagation operator held to its enclosing return type
- enumerate the requirements policies and reject a fulfill and break that can
  hold together, naming a witness state, and a policy no state satisfies
- name everything deferred with a not yet supported diagnostic instead of
  accepting it silently
- pin the checker with six golden programs and eighteen reject twins, and pin
  the block as value rule in the grammar

## [v0.0.3] the parser

- write the parser in dusk over parse order arenas: fixed slot nodes for every
  declaration, statement, expression, type, and pattern, a shared kids arena for
  the variable length lists, and interned names
- carry the whole surface: records, sums, provisional clauses, contracts with
  attributes, vows, pledges with optional bodies, clauses, subcontracts, and the
  requirements block with its own operator grammar
- terminate statements on the newline flag at group depth zero, with braces
  resetting the depth so a block inside an argument list keeps statement
  semantics
- ban the bare record literal in an if, while, for, or match head and require
  its brace on the name's line, both pinned in the grammar
- hold the 500 deep nesting ceiling without the quadratic unwind, one diagnostic
  and milliseconds on a ten thousand paren bomb
- add the parse command to ashc with a byte stable tree dump, and pin the front
  end with seven golden fixtures and six reject twins

## [v0.0.2] the token stream

- write the lexer in dusk: every literal form, the full escape set with unicode
  scalar validation, line and block comments, and span tracking with a newline
  flag on each token instead of a newline token
- add the lex command to ashc, one token per line to stdout in a byte stable
  dump format, diagnostics to stderr with the caret line
- pin the lexer with golden fixtures over every construct and reject twins for
  the escape and termination faults, run by tests/run.sh
- pin three literal rules in the grammar: no raw newline in a string, integer
  literals cap at the Int maximum for now, and a unicode escape never names a
  surrogate or a value past 0x10FFFF
- write the how to use section of the README around make smoke and the sanitizer
  gate
- rebuild ashc when any compiler source changes, not just the two files the M0
  rule listed

## [v0.0.1] the walking skeleton

- pin the surface in docs/grammar.md: the statement and expression core, record
  and either declarations, method call fulfillment, the requirements grammar
  with the latch on first Ok rule, and the fixed numeric representations
- write the README: what a contract is, the lifecycle, and how the interop
  crosses the C ABI
- define the ABI in ash_abi.h as the single source of layout truth: the
  AshValue fat value, the uniform pledge thunk frame, and the descriptor tables
  a compiled module registers
- build the runtime: module load over dlopen, contract sign with shape hash and
  unbound pledge checks, synchronous fulfillment through the thunk frame,
  outcome latching, and a per instance block list that break reclaims in one walk
- write ashc in dusk as the first spike: read the source, emit the canned
  Greeter module C, and link it with cc into a loadable .ash.so
- drive the whole pipeline from a C host that checks the greeting, the state
  transitions, and the error paths, gated by make smoke and an address and leak
  sanitizer build
