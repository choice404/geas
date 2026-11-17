"""status.py: the line between business and transport, held across the wire.

A contract's own Err is a value on an OK rpc and never reaches this file. A
Geas status is a fulfillment, a delivery, or a sign that did not run, and
crosses as a gRPC code by the table in docs/bridge.md. A status the table
does not name is INTERNAL: the bridge does not invent a meaning it was not
given. Messages carry the status name and the call, never a path or a
trace, since the peer is outside the process.
"""

import logging

import grpc

from geas import (GEAS_ERR_NAME, GEAS_ERR_STATE, GEAS_ERR_TYPE,
                  GEAS_ERR_UNBOUND, GEAS_ERR_VERSION)

log = logging.getLogger("bridge_server")

STATUS_TO_GRPC = {
    GEAS_ERR_STATE: grpc.StatusCode.FAILED_PRECONDITION,
    GEAS_ERR_NAME: grpc.StatusCode.NOT_FOUND,
    GEAS_ERR_TYPE: grpc.StatusCode.INVALID_ARGUMENT,
    GEAS_ERR_VERSION: grpc.StatusCode.ABORTED,
    GEAS_ERR_UNBOUND: grpc.StatusCode.FAILED_PRECONDITION,
}


def abort_status(context, err, what):
    """Ends the rpc on a GeasError's status. Raises, like context.abort."""
    code = STATUS_TO_GRPC.get(err.status, grpc.StatusCode.INTERNAL)
    if code == grpc.StatusCode.INTERNAL:
        log.warning("%s answered unmapped status %s", what, err.status_name)
    context.abort(code, f"{what}: {err.status_name}")


def abort_unknown(context, instance_id):
    context.abort(grpc.StatusCode.NOT_FOUND, f"unknown instance {instance_id}")


def abort_argument(context, what, err):
    context.abort(grpc.StatusCode.INVALID_ARGUMENT, f"{what}: {err}")


def abort_reply(context, what, err):
    """A body answered a value its declared reply cannot carry. The detail
    stays in the server's log."""
    log.error("%s answered a value outside its reply shape: %s", what, err)
    context.abort(grpc.StatusCode.INTERNAL,
                  f"{what} answered a value outside its declared shape")
