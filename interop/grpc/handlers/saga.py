"""The host's bodies for skeleton/saga.geas, the host driven order saga:
a gateway that authorizes and captures, a carrier that ships, and the refund
remedy that undoes a capture. reserve and release are written in Geas and
need nothing here.

The carrier refuses any reservation whose sku is "refuse", which is how a
client walks the break path over the wire: ship answers Err, the break line
fires, and the remedies undo what landed, newest first.

  bridge_server.py --module target/geas-out/libsaga.geas.so \\
      --handlers interop/grpc/handlers/saga.py
"""

import logging

from geas import Err, Ok, Record, Variant

log = logging.getLogger("bridge_server")

# The arm of ShipError a refusal answers, its declaration index
SHIP_REFUSED = 1
REFUSED_SKU = "refuse"


def authorize(inst, args):
    card = args[0]
    return Ok(Record(f"AUTH-{card[0]}"))


def capture(inst, args):
    auth = args[0]
    return Ok(Record(f"RC-{auth[0]}", inst.vow("amount")))


def ship(inst, args):
    res_id, sku, hold = args[0]
    if sku == REFUSED_SKU:
        return Err(Variant(SHIP_REFUSED, "no carrier takes it"))
    return Ok(Record("ACME", f"T-{res_id}"))


def refund(inst, args):
    receipt = args[0]
    log.info("refunding %s, %d", receipt[0], receipt[1])
    return Ok(None)


HANDLERS = {
    "OrderSaga.authorize": authorize,
    "OrderSaga.capture": capture,
    "OrderSaga.ship": ship,
    "OrderSaga.refund": refund,
}
