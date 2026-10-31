#!/usr/bin/env python3
"""The RSX walker's time per guest frame, split into active work and each kind
of wait, and which of it sits between the guest's flip and the frame shown.

  tests/walker_breakdown.py <ftrace.txt> [<ftrace.txt> ...]

Per guest frame i (from a PS3_FTRACE timeline; needs the walker spans of
kind 18 and, for GPU time, VK_GPU_TIME=1):
  req    the guest asking for the flip (FLIP_REQ): its commands are all written
  fifo   the walker reaching that flip in the FIFO (FLIP_FIFO)
  shown  the end of the first present that follows (PRESENT, who 1: the GPU
         done, the readback and the window's present) -- the frame on screen.
         The engine presents each frame twice (the FIFO's flip, then the
         walker loop's present of the same buffer); the second is a repeat.
  lag = fifo - req: walker work still to do once the guest is done -- it
  delays this frame's present directly; latency = shown - fifo; the
  displayed interval = this frame's shown - the previous frame's.
The walker thread's time is split into drain passes (active: decoding and
recording; minus the GPU waits inside them, by kind: flush = submit and wait,
infl = the previous batch still in flight, prlock = the previous present not
finished), a guest frame's present begun on it (minus its waits), the window
pump and the 1 ms sleep between passes. Shown for the whole frame (req to
next req, overlapping the guest's own work) and for the lag alone. Frames are
banded by displayed interval. The main thread's idle time per frame is listed
by what it waited in (HLE import by NID, usleep / syscall by guest lr).
"""
import bisect
import collections
import sys

SYSCALL, USLEEP, SEMA, DRAIN, FLIP_FIFO, FLIP_REQ, PRESENT, GPU_WAIT, GPU_EXEC, HLE, WALKER = \
    1, 2, 5, 7, 8, 9, 10, 12, 14, 15, 18
MAIN = 1
WAITS = {0: "flush", 1: "prlock", 2: "infl"}
WALK = {1: "present", 2: "pump", 3: "sleep"}
CATS = ["drain.active", "drain.flush", "drain.infl", "drain.prlock", "present.active", "present.prlock",
        "present.flush", "present.infl", "pump", "sleep"]


def clip(a, b, lo, hi):
    return max(0.0, min(b, hi) - max(a, lo))


def load(path):
    rec = collections.defaultdict(list)
    for line in (__import__("gzip").open(path, "rt", encoding="utf-8", errors="replace") if path.endswith(".gz")
                 else open(path, encoding="utf-8", errors="replace")):   # kept baselines are gzipped
        f = line.split()
        if len(f) != 6 or not f[0].isdigit():
            continue
        k, who, a, b, t0, t1 = int(f[0]), int(f[1]), int(f[2], 16), int(f[3], 16), float(f[4]), float(f[5])
        rec[k].append((t0, t1, who, a, b))
    for v in rec.values():
        v.sort()
    return rec


def walker_spans(rec):
    """(t0, t1, category prefix, [nested waits]) for the walker thread"""
    spans = [(t0, t1, "drain") for t0, t1, *_ in rec[DRAIN]]
    spans += [(t0, t1, WALK.get(who, "walker%d" % who)) for t0, t1, who, *_ in rec[WALKER]]
    spans.sort()
    waits = rec[GPU_WAIT]
    starts = [w[0] for w in waits]
    out = []
    for t0, t1, cat in spans:
        nested = []
        if cat in ("drain", "present"):
            i = bisect.bisect_left(starts, t0)
            while i < len(waits) and waits[i][0] < t1:
                w0, w1, _, a, _b = waits[i]
                if w1 <= t1 + 1:
                    nested.append((w0, min(w1, t1), WAITS.get(a, "wait%d" % a)))
                i += 1
        out.append((t0, t1, cat, nested))
    return out


def split(spans, lo, hi, acc):
    """add each walker category's time inside [lo, hi) to acc (ms)"""
    for t0, t1, cat, nested in spans:
        if t1 <= lo or t0 >= hi:
            continue
        span = clip(t0, t1, lo, hi)
        waited = 0.0
        for w0, w1, kind in nested:
            d = clip(w0, w1, lo, hi)
            acc[f"{cat}.{kind}"] += d / 1000
            waited += d
        if cat in ("drain", "present"):
            acc[f"{cat}.active"] += (span - waited) / 1000
        else:
            acc[cat] += span / 1000


