# Geas from dusk

So this is the dusk host binding for libgeasrt, the same surface the Python binding gives you, written as a dusk module over the C ABI. You load a compiled contract module, bind dusk functions over the abstract pledges and remedies, sign, fulfill, signal, tick, and read the partial surface, and the runtime does the rest, the terms, the retries, the remedy pass, all of it.

Four files make up the binding, `geas.dusk` is the runtime surface, `geas_value.dusk` is the `Value` tree and its conversion to and from `GeasValue`, `geas_attrs.dusk` reads the declared settings, and `geas_shim.c` is the thin C side, which holds only what a dusk foreign block cannot say by itself: the union inside `GeasValue`, the trampolines the runtime calls back through, and the few calls that take a NULL. Every other runtime call is declared straight in `geas.dusk` and linked against libgeasrt.

## Running the demo

```sh
interop/dusk/run_demo.sh
```

It runs `make compiler runtime`, builds `skeleton/saga.geas` with `target/bin/geas`, builds `demo_saga.dusk` with dusk, and runs it. The demo walks the order saga twice, a happy path where authorize eats one Timeout through its retry term and the saga lands Fulfilled, and a break path where the carrier refuses and the remedies undo what landed newest first, release then refund, and it ends with `[demo_saga_dusk] ok`. When `dusk` is not on PATH the script prints a skip line and exits 0.

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
| `runtime_attrs(rt, contract)`, `runtime_attr(rt, contract, item, key)` | `geas_attr_count`, `geas_attr_at`, `geas_contract_attr`, `geas_item_attr` | in `geas_attrs`, item `""` is the contract, the bool says whether the key exists, free a list with `attrs_free` |
| `runtime_shutdown(rt)` | `geas_runtime_shutdown` | then frees every binding and clock the shim held |
| `contract_sign(rt, name, vows)`, `contract_sign_pinned(..., hash)` | `geas_contract_sign` | vows are a `*Vector<Vow>`, copied at the call |
| `contract_resume(rt, dsn, key, hash)`, `contract_park(c, dsn, key)` | `geas_instance_resume`, `geas_instance_park` | bind on the new runtime before resuming |
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

Every dusk name stays clear of the `geas_` prefix on purpose, a dusk function is emitted under its own name and one called `geas_something` in the executable would interpose the runtime's own symbol.

Errors are values. A non OK status comes back as an `error` whose message ends in the status name, `signal delivered: GEAS_ERR_STATE`, and `is_status(e, status_err_state())` tells you which one it was, since a dusk error carries a message and no code.

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

What does not cross: a `String` holding a NUL byte, since a dusk string ends at its first NUL, decodes to an error rather than a cut off string, and a List carries the element tag you hand it, it is not inferred, so an empty list still says what it holds. By reference arguments, `GeasRef`, are not exposed, nor are the iname table, the generated header lookups, or `geas_partial_value`.

## Threads

A handler runs on whatever thread the runtime calls it from, a pool worker for a pledge body, the breaking thread for a remedy, any thread at all for the clock, which is how a dusk C callback always works. The dusk heap is thread safe there, the collector is not, so a handler or a clock must never mint a `collector<T>` or force a collection, that aborts by name off the main thread. The state a handler touches is shared with your host thread; a synchronous fulfillment blocks the host thread inside the runtime while the body runs, which orders the two, however a host that fulfills through receipts and reads that state before the wait has to guard it with a `std.concurrent` Mutex. The clock may be called with an instance lock held, so it must not call back into the runtime.

Rebinding a pledge leaves the old binding alive, an instance signed before the rebind still dispatches it, and the shim frees every binding and clock of a runtime only in `runtime_shutdown`, after the runtime has joined its workers.
