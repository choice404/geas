"""walk_stall.py: endings beside a body in flight, and the session limit.

Stall.hold's body returns only once a cancel request reaches it. The break
walk calls Hold and, while it runs, Break on the same instance: Break asks
the body to stop, waits for the call it admitted, and breaks, so both rpcs
return and the instance reads broken with its stream still open. The drop
walk calls Hold and closes the stream under it: the ending asks the body to
stop and waits it out before the instance goes. After each, a fresh session
on the same server still answers, which is the server alive.

The admission walk fills the server's session limit, shows a unary call on
a live instance still answering on the reserved workers, shows one more
Session refused with RESOURCE_EXHAUSTED, and shows the slot coming back
once a session closes.
"""

import time
from concurrent import futures

import grpc

from clientkit import Session, wait_gone

CALL_S = 15.0
SETTLE_S = 0.3


def _stubs():
    import stall_pb2 as pb
    import stall_pb2_grpc as pb_grpc
    return pb, pb_grpc


def _hold_beside(stub, pb, iid, pool):
    """Starts Hold on iid and answers its future once the body is running."""
    held = pool.submit(stub.Hold, pb.HoldRequest(instance_id=iid), timeout=CALL_S)
    time.sleep(SETTLE_S)
    return held


def _outcome(future):
    try:
        out = future.result(timeout=CALL_S)
        if "result" not in out.DESCRIPTOR.oneofs_by_name:
            return "ok"
        return f"ok {out.WhichOneof('result')}"
    except grpc.RpcError as e:
        return e.code().name
    except futures.TimeoutError:
        return "hung"


def _server_alive(stub, pb, tally, how):
    with Session(stub, pb.SignRequest()) as session:
        out = stub.Ping(pb.PingRequest(instance_id=session.instance_id), timeout=CALL_S)
        tally.check(out.WhichOneof("result") == "ok" and out.ok,
                    f"the server answers a fresh session after {how}")


def break_beside_hold(stub, pb, tally, pool):
    with Session(stub, pb.SignRequest()) as session:
        iid = session.instance_id
        held = _hold_beside(stub, pb, iid, pool)
        broke = pool.submit(stub.Break, pb.BreakRequest(instance_id=iid), timeout=CALL_S)
        hold_out, break_out = _outcome(held), _outcome(broke)
        tally.check(hold_out == "ok ok", f"Hold beside Break returned, got {hold_out}")
        tally.check(break_out.startswith("ok"), f"Break beside Hold returned, got {break_out}")
        p = stub.GetPartial(pb.PartialRequest(instance_id=iid), timeout=CALL_S)
        tally.check(p.state == pb.BROKEN, "the broken instance still reads broken")
        tally.expect_code(lambda: stub.Ping(pb.PingRequest(instance_id=iid)),
                          grpc.StatusCode.FAILED_PRECONDITION,
                          "a pledge after Break")
    _server_alive(stub, pb, tally, "a Break beside a hold")


def drop_beside_hold(stub, pb, tally, pool):
    session = Session(stub, pb.SignRequest())
    iid = session.instance_id
    held = _hold_beside(stub, pb, iid, pool)
    session.close()
    hold_out = _outcome(held)
    tally.check(hold_out != "hung", f"Hold under a dropped stream returned, got {hold_out}")
    tally.check(wait_gone(stub, pb.PartialRequest, iid, CALL_S) is not None,
                "the dropped instance left the table")
    _server_alive(stub, pb, tally, "a stream dropped beside a hold")


def walk(channel, tally):
    pb, pb_grpc = _stubs()
    stub = pb_grpc.StallStub(channel)
    with futures.ThreadPoolExecutor(max_workers=4) as pool:
        break_beside_hold(stub, pb, tally, pool)
        drop_beside_hold(stub, pb, tally, pool)
    print("[edge_client] endings beside a hold in flight returned")


def _open_until(stub, pb, deadline):
    """A Session once a slot is free again, else the last refusal."""
    while True:
        try:
            return Session(stub, pb.SignRequest())
        except grpc.RpcError as e:
            if e.code() != grpc.StatusCode.RESOURCE_EXHAUSTED or time.monotonic() > deadline:
                return e
            time.sleep(0.02)


def admission_walk(channel, tally, limit):
    pb, pb_grpc = _stubs()
    stub = pb_grpc.StallStub(channel)
    sessions = [Session(stub, pb.SignRequest()) for _ in range(limit)]
    try:
        out = stub.Ping(pb.PingRequest(instance_id=sessions[0].instance_id), timeout=CALL_S)
        tally.check(out.WhichOneof("result") == "ok" and out.ok,
                    f"a unary rpc answers with {limit} sessions live")
        p = stub.GetPartial(pb.PartialRequest(instance_id=sessions[-1].instance_id),
                            timeout=CALL_S)
        tally.check(p.state == pb.PARTIAL or p.state == pb.SIGNED,
                    "GetPartial answers with the limit held")
        tally.expect_code(lambda: Session(stub, pb.SignRequest()),
                          grpc.StatusCode.RESOURCE_EXHAUSTED, "a session past the limit")
        sessions.pop().close()
        again = _open_until(stub, pb, time.monotonic() + CALL_S)
        tally.check(isinstance(again, Session), "a closed session gives its slot back")
        if isinstance(again, Session):
            sessions.append(again)
    finally:
        for session in sessions:
            session.close()
    print(f"[edge_client] {limit} sessions admitted, the next refused")
