"""demo_amend.py: an order saga parked under one version and finished under
the next, from Python. skeleton/order_course.geas is the order saga at
version 1 and skeleton/order_course_v2.geas the same contract at version 2,
whose amends 1 block renames ship to dispatch, adds notify, retires chase,
works the cents vow out of the amount version 1 was signed with, and
restarts the changed course. The two modules declare one contract, so each
Runtime here loads exactly one of them.

The walk signs version 1, arms autopark, lets the course run to its select,
and shuts the runtime down, the row left in the store the way a deploy that
stops the old module leaves it. A fresh Runtime loads version 2, binds its
bodies, and resumes the row: the instance reads amended from version 1, the
hook sees amended, resumed, and a fresh course start in that order, cents
holds the amount in cents, dispatch holds the latch ship landed, and the
restarted course runs every step again without moving a latch, notifies the
customer, and waits on delivered, which finishes the saga under version 2.

Exit status 0 means every check below held.
"""

import os
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from geas import (GEAS_COURSE_FINISHED, GEAS_COURSE_WAITING,  # noqa: E402
                  GEAS_EVENT_AMENDED, GEAS_EVENT_COURSE_STARTED,
                  GEAS_EVENT_LATCH_FULFILLED, GEAS_EVENT_RESUMED,
                  GEAS_FULFILLED, Ok, Record, Runtime)

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "target" / "geas-out"

# The step each course waits at: version 1's select and version 2's await
V1_SELECT = 5
V2_AWAIT = 6

WAIT_S = 5.0
TICKER_MS = 5

_failures = 0


def check(cond, what):
    global _failures
    if not cond:
        print(f"[demo_amend] FAIL: {what}", file=sys.stderr)
        _failures = 1


class Clock:
    """The fake clock both runtimes read, the same one across the deploy."""

    def __init__(self):
        self.ms = 1000
        self._lock = threading.Lock()

    def now(self):
        with self._lock:
            return self.ms

    def sleep(self, ms):
        with self._lock:
            self.ms += ms


class Host:
    """The bodies both versions call, with a count of each run. Version 1's
    ship hands out T-1 and version 2's dispatch T-2, so a tracking code says
    which version produced it."""

    def __init__(self):
        self.runs = {}
        self.notified = None
        self._lock = threading.Lock()

    def _count(self, body):
        with self._lock:
            self.runs[body] = self.runs.get(body, 0) + 1

    def authorize(self, inst, args):
        self._count("authorize")
        return Ok(Record("AUTH-1"))

    def capture(self, inst, args):
        self._count("capture")
        return Ok(Record("RC-7", 100))

    def reserve(self, inst, args):
        self._count("reserve")
        sku, hold = args
        return Ok(Record(f"R-{sku}", sku, hold))

    def ship(self, inst, args):
        self._count("ship")
        return Ok(Record("ACME", "T-1"))

    def dispatch(self, inst, args):
        self._count("dispatch")
        return Ok(Record("ACME", "T-2"))

    def notify(self, inst, args):
        self._count("notify")
        self.notified = args[0][1]
        return Ok(None)

    def chase(self, inst, args):
        self._count("chase")
        return Ok(None)

    def refund(self, inst, args):
        self._count("refund")
        return Ok(None)

    def recall(self, inst, args):
        self._count("recall")
        return Ok(None)


class Events:
    """The kinds the hook saw, in delivery order, with each item named."""

    def __init__(self):
        self.seen = []
        self._lock = threading.Lock()

    def observe(self, event):
        with self._lock:
            self.seen.append((event.kind, event.item, event.old_state, event.new_state))

    def positions(self, kind, item=None):
        with self._lock:
            return [i for i, (k, it, _, _) in enumerate(self.seen)
                    if k == kind and (item is None or it == item)]

    def first(self, kind):
        with self._lock:
            for k, item, old, new in self.seen:
                if k == kind:
                    return (k, item, old, new)
        return None


