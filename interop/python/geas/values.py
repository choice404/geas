"""Python values, decoding, and storage for by reference arguments."""

import ctypes as C

from ._abi import (
    GEAS_ERR_TYPE,
    GEAS_TY_BOOL,
    GEAS_TY_BYTE,
    GEAS_TY_BYTES,
    GEAS_TY_CHAR,
    GEAS_TY_FLOAT,
    GEAS_TY_INT,
    GEAS_TY_LIST,
    GEAS_TY_MAP,
    GEAS_TY_OPTION,
    GEAS_TY_RECORD,
    GEAS_TY_RESULT,
    GEAS_TY_STRING,
    GEAS_TY_SUM,
    GEAS_TY_TUPLE,
    GEAS_TY_UINT,
    GEAS_TY_UNIT,
    GeasString,
    GeasValue,
)
from .errors import GeasError


# ---------------------------------------------------------------------------
# The sum shaped values, seen from Python. Ok and Err wrap a Result's payload,
# Some wraps an Option's, and NONE is the one None variant, a distinct
# sentinel because Python's None already means Unit at this boundary.
# ---------------------------------------------------------------------------


class Ok:
    __slots__ = ("value",)

    def __init__(self, value):
        self.value = value

    def __repr__(self):
        return f"Ok({self.value!r})"

    def __eq__(self, other):
        return isinstance(other, Ok) and other.value == self.value


class Err:
    __slots__ = ("value",)

    def __init__(self, value):
        self.value = value

    def __repr__(self):
        return f"Err({self.value!r})"

    def __eq__(self, other):
        return isinstance(other, Err) and other.value == self.value


class Some:
    __slots__ = ("value",)

    def __init__(self, value):
        self.value = value

    def __repr__(self):
        return f"Some({self.value!r})"

    def __eq__(self, other):
        return isinstance(other, Some) and other.value == self.value


class Record:
    """A record value built from Python, its fields in declaration order.
    A record decodes back as a plain list, so this is only needed going in."""

    def __init__(self, *fields):
        self.fields = list(fields)

    def __repr__(self):
        return f"Record({', '.join(repr(f) for f in self.fields)})"


class Variant:
    """A sum value built from Python: the variant's declaration index in its
    sum and its fields. A sum decodes back as the (tag, fields) pair."""

    def __init__(self, tag, *fields):
        self.tag = tag
        self.fields = list(fields)

    def __repr__(self):
        return f"Variant({self.tag}, {self.fields})"


class _NoneVariant:
    _instance = None

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance

    def __repr__(self):
        return "NONE"


NONE = _NoneVariant()


class Typed:
    """A value whose Geas type tag the host names itself, for the shapes
    Python's own types cannot pick: a UInt, a Byte, a Char, a list whose
    element tag is declared rather than guessed, an empty list included, a
    Map, and a Tuple. value is the scalar, the element list, or for a Map
    the (key, value) pairs in insertion order, and elem_ty is a list's
    element tag or a map's key tag. A host that knows a declared signature,
    the gRPC bridge reading an emitted .proto, builds its values with it."""

    __slots__ = ("ty", "value", "elem_ty")

    def __init__(self, ty, value=None, elem_ty=0):
        self.ty = ty
        self.value = value
        self.elem_ty = elem_ty

    def __repr__(self):
        return f"Typed({self.ty}, {self.value!r}, elem_ty={self.elem_ty})"


# ---------------------------------------------------------------------------
# Decoding: GeasValue to Python. Everything is copied out, strings included,
# so nothing the caller holds afterwards points into instance memory.
# ---------------------------------------------------------------------------


