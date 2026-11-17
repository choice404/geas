"""sessions.py: what bounds the live sessions and what ends them.

A Session or Resume stream holds one gRPC worker thread for as long as the
instance lives, so live sessions draw on the same pool every pledge, signal,
GetPartial, GetCourse, and Break needs. SessionSlots admits at most the pool
less a reserve of workers kept for those unary calls, and a stream beyond
that is refused with RESOURCE_EXHAUSTED before it signs or claims anything,
so a full server still answers the instances it holds.

An ending waits on work in flight, the calls the gate admitted and the
bodies they run, and gRPC calls the termination callback on a thread of its
own that must not block. Reaper is where endings run: the callback hands the
ending over and returns, and a small pool of its own threads carries it out.
Shutdown drains it before the runtime goes.

The runtime calls an ending makes are here too, each answering quietly for
the one status an ending expects and logging any other.
"""

import logging
import threading
from concurrent import futures

from geas import GEAS_ERR_STATE, GeasError

log = logging.getLogger("bridge_server")

# Workers kept for unary rpcs when --reserve-workers is not given
DEFAULT_RESERVE_WORKERS = 4

# Threads that carry endings out, apart from the gRPC pool
REAPER_THREADS = 4


class AdmissionError(ValueError):
    """A pool too small to hold both a session and the reserve."""


class SessionSlots:
    """Counts live session streams against the limit the pool allows."""

    def __init__(self, max_workers, reserve_workers):
        if reserve_workers < 1:
            raise AdmissionError("--reserve-workers must keep at least one "
                                 "worker for unary rpcs")
        if max_workers - reserve_workers < 1:
            raise AdmissionError(f"--max-workers {max_workers} leaves no room for a "
                                 f"session beside {reserve_workers} reserved workers")
        self.limit = max_workers - reserve_workers
        self._lock = threading.Lock()
        self._taken = 0

    def try_take(self):
        """Takes one slot, or answers False when every slot is held."""
        with self._lock:
            if self._taken >= self.limit:
                return False
            self._taken += 1
            return True

    def give_back(self):
        with self._lock:
            self._taken -= 1


class Reaper:
    """Runs endings off the threads that must not wait on them."""

    def __init__(self):
        self._pool = futures.ThreadPoolExecutor(max_workers=REAPER_THREADS,
                                                thread_name_prefix="bridge-end")

    def submit(self, fn, *args):
        """Hands fn over and returns at once; a failure is logged with its
        traceback, since no caller is left to hear it."""
        def run():
            try:
                fn(*args)
            except Exception:
                log.exception("an ending raised")
        try:
            self._pool.submit(run)
        except RuntimeError as e:
            # A reaper already shut down runs the ending inline
            log.info("reaper closed (%s), ending inline", e)
            run()

    def shutdown(self):
        self._pool.shutdown(wait=True)


def _quietly(what, call):
    """Runs one runtime call an ending makes. GEAS_ERR_STATE is the answer of
    an instance already over, a break of a broken one or a cancel of one
    reclaimed, and is not an error to an ending; anything else is logged.
    Answers whether the call went through."""
    try:
        call()
        return True
    except GeasError as e:
        if e.status != GEAS_ERR_STATE:
            log.error("%s answered %s", what, e.status_name)
        return False


def cancel_quietly(contract):
    """Asks the instance's bodies to stop, so a body that polls returns and
    the gate drains."""
    return _quietly("cancel request", contract.request_cancel)


def break_quietly(contract):
    """Breaks an instance; one the contract already broke, or one that
    fulfilled, answers GEAS_ERR_STATE, which is not an error to reclaim."""
    return _quietly("break", contract.break_)


def release_quietly(contract):
    """Lets a parked instance go with no remedy run, its row now the
    obligation's only home."""
    return _quietly("release", contract.release)
