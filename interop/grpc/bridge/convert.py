"""convert.py: proto messages to Geas values and back, by reflection over
the emitted descriptors alone.

Inbound values are built from the geas package's own types with the type tag spelled
out through Typed, so a UInt stays a UInt, a Byte map key stays a Byte, and
an empty list keeps its element tag. Outbound values are what geas.decode
answers: a record as a list, a sum as its (tag, fields) pair, a tuple as a
tuple, a map as a dict in insertion order, and Ok, Err, Some, and NONE for
the sum shaped types.

A declared field, a record's, a sum arm's, or a pledge or signal
parameter, spells an Option of a message shaped type as the message itself,
its presence the Option's. Where that message is an Option wrapper, the
field is an Option of an Option, never anything else, so its presence is the
outer Some and the wrapper the inner Option, and None, Some(None), and
Some(Some(x)) all cross both ways. Where it is any other message the same
spelling also names the plain type, so outbound an absent value leaves the
field unset and inbound the field reads as the plain type. A position the
emitter spells by type alone, an Option wrapper's value, a map value, a
tuple item, carries its type exactly. Every other shape crosses both ways
without a guess.
"""

from google.protobuf.descriptor import FieldDescriptor as FD

from geas import (GEAS_TY_BYTE, GEAS_TY_BYTES, GEAS_TY_CHAR, GEAS_TY_LIST, GEAS_TY_MAP, GEAS_TY_TUPLE,
                  NONE, Err, Ok, Record, Some, Typed, Variant)
from .shapes import (BYTES_VALUE, LIST, MAP, OPTION, RECORD, RESULT, SUM,
                    TUPLE, UNIT, ConversionError, element_tag, is_optional,
                    is_repeated, ordered_fields, shape_of)

# ---- inbound: a message to the Geas value its shape names ----


def message_value(msg):
    """The Geas value one whole message carries."""
    return _INBOUND[shape_of(msg.DESCRIPTOR)](msg)


def request_args(request):
    """A pledge or signal request's parameters in declaration order, every
    field after the instance id."""
    return [declared_value(request, f) for f in ordered_fields(request.DESCRIPTOR)
            if f.name != "instance_id"]


def _is_option_message(field):
    return (field.type == FD.TYPE_MESSAGE and not is_repeated(field) and
            shape_of(field.message_type) == OPTION)


def declared_value(msg, field):
    """One declared field: an Option wrapper there is an Option of that
    Option, the field's presence the outer Some."""
    if not _is_option_message(field):
        return field_value(msg, field)
    if not msg.HasField(field.name):
        return NONE
    return Some(message_value(getattr(msg, field.name)))


def field_value(msg, field):
    """One field as the Geas value its declaration names."""
    if is_repeated(field):
        items = [_element_value(field, x) for x in getattr(msg, field.name)]
        return Typed(GEAS_TY_LIST, items, element_tag(field))
    if is_optional(field):
        if not msg.HasField(field.name):
            return NONE
        return Some(_element_value(field, getattr(msg, field.name)))
    return _element_value(field, getattr(msg, field.name))


def _element_value(field, value):
    if field.type == FD.TYPE_MESSAGE:
        return message_value(value)
    if field.type == FD.TYPE_ENUM:
        if field.enum_type.values_by_number.get(value) is None:
            raise ConversionError(f"{field.name} names no arm of "
                                  f"{field.enum_type.name}")
        return Variant(int(value))
    tag = element_tag(field)
    if tag in (GEAS_TY_BYTE, GEAS_TY_CHAR):
        _check_narrow(field, tag, value)
    return Typed(tag, value)


def _check_narrow(field, tag, value):
    """A Byte and a Char ride uint32, wider than either, so the value is
    held to its own range: a Byte to 0 through 255, a Char to a Unicode
    scalar value, which leaves out the surrogate block."""
    if tag == GEAS_TY_BYTE and value > 0xFF:
        raise ConversionError(f"{field.name} is a Byte and {value} is past 255")
    is_surrogate = 0xD800 <= value <= 0xDFFF
    if tag == GEAS_TY_CHAR and (value > 0x10FFFF or is_surrogate):
        raise ConversionError(f"{field.name} is a Char and {value} is not a "
                              "Unicode scalar value")


