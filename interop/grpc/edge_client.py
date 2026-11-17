"""edge_client.py: the bridge's endings and limits, each a walk against its
own small contract under interop/grpc/contracts.

  --stall-walk         Break and a dropped stream beside a hold in flight
  --admission-walk N   N sessions fill the limit, a unary call still answers,
                       the next Session is RESOURCE_EXHAUSTED
  --handoff-walk DIR   a parked course runs no Geas remedy, its stores in DIR
  --nested-walk        None, Some(None), and Some(Some(x)) both ways

The stubs are protoc's output over the emitted .proto files, generated into
target/grpc-gen by the make gate. Exit status 0 means every check held.
"""

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "target" / "grpc-gen"))

from clientkit import Tally, channel_ready  # noqa: E402

TALLY = Tally("edge_client")


def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--stall-walk", action="store_true",
                      help="Break and a dropped stream beside a hold in flight")
    mode.add_argument("--admission-walk", type=int, metavar="N",
                      help="fill a session limit of N and probe past it")
    mode.add_argument("--handoff-walk", metavar="DIR",
                      help="park a course and count its remedy runs, stores in DIR")
    mode.add_argument("--nested-walk", action="store_true",
                      help="cross every nesting of an Option of an Option")
    return ap.parse_args()


def run(args, channel):
    if args.stall_walk:
        import walk_stall
        walk_stall.walk(channel, TALLY)
    elif args.admission_walk is not None:
        import walk_stall
        walk_stall.admission_walk(channel, TALLY, args.admission_walk)
    elif args.handoff_walk:
        import walk_handoff
        walk_handoff.walk(channel, TALLY, args.handoff_walk)
    else:
        import walk_nested
        walk_nested.walk(channel, TALLY)


def main():
    args = parse_args()
    channel = channel_ready(args.port, TALLY)
    if channel is None:
        return 1
    with channel:
        run(args, channel)
    if TALLY.failures:
        return 1
    print("[edge_client] ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
