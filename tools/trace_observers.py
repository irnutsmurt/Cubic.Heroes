#!/usr/bin/env python3
"""A tracing lift's entry observers (M9.2g, docs/M92_SHADER_PLAN.md section 2h; development only): R7's hooks
(config/r7_native_hooks.json) and every lifted function in the given address ranges, written as an observers file for
tools/r8_lift.sh (R8_OBSERVERS) and, beside it, the list of the added functions for the build's tracer
(DOTHEROES_FN_TRACE_EXTRA: port/native/render/ppfx_recorder.cpp fn_trace -- an added function returns at once, its
body running as in play; DOTHEROES_FN_TRACE=FILE records each distinct guest call stack and, for the functions
DOTHEROES_FN_TRACE_ARGS lists, each distinct argument tuple). Addresses only.

    tools/trace_observers.py --functions FUNCTIONS.json --out OBSERVERS.json --list EXTRA.txt LO-HI [LO-HI ...]
e.g. the engine and PPFX: 006C0000-00720000 007497E0-007D3E38 (FUNCTIONS: build/gen/out/EBOOT.functions.fixed.json)
"""
import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def function_starts(path):
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    items = d.get("functions", d) if isinstance(d, dict) else d
    out = set()
    for it in items:
        a = it.get("start", it.get("addr")) if isinstance(it, dict) else it
        if isinstance(a, str):
            a = int(a, 16)
        if isinstance(a, int):
            out.add(a)
    return sorted(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--functions", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--list", required=True)
    ap.add_argument("ranges", nargs="+")
    a = ap.parse_args()
    with open(os.path.join(ROOT, "config", "r7_native_hooks.json"), encoding="utf-8") as f:
        hooks = json.load(f)
    hooked = {int(o["addr"], 16) for o in hooks["observers"]}
    rng = [tuple(int(x, 16) for x in r.split("-")) for r in a.ranges]
    extra = [f for f in function_starts(a.functions) if any(lo <= f < hi for lo, hi in rng) and f not in hooked]
    obs = dict(hooks)
    obs["_comment"] = ("A tracing lift (tools/trace_observers.py, development only): R7's hooks and the functions of "
                       + ", ".join(a.ranges) + ". Addresses only.")
    obs["observers"] = hooks["observers"] + [{"addr": "0x%08X" % f, "note": "traced"} for f in extra]
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obs, f, indent=1)
    with open(a.list, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join("%08X" % x for x in extra) + "\n")
    print(f"{len(extra)} functions added to {len(hooked)} hooks -> {a.out}, {a.list}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
