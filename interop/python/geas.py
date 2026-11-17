"""geas.py: a ctypes binding for libgeasrt, the Geas intermediary runtime.

This file is the product claim made literal: Python is not C, geas knows
nothing about it, and nothing here was generated from a header. Every struct
layout, calling convention, and ownership rule below was written against
docs/abi.md alone; the C headers are the source of the ABI but this binding
never includes them. If something in here needed a fact the document did not
state, that was a documentation bug, and the fix went into docs/abi.md, not
into a private workaround.

The shape of the binding follows the shape of the ABI. A Runtime owns the
loaded library, the worker pool behind it, and the anchor list that keeps
Python-made pledge trampolines alive. A Contract is a signed instance:
fulfill pledges on it, read its vows and its partial surface, break it. A
Future is one fulfillment's receipt, delivered exactly once by wait. Values
cross as GeasValue and are decoded into ordinary Python data at the boundary,
with Ok, Err, Some, and NONE standing in for the sum shaped types.

Ownership is the ABI's one rule, restated for a garbage collected host:
everything a fulfillment builds lives on the instance and dies at break, so
decode what you want to keep before breaking, and never hold a raw pointer
across a break. Strings and payloads returned by this binding are always
copied into Python objects at the call, so the ordinary user never sees
instance memory at all.
"""

import ctypes as C
import traceback

# ---------------------------------------------------------------------------
# The ABI constants. docs/abi.md, Values and Status codes: the numeric values
# are normative and fixed, declaration order in the enums.
# ---------------------------------------------------------------------------

GEAS_TY_UNIT = 0
GEAS_TY_INT = 1
GEAS_TY_UINT = 2
GEAS_TY_FLOAT = 3
GEAS_TY_BOOL = 4
GEAS_TY_BYTE = 5
GEAS_TY_CHAR = 6
GEAS_TY_STRING = 7
GEAS_TY_LIST = 8
GEAS_TY_MAP = 9
GEAS_TY_TUPLE = 10
GEAS_TY_OPTION = 11
GEAS_TY_RESULT = 12
GEAS_TY_RECORD = 13
GEAS_TY_PLEDGE_REF = 14
GEAS_TY_SUM = 15
GEAS_TY_BYTES = 17

GEAS_OK = 0
GEAS_ERR_STATE = 1
GEAS_ERR_TYPE = 2
GEAS_ERR_VERSION = 3
GEAS_ERR_UNBOUND = 4
GEAS_ERR_NAME = 5
GEAS_ERR_DEADLOCK = 6
GEAS_ERR_OOM = 7
GEAS_ERR_LOAD = 8
GEAS_ERR_NET = 9
GEAS_ERR_STORE = 10

_STATUS_NAMES = {
    GEAS_OK: "GEAS_OK",
    GEAS_ERR_STATE: "GEAS_ERR_STATE",
    GEAS_ERR_TYPE: "GEAS_ERR_TYPE",
    GEAS_ERR_VERSION: "GEAS_ERR_VERSION",
    GEAS_ERR_UNBOUND: "GEAS_ERR_UNBOUND",
    GEAS_ERR_NAME: "GEAS_ERR_NAME",
    GEAS_ERR_DEADLOCK: "GEAS_ERR_DEADLOCK",
    GEAS_ERR_OOM: "GEAS_ERR_OOM",
    GEAS_ERR_LOAD: "GEAS_ERR_LOAD",
    GEAS_ERR_NET: "GEAS_ERR_NET",
    GEAS_ERR_STORE: "GEAS_ERR_STORE",
}

GEAS_UNSIGNED = 0
GEAS_SIGNED = 1
GEAS_FULFILLED = 2
GEAS_PARTIAL = 3
GEAS_BROKEN = 4
GEAS_REMEDYING = 5

STATE_NAMES = {
    GEAS_UNSIGNED: "unsigned",
    GEAS_SIGNED: "signed",
    GEAS_FULFILLED: "fulfilled",
    GEAS_PARTIAL: "partial",
    GEAS_BROKEN: "broken",
    GEAS_REMEDYING: "remedying",
}

# A remedy's progress, docs/abi.md, Remedies.
GEAS_REMEDY_IDLE = 0
GEAS_REMEDY_RUNNING = 1
GEAS_REMEDY_DONE = 2
GEAS_REMEDY_FAILED = 3

REMEDY_STATE_NAMES = {
    GEAS_REMEDY_IDLE: "idle",
    GEAS_REMEDY_RUNNING: "running",
    GEAS_REMEDY_DONE: "done",
    GEAS_REMEDY_FAILED: "failed",
}

GEAS_ITEM_PENDING = 0
GEAS_ITEM_FULFILLED = 1
GEAS_ITEM_BROKEN = 2

