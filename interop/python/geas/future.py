"""Fulfillment receipts and their by reference storage."""

import ctypes as C

from ._abi import GeasValue
from .errors import _check
from .values import decode


class Future:
    """One fulfillment's receipt. wait() delivers exactly once; the Refs it
    carries are anchored here so their storage survives until the write
    back the delivering wait performs."""

    def __init__(self, rt, ptr, refs):
        self._rt = rt
        self._ptr = ptr
        self._refs = refs

    def wait(self):
        out = GeasValue()
        _check(self._rt._lib.geas_future_wait(self._ptr, C.byref(out)),
               "geas_future_wait")
        return decode(out)

    def poll(self):
        """Whether an outcome exists, without delivering or writing refs."""
        ready = C.c_int()
        _check(self._rt._lib.geas_future_poll(self._ptr, C.byref(ready)),
               "geas_future_poll")
        return bool(ready.value)

    def release(self):
        """Releases a waited or forfeited receipt and invalidates its handle."""
        _check(self._rt._lib.geas_future_release(self._ptr),
               "geas_future_release")
        self._ptr = None
        self._refs = None
