"""Exercises future readiness, refusal, and release through ctypes."""

from pathlib import Path
import time

from geas import GEAS_ERR_NAME, GEAS_ERR_STATE, GEAS_ERR_TYPE, GeasError, Ok, Runtime


OUT = Path(__file__).resolve().parents[2] / "target" / "geas-out"


def expect_error(status, call):
    try:
        call()
    except GeasError as error:
        assert error.status == status, error
        return
    raise AssertionError(f"expected status {status}")


def wait_ready(future):
    deadline = time.monotonic() + 10
    while not future.poll():
        assert time.monotonic() < deadline, "future completion timed out"
        time.sleep(0.001)


def main():
    with Runtime(OUT / "libgeasrt.so", max_threads=1) as runtime:
        runtime.load(OUT / "libpayment.geas.so")
        runtime.bind("PaymentService.charge", lambda instance, args: Ok(True))
        instance = runtime.sign("PaymentService")
        for _ in range(100):
            future = instance.fulfill("validate_amount", 25.0)
            wait_ready(future)
            expect_error(GEAS_ERR_STATE, future.release)
            assert future.wait() == Ok(True)
            assert future.poll() is True
            future.release()
            assert future._ptr is None and future._refs is None
            expect_error(GEAS_ERR_TYPE, future.poll)
            expect_error(GEAS_ERR_TYPE, future.wait)
            expect_error(GEAS_ERR_TYPE, future.release)

        assert instance.fulfill_sync("validate_amount", 25.0) == Ok(True)
        expect_error(GEAS_ERR_NAME, lambda: instance.fulfill_sync("missing"))
        future = instance.fulfill("validate_amount", 25.0)
        instance.break_()
        assert future.poll() is True
        future.release()
    print("[test_future.py] ok")


if __name__ == "__main__":
    main()