GEAS_EVENT_LATCH_FULFILLED = 1
GEAS_EVENT_LATCH_BROKEN = 2
GEAS_EVENT_LATCH_LAPSED = 3
GEAS_EVENT_SIGNAL_DELIVERED = 4
GEAS_EVENT_REMEDY_STARTED = 5
GEAS_EVENT_REMEDY_DONE = 6
GEAS_EVENT_REMEDY_FAILED = 7
GEAS_EVENT_STATE_CHANGED = 8
GEAS_EVENT_TERM_LAPSED = 9
GEAS_EVENT_PARKED = 10
GEAS_EVENT_RESUMED = 11
GEAS_EVENT_CANCELLED = 12
GEAS_EVENT_RECLAIMED = 13
GEAS_EVENT_LOST = 14

GEAS_INAME_CONTRACT = 0
GEAS_INAME_PLEDGE = 1

# ---------------------------------------------------------------------------
# The wire structs, straight from docs/abi.md. Natural C alignment on LP64,
# no packing anywhere, and the sizeof asserts at the bottom of this section
# pin the layouts this binding computed to the ones the document promises.
# ---------------------------------------------------------------------------


class GeasString(C.Structure):
    """UTF-8 bytes behind a pointer, length in bytes, no terminator."""

    _fields_ = [("ptr", C.c_void_p), ("len", C.c_uint64)]


class GeasList(C.Structure):
    """A contiguous GeasValue array: data, live count, capacity, element tag."""

    _fields_ = [
        ("data", C.c_void_p),
        ("len", C.c_uint64),
        ("cap", C.c_uint64),
        ("elem_ty", C.c_uint32),
    ]


class _AshValueArm(C.Union):
    _fields_ = [
        ("i", C.c_int64),
        ("u", C.c_uint64),
        ("f", C.c_double),
        ("b", C.c_uint8),
        ("ch", C.c_uint32),
        ("s", GeasString),
        ("list", GeasList),
        ("box", C.c_void_p),
    ]


class GeasValue(C.Structure):
    """The one value shape: a type tag, a variant tag, and the union arm."""

    _fields_ = [("ty", C.c_uint32), ("tag", C.c_uint32), ("as_", _AshValueArm)]


class GeasAttrInfo(C.Structure):
    """One borrowed host setting, copied into Python at the boundary."""

    _fields_ = [("owner", C.c_uint32), ("owner_name", C.c_char_p),
                ("key", C.c_char_p), ("value", C.POINTER(GeasValue))]


class GeasVowBinding(C.Structure):
    """A vow override at sign; the runtime copies the value, keeps nothing."""

    _fields_ = [("name", C.c_char_p), ("value", GeasValue)]


class GeasInameEntry(C.Structure):
    """One row of the discovery table, copied out by lookup and at."""

    _fields_ = [
        ("mangled", C.c_char_p),
        ("kind", C.c_uint32),
        ("contract", C.c_char_p),
        ("symbol", C.c_char_p),
        ("shape_hash", C.c_uint64),
        ("version", C.c_uint32),
        ("nargs", C.c_uint32),
    ]


class GeasRuntimeConfig(C.Structure):
    """Pool sizing. max_threads of 0 selects the default of four workers.
    handshake_ms is reserved: the layout is pinned, so the two uint32 fields
    stay in declaration order with nothing packed between them and a foreign
    host that rebuilds this struct writes exactly these eight bytes."""

    _fields_ = [("max_threads", C.c_uint32), ("handshake_ms", C.c_uint32)]


# The uniform thunk frame. Every pledge, compiled or Python-bound, crosses in
# this one shape: status back, ctx is the signed instance, args the frame,
# out the declared value. GeasStatus and size_t ride as c_int and c_size_t.
GeasPledgeFn = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(GeasValue), C.c_size_t,
                          C.POINTER(GeasValue))

GeasWriteBackFn = C.CFUNCTYPE(None, C.c_void_p, C.POINTER(GeasValue), C.c_void_p)

# The clock pair a host may hand the runtime, docs/abi.md, The clock and the
# tick: now in wall clock milliseconds, and a sleep through a retry delay.
GeasClockFn = C.CFUNCTYPE(C.c_int64, C.c_void_p)
GeasSleepFn = C.CFUNCTYPE(None, C.c_void_p, C.c_uint64)


class GeasEvent(C.Structure):
    """The permanent event prefix, followed by any future producer fields."""

    _fields_ = [
        ("size", C.c_uint32),
        ("kind", C.c_uint32),
        ("instance", C.c_void_p),
        ("contract", C.c_char_p),
        ("item", C.c_char_p),
        ("old_state", C.c_uint32),
        ("new_state", C.c_uint32),
        ("at_ms", C.c_int64),
        ("seq", C.c_uint64),
    ]


GeasEventFn = C.CFUNCTYPE(None, C.c_void_p, C.POINTER(GeasEvent))


