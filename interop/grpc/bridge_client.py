"""bridge_client.py: the demo_payment.py walk, driven over gRPC instead of
the C ABI, asserting the same answers, with the session held as a stream.

This client never touches libgeasrt, holds no handle, and knows the
instance only by a uint64 the server issued on a stream it keeps open. Its
stubs are protoc's output over the payment.proto geas emitted, the same file
the generic server reads at startup. If the two agree pledge for pledge and
state for state, the session model carries the contract across a process
boundary.

The payment walk checks: the vow override lands, the pending order holds,
half a subcontract moves nothing, Validation lands and the instance goes
partial, charge answers its declared Bool through a Python body two
boundaries away, the instance fulfills, Charge(-2.0) answers err=41 on an OK
rpc and the contract's own break line fires, a fulfillment against a broken
instance is FAILED_PRECONDITION, an id nobody was issued is NOT_FOUND, a
client that dies takes its instance with it in milliseconds, a client that
holds its stream quietly keeps its instance, and an explicit Break still
reads broken until the stream leaves.

The other modes drive the rest of the surface: the park, resume, and race
walks the resume and failover gates run, the host driven saga with its
signal, the engine driven course read through GetCourse, and the language
gauntlet whose lists, maps, tuples, and options cross through the generic
server's reflection.

Exit status 0 means every check below held.
"""

import argparse
import string
import subprocess
import sys
import time
from pathlib import Path

import grpc

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "target" / "grpc-gen"))

from clientkit import Session, Tally, channel_ready, wait_gone  # noqa: E402
from walk_race import DEFAULT_ROUNDS  # noqa: E402

TALLY = Tally("bridge_client")
check = TALLY.check
expect_code = TALLY.expect_code


def _payment():
    import payment_pb2 as pb
    import payment_pb2_grpc as pb_grpc
    return pb, pb_grpc


def gone(stub, pb, instance_id, timeout=5.0):
    return wait_gone(stub, pb.PartialRequest, instance_id, timeout)


def check_ok(out, what):
    check(out.WhichOneof("result") == "ok" and out.ok is True, what)


def park_walk(stub, pb):
    """Parks a partly progressed session and leaves its token as the one
    machine readable line, for the restart half of the round trip."""
    with Session(stub, pb.SignRequest(currency="EUR")) as session:
        signed = session.signed
        token = signed.park_token
        check(signed.instance_id != 0, "the park session issued an instance id")
        check(signed.shape_hash != 0,
              "the park session carries a nonzero shape hash")
        check(len(token) == 32 and all(ch in string.hexdigits for ch in token),
              "the park session carries a hex park token")
        check_ok(stub.ValidateCard(pb.ValidateCardRequest(
            instance_id=session.instance_id, card="4111 1111")),
            "the park session latched validate_card")
        print(token, flush=True)

    check(gone(stub, pb, signed.instance_id) is not None,
          "closing the park stream dropped its row")


def resume_walk(stub, pb, token):
    """Reopens one parked session and proves its latch crossed the store and
    a server restart, while the bad and repeated token cases stay one shot."""
    expect_code(lambda: Session(
        stub, pb.ResumeRequest(park_token="not-a-park-token"), stub.Resume),
        grpc.StatusCode.NOT_FOUND, "a garbage park token")
    expect_code(lambda: Session(
        stub, pb.ResumeRequest(park_token=token, expected_hash=12345),
        stub.Resume), grpc.StatusCode.ABORTED, "a wrong resume shape hash")

    with Session(stub, pb.ResumeRequest(park_token=token), stub.Resume) as session:
        iid = session.instance_id
        check(iid != 0, "the resumed stream issued a fresh instance id")
        check(session.signed.park_token == token,
              "the resumed stream kept its park token")
        expect_code(lambda: Session(
            stub, pb.ResumeRequest(park_token=token), stub.Resume),
            grpc.StatusCode.NOT_FOUND, "a consumed park token")
        finish_parked(stub, pb, iid, "resumed")

    check(gone(stub, pb, iid) is not None,
          "closing the resumed stream dropped its row")


