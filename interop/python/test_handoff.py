"""test_handoff.py: the order saga handed from one runtime to another
through ctypes. The first runtime takes the saga through reserve, asks for a
cancel a ship body reads while the saga stays live, parks the row and
releases, with no refund and no state change. The second runtime claims the
row, ships, delivers the signal, and lands Fulfilled, and a second claim of
the same key finds nothing.

Exit status 0 means every check below held.
"""

import sys
import time
from pathlib import Path
from threading import Event as Flag
from threading import Thread

sys.path.insert(0, str(Path(__file__).parent))

from geas import (GEAS_ERR_NAME, GEAS_ERR_STATE,  # noqa: E402
                  GEAS_EVENT_RELEASED, GEAS_EVENT_REMEDY_STARTED,
                  GEAS_FULFILLED, GEAS_PARTIAL, GEAS_REMEDY_IDLE, GeasError,
                  Ok, Record, Runtime)

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "target" / "geas-out"
DSN = str(OUT / "test_handoff_python.db")
KEY = "saga-handoff"

# Real seconds a body or a delivery gets before the gate calls it lost
WAIT_SECONDS = 10

_failures = 0


def check(cond, what):
    global _failures
    if not cond:
        print(f"[test_handoff.py] FAIL: {what}", file=sys.stderr)
        _failures = 1


class Host:
    """The saga's abstract steps; ship waits for a cancel when asked to"""

    def __init__(self):
        self.refunds = []
        self.is_holding_ship = False
        self.ship_started = Flag()
        self.ship_saw_cancel = False

    def authorize(self, inst, args):
        return Ok(Record(f"AUTH-{args[0][0]}"))

    def capture(self, inst, args):
        return Ok(Record("RC-7", inst.vow("amount")))

    def ship(self, inst, args):
        if self.is_holding_ship:
            self.ship_started.set()
            deadline = time.monotonic() + WAIT_SECONDS
            while not inst.cancel_requested() and time.monotonic() < deadline:
                time.sleep(0.001)
            self.ship_saw_cancel = inst.cancel_requested()
        return Ok(Record("ACME", "T-1"))

    def refund(self, inst, args):
        self.refunds.append(args[0][0])
        return Ok(None)


def saga_runtime(host, kinds):
    rt = Runtime(OUT / "libgeasrt.so")
    rt.load(OUT / "libsaga.geas.so")
    for step in ("authorize", "capture", "ship", "refund"):
        rt.bind(f"OrderSaga.{step}", getattr(host, step))
    rt.on_event(lambda event: kinds.append(event.kind))
    rt.freeze()
    return rt


def through_reserve(saga):
    auth = saga.fulfill_sync("authorize", Record("4111"))
    receipt = saga.fulfill_sync("capture", Record(*auth.value))
    check(isinstance(receipt, Ok), f"capture lands, got {receipt}")
    res = saga.fulfill_sync("reserve", "sku-1", 3_600_000)
    check(isinstance(res, Ok), f"reserve lands, got {res}")
    check(saga.state() == GEAS_PARTIAL, "Payment alone is partial")
    return res.value


def request_while_shipping(saga, host, res):
    """A cancel request the ship body reads, the saga staying live"""
    host.is_holding_ship = True
    outcome = []
    shipper = Thread(target=lambda: outcome.append(
        saga.fulfill_sync("ship", Record(*res))))
    shipper.start()
    check(host.ship_started.wait(WAIT_SECONDS), "ship started")
    saga.request_cancel()
    shipper.join(WAIT_SECONDS)
    check(host.ship_saw_cancel, "the ship body read the request")
    check(outcome and isinstance(outcome[0], Ok), f"ship still landed, got {outcome}")
    check(saga.state() == GEAS_PARTIAL, "the saga is live and partial")
    saga.request_cancel()
    check(saga.cancel_requested(), "the flag stays up")


def give(host, kinds):
    rt = saga_runtime(host, kinds)
    saga = rt.sign("OrderSaga")
    res = through_reserve(saga)
    request_while_shipping(saga, host, res)
    saga.park(DSN, KEY)
    saga.release()
    check(kinds.count(GEAS_EVENT_RELEASED) == 1, "one released event")
    check(GEAS_EVENT_REMEDY_STARTED not in kinds, "no remedy started")
    check(host.refunds == [], "no refund ran")
    check(saga.state() == GEAS_PARTIAL, "the released saga still reads partial")
    check(saga.remedy_state("refund") == GEAS_REMEDY_IDLE, "refund stayed idle")
    try:
        saga.signal("delivered", Record("ACME", "T-1"))
        check(False, "a signal after release should be refused")
    except GeasError as error:
        check(error.status == GEAS_ERR_STATE, "a signal after release is a state error")
    rt.shutdown()


def take(host, kinds):
    rt = saga_runtime(host, kinds)
    saga = rt.claim(DSN, KEY)
    check(saga.state() == GEAS_PARTIAL, "the claimed saga is partial")
    saga.signal("delivered", Record("ACME", "T-1"))
    check(saga.state() == GEAS_FULFILLED, "the claimed saga ends fulfilled")
    try:
        rt.claim(DSN, KEY)
        check(False, "a second claim should find nothing")
    except GeasError as error:
        check(error.status == GEAS_ERR_NAME, "a second claim answers GEAS_ERR_NAME")
    check(host.refunds == [], "no refund ran on either side")
    rt.shutdown()


def main():
    Path(DSN).unlink(missing_ok=True)
    host = Host()
    give(host, [])
    take(host, [])
    Path(DSN).unlink(missing_ok=True)
    if _failures:
        return 1
    print("[test_handoff.py] ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