class Event:
    """One transition, copied into Python with its borrowed Contract handle."""

    def __init__(self, runtime, event):
        self.kind = event.kind
        self.instance = Contract(runtime, event.instance)
        self.contract = event.contract.decode()
        self.item = event.item.decode() if event.item else None
        self.old_state = event.old_state
        self.new_state = event.new_state
        self.at_ms = event.at_ms
        self.seq = event.seq


class GeasRef(C.Structure):
    """A by-reference argument: host storage, its type, and the write back."""

    _fields_ = [
        ("host_ptr", C.c_void_p),
        ("ty", C.c_uint32),
        ("cap", C.c_uint64),
        ("write_back", GeasWriteBackFn),
        ("user", C.c_void_p),
    ]


# The layouts docs/abi.md promises for LP64. A failure here means this file
# and the document disagree, which is a bug in exactly one of them.
assert C.sizeof(GeasString) == 16
assert C.sizeof(GeasList) == 32
assert C.sizeof(GeasValue) == 40
assert C.sizeof(GeasRef) == 40
assert C.sizeof(GeasInameEntry) == 48
assert C.sizeof(GeasRuntimeConfig) == 8

# ---------------------------------------------------------------------------
# The sum shaped values, seen from Python. Ok and Err wrap a Result's payload,
# Some wraps an Option's, and NONE is the one None variant, a distinct
# sentinel because Python's None already means Unit at this boundary.
# ---------------------------------------------------------------------------


class Ok:
    __slots__ = ("value",)

    def __init__(self, value):
        self.value = value

    def __repr__(self):
        return f"Ok({self.value!r})"

    def __eq__(self, other):
        return isinstance(other, Ok) and other.value == self.value


class Err:
    __slots__ = ("value",)

    def __init__(self, value):
        self.value = value

    def __repr__(self):
        return f"Err({self.value!r})"

    def __eq__(self, other):
        return isinstance(other, Err) and other.value == self.value


class Some:
    __slots__ = ("value",)

    def __init__(self, value):
        self.value = value

    def __repr__(self):
        return f"Some({self.value!r})"

    def __eq__(self, other):
        return isinstance(other, Some) and other.value == self.value


class Record:
    """A record value built from Python, its fields in declaration order.
    A record decodes back as a plain list, so this is only needed going in."""

    def __init__(self, *fields):
        self.fields = list(fields)

    def __repr__(self):
        return f"Record({', '.join(repr(f) for f in self.fields)})"


class Variant:
    """A sum value built from Python: the variant's declaration index in its
    sum and its fields. A sum decodes back as the (tag, fields) pair."""

    def __init__(self, tag, *fields):
        self.tag = tag
        self.fields = list(fields)

    def __repr__(self):
        return f"Variant({self.tag}, {self.fields})"


class _NoneVariant:
    _instance = None

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance

    def __repr__(self):
        return "NONE"


NONE = _NoneVariant()


class GeasError(RuntimeError):
    """A non-OK status from the runtime, carrying the status word."""

    def __init__(self, status, what):
        name = _STATUS_NAMES.get(status, str(status))
        super().__init__(f"{what}: {name}")
        self.status = status
        self.status_name = name


def _check(status, what):
    if status != GEAS_OK:
        raise GeasError(status, what)


# ---------------------------------------------------------------------------
# Decoding: GeasValue to Python. Everything is copied out, strings included,
# so nothing the caller holds afterwards points into instance memory.
# ---------------------------------------------------------------------------


def decode(v):
    """Decodes one GeasValue into ordinary Python data, deeply and by copy."""
    ty = v.ty
    if ty == GEAS_TY_UNIT:
        return None
    if ty == GEAS_TY_INT:
        return v.as_.i
    if ty == GEAS_TY_UINT:
        return v.as_.u
    if ty == GEAS_TY_FLOAT:
        return v.as_.f
    if ty == GEAS_TY_BOOL:
        return bool(v.as_.b)
    if ty == GEAS_TY_BYTE:
        return v.as_.b
    if ty == GEAS_TY_CHAR:
        return chr(v.as_.ch)
    if ty == GEAS_TY_BYTES:
        return C.string_at(v.as_.s.ptr, v.as_.s.len) if v.as_.s.len else b""
    if ty == GEAS_TY_STRING:
        if not v.as_.s.ptr or v.as_.s.len == 0:
            return ""
        return C.string_at(v.as_.s.ptr, v.as_.s.len).decode("utf-8")
    if ty in (GEAS_TY_LIST, GEAS_TY_TUPLE, GEAS_TY_RECORD, GEAS_TY_SUM):
        n = v.as_.list.len
        elems = []
        if v.as_.list.data and n:
            arr = C.cast(v.as_.list.data, C.POINTER(GeasValue))
            elems = [decode(arr[i]) for i in range(n)]
        if ty == GEAS_TY_TUPLE:
            return tuple(elems)
        if ty == GEAS_TY_SUM:
            return (v.tag, elems)
        return elems
    if ty == GEAS_TY_OPTION:
        if v.tag == 0 or not v.as_.box:
            return NONE
        return Some(decode(C.cast(v.as_.box, C.POINTER(GeasValue))[0]))
    if ty == GEAS_TY_RESULT:
        payload = None
        if v.as_.box:
            payload = decode(C.cast(v.as_.box, C.POINTER(GeasValue))[0])
        return Ok(payload) if v.tag == 0 else Err(payload)
    raise GeasError(GEAS_ERR_TYPE, f"cannot decode type tag {ty}")


