"""ABI constants, wire layouts, and callback signatures from docs/abi.md."""

import ctypes as C


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
GEAS_EVENT_COURSE_STARTED = 15
GEAS_EVENT_COURSE_SUSPENDED = 16
GEAS_EVENT_COURSE_RESUMED = 17
GEAS_EVENT_COURSE_FINISHED = 18
GEAS_EVENT_COURSE_HALTED = 19
GEAS_EVENT_COURSE_CANCELLED = 20
# A resume through an amends block, before RESUMED, old_state the version
# the row was written under and new_state the version it resumed as
GEAS_EVENT_AMENDED = 21
# A release let the instance go without its remedies, where a break's
# RECLAIMED would stand, both states the one it was released in
GEAS_EVENT_RELEASED = 22

# Where an engine driven course stands, docs/abi.md, The course. NONE is a
# contract with no course, RUNNING one scheduled, running, or delivering the
# events of its last entry, WAITING one suspended at a wait.
GEAS_COURSE_NONE = 0
GEAS_COURSE_RUNNING = 1
GEAS_COURSE_WAITING = 2
GEAS_COURSE_FINISHED = 3
GEAS_COURSE_HALTED = 4
GEAS_COURSE_CANCELLED = 5

COURSE_STATE_NAMES = {
    GEAS_COURSE_NONE: "none",
    GEAS_COURSE_RUNNING: "running",
    GEAS_COURSE_WAITING: "waiting",
    GEAS_COURSE_FINISHED: "finished",
    GEAS_COURSE_HALTED: "halted",
    GEAS_COURSE_CANCELLED: "cancelled",
}

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


class GeasCourseInfo(C.Structure):
    """Where a course stands. size is the caller's sizeof of this header,
    the runtime fills only the fields that fit and leaves size as it was."""

    _fields_ = [
        ("size", C.c_uint32),
        ("state", C.c_uint32),
        ("step", C.c_uint32),
        ("status", C.c_uint32),
        ("wait_due_ms", C.c_int64),
    ]


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
assert C.sizeof(GeasCourseInfo) == 24
