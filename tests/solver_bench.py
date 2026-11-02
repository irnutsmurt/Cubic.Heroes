#!/usr/bin/env python3
"""The write-back's and copy-back's cost both ways on the same inputs:
DOTHEROES_SOLVER_BENCH's native runs (compute only) set beside the lifted
commands' dispatched runs and phases in the same steps.

  tests/solver_bench.py <run dir> [...]      (each with bench.txt and ftrace.txt)

A run with the lifted commands, DOTHEROES_PHYS_STATS=1, PS3_FTRACE and
DOTHEROES_SOLVER_BENCH=<run dir>/bench.txt (port/native/bullet_solver_task.cpp):
just before the lifted tasks did each step, the native write-back was timed on
the step's own inputs as one task claiming every cell, and the native
copy-back on each copy-back task's own bodies; both then undone.
  write-back (6)  each bench line matched with the step's write-back in the
                  trace (the command-6 runs starting within 2 ms after it): the
                  phase's span (all 10 rounds), round 1's slowest run (round 1
                  does the work; 2-10 rewrite the same values), the CPU of the
                  30 runs; by contacts in the step.
  copy-back (5)   each bench line matched with the command-5 run of that task
                  (the first copy-back run starting within 2 ms after it): the
                  run as dispatched (start to end on its worker); by bodies in
                  the task.
Compute only, the native figures leave out what a native command still costs
in the game: the PPU creating its tasks and their dispatch, which do not
depend on the inputs (the native write-back's phase in the game, ~0.21 ms a
step, is that).
"""
import bisect
import collections
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import solver_breakdown as sb
import walker_breakdown as wb

WB_BINS = [(1, 50), (51, 100), (101, 200), (201, 400), (401, 99999)]
CB_BINS = [(1, 16), (17, 32), (33, 48), (49, 64)]


def med(v):
    return statistics.median(v) if v else float("nan")


def main():
    wb_rows, cb_rows = [], []
    for d in sys.argv[1:]:
        trace = next((os.path.join(d, n) for n in ("ftrace.txt", "ftrace.txt.gz") if os.path.exists(os.path.join(d, n))), None)
        bench = os.path.join(d, "bench.txt")
        if not trace or not os.path.exists(bench):
            continue
        rec = wb.load(trace)
        phases, cb_runs = [], []
        for cr, segs in sb.steps_of(rec):
            for g in segs:
                if g["cmd"] == 6:
                    runs = g["runs"]
                    first = min(r[0] for r in runs)
                    phases.append((first, max(r[1] for r in runs) - first, max(r[1] - r[0] for r in sorted(runs)[:3]),
                                   sum(r[1] - r[0] for r in runs), len(runs)))
                elif g["cmd"] == 5:
                    cb_runs += [(r[0], r[1] - r[0]) for r in g["runs"]]
        ps, cs = [p[0] for p in phases], sorted(cb_runs)
        cst = [c[0] for c in cs]
        n6 = m6 = n5 = m5 = 0
        for line in open(bench):
            if line.startswith("#"):
                continue
            f = line.split()
            cmd = int(f[0]) if len(f) == 5 else 6          # older files: write-back lines only, no command
            t, items, native = float(f[-4]), int(f[-3]), float(f[-1])
            if cmd == 6:
                n6 += 1
                i = bisect.bisect_left(ps, t)
                if i < len(phases) and phases[i][0] - t < 2000:
                    p = phases[i]
                    wb_rows.append(dict(contacts=items, span=p[1], round1=p[2], cpu=p[3], runs=p[4], native=native))
                    m6 += 1
            elif cmd == 5:
                n5 += 1
                i = bisect.bisect_left(cst, t)
                if i < len(cs) and cs[i][0] - t < 2000:
                    cb_rows.append(dict(bodies=items, run=cs[i][1], native=native))
                    m5 += 1
        print(f"{os.path.basename(d)}: write-back {n6} benched steps, {m6} matched in the trace; "
              f"copy-back {n5} benched tasks, {m5} matched")
    if wb_rows:
        print(f"\nwrite-back, {len(wb_rows)} steps (runs a step: {collections.Counter(r['runs'] for r in wb_rows).most_common(2)})")
        print("contacts   steps   lifted, dispatched: phase   round 1   CPU (30 runs)   native, compute only: one task")
        for lo, hi in WB_BINS + [(1, 99999)]:
            s = [r for r in wb_rows if lo <= r["contacts"] <= hi]
            if s:
                lab = "all" if (lo, hi) == (1, 99999) else f"{lo}-{hi if hi < 99999 else ''}"
                print(f"  {lab:<9} {len(s):5d}   {med([r['span'] for r in s]):9.0f} us  {med([r['round1'] for r in s]):6.0f} us"
                      f"   {med([r['cpu'] for r in s]):9.0f} us     {med([r['native'] for r in s]):9.1f} us")
    if cb_rows:
        print(f"\ncopy-back, {len(cb_rows)} tasks")
        print("bodies     tasks   lifted, dispatched: the task's run   native, compute only: the same bodies")
        for lo, hi in CB_BINS + [(1, 99999)]:
            s = [r for r in cb_rows if lo <= r["bodies"] <= hi]
            if s:
                lab = "all" if (lo, hi) == (1, 99999) else f"{lo}-{hi}"
                print(f"  {lab:<9} {len(s):5d}   {med([r['run'] for r in s]):9.1f} us                       "
                      f"{med([r['native'] for r in s]):9.2f} us")


if __name__ == "__main__":
    main()
