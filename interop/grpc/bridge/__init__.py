"""The generic gRPC bridge server: any compiled contract, served over the
surface geas emit-proto wrote for it. bridge_server.py is the entry point;
docs/bridge.md is the normative surface.

  protoload  an emitted .proto compiled into live descriptors at startup
  shapes     the Geas shape each message carries, read from its descriptor
  convert    proto messages to Geas values and back
  surface    one service's rpcs read against the loaded module
  table      the live instances and the gate every call passes
  sessions   the session limit, the reaper endings run on, and the quiet
             runtime calls an ending makes
  lifecycle  Session, Resume, and the one ending, park and release included
  calls      pledges, signals, the partial surface, the course, the break
  handlers   the abstract bodies a deployment supplies
  server     startup and the generic handlers
  cli        flags
"""
