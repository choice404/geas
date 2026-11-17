"""lifecycle.py: the two streams that own an instance, and its one ending.

Session signs, yields the signature as the stream's first event, and then
blocks for as long as the peer holds the stream. gRPC calls the termination
callback the moment the stream ends, for any reason, and the callback hands
the ending to the reaper, which ends the session off gRPC's own thread.
There is no window in which an instance exists without a stream, and no
timer anywhere decides a contract's ending.

An ending asks the instance's bodies to stop, closes the gate and waits out
the calls it admitted, and then either breaks the instance or, with a park
store, parks it under the session's token and releases it: the runtime lets
the instance go with no remedy run, the row is the obligation's only home,
and whichever replica resumes the token owes whatever the contract owes.
An engine driven contract parks without the request, since a course the
request cut would reach the row as cancelled.

Resume claims a row, the runtime's one move that resumes the instance and
deletes its row, so a token is spent the moment one replica holds it: of two
replicas resuming one token over a shared store exactly one claim takes the
row and the other answers NOT_FOUND, the same answer a spent token gets.

A Session or Resume holds a gRPC worker for its whole life, so the server
admits only as many as its pool spares beside the reserve its unary calls
need, and answers one more with RESOURCE_EXHAUSTED.
"""

import logging
import uuid
from dataclasses import dataclass

import grpc

from geas import NONE, GEAS_COURSE_RUNNING, GeasError

from .convert import field_value, set_field
from .sessions import break_quietly, cancel_quietly, release_quietly
from .status import abort_argument, abort_status

log = logging.getLogger("bridge_server")

# How long an ending waits for a running course to settle before parking it
COURSE_SETTLE_MS = 5000


@dataclass(frozen=True)
class ParkStore:
    """The sqlite park store a closed session writes its row to and a
    Resume claims it from."""
    dsn: str


def sign_vows(surface, request):
    """The vow overrides a SignRequest carries; an unset field takes the
    declared default."""
    vows = {}
    for field in surface.vow_fields:
        value = field_value(request, field)
        if value is not NONE:
            vows[field.name] = value.value
    return vows


