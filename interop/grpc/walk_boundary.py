"""walk_boundary.py: the checked boundary across the bridge.

Every value a proto client can send that is not the declared type is
answered INVALID_ARGUMENT, with no body run and nothing latched, and the
conforming request beside each one lands. Bytes that do not parse as the
request, a string that is not UTF-8 or a message nested past the parser's
depth, are refused before any conversion. A Char map key in the surrogate
block, a Char past U+10FFFF, and a map key sent twice are refused by the
conversion. A body the deployer supplied that answers outside its declared
return, or raises, is the server's fault, not the client's, and crosses as
INTERNAL, nothing latched, and the same pledge with a conforming answer lands
after.
"""

import grpc

from clientkit import Session

PACKAGE = "geas.boundary"

# The deepest census root the protobuf parser takes, in Categories, the
# request message around it aside
PARSER_DEPTH = 100


def _stubs():
    import boundary_pb2 as pb
    import boundary_pb2_grpc as pb_grpc
    return pb, pb_grpc


def _varint(n):
    out = bytearray()
    while True:
        low = n & 0x7F
        n >>= 7
        if n:
            out.append(low | 0x80)
        else:
            out.append(low)
            return bytes(out)


def _field(number, payload):
    """One length delimited field, written by hand so a request the client
    library would refuse to build still reaches the server"""
    return _varint(number << 3 | 2) + _varint(len(payload)) + payload


def _census_bytes(iid, depth, title=b"t"):
    """A census request whose root Category nests depth deep"""
    node = _field(1, title)
    for _ in range(depth - 1):
        node = _field(1, b"t") + _field(2, node)
    return _varint(1 << 3) + _varint(iid) + _field(2, node)


def _raw_census(channel, pb):
    return channel.unary_unary(f"/{PACKAGE}.Boundary/Census",
                               request_serializer=None,
                               response_deserializer=pb.IntStringResult.FromString)


def _unparsed(channel, pb, tally, iid):
    census = _raw_census(channel, pb)
    out = census(_census_bytes(iid, 3))
    tally.check(out.WhichOneof("result") == "ok" and out.ok == 3,
                "a census three deep written by hand lands")
    tally.expect_code(lambda: census(_census_bytes(iid, 1, b"\xed\xa0\x80")),
                      grpc.StatusCode.INVALID_ARGUMENT, "a title that is not UTF-8")
    tally.expect_code(lambda: census(_census_bytes(iid, PARSER_DEPTH + 1)),
                      grpc.StatusCode.INVALID_ARGUMENT,
                      "a census nested past the parser's depth")
    out = census(_census_bytes(iid, PARSER_DEPTH))
    tally.check(out.WhichOneof("result") == "ok" and out.ok == PARSER_DEPTH,
                "a census at the parser's depth lands")


def _marks(pb, keys):
    marks = pb.CharIntMap()
    for key in keys:
        marks.entries.add(key=key, value=1)
    return marks


def _converted(stub, pb, tally, iid):
    def stamp(keys):
        return stub.Stamp(pb.BoundaryStampRequest(
            instance_id=iid, data=b"\x01", wait=1000, flag=True, marks=_marks(pb, keys)))
    tally.expect_code(lambda: stamp([0xD800]), grpc.StatusCode.INVALID_ARGUMENT,
                      "a Char map key in the surrogate block")
    tally.expect_code(lambda: stamp([0x110000]), grpc.StatusCode.INVALID_ARGUMENT,
                      "a Char map key past U+10FFFF")
    tally.expect_code(lambda: stamp([ord("a"), ord("a")]), grpc.StatusCode.INVALID_ARGUMENT,
                      "a Char map key sent twice")
    scores = pb.StringIntMap()
    scores.entries.add(key="a", value=1)
    scores.entries.add(key="a", value=2)
    tally.expect_code(lambda: stub.Tally(pb.BoundaryTallyRequest(
        instance_id=iid, scores=scores, pair=pb.IntStringTuple(item1=1, item2="one"))),
        grpc.StatusCode.INVALID_ARGUMENT, "a String map key sent twice")
    out = stamp([ord("a"), 0x1F30D])
    tally.check(out.WhichOneof("result") == "ok" and out.ok == 3,
                "a map of two scalar value keys lands")


def _answers(stub, pb, tally, iid):
    customer = pb.Customer(name="short", address=pb.Address(street="elm", zip=1))
    tally.expect_code(lambda: stub.Lookup(pb.BoundaryLookupRequest(
        instance_id=iid, customer=customer, wait=10)),
        grpc.StatusCode.INTERNAL, "a supplied body's Address a field short")
    tally.expect_code(lambda: stub.Find(pb.BoundaryFindRequest(instance_id=iid, name="wrong")),
                      grpc.StatusCode.INTERNAL, "a supplied body's Some(5) for an Option<Address>")
    tally.expect_code(lambda: stub.Find(pb.BoundaryFindRequest(instance_id=iid, name="raise")),
                      grpc.StatusCode.INTERNAL, "a supplied body that raises")
    partial = stub.GetPartial(pb.PartialRequest(instance_id=iid))
    tally.check("lookup" not in partial.fulfilled and "find" not in partial.fulfilled,
                "neither malformed answer latched")
    customer.name = "ada"
    out = stub.Lookup(pb.BoundaryLookupRequest(instance_id=iid, customer=customer, wait=10))
    tally.check(out.WhichOneof("result") == "ok" and out.ok.street == "elm" and
                out.ok.zip == 94110, "a conforming answer lands after")
    out = stub.Find(pb.BoundaryFindRequest(instance_id=iid, name="dock"))
    tally.check(out.HasField("value") and out.value.street == "dock",
                "and Find answers Some(Address)")


def walk(channel, tally):
    pb, pb_grpc = _stubs()
    stub = pb_grpc.BoundaryStub(channel)
    with Session(stub, pb.BoundarySignRequest(seed=b"\x00")) as session:
        _unparsed(channel, pb, tally, session.instance_id)
        _converted(stub, pb, tally, session.instance_id)
        _answers(stub, pb, tally, session.instance_id)
    print("[edge_client] every malformed request refused, every malformed answer INTERNAL")
