#!/usr/bin/env python3
"""CI (M11.4): a lift with nothing of the game in it, so the runtime library and the native code compile without the
player's disc.

    python3 tools/ci_stub_lift.py OUT_DIR [--ps3recomp DIR]

writes OUT_DIR/ppu_recomp.h -- the lifter's own fixed preamble (ppu_lifter.HEADER_PREAMBLE: the context, the memory
model, the load and store helpers), no function of any game declared -- and OUT_DIR/ppu_recomp_stub.cpp, an empty
translation unit including it (port/CMakeLists.txt wants a source beside the header). Enough for the targets
tools/zig_build.py build_prebuilt builds (ps3recomp_runtime, dotheroes_native, the HLE NID table); the game's program
itself needs a real lift and is never built from this.
"""
import argparse
import importlib.util
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out")
    ap.add_argument("--ps3recomp", default=os.path.join(ROOT, "build", "ref", "ps3recomp"))
    a = ap.parse_args()
    spec = importlib.util.spec_from_file_location("ppu_lifter", os.path.join(a.ps3recomp, "tools", "ppu_lifter.py"))
    lifter = importlib.util.module_from_spec(spec)
    sys.path.insert(0, os.path.join(a.ps3recomp, "tools"))
    spec.loader.exec_module(lifter)
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "ppu_recomp.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write(lifter.HEADER_PREAMBLE + "\n/* tools/ci_stub_lift.py: no function of any game */\n")
    with open(os.path.join(a.out, "ppu_recomp_stub.cpp"), "w", encoding="utf-8", newline="\n") as f:
        f.write('#include "ppu_recomp.h"\n')
    print(f"stub lift in {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
