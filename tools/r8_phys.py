#!/usr/bin/env python3
"""R8: the physics tasks' handshake in a run's frame trace, a frame at a time
(docs/NATIVE_R8.md section 6.1).

  tools/r8_phys.py <run dir> [<run dir> ...] [--heavy N]

From ftrace.txt (PS3_FTRACE): the guest frames (flip requests, kind 9) in
the trace window; for every frame and for the heavy-debris frames (a
collision task's load with N or more awake bodies in the frame, kind 19,
default 48):
  - the physics tasks a frame, by image (4 collision, 5 solver; kind 16 for
    both the SPURS pool and R8's native jobs), their dispatch-to-start
    latency and their run time;
  - the waiting thread's waits for a task (kind 17): from the first check
    that found nothing to the one that took a completion; with R8's direct
    issue, the part of it the waiting thread spent running jobs itself
    (useful work) apart from the polling.
"""
import bisect, os, statistics as st, sys


def load(run):
    f = os.path.join(run, "ftrace.txt")
    if not os.path.exists(f):
        f = os.path.join(run, "run", "ftrace.txt")
    flips, tasks, waits, loads = [], [], [], []
    for ln in open(f, encoding="utf-8", errors="replace"):
        x = ln.split()
        if len(x) != 6 or not x[0].isdigit():
            continue
        k, t0, t1 = int(x[0]), float(x[4]), float(x[5])
        if k == 9:
            flips.append(t0)
        elif k == 16:
            tasks.append((t0, t1, int(x[2], 16), int(x[3], 16)))
        elif k == 17:
            waits.append((t0, t1, int(x[3], 16), int(x[2], 16)))
        elif k == 19:
            loads.append((t0, int(x[1])))
    flips.sort()
    return flips, tasks, waits, loads


def frame_of(flips, t):
    i = bisect.bisect_right(flips, t) - 1
    return i if 0 <= i < len(flips) - 1 else None


def report(run, heavy_at):
    flips, tasks, waits, loads = load(run)
    if len(flips) < 10:
        print(f"{run}: no frames"); return
    nf = len(flips) - 1
    heavy = set()
    for t, bodies in loads:
        i = frame_of(flips, t)
        if i is not None and bodies >= heavy_at:
            heavy.add(i)
    sets = {"all": set(range(nf)), "heavy": heavy}
    print(f"{run}: {nf} frames, {len(heavy)} heavy (>= {heavy_at} awake bodies)")
    for name, fr in sets.items():
        if not fr:
            continue
        n = len(fr)
        line = [f"  {name:5s}"]
        for img, label in ((4, "collision"), (5, "solver")):
            v = [(lat, t1 - t0) for t0, t1, im, lat in tasks if im == img and frame_of(flips, t0) in fr]
            if v:
                lat = [a for a, b in v]; run_us = [b for a, b in v]
                line.append(f"{label} {len(v) / n:.1f}/f lat p50 {st.median(lat):.1f} us run p50 {st.median(run_us):.1f} us "
                            f"({sum(run_us) / n / 1000:.3f} ms/f)")
        w = [(t1 - t0, a) for t0, t1, polls, a in waits if frame_of(flips, t0) in fr]
        # R8's direct issue: a = the microseconds of the wait the waiting
        # thread spent running jobs itself (the runtime's SPURS pop: a queue
        # address, never below 0x10000 -- not a time)
        ran = sum(a for d, a in w if a < 0x10000)
        tot = sum(d for d, a in w)
        line.append(f"waits {len(w) / n:.1f}/f {tot / n / 1000:.3f} ms/f, of it running jobs {ran / n / 1000:.3f}, "
                    f"polling {(tot - ran) / n / 1000:.3f}" + (f"; p50 {st.median([d for d, a in w]):.1f} us" if w else ""))
        print("; ".join(line))


if __name__ == "__main__":
    args = sys.argv[1:]
    heavy_at = 48
    if "--heavy" in args:
        i = args.index("--heavy"); heavy_at = int(args[i + 1]); del args[i:i + 2]
    for r in args:
        report(r, heavy_at)