# ---------------------------------------------------------------------------
# By-reference arguments. Each Ref owns the host side storage the runtime
# copies in from and writes back to; read .value after the wait delivered.
# ---------------------------------------------------------------------------


class IntRef:
    """An Int passed by reference: host storage the delivery writes back."""

    ty = GEAS_TY_INT

    def __init__(self, initial=0):
        self._storage = C.c_int64(int(initial))

    def _host_ptr(self):
        return C.cast(C.byref(self._storage), C.c_void_p)

    @property
    def value(self):
        return self._storage.value


class FloatRef:
    ty = GEAS_TY_FLOAT

    def __init__(self, initial=0.0):
        self._storage = C.c_double(float(initial))

    def _host_ptr(self):
        return C.cast(C.byref(self._storage), C.c_void_p)

    @property
    def value(self):
        return self._storage.value


class StringRef:
    """A String passed by reference. The default write back repoints the
    GeasString at instance owned bytes, so .value copies them into Python at
    the read; read it after the wait and before the break, per the ABI."""

    ty = GEAS_TY_STRING

    def __init__(self, initial=""):
        raw = initial.encode("utf-8")
        self._buf = C.create_string_buffer(raw, max(len(raw), 1))
        self._storage = GeasString(C.cast(self._buf, C.c_void_p), len(raw))

    def _host_ptr(self):
        return C.cast(C.byref(self._storage), C.c_void_p)

    @property
    def value(self):
        if not self._storage.ptr or self._storage.len == 0:
            return ""
        return C.string_at(self._storage.ptr, self._storage.len).decode("utf-8")


# ---------------------------------------------------------------------------
# The runtime.
# ---------------------------------------------------------------------------


