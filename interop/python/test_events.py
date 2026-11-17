"""Checks event delivery, read calls, callback lifetime, and clearing through ctypes."""

from pathlib import Path
import contextlib
import gc
import io
import threading
import time

from geas import (GEAS_ERR_STATE, GEAS_EVENT_CANCELLED, GEAS_EVENT_LATCH_FULFILLED,
                  GEAS_EVENT_RECLAIMED, GEAS_EVENT_STATE_CHANGED, GEAS_SIGNED,
                  GeasError, Ok, Runtime)

OUT = Path(__file__).resolve().parents[2] / "target" / "geas-out"


def main():
    events = []
    callback_threads = []
    body_threads = []
    with Runtime(OUT / "libgeasrt.so", max_threads=1) as runtime:
        runtime.load(OUT / "libpayment.geas.so")

        def charge(instance, args):
            body_threads.append(threading.get_ident())
            return Ok(True)

        runtime.bind("PaymentService.charge", charge)
        runtime.freeze()
        runtime.set_clock(now=lambda: 1234)

        def observe(event):
            assert event.contract == "PaymentService"
            assert event.at_ms == 1234
            assert event.instance.state() != 0
            event.instance.partial()
            event.instance.attrs()
            if event.kind == GEAS_EVENT_LATCH_FULFILLED:
                assert event.instance.vow("currency") == "USD"
                callback_threads.append(threading.get_ident())
            try:
                event.instance.break_()
            except GeasError as error:
                assert error.status == GEAS_ERR_STATE
            else:
                raise AssertionError("callback break succeeded")
            events.append(event)

        runtime.on_event(observe)
        gc.collect()
        instance = runtime.sign("PaymentService")
        assert len(events) == 1 and events[0].new_state == GEAS_SIGNED
        assert instance.fulfill_sync("charge", "card", 25.0) == Ok(True)

        # An accepted park is also a delivery boundary for preceding worker events
        deadline = time.monotonic() + 10
        while True:
            try:
                instance.park(":memory:", "events")
                break
            except GeasError as error:
                assert error.status == GEAS_ERR_STATE
                assert time.monotonic() < deadline
                time.sleep(0.001)
        assert callback_threads == body_threads
        assert [event.kind for event in events[:3]] == [
            GEAS_EVENT_STATE_CHANGED, GEAS_EVENT_LATCH_FULFILLED, GEAS_EVENT_STATE_CHANGED]
        assert events[1].item == "charge"
        assert [event.seq for event in events] == list(range(1, len(events) + 1))
        assert events[0].instance._ptr == instance._ptr.value

        runtime.on_event(None)
        previous = len(events)
        assert instance.fulfill_sync("validate_amount", 25.0) == Ok(True)
        assert len(events) == previous
        runtime.on_event(observe)
        instance.break_()
        assert [event.kind for event in events[-3:]] == [
            GEAS_EVENT_CANCELLED, GEAS_EVENT_STATE_CHANGED, GEAS_EVENT_RECLAIMED]
        assert [event.seq for event in events] == list(range(1, len(events) + 1))

        # A void callback reports exceptions without interrupting runtime delivery
        def raises(event):
            raise ValueError("event callback sentinel")

        runtime.on_event(raises)
        errors = io.StringIO()
        with contextlib.redirect_stderr(errors):
            runtime.sign("PaymentService")
        assert "event callback sentinel" in errors.getvalue()
        runtime.on_event(None)
    assert events[1].contract == "PaymentService" and events[1].item == "charge"
    print("[test_events.py] ok")


if __name__ == "__main__":
    main()
