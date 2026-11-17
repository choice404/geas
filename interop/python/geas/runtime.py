"""Runtime loading, callbacks, discovery, and instance creation."""

import ctypes as C
import traceback

from ._abi import (
    GEAS_ERR_OOM,
    GEAS_ERR_TYPE,
    GEAS_INAME_CONTRACT,
    GEAS_OK,
    GeasAttrInfo,
    GeasClockFn,
    GeasEventFn,
    GeasInameEntry,
    GeasPledgeFn,
    GeasRuntimeConfig,
    GeasSleepFn,
    GeasValue,
    GeasVowBinding,
)
from ._encoding import _ValueEncoder
from ._prototypes import _declare
from .contract import Contract
from .errors import _check
from .events import Event
from .values import decode


class Runtime(_ValueEncoder):
    """One loaded libgeasrt and its worker pool.

    The library is opened with RTLD_GLOBAL on purpose: a compiled module
    carries undefined references to the runtime's exports and no library
    dependency of its own, so whoever maps libgeasrt must put its symbols in
    the global scope the runtime's dlopen resolves against. docs/abi.md,
    Loading the runtime dynamically.
    """

    def __init__(self, lib_path, max_threads=0, handshake_ms=0):
        self._lib = C.CDLL(str(lib_path), mode=C.RTLD_GLOBAL)
        self._declare(self._lib)
        # Python-made pledge trampolines the runtime holds raw pointers to.
        # The anchor list keeps them alive for the life of the runtime, the
        # binding lifetime rule the ABI states for FFI hosts.
        self._thunks = []
        rt = C.c_void_p()
        cfg = GeasRuntimeConfig(max_threads, handshake_ms)
        _check(self._lib.geas_runtime_init(C.byref(cfg), C.byref(rt)),
               "geas_runtime_init")
        self._rt = rt

    _declare = staticmethod(_declare)

    # ---- lifecycle ----

    def load(self, so_path):
        _check(self._lib.geas_module_load(self._rt, str(so_path).encode()),
               f"geas_module_load {so_path}")

    def freeze(self):
        _check(self._lib.geas_runtime_freeze(self._rt), "geas_runtime_freeze")

    def on_event(self, callback):
        """Observes transitions on their delivery thread, None clears the hook.

        Reads on event.instance do not tick during delivery. Fulfill, signal,
        break and park on that instance raise GEAS_ERR_STATE. Captured callbacks
        can finish after replacement, so their trampolines live until shutdown.
        Callback exceptions are printed and delivery continues.
        """
        def event_tramp(user, pointer):
            try:
                callback(Event(self, pointer.contents))
            except Exception:
                traceback.print_exc()

        event_fn = GeasEventFn(event_tramp) if callback is not None else GeasEventFn()
        self._thunks.append(event_fn)
        _check(self._lib.geas_runtime_set_event_hook(self._rt, event_fn, None),
               "geas_runtime_set_event_hook")

    def set_clock(self, now=None, sleep=None):
        """Points the runtime at a Python clock: now() answers wall clock
        milliseconds and sleep(ms) waits out a retry delay. Either left None
        restores the runtime's default. Both are called from any runtime
        thread and must not call back into the runtime, and both callbacks are
        anchored on this Runtime for its life, the same rule a trampoline
        keeps. An exception inside one is swallowed to a 0 reading and a
        skipped sleep, since a clock has no status to report it through."""
        def now_tramp(user):
            try:
                return int(now())
            except Exception:
                return 0

        def sleep_tramp(user, ms):
            try:
                sleep(ms)
            except Exception:
                pass

        now_fn = GeasClockFn(now_tramp) if now else GeasClockFn()
        sleep_fn = GeasSleepFn(sleep_tramp) if sleep else GeasSleepFn()
        self._thunks.extend([now_fn, sleep_fn])
        _check(self._lib.geas_runtime_set_clock(self._rt, now_fn, sleep_fn,
                                                None),
               "geas_runtime_set_clock")

    def tick(self):
        """Applies every deadline that has passed, over every instance."""
        _check(self._lib.geas_runtime_tick(self._rt), "geas_runtime_tick")

    def start_ticker(self, interval_ms):
        """Starts the runtime's own timer thread, a tick every interval_ms of
        real time, so deadlines and course waits fall due on time without the
        host calling tick. The tick reads the clock set_clock installed, so a
        Python clock is called from that thread too. A second start while one
        runs raises GEAS_ERR_STATE, an interval of 0 GEAS_ERR_TYPE."""
        _check(self._lib.geas_runtime_start_ticker(self._rt, interval_ms),
               "geas_runtime_start_ticker")

    def stop_ticker(self):
        """Stops the ticker and joins its thread before returning. With no
        ticker running it raises GEAS_ERR_STATE; shutdown stops a running
        ticker on its own."""
        _check(self._lib.geas_runtime_stop_ticker(self._rt),
               "geas_runtime_stop_ticker")

    def shutdown(self):
        if self._rt:
            self._lib.geas_runtime_shutdown(self._rt)
            self._rt = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.shutdown()
        return False

    # ---- binding Python implementations ----

    def bind(self, pledge_name, fn, raw=False):
        """Binds a Python callable as a pledge implementation.

        In the friendly form fn(contract, args) receives the signed instance
        and the decoded argument list and returns a Python value, Ok and Err
        included, which is encoded into instance owned memory. In the raw
        form fn(contract, args_ptr, nargs, out_ptr) sees the thunk frame
        itself and returns a status, the shape for an implementation that
        mutates a by-reference slot.

        The trampoline is anchored on this Runtime: the runtime stores the
        raw function pointer and nothing else, so the ctypes callback object
        must outlive every instance that can dispatch it, and here it lives
        exactly as long as the Runtime does. A Python exception inside the
        body reports GEAS_ERR_TYPE, the thunk produced no value.
        """
        if raw:
            def tramp(ctx, args, nargs, out):
                try:
                    return fn(Contract(self, ctx), args, nargs, out)
                except Exception:
                    return GEAS_ERR_TYPE
        else:
            def tramp(ctx, args, nargs, out):
                try:
                    inst = Contract(self, ctx)
                    py_args = [decode(args[k]) for k in range(nargs)]
                    result = fn(inst, py_args)
                    self._encode_owned(ctx, out[0], result)
                    return GEAS_OK
                except Exception:
                    return GEAS_ERR_TYPE
        cfn = GeasPledgeFn(tramp)
        self._thunks.append(cfn)
        _check(self._lib.geas_pledge_bind(self._rt, pledge_name.encode(), cfn),
               f"geas_pledge_bind {pledge_name}")

    # ---- signing ----

    def sign(self, contract_name, vows=None, expected_hash=0):
        """Signs a contract, vows as a dict of overrides, and returns the
        instance. The runtime copies every override at the call."""
        keep = []
        arr, n = None, 0
        if vows:
            n = len(vows)
            arr = (GeasVowBinding * n)()
            for k, (name, val) in enumerate(vows.items()):
                nb = name.encode()
                keep.append(nb)
                arr[k].name = nb
                self._encode_host(arr[k].value, val, keep)
        c = C.c_void_p()
        _check(self._lib.geas_contract_sign(self._rt, contract_name.encode(),
                                           arr, n, expected_hash, C.byref(c)),
               f"geas_contract_sign {contract_name}")
        del keep
        return Contract(self, c)

    def resume(self, dsn, key, expected_hash=0):
        """Resumes the parked instance key names in dsn and returns its
        contract handle. A nonzero expected hash pins the shape at resume."""
        c = C.c_void_p()
        _check(self._lib.geas_instance_resume(
            self._rt, dsn.encode(), key.encode(), expected_hash, C.byref(c)),
            f"geas_instance_resume {key}")
        return Contract(self, c)

    def claim(self, dsn, key, expected_hash=0):
        """Resumes the parked instance key names in dsn and takes its row
        out of the store in the same move, the one shot hand off. Of two
        claimers of one key exactly one gets the instance; the other raises
        GEAS_ERR_NAME, the row being gone. A row that will not build, a
        version skew included, raises its status and stays in the store."""
        c = C.c_void_p()
        _check(self._lib.geas_instance_claim(
            self._rt, dsn.encode(), key.encode(), expected_hash, C.byref(c)),
            f"geas_instance_claim {key}")
        return Contract(self, c)

    def attrs(self, contract):
        """Declared host settings, available before sign."""
        count = C.c_size_t()
        _check(self._lib.geas_attr_count(self._rt, contract.encode(), C.byref(count)),
               f"geas_attr_count {contract}")
        rows = []
        for index in range(count.value):
            row = GeasAttrInfo()
            _check(self._lib.geas_attr_at(self._rt, contract.encode(), index, C.byref(row)),
                   f"geas_attr_at {contract} {index}")
            rows.append(self._attr_py(row))
        return rows

    def attr(self, contract, key, item=None):
        """A setting's decoded value, or None when the key is absent."""
        value = C.POINTER(GeasValue)()
        if item is None:
            status = self._lib.geas_contract_attr(self._rt, contract.encode(),
                                                  key.encode(), C.byref(value))
        else:
            status = self._lib.geas_item_attr(self._rt, contract.encode(), item.encode(),
                                              key.encode(), C.byref(value))
        _check(status, f"attribute {contract} {key}")
        return decode(value.contents) if value else None

    @staticmethod
    def _attr_py(row):
        kinds = ("contract", "subcontract", "pledge", "signal", "remedy")
        return {"owner": kinds[row.owner],
                "owner_name": row.owner_name.decode() if row.owner_name else None,
                "key": row.key.decode(), "value": decode(row.value.contents)}

    # ---- the iname table ----

    def iname_count(self):
        return self._lib.geas_iname_count(self._rt)

    def iname_at(self, i):
        e = GeasInameEntry()
        _check(self._lib.geas_iname_at(self._rt, i, C.byref(e)),
               f"geas_iname_at {i}")
        return self._iname_py(e)

    def iname_lookup(self, mangled):
        e = GeasInameEntry()
        _check(self._lib.geas_iname_lookup(self._rt, mangled.encode(),
                                          C.byref(e)),
               f"geas_iname_lookup {mangled}")
        return self._iname_py(e)

    def iname_dump(self):
        """The canonical discovery text, sized by the two call protocol: a
        NULL buffer reports GEAS_ERR_OOM with the needed size, the second
        call writes the text."""
        need = C.c_size_t()
        st = self._lib.geas_iname_dump(self._rt, None, 0, C.byref(need))
        if st != GEAS_ERR_OOM:
            _check(st, "geas_iname_dump size")
            return ""
        buf = C.create_string_buffer(need.value)
        _check(self._lib.geas_iname_dump(self._rt, buf, need.value,
                                        C.byref(need)),
               "geas_iname_dump")
        return buf.value.decode("utf-8")

    @staticmethod
    def _iname_py(e):
        return {
            "mangled": e.mangled.decode() if e.mangled else None,
            "kind": "contract" if e.kind == GEAS_INAME_CONTRACT else "pledge",
            "contract": e.contract.decode() if e.contract else None,
            "symbol": e.symbol.decode() if e.symbol else None,
            "shape_hash": e.shape_hash,
            "version": e.version,
            "nargs": e.nargs,
        }
