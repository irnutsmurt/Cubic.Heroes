#!/usr/bin/env python3
"""The characters at different frame rates, segment by segment (M11.1b; tests/scripts/fps_actors.drive).

  tests/fps_actors.py <run dir> <run dir> [...]          (the first run is the reference)

Each run needs DOTHEROES_DEV_ACTOR_LOG=1 (actors.bin: the characters within 40 units of the hero, every 4 ms, on
the trace's clock), DOTHEROES_DEV_KEYS=1 (the drive's segments carry their marks' times: segments.jsonl) and
DIAG_FRAMES (frames.log: the frame rate in each segment). Per segment and per class (vtable), over the characters
that moved in it:
  speed      in 0.25 s windows in which the character moved (the distance over the time its steps cover): the
             median and the 90th percentile, units a second
  travel     the class's whole path in the segment, units a second of the segment
  new        characters that appeared in the segment (no record in the 5 s before it) -- a created enemy, its
             shots -- a minute; a frame-counted attack shows as more shots at a higher rate
An enemy segment (every enemy is one class, 0x00FE4500) is measured over its new characters only -- the created
type and what it throws -- apart from the map's own. Then each run against the first: a quantity that follows time
keeps its ratio near 1, a frame-counted one follows the frame rate's ratio. A class is SUSPECT when its median
speed's (or, for many new characters, their rate's) ratio is outside 1 +- TOL in the direction of the frame rate's
ratio (enemies wander and choose by chance: a single run's ratio is noisy, so a suspect is looked at, not judged).
"""
import bisect
import collections
import json
import math
import os
import statistics
import struct
import sys

REC = struct.Struct("<QIIfff")
WIN = 0.25
TOL = 0.35
ENEMY = 0x00FE4500


def load_actors(d):
    out = collections.defaultdict(list)   # ea -> [(t s, vt, x, z)]
    with open(os.path.join(d, "actors.bin"), "rb") as f:
        data = f.read()
    for i in range(0, len(data) - REC.size + 1, REC.size):
        t, ea, vt, x, _y, z = REC.unpack_from(data, i)
        out[ea].append((t / 1e9, vt, x, z))
    return out


def load_segments(d):
    segs = []
    try:
        with open(os.path.join(d, "segments.jsonl"), encoding="utf-8") as f:
            for ln in f:
                s = json.loads(ln)
                if s.get("t0") is not None and s.get("t1") is not None:
                    segs.append(s)
    except OSError:
        pass
    return segs


def load_frames(d):
    ts = []
    try:
        with open(os.path.join(d, "frames.log"), encoding="utf-8", errors="replace") as f:
            for ln in f:
                if ln.startswith("f "):
                    ts.append(int(ln.split()[1]) / 1e6)
    except OSError:
        pass
    ts.sort()
    return ts


def measure(actors, t0, t1, only_new_class=None):
    """{vt: dict(windows=[speeds], path, moved=set(ea), new=n)} over [t0, t1]"""
    per = collections.defaultdict(lambda: dict(windows=[], path=0.0, moved=set(), new=0))
    for ea, recs in actors.items():
        ts = [r[0] for r in recs]
        i0, i1 = bisect.bisect_left(ts, t0), bisect.bisect_right(ts, t1)
        if i1 <= i0:
            continue
        new = bisect.bisect_left(ts, t0 - 5.0) == i0   # no record in the 5 s before the segment
        seg = recs[i0:i1]
        by_vt = collections.defaultdict(list)
        for r in seg:
            by_vt[r[1]].append(r)
        for vt, rs in by_vt.items():
            if only_new_class is not None and vt == only_new_class and not new:
                continue
            e = per[vt]
            e["new"] += new
            win = collections.defaultdict(lambda: [0.0, 0.0])   # window -> [distance, time its steps cover]
            for (ta, _, xa, za), (tb, _, xb, zb) in zip(rs, rs[1:]):
                dt, dd = tb - ta, math.hypot(xb - xa, zb - za)
                if dt <= 0 or dt > 0.1 or dd > 2.0:   # a gap, or a jump (created, moved by a warp)
                    continue
                e["path"] += dd
                w = win[int((tb - t0) / WIN)]
                w[0] += dd
                w[1] += dt
            moving = [d / t for d, t in win.values() if d > 0.02 and t >= WIN / 2]
            if moving:
                e["moved"].add(ea)
                e["windows"] += moving
    return per


