"""demo_course.py: the engine driven order saga from Python. The same module
tests/runtime/test_course_saga.c drives in C, built from
skeleton/order_course.geas by the C++ compiler, loaded here through the geas package
with Python callables bound over the abstract bodies, a Python clock the
runtime reads instead of the wall, and the runtime's own ticker running so
a deadline falls due without this file ticking.

The host never calls a step. The contract carries a course, the runtime runs
it from the moment the saga signs, and this file only waits on the course,
hands in the delivered signal, and reads where things stand.

Four walks. The happy path signs, watches the course plan the hold in Geas,
authorize through one Timeout the retry term absorbs, capture and reserve at
once, ship, and wait at the select, then delivers the signal and sees the
course finish and the saga land Fulfilled, the hook having seen the course's
events in order. The break path has the carrier refuse, the course ends on
ship's `?`, and the remedies undo what landed. The park path arms autopark
right after the sign, lets the course reach the select, shuts the runtime
down, resumes the row in a fresh runtime, and finishes the course there with
no host body running again. The watch path holds ship's body and reads the
running course from the host, a wait that runs out answering the course as
it stands, running at the ship step, where a wait from the body itself is a
state error at once.

Exit status 0 means every check below held.
"""

import os
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from geas import (GEAS_BROKEN, GEAS_COURSE_FINISHED, GEAS_COURSE_RUNNING,  # noqa: E402
                  GEAS_COURSE_WAITING,
                  GEAS_ERR_STATE, GEAS_EVENT_COURSE_CANCELLED,
                  GEAS_EVENT_COURSE_FINISHED, GEAS_EVENT_COURSE_HALTED,
                  GEAS_EVENT_COURSE_RESUMED, GEAS_EVENT_COURSE_STARTED,
                  GEAS_EVENT_COURSE_SUSPENDED, GEAS_EVENT_LATCH_FULFILLED,
                  GEAS_EVENT_SIGNAL_DELIVERED, GEAS_FULFILLED, GEAS_PARTIAL,
                  GEAS_REMEDY_DONE, GEAS_REMEDYING, GeasError, Ok, Err, Record,
                  Runtime, Variant, STATE_NAMES)

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "target" / "geas-out"

# The variant tags of the saga's sums, their declaration order
PAY_TIMEOUT = 1
SHIP_NO_CARRIER = 0

# The steps of the compiled course, the number each checkpoint stores
STEP_SHIP = 4
STEP_SELECT = 5

MINUTE_MS = 60_000
HOUR_MS = 60 * MINUTE_MS
DAY_MS = 24 * HOUR_MS

# Real time a walk waits on a course before calling it lost, and the period
# of the runtime's ticker
WAIT_S = 5.0
TICKER_MS = 5

COURSE_EVENT_NAMES = {
    GEAS_EVENT_COURSE_STARTED: "started",
    GEAS_EVENT_COURSE_SUSPENDED: "suspended",
    GEAS_EVENT_COURSE_RESUMED: "resumed",
    GEAS_EVENT_COURSE_FINISHED: "finished",
    GEAS_EVENT_COURSE_HALTED: "halted",
    GEAS_EVENT_COURSE_CANCELLED: "cancelled",
}

_failures = 0


def check(cond, what):
    global _failures
    if not cond:
        print(f"[demo_course] FAIL: {what}", file=sys.stderr)
        _failures = 1


class Clock:
    """The fake clock: now is a number, and a sleep moves it and is logged.
    The ticker and the pool read it from their own threads, so a lock keeps
    the reading and the move whole."""

    def __init__(self):
        self.ms = 1000
        self.sleeps = []
        self._lock = threading.Lock()

    def now(self):
        with self._lock:
            return self.ms

    def sleep(self, ms):
        with self._lock:
            self.sleeps.append(ms)
            self.ms += ms


