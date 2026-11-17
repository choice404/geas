"""walk_course.py: the engine driven order saga over the bridge. The client
never calls a step, and the emitted surface carries no pledge rpc to call:
opening the Session signs, the course runs on the server's runtime pool
through the handler bodies the server was started with, and the client only
watches it through GetCourse, hands in the delivered signal, and reads the
partial surface.

The walk signs, waits for the course to rest at its select with a wait due,
delivers the signal, and reads the course FINISHED and the saga FULFILLED.
An order of nothing finishes its course at once with nothing latched. With
--park, a second saga waits at its select, its stream closes and the server
parks it, and Resume stands it back up waiting at the same step, where the
signal finishes it.
"""

import grpc

from clientkit import Session, wait_gone, wait_until

WAIT_S = 10.0


def _stubs():
    import order_course_pb2 as pb
    import order_course_pb2_grpc as pb_grpc
    return pb, pb_grpc


def _course(stub, pb, iid):
    return stub.GetCourse(pb.CourseRequest(instance_id=iid))


def _until_course(stub, pb, iid, state):
    return wait_until(lambda: _course(stub, pb, iid), lambda c: c.state == state, WAIT_S)


def _await_select(stub, pb, tally, iid, how):
    course = _until_course(stub, pb, iid, pb.COURSE_WAITING)
    tally.check(course.state == pb.COURSE_WAITING and course.wait_due_ms > 0,
                f"the {how} course rests at its select with a wait due")
    p = stub.GetPartial(pb.PartialRequest(instance_id=iid))
    tally.check(p.state == pb.PARTIAL and "delivered" in list(p.pending) and
                "Payment" in list(p.fulfilled),
                f"the {how} course landed Payment and waits on delivered")
    return course


def _deliver_and_finish(stub, pb, tally, iid, how):
    tracking = pb.Tracking(carrier="ACME", code="T-R-sku-1")
    stub.SignalDelivered(pb.SignalDeliveredRequest(instance_id=iid, tracking=tracking))
    course = _until_course(stub, pb, iid, pb.COURSE_FINISHED)
    tally.check(course.state == pb.COURSE_FINISHED and course.status == 0,
                f"the {how} course ran to its end")
    p = wait_until(lambda: stub.GetPartial(pb.PartialRequest(instance_id=iid)),
                   lambda r: r.state == pb.FULFILLED, WAIT_S)
    tally.check(p.state == pb.FULFILLED and list(p.pending) == ["chase"],
                f"the {how} saga fulfilled, chase never needed")


def happy_path(stub, pb, tally):
    with Session(stub, pb.SignRequest()) as session:
        iid = session.instance_id
        tally.check(session.signed.card == "4111" and session.signed.hold == 3600000,
                    "the course signature carries the declared vows")
        _await_select(stub, pb, tally, iid, "signed")
        _deliver_and_finish(stub, pb, tally, iid, "signed")
        print(f"[bridge_client] course {iid} finished, saga FULFILLED")


def empty_order(stub, pb, tally):
    with Session(stub, pb.SignRequest(amount=0)) as session:
        iid = session.instance_id
        course = _until_course(stub, pb, iid, pb.COURSE_FINISHED)
        tally.check(course.state == pb.COURSE_FINISHED,
                    "an order of nothing finishes its course at once")
        p = stub.GetPartial(pb.PartialRequest(instance_id=iid))
        tally.check(p.state == pb.SIGNED and list(p.fulfilled) == [],
                    "and latches nothing")


def park_path(stub, pb, tally):
    with Session(stub, pb.SignRequest()) as session:
        iid = session.instance_id
        token = session.signed.park_token
        tally.check(len(token) == 32, "a park enabled server issued a token")
        _await_select(stub, pb, tally, iid, "parked")
    tally.check(wait_gone(stub, pb.PartialRequest, iid, WAIT_S) is not None,
                "closing the course's stream parked and dropped it")
    with Session(stub, pb.ResumeRequest(park_token=token), stub.Resume) as session:
        iid = session.instance_id
        tally.expect_code(lambda: Session(stub, pb.ResumeRequest(park_token=token),
                                          stub.Resume),
                          grpc.StatusCode.NOT_FOUND, "a course token already claimed")
        _await_select(stub, pb, tally, iid, "resumed")
        _deliver_and_finish(stub, pb, tally, iid, "resumed")
        print(f"[bridge_client] course {iid} resumed at its select and finished")


def walk(channel, tally, is_park):
    pb, pb_grpc = _stubs()
    stub = pb_grpc.OrderCourseStub(channel)
    happy_path(stub, pb, tally)
    empty_order(stub, pb, tally)
    if is_park:
        park_path(stub, pb, tally)
    tally.expect_code(lambda: _course(stub, pb, 999999),
                      grpc.StatusCode.NOT_FOUND, "GetCourse on an unknown id")
