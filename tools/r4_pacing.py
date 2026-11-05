#!/usr/bin/env python3
"""Achieved pacing of a run (docs/NATIVE_R4.md section 6): from its
DIAG_FRAMES log, the guest's flip requests ('f' lines, microseconds) over the
run's last <secs> seconds: frames per second, interval p50 / p99 / max, and
intervals over 25 ms.

  tools/r4_pacing.py <run dir> [...] [--secs 60]
"""
import argparse
import os


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--secs", type=float, default=60)
    a = ap.parse_args()
    for d in a.runs:
        p = os.path.join(d, "frames.log")
        t = []
        for ln in open(p, encoding="utf-8", errors="replace"):
            if ln.startswith("f "):
                t.append(int(ln.split()[1]))
        if len(t) < 10:
            print("%s: no flips" % d)
            continue
        end = t[-1]
        t = [x for x in t if x >= end - a.secs * 1e6]
        iv = sorted((t[i] - t[i - 1]) / 1000.0 for i in range(1, len(t)))
        n = len(iv)
        fps = n / ((t[-1] - t[0]) / 1e6)
        print("%s: %.1f fps over the last %.0f s | interval p50 %.1f p99 %.1f max %.1f ms | over 25 ms %d" % (
            d, fps, a.secs, iv[n // 2], iv[min(n - 1, int(n * 0.99))], iv[-1], sum(1 for x in iv if x > 25.0)))


if __name__ == "__main__":
    main()
