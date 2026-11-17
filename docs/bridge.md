# The Geas Bridge

Geas connects languages in two shapes, and this document is the normative reference for the second. Inside one process, contracts glue languages over the C ABI: every language loads `libgeasrt`, signs, fulfills, and breaks in shared memory, and nothing is serialized. Across processes, contracts speak gRPC: the compiler emits the wire surface, stock tooling builds the clients, and the contract's lifecycle rides a protocol every language already carries. One contract, two boundaries, one lifecycle.

## The emitted surface

`geas emit-proto file.geas` runs the whole front end and writes three artifacts into `target/geas-out`, each byte stable for a given source and pinned as a golden:

- `<stem>.proto`, the wire surface. Every public contract is its own service, every pledge its own typed rpc, so a consumer in any gRPC language gets the contract's argument and result types from protoc alone, with no generic value envelope and no dispatch on a name string.
- `<stem>_session.go`, the Go session wrapper, living in the same package as protoc's output.
- `<stem>_session.ts`, the TypeScript session wrapper, structural interfaces over `@grpc/proto-loader`, no protoc step and no native code, and node runs it directly under type stripping.

The wrappers exist because protoc gives a consumer every typed call and cannot give it the one thing the surface turns on: the stream whose lifetime is the instance's. Each wrapper is a page with one idea in it, and another language's wrapper is a morning, not a milestone.

## The session

Signing is a server streaming rpc, and that is the surface's one load bearing decision. `Session` takes what sign takes, the vow overrides as optional fields and an `expected_hash` where 0 skips the check, and answers with a stream whose first event is the signature: the instance id every pledge rpc is keyed by, the effective vows, the shape hash, the signing time, and the park token when the server parks. The stream then stays open for as long as the instance lives. Holding it is how a client says still mine; closing it is how a client says done; dying is a fact the server has in milliseconds. No timer ever decides a contract's ending.

The pledge rpcs are unary and typed, keyed by the instance id. `GetPartial` reads the partial surface, the name lists repeated in the runtime's insertion order, which is normative, beside the contract's state. A file whose contracts reach the workflow surface spells that state with `REMEDYING`, the break decided with the remedies still running, always followed by `BROKEN`; a file that does not keeps the five states it always had. Every signal is its own unary rpc, `Signal` then the signal's CamelCase name, keyed by the instance id with the signal's parameters, answering an empty `SignalReply`: the delivery landed, or a repeat of one that already had, which changes nothing. A refusal rides the call's status, an early delivery, a late one, or one into a contract already Remedying or Broken all FAILED_PRECONDITION, and each session wrapper carries a helper that keys the call by its own session. `Break` ends the contract explicitly; the row outlives the break behind its stream, so a broken instance still says broken instead of saying it never existed, and leaves when the stream does.

The session stream carries the signature and nothing after it. Its event is a oneof so a later arm can ride the same stream, and until the emitted surface declares one, a client reads what changed through `GetPartial` and `GetCourse`, which is what they are for.

## Engine driven contracts

A contract with a `course` runs its own pledges, so its service carries no pledge rpc at all, and that is the whole difference on the wire. `Session` signs and the course starts with the sign, on the server's runtime, calling the bodies the server bound; the client moves it on with the signals the course awaits, and reads where it stands with `GetCourse`, the runtime's course read over the wire: the course state, `COURSE_NONE` through `COURSE_CANCELLED` in the runtime's own numbering, the step it is at or ended on, the status that halted it, and the clock time its pending wait falls due. `GetCourse` never waits on the course, a client polls it or watches the partial surface. The server runs the runtime's ticker for its whole life, so a `sleep`, a `select` timeout, or a `within` falls due with no rpc in flight to tick it. A course resting at a wait parks with its stream like any other instance and resumes at the same step, every binding it had made still in its slots.

## The shape hash

The wrappers pin the contract's shape hash as a constant, the same value the compiled module registers, computed by the same calls the module emitter makes. A client that signs under the pinned hash has proven the wire surface and the loaded module describe one contract before the first pledge runs. A hash that disagrees is refused at sign, before an instance exists.

## Business and transport

A contract's own `Err` is a value. It crosses as one arm of the result oneof on an OK rpc, exactly as it returns in process, and never becomes a transport error. A Geas status is the transport and lifecycle layer, a fulfillment, a delivery, or a sign that did not run, and crosses as a gRPC code:

| Geas status    | gRPC code           |
| ----------------- | ------------------- |
| `GEAS_ERR_STATE`   | FAILED_PRECONDITION |
| `GEAS_ERR_NAME`    | NOT_FOUND           |
| `GEAS_ERR_TYPE`    | INVALID_ARGUMENT    |
| `GEAS_ERR_VERSION` | ABORTED             |
| `GEAS_ERR_UNBOUND` | FAILED_PRECONDITION |

