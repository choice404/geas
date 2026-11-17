"""walk_saga.py: the host driven order saga over the bridge, the walk
demo_saga.py takes in process, driven through the typed rpcs of the
saga.proto geas emitted and the generic server serving it.

The happy path signs on the declared vows, sees capture refused before
authorize as FAILED_PRECONDITION, authorizes and captures through the
server's handler bodies, reserves through the Geas body, sees the delivered
signal refused before ship, ships, delivers the signal, and reads the saga
Fulfilled. A repeat of a delivery that landed changes nothing. The break
path ships a reservation the carrier refuses: the Err arrives as a value on
an OK rpc, the break line fires, the remedies run, and the saga reads Broken
with ship's error arm on the partial surface, where a later delivery is
refused.
"""

import grpc

from clientkit import Session, wait_until

# The vows' declared defaults: amount 100, grace 30 minutes in milliseconds
AMOUNT = 100
GRACE_MS = 30 * 60 * 1000
HOUR_MS = 60 * 60 * 1000

# ShipError's Refused arm, its declaration index
SHIP_REFUSED = 1


def _stubs():
    import saga_pb2 as pb
    import saga_pb2_grpc as pb_grpc
    return pb, pb_grpc


def _partial(stub, pb, iid):
    return stub.GetPartial(pb.PartialRequest(instance_id=iid))


def _land_payment(stub, pb, tally, iid):
    """authorize then capture, the Payment subcontract, through the server's
    handler bodies. Answers the receipt."""
    tally.expect_code(lambda: stub.Capture(pb.CaptureRequest(
        instance_id=iid, auth=pb.AuthId(token="early"))),
        grpc.StatusCode.FAILED_PRECONDITION, "capture before authorize")
    auth = stub.Authorize(pb.AuthorizeRequest(instance_id=iid, card=pb.Card(number="4111")))
    tally.check(auth.WhichOneof("result") == "ok" and auth.ok.token == "AUTH-4111",
                "authorize answers the gateway's AuthId record")
    receipt = stub.Capture(pb.CaptureRequest(instance_id=iid, auth=auth.ok))
    tally.check(receipt.WhichOneof("result") == "ok" and
                receipt.ok.id == "RC-AUTH-4111" and receipt.ok.amount == AMOUNT,
                "capture answers a receipt carrying the amount vow")
    return receipt.ok


def _reserve(stub, pb, tally, iid, sku):
    res = stub.Reserve(pb.ReserveRequest(instance_id=iid, sku=sku, hold=HOUR_MS))
    tally.check(res.WhichOneof("result") == "ok" and res.ok.id == f"R-{sku}" and
                res.ok.hold == HOUR_MS + GRACE_MS,
                "reserve's Geas body adds the grace vow to the hold")
    return res.ok


def happy_path(stub, pb, tally):
    with Session(stub, pb.SignRequest()) as session:
        iid = session.instance_id
        tally.check(session.signed.amount == AMOUNT and session.signed.grace == GRACE_MS,
                    "the signature carries the declared vows, the Duration in ms")
        _land_payment(stub, pb, tally, iid)
        tracking = pb.Tracking(carrier="ACME", code="T-early")
        tally.expect_code(lambda: stub.SignalDelivered(pb.SignalDeliveredRequest(
            instance_id=iid, tracking=tracking)),
            grpc.StatusCode.FAILED_PRECONDITION, "delivered before ship")
        res = _reserve(stub, pb, tally, iid, "sku-1")
        shipped = stub.Ship(pb.ShipRequest(instance_id=iid, res=res))
        tally.check(shipped.WhichOneof("result") == "ok" and
                    shipped.ok.code == "T-R-sku-1", "ship answers the carrier's tracking")
        p = _partial(stub, pb, iid)
        tally.check(p.state == pb.PARTIAL and list(p.pending) == ["delivered"],
                    "every step landed, the saga waits on its signal")
        stub.SignalDelivered(pb.SignalDeliveredRequest(instance_id=iid, tracking=shipped.ok))
        stub.SignalDelivered(pb.SignalDeliveredRequest(instance_id=iid, tracking=shipped.ok))
        p = _partial(stub, pb, iid)
        tally.check(p.state == pb.FULFILLED and list(p.pending) == [],
                    "the delivered signal fulfills the saga, a repeat changes nothing")
        print(f"[bridge_client] saga {iid} fulfilled on its delivered signal")


def break_path(stub, pb, tally):
    with Session(stub, pb.SignRequest(amount=250)) as session:
        iid = session.instance_id
        tally.check(session.signed.amount == 250, "the amount vow override landed")
        _land_payment_amount(stub, pb, tally, iid)
        res = _reserve(stub, pb, tally, iid, "refuse")
        shipped = stub.Ship(pb.ShipRequest(instance_id=iid, res=res))
        tally.check(shipped.WhichOneof("result") == "err" and
                    shipped.err.WhichOneof("arm") == "refused" and
                    shipped.err.refused.reason == "no carrier takes it",
                    "ship's Err arrives as a value carrying its sum arm")
        p = wait_until(lambda: _partial(stub, pb, iid),
                       lambda r: r.state == pb.BROKEN, 5.0)
        tally.check(p.state == pb.BROKEN, "the break line fired and the remedies ran")
        tally.check("ship" in list(p.broken) and
                    [(e.pledge, e.err) for e in p.errors] == [("ship", SHIP_REFUSED)],
                    "the partial surface names ship's error arm")
        tally.expect_code(lambda: stub.SignalDelivered(pb.SignalDeliveredRequest(
            instance_id=iid, tracking=pb.Tracking(carrier="ACME", code="T-1"))),
            grpc.StatusCode.FAILED_PRECONDITION, "a delivery into a broken saga")
        print(f"[bridge_client] saga {iid} broke on ship and read BROKEN")


def _land_payment_amount(stub, pb, tally, iid):
    auth = stub.Authorize(pb.AuthorizeRequest(instance_id=iid, card=pb.Card(number="4111")))
    receipt = stub.Capture(pb.CaptureRequest(instance_id=iid, auth=auth.ok))
    tally.check(receipt.ok.amount == 250, "capture reads the overridden amount")


def walk(channel, tally):
    pb, pb_grpc = _stubs()
    stub = pb_grpc.OrderSagaStub(channel)
    happy_path(stub, pb, tally)
    break_path(stub, pb, tally)
    tally.expect_code(lambda: stub.SignalDelivered(pb.SignalDeliveredRequest(
        instance_id=999999)), grpc.StatusCode.NOT_FOUND, "a signal to an unknown id")
