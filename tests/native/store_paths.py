#!/usr/bin/env python3
"""The lifted code's guest stores, tested outside the game (docs/NATIVE_R8.md
section 5.4.1): ppu_recomp.h's header as the lifter writes it, compiled with
tests/native/store_paths_test.cpp for each store variant and run.
    build/venv/Scripts/python.exe tests/native/store_paths.py [ps3recomp checkout]
One small compile per variant (the bundled Zig); the game is not built or run."""
import importlib.util
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import zig_build  # noqa: E402

PS3 = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "ref", "ps3recomp")
VARIANTS = {"checked": [], "mov": ["-DPPU_STORE_MOV=1"]}


def header():
    spec = importlib.util.spec_from_file_location("ppu_lifter", os.path.join(PS3, "tools", "ppu_lifter.py"))
    mod = importlib.util.module_from_spec(spec)
    sys.path.insert(0, os.path.join(PS3, "tools"))
    spec.loader.exec_module(mod)
    return mod.HEADER_PREAMBLE


def main():
    zig = zig_build.find_zig()
    failed = 0
    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, "ppu_recomp.h"), "w", newline="\n") as f:
            f.write(header())
        for name, defs in VARIANTS.items():
            exe = os.path.join(d, f"store_paths_{name}" + zig_build.exe_suffix())
            r = subprocess.run([zig, "c++", "-std=gnu++20", "-O1", "-fno-strict-aliasing", "-fwrapv", "-w", *defs,
                                "-I", d, os.path.join(ROOT, "tests", "native", "store_paths_test.cpp"), "-o", exe],
                               capture_output=True, text=True)
            if r.returncode:
                print(f"{name}: compile failed\n{r.stderr[-3000:]}")
                failed += 1
                continue
            r = subprocess.run([exe], capture_output=True, text=True)
            print(r.stdout.strip())
            failed += r.returncode != 0
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
