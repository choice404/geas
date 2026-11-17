"""clientkit.py: what every Python bridge walk shares, the check tally, the
status assertion, the session handle, and the wait for a row to leave.

The client knows an instance only by the id its stream issued, so it reads
an instance's fate through the contract surface alone: GetPartial answers
while the stream holds the row and NOT_FOUND once the row is gone.
"""

import sys
import time

import grpc


class Tally:
    """Counts failed checks; a walk exits nonzero when any failed."""

    def __init__(self, tag):
        self.tag = tag
        self.failures = 0

    def check(self, cond, what):
        if not cond:
            print(f"[{self.tag}] FAIL: {what}", file=sys.stderr)
            self.failures += 1

    def expect_code(self, fn, code, what):
        """Demands a gRPC error with a given code. A pledge's Err never
        lands here: only a call that did not run does."""
        try:
            fn()
            self.check(False, f"{what} did not fail")
        except grpc.RpcError as e:
            self.check(e.code() == code,
                       f"{what} is {code.name}, got {e.code().name}")


class Session:
    """The client half of the session. Opening it signs, or resumes when
    handed the Resume call; the first event off the stream is the signature;
    holding the object holds the instance, and closing it ends the stream."""

    def __init__(self, stub, request, stream_call=None):
        self._stream = (stub.Session if stream_call is None else stream_call)(request)
        event = next(self._stream)
        if event.WhichOneof("event") != "signed":
            raise RuntimeError("the session stream opened without the signature")
        self.signed = event.signed
        self.instance_id = event.signed.instance_id

    def close(self):
        self._stream.cancel()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


def wait_gone(stub, partial_request, instance_id, timeout):
    """Polls GetPartial on an id until it answers NOT_FOUND, and answers how
    long that took, None on timeout."""
    t0 = time.monotonic()
    deadline = t0 + timeout
    while time.monotonic() < deadline:
        try:
            stub.GetPartial(partial_request(instance_id=instance_id))
        except grpc.RpcError as e:
            if e.code() == grpc.StatusCode.NOT_FOUND:
                return time.monotonic() - t0
            raise
        time.sleep(0.005)
    return None


def wait_until(read, is_done, timeout):
    """Polls read() until is_done holds on what it answered, and answers the
    last reading."""
    deadline = time.monotonic() + timeout
    reading = read()
    while not is_done(reading) and time.monotonic() < deadline:
        time.sleep(0.01)
        reading = read()
    return reading


def channel_ready(port, tally):
    """A channel to the local server once it answers, else None."""
    ch = grpc.insecure_channel(f"127.0.0.1:{port}")
    try:
        grpc.channel_ready_future(ch).result(timeout=10)
    except grpc.FutureTimeoutError:
        tally.check(False, "server never came up")
        ch.close()
        return None
    return ch
