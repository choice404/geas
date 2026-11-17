"""Event snapshots with borrowed contract handles."""

from .contract import Contract


class Event:
    """One transition, copied into Python with its borrowed Contract handle."""

    def __init__(self, runtime, event):
        self.kind = event.kind
        self.instance = Contract(runtime, event.instance)
        self.contract = event.contract.decode()
        self.item = event.item.decode() if event.item else None
        self.old_state = event.old_state
        self.new_state = event.new_state
        self.at_ms = event.at_ms
        self.seq = event.seq
