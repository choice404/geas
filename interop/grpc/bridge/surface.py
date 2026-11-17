"""surface.py: one emitted service read against the module that serves it.

Every rpc of a service gets a role from its spelling and its messages, the
way docs/bridge.md lays the surface out: Session and Resume are the streams,
GetPartial, Break, and GetCourse are the shared reads and the ending, a
Signal rpc answering SignalReply delivers a signal, and every other rpc is a
pledge. A pledge rpc is matched against the runtime's iname table, the
pledge names the loaded module registered, through the same CamelCase rule
the emitter spells them with, so a surface and a module that disagree on a
pledge fail at startup instead of at the first call.
"""

import re
from dataclasses import dataclass

from .protoload import message_class

SESSION, RESUME, PLEDGE, SIGNAL, PARTIAL, BREAK, COURSE = (
    "session", "resume", "pledge", "signal", "partial", "break", "course")

FIXED_ROLES = {"Session": SESSION, "Resume": RESUME, "GetPartial": PARTIAL,
               "Break": BREAK, "GetCourse": COURSE}

# The emitted comment above a signal's request message names the signal in
# its declared spelling
SIGNAL_COMMENT = re.compile(r"^signal (\w+), ")


class SurfaceError(RuntimeError):
    """An emitted surface the loaded modules cannot serve."""


@dataclass(frozen=True)
class Method:
    """One rpc: its role, the pledge or signal it reaches, and its classes."""
    name: str
    role: str
    target: str
    request: type
    reply: type
    is_stream: bool


@dataclass(frozen=True)
class ContractSurface:
    """One contract's service as the server serves it."""
    contract: str
    service: str
    shape_hash: int
    methods: tuple
    vow_fields: tuple
    event: type
    signed_field: object
    state_enum: object
    has_course: bool


def camel_case(name):
    """The emitter's rpc spelling of a declared name: each word after an
    underscore, and the first, starts upper case."""
    return "".join(word[:1].upper() + word[1:] for word in name.split("_") if word)


def snake_case(camel):
    out = []
    for i, ch in enumerate(camel):
        if ch.isupper() and i > 0:
            out.append("_")
        out.append(ch.lower())
    return "".join(out)


def _contract_rows(runtime, contract):
    """The contract's iname row and its pledge names, from the runtime."""
    rows = [runtime.iname_at(i) for i in range(runtime.iname_count())]
    head = [r for r in rows if r["kind"] == "contract" and r["contract"] == contract]
    if not head:
        raise SurfaceError(f"service {contract} names no contract any loaded "
                           "module registered")
    pledges = {r["symbol"]: r["nargs"] for r in rows
               if r["kind"] == "pledge" and r["contract"] == contract}
    return head[0], pledges


def _signal_name(loaded, method):
    comment = loaded.comments.get(method.input_type.name, "")
    found = SIGNAL_COMMENT.match(comment)
    if found:
        return found.group(1)
    return snake_case(method.name[len("Signal"):])


def _pledge_name(method, pledges, contract):
    """The declared pledge an rpc reaches, its parameter count checked."""
    for name, nargs in pledges.items():
        if camel_case(name) != method.name:
            continue
        params = len(method.input_type.fields) - 1
        if params != nargs:
            raise SurfaceError(f"{contract}.{name} takes {nargs} arguments, "
                               f"its rpc carries {params}")
        return name
    raise SurfaceError(f"rpc {method.name} names no pledge of {contract} in "
                       "the loaded module")


def _method(loaded, method, pledges, contract):
    role = FIXED_ROLES.get(method.name)
    target = ""
    is_signal = (method.name.startswith("Signal") and
                 method.output_type.name == "SignalReply")
    if role is None and is_signal:
        role, target = SIGNAL, _signal_name(loaded, method)
    elif role is None:
        role, target = PLEDGE, _pledge_name(method, pledges, contract)
    return Method(method.name, role, target, message_class(method.input_type),
                  message_class(method.output_type), method.server_streaming)


def build_surface(loaded, service, runtime):
    """Reads one service of one loaded file into a ContractSurface."""
    contract = service.name
    head, pledges = _contract_rows(runtime, contract)
    methods = tuple(_method(loaded, m, pledges, contract) for m in service.methods)
    by_name = {m.name: m for m in service.methods}
    session = by_name.get("Session")
    if session is None or not session.server_streaming:
        raise SurfaceError(f"service {contract} carries no Session stream")
    sign = session.input_type
    vows = tuple(f for f in sign.fields if f.name != "expected_hash")
    state_enum = loaded.file.enum_types_by_name.get("ContractState")
    if state_enum is None:
        raise SurfaceError(f"{loaded.path.name} declares no ContractState")
    return ContractSurface(
        contract=contract, service=service.full_name,
        shape_hash=head["shape_hash"], methods=methods, vow_fields=vows,
        event=message_class(session.output_type),
        signed_field=session.output_type.fields_by_name["signed"],
        state_enum=state_enum, has_course="GetCourse" in by_name)


def build_surfaces(loaded_files, runtime):
    """Every service of every loaded file, each contract served once."""
    surfaces = {}
    for loaded in loaded_files:
        for service in loaded.file.services_by_name.values():
            if service.name in surfaces:
                raise SurfaceError(f"contract {service.name} is served by "
                                   "two .proto files")
            surfaces[service.name] = build_surface(loaded, service, runtime)
    return surfaces
