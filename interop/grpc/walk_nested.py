"""walk_nested.py: an Option of an Option across the bridge, both ways.

A declared parameter of Option<Option<Int>> is the IntOption message, its
presence the outer Some, and the reply of the same type is IntOptionOption.
None, Some(None), and Some(Some(5)) each cross in, are matched by a Geas
body that tells all three apart, and come back out as sent; the same holds
for an Option of an Option of a record inside a Result.
"""

from clientkit import Session


def _stubs():
    import nested_option_pb2 as pb
    import nested_option_pb2_grpc as pb_grpc
    return pb, pb_grpc


def _depths(stub, pb, tally, iid):
    cases = [("None", None, 0), ("Some(None)", pb.IntOption(), 1),
             ("Some(Some(5))", pb.IntOption(value=5), 7)]
    for name, x, want in cases:
        request = pb.DepthRequest(instance_id=iid)
        if x is not None:
            request.x.CopyFrom(x)
        out = stub.Depth(request)
        tally.check(out.WhichOneof("result") == "ok" and out.ok == want,
                    f"{name} crosses in as itself, depth {want}")


def _echo_ints(stub, pb, tally, iid):
    none = stub.Maybe(pb.MaybeRequest(instance_id=iid))
    tally.check(not none.HasField("value"), "None comes back unset")
    request = pb.MaybeRequest(instance_id=iid)
    request.x.SetInParent()
    some_none = stub.Maybe(request)
    tally.check(some_none.HasField("value") and not some_none.value.HasField("value"),
                "Some(None) comes back as a present empty IntOption")
    some = stub.Maybe(pb.MaybeRequest(instance_id=iid, x=pb.IntOption(value=5)))
    tally.check(some.HasField("value") and some.value.HasField("value") and
                some.value.value == 5, "Some(Some(5)) comes back whole")


def _echo_notes(stub, pb, tally, iid):
    request = pb.NoteRequest(instance_id=iid)
    request.n.SetInParent()
    out = stub.Note(request)
    tally.check(out.WhichOneof("result") == "ok" and out.ok.HasField("value") and
                not out.ok.value.HasField("value"),
                "Some(None) of a record crosses inside a Result")
    note = pb.NoteOption(value=pb.Note(text="hi"))
    out = stub.Note(pb.NoteRequest(instance_id=iid, n=note))
    tally.check(out.ok.value.value.text == "hi", "Some(Some(note)) crosses whole")
    out = stub.Note(pb.NoteRequest(instance_id=iid))
    tally.check(out.WhichOneof("result") == "ok" and not out.ok.HasField("value"),
                "None of a record option crosses as unset")


def walk(channel, tally):
    pb, pb_grpc = _stubs()
    stub = pb_grpc.NestedOptionStub(channel)
    with Session(stub, pb.SignRequest()) as session:
        _depths(stub, pb, tally, session.instance_id)
        _echo_ints(stub, pb, tally, session.instance_id)
        _echo_notes(stub, pb, tally, session.instance_id)
    print("[edge_client] None, Some(None), and Some(Some(x)) crossed both ways")
