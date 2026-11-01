#!/usr/bin/env python3
"""Where a Bullet step's solver time goes, phase by phase, and what of it the
main thread waits for.

  tests/solver_breakdown.py <ftrace.txt[.gz]> [...] [--heavy N]

Needs a PS3_FTRACE timeline with DOTHEROES_PHYS_STATS=1 (collision workload,
kind 19; solver commands, kind 20) and the SPU task spans (kind 16) and queue
waits (kind 17). A step opens with its collision tasks (image 4); its solver
runs (image 5) then come as phases: the main thread fills each slot's
descriptor with a command, creates the three tasks, and waits for all three
before the next. Per phase: the command (1 set up bodies, 2 manifold setup,
3 constraint setup, 4 iterate, 5 copy bodies back, 6 manifold warm-start
write-back) and its item count, the slowest slot's run, the span from the
first run's start to the last's end, the gap before it (from the previous
phase's last end, or the collision's: the main thread's turnaround and the
dispatch), and the main thread's queue wait inside the span and the gap.
Totals per command, over all steps and over steps in frames with at least
--heavy collision pairs (default 121).
"""
import argparse
import bisect
import collections
import gzip
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import walker_breakdown as wb

SPU_TASK, QUEUE_WAIT, FLIP_REQ, PHYS_LOAD, PHYS_CMD = 16, 17, 9, 19, 20
CD, SOLVER, MAIN = 4, 5, 1
NAMES = {1: "setup bodies", 2: "manifold setup", 3: "constraint setup", 4: "iterate", 5: "copy bodies back",
         6: "warm-start write-back", 0: "?"}


def union(sp):
    tot, cur = 0.0, None
    for a, b in sorted(sp):
        if cur is None or a > cur[1]:
            if cur:
                tot += cur[1] - cur[0]
            cur = [a, b]
        else:
            cur[1] = max(cur[1], b)
    return tot + (cur[1] - cur[0] if cur else 0.0)


def clip(sp, lo, hi):
    return [(max(a, lo), min(b, hi)) for a, b in sp if b > lo and a < hi]


