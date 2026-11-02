#!/usr/bin/env python3
"""The heavy-fight benchmark's results (tests/scripts/fight_bench.drive): each run
measured over its fight segment only, classified by how the segment ended, and
arms compared at matched contact points.

  tests/fight_bench.py <run dir> [...] [--arm-env DOTHEROES_NATIVE_SOLVER]

Needs PS3_FTRACE covering the fight, DOTHEROES_DEV_STATE=1 (the game's state,
kind 23), DOTHEROES_DEV_KEYS=1 (the drive's marks, kind 22), DOTHEROES_PHYS_STATS=1
(workload, kinds 19 and 20). The segment: from the first physics step after the
drive's first mark (the hero confirmed in the river's area) to its second mark
-- or, earlier, the first state sample with the hero dead (life 0) or out of
that area. A run is
  complete   the segment ran to the second mark
  died       life reached 0 in it      departed  the hero left the area
  unfinished the run ended before the drive's second mark (measured to its end)
  no-debris  no physics step after the first mark
  no-start   no first mark (the route did not reach the area; the drive stopped)
Per run (all measured ones; arms pool the complete and unfinished): the segment's length,
physics steps and their contact points, can't-die top-ups (DOTHEROES_DEV_CANT_DIE;
such runs are reported as such), guest frames (flip request to flip request) and
present-call completion intervals (not display timing), catch-up (physics frames
with 2+ steps), the main thread's ring and physics waits per frame. Per arm, the
runs pooled whose hero stayed alive and in the area (complete, unfinished), and
physics frames binned by contact points.
"""
import argparse
import bisect
import collections
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import solver_breakdown as sb
import walker_breakdown as wb

QUEUE_WAIT, PHYS_LOAD, RING_WAIT, MARK, STATE = 17, 19, 21, 22, 23
MAIN, RIVER = 1, 18
FB = [(1, 100), (101, 250), (251, 99999)]


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))] if v else float("nan")


def mean(v):
    return sum(v) / len(v) if v else float("nan")


def trace_of(d):
    for n in ("ftrace.txt", "ftrace.txt.gz"):
        if os.path.exists(os.path.join(d, n)):
            return os.path.join(d, n)


def overlap(spans, starts, longest, lo, hi):
    i, j = bisect.bisect_left(starts, lo - longest), bisect.bisect_left(starts, hi)
    return sum(wb.clip(x[0], x[1], lo, hi) for x in spans[i:j]) / 1000


def run(d):
    sw = json.load(open(os.path.join(d, "run.json"))).get("switches", {})
    rec = wb.load(trace_of(d))
    marks = [x[0] for x in rec[MARK] if x[2] == 3]
    states = [(t0, who & 0xFF, (who >> 8) & 0xFF, who >> 16) for t0, t1, who, a, b in rec[STATE]]
    loads = rec[PHYS_LOAD]
    out = dict(dir=d, sw=sw, kind=None)
    if not marks:
        out["kind"] = "no-start"
        return out
    m1 = marks[0]
    m2 = marks[1] if len(marks) > 1 else max((s[0] for s in states), default=m1)
    end, kind = m2, "complete" if len(marks) > 1 else "unfinished"
    for t, life, area, top in states:
        if m1 <= t <= m2 and (life == 0 or area != RIVER):
            end, kind = t, "died" if life == 0 else "departed"
            break
    first = next((x[0] for x in loads if x[0] >= m1), None)
    if first is None or first >= end:
        out["kind"] = "no-debris"
        return out
    lo, hi = first, end
    tops = [s[3] for s in states if lo <= s[0] <= hi]
    out.update(kind=kind, lo=lo, hi=hi, secs=(hi - lo) / 1e6, topups=(tops[-1] - tops[0]) if tops else 0)
    # steps (as tests/solver_ab.py): collision start to the last solver run, with the step's workload
    lt = [x[0] for x in loads]
    steps = []
    for cr, segs in sb.steps_of(rec):
        if not cr or not segs or not lo <= cr[0][0] < hi:
            continue
        s0 = cr[0][0]
        e = max(r[1] for g in segs for r in g["runs"])
        i0, i1 = bisect.bisect_left(lt, s0), bisect.bisect_left(lt, max(r[1] for r in cr) + 50)
        steps.append(dict(t=s0, span=e - s0, points=sum(loads[i][4] for i in range(i0, i1))))
    out["steps"] = steps
    st = [s["t"] for s in steps]
    ring = sorted(x for x in rec[RING_WAIT] if x[2] == MAIN and (x[3] & 0xF) in (1, 2))
    phys = sorted(x for x in rec[QUEUE_WAIT] if x[2] == MAIN)
    idx = lambda v: (v, [x[0] for x in v], max((x[1] - x[0] for x in v), default=0.0))
    ri, pi = idx(ring), idx(phys)
    frames = []
    for f in wb.guest_frames(rec):
        if not lo <= f["lo"] < hi:
            continue
        a, b = bisect.bisect_left(st, f["lo"]), bisect.bisect_left(st, f["hi"])
        frames.append(dict(ppu=f["ppu"], shown=f["shown"], steps=b - a, points=sum(s["points"] for s in steps[a:b]),
                           ring=overlap(*ri, f["lo"], f["hi"]), physw=overlap(*pi, f["lo"], f["hi"]),
                           stp=f["ppu"] + f["lag"] + f["lat"], lag=f["lag"]))
    out["frames"] = frames
    return out


