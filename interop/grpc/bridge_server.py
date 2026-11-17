"""bridge_server.py: any compiled Geas contract, served over gRPC from the
surface geas emit-proto wrote for it.

The server loads each module through the C ABI, binds the abstract bodies a
deployer supplies in --handlers files, freezes, and reads each emitted
.proto at startup into descriptors, with no generated stub anywhere. Every
service in those files becomes a generic handler: Session and Resume are the
streams whose lifetime is the instance's, each pledge and each signal its
own typed unary rpc keyed by the instance id, and GetPartial, GetCourse, and
Break the shared reads and the ending. Values cross by reflection over the
descriptors, so one server serves every contract the language can emit.

  python bridge_server.py --module target/geas-out/libpayment.geas.so \\
      --handlers interop/grpc/handlers/payment.py --port 50251

The package beside this file holds the parts; docs/bridge.md is the
normative surface.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "interop" / "python"))

from bridge.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
