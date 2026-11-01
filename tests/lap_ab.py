#!/usr/bin/env python3
"""Ring-wrap A/B: does a change to the command-ring waits make frames faster,
or move the wait somewhere else?

  tests/lap_ab.py <run dir> [...] [--arm-env GCM_WRAP_LAP]

Runs are grouped by the value of an environment switch in their run.json
(default GCM_WRAP_LAP). Per arm, over each run's PS3_FTRACE window:
  ring      the title's waits for room in its ring (kind 21 spans), per guest
            frame; wraps into a lap ending at the walker (instants 3 at a
            flip, 4 at the ring's end) and catch-up waits (spans of kind 1)
  pacing    present-call completion intervals (not display timing): median,
            p95, p99, share over 25 and 33 ms; guest frames (flip request to
            flip request): mean, p90, p99, share over 18 and 20 ms
  queue     work queued behind the title: lag = flip request -> the walker
            reaching that flip; latency = walker at the flip -> the present
            returned; and frame start -> present returned (the previous flip
            request to this frame's present: what input read at the frame's
            start waits before its frame is presented) -- median, p90, p99
  main      the main thread's waits per guest frame, by kind: ring, Bullet
            queue, usleep, syscalls, HLE -- a wait that moved shows here
  flips     flip requests against flips the walker reached, in order:
            missing or out of order ones
and from each run's stderr: ring-check results, stalls, the wrap counters.
"""
import argparse
import bisect
import collections
import glob
import json
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import solver_breakdown as sb
import walker_breakdown as wb

SYSCALL, USLEEP, FLIP_FIFO, FLIP_REQ, HLE, QUEUE_WAIT, RING = 1, 2, 8, 9, 15, 17, 21


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))] if v else float("nan")


def trace_of(d):
    for n in ("ftrace.txt", "ftrace.txt.gz"):
        if os.path.exists(os.path.join(d, n)):
            return os.path.join(d, n)


