#!/usr/bin/env python3
"""Where a frame's time goes, from a PS3_FTRACE timeline, by frame length.

  tests/frame_costs.py <ftrace.txt> [<ftrace.txt> ...]

Frames run from one flip request to the next. Per band of frame lengths, the
mean per frame of:
  walker   RSX FIFO drain passes (CPU: decoding and recording the frame's draws)
  gpu      measured GPU execution of the draw batches (needs VK_GPU_TIME=1)
  presents frames the engine presented in it (more than 1: it caught up)
  draws    draws of those frames, passes the render passes they began
  barriers full barriers in the batches that completed in it
  w.flush  CPU waits on the GPU for a flush (a submit and wait)
  w.pres   CPU waits for the present's copy (frame pacing lives here too)
  w.infl   CPU waits because the previous batch is still on the GPU
  readbk   reading the presented frame back (centre pixel, frame dump)
  phys     Bullet SPU task CPU time (images 4 and 5)
  main.idle  the PPU main thread asleep or blocked (usleep, syscalls, HLE)
Spans are summed where they overlap the frame, so threads add up; compare a
band with the next to see what grows with a slow frame.
"""
import bisect
import sys

SYSCALL, USLEEP, DRAIN, FLIP_REQ, PRESENT, GPU_WAIT, READBACK, GPU_EXEC, HLE, SPU_TASK = 1, 2, 7, 9, 10, 12, 13, 14, 15, 16
MAIN = 1
COLS = ("walker", "gpu", "presents", "draws", "passes", "barriers", "w.flush", "w.pres", "w.infl", "readbk", "phys", "main.idle")

for path in sys.argv[1:]:
    recs, flips = [], []
    for line in (__import__("gzip").open(path, "rt", encoding="utf-8", errors="replace") if path.endswith(".gz")
                 else open(path, encoding="utf-8", errors="replace")):   # kept baselines are gzipped
        f = line.split()
        if len(f) != 6 or not f[0].isdigit():
            continue
        k, who, a, b, t0, t1 = int(f[0]), int(f[1]), int(f[2], 16), int(f[3], 16), float(f[4]), float(f[5])
        if k == FLIP_REQ:
            flips.append(t0)
        else:
            recs.append((k, who, a, b, t0, t1))
    flips.sort()
    acc = [dict.fromkeys(COLS, 0.0) for _ in flips]

    def add(i, key, v):
        if 0 <= i < len(flips) - 1:
            acc[i][key] += v

    for k, who, a, b, t0, t1 in recs:
        key = None
        if k == DRAIN: key = "walker"
        elif k == GPU_EXEC: key = "gpu"
        elif k == GPU_WAIT: key = ("w.flush", "w.pres", "w.infl")[a] if a < 3 else None
        elif k == READBACK: key = "readbk"
        elif k == SPU_TASK and a in (4, 5): key = "phys"
        elif who == MAIN and k in (SYSCALL, USLEEP, HLE): key = "main.idle"
        if k == GPU_EXEC:
            add(bisect.bisect_right(flips, t1) - 1, "barriers", b)
        if k == PRESENT and who == 2:                  # the engine's present: that frame's counts
            i = bisect.bisect_right(flips, t0) - 1
            add(i, "presents", 1); add(i, "draws", a); add(i, "passes", b)
            continue
        if not key:
            continue
        # split the span across the frames it overlaps
        i = max(bisect.bisect_right(flips, t0) - 1, 0)
        while i < len(flips) - 1 and flips[i] < t1:
            lo, hi = max(t0, flips[i]), min(t1, flips[i + 1])
            if hi > lo:
                add(i, key, (hi - lo) / 1000.0)
            i += 1

    print(f"== {path}")
    print("frame ms   frames " + " ".join(f"{c:>9s}" for c in COLS))
    for lo, hi in ((0, 18), (18, 25), (25, 34), (34, 50), (50, 1e9)):
        idx = [i for i in range(len(flips) - 1) if lo <= (flips[i + 1] - flips[i]) / 1000 < hi]
        if not idx:
            continue
        means = [sum(acc[i][c] for i in idx) / len(idx) for c in COLS]
        print(f"{lo:3.0f}-{hi if hi < 1e8 else 999:3.0f}  {len(idx):7d} " + " ".join(f"{m:9.2f}" for m in means))
