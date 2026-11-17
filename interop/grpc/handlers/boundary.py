"""The bodies behind Boundary's abstract pledges and its remedy, and
BoundaryCourse's fetch. Each answers a conforming value, except where the
request asks for one that is not: a customer named "short" gets an Address a
field short from lookup, a name of "wrong" gets Some(5) from find where an
Option<Address> belongs, and a name of "raise" makes find raise. The runtime
holds every answer to the declared return, so none of those reach a latch,
and the bridge answers the rpc INTERNAL, the server's own fault and not the
client's.

  bridge_server.py --module target/geas-out/libboundary.geas.so \\
      --handlers interop/grpc/handlers/boundary.py
"""

from geas import GEAS_TY_UINT, NONE, Ok, Record, Some, Typed

ZIP = 94110


def _address(street):
    return Record(street, Typed(GEAS_TY_UINT, ZIP))


def lookup(inst, args):
    customer = args[0]
    if customer[0] == "short":
        return Ok(Record("elm"))
    return Ok(_address(customer[1][0] or "elm"))


def find(inst, args):
    name = args[0]
    if name == "wrong":
        return Some(5)
    if name == "raise":
        raise RuntimeError("find was asked to raise")
    if name == "none":
        return NONE
    return Some(_address(name))


def unlookup(inst, args):
    return Ok(None)


def fetch(inst, args):
    return Ok(_address("dock"))


HANDLERS = {
    "Boundary.lookup": lookup,
    "Boundary.find": find,
    "Boundary.unlookup": unlookup,
    "BoundaryCourse.fetch": fetch,
}