class Runtime:
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

    @staticmethod
    def _declare(lib):
        """Prototypes, each one spelled in docs/abi.md."""
        v, i, z = C.c_void_p, C.c_int, C.c_size_t
        s, VP = C.c_char_p, C.POINTER(GeasValue)
        lib.geas_runtime_init.argtypes = [C.POINTER(GeasRuntimeConfig),
                                         C.POINTER(v)]
        lib.geas_runtime_init.restype = i
        lib.geas_runtime_shutdown.argtypes = [v]
        lib.geas_runtime_shutdown.restype = None
        lib.geas_runtime_set_event_hook.argtypes = [v, GeasEventFn, v]
        lib.geas_runtime_set_event_hook.restype = i
        lib.geas_module_load.argtypes = [v, s]
        lib.geas_module_load.restype = i
        lib.geas_runtime_freeze.argtypes = [v]
        lib.geas_runtime_freeze.restype = i
        lib.geas_pledge_bind.argtypes = [v, s, GeasPledgeFn]
        lib.geas_pledge_bind.restype = i
        lib.geas_contract_sign.argtypes = [v, s, C.POINTER(GeasVowBinding), z,
                                          C.c_uint64, C.POINTER(v)]
        lib.geas_contract_sign.restype = i
        lib.geas_instance_park.argtypes = [v, s, s]
        lib.geas_instance_park.restype = i
        lib.geas_instance_resume.argtypes = [v, s, s, C.c_uint64,
                                            C.POINTER(v)]
        lib.geas_instance_resume.restype = i
        lib.geas_contract_state.argtypes = [v]
        lib.geas_contract_state.restype = i
        lib.geas_contract_hash.argtypes = [v]
        lib.geas_contract_hash.restype = C.c_uint64
        lib.geas_contract_signed_at.argtypes = [v]
        lib.geas_contract_signed_at.restype = C.c_int64
        lib.geas_contract_break.argtypes = [v]
        lib.geas_contract_break.restype = i
        lib.geas_cancel_requested.argtypes = [v]
        lib.geas_cancel_requested.restype = i
        lib.geas_pledge_fulfill.argtypes = [v, s, VP, z, C.POINTER(GeasRef), z]
        lib.geas_pledge_fulfill.restype = v
        lib.geas_future_wait.argtypes = [v, VP]
        lib.geas_future_wait.restype = i
        lib.geas_future_poll.argtypes = [v, C.POINTER(i)]
        lib.geas_future_poll.restype = i
        lib.geas_future_release.argtypes = [v]
        lib.geas_future_release.restype = i
        lib.geas_pledge_fulfill_sync.argtypes = [v, s, VP, z,
                                                C.POINTER(GeasRef), z, VP]
        lib.geas_pledge_fulfill_sync.restype = i
        lib.geas_vow_ref.argtypes = [v, s]
        lib.geas_vow_ref.restype = VP
        lib.geas_partial_count.argtypes = [v, i]
        lib.geas_partial_count.restype = z
        lib.geas_partial_name.argtypes = [v, i, z]
        lib.geas_partial_name.restype = s
        lib.geas_partial_nerrors.argtypes = [v]
        lib.geas_partial_nerrors.restype = z
        lib.geas_partial_error.argtypes = [v, z, C.POINTER(s), C.POINTER(VP)]
        lib.geas_partial_error.restype = i
        lib.geas_attr_count.argtypes = [v, s, C.POINTER(z)]
        lib.geas_attr_count.restype = i
        lib.geas_attr_at.argtypes = [v, s, z, C.POINTER(GeasAttrInfo)]
        lib.geas_attr_at.restype = i
        lib.geas_contract_attr.argtypes = [v, s, s, C.POINTER(VP)]
        lib.geas_contract_attr.restype = i
        lib.geas_item_attr.argtypes = [v, s, s, s, C.POINTER(VP)]
        lib.geas_item_attr.restype = i
        lib.geas_instance_attr_count.argtypes = [v, C.POINTER(z)]
        lib.geas_instance_attr_count.restype = i
        lib.geas_instance_attr_at.argtypes = [v, z, C.POINTER(GeasAttrInfo)]
        lib.geas_instance_attr_at.restype = i
        lib.geas_instance_attr.argtypes = [v, s, s, C.POINTER(VP)]
        lib.geas_instance_attr.restype = i
        lib.geas_iname_lookup.argtypes = [v, s, C.POINTER(GeasInameEntry)]
        lib.geas_iname_lookup.restype = i
        lib.geas_iname_count.argtypes = [v]
        lib.geas_iname_count.restype = z
        lib.geas_iname_at.argtypes = [v, z, C.POINTER(GeasInameEntry)]
        lib.geas_iname_at.restype = i
        lib.geas_iname_dump.argtypes = [v, C.c_char_p, z, C.POINTER(z)]
        lib.geas_iname_dump.restype = i
        lib.geas_box.argtypes = [v]
        lib.geas_box.restype = VP
        lib.geas_string_copy.argtypes = [v, s, C.c_uint64]
        lib.geas_string_copy.restype = GeasValue
        lib.geas_bytes_copy_into.argtypes = [v, s, C.c_uint64, VP]
        lib.geas_bytes_copy_into.restype = i
        lib.geas_list_new.argtypes = [v, C.c_uint32, C.c_uint64, VP]
        lib.geas_list_new.restype = i
        lib.geas_list_push.argtypes = [v, VP, VP]
        lib.geas_list_push.restype = i
        lib.geas_bytes.argtypes = [v, C.c_uint64]
        lib.geas_bytes.restype = C.c_void_p
        # The workflow surface: signals, the clock and the tick, lapses, and
        # the remedy partial surface
        lib.geas_signal.argtypes = [v, s, VP, z]
        lib.geas_signal.restype = i
        lib.geas_signal_payload.argtypes = [v, s, C.POINTER(VP)]
        lib.geas_signal_payload.restype = i
        lib.geas_runtime_set_clock.argtypes = [v, GeasClockFn, GeasSleepFn, v]
        lib.geas_runtime_set_clock.restype = i
        lib.geas_instance_tick.argtypes = [v]
        lib.geas_instance_tick.restype = i
        lib.geas_runtime_tick.argtypes = [v]
        lib.geas_runtime_tick.restype = i
        lib.geas_partial_lapsed.argtypes = [v, s]
        lib.geas_partial_lapsed.restype = C.c_int
        lib.geas_contract_term_lapsed.argtypes = [v]
        lib.geas_contract_term_lapsed.restype = C.c_int
        lib.geas_remedy_state.argtypes = [v, s]
        lib.geas_remedy_state.restype = C.c_int
        lib.geas_partial_remedy_count.argtypes = [v]
        lib.geas_partial_remedy_count.restype = z
        lib.geas_partial_remedy_name.argtypes = [v, z]
        lib.geas_partial_remedy_name.restype = s
        lib.geas_partial_remedy_status.argtypes = [v, z]
        lib.geas_partial_remedy_status.restype = i
        lib.geas_partial_remedy_lapsed.argtypes = [v, z]
        lib.geas_partial_remedy_lapsed.restype = C.c_int
        lib.geas_partial_remedy_error.argtypes = [v, z, C.POINTER(VP)]
        lib.geas_partial_remedy_error.restype = i

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

    # ---- value encoding ----

    def _encode_host(self, target, py, keep):
        """Encodes a Python scalar or string into target using host owned
        memory; the runtime deep copies at the call, so keep only needs to
        hold the buffers until the call returns. bool tests first because a
        Python bool is an int."""
        C.memset(C.byref(target), 0, C.sizeof(target))
        if isinstance(py, bool):
            target.ty = GEAS_TY_BOOL
            target.as_.b = 1 if py else 0
        elif isinstance(py, int):
            target.ty = GEAS_TY_INT
            target.as_.i = py
        elif isinstance(py, float):
            target.ty = GEAS_TY_FLOAT
            target.as_.f = py
        elif isinstance(py, (str, bytes, bytearray)):
            raw = py.encode("utf-8") if isinstance(py, str) else bytes(py)
            buf = C.create_string_buffer(raw, max(len(raw), 1))
            keep.append(buf)
            target.ty = GEAS_TY_STRING if isinstance(py, str) else GEAS_TY_BYTES
            target.as_.s.ptr = C.cast(buf, C.c_void_p)
            target.as_.s.len = len(raw)
        else:
            raise GeasError(GEAS_ERR_TYPE,
                           f"cannot pass {type(py).__name__} by host memory")

    def _encode_owned(self, ctx, target, py):
        """Encodes a Python value into target using instance owned memory,
        the form a pledge body's out and a list's elements require."""
        _TAGS = {int: GEAS_TY_INT, float: GEAS_TY_FLOAT, bool: GEAS_TY_BOOL}
        C.memset(C.byref(target), 0, C.sizeof(target))
        if py is None:
            target.ty = GEAS_TY_UNIT
        elif isinstance(py, bool):
            target.ty = GEAS_TY_BOOL
            target.as_.b = 1 if py else 0
        elif isinstance(py, int):
            target.ty = GEAS_TY_INT
            target.as_.i = py
        elif isinstance(py, float):
            target.ty = GEAS_TY_FLOAT
            target.as_.f = py
        elif isinstance(py, str):
            raw = py.encode("utf-8")
            sv = self._lib.geas_string_copy(ctx, raw, len(raw))
            C.memmove(C.byref(target), C.byref(sv), C.sizeof(GeasValue))
        elif isinstance(py, (bytes, bytearray)):
            raw = bytes(py)
            _check(self._lib.geas_bytes_copy_into(ctx, raw, len(raw), C.byref(target)),
                   "geas_bytes_copy_into")
        elif isinstance(py, (Ok, Err)):
            box = self._lib.geas_box(ctx)
            if not box:
                raise GeasError(GEAS_ERR_OOM, "geas_box")
            self._encode_owned(ctx, box[0], py.value)
            target.ty = GEAS_TY_RESULT
            target.tag = 0 if isinstance(py, Ok) else 1
            target.as_.box = C.cast(box, C.c_void_p)
        elif isinstance(py, Some):
            box = self._lib.geas_box(ctx)
            if not box:
                raise GeasError(GEAS_ERR_OOM, "geas_box")
            self._encode_owned(ctx, box[0], py.value)
            target.ty = GEAS_TY_OPTION
            target.tag = 1
            target.as_.box = C.cast(box, C.c_void_p)
        elif py is NONE:
            target.ty = GEAS_TY_OPTION
        elif isinstance(py, (Record, Variant)):
            self._encode_fields(ctx, target, py.fields)
            target.ty = GEAS_TY_RECORD if isinstance(py, Record) else GEAS_TY_SUM
            target.tag = py.tag if isinstance(py, Variant) else 0
        elif isinstance(py, list):
            if not py:
                raise GeasError(GEAS_ERR_TYPE, "cannot infer an empty list's "
                                             "element type")
            elem_ty = _TAGS.get(bool if isinstance(py[0], bool)
                                else type(py[0]))
            if isinstance(py[0], str):
                elem_ty = GEAS_TY_STRING
            if isinstance(py[0], (bytes, bytearray)):
                elem_ty = GEAS_TY_BYTES
            if elem_ty is None:
                raise GeasError(GEAS_ERR_TYPE,
                               f"unsupported list element {type(py[0])}")
            _check(self._lib.geas_list_new(ctx, elem_ty, len(py), target),
                   "geas_list_new")
            elem = GeasValue()
            for x in py:
                self._encode_owned(ctx, elem, x)
                _check(self._lib.geas_list_push(ctx, target, C.byref(elem)),
                       "geas_list_push")
        else:
            raise GeasError(GEAS_ERR_TYPE,
                           f"cannot encode {type(py).__name__}")


    def _encode_fields(self, ctx, target, fields):
        """Lays a record's or a variant's fields out in an instance owned
        array, the shape both carry on the wire."""
        if not fields:
            return
        n = len(fields)
        raw = self._lib.geas_bytes(ctx, n * C.sizeof(GeasValue))
        if not raw:
            raise GeasError(GEAS_ERR_OOM, "geas_bytes")
        arr = C.cast(raw, C.POINTER(GeasValue))
        for k, field in enumerate(fields):
            self._encode_owned(ctx, arr[k], field)
        target.as_.list.data = raw
        target.as_.list.len = n
        target.as_.list.cap = n


