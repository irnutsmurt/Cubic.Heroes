#!/usr/bin/env python3
"""Where a frame's physics time goes, from a PS3_FTRACE timeline, without
double-counting overlapping tasks.

  tests/phys_breakdown.py <ftrace.txt> [<ftrace.txt> ...]

A Bullet step is a burst of collision runs (SPU image 4) and the solver runs
(image 5) after it, up to the next collision burst. Per frame (flip request to
flip request), by band of frame length, the mean of:
  steps     Bullet steps that started in the frame
  span      first task start to last task end, summed over its steps
  exec      the union of task runs inside those spans (parallel runs once);
            cd.exec / sv.exec the same for the collision and solver runs alone
  gaps      span - exec: no task running -- dispatch, task start-up and the
            PPU's turnaround between phases
  disp      mean wait from a task's dispatch to its start (us)
  ppu.wait  the main thread waiting for a queue entry (PS3_FT_QUEUE_WAIT),
            union: what physics actually costs the frame's critical thread
  wait.out  the part of ppu.wait outside the steps' spans
Needs task spans (kind 16) and queue waits (kind 17): runtime patches 0051
and 0056.
"""
import bisect
import sys

FLIP_REQ, SPU_TASK, QUEUE_WAIT = 9, 16, 17
CD, SOLVER = 4, 5
MAIN = 1
BURST_GAP = 300.0   # us: collision runs closer than this belong to one step


def union(spans):
    tot, cur = 0.0, None
    for a, b in sorted(spans):
        if cur is None or a > cur[1]:
            if cur:
                tot += cur[1] - cur[0]
            cur = [a, b]
        else:
            cur[1] = max(cur[1], b)
    return tot + (cur[1] - cur[0] if cur else 0.0)


def clip(spans, lo, hi):
    return [(max(a, lo), min(b, hi)) for a, b in spans if b > lo and a < hi]


for path in sys.argv[1:]:
    tasks, waits, flips = [], [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        f = line.split()
        if len(f) != 6 or not f[0].isdigit():
            continue
        k, who, a, b, t0, t1 = int(f[0]), int(f[1]), int(f[2], 16), int(f[3], 16), float(f[4]), float(f[5])
        if k == FLIP_REQ:
            flips.append(t0)
        elif k == SPU_TASK and a in (CD, SOLVER):
            tasks.append((t0, t1, a, b))
        elif k == QUEUE_WAIT and who == MAIN:
            waits.append((t0, t1))
    flips.sort(); tasks.sort(); waits.sort()

    # steps: a collision burst opens one; it runs to the next step's opening
    starts = []
    for t0, t1, img, _ in tasks:
        if img == CD and (not starts or t0 - starts[-1][1] > BURST_GAP):
            starts.append([t0, t0])
        if img == CD and starts:
            starts[-1][1] = t0
    steps = []
    for i, (s0, _) in enumerate(starts):
        nxt = starts[i + 1][0] if i + 1 < len(starts) else float("inf")
        runs = [(t0, t1, b, img) for t0, t1, img, b in tasks if s0 <= t0 < nxt]
        if runs:
            steps.append((s0, max(r[1] for r in runs), runs))

    n = len(flips) - 1
    per = [dict(steps=0, span=0.0, exec=0.0, cd=0.0, sv=0.0, disp=[], wait=0.0, wait_out=0.0) for _ in range(n)]
    for s0, s1, runs in steps:
        i = bisect.bisect_right(flips, s0) - 1
        if not 0 <= i < n:
            continue
        p = per[i]
        p["steps"] += 1
        p["span"] += s1 - s0
        p["exec"] += union([(a, b) for a, b, _, _ in runs])
        p["cd"] += union([(a, b) for a, b, _, img in runs if img == CD])
        p["sv"] += union([(a, b) for a, b, _, img in runs if img == SOLVER])
        p["disp"] += [d for _, _, d, _ in runs]
    spans = [(s0, s1) for s0, s1, _ in steps]
    for i in range(n):
        w = clip(waits, flips[i], flips[i + 1])
        per[i]["wait"] = union(w)
        inside = union([(max(a, c), min(b, d)) for a, b in w for c, d in clip(spans, flips[i], flips[i + 1])
                        if min(b, d) > max(a, c)])
        per[i]["wait_out"] = per[i]["wait"] - inside

    print(f"== {path}")
    print("frame ms   frames   steps   span ms   exec ms  cd.exec  sv.exec   gaps ms   disp us  ppu.wait ms  wait.out ms")
    for lo, hi in ((0, 18), (18, 25), (25, 34), (34, 50), (50, 1e9)):
        idx = [i for i in range(n) if lo <= (flips[i + 1] - flips[i]) / 1000 < hi]
        if not idx:
            continue
        m = lambda key: sum(per[i][key] for i in idx) / len(idx) / (1 if key == "steps" else 1000)
        d = [x for i in idx for x in per[i]["disp"]]
        print(f"{lo:3.0f}-{hi if hi < 1e8 else 999:3.0f}  {len(idx):7d}  {m('steps'):6.2f}  {m('span'):8.2f}  {m('exec'):8.2f}"
              f"  {m('cd'):7.2f}  {m('sv'):7.2f}"
              f"  {m('span') - m('exec'):8.2f}  {sum(d) / len(d) if d else 0:8.1f}  {m('wait'):11.2f}  {m('wait_out'):11.2f}")
    hist = {}
    for i in range(n):
        hist[per[i]["steps"]] = hist.get(per[i]["steps"], 0) + 1
    print("steps per frame:", dict(sorted(hist.items())))