def summary(r):
    s, f = r["steps"], r["frames"]
    ph = [x for x in f if x["steps"]]
    n = max(len(f), 1)
    return (f"{r['secs']:5.1f} s, {len(s)} steps ({mean([x['points'] for x in s]):.0f} points, {mean([x['span'] for x in s]) / 1000:.2f} ms "
            f"each), {len(f)} frames: guest mean {mean([x['ppu'] for x in f]):.2f} ms, over 18 ms {100 * sum(x['ppu'] > 18 for x in f) / n:.1f}%, "
            f"over 20 ms {100 * sum(x['ppu'] > 20 for x in f) / n:.1f}%, p99 {pct([x['ppu'] for x in f], .99):.1f}; present over 25 ms "
            f"{100 * sum(x['shown'] > 25 for x in f) / n:.2f}%; 2+ steps {100 * sum(x['steps'] >= 2 for x in ph) / max(len(ph), 1):.0f}% of "
            f"{len(ph)} physics frames; per frame ring wait {mean([x['ring'] for x in f]):.2f} ms, physics wait "
            f"{mean([x['physw'] for x in f]):.2f}; frame start -> present returned median {pct([x['stp'] for x in f], .5):.1f}, "
            f"p90 {pct([x['stp'] for x in f], .9):.1f} ms (walker behind at the flip: median {pct([x['lag'] for x in f], .5):.1f}, "
            f"p90 {pct([x['lag'] for x in f], .9):.1f}); top-ups {r['topups']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--arm-env", default="DOTHEROES_NATIVE_SOLVER")
    a = ap.parse_args()
    arms = collections.defaultdict(list)
    for d in a.runs:
        if not trace_of(d) or not os.path.exists(os.path.join(d, "run.json")):
            continue
        r = run(d)
        arm = r["sw"].get(a.arm_env, "unset")
        cd = r["sw"].get("DOTHEROES_DEV_CANT_DIE", "0")
        tag = f"[{a.arm_env}={arm}{', cant-die' if cd not in ('', '0') else ''}]"
        if r["kind"] in ("complete", "died", "departed", "unfinished"):
            print(f"{os.path.basename(d)} {tag} {r['kind']}: {summary(r)}")
        else:
            print(f"{os.path.basename(d)} {tag} {r['kind']}: not measured")
        if r["kind"] in ("complete", "unfinished"):   # alive and in the area to the segment's end
            arms[arm].append(r)
    names = sorted(arms)
    for arm in names:
        rs = arms[arm]
        pooled = dict(secs=sum(r["secs"] for r in rs), steps=[s for r in rs for s in r["steps"]],
                      frames=[f for r in rs for f in r["frames"]], topups=sum(r["topups"] for r in rs))
        print(f"\n== {a.arm_env}={arm}: {len(rs)} runs pooled (complete or unfinished): {summary(pooled)}")
    if len(names) == 2:
        A, B = names
        print(f"\nphysics frames by contact points in the frame ({A} -> {B}): frames, steps each, guest mean, over 18 ms, "
              f"present over 25 ms, ring wait, physics wait")
        for lo, hi in FB:
            x = [f for r in arms[A] for f in r["frames"] if f["steps"] and lo <= f["points"] <= hi]
            y = [f for r in arms[B] for f in r["frames"] if f["steps"] and lo <= f["points"] <= hi]
            o = lambda v, k, t: 100 * sum(f[k] > t for f in v) / max(len(v), 1)
            m = lambda v, k: mean([f[k] for f in v])
            print(f"   {lo:>4}-{hi if hi < 99999 else '':<4} {len(x):5d} / {len(y):5d}   steps {m(x, 'steps'):.2f} / {m(y, 'steps'):.2f}   "
                  f"guest {m(x, 'ppu'):.2f} -> {m(y, 'ppu'):.2f}   over 18 {o(x, 'ppu', 18):.1f}% -> {o(y, 'ppu', 18):.1f}%   "
                  f"over 25 {o(x, 'shown', 25):.1f}% -> {o(y, 'shown', 25):.1f}%   ring {m(x, 'ring'):.2f} -> {m(y, 'ring'):.2f}   "
                  f"physics {m(x, 'physw'):.2f} -> {m(y, 'physw'):.2f}")


if __name__ == "__main__":
    main()
