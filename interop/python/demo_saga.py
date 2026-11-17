"""demo_saga.py: the order saga from Python. The same module tests/runtime/
test_saga.c drives in C, built from skeleton/saga.geas by the C++ compiler,
loaded here through the geas package with Python callables bound over the abstract
steps and a Python clock the runtime reads instead of the wall, so a three
day deadline is a number this file moves.

Two walks. The happy path signs, authorizes through one Timeout the retry
term absorbs, captures, reserves through the compiled Geas body, ships, and
delivers the signal, landing Fulfilled. The break path does the same up to
ship, has the carrier refuse, and watches the remedies undo what landed,
release first since reserve landed last, then refund handed the receipt
capture produced, the saga reading Remedying while they run and Broken once
they are done.

Exit status 0 means every check below held.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from geas import (GEAS_BROKEN, GEAS_ERR_STATE, GEAS_FULFILLED,  # noqa: E402
                  GEAS_PARTIAL, GEAS_REMEDY_DONE, GEAS_REMEDYING, GeasError,
                  Ok, Err, Record, Runtime, Variant, STATE_NAMES)

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "target" / "geas-out"

# The variant tags of the saga's sums, their declaration order
PAY_TIMEOUT = 1
SHIP_NO_CARRIER = 0

_failures = 0


def check(cond, what):
    global _failures
    if not cond:
        print(f"[demo_saga] FAIL: {what}", file=sys.stderr)
        _failures = 1


class Clock:
    """The fake clock: now is a number, and a sleep moves it and is logged."""

    def __init__(self):
        self.ms = 1000
        self.sleeps = []

    def now(self):
        return self.ms

    def sleep(self, ms):
        self.sleeps.append(ms)
        self.ms += ms


class Host:
    """The Python side of the saga, the four abstract steps and a log of
    what each one saw."""

    def __init__(self):
        self.timeouts = 1
        self.ship_fails = False
        self.refunds = []

    def authorize(self, inst, args):
        if self.timeouts > 0:
            self.timeouts -= 1
            return Err(Variant(PAY_TIMEOUT))
        card = args[0]
        return Ok(Record(f"AUTH-{card[0]}"))

    def capture(self, inst, args):
        return Ok(Record("RC-7", inst.vow("amount")))

    def ship(self, inst, args):
        if self.ship_fails:
            return Err(Variant(SHIP_NO_CARRIER))
        return Ok(Record("ACME", "T-1"))

    def refund(self, inst, args):
        receipt = args[0]
        self.refunds.append((receipt[0], STATE_NAMES[inst.state()],
                             inst.remedy_state("release")))
        return Ok(None)


def saga_runtime(host, clock):
    rt = Runtime(OUT / "libgeasrt.so")
    rt.set_clock(clock.now, clock.sleep)
    rt.load(OUT / "libsaga.geas.so")
    rt.bind("OrderSaga.authorize", host.authorize)
    rt.bind("OrderSaga.capture", host.capture)
    rt.bind("OrderSaga.ship", host.ship)
    rt.bind("OrderSaga.refund", host.refund)
    rt.freeze()
    return rt


def through_reserve(saga):
    """Authorize, capture, and reserve, answering the reservation."""
    auth = saga.fulfill_sync("authorize", Record("4111"))
    check(isinstance(auth, Ok), f"authorize lands, got {auth}")
    receipt = saga.fulfill_sync("capture", Record(*auth.value))
    check(isinstance(receipt, Ok), f"capture lands, got {receipt}")
    check(saga.state() == GEAS_PARTIAL, "Payment alone is partial")
    hour_ms = 3_600_000
    res = saga.fulfill_sync("reserve", "sku-1", hour_ms)
    check(isinstance(res, Ok), f"reserve lands, got {res}")
    return res.value


def happy_path():
    host, clock = Host(), Clock()
    rt = saga_runtime(host, clock)
    saga = rt.sign("OrderSaga")
    res = through_reserve(saga)
    print(f"[demo_saga] authorize retried after a Timeout, slept {clock.sleeps}")
    check(clock.sleeps == [2000], "one 2s delay before the second attempt")
    print(f"[demo_saga] reserved {res}")
    check(res[2] == 5_400_000, "the hold is the hour plus the thirty minute grace")
    try:
        saga.signal("delivered", Record("ACME", "T-1"))
        check(False, "delivered before ship should be refused")
    except GeasError as e:
        check(e.status == GEAS_ERR_STATE, "an early delivery is a state error")
    shipped = saga.fulfill_sync("ship", Record(*res))
    check(isinstance(shipped, Ok), f"ship lands, got {shipped}")
    saga.signal("delivered", Record(*shipped.value))
    print(f"[demo_saga] happy path ends {saga.state_name()}, "
          f"delivered with {saga.signal_payload('delivered')}")
    check(saga.state() == GEAS_FULFILLED, "the saga is fulfilled")
    saga.break_()
    check(host.refunds == [], "a fulfilled saga undoes nothing")
    rt.shutdown()


def break_path():
    host, clock = Host(), Clock()
    host.timeouts = 0
    rt = saga_runtime(host, clock)
    saga = rt.sign("OrderSaga")
    res = through_reserve(saga)
    host.ship_fails = True
    shipped = saga.fulfill_sync("ship", Record(*res))
    print(f"[demo_saga] ship answered {shipped}")
    check(isinstance(shipped, Err), "the carrier refused")
    print(f"[demo_saga] break path ends {saga.state_name()}, refund saw {host.refunds}")
    check(saga.state() == GEAS_BROKEN, "the saga is broken")
    check(host.refunds == [("RC-7", STATE_NAMES[GEAS_REMEDYING], GEAS_REMEDY_DONE)],
          "refund ran once with the receipt, mid pass, after release")
    check(saga.remedy_state("release") == GEAS_REMEDY_DONE,
          "release's Geas body accepted the reservation")
    check(saga.remedy_failures() == [], "no remedy failed")
    check(not saga.lapsed("ship"), "ship broke by its Err, not by a deadline")
    rt.shutdown()


def main():
    happy_path()
    break_path()
    if _failures:
        return 1
    print("[demo_saga] ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
