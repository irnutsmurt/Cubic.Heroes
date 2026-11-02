#!/usr/bin/env python3
"""Frame-rate independence: runs of tests/scripts/fps_check.drive at different
frame rates (DOTHEROES_FPS, DIAG_VBLANK_MS) compared -- the same inputs, the
game's state measured. The first run is the reference (the original 30 fps).

  tests/fps_check.py <run dir> <run dir> [...]

Each run needs DOTHEROES_DEV_STATE=1, DOTHEROES_DEV_KEYS=1, DOTHEROES_DEV_MEMSNAP=1,
DOTHEROES_DEV_PLAYER_LOG=1, DOTHEROES_PHYS_STATS=1 and PS3_FTRACE covering the
drive (port/native/dev_state.cpp). Between the drive's marks (1-2 standing still,
2-3 walking down 1 s, 3-4 back up, 4-5 to the grass and cutting it, 5-6 standing
while the debris settles), per run:
  frames     guest frames (flip requests) a second
  walk       the camera target's travel (it follows the hero) walking down, up
  physics    steps (collision tasks) a second while there is debris, the most
             bodies, and when the steps stop after the cuts
  clocks     static memory (below 0x03000000) between the two snapshots taken
             10 s apart standing still (tests/native/find_state.cpp clocks, kept
             in the run as clocks.txt): each word's change a second
  player     the player's object (player.bin) in each segment: each word's total
             change, as an integer (steps under 2^16) and as a float, and how
             long each value of a word with few values lasts (the hero's states)
Then each run against the first: a quantity that follows time stays the same;
one that follows frames changes by the ratio of the frame rates ("frames"); one
that follows the vblank clock, by the ratio of the vblank rates ("vblank").
"""
import bisect
import collections
import json
import os
import re
import statistics
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import solver_breakdown as sb
import walker_breakdown as wb

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIND_STATE = os.path.join(ROOT, "build", "native-test", "find_state.exe")
FLIP_REQ, PHYS_LOAD, MARK, STATE = 9, 19, 22, 23
SEGS = [("still", 0, 1), ("walk", 1, 3), ("grass", 3, 4), ("settle", 4, 5)]
TOL = 0.12   # a ratio within 12% of the expected one


def trace_of(d):
    for n in ("ftrace.txt", "ftrace.txt.gz"):
        if os.path.exists(os.path.join(d, n)):
            return os.path.join(d, n)


def f32(bits):
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def clocks_of(d):
    """{(ea, kind): (before, after)}, the snapshots' start times (s), from clocks.txt (made once)"""
    log = open(os.path.join(d, "stderr.log"), encoding="utf-8", errors="replace").read()
    snaps = re.findall(r"\[memsnap\] (memsnap_\d+\.bin) at ([\d.]+)-([\d.]+) s", log)
    if len(snaps) < 2:
        return None, None
    out = os.path.join(d, "clocks.txt")
    if not os.path.exists(out):
        with open(out, "w") as f:
            subprocess.run([FIND_STATE, "clocks", "03000000", os.path.join(d, snaps[0][0]), os.path.join(d, snaps[1][0])],
                           stdout=f, check=True)
    words = {}
    for line in open(out):
        ea, kind, a, b = line.split()
        words[(int(ea, 16), kind)] = (float(a), float(b)) if kind == "f32" else (int(a), int(b))
    return words, [(float(s[1]), float(s[2])) for s in snaps[:2]]