def main(path):
    rec = load(path)
    reqs = [t0 for t0, *_ in rec[FLIP_REQ]]
    fifos = [t0 for t0, *_ in rec[FLIP_FIFO]]
    shows = [t1 for t0, t1, who, *_ in rec[PRESENT] if who == 1]
    spans = walker_spans(rec)
    gpu = rec[GPU_EXEC]
    frames, prev_shown = [], None
    for i in range(1, len(reqs) - 1):
        req, nxt = reqs[i], reqs[i + 1]
        j = bisect.bisect_left(fifos, req)
        if j >= len(fifos):
            break
        fifo = fifos[j]
        k = bisect.bisect_left(shows, fifo)
        if k >= len(shows):
            break
        if prev_shown is None or shows[k] <= prev_shown:
            prev_shown = shows[k]
            continue
        whole, lagacc = collections.Counter(), collections.Counter()
        split(spans, reqs[i - 1], req, whole)   # the frame's commands being written: req(i-1) to req(i)
        split(spans, req, fifo, lagacc)
        g = sum(clip(t0, t1, fifo, shows[k]) for t0, t1, *_ in gpu) / 1000
        frames.append(dict(shown=shows[k] - prev_shown, lag=(fifo - req) / 1000, lat=(shows[k] - fifo) / 1000,
                           ppu=(req - reqs[i - 1]) / 1000, whole=whole, lagacc=lagacc, gpu_lat=g,
                           lo=reqs[i - 1], hi=req))
        prev_shown = shows[k]
    for f in frames:
        f["shown"] /= 1000
    # main thread idle, by what it waited in
    idle = [(t0, t1, ("HLE %08X" % a) if k == HLE else ("usleep lr %08X" % b) if k == USLEEP else
             ("syscall %d" % a)) for k in (SYSCALL, USLEEP, HLE) for t0, t1, who, a, b in rec[k] if who == MAIN]
    idle.sort()
    istarts = [x[0] for x in idle]

    d = sorted(f["shown"] for f in frames)
    pct = lambda q: d[min(len(d) - 1, int(q * len(d)))]
    span = sum(d) / 1000
    print(f"== {path}: {len(frames)} frames shown in {span:.1f} s = {len(d) / span:.1f}/s; interval median "
          f"{pct(0.5):.1f} ms, p95 {pct(0.95):.1f}, p99 {pct(0.99):.1f}, max {d[-1]:.1f}; over 25 ms "
          f"{sum(x > 25 for x in d)}, over 33 ms {sum(x > 33.4 for x in d)}")
    for lo, hi in ((0, 18), (18, 25), (25, 34), (34, 50), (50, 1e9)):
        sel = [f for f in frames if lo <= f["shown"] < hi]
        if not sel:
            continue
        n = len(sel)
        m = lambda key: sum(f[key] for f in sel) / n
        print(f"\nshown {lo:.0f}-{hi if hi < 1e8 else 999:.0f} ms: {n} frames; guest frame {m('ppu'):.2f} ms, "
              f"lag (flip request -> walker at flip) {m('lag'):.2f}, latency (walker at flip -> shown) "
              f"{m('lat'):.2f} (GPU busy in it {m('gpu_lat'):.2f})")
        for title, key in (("walker thread over the guest frame", "whole"), ("walker thread in the lag", "lagacc")):
            tot = collections.Counter()
            for f in sel:
                tot.update(f[key])
            parts = ", ".join(f"{c} {tot[c] / n:.2f}" for c in CATS if tot[c] / n >= 0.005)
            other = sum(v for c, v in tot.items() if c not in CATS) / n
            print(f"  {title}: {sum(tot.values()) / n:.2f} ms = {parts}" + (f", other {other:.2f}" if other >= 0.005 else ""))
        waits = collections.Counter()
        for f in sel:
            i = bisect.bisect_left(istarts, f["lo"] - 50000)
            while i < len(idle) and idle[i][0] < f["hi"]:
                t0, t1, what = idle[i]
                waits[what] += clip(t0, t1, f["lo"], f["hi"]) / 1000
                i += 1
        top = ", ".join(f"{w} {v / n:.2f}" for w, v in waits.most_common(5) if v / n >= 0.01)
        print(f"  main thread idle over the guest frame: {sum(waits.values()) / n:.2f} ms" + (f" ({top})" if top else ""))


for p in sys.argv[1:]:
    main(p)