def run_stats(d):
    rec = wb.load(trace_of(d))
    frames = wb.guest_frames(rec)
    ring = sorted((t0, t1) for t0, t1, who, a, b in rec[RING] if (a & 0xF) in (1, 2))
    kinds = collections.Counter(a & 0xF for t0, t1, who, a, b in rec[RING])
    waits = {"ring": ring,
             "bullet queue": sorted((t0, t1) for t0, t1, who, a, b in rec[QUEUE_WAIT] if who == 1),
             "usleep": sorted((t0, t1) for t0, t1, who, a, b in rec[USLEEP] if who == 1),
             "syscall": sorted((t0, t1) for t0, t1, who, a, b in rec[SYSCALL] if who == 1),
             "HLE": sorted((t0, t1) for t0, t1, who, a, b in rec[HLE] if who == 1)}
    steps = [cr[0][0] for cr, segs in sb.steps_of(rec) if cr]
    out = []
    for f in frames:
        w = {k: sb.union(sb.clip(v, f["lo"], f["hi"])) / 1000 for k, v in waits.items()}
        n = bisect.bisect_left(steps, f["hi"]) - bisect.bisect_left(steps, f["lo"])
        out.append(dict(shown=f["shown"], ppu=f["ppu"], lag=f["lag"], lat=f["lat"],
                        start_to_present=f["ppu"] + f["lag"] + f["lat"], steps=n, waits=w))
    reqs = [(t0, a) for t0, t1, who, a, b in rec[FLIP_REQ] if b == 1]   # queued in the FIFO (b = in_fifo)
    fifos = [(t0, a) for t0, t1, who, a, b in rec[FLIP_FIFO]]
    # The walker can reach a flip before its request is traced, so match them as sequences: the
    # requests' buffer ids against the flips', at the offset (a few either way, for the window's
    # edges) where they agree best. Missing = the counts' difference beyond that offset; wrong =
    # disagreements left there (a flip dropped or doubled shifts the sequence and shows as many).
    rb, fb = [x[1] & 7 for x in reqs], [x[1] & 7 for x in fifos]
    best = None
    for o in range(-4, 5):
        pairs = [(rb[i], fb[i + o]) for i in range(len(rb)) if 0 <= i + o < len(fb)]
        bad = sum(x != y for x, y in pairs)
        if best is None or bad < best[0]:
            best = (bad, o)
    wrong, off = best if best else (0, 0)
    missing = max(0, abs(len(rb) - len(fb)) - abs(off) - 1)
    log = {}
    p = os.path.join(d, "stderr.log")
    if os.path.exists(p):
        txt = open(p, encoding="utf-8", errors="replace").read()
        log["ring-check"] = (re.findall(r"\[ring-check\] (\d+) wraps.*?(\d+) commands compared there, (\d+) overwritten", txt) or [None])[-1]
        log["overwritten lines"] = len(re.findall(r"OVERWRITTEN UNREAD", txt))
        log["stalls"] = len(re.findall(r"drain stalled|walker stalled", txt))
        log["laps"] = (re.findall(r"wrap into a lap ending at the walker: (\d+); waited instead: (.*)", txt) or [None])[-1]
        fps = [float(x) for x in re.findall(r"\[diag\] (?:\d+) s: ([\d.]+) flips/s", txt)]
        log["fps 60-140 s"] = fps[5:14]
    return dict(frames=out, kinds=kinds, reqs=len(reqs), fifos=len(fifos), missing=missing, wrong=wrong, log=log)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--arm-env", default="GCM_WRAP_LAP")
    a = ap.parse_args()
    arms = collections.defaultdict(list)
    for d in a.runs:
        if not trace_of(d):
            continue
        try:
            sw = json.load(open(os.path.join(d, "run.json"))).get("switches", {})
        except OSError:
            sw = {}
        arms[sw.get(a.arm_env, "unset")].append(d)
    res = {arm: [(d, run_stats(d)) for d in sorted(ds)] for arm, ds in sorted(arms.items())}
    for arm, rs in res.items():
        fr = [f for d, r in rs for f in r["frames"]]
        print(f"== {a.arm_env}={arm}: {len(rs)} runs, {len(fr)} guest frames")
        for d, r in rs:
            lg = r["log"]
            print(f"   {os.path.basename(d)}: ring waits {r['kinds'].get(2, 0)} wrap / {r['kinds'].get(1, 0)} "
                  f"catch-up, laps at a flip {r['kinds'].get(3, 0)}, at the ring's end {r['kinds'].get(4, 0)}; flips requested "
                  f"{r['reqs']}, reached {r['fifos']}, missing {r['missing']}, out of order {r['wrong']}; ring-check "
                  f"{lg.get('ring-check')}, overwritten lines {lg.get('overwritten lines')}, stalls {lg.get('stalls')}")
            if lg.get("laps"):
                print(f"      wrap counters: laps {lg['laps'][0]}; waited instead: {lg['laps'][1]}")
            print(f"      fps 60-140 s: {' '.join(f'{x:.1f}' for x in lg.get('fps 60-140 s', []))}")
        sh = [f["shown"] for f in fr]
        g = [f["ppu"] for f in fr]
        n = len(fr) or 1
        print(f"   present interval: median {pct(sh, .5):.1f} ms, p95 {pct(sh, .95):.1f}, p99 {pct(sh, .99):.1f}; "
              f"over 25 ms {100 * sum(x > 25 for x in sh) / n:.2f}%, over 33 ms {100 * sum(x > 33.4 for x in sh) / n:.2f}%")
        print(f"   guest frame: mean {sum(g) / n:.2f} ms, p90 {pct(g, .9):.2f}, p99 {pct(g, .99):.2f}; over 18 ms "
              f"{100 * sum(x > 18 for x in g) / n:.2f}%, over 20 ms {100 * sum(x > 20 for x in g) / n:.2f}%")
        for k, lab in (("lag", "flip request -> walker at the flip"), ("lat", "walker at the flip -> present returned"),
                       ("start_to_present", "frame start -> present returned")):
            v = [f[k] for f in fr]
            print(f"   {lab}: median {pct(v, .5):.2f} ms, p90 {pct(v, .9):.2f}, p99 {pct(v, .99):.2f}")
        for title, sel in (("all frames", fr), ("frames with a physics step", [f for f in fr if f["steps"]])):
            if not sel:
                continue
            m = len(sel)
            ws = collections.Counter()
            for f in sel:
                ws.update(f["waits"])
            print(f"   main thread waits per frame, {title} ({m}): " +
                  ", ".join(f"{k} {v / m:.2f}" for k, v in ws.items()) +
                  f"; guest frame {sum(f['ppu'] for f in sel) / m:.2f}, over 25 ms "
                  f"{100 * sum(f['shown'] > 25 for f in sel) / m:.1f}%")


if __name__ == "__main__":
    main()