class Host:
    """The Python side of the saga, the bodies the course calls and a count
    of how often each one ran. capture and reserve run at the same time on
    two pool workers, so the counts sit behind a lock."""

    def __init__(self):
        self.timeouts = 1
        self.ship_fails = False
        self.runs = {"authorize": 0, "capture": 0, "reserve": 0, "ship": 0,
                     "chase": 0, "refund": 0}
        self.hold = None
        self.refunds = []
        self.ship_gate = None
        self.ship_inside = threading.Event()
        self.ship_wait_status = None
        self._lock = threading.Lock()

    def _count(self, body):
        with self._lock:
            self.runs[body] += 1

    def authorize(self, inst, args):
        self._count("authorize")
        if self.timeouts > 0:
            self.timeouts -= 1
            return Err(Variant(PAY_TIMEOUT))
        card = args[0]
        check(card == ["4111"], f"authorize is handed the vow's card, got {card}")
        return Ok(Record("AUTH-1"))

    def capture(self, inst, args):
        self._count("capture")
        check(args[0] == ["AUTH-1"], f"capture is handed authorize's id, got {args[0]}")
        return Ok(Record("RC-7", 100))

    def reserve(self, inst, args):
        self._count("reserve")
        sku, hold = args
        self.hold = hold
        return Ok(Record(f"R-{sku}", sku, hold))

    def ship(self, inst, args):
        self._count("ship")
        if self.ship_gate is not None:
            self.hold_ship(inst)
        if self.ship_fails:
            return Err(Variant(SHIP_NO_CARRIER))
        return Ok(Record("ACME", "T-1"))

    def hold_ship(self, inst):
        """Holds ship's body until the walk lets it go, after a wait from
        the body itself, which answers at once."""
        try:
            inst.course_wait(0)
        except GeasError as e:
            self.ship_wait_status = e.status
        self.ship_inside.set()
        check(self.ship_gate.wait(WAIT_S), "the walk lets ship's body go")

    def chase(self, inst, args):
        self._count("chase")
        return Ok(None)

    def refund(self, inst, args):
        self._count("refund")
        receipt = args[0]
        self.refunds.append((receipt[0], STATE_NAMES[inst.state()]))
        return Ok(None)


class Events:
    """What the hook saw, as (kind, item) pairs in delivery order."""

    def __init__(self):
        self.seen = []
        self._lock = threading.Lock()

    def observe(self, event):
        with self._lock:
            self.seen.append((event.kind, event.item))

    def course_kinds(self):
        with self._lock:
            return [COURSE_EVENT_NAMES[k] for k, _ in self.seen
                    if k in COURSE_EVENT_NAMES]

    def in_order(self, want):
        """Whether want appears in what was seen as a subsequence."""
        with self._lock:
            at = 0
            for kind, item in self.seen:
                if at < len(want) and want[at] == (kind, item):
                    at += 1
            return at == len(want)


def course_runtime(host, clock, events=None):
    rt = Runtime(OUT / "libgeasrt.so")
    rt.set_clock(clock.now, clock.sleep)
    rt.load(OUT / "liborder_course.geas.so")
    rt.bind("OrderCourse.authorize", host.authorize)
    rt.bind("OrderCourse.capture", host.capture)
    rt.bind("OrderCourse.reserve", host.reserve)
    rt.bind("OrderCourse.ship", host.ship)
    rt.bind("OrderCourse.chase", host.chase)
    rt.bind("OrderCourse.refund", host.refund)
    if events is not None:
        rt.on_event(events.observe)
    rt.freeze()
    rt.start_ticker(TICKER_MS)
    return rt


def close_runtime(rt):
    rt.stop_ticker()
    rt.shutdown()


def settle(saga, want, step=None):
    """Waits until the course reads want, at step when one is named, and
    returns the last reading. The course passes through WAITING at the
    together on its way, so one wait is not always the one asked for."""
    deadline = time.monotonic() + WAIT_S
    info = saga.course_wait(int(WAIT_S * 1000))
    while time.monotonic() < deadline:
        if info.state == want and (step is None or info.step == step):
            return info
        time.sleep(0.001)
        info = saga.course_wait(int(WAIT_S * 1000))
    return info


