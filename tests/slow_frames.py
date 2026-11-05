#!/usr/bin/env python3
"""Slow frames traced through the threads that make them: the game's main thread
writing commands (and waiting for ring space or physics), the RSX walker
reading them (draining, waiting on the GPU, on the previous present), the GPU
and the present.

  tests/slow_frames.py <ftrace.txt[.gz]> [--top 8] [--sample 8] [--marks]

From a PS3_FTRACE timeline (DOTHEROES_PHYS_STATS=1 and VK_GPU_TIME=1 help).
A guest frame runs from one flip request to the next (tests/walker_breakdown.py).
Per frame:
  main     the PPU main thread (tid 1): waiting for room in the command ring
           (kind 21; "made" = until the walker had made the room, the rest is
           the waiter's own sleep and wake-up), waiting for physics results
           (kind 17), asleep or in a blocking call (usleep, syscalls, HLE
           imports), and the rest: running the game and writing commands
  walker   drain passes, active or in a GPU wait (flush = submit and wait,
           infl = the previous batch still on the GPU, prlock = the previous
           present not finished), a present begun on it, the window pump, its
           sleep between passes; in the frame, and in the lag (flip request to
           the walker reaching that flip)
  gpu      GPU execution in the frame
  present  the present interval (present-call completion, not display
           timing), lag and latency (walker at the flip to present returned)
Bands: per guest-frame length, the mean of each. Then the slowest frames
(--top) and a spread sample of frames of 18-50 ms (--sample), each in full;
with --loads, the frames of 50 ms or more in full too; with --marks, every hitch mark (kind 22: DOTHEROES_DEV_KEYS's F8 or L3+R3, or
a script's mark.trigger) and the frames over 18 ms in the 3 s before it.
Frames over 50 ms are listed apart: loads (the main thread blocked).
"""
import argparse
import bisect
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import walker_breakdown as wb

SYSCALL, USLEEP, QUEUE_WAIT, HLE, RING_WAIT, MARK = 1, 2, 17, 15, 21, 22
MAIN = 1


