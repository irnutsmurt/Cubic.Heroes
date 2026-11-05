#!/usr/bin/env python3
"""Smoothness of an alternating run (DOTHEROES_PPFX_BYPASS=alt:<n>,
docs/NATIVE_R4.md section 6) by block: from its DIAG_FRAMES log, the guest's
flip intervals in the R4 blocks (the native renderer's output shown) and the
R3 blocks (the engine's shown, the native compared), by area when the run
has a record.log (a play session's): fps, intervals over 25 ms; and the
flips around each of the owner's marks.

  tools/r4_blocks.py <run dir> [...] [--alt 300] [--skip 10]

--skip: seconds at the start left out (the boot and the title).
"""
import argparse
import bisect
import collections
import os


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--alt", type=int, default=300)
    ap.add_argument("--skip", type=float, default=10)
    a = ap.parse_args()
    for d in a.runs:
        f = [int(ln.split()[1]) / 1e6 for ln in open(os.path.join(d, "frames.log"), encoding="utf-8", errors="replace")
             if ln.startswith("f ")]
        if len(f) < 10:
            print("%s: no flips" % d)
            continue
        areas, marks = [], []
        rec = os.path.join(d, "record.log")
        if os.path.exists(rec):
            for ln in open(rec, encoding="utf-8", errors="replace"):
                p = ln.split()
                if len(p) >= 3 and p[1] == "area":
                    areas.append((float(p[0]), int(p[2])))
                elif len(p) >= 3 and p[1] == "mark":
                    marks.append((int(p[2]), float(p[0])))
        at = [x[0] for x in areas]
        st = collections.defaultdict(lambda: [0, 0, 0.0])
        for i in range(1, len(f)):
            if f[i] - f[0] < a.skip:
                continue
            j = bisect.bisect_right(at, f[i]) - 1
            area = areas[j][1] if j >= 0 else -1
            k = (area, "R4" if (i // a.alt) % 2 else "R3")
            dt = (f[i] - f[i - 1]) * 1000
            s = st[k]
            s[0] += 1
            s[1] += dt > 25
            s[2] += dt
        print(d)
        tot = collections.defaultdict(lambda: [0, 0, 0.0])
        for k in sorted(st):
            n, o, ms = st[k]
            t = tot[k[1]]
            t[0] += n; t[1] += o; t[2] += ms
            if areas:
                print("  area %3d %s: %5d frames, %.1f fps, over 25 ms %d (%.1f%%)" % (k[0], k[1], n, n / (ms / 1000), o, 100.0 * o / n))
        for b in sorted(tot):
            n, o, ms = tot[b]
            print("  all %s: %5d frames, %.1f fps, over 25 ms %d (%.1f%%)" % (b, n, n / (ms / 1000), o, 100.0 * o / n))
        for m, t in marks:
            i = bisect.bisect_right(f, t)
            lo = bisect.bisect_left(f, t - 4)
            slow = [(f[x] - t, "R4" if (x // a.alt) % 2 else "R3") for x in range(max(lo, 1), i)
                    if (f[x] - f[x - 1]) * 1000 > 25]
            by = collections.Counter(b for _, b in slow)
            print("  mark %d: in an %s block; the 4 s before it: %d intervals over 25 ms (%s)" % (
                m, "R4" if (i // a.alt) % 2 else "R3", len(slow), ", ".join("%s %d" % kv for kv in sorted(by.items()))))


if __name__ == "__main__":
    main()