def finish_parked(stub, pb, iid, how):
    """The walk a parked payment session finishes after its resume."""
    check_ok(stub.ValidateAmount(pb.ValidateAmountRequest(instance_id=iid, amount=25.0)),
             f"the {how} validation fulfilled")
    partial = stub.GetPartial(pb.PartialRequest(instance_id=iid))
    check(partial.state == pb.PARTIAL and list(partial.fulfilled) == ["Validation"],
          f"the {how} validate_card latch crossed park and resume")
    check_ok(stub.Charge(pb.ChargeRequest(instance_id=iid, card="4111 1111", amount=25.0)),
             f"the {how} charge fulfilled")
    check_ok(stub.NotifyUser(pb.NotifyUserRequest(instance_id=iid, ok=True)),
             f"the {how} notify_user fulfilled")
    partial = stub.GetPartial(pb.PartialRequest(instance_id=iid))
    check(partial.state == pb.FULFILLED, f"the {how} instance fulfilled")


def partial_path(stub, pb):
    """demo_payment.py's partial path, signed with a vow override."""
    with Session(stub, pb.SignRequest(currency="EUR")) as s1:
        c1 = s1.instance_id
        check(c1 != 0, "the session stream issued an instance id")
        check(s1.signed.currency == "EUR", "vow override landed across the wire")
        check(s1.signed.shape_hash != 0 and s1.signed.signed_at > 0,
              "c1 carries a signature")
        print(f"[bridge_client] session {c1} open, currency {s1.signed.currency}")

        p = stub.GetPartial(pb.PartialRequest(instance_id=c1))
        check(list(p.pending) == ["Validation", "Processing", "notify_user"],
              "c1 pending order is subs then loose pledges")
        check_ok(stub.ValidateCard(pb.ValidateCardRequest(instance_id=c1, card="4111 1111")),
                 "validate_card Ok")
        p = stub.GetPartial(pb.PartialRequest(instance_id=c1))
        check(p.state == pb.SIGNED, "half a subcontract moves nothing")
        check_ok(stub.ValidateAmount(pb.ValidateAmountRequest(instance_id=c1, amount=25.0)),
                 "validate_amount Ok")
        p = stub.GetPartial(pb.PartialRequest(instance_id=c1))
        check(p.state == pb.PARTIAL, "Validation lands, c1 partial")
        check(list(p.fulfilled) == ["Validation"] and
              list(p.pending) == ["Processing", "notify_user"] and
              list(p.broken) == [] and list(p.errors) == [],
              "partial names after Validation")

        # charge is the Python body behind the abstract pledge, supplied by
        # the server's handlers file: the answer crosses two boundaries, the
        # C ABI into Python and Python out over gRPC
        check_ok(stub.Charge(pb.ChargeRequest(instance_id=c1, card="4111 1111", amount=25.0)),
                 "charge Ok carries the declared Bool")
        check_ok(stub.NotifyUser(pb.NotifyUserRequest(instance_id=c1, ok=True)),
                 "notify_user Ok")
        p = stub.GetPartial(pb.PartialRequest(instance_id=c1))
        check(p.state == pb.FULFILLED, "c1 fulfilled")
        check(len(p.fulfilled) == 3 and list(p.pending) == [] and
              list(p.errors) == [], "every item fulfilled")
        print("[bridge_client] state FULFILLED")

    check(gone(stub, pb, c1) is not None, "closing c1's stream dropped its row")