def happy_path():
    host, clock, events = Host(), Clock(), Events()
    rt = course_runtime(host, clock, events)
    saga = rt.sign("OrderCourse")
    info = settle(saga, GEAS_COURSE_WAITING, STEP_SELECT)
    print(f"[demo_course] the course ran itself to the select, {info}")
    check(info.state == GEAS_COURSE_WAITING and info.step == STEP_SELECT,
          "the course waits at the select")
    check(host.runs["authorize"] == 2 and host.runs["capture"] == 1 and
          host.runs["reserve"] == 1 and host.runs["ship"] == 1,
          f"every step ran with no host call, got {host.runs}")
    print(f"[demo_course] authorize retried after a Timeout, slept {clock.sleeps}")
    check(clock.sleeps == [2000], "one 2s delay before the second attempt")
    print(f"[demo_course] plan's Geas body handed reserve a hold of {host.hold}")
    check(host.hold == HOUR_MS + 30 * MINUTE_MS, "the hold is the hour plus the grace")
    check(info.wait_due_ms == clock.now() + 2 * DAY_MS, "the select times out two days on")
    check(saga.state() == GEAS_PARTIAL, "Payment and reserve make it partial")
    try:
        saga.fulfill_sync("ship", Record("R-sku-1", "sku-1", HOUR_MS))
        check(False, "a host fulfillment on a course should be refused")
    except GeasError as e:
        print(f"[demo_course] a host fulfillment of ship answered {e.status_name}")
        check(e.status == GEAS_ERR_STATE, "a host fulfillment is a state error")
    check(host.runs["ship"] == 1, "the refused body never ran")
    saga.signal("delivered", Record("ACME", "T-1"))
    info = settle(saga, GEAS_COURSE_FINISHED)
    print(f"[demo_course] happy path course {info.state_name}, saga {saga.state_name()}, "
          f"delivered with {saga.signal_payload('delivered')}")
    check(info.state == GEAS_COURSE_FINISHED, "the course finishes")
    check(saga.state() == GEAS_FULFILLED, "the saga is fulfilled")
    check(host.runs["chase"] == 0, "a parcel on time is never chased")
    print(f"[demo_course] course events {events.course_kinds()}")
    check(events.in_order([
        (GEAS_EVENT_COURSE_STARTED, None),
        (GEAS_EVENT_LATCH_FULFILLED, "plan"),
        (GEAS_EVENT_LATCH_FULFILLED, "authorize"),
        (GEAS_EVENT_COURSE_SUSPENDED, None),
        (GEAS_EVENT_COURSE_RESUMED, None),
        (GEAS_EVENT_LATCH_FULFILLED, "ship"),
        (GEAS_EVENT_COURSE_SUSPENDED, None),
        (GEAS_EVENT_SIGNAL_DELIVERED, "delivered"),
        (GEAS_EVENT_COURSE_RESUMED, None),
        (GEAS_EVENT_COURSE_FINISHED, None),
    ]), "the hook saw the course's events in order")
    kinds = events.course_kinds()
    check(kinds[:1] == ["started"] and kinds[-1:] == ["finished"] and
          "halted" not in kinds and "cancelled" not in kinds,
          "the course started, finished, and neither halted nor was cancelled")
    saga.break_()
    check(host.refunds == [], "a fulfilled saga undoes nothing")
    close_runtime(rt)


