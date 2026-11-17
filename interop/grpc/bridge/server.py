"""server.py: one runtime, every loaded contract, one gRPC server.

Startup is the in process host's order and nothing else: load each module,
bind the supplied bodies, freeze, then read each emitted .proto against the
registered contracts and register one generic handler per service. The
runtime's ticker runs for the server's life, so a deadline, a lapse, and a
course's wait fall due with no rpc in flight to tick them.
"""

import logging
from concurrent import futures
from dataclasses import dataclass

import grpc

from geas import Runtime

from .calls import Calls
from .handlers import bind_handlers, load_handlers
from .lifecycle import Lifecycle, ParkStore
from .protoload import load_protos
from .sessions import Reaper, SessionSlots
from .surface import RESUME, SESSION, build_surfaces
from .table import InstanceTable

log = logging.getLogger("bridge_server")


@dataclass(frozen=True)
class Config:
    runtime: str
    modules: tuple
    protos: tuple
    handlers: tuple
    host: str
    port: int
    park_dsn: str
    tick_ms: int
    max_workers: int
    reserve_workers: int


def _method_handler(lifecycle, calls, surface, method):
    serializers = dict(request_deserializer=method.request.FromString,
                       response_serializer=method.reply.SerializeToString)
    if method.role == SESSION:
        return grpc.unary_stream_rpc_method_handler(
            lambda req, ctx: lifecycle.session(surface, req, ctx), **serializers)
    if method.role == RESUME:
        return grpc.unary_stream_rpc_method_handler(
            lambda req, ctx: lifecycle.resume(surface, req, ctx), **serializers)
    return grpc.unary_unary_rpc_method_handler(
        calls.handler(surface, method), **serializers)


def _generic_handlers(lifecycle, calls, surfaces):
    out = []
    for surface in surfaces.values():
        table = {m.name: _method_handler(lifecycle, calls, surface, m)
                 for m in surface.methods}
        out.append(grpc.method_handlers_generic_handler(surface.service, table))
        log.info("serving %s, %d rpcs%s", surface.service, len(table),
                 ", engine driven" if surface.has_course else "")
    return tuple(out)


def open_runtime(config):
    """The runtime with every module loaded, every body bound, and frozen."""
    runtime = Runtime(config.runtime)
    for module in config.modules:
        runtime.load(module)
    bind_handlers(runtime, load_handlers(config.handlers))
    runtime.freeze()
    if config.tick_ms > 0:
        runtime.start_ticker(config.tick_ms)
    return runtime


def serve(config):
    # The pool is checked before anything loads, so a size that cannot hold
    # a session beside the reserve refuses to start
    slots = SessionSlots(config.max_workers, config.reserve_workers)
    runtime = open_runtime(config)
    _, loaded = load_protos(config.protos)
    surfaces = build_surfaces(loaded, runtime)
    table = InstanceTable()
    park = ParkStore(config.park_dsn) if config.park_dsn else None
    lifecycle = Lifecycle(runtime, table, park, slots, Reaper())
    # A session holds a worker thread for the instance's whole life, and the
    # slots keep the reserve free for the calls that drive them
    pool = futures.ThreadPoolExecutor(max_workers=config.max_workers)
    server = grpc.server(pool)
    server.add_generic_rpc_handlers(_generic_handlers(lifecycle, Calls(table), surfaces))
    bound = server.add_insecure_port(f"{config.host}:{config.port}")
    server.start()
    log.info("listening on %s:%d, sessions are streams, at most %d live",
             config.host, bound, slots.limit)
    try:
        server.wait_for_termination()
    except KeyboardInterrupt:
        pass
    finally:
        # The pool joins before the runtime goes away underneath a body
        server.stop(0).wait()
        lifecycle.shutdown()
        pool.shutdown(wait=True)
        runtime.shutdown()
        log.info("stopped")
