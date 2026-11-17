"""walk_handoff.py: a parked session hands its obligation to the row and no
remedy runs on the way.

Handoff's course takes a hold and waits for go, and its Geas written remedy
for take writes one row to the store its dsn vow names each run, so a row
count is a remedy count. Each session gets its own store. A session broken
outright runs the remedy once, the yardstick. A session whose stream drops
while the course waits is parked and released, and its store holds no row.
Resumed, the same obligation is whole: a Break then runs the remedy exactly
once, on the replica that holds the contract for real.
"""

import sqlite3

from clientkit import Session, wait_gone, wait_until

WAIT_S = 10.0


def _stubs():
    import handoff_pb2 as pb
    import handoff_pb2_grpc as pb_grpc
    return pb, pb_grpc


def remedy_runs(path):
    """Rows in the Undo table at path, 0 when the remedy never made it."""
    with sqlite3.connect(path) as db:
        names = [r[0] for r in db.execute(
            "SELECT name FROM sqlite_master WHERE type = 'table'")]
        table = next((n for n in names if n.lower() == "undo"), None)
        if table is None:
            return 0
        return db.execute(f'SELECT count(*) FROM "{table}"').fetchone()[0]


def _await_go(stub, pb, tally, iid, how):
    course = wait_until(lambda: stub.GetCourse(pb.CourseRequest(instance_id=iid)),
                        lambda c: c.state == pb.COURSE_WAITING, WAIT_S)
    tally.check(course.state == pb.COURSE_WAITING, f"the {how} course waits for go")


def _break_and_count(stub, pb, tally, iid, path, how):
    stub.Break(pb.BreakRequest(instance_id=iid))
    p = stub.GetPartial(pb.PartialRequest(instance_id=iid))
    tally.check(p.state == pb.BROKEN, f"the {how} session reads broken")
    runs = remedy_runs(path)
    tally.check(runs == 1, f"the {how} break ran the Geas remedy once, ran {runs}")


def broken_outright(stub, pb, tally, store_dir):
    path = f"{store_dir}/undo_break.db"
    with Session(stub, pb.SignRequest(dsn=f"file:{path}")) as session:
        _await_go(stub, pb, tally, session.instance_id, "control")
        _break_and_count(stub, pb, tally, session.instance_id, path, "control")


def parked_then_resumed(stub, pb, tally, store_dir):
    path = f"{store_dir}/undo_park.db"
    with Session(stub, pb.SignRequest(dsn=f"file:{path}")) as session:
        iid, token = session.instance_id, session.signed.park_token
        _await_go(stub, pb, tally, iid, "parked")
    tally.check(wait_gone(stub, pb.PartialRequest, iid, WAIT_S) is not None,
                "the dropped stream parked its session")
    runs = remedy_runs(path)
    tally.check(runs == 0, f"parking ran no Geas remedy, ran {runs}")
    with Session(stub, pb.ResumeRequest(park_token=token), stub.Resume) as session:
        _await_go(stub, pb, tally, session.instance_id, "resumed")
        _break_and_count(stub, pb, tally, session.instance_id, path, "resumed")


def walk(channel, tally, store_dir):
    pb, pb_grpc = _stubs()
    stub = pb_grpc.HandoffStub(channel)
    broken_outright(stub, pb, tally, store_dir)
    parked_then_resumed(stub, pb, tally, store_dir)
    print("[edge_client] a parked session ran no remedy, its resumed break ran one")
