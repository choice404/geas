"""cli.py: the server's flags, and the .proto each module's surface lives in.

A module's surface is the file geas emit-proto wrote beside it: the module
target/geas-out/libsaga.geas.so serves target/geas-out/saga.proto unless a
--proto names the files outright.
"""

import argparse
import logging
import sys
from pathlib import Path

from geas import GeasError

from .handlers import HandlerError
from .protoload import ProtoLoadError
from .server import Config, serve
from .sessions import DEFAULT_RESERVE_WORKERS, AdmissionError
from .surface import SurfaceError

log = logging.getLogger("bridge_server")

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_RUNTIME = ROOT / "target" / "geas-out" / "libgeasrt.so"
MODULE_SUFFIX = ".geas.so"


def proto_beside(module):
    """The emitted .proto beside a built module."""
    path = Path(module)
    stem = path.name
    if stem.endswith(MODULE_SUFFIX):
        stem = stem[:-len(MODULE_SUFFIX)]
    if stem.startswith("lib"):
        stem = stem[len("lib"):]
    return path.with_name(f"{stem}.proto")


def parse_args(argv):
    ap = argparse.ArgumentParser(
        prog="bridge_server.py",
        description="Serve compiled Geas contracts over their emitted gRPC surface.")
    ap.add_argument("--runtime", default=str(DEFAULT_RUNTIME), metavar="PATH",
                    help="libgeasrt to load")
    ap.add_argument("--module", action="append", required=True, metavar="PATH",
                    help="a compiled .geas.so, repeatable")
    ap.add_argument("--proto", action="append", default=[], metavar="PATH",
                    help="an emitted .proto, repeatable; default <stem>.proto "
                         "beside each module")
    ap.add_argument("--handlers", action="append", default=[], metavar="PYFILE",
                    help="a Python file whose HANDLERS dict supplies abstract "
                         "bodies, repeatable")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=50251)
    ap.add_argument("--park-dsn", metavar="PATH",
                    help="the sqlite park store a closed session writes to")
    ap.add_argument("--tick-ms", type=int, default=50, metavar="N",
                    help="the runtime ticker's period, 0 to run without one")
    ap.add_argument("--max-workers", type=int, default=32, metavar="N",
                    help="gRPC worker threads, one held per live session")
    ap.add_argument("--reserve-workers", type=int, default=DEFAULT_RESERVE_WORKERS,
                    metavar="N",
                    help="workers no session may hold, kept for pledge, signal, "
                         "partial, course, and Break rpcs; live sessions are "
                         "capped at max workers less this")
    return ap.parse_args(argv)


def build_config(args):
    protos = args.proto or [str(proto_beside(m)) for m in args.module]
    return Config(runtime=args.runtime, modules=tuple(args.module),
                  protos=tuple(protos), handlers=tuple(args.handlers),
                  host=args.host, port=args.port, park_dsn=args.park_dsn,
                  tick_ms=args.tick_ms, max_workers=args.max_workers,
                  reserve_workers=args.reserve_workers)


def main(argv=None):
    logging.basicConfig(stream=sys.stdout, level=logging.INFO,
                        format="[bridge_server] %(message)s")
    config = build_config(parse_args(argv))
    try:
        serve(config)
    except (AdmissionError, GeasError, HandlerError, ProtoLoadError,
            SurfaceError) as e:
        log.error("cannot start: %s", e)
        return 1
    return 0