def runtime(module, binds, host, clock, events=None):
    rt = Runtime(OUT / "libgeasrt.so")
    rt.set_clock(clock.now, clock.sleep)
    rt.load(OUT / module)
    for name in binds:
        rt.bind(f"OrderCourse.{name}", getattr(host, name))
    if events is not None:
        rt.on_event(events.observe)
    rt.freeze()
    rt.start_ticker(TICKER_MS)
    return rt


def close_runtime(rt):
    rt.stop_ticker()
    rt.shutdown()


def settle(saga, want, step=None):
    """Waits until the course reads want, at step when one is named."""
    deadline = time.monotonic() + WAIT_S
    info = saga.course_wait(int(WAIT_S * 1000))
    while time.monotonic() < deadline:
        if info.state == want and (step is None or info.step == step):
            return info
        time.sleep(0.001)
        info = saga.course_wait(int(WAIT_S * 1000))
    return info


def park_version_one(dsn, key, clock):
    host = Host()
    rt = runtime("liborder_course.geas.so",
                 ["authorize", "capture", "reserve", "ship", "chase", "refund"], host, clock)
    saga = rt.sign("OrderCourse")
    saga.set_autopark(dsn, key)
    info = settle(saga, GEAS_COURSE_WAITING, V1_SELECT)
    print(f"[demo_amend] version 1 waits at the select with {host.runs}, "
          f"amount {saga.vow('amount')}")
    check(info.state == GEAS_COURSE_WAITING and info.step == V1_SELECT,
          "version 1 runs to its select")
    close_runtime(rt)


def check_amended(saga, events):
    print(f"[demo_amend] resumed as version 2, amended from version {saga.amended_from()}")
    check(saga.amended_from() == 1, "the instance was amended from version 1")
    amended = events.first(GEAS_EVENT_AMENDED)
    check(amended is not None and amended[2:] == (1, 2),
          f"the amended event names versions 1 and 2, got {amended}")
    print(f"[demo_amend] cents {saga.vow('cents')}, sku {saga.vow('sku')}")
    check(saga.vow("cents") == 10000, "cents is the amount version 1 signed, in cents")
    check("dispatch" in saga.partial().fulfilled, "dispatch holds ship's latch")


def finish_version_two(dsn, key, clock):
    host, events = Host(), Events()
    rt = runtime("liborder_course_v2.geas.so",
                 ["authorize", "capture", "reserve", "dispatch", "notify", "recall", "refund"],
                 host, clock, events)
    saga = rt.resume(dsn, key)
    check_amended(saga, events)
    info = settle(saga, GEAS_COURSE_WAITING, V2_AWAIT)
    print(f"[demo_amend] the restarted course ran {host.runs} and waits at its await")
    check(info.state == GEAS_COURSE_WAITING and info.step == V2_AWAIT,
          "the restarted course reaches its await")
    check(host.runs.get("dispatch") == 1 and host.runs.get("notify") == 1,
          "dispatch ran again and notify once")
    check(host.notified == "T-2", "notify got the tracking the course bound")
    at = events.positions(GEAS_EVENT_AMENDED)
    resumed = events.positions(GEAS_EVENT_RESUMED)
    started = events.positions(GEAS_EVENT_COURSE_STARTED)
    check(at and resumed and started and at[0] < resumed[0] < started[0],
          "amended, then resumed, then the course started over")
    check(not events.positions(GEAS_EVENT_LATCH_FULFILLED, "dispatch"),
          "a step run again moves no latch")
    saga.signal("delivered", Record("ACME", "T-1"))
    info = settle(saga, GEAS_COURSE_FINISHED)
    print(f"[demo_amend] course {info.state_name}, saga {saga.state_name()}")
    check(info.state == GEAS_COURSE_FINISHED, "the course finishes")
    check(saga.state() == GEAS_FULFILLED, "the saga is fulfilled under version 2")
    close_runtime(rt)


def main():
    clock = Clock()
    fd, dsn = tempfile.mkstemp(prefix="geas_amend_", suffix=".db")
    os.close(fd)
    try:
        park_version_one(dsn, "order-1", clock)
        finish_version_two(dsn, "order-1", clock)
    finally:
        os.unlink(dsn)
    if _failures:
        return 1
    print("[demo_amend] ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