An instance id the server does not hold, never issued, already gone with its stream, or issued for another contract's service, is NOT_FOUND, and a request whose value cannot be the declared type, a sum with no arm set, is INVALID_ARGUMENT before the runtime sees it. A status the table does not name is INTERNAL, and no message the server sends carries a path or a trace. The split is the language's own line, drawn once and held across the wire.

## The type set

Scalars cross as proto scalars, Int and UInt as their 64 bit forms, Float as double, Bool and String as themselves. A declared record is a message with its fields in declaration order. A declared sum whose arms are all bare is an enum numbered in declared arm order, the ABI's own tag space; a sum with a payload arm is a message whose oneof carries one message per arm in the same order. Unit is the empty message. Option and List ride oneof arms as wrapper messages and sit in fields as optional and repeated. A pledge may answer a bare Option and it crosses as the wrapper directly. A Map crosses as a wrapper message holding a repeated entry pair, key and value fields in one small message, never as proto's own map type: Geas pins map iteration as insertion ordered and proto's map carries no order at all, so the bridge spells a map as the ordered entry list it actually is and the order a program built is the order the wire carries. A tuple crosses as a wrapper message with one numbered item field per position. Both wrappers are emitted once per file under deterministic names, the value kind last, StringIntMap and IntStringTuple, the same rule that names BoolIntResult.

Message names carry no contract prefix while a file declares one public contract; a file with several prefixes every contract scoped message with its contract's name. The rpc spellings `Session`, `Resume`, `GetPartial`, and `Break` belong to the surface, and a pledge under one of those names is refused by name; a pledge spelled `sign` keeps its rpc and moves its request message alone to `SignPledgeRequest`.

## Park and resume

A server started with a park store issues every session a park token beside its signature. When a stream ends without an explicit `Break`, the server parks the instance under that token and then releases it: the vows, the latches, the error payloads, and the transactional fates go into one store row through the runtime's own park call, and the release lets the live instance go from the process with no remedy run and no state change, so the row is the obligation's only home and nothing it owes is paid on the way out. `Resume` takes the token, stands the instance back up under a fresh id on a new stream, and consumes the row, one shot: a spent or unknown token is NOT_FOUND, a lying hash is ABORTED before the row is touched, and a resumed session that later drops parks again under the same token. An explicit `Break` stays the one true ending, and the token dies with the instance.

The park store is what makes the session survive everything the stream cannot. A network partition that kills the stream loses nothing a resume cannot recover; a server that dies is replaced by any server holding the park store, which is the whole answer to replica affinity. Who may resume a token, how often, and for how long is server policy, deliberately outside the runtime: the runtime writes and reads park rows, and the bridge decides what a token is worth.

Two replicas may hold one park store, and the runtime's claim keeps them honest. A `Resume` is one claim call: the runtime reads the row, builds the instance without publishing it, deletes the row with a statement that matches every column it read, and publishes the instance only when that delete removed the row. When two replicas race the same token exactly one delete removes it, so exactly one replica holds the contract, and the loser publishes nothing and answers `NOT_FOUND`, the same answer a spent token gets, because a token another replica already claimed is spent from where the loser stands. The claim always checks the row against the shape hash of the service it arrives on, so a lying hash, a token naming another contract's row, and a row from another build all fail before the delete, the store untouched: a wrong hash stays ABORTED and a lost race stays NOT_FOUND with no way to confuse the two.

## The server

The reference bridge server is a Python host over `libgeasrt`, and it is generic: it serves any contract the compiler can emit, and no line of it names one. It loads each module through the C ABI, binds the bodies a deployer supplies over the abstract pledges and remedies, freezes, and then reads each emitted `.proto` at startup, compiled by grpc_tools into descriptors in a pool of its own, with no generated stub anywhere on the server side. Every service in those files becomes one generic handler. Each rpc gets its role from the surface's own spelling, `Session`, `Resume`, `GetPartial`, `Break`, `GetCourse`, a `Signal` rpc answering `SignalReply`, and every other rpc a pledge, matched against the pledge names the loaded module registered through the same CamelCase rule the emitter spells them with, its parameter count checked. A surface and a module that disagree fail at startup, not at the first call.

```sh
python interop/grpc/bridge_server.py --port 50251 \
    --module target/geas-out/libsaga.geas.so \
    --handlers interop/grpc/handlers/saga.py \
    --park-dsn target/park.db
```

`--module` repeats, and each module's surface is the `<stem>.proto` emit-proto wrote beside it unless `--proto` names the files outright. `--tick-ms` sets the ticker's period and `--max-workers` the gRPC pool.

