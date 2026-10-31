#!/usr/bin/env python3
"""Frame-time distribution of a PS3_FTRACE timeline, three ways:
  request  the guest asking for a flip (what the game paces itself by)
  rsx      the FIFO walker reaching that flip
  present  the host showing the frame (the end of the present span; Vulkan)

  tests/frame_times.py <ftrace.txt> [<ftrace.txt> ...]

For each: fps, the median, 95th and 99th percentile and longest interval,
and how many missed a 60 Hz slot (over 1.5 vblanks, 25 ms) -- the numbers
behind "a solid 60".
"""
import statistics
import sys

FLIP_FIFO, FLIP_REQ, PRESENT = 8, 9, 10

for path in sys.argv[1:]:
    times = {"request": [], "rsx": [], "present": []}
    for line in (__import__("gzip").open(path, "rt", encoding="utf-8", errors="replace") if path.endswith(".gz")
                 else open(path, encoding="utf-8", errors="replace")):   # kept baselines are gzipped
        f = line.split()
        if len(f) != 6 or not f[0].isdigit():
            continue
        k = int(f[0])
        if k == FLIP_REQ: times["request"].append(float(f[4]))
        elif k == FLIP_FIFO: times["rsx"].append(float(f[4]))
        elif k == PRESENT and f[1] == "1": times["present"].append(float(f[5]))   # who 1: the window's present; who 2 marks the engine's
    print(path)
    for what, t in times.items():
        t.sort()
        d = sorted((b - a) / 1000.0 for a, b in zip(t, t[1:]))
        if not d:
            continue
        pct = lambda p: d[min(len(d) - 1, int(p / 100.0 * len(d)))]
        span = (t[-1] - t[0]) / 1e6
        print(f"  {what:8s} {len(d)} in {span:.1f} s = {len(d) / span:.1f}/s; median {statistics.median(d):.1f} ms, "
              f"p95 {pct(95):.1f}, p99 {pct(99):.1f}, max {d[-1]:.1f}; over 25 ms: "
              f"{sum(1 for x in d if x > 25.0)}, over 33 ms: {sum(1 for x in d if x > 33.4)}")