def steps_of(rec):
    """[(collision runs, [segment, ...])]: a segment = consecutive solver runs with one command,
    dict(cmd, count, runs=[(t0, t1, disp_us, slot)], iters=[[runs of one barrier round], ...])"""
    cd = sorted((t0, t1) for t0, t1, who, a, b in rec[SPU_TASK] if a == CD)
    sv = sorted((t0, t1, b) for t0, t1, who, a, b in rec[SPU_TASK] if a == SOLVER)
    marks = sorted((t0, a, b) for t0, t1, who, a, b in rec[PHYS_CMD] if who == SOLVER)
    mt = [m[0] for m in marks]
    used = set()

    def command(t0):   # the mark made at this run's start (just after it)
        i = bisect.bisect_left(mt, t0)
        while i < len(marks) and marks[i][0] <= t0 + 2000:
            if i not in used:
                used.add(i)
                return marks[i][1], marks[i][2] >> 8, marks[i][2] & 0xFF
            i += 1
        return 0, 0, -1
    starts = []
    for t0, t1 in cd:
        if not starts or t0 - starts[-1][1] > 300:
            starts.append([t0, t1])
        else:
            starts[-1][1] = max(starts[-1][1], t1)
    out = []
    for k, (s0, s1) in enumerate(starts):
        nxt = starts[k + 1][0] if k + 1 < len(starts) else float("inf")
        cruns = [r for r in cd if s0 <= r[0] < nxt]
        segs = []
        for t0, t1, disp in (r for r in sv if s0 <= r[0] < nxt):
            c, n, slot = command(t0)
            if not segs or segs[-1]["cmd"] != c:
                segs.append(dict(cmd=c, count=0, runs=[], iters=[]))
            g = segs[-1]
            g["count"] = max(g["count"], n)
            g["runs"].append((t0, t1, disp, slot))
            # barrier rounds: a run that starts after every run of the current round has ended
            if g["iters"] and t0 < max(r[1] for r in g["iters"][-1]) - 5:
                g["iters"][-1].append((t0, t1, disp, slot))
            else:
                g["iters"].append([(t0, t1, disp, slot)])
        out.append((cruns, segs))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("traces", nargs="+")
    ap.add_argument("--heavy", type=int, default=121)
    a = ap.parse_args()
    for sel_name in ("all steps", f"steps in frames with >= {a.heavy} collision pairs"):
        tot = collections.defaultdict(lambda: collections.Counter())
        nsteps, step_wait, step_span, coll_exec, coll_wait = 0, 0.0, 0.0, 0.0, 0.0
        phases_per_step = collections.Counter()
        for path in a.traces:
            rec = wb.load(path)
            waits = sorted((t0, t1) for t0, t1, who, x, y in rec[QUEUE_WAIT] if who == MAIN)
            heavy_windows = []
            if sel_name != "all steps":
                for f in wb.guest_frames(rec):
                    if f["pairs"] >= a.heavy:
                        heavy_windows.append((f["lo"], f["hi"]))
            hw0 = [w[0] for w in heavy_windows]
            for cruns, phases in steps_of(rec):
                if not cruns or not phases:
                    continue
                s0 = cruns[0][0]
                if sel_name != "all steps":
                    i = bisect.bisect_right(hw0, s0) - 1
                    if i < 0 or not (heavy_windows[i][0] <= s0 < heavy_windows[i][1]):
                        continue
                nsteps += 1
                end = max(r[1] for g in phases for r in g["runs"])
                step_span += end - s0
                step_wait += union(clip(waits, s0, end))
                ce = max(t1 for t0, t1 in cruns)
                coll_exec += union(cruns)
                coll_wait += union(clip(waits, s0, ce))
                prev_end = ce
                n_by_cmd = collections.Counter()
                for g in phases:
                    first = min(r[0] for r in g["runs"])
                    last = max(r[1] for r in g["runs"])
                    t = tot[g["cmd"] or 0]
                    t["segments"] += 1
                    n_by_cmd[g["cmd"] or 0] += len(g["iters"])
                    t["rounds"] += len(g["iters"])
                    t["runs"] += len(g["runs"])
                    t["items"] += g["count"]
                    t["span"] += last - first
                    t["gap"] += max(0.0, first - prev_end)
                    t["sum_exec"] += sum(r[1] - r[0] for r in g["runs"])
                    t["slowest"] += sum(max(r[1] - r[0] for r in it) for it in g["iters"])
                    t["round_gaps"] += sum(max(0.0, min(r[0] for r in b) - max(r[1] for r in a_))
                                           for a_, b in zip(g["iters"], g["iters"][1:]))
                    t["disp"] += sum(r[2] for r in g["runs"])
                    t["wait_span"] += union(clip(waits, first, last))
                    t["wait_gap"] += union(clip(waits, prev_end, first)) if first > prev_end else 0.0
                    prev_end = last
                phases_per_step[tuple(sorted(n_by_cmd.items()))] += 1
        print(f"== {sel_name}: {nsteps} steps over {len(a.traces)} trace(s)")
        if not nsteps:
            continue
        n = nsteps
        print(f"   per step: span {step_span / n / 1000:.2f} ms (collision start -> last solver end), main thread waiting "
              f"{step_wait / n / 1000:.2f}; collision exec {coll_exec / n / 1000:.2f}, main thread waiting during it "
              f"{coll_wait / n / 1000:.2f}")
        common = phases_per_step.most_common(3)
        print("   rounds per step by command, most common: " +
              "; ".join(f"{dict(k)} x{v}" for k, v in common))
        print(f"   {'command':24s} {'rounds':>7s} {'runs':>6s} {'items':>6s} {'span':>7s} {'slowest':>8s} {'r.gaps':>7s} "
              f"{'gap':>6s} {'all runs':>8s} {'disp':>5s} {'wait in':>8s} {'wait gap':>8s}   (ms per step; disp us per run)")
        allw = 0.0
        for cmd in sorted(tot):
            t = tot[cmd]
            allw += t["wait_span"] + t["wait_gap"]
            print(f"   {str(cmd) + ' ' + NAMES.get(cmd, '?'):24s} {t['rounds'] / n:7.2f} {t['runs'] / n:6.2f} "
                  f"{t['items'] / max(t['segments'], 1):6.0f} {t['span'] / n / 1000:7.3f} {t['slowest'] / n / 1000:8.3f} "
                  f"{t['round_gaps'] / n / 1000:7.3f} {t['gap'] / n / 1000:6.3f} {t['sum_exec'] / n / 1000:8.3f} "
                  f"{t['disp'] / max(t['runs'], 1):5.1f} {t['wait_span'] / n / 1000:8.3f} {t['wait_gap'] / n / 1000:8.3f}")
        print(f"   main thread waiting during solver segments and the gaps before them: {allw / n / 1000:.2f} ms per step")


if __name__ == "__main__":
    main()