def err_path(stub, pb):
    """The contract's Err as a value, the automatic break, and the explicit
    one, whose row stays readable behind its stream."""
    with Session(stub, pb.SignRequest()) as s2:
        c2 = s2.instance_id
        check(s2.signed.currency == "USD", "c2 signs on the declared default")
        out = stub.Charge(pb.ChargeRequest(instance_id=c2, card="4111 1111", amount=-2.0))
        check(out.WhichOneof("result") == "err" and out.err == 41,
              "charge Err returns to the caller as a value, not an rpc error")
        print(f"[bridge_client] charge answered err={out.err} on an OK rpc")

        p = stub.GetPartial(pb.PartialRequest(instance_id=c2))
        check(p.state == pb.BROKEN, "the break line fired by itself")
        check(list(p.broken) == ["Processing"] and
              list(p.pending) == ["Validation", "notify_user"],
              "c2 broken lists Processing")
        check([(e.pledge, e.err) for e in p.errors] == [("charge", 41)],
              "the automatic break kept the Err payload readable")
        expect_code(lambda: stub.ValidateCard(
            pb.ValidateCardRequest(instance_id=c2, card="4111 1111")),
            grpc.StatusCode.FAILED_PRECONDITION, "fulfillment after automatic break")

        # The explicit break zeroes the Err payloads, as it does in process,
        # and keeps the latches
        stub.Break(pb.BreakRequest(instance_id=c2))
        expect_code(lambda: stub.ValidateCard(
            pb.ValidateCardRequest(instance_id=c2, card="4111 1111")),
            grpc.StatusCode.FAILED_PRECONDITION, "fulfillment after an explicit break")
        p = stub.GetPartial(pb.PartialRequest(instance_id=c2))
        check(p.state == pb.BROKEN,
              "an explicitly broken instance still reads broken on its stream")
        check(list(p.broken) == ["Processing"] and
              list(p.pending) == ["Validation", "notify_user"],
              "the explicit break kept the partial names readable")
        check(list(p.errors) == [],
              "the explicit break zeroed the Err payloads, as it does in process")

    check(gone(stub, pb, c2) is not None,
          "closing a broken instance's stream dropped its row")
    expect_code(lambda: stub.GetPartial(pb.PartialRequest(instance_id=c2)),
                grpc.StatusCode.NOT_FOUND, "c2's id after its stream closed")
    expect_code(lambda: stub.GetPartial(pb.PartialRequest(instance_id=999999)),
                grpc.StatusCode.NOT_FOUND, "unknown instance id")
    expect_code(lambda: Session(stub, pb.SignRequest(expected_hash=12345)),
                grpc.StatusCode.ABORTED, "a sign under a wrong shape hash")


def dead_client(stub, pb, port, legacy_ttl):
    """A child opens a session and is killed without a word; the dead stream
    is a fact and the server has it in milliseconds."""
    child = subprocess.Popen(
        [sys.executable, str(Path(__file__).resolve()), "--hold-session",
         "--port", str(port)], stdout=subprocess.PIPE, text=True)
    try:
        line = child.stdout.readline().strip()
        check(line.isdigit(), f"the child opened a session, said {line!r}")
        doomed = int(line) if line.isdigit() else 0
        child.kill()
        t0 = time.monotonic()
        child.wait()
        settled = gone(stub, pb, doomed)
        elapsed = None if settled is None else time.monotonic() - t0
    finally:
        child.kill()
        child.wait()
    check(elapsed is not None, "a dead client's instance was collected")
    if elapsed is not None:
        print(f"[bridge_client] client died, instance {doomed} broken and "
              f"dropped in {elapsed * 1000:.0f}ms")
        check(elapsed < 1.0, f"the death was noticed in well under the old "
                             f"{legacy_ttl}s timer, took {elapsed:.3f}s")


