#!/usr/bin/env python3
"""How much the physics workload and its cost vary from fight to fight, by
collision mode: is a comparison of a change smaller than that variation
possible with these runs?

  tests/workload_variation.py <run dir> [...] [--group-env DOTHEROES_NATIVE_CD] [--min-steps 50]

Per run (from its PS3_FTRACE window, DOTHEROES_PHYS_STATS=1): physics steps,
physics frames and steps each (and the share with 2+: catch-up), awake bodies
(the collision census, summed over the step's collision tasks), collision
pairs and contact points a step, and what a step costs: collision (first
collision task's start to the last one's end) and solver (the five commands'
spans), in ms. Runs with fewer than --min-steps steps had no fight in the
window: listed apart. Then per group (the switch's value): each quantity's
mean over the group's runs, its spread between runs (standard deviation and
coefficient of variation of the per-run means) and the spread within runs
(the per-step coefficient of variation, averaged over runs).
"""
import argparse
import bisect
import collections
import json
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import solver_breakdown as sb
import walker_breakdown as wb

KEYS = [("bodies", "awake bodies"), ("pairs", "pairs"), ("points", "contact points"),
        ("coll", "collision ms"), ("solver", "solver ms"), ("total", "step ms")]


def trace_of(d):
    for n in ("ftrace.txt", "ftrace.txt.gz"):
        if os.path.exists(os.path.join(d, n)):
            return os.path.join(d, n)


def run(d):
    rec = wb.load(trace_of(d))
    load = rec[19]
    lt = [x[0] for x in load]
    steps = []
    for cr, segs in sb.steps_of(rec):
        if not cr or not segs:
            continue
        s0, ce = cr[0][0], max(r[1] for r in cr)
        i0, i1 = bisect.bisect_left(lt, s0), bisect.bisect_left(lt, ce + 50)
        solver = sum(max(r[1] for r in g["runs"]) - min(r[0] for r in g["runs"]) for g in segs)
        end = max(r[1] for g in segs for r in g["runs"])
        steps.append(dict(t=s0, bodies=sum(load[i][2] for i in range(i0, i1)), pairs=sum(load[i][3] for i in range(i0, i1)),
                          points=sum(load[i][4] for i in range(i0, i1)), coll=(ce - s0) / 1000, solver=solver / 1000,
                          total=(end - s0) / 1000))
    st = [s["t"] for s in steps]
    per_frame = []
    for f in wb.guest_frames(rec):
        n = bisect.bisect_left(st, f["hi"]) - bisect.bisect_left(st, f["lo"])
        if n:
            per_frame.append(n)
    return steps, per_frame


def cv(v):
    return statistics.pstdev(v) / statistics.mean(v) if len(v) > 1 and statistics.mean(v) else float("nan")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--group-env", default="DOTHEROES_NATIVE_CD")
    ap.add_argument("--min-steps", type=int, default=50)
    a = ap.parse_args()
    groups = collections.defaultdict(list)
    for d in a.runs:
        if not trace_of(d):
            continue
        sw = json.load(open(os.path.join(d, "run.json"))).get("switches", {})
        g = sw.get(a.group_env, "unset")
        steps, frames = run(d)
        name = os.path.basename(d)
        if len(steps) < a.min_steps:
            print(f"no fight in the window (left out): {name} [{a.group_env}={g}], {len(steps)} steps")
            continue
        groups[g].append((name, steps, frames))
    for g, rs in sorted(groups.items()):
        print(f"\n== {a.group_env}={g}: {len(rs)} fight runs")
        print(f"   {'run':44s} {'steps':>5s} {'ph.fr':>5s} {'st/fr':>5s} {'2+':>5s} " +
              " ".join(f"{lab[:12]:>12s}" for _, lab in KEYS))
        means = collections.defaultdict(list)
        within = collections.defaultdict(list)
        for name, steps, frames in rs:
            spf = statistics.mean(frames) if frames else 0
            two = 100 * sum(n >= 2 for n in frames) / max(len(frames), 1)
            means["steps/frame"].append(spf)
            means["2+ share"].append(two)
            row = []
            for k, _ in KEYS:
                v = [s[k] for s in steps]
                means[k].append(statistics.mean(v))
                within[k].append(cv(v))
                row.append(f"{statistics.mean(v):12.2f}")
            print(f"   {name[:44]:44s} {len(steps):5d} {len(frames):5d} {spf:5.2f} {two:4.0f}% " + " ".join(row))
        print(f"   between runs (per-run means): mean, SD, CV  |  within runs (per-step CV, run average)")
        for k, lab in [("steps/frame", "steps a physics frame"), ("2+ share", "% physics frames with 2+")] + KEYS:
            v = means[k]
            w = f"  |  {statistics.mean(within[k]):.2f}" if k in within else ""
            print(f"     {lab:26s} {statistics.mean(v):8.2f} {statistics.pstdev(v):7.2f} {cv(v):5.2f}{w}")


if __name__ == "__main__":
    main()
