#!/usr/bin/env python3
"""How characters move at different frame rates, from actors.bin
(DOTHEROES_DEV_ACTOR_LOG=1, port/native/dev_state.cpp).

  tests/actor_steps.py <run dir> [...]

Per run and per class (vtable): the frame-to-frame steps of that class's
characters while they move -- how many, the share that are multiples of 0.01
on both axes (the hero's were: the game truncates its step to 0.01, see
port/game_patches.cpp), and the speed while moving (distance over time, over
runs of steps less than 0.1 s apart). A class whose steps are multiples of
0.01 is rounded as the hero's was, and slows as the frame rate rises unless
the rounding carries; one whose speed is the same at every rate moves by
time. Enemies wander, so a speed is a mean over what each run happened to
see: compare classes with many steps.
"""
import collections
import json
import math
import os
import struct
import sys

REC = struct.Struct("<QIIfff")


def load(d):
    out = collections.defaultdict(list)   # ea -> [(t, vt, x, y, z)]
    with open(os.path.join(d, "actors.bin"), "rb") as f:
        data = f.read()
    for i in range(0, len(data) - REC.size + 1, REC.size):
        t, ea, vt, x, y, z = REC.unpack_from(data, i)
        out[ea].append((t / 1e9, vt, x, y, z))
    return out


def quantized(v):
    return abs(v * 100 - round(v * 100)) < 0.01


def stats(actors):
    by_vt = collections.defaultdict(lambda: dict(steps=0, quant=0, dist=0.0, time=0.0, actors=set()))
    for ea, s in actors.items():
        for (t0, vt0, x0, _, z0), (t1, vt1, x1, _, z1) in zip(s, s[1:]):
            dt = t1 - t0
            if vt0 != vt1 or dt <= 0 or dt > 0.1:
                continue
            dx, dz = x1 - x0, z1 - z0
            d = math.hypot(dx, dz)
            if d == 0 or d > 2.0:   # standing, or a jump (respawn, warp)
                continue
            e = by_vt[vt1]
            e["steps"] += 1
            e["quant"] += quantized(dx) and quantized(dz)
            e["dist"] += d
            e["time"] += dt
            e["actors"].add(ea)
    return by_vt


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    table = {}
    for d in sys.argv[1:]:
        sw = json.load(open(os.path.join(d, "run.json"))).get("switches", {})
        label = f"cap {sw.get('DOTHEROES_FPS', '60')}, vblank {sw.get('DIAG_VBLANK_MS', '16.7')} ms"
        st = stats(load(d))
        table[d] = st
        print(f"== {os.path.basename(d)} ({label})")
        for vt, e in sorted(st.items(), key=lambda kv: -kv[1]["steps"]):
            if e["steps"] < 20:
                continue
            print(f"   class 0x{vt:08X}: {len(e['actors'])} characters, {e['steps']} steps, "
                  f"{100 * e['quant'] / e['steps']:.0f}% multiples of 0.01, "
                  f"{e['dist'] / e['time']:.2f} units a second while moving")
    return 0


if __name__ == "__main__":
    sys.exit(main())
