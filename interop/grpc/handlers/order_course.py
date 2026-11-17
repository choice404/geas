"""The host's bodies for skeleton/order_course.geas, the engine driven order
saga. The course calls every step itself, so these are all a deployment
supplies: the gateway's authorize and capture, the warehouse's reserve, the
carrier's ship and chase, and the refund remedy. plan and release are
written in Geas.

  bridge_server.py --module target/geas-out/liborder_course.geas.so \\
      --handlers interop/grpc/handlers/order_course.py
"""

import logging

from geas import Ok, Record

log = logging.getLogger("bridge_server")


def authorize(inst, args):
    card = args[0]
    return Ok(Record(f"AUTH-{card[0]}"))


def capture(inst, args):
    auth = args[0]
    return Ok(Record(f"RC-{auth[0]}", inst.vow("amount")))


def reserve(inst, args):
    sku, hold = args
    return Ok(Record(f"R-{sku}", sku, hold))


def ship(inst, args):
    res_id = args[0][0]
    return Ok(Record("ACME", f"T-{res_id}"))


def chase(inst, args):
    tracking, waited = args
    log.info("chasing %s after %dms", tracking[1], waited)
    return Ok(None)


def refund(inst, args):
    receipt = args[0]
    log.info("refunding %s, %d", receipt[0], receipt[1])
    return Ok(None)


HANDLERS = {
    "OrderCourse.authorize": authorize,
    "OrderCourse.capture": capture,
    "OrderCourse.reserve": reserve,
    "OrderCourse.ship": ship,
    "OrderCourse.chase": chase,
    "OrderCourse.refund": refund,
}