# ---------------------------------------------------------------------------
# Contracts and futures.
# ---------------------------------------------------------------------------


class PartialResult:
    """A snapshot of the partial surface: item names by state, and the
    (pledge name, decoded payload) pair of every broken pledge."""

    def __init__(self, pending, fulfilled, broken, errors):
        self.pending = pending
        self.fulfilled = fulfilled
        self.broken = broken
        self.errors = errors

    def __repr__(self):
        return (f"PartialResult(pending={self.pending}, "
                f"fulfilled={self.fulfilled}, broken={self.broken}, "
                f"errors={self.errors})")


class Future:
    """One fulfillment's receipt. wait() delivers exactly once; the Refs it
    carries are anchored here so their storage survives until the write
    back the delivering wait performs."""

    def __init__(self, rt, ptr, refs):
        self._rt = rt
        self._ptr = ptr
        self._refs = refs

    def wait(self):
        out = GeasValue()
        _check(self._rt._lib.geas_future_wait(self._ptr, C.byref(out)),
               "geas_future_wait")
        return decode(out)

    def poll(self):
        """Whether an outcome exists, without delivering or writing refs."""
        ready = C.c_int()
        _check(self._rt._lib.geas_future_poll(self._ptr, C.byref(ready)),
               "geas_future_poll")
        return bool(ready.value)

    def release(self):
        """Releases a waited or forfeited receipt and invalidates its handle."""
        _check(self._rt._lib.geas_future_release(self._ptr),
               "geas_future_release")
        self._ptr = None
        self._refs = None


