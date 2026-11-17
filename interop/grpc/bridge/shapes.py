"""shapes.py: the Geas shape each emitted message carries, read from the
descriptor alone.

docs/bridge.md, The type set, is the whole map. A scalar is a proto scalar,
Duration and Time ride int64 as milliseconds, a declared record is a message
with its fields in declaration order, a sum with a payload arm is a message
whose oneof named arm carries one message per arm in arm order, a sum of bare
arms is an enum in the ABI's tag space, Unit is the empty message, and
Option, List, Map, and Tuple are wrapper messages named for their element
words. The wrappers are recognized by their emitted shape, a name suffix and
the exact fields the emitter writes, so nothing here knows any one contract.
"""

from google.protobuf.descriptor import FieldDescriptor as FD

from geas import (GEAS_TY_BOOL, GEAS_TY_BYTE, GEAS_TY_BYTES, GEAS_TY_CHAR,
                  GEAS_TY_FLOAT, GEAS_TY_INT, GEAS_TY_LIST, GEAS_TY_MAP,
                  GEAS_TY_OPTION, GEAS_TY_RECORD, GEAS_TY_RESULT,
                  GEAS_TY_STRING, GEAS_TY_SUM, GEAS_TY_TUPLE, GEAS_TY_UINT,
                  GEAS_TY_UNIT)

UNIT, OPTION, LIST, MAP, TUPLE, RESULT, SUM, RECORD, BYTES_VALUE = (
    "unit", "option", "list", "map", "tuple", "result", "sum", "record",
    "bytes_value")

SCALAR_TAGS = {
    FD.TYPE_INT64: GEAS_TY_INT, FD.TYPE_UINT64: GEAS_TY_UINT,
    FD.TYPE_DOUBLE: GEAS_TY_FLOAT, FD.TYPE_BOOL: GEAS_TY_BOOL,
    FD.TYPE_STRING: GEAS_TY_STRING, FD.TYPE_BYTES: GEAS_TY_BYTES,
}

KIND_TAGS = {
    UNIT: GEAS_TY_UNIT, OPTION: GEAS_TY_OPTION, LIST: GEAS_TY_LIST,
    MAP: GEAS_TY_MAP, TUPLE: GEAS_TY_TUPLE, RESULT: GEAS_TY_RESULT,
    SUM: GEAS_TY_SUM, RECORD: GEAS_TY_RECORD, BYTES_VALUE: GEAS_TY_BYTES,
}


class ConversionError(ValueError):
    """A value that does not fit the shape its descriptor names."""


# ---- recognizing the emitted shapes ----

def _field_names(desc):
    return [f.name for f in desc.fields]


def _is_one_field(desc, suffix, name, want_repeated):
    if not desc.name.endswith(suffix) or _field_names(desc) != [name]:
        return False
    field = desc.fields[0]
    return field.number == 1 and is_repeated(field) == want_repeated


def _is_option(desc):
    """An Option wrapper's one value field either tracks presence by the
    proto3 optional label or is a message, whose presence proto tracks
    itself; a record that only happens to share the name and field is a
    plain scalar and stays a record."""
    if not _is_one_field(desc, "Option", "value", False):
        return False
    field = desc.fields[0]
    return is_optional(field) or field.type == FD.TYPE_MESSAGE


def _is_map(desc):
    if not _is_one_field(desc, "Map", "entries", True):
        return False
    entry = desc.fields[0].message_type
    return entry is not None and _field_names(entry) == ["key", "value"]


def _is_tuple(desc):
    names = _field_names(desc)
    return (desc.name.endswith("Tuple") and len(names) > 1 and
            names == [f"item{i}" for i in range(1, len(names) + 1)])


def _is_oneof_of_all(desc, oneof_name):
    oneof = desc.oneofs_by_name.get(oneof_name)
    return oneof is not None and len(oneof.fields) == len(desc.fields)


_kinds = {}


def shape_of(desc):
    """The Geas shape a message descriptor carries, cached by full name."""
    kind = _kinds.get(desc.full_name)
    if kind is None:
        kind = _classify(desc)
        _kinds[desc.full_name] = kind
    return kind


def _classify(desc):
    if desc.name == "Unit" and not desc.fields:
        return UNIT
    if desc.name == "BytesValue" and _field_names(desc) == ["value"]:
        return BYTES_VALUE
    if _is_option(desc):
        return OPTION
    if _is_one_field(desc, "List", "values", True):
        return LIST
    if _is_map(desc):
        return MAP
    if _is_tuple(desc):
        return TUPLE
    if desc.name.endswith("Result") and _is_oneof_of_all(desc, "result"):
        return RESULT
    if desc.fields and _is_oneof_of_all(desc, "arm"):
        return SUM
    return RECORD


def is_repeated(field):
    """Whether a field is repeated; protobuf 4 spells it as the label,
    later releases as a property."""
    flag = getattr(field, "is_repeated", None)
    if flag is not None:
        return bool(flag)
    return field.label == FD.LABEL_REPEATED


def is_optional(field):
    """Whether a field is proto3 optional, an Option whose presence proto
    tracks by label: a scalar or enum alone in its synthetic oneof."""
    oneof = field.containing_oneof
    return (oneof is not None and len(oneof.fields) == 1 and
            oneof.name == "_" + field.name)


def _uint32_tag(owner):
    """A uint32 is only ever a Map key, Byte or Char, and the map's own name
    leads with the key's word."""
    name = owner.name
    return GEAS_TY_BYTE if name.startswith("Byte") else GEAS_TY_CHAR


def element_tag(field):
    """The Geas type tag of one element a field carries."""
    if field.type == FD.TYPE_MESSAGE:
        return KIND_TAGS[shape_of(field.message_type)]
    if field.type == FD.TYPE_ENUM:
        return GEAS_TY_SUM
    if field.type == FD.TYPE_UINT32:
        return _uint32_tag(field.containing_type)
    tag = SCALAR_TAGS.get(field.type)
    if tag is None:
        raise ConversionError(f"field {field.name} has a proto type outside "
                              "the bridged set")
    return tag


def ordered_fields(desc):
    return sorted(desc.fields, key=lambda f: f.number)
