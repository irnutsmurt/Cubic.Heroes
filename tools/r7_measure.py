#!/usr/bin/env python3
"""R7's measurement (docs/NATIVE_R7.md section 8): R6's per-segment frame times
(tools/r6_measure.py: guest flips and window presents inside the drive's
segments of a kind -- rate, p50/p90/p99/p99.9/max, intervals over 25 ms) and
the threads' costs, plus two views R6 did not separate:

  debris       the frame trace's flips (kind 9) split by the physics load of
               their interval: the most awake bodies any collision task saw
               (kind 19, DOTHEROES_PHYS_STATS=1); "heavy" = the intervals at
               or above --bodies (default 48), "light" the rest
  transitions  the drive's route segments (walks that cross an area: its
               load), whole: p99, p99.9, max, intervals over 25 and 50 ms

  busy         the game thread's busy time a guest frame from the frame trace alone
               (flip request to flip request, less the main thread's waits): builds without a walker

  tools/r7_measure.py <fight|explore|debris|transitions|busy> <tag dir> [...] [--bodies N]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r6_measure as m6  # noqa: E402


def debris(run, heavy_at):
    f = os.path.join(run, "ftrace.txt")
    if not os.path.exists(f):
        return None
    flips, phys = [], []
    for line in open(f, encoding="utf-8", errors="replace"):
        x = line.split()
        if len(x) != 6 or not x[0].isdigit():
            continue
        k = int(x[0])
        if k == 9:
            flips.append(float(x[4]))
        elif k == 19:
            phys.append((float(x[4]), int(x[1])))
    if len(flips) < 10 or not phys:
        return None
    flips.sort(); phys.sort()
    heavy, light, j, most = [], [], 0, []
    for i in range(1, len(flips)):
        a, b = flips[i - 1], flips[i]
        while j < len(phys) and phys[j][0] < a:
            j += 1
        bodies, k = 0, j
        while k < len(phys) and phys[k][0] < b:
            bodies = max(bodies, phys[k][1]); k += 1
        most.append(bodies)
        (heavy if bodies >= heavy_at else light).append((b - a) / 1000.0)
    return sorted(heavy), sorted(light), sorted(most)


def busy(run):
    """The game thread's busy time a guest frame from the frame trace alone
    (no walker needed): flip request to flip request (kind 9), less the main
    thread's waits in it (syscalls, usleeps, HLE calls: kinds 1, 2, 15 on
    thread 1) -- what walker_breakdown.py's 'busy' is, for builds without a
    walker. Also the native renderer's GPU time a frame (kind 14 spans)."""
    import bisect
    f = os.path.join(run, "ftrace.txt")
    if not os.path.exists(f):
        return None
    reqs, idle, gpu = [], [], 0.0
    for ln in open(f, encoding="utf-8", errors="replace"):
        x = ln.split()
        if len(x) != 6 or not x[0].isdigit():
            continue
        k, who, t0, t1 = int(x[0]), int(x[1]), float(x[4]), float(x[5])
        if k == 9:
            reqs.append(t0)
        elif k in (1, 2, 15) and who == 1:
            idle.append((t0, t1))
        elif k == 14:
            gpu += t1 - t0
    if len(reqs) < 10:
        return None
    reqs.sort(); idle.sort()
    starts = [i[0] for i in idle]
    longest = max((b - a for a, b in idle), default=0.0)
    frames, busy_sum = 0, 0.0
    for i in range(1, len(reqs)):
        lo, hi = reqs[i - 1], reqs[i]
        j = bisect.bisect_left(starts, lo - longest)
        w = 0.0
        while j < len(idle) and idle[j][0] < hi:
            a, b = idle[j]
            w += max(0.0, min(b, hi) - max(a, lo))
            j += 1
        busy_sum += (hi - lo) - w
        frames += 1
    span = (reqs[-1] - reqs[0]) / 1000.0
    return {"guest": span / frames, "busy": busy_sum / 1000.0 / frames, "gpu": gpu / 1000.0 / frames}


def line(label, iv, span_s=None):
    if len(iv) < 5:
        return "%s: %d intervals" % (label, len(iv))
    rate = (len(iv) / span_s) if span_s else 1000.0 / (sum(iv) / len(iv))
    return "%s %d intervals %.1f/s p50 %.2f p90 %.2f p99 %.2f p99.9 %.2f max %.1f >25ms %d" % (
        label, len(iv), rate, m6.pct(iv, 0.5), m6.pct(iv, 0.9), m6.pct(iv, 0.99), m6.pct(iv, 0.999), iv[-1],
        sum(1 for x in iv if x > 25))


def main():
    args = sys.argv[1:]
    heavy_at = 48
    if "--bodies" in args:
        i = args.index("--bodies"); heavy_at = int(args[i + 1]); del args[i:i + 2]
    kind, dirs = args[0], args[1:]
    if kind == "busy":
        for d in dirs:
            b = busy(os.path.join(d, "run"))
            name = os.path.basename(d.rstrip("/\\"))
            print("%s | %s" % (name, "game thread busy %.2f ms of a %.2f ms guest frame, GPU spans %.2f ms a frame"
                                % (b["busy"], b["guest"], b["gpu"]) if b else "no frame trace"))
        return 0
    if kind in ("fight", "explore"):
        sys.argv = [sys.argv[0], kind] + dirs
        return m6.main()
    for d in dirs:
        run = os.path.join(d, "run")
        name = os.path.basename(d.rstrip("/\\"))
        if kind == "debris":
            r = debris(run, heavy_at)
            if not r:
                print("%s | no frame trace with physics loads: not measured" % name)
                continue
            heavy, light, most = r
            print("%s | heavy (>= %d awake bodies) %s | light %s | awake bodies an interval p50 %d p90 %d max %d" % (
                name, heavy_at, line("", heavy), line("", light), m6.pct(most, 0.5), m6.pct(most, 0.9), most[-1]))
        elif kind == "transitions":
            segs = m6.segments(run, "route")
            if not segs:
                print("%s | no route segment" % name)
                continue
            for which, label in (("f", "flips"), ("p", "presents")):
                iv, span = m6.intervals(run, segs, which)
                if len(iv) < 10:
                    continue
                print("%s | %s in %d route segments (%.0f s): %s >50ms %d" % (
                    name, label, len(segs), span, line("", iv, span), sum(1 for x in iv if x > 50)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