A live `Session` or `Resume` stream holds one pool thread for the instance's whole life, and every pledge, signal, `GetPartial`, `GetCourse`, and `Break` needs a thread of the same pool, so the server never lets sessions take all of it. `--reserve-workers`, 4 by default, is the part no session may hold, and the server admits at most `--max-workers` less `--reserve-workers` live sessions at once. A `Session` or `Resume` past that limit is answered with RESOURCE_EXHAUSTED before it signs or claims anything, and the slot comes back the moment a session ends. A pool that leaves no room for both one session and the reserve refuses to start.

Values cross by reflection over the descriptors, the type set above read backwards: a message's shape says what Geas value it carries, a wrapper recognized by its emitted name and its exact fields, a sum by its oneof of arms, a record by everything else, and the value is built with its type tag spelled out, so a `UInt` stays a `UInt`, a `Byte` map key stays a `Byte`, and an empty list keeps its element tag. A declared field or parameter of `Option` over a message shaped type is the message itself, its presence the Option's. When that message is an Option wrapper the field can only be an `Option` of an `Option`, so its presence is the outer Some and the wrapper the inner Option, and `None`, `Some(None)`, and `Some(Some(x))` all cross both ways as three different messages. When it is a record, a sum with payload arms, or another wrapper the same spelling also names the plain type, so going out an absent value leaves the field unset, and coming in the field reads as the plain type. An Option wrapper's own value, a map value, and a tuple item are spelled by type alone and carry their type exactly.

A deployer supplies bodies in plain Python files, each passed with `--handlers`, each defining a `HANDLERS` dict from a `Contract.pledge` or `Contract.remedy` name to a callable in the binding's own form, `fn(instance, args)` answering a Python value, `Ok` and `Err` included:

```python
from geas import Err, Ok, Record, Variant

def ship(inst, args):
    res_id, sku, hold = args[0]
    if sku == "refuse":
        return Err(Variant(1, "no carrier takes it"))
    return Ok(Record("ACME", f"T-{res_id}"))

HANDLERS = {"OrderSaga.ship": ship}
```

A name nothing declares fails startup. An abstract body no file supplies leaves its contract refusing to sign with `GEAS_ERR_UNBOUND`, which is FAILED_PRECONDITION on the `Session` call, the same answer an in process host gets before it binds. A body that raises is logged with its traceback and reported to the runtime as `GEAS_ERR_TYPE`, the fulfillment produced no value.

Calls against one instance run side by side and the runtime serializes what it must on its own instance lock, and every reply is built while its call is admitted, so nothing a reply reads outlives the instance it came from. The ending runs off gRPC's own threads, which the termination callback only hands it to, and it goes in a fixed order: the row leaves the table so no new call resolves the id, the bodies are asked to stop through the runtime's cancel request, the calls already admitted are waited out, and only then does the instance park and release, or break. A body that polls the cancel flag returns, so the wait always ends, and nothing is reclaimed under a call still decoding its reply. An engine driven contract that parks skips the request, since a course the request cut would reach the row as cancelled, and its course is given a moment to reach its next wait. `Break` drains instead of closing: it asks the bodies to stop, holds new calls back while the admitted ones finish, breaks with nothing in flight, and lets the held calls through to a broken instance that still answers. Stopping the server ends every live session the same way, and waits for every ending before the runtime goes.

A session that parks hands its obligation to the row and is released, never broken, so no remedy runs on a hand off, a Geas written one or a supplied one alike: the remedies stay owed in the row, and whichever replica resumes the session runs them if the contract ever breaks for real.

Nothing about the server is privileged. Any language that reaches the ABI can hold the runtime, and any language with a gRPC server can serve the surface, because the bridge is ordinary host code on one side and ordinary protobuf on the other.

The gates stand the server up against clients in Python, Go, and Node, the Go and Node ones built from nothing but the emitted files and stock tooling. They walk the payment contract, the host driven order saga through its signal, and the engine driven order saga through `GetCourse`, and the language gauntlet's lists, maps, tuples, and options cross through nothing but reflection. They kill the server between a park and a resume and demand the contract finish on latches set before the death, park a course at its select and resume it at the same step, and stand two replicas over one park store, kill the one that holds an instance, resume it on the other, and then set two replicas racing round after round, one token a round, and demand exactly one win and one NOT_FOUND every round. Smaller contracts walk the edges: a `Break` and a dropped stream beside a body that returns only once a cancel request reaches it, a full session limit that still answers a unary call and refuses the next `Session`, a parked course whose Geas written remedy never runs on the hand off and runs once when the resumed contract breaks, and every nesting of an `Option` of an `Option`.

## What the bridge does not do

There is no relay and no registry: a client dials a server that holds the contract, and a server serves the contracts its runtime registered. There is no cross server transaction and no shared session bus; one instance lives behind one stream or one park row at a time, never both, never neither. Discovery, load balancing, and transport security are gRPC's own ground, and the bridge stands on it rather than beside it.