def quiet_client(stub, pb, legacy_ttl):
    """A contract legitimately idle far past any timer a payment walk would
    want holds its stream, says nothing, and keeps its instance."""
    idle = legacy_ttl * 3
    with Session(stub, pb.SignRequest(currency="EUR")) as s3:
        c3 = s3.instance_id
        stub.ValidateCard(pb.ValidateCardRequest(instance_id=c3, card="4111 1111"))
        print(f"[bridge_client] session {c3} going quiet for {idle}s")
        time.sleep(idle)
        p = stub.GetPartial(pb.PartialRequest(instance_id=c3))
        check(p.state == pb.SIGNED and list(p.fulfilled) == [],
              "the quiet instance kept its state")
        check_ok(stub.ValidateAmount(pb.ValidateAmountRequest(instance_id=c3, amount=25.0)),
                 "the quiet instance resumed its walk")
        p = stub.GetPartial(pb.PartialRequest(instance_id=c3))
        check(p.state == pb.PARTIAL,
              "Validation lands after the silence, on latches set before it")
    check(gone(stub, pb, c3) is not None, "the quiet instance left when its stream did")


def walk(stub, pb, legacy_ttl, port):
    partial_path(stub, pb)
    err_path(stub, pb)
    dead_client(stub, pb, port, legacy_ttl)
    quiet_client(stub, pb, legacy_ttl)


def hold_session(port):
    """The doomed child: opens a session, says which instance it got, and
    holds the stream until something kills it."""
    pb, pb_grpc = _payment()
    with grpc.insecure_channel(f"127.0.0.1:{port}") as ch:
        stub = pb_grpc.PaymentServiceStub(ch)
        stream = stub.Session(pb.SignRequest(currency="EUR"))
        print(next(stream).signed.instance_id, flush=True)
        for _ in stream:
            pass
    return 0


def run_payment(args):
    pb, pb_grpc = _payment()
    if args.race_resume:
        import walk_race
        walk_race.walk(pb, pb_grpc, (args.port, args.peer_port),
                       args.rounds, TALLY, finish_parked)
        return
    ch = channel_ready(args.port, TALLY)
    if ch is None:
        return
    with ch:
        stub = pb_grpc.PaymentServiceStub(ch)
        if args.park_walk:
            park_walk(stub, pb)
        elif args.resume_walk:
            resume_walk(stub, pb, args.resume_walk)
        else:
            walk(stub, pb, args.legacy_ttl, args.port)


def run_other(args):
    """The saga, course, and language walks, each in its own file."""
    ch = channel_ready(args.port, TALLY)
    if ch is None:
        return
    with ch:
        if args.saga_walk:
            import walk_saga
            walk_saga.walk(ch, TALLY)
        elif args.course_walk:
            import walk_course
            walk_course.walk(ch, TALLY, args.park)
        else:
            import walk_lang
            walk_lang.walk(ch, TALLY)


def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=50251)
    ap.add_argument("--legacy-ttl", type=float, default=2.0,
                    help="the idle timer the first prototype ran, kept as "
                         "the yardstick the quiet check is measured against")
    ap.add_argument("--peer-port", type=int, default=50258,
                    help="the second server port for --race-resume")
    ap.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS,
                    help="with --race-resume, how many tokens to race")
    ap.add_argument("--park", action="store_true",
                    help="with --course-walk, also park and resume a course")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--park-walk", action="store_true",
                      help="park one partly progressed session and print its token")
    mode.add_argument("--resume-walk", metavar="TOKEN",
                      help="resume TOKEN and finish its parked walk")
    mode.add_argument("--race-resume", action="store_true",
                      help="park --rounds sessions on --peer-port and race "
                           "both servers to resume each")
    mode.add_argument("--saga-walk", action="store_true",
                      help="walk the host driven order saga and its signal")
    mode.add_argument("--course-walk", action="store_true",
                      help="walk the engine driven order saga through GetCourse")
    mode.add_argument("--lang-walk", action="store_true",
                      help="walk the language gauntlet's containers")
    mode.add_argument("--hold-session", action="store_true",
                      help="internal: open a session and hold it until killed")
    return ap.parse_args()


def main():
    args = parse_args()
    if args.hold_session:
        return hold_session(args.port)
    if args.saga_walk or args.course_walk or args.lang_walk:
        run_other(args)
    else:
        run_payment(args)
    if TALLY.failures:
        return 1
    if not args.park_walk:
        print("[bridge_client] ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
