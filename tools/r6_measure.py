#!/usr/bin/env python3
"""R6's measurement (docs/NATIVE_R6.md section 8): per run, the frame times
inside the drive's segments of one kind (fight: the river fight; explore:
fields_beyond's walks) -- guest flips ('f') and window presents ('p') of its
DIAG_FRAMES log: rate, interval p50 / p90 / p99 / p99.9 / max and intervals
over 25 ms -- and, where the run has them, the threads' costs a frame: the
game thread's busy time (the guest frame less its idle time, from the frame
trace through tests/walker_breakdown.py), the walker's active time, and from
the native renderer's report its recording thread and GPU time.

  tools/r6_measure.py <fight|explore> <tag dir> [...]   (a dir holding run/ and report.txt)

A run whose segments of that kind are missing is reported as such and not
measured (the bot did not reach the encounter: its numbers would be the
whole run's, loads included).
"""
import json
import os
import re
import subprocess
import sys


def pct(v, q):
    return v[min(len(v) - 1, int(len(v) * q))]


def segments(run, kind):
    out = []
    try:
        for ln in open(os.path.join(run, "segments.jsonl"), encoding="utf-8"):
            j = json.loads(ln)
            if j.get("kind") == kind:
                t1 = j.get("t1") or (j.get("s1") or {}).get("t")
                if t1:
                    out.append((j["t0"], t1))
    except OSError:
        pass
    return out


def intervals(run, segs, which):
    iv, span = [], 0.0
    t = []
    for ln in open(os.path.join(run, "frames.log"), encoding="utf-8", errors="replace"):
        if ln[:2] == which + " ":
            t.append(int(ln.split()[1]) / 1e6)
    for a, b in segs:
        s = [x for x in t if a <= x <= b]
        iv += [(s[i] - s[i - 1]) * 1000 for i in range(1, len(s))]
        if len(s) > 1:
            span += s[-1] - s[0]
    return sorted(iv), span


def walker(run):
    f = os.path.join(run, "ftrace.txt")
    if not os.path.exists(f):
        return None
    here = os.path.dirname(os.path.abspath(__file__))
    py = sys.executable
    out = subprocess.run([py, os.path.join(here, "..", "tests", "walker_breakdown.py"), f],
                         capture_output=True, text=True).stdout
    m = re.search(r"present interval 0-18 ms: (\d+) frames; guest frame ([\d.]+) ms", out)
    if not m:
        return None
    guest = float(m.group(2))
    rest = out[m.end():]
    idle = re.search(r"main thread idle over the guest frame: ([\d.]+) ms", rest)
    act = re.search(r"walker thread over the guest frame: [\d.]+ ms = drain\.active ([\d.]+)", rest)
    pr = re.search(r"present\.active ([\d.]+)", rest.split("\n")[1] if "\n" in rest else rest)
    gpu = re.search(r"GPU busy in it ([\d.]+)", out[m.start():])
    return {"guest": guest, "busy": guest - (float(idle.group(1)) if idle else 0.0),
            "walker": (float(act.group(1)) if act else 0.0) + (float(pr.group(1)) if pr else 0.0),
            "engine_gpu": float(gpu.group(1)) if gpu else 0.0}


def engine_gpu(run):
    """The engine's GPU time a guest frame over the frame trace (VK_GPU_TIME=1:
    kind 14, its submissions' execution), and the flips in the same window"""
    f = os.path.join(run, "ftrace.txt")
    if not os.path.exists(f):
        return None
    gpu, flips, lo, hi = 0.0, 0, None, None
    for line in open(f, encoding="utf-8", errors="replace"):
        x = line.split()
        if len(x) != 6 or not x[0].isdigit():
            continue
        k, t0, t1 = int(x[0]), float(x[4]), float(x[5])
        if k == 14:
            gpu += t1 - t0
        elif k == 9:
            flips += 1
            lo = t0 if lo is None else min(lo, t0)
            hi = t0 if hi is None else max(hi, t0)
    return gpu / 1000.0 / flips if flips else None


def report(d):
    r = os.path.join(d, "report.txt")
    if not os.path.exists(r):
        return None
    s = open(r, encoding="utf-8", errors="replace").read()
    n = int((re.search(r"native frames_with_records (\d+)", s) or [0, "0"])[1]) or 0
    if not n:
        return None
    g = lambda rx: float((re.search(rx, s) or [0, "0"])[1])
    return {"recorder": g(r"recorder_ms ([\d.]+)") / n, "native": g(r"native_ms record_cpu ([\d.]+)") / n,
            "native_gpu": g(r"native_ms record_cpu [\d.]+ gpu ([\d.]+)") / n}


def main():
    kind = sys.argv[1]
    for d in sys.argv[2:]:
        run = os.path.join(d, "run")
        segs = segments(run, kind)
        name = os.path.basename(d.rstrip("/\\"))
        if not segs:
            print("%s | no %s segment: not measured" % (name, kind))
            continue
        out = [name]
        for which, label in (("f", "flips"), ("p", "presents")):
            iv, span = intervals(run, segs, which)
            if len(iv) < 10:
                continue
            out.append("%s %.1f/s p50 %.2f p90 %.2f p99 %.2f p99.9 %.2f max %.1f >25ms %d" % (
                label, len(iv) / span, pct(iv, 0.5), pct(iv, 0.9), pct(iv, 0.99), pct(iv, 0.999), iv[-1],
                sum(1 for x in iv if x > 25)))
        w = walker(run)
        if w:
            eg = engine_gpu(run)
            out.append("frame trace: game thread busy %.2f ms (guest frame %.2f), walker active %.2f, engine GPU %s"
                       % (w["busy"], w["guest"], w["walker"], "%.2f" % eg if eg is not None else "-"))
        rp = report(d)
        if rp:
            out.append("native: recorder %.3f ms, recording thread %.3f ms, GPU %.3f ms a frame"
                       % (rp["recorder"], rp["native"], rp["native_gpu"]))
        print(" | ".join(out))


if __name__ == "__main__":
    main()