def _option_in(msg):
    field = msg.DESCRIPTOR.fields[0]
    if is_optional(field):
        return field_value(msg, field)
    if not msg.HasField(field.name):
        return NONE
    return Some(field_value(msg, field))


def _map_in(msg):
    """A map's entries in the order they came. A key that comes twice is
    refused: the runtime holds a map to one entry per key at every boundary,
    and the bridge does not pick which of two values the client meant"""
    entry = msg.DESCRIPTOR.fields[0].message_type
    key_field, value_field = entry.fields_by_name["key"], entry.fields_by_name["value"]
    seen = set()
    for e in msg.entries:
        if e.key in seen:
            raise ConversionError(f"{msg.DESCRIPTOR.name} carries the key {e.key!r} twice")
        seen.add(e.key)
    pairs = [(_element_value(key_field, e.key), field_value(e, value_field))
             for e in msg.entries]
    return Typed(GEAS_TY_MAP, pairs, element_tag(key_field))


def _oneof_in(msg, oneof):
    which = msg.WhichOneof(oneof)
    if which is None:
        raise ConversionError(f"{msg.DESCRIPTOR.name} carries no {oneof} arm")
    field = msg.DESCRIPTOR.fields_by_name[which]
    return field, getattr(msg, which)


def _result_in(msg):
    field, value = _oneof_in(msg, "result")
    inner = _element_value(field, value)
    return Ok(inner) if field.name == "ok" else Err(inner)


def _sum_in(msg):
    field, arm = _oneof_in(msg, "arm")
    fields = [declared_value(arm, f) for f in ordered_fields(arm.DESCRIPTOR)]
    return Variant(field.number - 1, *fields)


_INBOUND = {
    UNIT: lambda msg: None,
    BYTES_VALUE: lambda msg: Typed(GEAS_TY_BYTES, msg.value),
    OPTION: _option_in,
    LIST: lambda msg: field_value(msg, msg.DESCRIPTOR.fields[0]),
    MAP: _map_in,
    TUPLE: lambda msg: Typed(GEAS_TY_TUPLE, [
        field_value(msg, f) for f in ordered_fields(msg.DESCRIPTOR)]),
    RESULT: _result_in,
    SUM: _sum_in,
    RECORD: lambda msg: Record(*[
        declared_value(msg, f) for f in ordered_fields(msg.DESCRIPTOR)]),
}

# ---- outbound: a decoded Geas value into a message ----


def fill_message(msg, value):
    """Writes a decoded value into msg, whose shape names its Geas type.
    Raises ConversionError when the value is not that type."""
    try:
        _OUTBOUND[shape_of(msg.DESCRIPTOR)](msg, value)
    except (TypeError, ValueError, AttributeError, IndexError) as e:
        if isinstance(e, ConversionError):
            raise
        raise ConversionError(f"{msg.DESCRIPTOR.name} cannot carry "
                              f"{value!r}: {e}") from e


def set_field(msg, field, value, is_declared=True):
    """Writes one field from its decoded value. A declared field spells an
    Option of a message as the message; a field spelled by type alone,
    is_declared False, carries the value as it is."""
    if is_repeated(field):
        _expect(value, list, field)
        for x in value:
            _append(msg, field, x)
        return
    if is_optional(field):
        if value is NONE:
            return
        _expect(value, Some, field)
        value = value.value
    if field.type == FD.TYPE_MESSAGE:
        _set_message(msg, field, value, is_declared)
        return
    setattr(msg, field.name, _scalar(field, value))


