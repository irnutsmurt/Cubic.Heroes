#!/usr/bin/env python3
"""Physics cost per guest frame of a PS3_FTRACE timeline, by frame length.

  tests/phys_frames.py <ftrace.txt>

Frames run from one flip request to the next. For each band of frame
lengths: how many Bullet collision runs (SPU image 4, BulletCD) and solver
runs (image 5, BulletSolver) the frames held, their CPU time, the solver's
wall time (first start to last end), the mean wait from dispatch to start,
and the main thread's syscall/HLE time. Then how many collision runs each
frame held: two per physics step, so 4, 6, 8 ... are frames that ran 2, 3,
4 ... steps to catch up (Bullet's fixed-step catch-up after a slow frame).
Needs a trace with async task spans (kind 16, runtime/spu/spu_workload.c).
"""
import bisect
import collections
import sys

SYSCALL, USLEEP, FLIP_REQ, HLE, SPU_TASK = 1, 2, 9, 15, 16
CD, SOLVER = 4, 5
MAIN = 1   # the PPU main thread's id in the trace

recs, flips = [], []
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    f = line.split()
    if len(f) != 6 or not f[0].isdigit():
        continue
    k, who, a, b, t0, t1 = int(f[0]), int(f[1]), int(f[2], 16), int(f[3], 16), float(f[4]), float(f[5])
    if k == FLIP_REQ:
        flips.append(t0)
    else:
        recs.append((k, who, a, b, t0, t1))
flips.sort()
frames = [dict(cd=[], sv=[], main=0.0) for _ in flips]
for k, who, a, b, t0, t1 in recs:
    i = bisect.bisect_right(flips, t0) - 1
    if i < 0 or i >= len(flips) - 1:
        continue
    if k == SPU_TASK and a == CD:
        frames[i]["cd"].append((t0, t1, b))
    elif k == SPU_TASK and a == SOLVER:
        frames[i]["sv"].append((t0, t1, b))
    elif who == MAIN and k in (SYSCALL, USLEEP, HLE):
        frames[i]["main"] += t1 - t0

rows = []
for i in range(len(flips) - 1):
    cd, sv = frames[i]["cd"], frames[i]["sv"]
    runs = cd + sv
    rows.append(((flips[i + 1] - flips[i]) / 1000, len(cd), sum(t1 - t0 for t0, t1, _ in cd) / 1000,
                 len(sv), sum(t1 - t0 for t0, t1, _ in sv) / 1000,
                 (max(t1 for _, t1, _ in sv) - min(t0 for t0, _, _ in sv)) / 1000 if sv else 0.0,
                 sum(b for *_, b in runs) / len(runs) if runs else 0.0, frames[i]["main"] / 1000))

print("frame ms  frames  CD/frame  CD ms  solver/frame  solver CPU ms  solver wall ms  start us  main sys ms")
for lo, hi in ((0, 18), (18, 25), (25, 34), (34, 50), (50, 1e9)):
    band = [r for r in rows if lo <= r[0] < hi]
    if not band:
        continue
    m = lambda j: sum(r[j] for r in band) / len(band)
    print(f"{lo:3.0f}-{hi if hi < 1e8 else 999:3.0f}  {len(band):6d}  {m(1):8.2f}  {m(2):5.2f}  {m(3):12.1f}"
          f"  {m(4):13.2f}  {m(5):14.2f}  {m(6):8.1f}  {m(7):11.2f}")
print("collision runs per frame:", dict(sorted(collections.Counter(r[1] for r in rows).items())))
