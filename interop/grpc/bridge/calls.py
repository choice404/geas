"""calls.py: the unary rpcs, each one runtime call against an admitted
instance.

A pledge rpc fulfills the pledge and answers its declared return, Err
included, on an OK rpc. A signal rpc delivers the signal and answers the
empty SignalReply, a refusal riding the call's status. GetPartial reads the
partial surface with the state, Remedying included where the file's enum
carries it, GetCourse reads where an engine driven contract's course stands
without waiting on it, and Break ends the contract while the row stays
behind its stream, so a broken instance still says broken. Every reply is
built while the call is admitted, so nothing a reply reads outlives the
instance it came from.
"""

from geas import GEAS_ERR_TYPE, STATE_NAMES, GeasError

from .convert import ConversionError, error_code, fill_message, request_args
from .handlers import ANSWERS
from .sessions import break_quietly, cancel_quietly
from .status import abort_argument, abort_reply, abort_status, abort_unknown
from .surface import BREAK, COURSE, PARTIAL, PLEDGE, SIGNAL
from .table import UnknownInstance


class Calls:
    def __init__(self, table):
        self._table = table

    def handler(self, surface, method):
        """The behavior of one unary rpc."""
        behaviors = {PLEDGE: self._pledge, SIGNAL: self._signal,
                     PARTIAL: self._partial, BREAK: self._break,
                     COURSE: self._course}
        behavior = behaviors[method.role]
        return lambda request, context: behavior(surface, method, request, context)

    def _run(self, surface, request, context, what, action):
        """Runs action(contract) with the instance admitted. An unknown id is
        NOT_FOUND and a Geas status its gRPC code."""
        try:
            inst = self._table.acquire(request.instance_id, surface.contract)
        except UnknownInstance:
            abort_unknown(context, request.instance_id)
        try:
            return action(inst.contract)
        except GeasError as e:
            abort_status(context, e, what)
        finally:
            inst.gate.leave()

    @staticmethod
    def _args(method, request, context):
        try:
            return request_args(request)
        except ValueError as e:
            # ConversionError is one, and protobuf's own reads raise the rest
            abort_argument(context, method.target, e)

    def _pledge(self, surface, method, request, context):
        """The reply is decoded inside the gate, so the value it reads off
        the instance heap is copied out before an ending can reclaim it."""
        args = self._args(method, request, context)
        name = f"{surface.contract}.{method.target}"

        def fulfill_and_fill(contract):
            reply = method.reply()
            with ANSWERS.watching(contract, name) as ran:
                try:
                    value = contract.fulfill_sync(method.target, *args)
                except GeasError as e:
                    # A supplied body ran, so the arguments conformed and
                    # the body's own answer did not
                    if e.status == GEAS_ERR_TYPE and ran[0]:
                        raise ConversionError("the runtime refused the supplied "
                                              "body's answer, GEAS_ERR_TYPE") from e
                    raise
            fill_message(reply, value)
            return reply
        try:
            return self._run(surface, request, context,
                             f"fulfill {method.target}", fulfill_and_fill)
        except ConversionError as e:
            abort_reply(context, method.target, e)

    def _signal(self, surface, method, request, context):
        args = self._args(method, request, context)
        self._run(surface, request, context, f"signal {method.target}",
                  lambda c: c.signal(method.target, *args))
        return method.reply()

    def _partial(self, surface, method, request, context):
        partial, state = self._run(surface, request, context, "partial",
                                   lambda c: (c.partial(), c.state()))
        reply = method.reply(state=state_number(surface, state))
        reply.fulfilled.extend(partial.fulfilled)
        reply.pending.extend(partial.pending)
        reply.broken.extend(partial.broken)
        for name, value in partial.errors:
            if value is not None:
                reply.errors.add(pledge=name, err=error_code(value))
        return reply

    def _break(self, surface, method, request, context):
        """Break ends the contract while its stream stays, so it drains the
        gate rather than closing it: the bodies are asked to stop, the calls
        already admitted finish, the break runs with nothing in flight, and
        the calls held back then reach the broken instance. A gate already
        closed belongs to an ending under way, which reclaims the instance
        itself."""
        try:
            inst = self._table.lookup(request.instance_id, surface.contract)
        except UnknownInstance:
            abort_unknown(context, request.instance_id)
        cancel_quietly(inst.contract)
        if not inst.gate.drain():
            abort_unknown(context, request.instance_id)
        try:
            break_quietly(inst.contract)
        finally:
            inst.gate.reopen()
        return method.reply()

    def _course(self, surface, method, request, context):
        info = self._run(surface, request, context, "course",
                         lambda c: c.course_info())
        return method.reply(state=info.state, step=info.step,
                            status=info.status, wait_due_ms=info.wait_due_ms)


def state_number(surface, state):
    """The runtime state in the file's ContractState enum. A file without
    the workflow tail has no REMEDYING, and none of its contracts reach it;
    the break it ends in is the nearest name it does carry."""
    name = STATE_NAMES.get(state, "Broken").upper()
    value = surface.state_enum.values_by_name.get(name)
    if value is None:
        value = surface.state_enum.values_by_name["BROKEN"]
    return value.number