class Lifecycle:
    def __init__(self, runtime, table, park_store, slots, reaper):
        self._rt = runtime
        self._table = table
        self._park = park_store
        self._slots = slots
        self._reaper = reaper

    def session(self, surface, request, context):
        """The signing rpc: admit, sign, hand back the signature, hold."""
        self._admit(context)
        try:
            yield from self._sign_and_hold(surface, request, context)
        finally:
            self._slots.give_back()

    def resume(self, surface, request, context):
        """Claims one parked row and holds its new stream under the same
        token, so a later drop parks the same session name again."""
        if not self._park:
            context.abort(grpc.StatusCode.FAILED_PRECONDITION,
                          "server runs without a park store")
        self._admit(context)
        try:
            yield from self._claim_and_hold(surface, request, context)
        finally:
            self._slots.give_back()

    def _admit(self, context):
        if self._slots.try_take():
            return
        context.abort(grpc.StatusCode.RESOURCE_EXHAUSTED,
                      f"the server holds its limit of {self._slots.limit} live "
                      "sessions, close one or try again later")

    def _sign_and_hold(self, surface, request, context):
        try:
            vows = sign_vows(surface, request)
        except ValueError as e:
            abort_argument(context, "sign", e)
        try:
            contract = self._rt.sign(surface.contract, vows=vows or None,
                                     expected_hash=request.expected_hash)
        except GeasError as e:
            abort_status(context, e, f"sign {surface.contract}")
        token = uuid.uuid4().hex if self._park else ""
        inst = self._table.insert(surface.contract, contract, token,
                                  surface.has_course)
        yield from self._hold(surface, inst, context, "signed")

    def _claim_and_hold(self, surface, request, context):
        """The claim always checks the row against this service's shape, so
        a wrong hash, a row of another contract, and a row of another build
        are all refused with ABORTED before the row is touched."""
        token = request.park_token
        if request.expected_hash not in (0, surface.shape_hash):
            context.abort(grpc.StatusCode.ABORTED,
                          f"resume: expected hash {request.expected_hash:#x} is "
                          f"not {surface.contract}'s {surface.shape_hash:#x}")
        try:
            contract = self._rt.claim(self._park.dsn, token, surface.shape_hash)
        except GeasError as e:
            abort_status(context, e, "resume")
        inst = self._table.insert(surface.contract, contract, token,
                                  surface.has_course)
        yield from self._hold(surface, inst, context, "resumed")

    def _hold(self, surface, inst, context, verb):
        """Yields the signature and holds the stream. add_callback answers
        False when the rpc is already over, the one case the callback cannot
        cover, so that case ends the session inline; the finally is the third
        path, free because ending is idempotent by the table."""
        instance_id = inst.instance_id
        log.info("session %d %s, %s", instance_id, verb, surface.contract)
        # The signature is read before the callback can end the instance
        try:
            event = signed_event(surface, inst)
        except Exception:
            self.end(instance_id, "signature could not be read")
            raise
        on_done = lambda: self._reaper.submit(self.end, instance_id, "stream terminated")
        if not context.add_callback(on_done):
            self.end(instance_id, "stream was already over")
            return
        try:
            yield event
            inst.done.wait()
        finally:
            self.end(instance_id, "handler exit")

    def end(self, instance_id, reason):
        """Ends a session, once, whoever calls first. The gate closes so no
        new call is admitted, the bodies are asked to stop, and the calls
        admitted before are waited out, their replies decoded; then the
        instance parks and is released, or breaks, and only then does its
        row leave the table."""
        inst = self._table.get(instance_id)
        if inst is None or not inst.gate.begin_close():
            return
        inst.done.set()
        try:
            action = self._finish(inst, reason)
            log.info("session %d ended (%s), %s", instance_id, reason, action)
        finally:
            self._table.remove(instance_id)
            inst.gate.finish()

    def _finish(self, inst, reason):
        if not (self._park and inst.has_course):
            cancel_quietly(inst.contract)
        inst.gate.wait_idle()
        is_parked = self._park_instance(inst, reason)
        if is_parked and release_quietly(inst.contract):
            return "parked, released and dropped"
        break_quietly(inst.contract)
        return "broken and dropped"

    def _park_instance(self, inst, reason):
        if not self._park:
            return False
        if inst.has_course:
            self._settle_course(inst.contract)
        try:
            inst.contract.park(self._park.dsn, inst.park_token)
            return True
        except GeasError as e:
            log.info("session %d (%s) did not park, park answered %s",
                     inst.instance_id, reason, e.status_name)
            return False

    @staticmethod
    def _settle_course(contract):
        """A course parks while it waits or after it ended, so a running one
        is given a moment to reach its next wait first."""
        try:
            info = contract.course_wait(COURSE_SETTLE_MS)
        except GeasError as e:
            log.info("course wait before the park answered %s", e.status_name)
            return
        if info.state == GEAS_COURSE_RUNNING:
            log.info("course still running after %dms, parking it as it stands",
                     COURSE_SETTLE_MS)

    def shutdown(self):
        """Ends every session still standing the way a dropped stream does,
        parked and released where the server has a store, so a restart can
        resume it, and broken where it has none, since a signed contract
        that outlives its server with no row is an obligation nobody can
        keep. The reaper drains after, so no ending outlives the runtime."""
        for instance_id in self._table.ids():
            self.end(instance_id, "server stopping")
        self._reaper.shutdown()


def signed_event(surface, inst):
    """The stream's first event: the id, the effective vows, the shape hash,
    the signing time, and the park token."""
    event = surface.event()
    signed = getattr(event, surface.signed_field.name)
    contract = inst.contract
    signed.instance_id = inst.instance_id
    for field in surface.vow_fields:
        set_field(signed, signed.DESCRIPTOR.fields_by_name[field.name],
                  contract.vow(field.name))
    signed.shape_hash = contract.hash()
    signed.signed_at = contract.signed_at()
    signed.park_token = inst.park_token
    return event
