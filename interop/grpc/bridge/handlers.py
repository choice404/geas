"""handlers.py: the bodies a deployment supplies for abstract pledges and
remedies.

A contract leaves a body abstract when the host owns it, a payment gateway,
a warehouse, a carrier. The generic server knows no contract, so the bodies
come from the deployer: each --handlers file is a plain Python module whose
HANDLERS dict maps a "Contract.pledge" or "Contract.remedy" name to a
callable in the geas package's bind form, fn(instance, args) answering a Python
value, Ok and Err included. The server binds every entry before it
freezes. A name nothing declares fails startup, and an abstract body no
file supplies leaves its contract refusing to sign with GEAS_ERR_UNBOUND,
which crosses as FAILED_PRECONDITION on the Session call.

A body that raises is logged here with its traceback and then reported to
the runtime the way the geas package reports any raising body, GEAS_ERR_TYPE, the
fulfillment produced no value. The runtime holds what a body answers to the
declared return and answers GEAS_ERR_TYPE for one that does not conform, the
same status a malformed argument gets, so every body supplied here is watched:
a pledge rpc whose fulfillment reports GEAS_ERR_TYPE after one of these bodies
ran on it had its arguments accepted, and the fault is the server's own, not
the client's. ANSWERS is that watch.

A session that parks hands its obligation to the row and the runtime then
releases the live instance, which reclaims it with no remedy run, so a body
supplied here runs only for an instance that is still the obligation's home,
and a remedy, a refund, a released hold, runs once, on whichever replica
holds the contract when it breaks for real.
"""

import importlib.util
import logging
import threading
from contextlib import contextmanager
from pathlib import Path

from geas import GeasError

log = logging.getLogger("bridge_server")


class HandlerError(RuntimeError):
    """A handlers file the server cannot bind."""


def _load_file(path, index):
    resolved = Path(path).resolve()
    if not resolved.is_file():
        raise HandlerError(f"no handlers file at {Path(path).name}")
    spec = importlib.util.spec_from_file_location(f"geas_handlers_{index}", resolved)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    table = getattr(module, "HANDLERS", None)
    if not isinstance(table, dict):
        raise HandlerError(f"{resolved.name} defines no HANDLERS dict")
    return resolved.name, table


def load_handlers(paths):
    """Every file's HANDLERS merged; a name supplied twice is refused."""
    merged = {}
    for index, path in enumerate(paths):
        source, table = _load_file(path, index)
        for name, fn in table.items():
            if not callable(fn):
                raise HandlerError(f"{source}: {name} is not callable")
            if name in merged:
                raise HandlerError(f"{source}: {name} is supplied twice")
            merged[name] = fn
    return merged


class AnswerWatch:
    """Which supplied bodies ran under a pledge rpc in flight. A body may run
    on a runtime worker rather than the rpc's own thread, so the watch is
    keyed by the instance address and the qualified pledge name, and a call
    watches only for as long as it is in flight."""

    def __init__(self):
        self._lock = threading.Lock()
        self._calls = {}

    @contextmanager
    def watching(self, contract, name):
        """Yields a one element list that reads [True] once a supplied body
        for name ran on contract while the call was in flight."""
        key = (contract.address, name)
        ran = [False]
        with self._lock:
            self._calls.setdefault(key, []).append(ran)
        try:
            yield ran
        finally:
            with self._lock:
                # By identity: two calls in flight hold equal lists
                calls = [x for x in self._calls.get(key, []) if x is not ran]
                if calls:
                    self._calls[key] = calls
                else:
                    self._calls.pop(key, None)

    def mark(self, contract, name):
        with self._lock:
            for ran in self._calls.get((contract.address, name), []):
                ran[0] = True


ANSWERS = AnswerWatch()


def _logged(name, fn):
    def body(inst, args):
        ANSWERS.mark(inst, name)
        try:
            return fn(inst, args)
        except Exception:
            log.exception("handler %s raised", name)
            raise
    return body


def bind_handlers(runtime, handlers):
    for name, fn in handlers.items():
        try:
            runtime.bind(name, _logged(name, fn))
        except GeasError as e:
            raise HandlerError(f"cannot bind {name}: {e.status_name}") from e
        log.info("bound %s", name)