def _set_message(msg, field, value, is_declared):
    """A message field. A declared one outside a oneof takes an absent
    Option as unset and a present one as its payload; an arm and a field
    spelled by type alone always write the value."""
    if is_declared and field.containing_oneof is None:
        if value is NONE:
            return
        if isinstance(value, Some):
            value = value.value
    sub = getattr(msg, field.name)
    fill_message(sub, value)
    sub.SetInParent()


def _append(msg, field, value):
    if field.type == FD.TYPE_MESSAGE:
        fill_message(getattr(msg, field.name).add(), value)
        return
    getattr(msg, field.name).append(_scalar(field, value))


def _scalar(field, value):
    """A decoded scalar or bare sum as the proto value its field holds."""
    if field.type == FD.TYPE_ENUM:
        _expect(value, tuple, field)
        return value[0]
    if field.type == FD.TYPE_UINT32 and isinstance(value, str):
        return ord(value)
    if field.type == FD.TYPE_DOUBLE:
        return float(value)
    return value


def _expect(value, kind, field):
    if not isinstance(value, kind):
        raise ConversionError(f"{field.name} expects a {kind.__name__}, "
                              f"got {type(value).__name__}")


def _option_out(msg, value):
    if value is not NONE and not isinstance(value, Some):
        raise ConversionError(f"{msg.DESCRIPTOR.name} expects an Option")
    field = msg.DESCRIPTOR.fields[0]
    if value is NONE:
        return
    if is_optional(field):
        set_field(msg, field, value)
        return
    set_field(msg, field, value.value, is_declared=False)


def _map_out(msg, value):
    if not isinstance(value, dict):
        raise ConversionError(f"{msg.DESCRIPTOR.name} expects a map")
    entry = msg.DESCRIPTOR.fields[0].message_type
    key_field, value_field = entry.fields_by_name["key"], entry.fields_by_name["value"]
    for k, v in value.items():
        e = msg.entries.add()
        set_field(e, key_field, k)
        set_field(e, value_field, v, is_declared=False)


def _fields_out(msg, value, is_declared=True):
    """A record's list or a tuple's items, one per field in number order;
    a tuple's items are spelled by type alone."""
    fields = ordered_fields(msg.DESCRIPTOR)
    if not isinstance(value, (list, tuple)) or len(value) != len(fields):
        raise ConversionError(f"{msg.DESCRIPTOR.name} expects "
                              f"{len(fields)} fields")
    for field, x in zip(fields, value):
        set_field(msg, field, x, is_declared)


def _result_out(msg, value):
    if not isinstance(value, (Ok, Err)):
        raise ConversionError(f"{msg.DESCRIPTOR.name} expects a Result")
    arm = "ok" if isinstance(value, Ok) else "err"
    set_field(msg, msg.DESCRIPTOR.fields_by_name[arm], value.value)


def _sum_out(msg, value):
    if not isinstance(value, tuple) or len(value) != 2:
        raise ConversionError(f"{msg.DESCRIPTOR.name} expects a sum")
    tag, fields = value
    arm = msg.DESCRIPTOR.fields_by_number.get(tag + 1)
    if arm is None:
        raise ConversionError(f"{msg.DESCRIPTOR.name} has no arm {tag}")
    sub = getattr(msg, arm.name)
    _fields_out(sub, fields)
    sub.SetInParent()


_OUTBOUND = {
    UNIT: lambda msg, value: None,
    BYTES_VALUE: lambda msg, value: setattr(msg, "value", bytes(value)),
    OPTION: _option_out,
    LIST: lambda msg, value: set_field(msg, msg.DESCRIPTOR.fields[0], value),
    MAP: _map_out,
    TUPLE: lambda msg, value: _fields_out(msg, value, is_declared=False),
    RESULT: _result_out,
    SUM: _sum_out,
    RECORD: _fields_out,
}


def error_code(value):
    """The int64 a PledgeError carries for a broken pledge's payload: an Int
    as itself, a sum as its arm's tag, anything else 0. None is a payload
    the runtime zeroed and the caller skips."""
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, tuple) and len(value) == 2 and isinstance(value[0], int):
        return value[0]
    return 0