class Index:
    """spans (t0, t1, ...) sorted by t0: the time of those matching inside [lo, hi)"""

    def __init__(self, spans):
        self.s = spans
        self.starts = [x[0] for x in spans]
        self.longest = max((x[1] - x[0] for x in spans), default=0.0)

    def within(self, lo, hi):
        i, j = bisect.bisect_left(self.starts, lo - self.longest), bisect.bisect_left(self.starts, hi)
        return self.s[i:j]

    def time(self, lo, hi):
        return sum(wb.clip(x[0], x[1], lo, hi) for x in self.within(lo, hi)) / 1000


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace")
    ap.add_argument("--top", type=int, default=8)
    ap.add_argument("--sample", type=int, default=8)
    ap.add_argument("--marks", action="store_true")
    ap.add_argument("--loads", type=float, metavar="T0-T1", nargs="?", const=-1.0, default=None,
                    help="also show every frame of 50 ms or more in full")
    a = ap.parse_args()
    rec = wb.load(a.trace)
    frames = wb.guest_frames(rec)
    ring = [x for x in rec[RING_WAIT] if x[2] == MAIN and (x[3] & 0xF) in (1, 2)]
    ring_i = Index(ring)
    phys_i = Index([x for x in rec[QUEUE_WAIT] if x[2] == MAIN])
    idle_i = Index(sorted(x for k in (SYSCALL, USLEEP, HLE) for x in rec[k] if x[2] == MAIN))
    gpu_i = Index(rec[wb.GPU_EXEC])
    base = 0.0   # trace times are us on the process's clock, the one the [mark] log lines use

    for f in frames:
        lo, hi = f["lo"], f["hi"]
        r = sum(wb.clip(x[0], x[1], lo, hi) for x in ring_i.within(lo, hi)) / 1000
        made = sum(wb.clip(x[0], x[0] + (x[1] - x[0] if x[4] == 0xFFFFFFFF else min(float(x[4]), x[1] - x[0])), lo, hi)
                   for x in ring_i.within(lo, hi)) / 1000
        p, idle = phys_i.time(lo, hi), idle_i.time(lo, hi)
        f["main"] = dict(ring=r, ring_made=made, phys=p, idle=idle, busy=max(0.0, f["ppu"] - r - p - idle))
        f["gpu"] = gpu_i.time(lo, hi)

    def line(f):
        m, w = f["main"], f["whole"]
        t = (f["lo"] - base) / 1e6
        wk = ", ".join(f"{k} {v:.1f}" for k, v in sorted(w.items(), key=lambda kv: -kv[1]) if v >= 0.05)
        lg = ", ".join(f"{k} {v:.1f}" for k, v in sorted(f["lagacc"].items(), key=lambda kv: -kv[1]) if v >= 0.05)
        return (f"  {t:9.3f} s  guest {f['ppu']:5.1f} ms, present interval {f['shown']:5.1f} (lag {f['lag']:.1f}, latency "
                f"{f['lat']:.1f}); physics steps {f['phys']}, {f['points']} points\n"
                f"      main   busy {m['busy']:.1f}, ring wait {m['ring']:.1f} (walker making room {m['ring_made']:.1f}), "
                f"physics wait {m['phys']:.1f}, idle {m['idle']:.1f}\n"
                f"      walker {wk}\n"
                f"      in lag {lg or '-'};  gpu busy {f['gpu']:.1f}")

    print(f"== {a.trace}: {len(frames)} guest frames")
    bands = ((0, 18), (18, 20), (20, 25), (25, 33.4), (33.4, 50), (50, 1e9))
    keys = ["busy", "ring", "ring_made", "phys", "idle"]
    wkeys = ["drain.active", "drain.flush", "drain.infl", "drain.prlock", "present.active", "present.prlock", "pump", "sleep"]
    print("guest ms   frames  steps | main: busy  ring (made)  phys  idle | walker: " + "  ".join(wkeys) + " | gpu  present")
    for lo, hi in bands:
        s = [f for f in frames if lo <= f["ppu"] < hi]
        if not s:
            continue
        n = len(s)
        mm = lambda k: sum(f["main"][k] for f in s) / n
        mw = lambda k: sum(f["whole"].get(k, 0) for f in s) / n
        lab = f"{lo:g}-{hi:g}" if hi < 1e8 else f"{lo:g}+"
        print(f"  {lab:<8} {n:6d}  {sum(f['phys'] for f in s) / n:5.2f} | {mm('busy'):10.2f} {mm('ring'):5.2f} ({mm('ring_made'):4.2f}) "
              f"{mm('phys'):5.2f} {mm('idle'):5.2f} | " + "  ".join(f"{mw(k):{len(k)}.2f}" for k in wkeys) +
              f" | {sum(f['gpu'] for f in s) / n:4.2f}  {sum(f['shown'] for f in s) / n:5.2f}")

    slow = [f for f in frames if 18 <= f["ppu"] < 50]
    print(f"\nslowest {a.top} frames of 18-50 ms (of {len(slow)}):")
    for f in sorted(slow, key=lambda f: -f["ppu"])[:a.top]:
        print(line(f))
    if slow and a.sample:
        step = max(1, len(slow) // a.sample)
        print(f"\na spread sample of frames of 18-50 ms (every {step}th):")
        for f in slow[::step][:a.sample]:
            print(line(f))
    loads = [f for f in frames if f["ppu"] >= 50]
    if loads:
        print(f"\nframes of 50 ms or more (loads, listed apart): {len(loads)}; at " +
              ", ".join(f"{(f['lo'] - base) / 1e6:.1f} s ({f['ppu']:.0f} ms)" for f in loads[:20]))
        if a.loads is not None:
            for f in loads:
                print(line(f))
    if a.marks:
        marks = rec[MARK]
        print(f"\nhitch marks: {len(marks)}")
        his = [f["hi"] for f in frames]
        for t0, t1, who, num, _ in marks:
            src = {1: "F8", 2: "L3+R3", 3: "script"}.get(who, str(who))
            near = [f for f in frames[bisect.bisect_left(his, t0 - 3e6):bisect.bisect_left(his, t0)] if f["ppu"] >= 18]
            print(f"  mark {num} ({src}) at {(t0 - base) / 1e6:.3f} s: {len(near)} frames over 18 ms in the 3 s before")
            for f in near[-6:]:
                print(line(f))


if __name__ == "__main__":
    main()