def summary(per, secs):
    out = {}
    for vt, e in per.items():
        w = sorted(e["windows"])
        if len(w) < 8 and e["new"] < 3:
            continue
        out[vt] = dict(n=len(e["moved"]), win=len(w), med=statistics.median(w) if w else 0.0,
                       p90=w[min(len(w) - 1, int(0.9 * len(w)))] if w else 0.0, travel=e["path"] / secs,
                       new=e["new"] * 60.0 / secs)
    return out


def run(d):
    with open(os.path.join(d, "run.json"), encoding="utf-8") as f:
        sw = json.load(f).get("switches", {})
    actors, frames = load_actors(d), load_frames(d)
    r = dict(dir=d, fps_set=sw.get("DOTHEROES_FPS", "60"), segs={})
    for s in load_segments(d):
        t0, t1 = s["t0"], s["t1"]
        secs = max(t1 - t0, 1e-3)
        nf = bisect.bisect_left(frames, t1) - bisect.bisect_left(frames, t0)
        enemy = s["name"].startswith("enemy-")
        r["segs"][s["name"]] = dict(secs=secs, fps=nf / secs if frames else None, ok=s.get("ok"),
                                    cls=summary(measure(actors, t0, t1, ENEMY if enemy else None), secs))
    return r


def ratio(a, b):
    return a / b if b else (float("inf") if a else 1.0)


def main():
    dirs = [a for a in sys.argv[1:] if not a.startswith("-")]
    if len(dirs) < 2:
        print(__doc__)
        return 2
    runs = [run(d) for d in dirs]
    ref = runs[0]
    for r in runs:
        fl = [s["fps"] for s in r["segs"].values() if s["fps"]]
        print(f"== {os.path.basename(r['dir'])}: cap {r['fps_set']}, {len(r['segs'])} segments, "
              f"{statistics.median(fl) if fl else 0:.1f} fps (median of the segments)")
    suspects = []
    names = list(ref["segs"])
    for name in names:
        rs = ref["segs"][name]
        print(f"\n-- {name}: {rs['secs']:.1f} s")
        for vt in sorted(rs["cls"], key=lambda v: -rs["cls"][v]["win"]):
            a = rs["cls"][vt]
            print(f"   0x{vt:08X} ref {ref['fps_set']:>4}: {a['n']} moving, {a['win']} windows, speed median "
                  f"{a['med']:.2f} p90 {a['p90']:.2f}, travel {a['travel']:.2f}/s, new {a['new']:.1f}/min")
            for r in runs[1:]:
                s = r["segs"].get(name)
                if not s or vt not in s["cls"]:
                    print(f"      {r['fps_set']:>4}: not seen")
                    continue
                b = s["cls"][vt]
                fr = ratio(s["fps"] or 0, rs["fps"] or 0)
                rm, rn = ratio(b["med"], a["med"]), ratio(b["new"], a["new"])
                flag = ""
                if a["win"] >= 20 and b["win"] >= 20 and fr > 1.3 and rm > 1 + TOL:
                    flag = "  SUSPECT (speed follows the frame rate)"
                elif a["new"] * rs["secs"] / 60 >= 6 and fr > 1.3 and rn > 1 + TOL:
                    flag = "  SUSPECT (new characters follow the frame rate)"
                if flag:
                    suspects.append((name, vt, r["fps_set"]))
                print(f"      {r['fps_set']:>4}: frames x{fr:.2f}; {b['n']} moving, {b['win']} windows, speed "
                      f"median {b['med']:.2f} (x{rm:.2f}) p90 {b['p90']:.2f} (x{ratio(b['p90'], a['p90']):.2f}), "
                      f"travel x{ratio(b['travel'], a['travel']):.2f}, new {b['new']:.1f}/min (x{rn:.2f}){flag}")
    print(f"\n{len(suspects)} suspect(s)")
    for name, vt, fps in suspects:
        print(f"   {name} class 0x{vt:08X} at cap {fps}")
    return 1 if suspects else 0


if __name__ == "__main__":
    sys.exit(main())
