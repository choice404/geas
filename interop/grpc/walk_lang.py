"""walk_lang.py: the language gauntlet's containers through the generic
server. skeleton/lang.geas has no abstract body and no host code of its own,
so every answer here crossed by reflection alone: a repeated field in as a
List with its element tag, an empty one included, a map wrapper in as the
runtime's own Map in entry order, and a Map, a Tuple, and an Option out
through their emitted wrappers.
"""

from clientkit import Session


def _stubs():
    import lang_pb2 as pb
    import lang_pb2_grpc as pb_grpc
    return pb, pb_grpc


def _entries(m):
    return [(e.key, e.value) for e in m.entries]


def _lists_and_options(stub, pb, tally, iid):
    out = stub.SumList(pb.SumListRequest(instance_id=iid, xs=[1, 2, 3]))
    tally.check(out.WhichOneof("result") == "ok" and out.ok == 6,
                "a repeated field crosses in as a List")
    out = stub.SumList(pb.SumListRequest(instance_id=iid, xs=[]))
    tally.check(out.WhichOneof("result") == "ok" and out.ok == 0,
                "an empty list crosses with its element tag")
    out = stub.HeadDoubled(pb.HeadDoubledRequest(instance_id=iid, xs=[4, 5]))
    tally.check(out.HasField("value") and out.value == 8, "a Some crosses out")
    out = stub.HeadDoubled(pb.HeadDoubledRequest(instance_id=iid, xs=[]))
    tally.check(not out.HasField("value"), "a None crosses out unset")


def _maps_and_tuples(stub, pb, tally, iid):
    out = stub.Tally(pb.TallyRequest(instance_id=iid, names=["a", "b", "a"]))
    tally.check(out.WhichOneof("result") == "ok" and
                _entries(out.ok) == [("a", 2), ("b", 1)],
                "a Map crosses out in insertion order")
    out = stub.Snapshot(pb.SnapshotRequest(instance_id=iid))
    tally.check(_entries(out.ok) == [("ada", 3), ("bob", 2)],
                "a key written twice keeps its first place")
    a = pb.StringIntMap(entries=[pb.StringIntMapEntry(key="x", value=1),
                                 pb.StringIntMapEntry(key="y", value=2)])
    b = pb.StringIntMap(entries=[pb.StringIntMapEntry(key="z", value=3)])
    out = stub.MergeCounts(pb.MergeCountsRequest(instance_id=iid, a=a, b=b))
    tally.check(out.WhichOneof("result") == "ok" and out.ok == 6,
                "two maps cross in as the runtime's Map")
    out = stub.Coords(pb.CoordsRequest(instance_id=iid))
    tally.check(out.WhichOneof("result") == "ok" and
                (out.ok.item1, out.ok.item2) == (1, "one"), "a Tuple crosses out")


def walk(channel, tally):
    pb, pb_grpc = _stubs()
    stub = pb_grpc.LangGauntletStub(channel)
    with Session(stub, pb.SignRequest(greeting="hi")) as session:
        iid = session.instance_id
        tally.check(session.signed.greeting == "hi", "the gauntlet's vow landed")
        _lists_and_options(stub, pb, tally, iid)
        _maps_and_tuples(stub, pb, tally, iid)
        print(f"[bridge_client] gauntlet {iid}, containers crossed both ways")
