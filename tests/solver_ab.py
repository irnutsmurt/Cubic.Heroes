#!/usr/bin/env python3
"""A/B of a physics change: task timings and end-to-end frames, at matched workload.

  tests/solver_ab.py <run dir> [...] [--arm-env DOTHEROES_NATIVE_SOLVER] [--min-steps 50]

Runs are grouped by an environment switch in their run.json. Runs with fewer
than --min-steps physics steps in their trace window had no fight there: listed
apart and left out. Needs PS3_FTRACE with DOTHEROES_PHYS_STATS=1 (collision
workload, kind 19; solver commands, kind 20). Per arm, over the fight runs:
  steps     contact points and pairs a step; each solver command's span a step
            (first run's start to last run's end), and the main thread's wait
  matched   steps binned by contact points: step span, main-thread wait, and
            the span of the command under test (--command, default 6)
  tasks     the command's task run times (median, p90)
  catch-up  physics frames, steps each, the share with 2+ steps, and after a
            guest frame of 25 ms or more
  frames    present-call completion intervals (not display timing): over 25 /
            33 ms, p99; guest frames over 18 / 20 ms, p99; frame start ->
            present returned, median / p90; physics frames binned by points
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

NAMES = {1: "setup", 2: "manifold", 4: "iterate", 5: "copy back", 6: "write-back"}
PB = [(1, 50), (51, 100), (101, 150), (151, 250), (251, 99999)]
FB = [(1, 100), (101, 250), (251, 99999)]


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))] if v else float("nan")


def trace_of(d):
    for n in ("ftrace.txt", "ftrace.txt.gz"):
        if os.path.exists(os.path.join(d, n)):
            return os.path.join(d, n)


def run_stats(d, command):
    rec = wb.load(trace_of(d))
    load = rec[19]
    lt = [x[0] for x in load]
    waits = sorted((t0, t1) for t0, t1, who, a, b in rec[17] if who == 1)
    steps = []
    for cr, segs in sb.steps_of(rec):
        if not cr or not segs:
            continue
        s0 = cr[0][0]
        end = max(r[1] for g in segs for r in g["runs"])
        i0, i1 = bisect.bisect_left(lt, s0), bisect.bisect_left(lt, max(r[1] for r in cr) + 50)
        span = {}
        runs = []
        for g in segs:
            span[g["cmd"]] = span.get(g["cmd"], 0.0) + max(r[1] for r in g["runs"]) - min(r[0] for r in g["runs"])
            if g["cmd"] == command:
                runs += [r[1] - r[0] for r in g["runs"]]
        steps.append(dict(t=s0, span=end - s0, wait=sb.union(sb.clip(waits, s0, end)),
                          points=sum(load[i][4] for i in range(i0, i1)), pairs=sum(load[i][3] for i in range(i0, i1)),
                          cmd_span=span, runs=runs))
    st = [s["t"] for s in steps]
    frames = []
    for f in wb.guest_frames(rec):
        a, b = bisect.bisect_left(st, f["lo"]), bisect.bisect_left(st, f["hi"])
        frames.append(dict(shown=f["shown"], ppu=f["ppu"], start_to_present=f["ppu"] + f["lag"] + f["lat"],
                           steps=b - a, points=sum(s["points"] for s in steps[a:b])))
    return steps, frames


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--arm-env", default="DOTHEROES_NATIVE_SOLVER")
    ap.add_argument("--command", type=int, default=6)
    ap.add_argument("--min-steps", type=int, default=50)
    a = ap.parse_args()
    arms = collections.defaultdict(list)
    for d in a.runs:
        if not trace_of(d):
            continue
        sw = json.load(open(os.path.join(d, "run.json"))).get("switches", {})
        steps, frames = run_stats(d, a.command)
        arm = sw.get(a.arm_env, "unset")
        if len(steps) < a.min_steps:
            print(f"no fight in the window (left out): {os.path.basename(d)} [{a.arm_env}={arm}], {len(steps)} physics steps")
            continue
        arms[arm].append((d, steps, frames))
    names = sorted(arms)
    m = lambda v: sum(v) / len(v) if v else float("nan")
    for arm in names:
        rs = arms[arm]
        steps = [s for _, ss, _ in rs for s in ss]
        frames = [f for _, _, ff in rs for f in ff]
        print(f"\n== {a.arm_env}={arm}: {len(rs)} fight runs, {len(steps)} steps, {len(frames)} guest frames")
        for d, ss, ff in rs:
            print(f"   {os.path.basename(d)}: {len(ss)} steps, {m([s['points'] for s in ss]):.0f} points and "
                  f"{m([s['pairs'] for s in ss]):.0f} pairs a step")
        print(f"   per step: span {m([s['span'] for s in steps]) / 1000:.2f} ms, main thread waiting {m([s['wait'] for s in steps]) / 1000:.2f}; "
              + ", ".join(f"{NAMES[c]} {m([s['cmd_span'].get(c, 0) for s in steps]) / 1000:.2f}" for c in (1, 2, 4, 5, 6)))
        runs = [r for s in steps for r in s["runs"]]
        print(f"   {NAMES.get(a.command, a.command)} task runs: {len(runs)}, median {pct(runs, .5):.1f} us, p90 {pct(runs, .9):.1f}")
        ph = [f for f in frames if f["steps"]]
        prev_long = [frames[i] for i in range(1, len(frames)) if frames[i]["steps"] and frames[i - 1]["ppu"] >= 25]
        print(f"   catch-up: {len(ph)} physics frames, {m([f['steps'] for f in ph]):.2f} steps each, "
              f"{100 * sum(f['steps'] >= 2 for f in ph) / max(len(ph), 1):.1f}% with 2+; after a guest frame of 25 ms+: "
              f"{len(prev_long)} frames, {100 * sum(f['steps'] >= 2 for f in prev_long) / max(len(prev_long), 1):.0f}% with 2+")
        sh, g = [f["shown"] for f in frames], [f["ppu"] for f in frames]
        n = max(len(frames), 1)
        print(f"   present interval: over 25 ms {100 * sum(x > 25 for x in sh) / n:.2f}%, over 33 ms "
              f"{100 * sum(x > 33.4 for x in sh) / n:.2f}%, p99 {pct(sh, .99):.1f}; guest frame over 18 ms "
              f"{100 * sum(x > 18 for x in g) / n:.2f}%, over 20 ms {100 * sum(x > 20 for x in g) / n:.2f}%, p99 {pct(g, .99):.1f}; "
              f"frame start -> present median {pct([f['start_to_present'] for f in frames], .5):.2f}, "
              f"p90 {pct([f['start_to_present'] for f in frames], .9):.2f}")
    if len(names) == 2:
        A, B = names
        sa = [s for _, ss, _ in arms[A] for s in ss]
        sb_ = [s for _, ss, _ in arms[B] for s in ss]
        print(f"\nsteps by contact points ({A} -> {B}): steps, span, main-thread wait, {NAMES.get(a.command, a.command)} span (ms)")
        for lo, hi in PB:
            x = [s for s in sa if lo <= s["points"] <= hi]
            y = [s for s in sb_ if lo <= s["points"] <= hi]
            f = lambda v, k: m([s[k] for s in v]) / 1000
            c = lambda v: m([s["cmd_span"].get(a.command, 0) for s in v]) / 1000
            print(f"   {lo:>4}-{hi if hi < 99999 else '':<4} {len(x):5d} / {len(y):5d}   span {f(x, 'span'):.2f} -> {f(y, 'span'):.2f}   "
                  f"wait {f(x, 'wait'):.2f} -> {f(y, 'wait'):.2f}   {NAMES.get(a.command, a.command)} {c(x):.2f} -> {c(y):.2f}")
        fa = [f for _, _, ff in arms[A] for f in ff if f["steps"]]
        fb = [f for _, _, ff in arms[B] for f in ff if f["steps"]]
        print(f"physics frames by contact points in the frame ({A} -> {B}): frames, steps each, guest frame, present over 25 ms")
        for lo, hi in FB:
            x = [f for f in fa if lo <= f["points"] <= hi]
            y = [f for f in fb if lo <= f["points"] <= hi]
            o = lambda v: 100 * sum(f["shown"] > 25 for f in v) / max(len(v), 1)
            print(f"   {lo:>4}-{hi if hi < 99999 else '':<4} {len(x):5d} / {len(y):5d}   steps {m([f['steps'] for f in x]):.2f} / "
                  f"{m([f['steps'] for f in y]):.2f}   guest {m([f['ppu'] for f in x]):.2f} -> {m([f['ppu'] for f in y]):.2f}   "
                  f"over 25 ms {o(x):.1f}% -> {o(y):.1f}%")


if __name__ == "__main__":
    main()