def decode(v):
    """Decodes one GeasValue into ordinary Python data, deeply and by copy."""
    ty = v.ty
    if ty == GEAS_TY_UNIT:
        return None
    if ty == GEAS_TY_INT:
        return v.as_.i
    if ty == GEAS_TY_UINT:
        return v.as_.u
    if ty == GEAS_TY_FLOAT:
        return v.as_.f
    if ty == GEAS_TY_BOOL:
        return bool(v.as_.b)
    if ty == GEAS_TY_BYTE:
        return v.as_.b
    if ty == GEAS_TY_CHAR:
        return chr(v.as_.ch)
    if ty == GEAS_TY_BYTES:
        return C.string_at(v.as_.s.ptr, v.as_.s.len) if v.as_.s.len else b""
    if ty == GEAS_TY_STRING:
        if not v.as_.s.ptr or v.as_.s.len == 0:
            return ""
        return C.string_at(v.as_.s.ptr, v.as_.s.len).decode("utf-8")
    if ty in (GEAS_TY_LIST, GEAS_TY_TUPLE, GEAS_TY_RECORD, GEAS_TY_SUM):
        n = v.as_.list.len
        elems = []
        if v.as_.list.data and n:
            arr = C.cast(v.as_.list.data, C.POINTER(GeasValue))
            elems = [decode(arr[i]) for i in range(n)]
        if ty == GEAS_TY_TUPLE:
            return tuple(elems)
        if ty == GEAS_TY_SUM:
            return (v.tag, elems)
        return elems
    if ty == GEAS_TY_MAP:
        # Interleaved key and value slots in insertion order, which a dict
        # keeps
        n = v.as_.list.len
        if not v.as_.list.data or n == 0:
            return {}
        arr = C.cast(v.as_.list.data, C.POINTER(GeasValue))
        return {decode(arr[i]): decode(arr[i + 1]) for i in range(0, n - 1, 2)}
    if ty == GEAS_TY_OPTION:
        if v.tag == 0 or not v.as_.box:
            return NONE
        return Some(decode(C.cast(v.as_.box, C.POINTER(GeasValue))[0]))
    if ty == GEAS_TY_RESULT:
        payload = None
        if v.as_.box:
            payload = decode(C.cast(v.as_.box, C.POINTER(GeasValue))[0])
        return Ok(payload) if v.tag == 0 else Err(payload)
    raise GeasError(GEAS_ERR_TYPE, f"cannot decode type tag {ty}")


def _set_typed_scalar(target, py):
    """Writes a Typed scalar's union arm and tag. A Char may come as a one
    character string or as its scalar value."""
    arms = {GEAS_TY_INT: "i", GEAS_TY_UINT: "u", GEAS_TY_FLOAT: "f",
            GEAS_TY_BOOL: "b", GEAS_TY_BYTE: "b", GEAS_TY_CHAR: "ch"}
    if py.ty == GEAS_TY_UNIT:
        target.ty = GEAS_TY_UNIT
        return
    arm = arms.get(py.ty)
    if arm is None:
        raise GeasError(GEAS_ERR_TYPE, f"cannot encode Typed tag {py.ty} here")
    value = py.value
    if py.ty == GEAS_TY_CHAR and isinstance(value, str):
        value = ord(value)
    if py.ty == GEAS_TY_FLOAT:
        value = float(value)
    elif py.ty == GEAS_TY_BOOL:
        value = 1 if value else 0
    else:
        value = int(value)
    setattr(target.as_, arm, value)
    target.ty = py.ty


# ---------------------------------------------------------------------------
# By-reference arguments. Each Ref owns the host side storage the runtime
# copies in from and writes back to; read .value after the wait delivered.
# ---------------------------------------------------------------------------


class IntRef:
    """An Int passed by reference: host storage the delivery writes back."""

    ty = GEAS_TY_INT

    def __init__(self, initial=0):
        self._storage = C.c_int64(int(initial))

    def _host_ptr(self):
        return C.cast(C.byref(self._storage), C.c_void_p)

    @property
    def value(self):
        return self._storage.value


class FloatRef:
    ty = GEAS_TY_FLOAT

    def __init__(self, initial=0.0):
        self._storage = C.c_double(float(initial))

    def _host_ptr(self):
        return C.cast(C.byref(self._storage), C.c_void_p)

    @property
    def value(self):
        return self._storage.value


class StringRef:
    """A String passed by reference. The default write back repoints the
    GeasString at instance owned bytes, so .value copies them into Python at
    the read; read it after the wait and before the break, per the ABI."""

    ty = GEAS_TY_STRING

    def __init__(self, initial=""):
        raw = initial.encode("utf-8")
        self._buf = C.create_string_buffer(raw, max(len(raw), 1))
        self._storage = GeasString(C.cast(self._buf, C.c_void_p), len(raw))

    def _host_ptr(self):
        return C.cast(C.byref(self._storage), C.c_void_p)

    @property
    def value(self):
        if not self._storage.ptr or self._storage.len == 0:
            return ""
        return C.string_at(self._storage.ptr, self._storage.len).decode("utf-8")