class Contract:
    """A signed instance. Thin on purpose: every method is one runtime call
    plus the decode at the boundary."""

    def __init__(self, rt, ptr):
        self._rt = rt
        self._ptr = ptr

    def attrs(self):
        count = C.c_size_t()
        _check(self._rt._lib.geas_instance_attr_count(self._ptr, C.byref(count)),
               "geas_instance_attr_count")
        rows = []
        for index in range(count.value):
            row = GeasAttrInfo()
            _check(self._rt._lib.geas_instance_attr_at(self._ptr, index, C.byref(row)),
                   f"geas_instance_attr_at {index}")
            rows.append(self._rt._attr_py(row))
        return rows

    def attr(self, key, item=None):
        value = C.POINTER(GeasValue)()
        _check(self._rt._lib.geas_instance_attr(
            self._ptr, item.encode() if item is not None else None,
            key.encode(), C.byref(value)), f"geas_instance_attr {key}")
        return decode(value.contents) if value else None

    # ---- state and signature ----

    def state(self):
        return self._rt._lib.geas_contract_state(self._ptr)

    def state_name(self):
        return STATE_NAMES.get(self.state(), "?")

    def hash(self):
        return self._rt._lib.geas_contract_hash(self._ptr)

    def signed_at(self):
        return self._rt._lib.geas_contract_signed_at(self._ptr)

    def break_(self):
        _check(self._rt._lib.geas_contract_break(self._ptr),
               "geas_contract_break")

    def cancel_requested(self):
        return bool(self._rt._lib.geas_cancel_requested(self._ptr))

    def park(self, dsn, key):
        """Snapshots this instance, including retry waits and remedy progress.

        Original futures keep waiting. Resumed pending pledges need arguments
        again and retain their retry budget and absolute due time.
        """
        _check(self._rt._lib.geas_instance_park(self._ptr, dsn.encode(),
                                                key.encode()),
               f"geas_instance_park {key}")

    # ---- vows ----

    def vow(self, name):
        p = self._rt._lib.geas_vow_ref(self._ptr, name.encode())
        if not p:
            raise GeasError(GEAS_ERR_NAME, f"vow {name}")
        return decode(p[0])

    # ---- fulfillment ----

    def _frame(self, args, refs):
        keep = []
        arr, n = None, len(args)
        if n:
            arr = (GeasValue * n)()
            for k, a in enumerate(args):
                if isinstance(a, (list, Record, Variant)):
                    self._rt._encode_owned(self._ptr, arr[k], a)
                else:
                    self._rt._encode_host(arr[k], a, keep)
        rarr, rn = None, 0
        if refs:
            rn = len(refs)
            rarr = (GeasRef * rn)()
            for k, r in enumerate(refs):
                rarr[k].host_ptr = r._host_ptr()
                rarr[k].ty = r.ty
        return arr, n, rarr, rn, keep

    def fulfill_sync(self, pledge_name, *args, refs=None):
        """Fulfill and wait fused. Returns the decoded value; raises
        GeasError when the fulfillment itself did not run, a pledge's Err
        being a value, not an error. The C call releases its own receipt
        before returning, including when the wait reports an error."""
        arr, n, rarr, rn, keep = self._frame(args, refs)
        out = GeasValue()
        _check(self._rt._lib.geas_pledge_fulfill_sync(
            self._ptr, pledge_name.encode(), arr, n, rarr, rn, C.byref(out)),
            f"fulfill {pledge_name}")
        del keep
        return decode(out)

    def fulfill(self, pledge_name, *args, refs=None):
        """Starts a fulfillment and returns its Future. Arguments are deep
        copied inside this call, so the host buffers are free once it
        returns; ref storage rides on the Future until its wait."""
        arr, n, rarr, rn, keep = self._frame(args, refs)
        ptr = self._rt._lib.geas_pledge_fulfill(
            self._ptr, pledge_name.encode(), arr, n, rarr, rn)
        del keep
        if not ptr:
            raise GeasError(GEAS_ERR_OOM, f"fulfill {pledge_name}")
        return Future(self._rt, ptr, refs)

    # ---- the partial surface ----

    def partial(self):
        lib = self._rt._lib
        c = self._ptr

        def names(k):
            cnt = lib.geas_partial_count(c, k)
            out = []
            for i in range(cnt):
                nm = lib.geas_partial_name(c, k, i)
                out.append(nm.decode() if nm else None)
            return out

        errors = []
        for i in range(lib.geas_partial_nerrors(c)):
            nm = C.c_char_p()
            ev = C.POINTER(GeasValue)()
            _check(lib.geas_partial_error(c, i, C.byref(nm), C.byref(ev)),
                   f"geas_partial_error {i}")
            errors.append((nm.value.decode() if nm.value else None,
                           decode(ev[0]) if ev else None))
        return PartialResult(names(GEAS_ITEM_PENDING),
                             names(GEAS_ITEM_FULFILLED),
                             names(GEAS_ITEM_BROKEN), errors)

    # ---- the workflow surface ----

    def signal(self, name, *args):
        """Delivers the named signal into this instance. The runtime copies
        the arguments at the call; a delivery the terms refuse, an early one
        or one past its deadline, raises GEAS_ERR_STATE, and a repeat of one
        that already landed is quietly a no op."""
        arr, n, _, _, keep = self._frame(args, None)
        _check(self._rt._lib.geas_signal(self._ptr, name.encode(), arr, n),
               f"signal {name}")
        del keep

    def signal_payload(self, name):
        """The payload the named signal was delivered with, decoded."""
        p = C.POINTER(GeasValue)()
        _check(self._rt._lib.geas_signal_payload(self._ptr, name.encode(),
                                                 C.byref(p)),
               f"geas_signal_payload {name}")
        return decode(p[0])

    def tick(self):
        """Applies every deadline of this instance that has passed."""
        _check(self._rt._lib.geas_instance_tick(self._ptr), "geas_instance_tick")

    def lapsed(self, name):
        """Whether the named pledge, signal, or subcontract broke by its
        deadline rather than by an Err."""
        return bool(self._rt._lib.geas_partial_lapsed(self._ptr, name.encode()))

    def term_lapsed(self):
        """Whether the contract's own term is what broke it."""
        return bool(self._rt._lib.geas_contract_term_lapsed(self._ptr))

    def remedy_state(self, name):
        """The named remedy's progress, one of the GEAS_REMEDY_* values."""
        return self._rt._lib.geas_remedy_state(self._ptr, name.encode())

    def remedy_failures(self):
        """Every failed remedy in declaration order, as dicts of its name,
        the last attempt's status, whether its deadline stopped it, and its
        decoded Err payload, None when it failed by status."""
        lib = self._rt._lib
        out = []
        for i in range(lib.geas_partial_remedy_count(self._ptr)):
            nm = lib.geas_partial_remedy_name(self._ptr, i)
            ev = C.POINTER(GeasValue)()
            st = lib.geas_partial_remedy_error(self._ptr, i, C.byref(ev))
            out.append({
                "name": nm.decode() if nm else None,
                "status": lib.geas_partial_remedy_status(self._ptr, i),
                "lapsed": bool(lib.geas_partial_remedy_lapsed(self._ptr, i)),
                "err": decode(ev[0]) if st == GEAS_OK and ev else None,
            })
        return out
