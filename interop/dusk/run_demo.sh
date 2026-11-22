#!/usr/bin/env bash
# Builds the runtime, the compiler, and the two saga modules, the host driven
# one and the engine driven one, builds both dusk demos against libgeasrt by
# its SONAME, and runs them. Exits 0 with a skip line when dusk is not on
# PATH, so a machine without dusk still passes the gate.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/target/geas-out"

if ! command -v dusk >/dev/null 2>&1; then
    echo "[demo_saga_dusk] dusk not found on PATH, skipping"
    echo "[demo_course_dusk] dusk not found on PATH, skipping"
    exit 0
fi

cd "$ROOT"

# make test-dusk builds the runtime and both modules through their own rules
# and sets GEAS_PREBUILT, so the gate never rebuilds a module another gate
# beside it is loading
if [ "${GEAS_PREBUILT:-0}" != "1" ]; then
    make compiler runtime
    target/bin/geas build skeleton/saga.geas
    target/bin/geas build skeleton/order_course.geas
fi

# The link resolves -l:libgeasrt.so.0 through LIBRARY_PATH and the loader
# finds the same file through LD_LIBRARY_PATH
export LIBRARY_PATH="$OUT${LIBRARY_PATH:+:$LIBRARY_PATH}"
export LD_LIBRARY_PATH="$OUT${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GEAS_OUT="$OUT"

dusk build interop/dusk/demo_saga.dusk
./target/dusk-out/demo_saga

dusk build interop/dusk/demo_course.dusk
./target/dusk-out/demo_course
