#!/usr/bin/env python3
"""The title's waits for room in its command ring, split into the walker's part
and the waiter's own, and how much of each guest frame they take.

  tests/ring_wait.py <ftrace.txt[.gz]> [...]

Needs a PS3_FTRACE timeline with the ring waits (kind 21, cellGcmSys's
buffer-full callback: 1 the title caught up with the walker in a new lap of
its ring, 2 it wrapped at the ring's end; 3, instants, the wraps taken at a
flip into a lap that ends at the walker, which can lead to waits of kind 1).
For wraps, where the walker was as the title waited: KB past the ring's
begin, i.e. the room a lap ending at the walker would have given at once. Each wait: from its start to the
moment the walker had made the room ("walker", noted by the walker as it
moves), and from then to the wait's end ("waiter": the rest of its 1 ms
sleep, and being scheduled again). While the walker made the room, what it
was doing (tests/walker_breakdown.py's categories: drain passes, active or in
a GPU wait; a present; the window pump; its sleep between passes; "none":
no walker span). Per guest frame (flip request to flip request), by physics
steps in it and by present-call completion interval.
"""
import bisect
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import solver_breakdown as sb
import walker_breakdown as wb

RING_WAIT = 21
KINDS = {1: "caught up (new lap)", 2: "wrap (ring end)"}


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))] if v else 0.0


def main():
    waits_all, frames_all, flip_wraps = [], [], []
    for path in sys.argv[1:]:
        rec = wb.load(path)
        spans = wb.walker_spans(rec)
        steps = [cr[0][0] for cr, segs in sb.steps_of(rec) if cr]
        waits = []
        for t0, t1, who, a, b in rec[RING_WAIT]:
            kind, sleeps, room = a & 0xF, (a >> 4) & 0xFFF, a >> 16
            if kind == 3:
                flip_wraps.append(room)
                continue
            total = t1 - t0
            walker = total if b == 0xFFFFFFFF else min(float(b), total)
            state = collections.Counter()
            wb.split(spans, t0, t0 + walker, state)
            covered = sum(state.values())
            state["none"] += max(0.0, walker / 1000 - covered)
            waits.append(dict(t0=t0, t1=t1, kind=kind, sleeps=sleeps, room=room, total=total, walker=walker,
                              waiter=total - walker, resolved=b != 0xFFFFFFFF, state=state))
        waits_all += waits
        w0 = [w["t0"] for w in waits]
        for f in wb.guest_frames(rec):
            n = bisect.bisect_left(steps, f["hi"]) - bisect.bisect_left(steps, f["lo"])
            i, acc = bisect.bisect_left(w0, f["lo"] - 100000), collections.Counter()
            while i < len(waits) and waits[i]["t0"] < f["hi"]:
                w = waits[i]
                frac = wb.clip(w["t0"], w["t1"], f["lo"], f["hi"]) / max(w["total"], 1e-9)
                if frac > 0:
                    acc["waits"] += 1
                    acc["total"] += w["total"] * frac / 1000
                    acc["walker"] += w["walker"] * frac / 1000
                    acc["waiter"] += w["waiter"] * frac / 1000
                i += 1
            frames_all.append(dict(steps=n, shown=f["shown"], ppu=f["ppu"], **acc))
    if not waits_all:
        print("no ring waits (kind 21) in the trace(s)")
        return
    print(f"== {len(waits_all)} ring waits in {len(sys.argv) - 1} trace(s), {len(frames_all)} guest frames")
    for kind in sorted({w['kind'] for w in waits_all}):
        s = [w for w in waits_all if w["kind"] == kind]
        tot = sum(w["total"] for w in s) / 1000
        print(f"  {KINDS.get(kind, kind)}: {len(s)} waits, {tot:.0f} ms in all; per wait median {pct([w['total'] for w in s], .5) / 1000:.2f} ms "
              f"(walker {pct([w['walker'] for w in s], .5) / 1000:.2f}, waiter {pct([w['waiter'] for w in s], .5) / 1000:.2f}), "
              f"p90 {pct([w['total'] for w in s], .9) / 1000:.2f} (walker {pct([w['walker'] for w in s], .9) / 1000:.2f}, "
              f"waiter {pct([w['waiter'] for w in s], .9) / 1000:.2f}); "
              f"walker share {100 * sum(w['walker'] for w in s) / max(sum(w['total'] for w in s), 1):.0f}%; "
              f"sleeps median {pct([w['sleeps'] for w in s], .5):.0f}; unresolved {sum(not w['resolved'] for w in s)}")
    print(f"  wraps at a flip into a lap ending at the walker: {len(flip_wraps)}; room at the start, KB: median "
          f"{pct(flip_wraps, .5):.0f}, min {min(flip_wraps) if flip_wraps else 0}")
    wr = [w for w in waits_all if w["kind"] == 2]
    if wr:
        known = [w for w in wr if w["room"] != 0xFFFF]
        print(f"  wraps at the ring's end: the walker {pct([w['room'] for w in known], .5):.0f} KB past begin (median; "
              f"p10 {pct([w['room'] for w in known], .1):.0f}, p90 {pct([w['room'] for w in known], .9):.0f}); "
              f"elsewhere {len(wr) - len(known)}")
        for lim in (128, 256, 512):
            s = [w for w in known if w["room"] >= lim]
            print(f"    walker >= {lim} KB past begin: {len(s)} of {len(wr)} waits, {sum(w['total'] for w in s) / 1000:.0f} of "
                  f"{sum(w['total'] for w in wr) / 1000:.0f} ms")
    st = collections.Counter()
    for w in waits_all:
        st.update(w["state"])
    allw = sum(w["walker"] for w in waits_all) / 1000
    print(f"  the walker, while making the room ({allw:.0f} ms): " +
          ", ".join(f"{k} {100 * v / max(allw, 1e-9):.0f}%" for k, v in st.most_common() if v / max(allw, 1e-9) >= 0.005))
    for title, key, bands in (("physics steps in the frame", "steps", ((0, 1), (1, 2), (2, 99))),
                              ("present interval, ms", "shown", ((0, 18), (18, 25), (25, 1e9)))):
        print(f"\n  by {title}: frames, share with a ring wait, ms per frame: ring wait = walker + waiter; guest frame")
        for lo, hi in bands:
            s = [f for f in frames_all if lo <= f[key] < hi]
            if not s:
                continue
            n = len(s)
            m = lambda k: sum(f.get(k, 0) for f in s) / n
            lab = (f"{lo}" if hi - lo == 1 else f"{lo}+") if key == "steps" else f"{lo:.0f}-{hi if hi < 1e8 else 999:.0f}"
            print(f"   {lab:>8}  {n:6d}  {100 * sum(1 for f in s if f.get('waits')) / n:5.1f}%   {m('total'):5.2f} = {m('walker'):5.2f} + "
                  f"{m('waiter'):5.2f};  {m('ppu'):5.2f}")


if __name__ == "__main__":
    main()