def break_path():
    host, clock = Host(), Clock()
    host.timeouts = 0
    host.ship_fails = True
    rt = course_runtime(host, clock)
    saga = rt.sign("OrderCourse")
    info = settle(saga, GEAS_COURSE_FINISHED)
    print(f"[demo_course] ship answered NoCarrier, course {info.state_name} at step {info.step}")
    check(info.state == GEAS_COURSE_FINISHED, "the course ends on ship's ?, not cancelled")
    check(info.step == STEP_SHIP, "at the ship step")
    print(f"[demo_course] break path ends {saga.state_name()}, refund saw {host.refunds}")
    check(saga.state() == GEAS_BROKEN, "the saga is broken")
    check(host.refunds == [("RC-7", STATE_NAMES[GEAS_REMEDYING])],
          "refund ran once with the receipt while the saga read Remedying")
    check(saga.remedy_state("release") == GEAS_REMEDY_DONE,
          "release's Geas body accepted the reservation")
    check(saga.remedy_failures() == [], "no remedy failed")
    check(host.runs["chase"] == 0, "nothing past ship ran")
    try:
        saga.signal("delivered", Record("ACME", "T-1"))
        check(False, "a broken saga should take no signal")
    except GeasError as e:
        check(e.status == GEAS_ERR_STATE, "a delivery on a broken saga is a state error")
    close_runtime(rt)


def park_path(dsn):
    key = "order-1"
    host, clock = Host(), Clock()
    host.timeouts = 0
    rt = course_runtime(host, clock)
    saga = rt.sign("OrderCourse")
    saga.set_autopark(dsn, key)
    info = settle(saga, GEAS_COURSE_WAITING, STEP_SELECT)
    check(info.state == GEAS_COURSE_WAITING and info.step == STEP_SELECT,
          "the first runtime's course waits at the select")
    ran = dict(host.runs)
    print(f"[demo_course] parked at the select with {ran}, shutting the runtime down")
    close_runtime(rt)

    fresh = Host()
    rt = course_runtime(fresh, clock)
    saga = rt.resume(dsn, key)
    settle(saga, GEAS_COURSE_WAITING, STEP_SELECT)
    info = saga.course_info()
    print(f"[demo_course] resumed in a fresh runtime, {info}")
    check(info.state == GEAS_COURSE_WAITING and info.step == STEP_SELECT,
          "the resumed course waits at the select it parked at")
    check(all(n == 0 for n in fresh.runs.values()),
          f"no host body ran again, got {fresh.runs}")
    saga.signal("delivered", Record("ACME", "T-1"))
    info = settle(saga, GEAS_COURSE_FINISHED)
    print(f"[demo_course] park path course {info.state_name}, saga {saga.state_name()}")
    check(info.state == GEAS_COURSE_FINISHED, "the resumed course finishes")
    check(saga.state() == GEAS_FULFILLED, "the resumed saga is fulfilled")
    check(all(n == 0 for n in fresh.runs.values()),
          "delivered was the only thing the resumed course needed")
    close_runtime(rt)


def watch_path():
    host, clock = Host(), Clock()
    host.timeouts = 0
    host.ship_gate = threading.Event()
    rt = course_runtime(host, clock)
    saga = rt.sign("OrderCourse")
    check(host.ship_inside.wait(WAIT_S), "ship's body runs inside the course")
    info = saga.course_wait(30)
    print(f"[demo_course] a wait of 30ms while ship runs answered {info}")
    check(info.state == GEAS_COURSE_RUNNING, "a wait that runs out reads running")
    check(info.step == STEP_SHIP, "at the step the running course stands at")
    check(saga.course_info().step == STEP_SHIP, "course_info reads the same step")
    check(host.ship_wait_status == GEAS_ERR_STATE,
          "a wait from the body is a state error at once")
    host.ship_gate.set()
    info = settle(saga, GEAS_COURSE_WAITING, STEP_SELECT)
    check(info.state == GEAS_COURSE_WAITING and info.step == STEP_SELECT,
          "the course goes on to the select once ship returns")
    close_runtime(rt)


def main():
    happy_path()
    break_path()
    watch_path()
    fd, dsn = tempfile.mkstemp(prefix="geas_course_", suffix=".db")
    os.close(fd)
    try:
        park_path(dsn)
    finally:
        os.unlink(dsn)
    if _failures:
        return 1
    print("[demo_course] ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
