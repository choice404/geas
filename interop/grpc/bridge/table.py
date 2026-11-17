"""table.py: the live instances, keyed by a server issued id.

An instance lives exactly as long as its Session stream. The table is the
session store gRPC does not provide: ids are server issued and monotonic,
never client supplied, so a peer cannot name an instance it was not handed.

Every rpc against an instance passes its gate. Calls run side by side, the
runtime serializing what must be serialized on its own instance lock. The
ending begins by closing the gate, which answers True to its first caller
only and so makes ending a session exactly once free, then waits out the
calls already admitted, so a body in flight finishes against a live
instance and its reply is decoded before the heap it reads goes. A call
that arrives while the ending runs waits for it and then reads the id as
gone, and the row leaves the table only once the ending is over, so a
NOT_FOUND on an ended id means its park row, where it has one, is written.
Break drains the gate instead of closing it: new calls wait while the
admitted ones finish, the break runs with nothing in flight, and the calls
held back then reach a broken instance that still answers.
"""

import threading
from dataclasses import dataclass


class CallGate:
    """Admits calls until an ending closes it. A drain holds new calls back
    while it waits the admitted ones out, for Break, which is not itself
    admitted, and then lets them through again."""

    def __init__(self):
        self._cond = threading.Condition()
        self._active = 0
        self._is_draining = False
        self._is_closing = False
        self._is_closed = False

    def _is_held(self):
        return (self._is_draining or self._is_closing) and not self._is_closed

    def enter(self):
        with self._cond:
            while self._is_held():
                self._cond.wait()
            if self._is_closed:
                return False
            self._active += 1
            return True

    def leave(self):
        with self._cond:
            self._active -= 1
            if self._active == 0:
                self._cond.notify_all()

    def drain(self):
        """Holds new calls back and waits until no admitted call is left,
        one drain at a time. Answers False, holding nothing, once an ending
        has begun, since the ending reclaims the instance itself."""
        with self._cond:
            while self._is_draining and not self._is_closing:
                self._cond.wait()
            if self._is_closing:
                return False
            self._is_draining = True
            while self._active:
                self._cond.wait()
            return True

    def reopen(self):
        """Ends a drain; the calls it held back are admitted."""
        with self._cond:
            self._is_draining = False
            self._cond.notify_all()

    def begin_close(self):
        """Admits nothing more. Answers True to the first caller only, the
        one that then owns the ending."""
        with self._cond:
            if self._is_closing:
                return False
            self._is_closing = True
            self._cond.notify_all()
            return True

    def wait_idle(self):
        """Waits out the admitted calls and a drain under way, so the ending
        runs after the Break that drained."""
        with self._cond:
            while self._active or self._is_draining:
                self._cond.wait()

    def finish(self):
        """The ending is over; every call held at the gate reads it gone."""
        with self._cond:
            self._is_closed = True
            self._cond.notify_all()


@dataclass(frozen=True)
class Instance:
    """One signed instance, written once at sign and read until the stream
    drops it. done releases the Session handler that holds the stream, and
    has_course marks an engine driven contract, whose ending parks without
    asking its course to stop first."""
    instance_id: int
    contract_name: str
    contract: object
    park_token: str
    gate: CallGate
    done: threading.Event
    has_course: bool = False


class UnknownInstance(LookupError):
    """An id this contract's table does not hold."""


class InstanceTable:
    def __init__(self):
        self._lock = threading.Lock()
        self._next_id = 1
        self._live = {}

    def insert(self, contract_name, contract, park_token="", has_course=False):
        with self._lock:
            instance_id = self._next_id
            self._next_id += 1
            inst = Instance(instance_id, contract_name, contract, park_token,
                            CallGate(), threading.Event(), has_course)
            self._live[instance_id] = inst
            return inst

    def get(self, instance_id):
        with self._lock:
            return self._live.get(instance_id)

    def remove(self, instance_id):
        with self._lock:
            return self._live.pop(instance_id, None)

    def ids(self):
        with self._lock:
            return list(self._live)

    def lookup(self, instance_id, contract_name):
        """The live instance under an id, admitted or not, for an ending that
        drains the gate instead of entering it. Raises UnknownInstance like
        acquire."""
        with self._lock:
            inst = self._live.get(instance_id)
        if inst is None or inst.contract_name != contract_name:
            raise UnknownInstance(instance_id)
        return inst

    def acquire(self, instance_id, contract_name):
        """Admits one call against an instance of contract_name and answers
        it; the caller releases with inst.gate.leave(). An id that is not
        live, belongs to another contract, or whose session is ending raises
        UnknownInstance: from the caller's side all three are an id that
        names nothing here."""
        inst = self.lookup(instance_id, contract_name)
        if not inst.gate.enter():
            raise UnknownInstance(instance_id)
        return inst
