"""The body behind the abstract Stall.hold: it polls the instance's cancel
flag and returns Ok(True) once a cancel request reaches it, the way a long
host call that honors cancellation behaves. A hold no request ever reaches
gives up after HOLD_LIMIT_S with Err(TIMED_OUT), so a walk that ends wrong
fails instead of hanging.

  bridge_server.py --module target/geas-out/libstall.geas.so \\
      --handlers interop/grpc/handlers/stall.py
"""

import logging
import time

from geas import Err, Ok

log = logging.getLogger("bridge_server")

HOLD_LIMIT_S = 30.0
POLL_S = 0.01
TIMED_OUT = 7


def hold(inst, args):
    deadline = time.monotonic() + HOLD_LIMIT_S
    while not inst.cancel_requested():
        if time.monotonic() > deadline:
            log.error("hold saw no cancel request in %.0fs", HOLD_LIMIT_S)
            return Err(TIMED_OUT)
        time.sleep(POLL_S)
    log.info("hold saw the cancel request and returns")
    return Ok(True)


HANDLERS = {"Stall.hold": hold}
