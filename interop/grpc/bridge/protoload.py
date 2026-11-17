"""protoload.py: an emitted .proto read at startup into live descriptors.

The server takes the .proto geas emitted beside a module and nothing
generated from it. grpc_tools compiles the file into a descriptor set in a
scratch directory, the set goes into a pool of the server's own, and every
message class the surface needs comes out of that pool by reflection. The
source info rides along, because the emitted comments name a signal in its
own spelling and a CamelCase rpc alone cannot.
"""

import logging
import tempfile
from dataclasses import dataclass
from pathlib import Path

from google.protobuf import descriptor_pb2, descriptor_pool, message_factory
from grpc_tools import protoc

log = logging.getLogger("bridge_server")

# The descriptor proto path of a message's leading comment: field 4 of a
# file is message_type
MESSAGE_TYPE_FIELD = 4


class ProtoLoadError(RuntimeError):
    """A .proto the server could not read into descriptors."""


@dataclass(frozen=True)
class LoadedProto:
    """One emitted file: its descriptor and each top level message's leading
    comment by message name."""
    path: Path
    file: object
    comments: dict


def _compile(path):
    """Runs protoc over one file and answers the FileDescriptorProto, with
    its source info, or raises ProtoLoadError."""
    with tempfile.TemporaryDirectory(prefix="geasbridge") as scratch:
        out = Path(scratch) / "set.pb"
        status = protoc.main(["protoc", f"-I{path.parent}",
                              f"--descriptor_set_out={out}",
                              "--include_source_info", str(path)])
        if status != 0 or not out.exists():
            raise ProtoLoadError(f"protoc refused {path.name}")
        file_set = descriptor_pb2.FileDescriptorSet.FromString(out.read_bytes())
    if len(file_set.file) != 1:
        raise ProtoLoadError(f"{path.name} imports other files, an emitted "
                             "surface never does")
    return file_set.file[0]


def _message_comments(file_proto):
    """Each top level message's leading comment, stripped."""
    comments = {}
    for location in file_proto.source_code_info.location:
        path = list(location.path)
        if len(path) != 2 or path[0] != MESSAGE_TYPE_FIELD:
            continue
        name = file_proto.message_type[path[1]].name
        comments[name] = location.leading_comments.strip()
    return comments


def load_protos(paths):
    """Compiles every file into one pool and answers the pool and the
    LoadedProto list in argument order."""
    pool = descriptor_pool.DescriptorPool()
    loaded = []
    for path in paths:
        resolved = Path(path).resolve()
        if not resolved.is_file():
            raise ProtoLoadError(f"no .proto at {Path(path).name}")
        file_proto = _compile(resolved)
        try:
            pool.Add(file_proto)
        except TypeError as e:
            raise ProtoLoadError(f"{resolved.name} clashes with a file "
                                 f"already loaded: {e}") from e
        descriptor = pool.FindFileByName(file_proto.name)
        loaded.append(LoadedProto(resolved, descriptor,
                                  _message_comments(file_proto)))
        log.info("loaded %s, services %s", resolved.name,
                 ", ".join(descriptor.services_by_name))
    return pool, loaded


def message_class(desc):
    """The concrete message class of a descriptor in the server's pool."""
    return message_factory.GetMessageClass(desc)
