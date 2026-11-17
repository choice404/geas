"""The body behind the abstract PaymentService.charge, the one
demo_payment.py binds in process: a positive amount charges and answers
Ok(True), anything else is Err(41). It reads the currency vow off the signed
instance, the part a stateless service could not do without the session.

  bridge_server.py --module target/geas-out/libpayment.geas.so \\
      --handlers interop/grpc/handlers/payment.py
"""

import logging

from geas import Err, Ok

log = logging.getLogger("bridge_server")

DECLINED = 41


def charge(inst, args):
    card, amount = args
    if amount <= 0:
        return Err(DECLINED)
    log.info("charging %s %.2f to %s", inst.vow("currency"), amount, card)
    return Ok(True)


HANDLERS = {"PaymentService.charge": charge}
