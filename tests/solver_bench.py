#!/usr/bin/env python3
"""The write-back's cost both ways on the same inputs: DOTHEROES_SOLVER_BENCH's
native runs set beside the lifted write-back of the same steps.

  tests/solver_bench.py <run dir> [...]      (each with bench.txt and ftrace.txt)

A run with the lifted write-back, DOTHEROES_PHYS_STATS=1, PS3_FTRACE and
DOTHEROES_SOLVER_BENCH=<run dir>/bench.txt (port/native/bullet_solver_task.cpp):
for each step, the native write-back was timed on the step's own inputs (one
task claiming every cell) just before the lifted tasks did the step. Here each
bench line is matched with the step's write-back in the trace (the command-6
runs that start within 2 ms after it): the phase's span (first run's start to
last run's end, all 10 rounds), round 1's slowest run (round 1 does the work;
rounds 2-10 rewrite the same values) and the CPU of all 30 runs. By contacts
in the step: steps, lifted phase / round 1 / CPU, native (us; medians).
The native write-back as the game runs it adds the PPU's issuing of 30 tasks
and their dispatch, which do not depend on the inputs: its phase in the
native A/B runs (~0.21 ms a step) is that.
"""
import bisect
import collections
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import solver_breakdown as sb
import walker_breakdown as wb

BINS = [(1, 50), (51, 100), (101, 200), (201, 400), (401, 99999)]


def main():
    rows = []
    for d in sys.argv[1:]:
        trace = next((os.path.join(d, n) for n in ("ftrace.txt", "ftrace.txt.gz") if os.path.exists(os.path.join(d, n))), None)
        bench = os.path.join(d, "bench.txt")
        if not trace or not os.path.exists(bench):
            continue
        rec = wb.load(trace)
        phases = []
        for cr, segs in sb.steps_of(rec):
            for g in segs:
                if g["cmd"] == 6:
                    runs = g["runs"]
                    first = min(r[0] for r in runs)
                    r1 = sorted(runs)[:3]
                    phases.append((first, max(r[1] for r in runs) - first, max(r[1] - r[0] for r in r1),
                                   sum(r[1] - r[0] for r in runs), len(runs)))
        ps = [p[0] for p in phases]
        n_b = n_m = 0
        for line in open(bench):
            if line.startswith("#"):
                continue
            t, contacts, cells, native = line.split()
            t, contacts, native = float(t), int(contacts), float(native)
            n_b += 1
            i = bisect.bisect_left(ps, t)
            if i < len(phases) and phases[i][0] - t < 2000:
                p = phases[i]
                rows.append(dict(contacts=contacts, span=p[1], round1=p[2], cpu=p[3], runs=p[4], native=native))
                n_m += 1
        print(f"{os.path.basename(d)}: {n_b} benched steps, {n_m} matched with their write-back in the trace")
    if not rows:
        return
    med = lambda v: statistics.median(v) if v else float("nan")
    print(f"\n{len(rows)} steps; write-back runs a step: {collections.Counter(r['runs'] for r in rows).most_common(3)}")
    print("contacts   steps   lifted: phase    round 1   CPU (30 runs)   native, one task   phase / native")
    for lo, hi in BINS:
        s = [r for r in rows if lo <= r["contacts"] <= hi]
        if not s:
            continue
        print(f"  {lo:>4}-{hi if hi < 99999 else '':<5} {len(s):5d}   {med([r['span'] for r in s]):9.0f} us  {med([r['round1'] for r in s]):6.0f} us"
              f"   {med([r['cpu'] for r in s]):9.0f} us     {med([r['native'] for r in s]):9.1f} us        "
              f"{med([r['span'] for r in s]) / max(med([r['native'] for r in s]), 1e-9):6.0f}x")
    print(f"  all       {len(rows):5d}   {med([r['span'] for r in rows]):9.0f} us  {med([r['round1'] for r in rows]):6.0f} us"
          f"   {med([r['cpu'] for r in rows]):9.0f} us     {med([r['native'] for r in rows]):9.1f} us")


if __name__ == "__main__":
    main()
