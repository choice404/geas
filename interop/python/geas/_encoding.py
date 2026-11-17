"""Value encoding into host and instance owned memory."""

import ctypes as C

from ._abi import (
    GEAS_ERR_OOM,
    GEAS_ERR_TYPE,
    GEAS_TY_BOOL,
    GEAS_TY_BYTES,
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
    GEAS_TY_UNIT,
    GeasValue,
)
from .errors import GeasError, _check
from .values import (
    Err,
    NONE,
    Ok,
    Record,
    Some,
    Typed,
    Variant,
    _set_typed_scalar,
)


class _ValueEncoder:
    """Builds host and instance owned values for a Runtime."""

    def _encode_host(self, target, py, keep):
        """Encodes a Python scalar or string into target using host owned
        memory; the runtime deep copies at the call, so keep only needs to
        hold the buffers until the call returns. bool tests first because a
        Python bool is an int."""
        C.memset(C.byref(target), 0, C.sizeof(target))
        if isinstance(py, Typed) and py.ty in (GEAS_TY_STRING, GEAS_TY_BYTES):
            self._encode_host(target, py.value, keep)
            target.ty = py.ty
        elif isinstance(py, Typed):
            _set_typed_scalar(target, py)
        elif isinstance(py, bool):
            target.ty = GEAS_TY_BOOL
            target.as_.b = 1 if py else 0
        elif isinstance(py, int):
            target.ty = GEAS_TY_INT
            target.as_.i = py
        elif isinstance(py, float):
            target.ty = GEAS_TY_FLOAT
            target.as_.f = py
        elif isinstance(py, (str, bytes, bytearray)):
            raw = py.encode("utf-8") if isinstance(py, str) else bytes(py)
            buf = C.create_string_buffer(raw, max(len(raw), 1))
            keep.append(buf)
            target.ty = GEAS_TY_STRING if isinstance(py, str) else GEAS_TY_BYTES
            target.as_.s.ptr = C.cast(buf, C.c_void_p)
            target.as_.s.len = len(raw)
        else:
            raise GeasError(GEAS_ERR_TYPE,
                           f"cannot pass {type(py).__name__} by host memory")

    def _encode_owned(self, ctx, target, py):
        """Encodes a Python value into target using instance owned memory,
        the form a pledge body's out and a list's elements require."""
        _TAGS = {int: GEAS_TY_INT, float: GEAS_TY_FLOAT, bool: GEAS_TY_BOOL}
        C.memset(C.byref(target), 0, C.sizeof(target))
        if py is None:
            target.ty = GEAS_TY_UNIT
        elif isinstance(py, bool):
            target.ty = GEAS_TY_BOOL
            target.as_.b = 1 if py else 0
        elif isinstance(py, int):
            target.ty = GEAS_TY_INT
            target.as_.i = py
        elif isinstance(py, float):
            target.ty = GEAS_TY_FLOAT
            target.as_.f = py
        elif isinstance(py, str):
            raw = py.encode("utf-8")
            sv = self._lib.geas_string_copy(ctx, raw, len(raw))
            C.memmove(C.byref(target), C.byref(sv), C.sizeof(GeasValue))
        elif isinstance(py, (bytes, bytearray)):
            raw = bytes(py)
            _check(self._lib.geas_bytes_copy_into(ctx, raw, len(raw), C.byref(target)),
                   "geas_bytes_copy_into")
        elif isinstance(py, (Ok, Err)):
            box = self._lib.geas_box(ctx)
            if not box:
                raise GeasError(GEAS_ERR_OOM, "geas_box")
            self._encode_owned(ctx, box[0], py.value)
            target.ty = GEAS_TY_RESULT
            target.tag = 0 if isinstance(py, Ok) else 1
            target.as_.box = C.cast(box, C.c_void_p)
        elif isinstance(py, Some):
            box = self._lib.geas_box(ctx)
            if not box:
                raise GeasError(GEAS_ERR_OOM, "geas_box")
            self._encode_owned(ctx, box[0], py.value)
            target.ty = GEAS_TY_OPTION
            target.tag = 1
            target.as_.box = C.cast(box, C.c_void_p)
        elif py is NONE:
            target.ty = GEAS_TY_OPTION
        elif isinstance(py, Typed):
            self._encode_typed(ctx, target, py)
        elif isinstance(py, (Record, Variant)):
            self._encode_fields(ctx, target, py.fields)
            target.ty = GEAS_TY_RECORD if isinstance(py, Record) else GEAS_TY_SUM
            target.tag = py.tag if isinstance(py, Variant) else 0
        elif isinstance(py, list):
            if not py:
                raise GeasError(GEAS_ERR_TYPE, "cannot infer an empty list's "
                                             "element type")
            elem_ty = _TAGS.get(bool if isinstance(py[0], bool)
                                else type(py[0]))
            if isinstance(py[0], str):
                elem_ty = GEAS_TY_STRING
            if isinstance(py[0], (bytes, bytearray)):
                elem_ty = GEAS_TY_BYTES
            if elem_ty is None:
                raise GeasError(GEAS_ERR_TYPE,
                               f"unsupported list element {type(py[0])}")
            _check(self._lib.geas_list_new(ctx, elem_ty, len(py), target),
                   "geas_list_new")
            elem = GeasValue()
            for x in py:
                self._encode_owned(ctx, elem, x)
                _check(self._lib.geas_list_push(ctx, target, C.byref(elem)),
                       "geas_list_push")
        else:
            raise GeasError(GEAS_ERR_TYPE,
                           f"cannot encode {type(py).__name__}")

    def _encode_typed(self, ctx, target, py):
        """Encodes a Typed value under the tag it names, onto the instance.
        A string, bytes, list, tuple, and map build their storage there; a
        scalar is the union arm alone."""
        if py.ty == GEAS_TY_STRING:
            self._encode_owned(ctx, target, str(py.value))
        elif py.ty == GEAS_TY_BYTES:
            self._encode_owned(ctx, target, bytes(py.value))
        elif py.ty == GEAS_TY_LIST:
            items = list(py.value)
            _check(self._lib.geas_list_new(ctx, py.elem_ty, len(items), target),
                   "geas_list_new")
            elem = GeasValue()
            for x in items:
                self._encode_owned(ctx, elem, x)
                _check(self._lib.geas_list_push(ctx, target, C.byref(elem)),
                       "geas_list_push")
        elif py.ty == GEAS_TY_TUPLE:
            self._encode_fields(ctx, target, list(py.value))
            target.ty = GEAS_TY_TUPLE
        elif py.ty == GEAS_TY_MAP:
            self._encode_map(ctx, target, py)
        else:
            _set_typed_scalar(target, py)

    def _encode_map(self, ctx, target, py):
        """A map through the runtime's own set, so a repeated key replaces
        its value in place the way the language's m[k] = v does."""
        _check(self._lib.geas_map_new_into(ctx, py.elem_ty, target),
               "geas_map_new_into")
        key, value = GeasValue(), GeasValue()
        for k, v in py.value:
            self._encode_owned(ctx, key, k)
            self._encode_owned(ctx, value, v)
            _check(self._lib.geas_map_set(ctx, target, C.byref(key),
                                          C.byref(value)), "geas_map_set")

    def _encode_fields(self, ctx, target, fields):
        """Lays a record's or a variant's fields out in an instance owned
        array, the shape both carry on the wire."""
        if not fields:
            return
        n = len(fields)
        raw = self._lib.geas_bytes(ctx, n * C.sizeof(GeasValue))
        if not raw:
            raise GeasError(GEAS_ERR_OOM, "geas_bytes")
        arr = C.cast(raw, C.POINTER(GeasValue))
        for k, field in enumerate(fields):
            self._encode_owned(ctx, arr[k], field)
        target.as_.list.data = raw
        target.as_.list.len = n
        target.as_.list.cap = n
