"""Exercises cancellation from a bound callback through ctypes."""

from pathlib import Path
from threading import Event
import time

from geas import GEAS_BROKEN, GEAS_ERR_STATE, GeasError, Ok, Runtime


OUT = Path(__file__).resolve().parents[2] / "target" / "geas-out"


def expect_cancelled(future):
    try:
        future.wait()
    except GeasError as error:
        assert error.status == GEAS_ERR_STATE, error
    else:
        raise AssertionError("cancelled future delivered a value")
    future.release()


def main():
    started = Event()
    returned = Event()
    observed = []

    def charge(instance, args):
        observed.append(instance.cancel_requested())
        started.set()
        deadline = time.monotonic() + 10
        while not instance.cancel_requested():
            assert time.monotonic() < deadline, "cancel request timed out"
            time.sleep(0.001)
        observed.append(instance.cancel_requested())
        returned.set()
        return Ok(True)

    with Runtime(OUT / "libgeasrt.so", max_threads=1) as runtime:
        runtime.load(OUT / "libpayment.geas.so")
        runtime.bind("PaymentService.charge", charge)
        instance = runtime.sign("PaymentService")
        assert instance.cancel_requested() is False
        future = instance.fulfill("charge", "card", 25.0)
        assert started.wait(10), "body did not start"
        instance.break_()
        assert returned.is_set(), "break returned before the body"
        assert observed == [False, True], observed
        assert instance.state() == GEAS_BROKEN
        expect_cancelled(future)

        def deferred(instance, args):
            observed.append(instance.cancel_requested())
            instance.break_()
            observed.append(instance.cancel_requested())
            return Ok(True)

        runtime.bind("PaymentService.charge", deferred)
        instance = runtime.sign("PaymentService")
        future = instance.fulfill("charge", "card", 25.0)
        expect_cancelled(future)
        assert observed == [False, True, False, True], observed
        assert instance.state() == GEAS_BROKEN
    print("[test_cancel.py] ok")


if __name__ == "__main__":
    main()
