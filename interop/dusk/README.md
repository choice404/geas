# Geas from dusk

So this is the dusk host binding for libgeasrt, the same surface the [Python package](../python/geas/) gives you, written as a dusk module over the C ABI. You load a compiled contract module, bind dusk functions over the abstract pledges and remedies, sign, fulfill, signal, tick, and read the partial surface, and the runtime does the rest, the terms, the retries, the remedy pass, all of it. Now a contract that carries a course goes one step further, the runtime runs the steps by itself, so the host only binds the bodies, signs, waits for the course to rest, and hands the signals in.

Four files make up the binding, `geas.dusk` is the runtime surface, `geas_value.dusk` is the `Value` tree and its conversion to and from `GeasValue`, `geas_attrs.dusk` reads the declared settings, and `geas_shim.c` is the thin C side, which holds only what a dusk foreign block cannot say by itself: the union inside `GeasValue`, the trampolines the runtime calls back through, and the few calls that take a NULL. Every other runtime call is declared straight in `geas.dusk` and linked against libgeasrt.

## Running the demos

```sh
interop/dusk/run_demo.sh
```

It runs `make compiler runtime`, builds `skeleton/saga.geas` and `skeleton/order_course.geas` with `target/bin/geas`, builds `demo_saga.dusk` and `demo_course.dusk` with dusk, and runs both. When `dusk` is not on PATH the script prints a skip line for each and exits 0.

`demo_saga.dusk` is the host driven saga, the host calls every step. It walks the order saga three times, a happy path where authorize eats one Timeout through its retry term and the saga lands Fulfilled, a break path where the carrier refuses and the remedies undo what landed newest first, release then refund, and a hand off. Now the hand off is the one to look at, the first runtime lands everything up to ship, requests a cancel and shows the instance still takes ship while the request stands, parks it, and releases it with no remedy run and the state still Partial, then a second runtime claims the row, delivers the signal, and lands Fulfilled, and a second claim of the same key answers `GEAS_ERR_NAME` since the row is gone. It ends with `[demo_saga_dusk] ok`.

`demo_course.dusk` is the same order saga with a course, so the runtime calls every step and the host never does. The happy path signs with the runtime's ticker running, waits until the course rests at its select, having planned, authorized through one Timeout, captured and reserved at the same time, and shipped, shows that a host fulfillment answers `GEAS_ERR_STATE`, then delivers the signal and waits for the course to finish with the saga Fulfilled. The break path has the carrier refuse, the course ends on ship's `?`, and refund runs once with the receipt while the saga reads Remedying, and it ends with `[demo_course_dusk] ok`.

## Building your own host

