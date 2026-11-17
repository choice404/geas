"""Runtime status names and errors."""

from ._abi import (
    GEAS_ERR_DEADLOCK,
    GEAS_ERR_LOAD,
    GEAS_ERR_NAME,
    GEAS_ERR_NET,
    GEAS_ERR_OOM,
    GEAS_ERR_STATE,
    GEAS_ERR_STORE,
    GEAS_ERR_TYPE,
    GEAS_ERR_UNBOUND,
    GEAS_ERR_VERSION,
    GEAS_OK,
)


_STATUS_NAMES = {
    GEAS_OK: "GEAS_OK",
    GEAS_ERR_STATE: "GEAS_ERR_STATE",
    GEAS_ERR_TYPE: "GEAS_ERR_TYPE",
    GEAS_ERR_VERSION: "GEAS_ERR_VERSION",
    GEAS_ERR_UNBOUND: "GEAS_ERR_UNBOUND",
    GEAS_ERR_NAME: "GEAS_ERR_NAME",
    GEAS_ERR_DEADLOCK: "GEAS_ERR_DEADLOCK",
    GEAS_ERR_OOM: "GEAS_ERR_OOM",
    GEAS_ERR_LOAD: "GEAS_ERR_LOAD",
    GEAS_ERR_NET: "GEAS_ERR_NET",
    GEAS_ERR_STORE: "GEAS_ERR_STORE",
}


class GeasError(RuntimeError):
    """A non-OK status from the runtime, carrying the status word."""

    def __init__(self, status, what):
        name = _STATUS_NAMES.get(status, str(status))
        super().__init__(f"{what}: {name}")
        self.status = status
        self.status_name = name


def _check(status, what):
    if status != GEAS_OK:
        raise GeasError(status, what)
