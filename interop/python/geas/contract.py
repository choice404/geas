"""Signed contracts, partial results, and course readings."""

import ctypes as C

from ._abi import (
    COURSE_STATE_NAMES,
    GEAS_ERR_NAME,
    GEAS_ERR_OOM,
    GEAS_ITEM_BROKEN,
    GEAS_ITEM_FULFILLED,
    GEAS_ITEM_PENDING,
    GEAS_OK,
    GeasAttrInfo,
    GeasCourseInfo,
    GeasRef,
    GeasValue,
    STATE_NAMES,
)
from .errors import GeasError, _STATUS_NAMES, _check
from .future import Future
from .values import (
    Err,
    NONE,
    Ok,
    Record,
    Some,
    Typed,
    Variant,
    decode,
)


class CourseInfo:
    """A course reading copied into Python: its GEAS_COURSE_* state, the step
    its frame stands at, the non OK status a HALTED course stopped on, and
    the clock reading its current wait falls due at, 0 when it has none."""

    __slots__ = ("state", "step", "status", "wait_due_ms")

    def __init__(self, info):
        self.state = info.state
        self.step = info.step
        self.status = info.status
        self.wait_due_ms = info.wait_due_ms

    @property
    def state_name(self):
        return COURSE_STATE_NAMES.get(self.state, "?")

    def __repr__(self):
        return (f"CourseInfo(state={self.state_name}, step={self.step}, "
                f"status={_STATUS_NAMES.get(self.status, self.status)}, "
                f"wait_due_ms={self.wait_due_ms})")


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

    @property
    def address(self):
        """The instance pointer as an int, the same for every handle on one
        instance, the handle a bound body receives included, so a host can
        key its own tables by it."""
        ptr = self._ptr
        return ptr.value if isinstance(ptr, C.c_void_p) else ptr

    def signed_at(self):
        return self._rt._lib.geas_contract_signed_at(self._ptr)

    def amended_from(self):
        """The version a resumed instance was amended from, 0 when it was not."""
        return self._rt._lib.geas_contract_amended_from(self._ptr)

    def break_(self):
        _check(self._rt._lib.geas_contract_break(self._ptr),
               "geas_contract_break")

    def cancel_requested(self):
        return bool(self._rt._lib.geas_cancel_requested(self._ptr))

    def request_cancel(self):
        """Publishes a cancel and returns at once. Bodies read
        cancel_requested() as True from then on, a retry wait wakes, and a
        course is cut at its next step, but the instance stays live and
        nothing breaks; the flag stays up until a break or a release."""
        _check(self._rt._lib.geas_contract_request_cancel(self._ptr),
               "geas_contract_request_cancel")

    def release(self):
        """Lets the instance go from this process with no remedy run and
        no state change, the obligation living on in its park row for a
        claim elsewhere. Waits out a running body, rolls back an open
        episode, forfeits unwaited futures, and reclaims the heap; an armed
        autopark writes its last row first. Later calls raise
        GEAS_ERR_STATE."""
        _check(self._rt._lib.geas_contract_release(self._ptr),
               "geas_contract_release")

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
                if isinstance(a, (list, Record, Variant, Typed, Some, Ok, Err)) \
                        or a is NONE or a is None:
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

    # ---- the course ----

    def course_info(self):
        """Where this instance's course stands, as a CourseInfo, read without
        waiting on the instance lock. A contract with no course reads
        GEAS_COURSE_NONE."""
        info = GeasCourseInfo(size=C.sizeof(GeasCourseInfo))
        _check(self._rt._lib.geas_course_info(self._ptr, C.byref(info)),
               "geas_course_info")
        return CourseInfo(info)

    def course_wait(self, timeout_ms=-1):
        """Blocks until the course is not RUNNING and returns the CourseInfo
        it settled at, WAITING, FINISHED, HALTED, or CANCELLED. timeout_ms is
        real time and a negative value waits forever. A wait that runs out
        raises nothing, it returns the CourseInfo as it stands with state
        running and the step the course runs, so a caller polling with a
        timeout reads info.state to tell a slow course from a settled one.
        Called from a pledge body or an event hook it raises GeasError with
        GEAS_ERR_STATE at once, since the course could be waiting on that
        very caller, and the reading as it stood rides on the error's info
        attribute.

        The library is loaded through CDLL, which releases the GIL for the
        length of every foreign call, so pool workers running Python bodies,
        the clock, and the event hook all take the GIL while this thread sits
        in the wait. The call returns only after the events of the run it
        waited on were delivered, which makes it the point after which a
        signal on this instance does not race a delivery in flight."""
        info = GeasCourseInfo(size=C.sizeof(GeasCourseInfo))
        status = self._rt._lib.geas_course_wait(self._ptr, int(timeout_ms),
                                                C.byref(info))
        if status != GEAS_OK:
            error = GeasError(status, "geas_course_wait")
            error.info = CourseInfo(info)
            raise error
        return CourseInfo(info)

    def set_autopark(self, dsn, key=None):
        """Names the store and key this instance parks itself into from now
        on, the row instance park writes, at every latch, lapse, state change,
        retry saved, remedy start and end, step checkpoint, and whenever a
        course waits or ends. The runtime keeps its own copies of both. A dsn
        of None clears them and nothing more is written; a dsn with no key
        raises GEAS_ERR_TYPE."""
        _check(self._rt._lib.geas_contract_set_autopark(
            self._ptr, dsn.encode() if dsn is not None else None,
            key.encode() if key is not None else None),
            f"geas_contract_set_autopark {key}")