Put the four files beside your program (dusk resolves an import from the importing file's directory), import `geas` and `geas_value`, plus `geas_attrs` when you read settings, and build with the runtime's directory on both library paths:

```sh
export LIBRARY_PATH=$PWD/target/geas-out
export LD_LIBRARY_PATH=$PWD/target/geas-out
dusk build my_host.dusk
```

The binding links the runtime by its SONAME, `-l:libgeasrt.so.0`, rather than `-lgeasrt`, since the linker searches the system directories before `LIBRARY_PATH` and an old unversioned `libgeasrt.so` sitting in `/usr/lib` would otherwise answer for it. Nothing is dlopened, the compiled module names the same `libgeasrt.so.0`, so the host and the module share one runtime. The shim includes the runtime header by a path relative to itself, so if you move it, fix that include.

A pledge handler is a top level function, or a lambda that captures nothing, over a state pointer of your own type:

```dusk
func capture(inst: Contract, args: *Vector<*Value>, host: *SagaHost) -> (*Value, error) {
    amount, e := contract_vow(inst, "amount")
    if e.exists() {
        return (amount, e)
    }
    return (value_ok(value_record(values2(value_text("RC-7"), amount))), error {})
}

e := runtime_bind(rt, "OrderSaga.capture", host, capture)
```

The state rides a pointer rather than a capture because a dusk closure that captures a local cannot outlive its frame, and the runtime calls the handler long after `runtime_bind` returned. Keep the state alive until `runtime_shutdown`. The args frame is freed when the handler returns, so clone what you want to keep, and the handler's answer is copied onto the instance and freed for you. A handler that returns an error gets it printed to stderr and the runtime sees `GEAS_ERR_TYPE`, the body produced no value, which a retry term then treats like any lifecycle failure.

## The API

| dusk | runtime | notes |
|---|---|---|
| `runtime_start(max_threads)` | `geas_runtime_init` | 0 picks the default pool of four |
| `runtime_load(rt, path)` | `geas_module_load` | |
| `runtime_bind(rt, name, state, handler)` | `geas_pledge_bind_user` | pledges and remedies, `"Contract.name"` or the mangled symbol |
| `runtime_set_clock(rt, state, now, sleep)` | `geas_runtime_set_clock` | `runtime_default_clock(rt)` puts the wall clock back |
| `runtime_freeze(rt)`, `runtime_tick(rt)` | `geas_runtime_freeze`, `geas_runtime_tick` | |
| `runtime_start_ticker(rt, interval_ms)`, `runtime_stop_ticker(rt)` | `geas_runtime_start_ticker`, `geas_runtime_stop_ticker` | a runtime thread that ticks in real time, a second start or a stop with none running is `GEAS_ERR_STATE`, shutdown stops it too |
| `runtime_attrs(rt, contract)`, `runtime_attr(rt, contract, item, key)` | `geas_attr_count`, `geas_attr_at`, `geas_contract_attr`, `geas_item_attr` | in `geas_attrs`, item `""` is the contract, the bool says whether the key exists, free a list with `attrs_free` |
| `runtime_shutdown(rt)` | `geas_runtime_shutdown` | then frees every binding and clock the shim held |
| `contract_sign(rt, name, vows)`, `contract_sign_pinned(..., hash)` | `geas_contract_sign` | vows are a `*Vector<Vow>`, copied at the call |
| `contract_resume(rt, dsn, key, hash)`, `contract_park(c, dsn, key)` | `geas_instance_resume`, `geas_instance_park` | bind on the new runtime before resuming |
| `contract_claim(rt, dsn, key, hash)` | `geas_instance_claim` | resumes the row and takes it out of the store in one move, a second claim of the key answers `GEAS_ERR_NAME`, a row that will not build stays put |
| `contract_fulfill_sync(c, pledge, args)` | `geas_pledge_fulfill_sync` | args is consumed; a pledge's Err is a value, not an error |
| `contract_fulfill(c, pledge, args)` | `geas_pledge_fulfill` | answers a `Receipt` |
| `future_poll(r)`, `future_wait(r)`, `future_release(r)` | `geas_future_poll`, `geas_future_wait`, `geas_future_release` | wait delivers once, release after wait or after a break |
| `contract_signal(c, name, args)`, `contract_signal_payload(c, name)` | `geas_signal`, `geas_signal_payload` | args is consumed |
| `contract_tick(c)` | `geas_instance_tick` | |
| `contract_partial(c)` | `geas_partial_count`, `geas_partial_name`, `geas_partial_error` | free it with `partial_free` |
| `contract_lapsed(c, name)`, `contract_term_lapsed(c)` | `geas_partial_lapsed`, `geas_contract_term_lapsed` | |
| `contract_remedy_state(c, name)`, `contract_remedy_failures(c)` | `geas_remedy_state`, `geas_partial_remedy_*` | free the failures with `remedy_failures_free` |
| `contract_attrs(c)`, `contract_attr(c, item, key)` | `geas_instance_attr_count`, `geas_instance_attr_at`, `geas_instance_attr` | in `geas_attrs` |
| `contract_state(c)`, `contract_hash(c)`, `contract_signed_at(c)`, `contract_vow(c, name)` | the matching `geas_contract_*` and `geas_vow_ref` | `state_name` spells a state |
| `contract_break(c)`, `contract_cancel_requested(c)` | `geas_contract_break`, `geas_cancel_requested` | a body polls cancel through the `Contract` it was handed |
| `contract_request_cancel(c)` | `geas_contract_request_cancel` | publishes a cancel bodies read and returns at once, the instance stays live and nothing breaks |
| `contract_release(c)` | `geas_contract_release` | lets the instance go with no remedy run and no state change, its park row carries the obligation on, later calls answer `GEAS_ERR_STATE` |
| `contract_course_info(c)` | `geas_course_info` | answers a `CourseInfo`, state, step, status and `wait_due_ms`; `course_state_name` spells a state |
| `contract_course_wait(c, timeout_ms)` | `geas_course_wait` | blocks until the course is not running, negative waits forever, a timeout is no error and reads `course_running()` at the step the course is on |
| `contract_set_autopark(c, dsn, key)`, `contract_clear_autopark(c)` | `geas_contract_set_autopark` | the instance parks itself after every change, clear is the NULL dsn form |

Every dusk name stays clear of the `geas_` prefix on purpose, a dusk function is emitted under its own name and one called `geas_something` in the executable would interpose the runtime's own symbol.

Errors are values. A non OK status comes back as an `error` whose message ends in the status name, `signal delivered: GEAS_ERR_STATE`, and `is_status(e, status_err_state())` tells you which one it was, since a dusk error carries a message and no code.

## Courses

So a contract with a `course` block is engine driven, the course starts on the runtime's pool the moment the contract signs or resumes, it calls the contract's own pledges through the same terms and retries a host fulfillment would get, and it suspends at a wait, an `await`, a `select`, a `together` whose calls are still out. The host side is small: bind the abstract bodies, sign, then ask where the course stands.

```dusk
saga, e := contract_sign(rt, "OrderCourse", vows)
info, we := contract_course_wait(saga, 5000)
if info.state == course_waiting() && info.step == 5 {
    de := contract_signal(saga, "delivered", values1(tracking))
}
```

`contract_course_wait` answers as soon as the course is not running, which is any rest and not only the one you are after, a course waiting on a `together` reads `course_waiting()` too, so loop until the state and step are the ones you want, the way `settle_at` in `demo_course.dusk` does. The states are `course_none()`, `course_running()`, `course_waiting()`, `course_finished()`, `course_halted()` and `course_cancelled()`, a halted course carries the status it stopped on in `info.status`, and a course that ends on a step's `?` reads finished, with the contract Broken and the remedies already run. A timeout is no error, it comes back with the info read as it stands, `course_running()` and the step the course is on, since every step boundary publishes the step, so the state tells a slow course from one at rest and an error only ever means the call itself failed.

Now the thing is a course waits on the clock, a `timeout` arm or a `sleep` falls due only when something ticks, so either call `runtime_tick` from a timer of your own or let `runtime_start_ticker(rt, 10)` do it on a runtime thread. A host fulfillment of a pledge on such a contract answers `GEAS_ERR_STATE` and the body never runs, signals are still yours to deliver. `contract_set_autopark(c, dsn, key)` makes the instance write its own park row after every step, so a process that dies resumes with `contract_resume` at the step it stored.

## Values

A `Value` is a heap tree, built with the `value_*` helpers, decoded from whatever the runtime hands back, and freed with `value_free`, which frees every child, string and vector under it. Everything that comes back from the runtime is copied, so a tree outlives the instance it came from, break or no break.

| Geas | `Value` | builder |
|---|---|---|
| `Unit` | `Unit` | `value_unit()` |
| `Int`, `Duration`, `Time` | `Int(i: int64)` | `value_int(i)` |
| `UInt` | `UInt(u: uint64)` | `value_uint(u)` |
| `Float` | `Float(f: float64)` | `value_float(f)` |
| `Bool` | `Bool(b: bool)` | `value_bool(b)` |
| `Byte` | `Byte(b: uint8)` | `value_byte(b)` |
| `Char` | `Char(c: rune)` | `value_char(c)` |
| `String` | `Text(s: string)` | `value_text(s)`, copied |
| `Bytes` | `Bytes(data: *Vector<uint8>)` | `value_bytes(v)` |
| `Option` | `None`, `Some(v)` | `value_none()`, `value_some(v)` |
| `Result` | `Ok(v)`, `Err(v)` | `value_ok(v)`, `value_err(v)` |
| `List<T>` | `List(elem_ty, items)` | `value_list(ty_int(), items)` |
| `Map<K, V>` | `Map(key_ty, pairs)` | `value_map(ty_string(), pairs)`, pairs flat as key, value |
| tuple | `Tuple(items)` | `value_tuple(items)` |
| record | `Record(fields)` | `value_record(fields)`, fields in declaration order |
| declared sum | `Sum(tag, fields)` | `value_variant(tag, fields)`, tag is the declaration index |

`values()`, `values1`, `values2` and `values3` build a frame or a field list, and `value_at`, `value_payload`, `value_as_text`, `value_as_int`, `value_is_ok`, `value_is_err` and `value_sum_tag` read one. `value_clone` deep copies a tree through the wire shape and `value_render` gives you the runtime's own debug spelling, `Ok({"RC-7", 100})`.

What does not cross: a `String` holding a NUL byte, since a dusk string ends at its first NUL, decodes to an error rather than a cut off string, and a List carries the element tag you hand it, it is not inferred, so an empty list still says what it holds. By reference arguments, `GeasRef`, are not exposed, nor are the iname table, the generated header lookups, `geas_partial_value`, or the event hook, so the course events, started, suspended, resumed and the rest, are read through `contract_course_info` from dusk rather than observed.

## Threads

A handler runs on whatever thread the runtime calls it from, a pool worker for a pledge body, the breaking thread for a remedy, any thread at all for the clock, which is how a dusk C callback always works. The dusk heap is thread safe there, the collector is not, so a handler or a clock must never mint a `collector<T>` or force a collection, that aborts by name off the main thread. The state a handler touches is shared with your host thread; a synchronous fulfillment blocks the host thread inside the runtime while the body runs, which orders the two, however a host that fulfills through receipts and reads that state before the wait has to guard it with a `std.concurrent` Mutex. The clock may be called with an instance lock held, so it must not call back into the runtime. With a course the bodies run on pool workers with no host call around them, the two calls of a `together` at the same time, and the ticker reads the clock from its own thread, so anything two of them touch wants a `std.concurrent.atomic` AtomicInt or a Mutex, which is why the demo's clock keeps its milliseconds in an AtomicInt. `contract_course_wait` is for the host thread only, from a handler or a clock it answers `GEAS_ERR_STATE` without waiting.

Rebinding a pledge leaves the old binding alive, an instance signed before the rebind still dispatches it, and the shim frees every binding and clock of a runtime only in `runtime_shutdown`, after the runtime has joined its workers.
