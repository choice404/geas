"""The Python binding keeps the runtime out of the global symbol scope.

A Runtime opens libgeasrt with RTLD_LOCAL, so no runtime export and no
SQLite symbol linked into it is visible through the process's default scope,
while a compiled module still loads and binds to the one copy the binding
mapped. Run from the repository root after the payment module is built.
"""

import ctypes as C
import os
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "interop" / "python"))

from geas import Runtime  # noqa: E402

OUT = pathlib.Path("target/geas-out")
RTLD_DEFAULT = C.c_void_p(0)


def global_has(name):
    # dlsym over the default scope: the executable and every RTLD_GLOBAL library
    libdl = C.CDLL(None)
    libdl.dlsym.restype = C.c_void_p
    libdl.dlsym.argtypes = [C.c_void_p, C.c_char_p]
    return libdl.dlsym(RTLD_DEFAULT, name.encode()) is not None


def runtime_copies():
    # Every distinct file the process maps whose name is the runtime's
    seen = set()
    with open("/proc/self/maps") as maps:
        for line in maps:
            parts = line.split()
            if len(parts) >= 6 and "libgeasrt" in parts[5]:
                seen.add(os.path.realpath(parts[5]))
    return seen


def main():
    failures = 0

    def check(is_ok, what):
        nonlocal failures
        print(("ok   " if is_ok else "FAIL ") + what)
        if not is_ok:
            failures += 1

    with Runtime(OUT / "libgeasrt.so", max_threads=1) as rt:
        check(not global_has("geas_runtime_init"),
              "no runtime export is in the global scope")
        check(not global_has("sqlite3_open_v2"),
              "no SQLite symbol of the runtime is in the global scope")
        rt.load(OUT / "libpayment.geas.so")
        check(True, "a compiled module loads against the local runtime")
        check(len(runtime_copies()) == 1, "exactly one copy of the runtime is mapped")
    if failures:
        print(f"[test-python-scope] {failures} failed")
        sys.exit(1)
    print("[test-python-scope] ok")


if __name__ == "__main__":
    main()