def player_of(d, lo_us, hi_us):
    """the player log's samples in [lo, hi] (us): [(t_us, ea, u32 tuple, f32 tuple)]"""
    path = os.path.join(d, "player.bin")
    out = []
    if not os.path.exists(path):
        return out
    with open(path, "rb") as f:
        while True:
            h = f.read(16)
            if len(h) < 16:
                break
            t, ea, n = struct.unpack("<QII", h)
            b = f.read(n)
            if len(b) < n:
                break
            tu = t / 1e3
            if tu < lo_us:
                continue
            if tu > hi_us:
                break
            out.append((tu, ea, struct.unpack(">%dI" % (n // 4), b), struct.unpack(">%df" % (n // 4), b)))
    return out


def player_stats(samples):
    """per word: u32 total change (steps under 2^16), f32 total change, and the states' durations"""
    if len(samples) < 2:
        return {}
    nw = len(samples[0][2])
    u_adv, f_adv, f_moves = [0] * nw, [0.0] * nw, [0] * nw
    for (t0, e0, u0, g0), (t1, e1, u1, g1) in zip(samples, samples[1:]):
        if e0 != e1:
            continue
        for w in range(nw):
            a, b = u0[w], u1[w]
            if a != b:
                if abs(b - a) < 65536:
                    u_adv[w] += abs(b - a)
                x, y = g0[w], g1[w]
                if abs(x) < 1e6 and abs(y) < 1e6 and x == x and y == y:
                    f_adv[w] += abs(y - x)
                    f_moves[w] += 1
    states = {}
    for w in range(nw):
        vals = [s[2][w] for s in samples]
        if len(set(vals)) > 16:
            continue
        changes = [(samples[i][0], vals[i]) for i in range(1, len(vals)) if vals[i] != vals[i - 1]]
        if len(changes) < 3:
            continue
        dur = collections.defaultdict(list)
        for (ta, va), (tb, _) in zip(changes, changes[1:]):
            dur[va].append((tb - ta) / 1e3)
        states[w] = dur
    return dict(u=u_adv, f=f_adv, fm=f_moves, states=states)


def run(d):
    sw = json.load(open(os.path.join(d, "run.json"))).get("switches", {})
    fps_set = sw.get("DOTHEROES_FPS", "60")
    vb = float(sw.get("DIAG_VBLANK_MS") or 1000 / 60)
    r = dict(dir=d, name=os.path.basename(d), label=f"cap {fps_set}, vblank {1000 / vb:.0f} Hz", vblank_hz=1000 / vb)
    tr = trace_of(d)
    if not tr:
        r["error"] = "no frame trace"
        return r
    rec = wb.load(tr)
    marks = [t0 for t0, t1, who, a, b in rec[MARK] if who == 3]
    if len(marks) < 6:
        r["error"] = f"{len(marks)} of the drive's 6 marks"
        return r
    r["marks"] = marks
    reqs = [x[0] for x in rec[FLIP_REQ]]
    r["fps"] = {}
    for name, a, b in SEGS:
        lo, hi = marks[a], marks[b]
        r["fps"][name] = (bisect.bisect_left(reqs, hi) - bisect.bisect_left(reqs, lo)) / ((hi - lo) / 1e6)
    st = [(x[0], f32(x[3]), f32(x[4])) for x in rec[STATE]]
    stt = [s[0] for s in st]

    def at(t):   # the last state sample at or before t
        return st[max(0, bisect.bisect_right(stt, t) - 1)]
    p1, p2, p3 = at(marks[1]), at(marks[2]), at(marks[3])
    r["walk"] = dict(down_z=p2[2] - p1[2], down_x=p2[1] - p1[1], up_z=p3[2] - p2[2], up_x=p3[1] - p2[1])
    # steps (tests/solver_breakdown.py: a step's collision runs, then its solver runs -- a step
    # has a collision task per 64 pairs) and bodies (DOTHEROES_PHYS_STATS, per collision task)
    steps = sorted(cr[0][0] for cr, segs in sb.steps_of(rec) if cr)
    loads = [(x[0], x[2]) for x in rec[PHYS_LOAD]]
    cuts = [t for t in steps if marks[3] <= t < marks[5]]
    if cuts:
        first, last = cuts[0], cuts[-1]
        r["phys"] = dict(steps=len(cuts), active_s=(last - first) / 1e6,
                         rate=(len(cuts) - 1) / ((last - first) / 1e6) if last > first else 0.0,
                         bodies=max((x[1] for x in loads if first <= x[0] <= last + 2000), default=0),
                         end_after_mark5_s=(last - marks[4]) / 1e6,
                         before=sum(1 for t in steps if marks[0] <= t < marks[3]))
    words, snap_t = clocks_of(d)
    if words:
        dt = snap_t[1][0] - snap_t[0][0]
        frames = bisect.bisect_left(reqs, snap_t[1][0] * 1e6) - bisect.bisect_left(reqs, snap_t[0][0] * 1e6)
        r["clocks"] = dict(words=words, dt=dt, frames=frames, copy_ms=[(b - a) * 1e3 for a, b in snap_t])
    r["player"] = {}
    for name, a, b in SEGS[:3]:
        r["player"][name] = player_stats(player_of(d, marks[a], marks[b]))
    return r


def classify(ratio, fr, vr):
    if ratio != ratio or ratio <= 0:
        return "other"
    if abs(ratio - 1) <= TOL:
        return "time" if abs(fr - 1) > TOL else "time/frames"
    if abs(ratio / fr - 1) <= TOL:
        return "frames" if abs(vr / fr - 1) > TOL else "frames/vblank"
    if abs(ratio / vr - 1) <= TOL:
        return "vblank"
    return "other"


def show_run(r):
    print(f"\n== {r['name']}  ({r['label']})")
    if "error" in r:
        print("   not measured:", r["error"])
        return
    print("   frames a second: " + ", ".join(f"{k} {v:.1f}" for k, v in r["fps"].items()))
    w = r["walk"]
    print(f"   walk: down 1 s z {w['down_z']:+.2f} (x {w['down_x']:+.2f}), back up 1 s z {w['up_z']:+.2f} (x {w['up_x']:+.2f})")
    p = r.get("phys")
    if p:
        print(f"   physics: {p['steps']} steps over {p['active_s']:.1f} s ({p['rate']:.1f} a second), up to {p['bodies']} bodies, "
              f"the last {p['end_after_mark5_s']:+.1f} s after the cuts' end; {p['before']} steps before the grass")
    else:
        print("   physics: no steps around the cuts")
    c = r.get("clocks")
    if c:
        print(f"   snapshots {c['dt']:.2f} s apart, {c['frames']} frames ({c['frames'] / c['dt']:.1f} a second), copies "
              f"{c['copy_ms'][0]:.0f} and {c['copy_ms'][1]:.0f} ms; {len(c['words'])} words changed")


def compare(ref, r):
    print(f"\n== {r['name']} against {ref['name']}  ({r['label']} vs {ref['label']})")
    if "error" in r or "error" in ref:
        print("   not compared")
        return
    vr = r["vblank_hz"] / ref["vblank_hz"]
    w0, w1 = ref["walk"], r["walk"]
    for k in ("down_z", "up_z"):
        if abs(w0[k]) > 0.1:
            print(f"   walk {k}: {w1[k]:+.2f} vs {w0[k]:+.2f}  (x{w1[k] / w0[k]:.3f})")
    p0, p1 = ref.get("phys"), r.get("phys")
    if p0 and p1:
        fr = r["fps"]["grass"] / ref["fps"]["grass"]
        print(f"   physics steps a second: {p1['rate']:.1f} vs {p0['rate']:.1f} (x{p1['rate'] / max(p0['rate'], 1e-9):.2f}; "
              f"frames x{fr:.2f}); debris active {p1['active_s']:.1f} vs {p0['active_s']:.1f} s; most bodies {p1['bodies']} vs {p0['bodies']}")
    c0, c1 = ref.get("clocks"), r.get("clocks")
    if c0 and c1:
        fr = (c1["frames"] / c1["dt"]) / (c0["frames"] / c0["dt"])
        print(f"   clocks (static memory, 10 s standing still): frames x{fr:.2f}, vblank x{vr:.2f}")
        cls = collections.defaultdict(list)
        for key, (a0, b0) in c0["words"].items():
            if key not in c1["words"]:
                continue
            a1, b1 = c1["words"][key]
            d0, d1 = b0 - a0, b1 - a1
            if key[1] == "u32" and (abs(d0) < 30 or abs(d1) < 1):
                continue
            if key[1] == "f32" and (abs(d0) < 0.05 or abs(d1) < 1e-6 or (d0 > 0) != (d1 > 0)):
                continue
            ratio = (d1 / c1["dt"]) / (d0 / c0["dt"])
            cls[classify(ratio, fr, vr)].append((key, d0, d1, ratio, a0, b0))
        print("   " + ", ".join(f"{k} {len(v)}" for k, v in sorted(cls.items())))
        for k in ("frames", "frames/vblank", "vblank", "time"):
            v = sorted(cls.get(k, []), key=lambda x: -abs(x[1] / max(abs(x[4]), 1)))
            for (ea, kind), d0, d1, ratio, a0, b0 in v[:25 if k != "time" else 10]:
                print(f"     {k:13s} {ea:08X} {kind} {a0:.6g} -> {b0:.6g} ({d0:+.6g}); here {d1:+.6g} (x{ratio:.2f})")
    for seg in ("still", "walk", "grass"):
        s0, s1 = ref["player"].get(seg), r["player"].get(seg)
        if not s0 or not s1:
            continue
        fr = r["fps"][seg] / ref["fps"][seg]
        cls = collections.defaultdict(list)
        for w in range(len(s0["u"])):
            if s0["u"][w] >= 30 and s1["u"][w] > 0:
                cls[classify(s1["u"][w] / s0["u"][w], fr, vr)].append((w * 4, "u32", s0["u"][w], s1["u"][w]))
            if s0["f"][w] > 1e-3 and s0["fm"][w] >= 10 and s1["f"][w] > 0:
                cls[classify(s1["f"][w] / s0["f"][w], fr, vr)].append((w * 4, "f32", s0["f"][w], s1["f"][w]))
        print(f"   player, {seg} (frames x{fr:.2f}; total change per word, here vs reference): "
              + ", ".join(f"{k} {len(v)}" for k, v in sorted(cls.items())))
        for k in ("frames", "frames/vblank", "vblank", "other"):
            for off, kind, a, b in cls.get(k, [])[:20]:
                print(f"     {k:13s} +{off:04X} {kind} {a:.6g} -> {b:.6g} (x{b / a:.2f})")
        rows = []
        for w, dur in s0["states"].items():
            if w not in s1["states"]:
                continue
            for v, ds in dur.items():
                ds1 = s1["states"][w].get(v)
                if len(ds) >= 2 and ds1 and len(ds1) >= 2:
                    m0, m1 = statistics.median(ds), statistics.median(ds1)
                    if m0 >= 100:   # 3 frames at 30 fps: shorter ones are quantised
                        rows.append((w * 4, v, len(ds), m0, len(ds1), m1, classify(m1 / m0, 1 / fr, 1 / vr)))
        if rows:
            print(f"   player states, {seg} (median duration of each value, ms; frame-counted ones shrink x{1 / fr:.2f}): "
                  + ", ".join(f"{k} {n}" for k, n in sorted(collections.Counter(x[6] for x in rows).items())))
            for off, v, n0, m0, n1, m1, k in rows:
                print(f"     +{off:04X} = {v:<10d} {n0:3d}x {m0:7.0f} ms   here {n1:3d}x {m1:7.0f} ms (x{m1 / m0:.2f})  {k}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    runs = [run(d.rstrip("/\\")) for d in sys.argv[1:]]
    for r in runs:
        show_run(r)
    for r in runs[1:]:
        compare(runs[0], r)
    return 0


if __name__ == "__main__":
    sys.exit(main())
