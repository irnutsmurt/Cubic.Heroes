#!/usr/bin/env python3
"""R4's measurement (docs/NATIVE_R4.md section 8): the river fight's frame
times of a run -- the guest's flip requests ('f') and the window presents
('p') of its DIAG_FRAMES log inside the drive's fight segment
(segments.jsonl): fps, interval p50 / p99 / p99.9 / max, intervals over 25
ms; and with a ppfx report, the native side's costs per frame with records.

  tools/r4_fight.py <tag dir> [...]     (build/r4/perf/<tag>, holding run/ and report.txt)
"""
import json
import os
import re
import sys


def pct(v, q):
    return v[min(len(v) - 1, int(len(v) * q))]


def main():
    for d in sys.argv[1:]:
        run = os.path.join(d, "run")
        seg = None
        try:
            for ln in open(os.path.join(run, "segments.jsonl"), encoding="utf-8"):
                j = json.loads(ln)
                if j.get("kind") == "fight":
                    seg = (j["t0"], j.get("t1") or j["s1"]["t"])
        except OSError:
            pass
        f, p = [], []
        for ln in open(os.path.join(run, "frames.log"), encoding="utf-8", errors="replace"):
            if ln[:2] in ("f ", "p "):
                t = int(ln.split()[1]) / 1e6
                if seg and not (seg[0] <= t <= seg[1]):
                    continue
                (f if ln[0] == "f" else p).append(t)
        out = [os.path.basename(d)]
        for name, t in (("flips", f), ("presents", p)):
            if len(t) < 10:
                continue
            iv = sorted((t[i] - t[i - 1]) * 1000 for i in range(1, len(t)))
            out.append("%s %.1f/s p50 %.1f p99 %.1f p99.9 %.1f max %.1f >25ms %d" % (
                name, len(iv) / (t[-1] - t[0]), pct(iv, 0.5), pct(iv, 0.99), pct(iv, 0.999), iv[-1],
                sum(1 for x in iv if x > 25)))
        rep = os.path.join(d, "report.txt")
        if os.path.exists(rep):
            r = open(rep, encoding="utf-8", errors="replace").read()
            n = int((re.search(r"native frames_with_records (\d+)", r) or [0, "0"])[1]) or 1
            def g(rx):
                m = re.search(rx, r)
                return float(m.group(1)) if m else 0.0
            out.append("per frame with records (%d): recorder %.3f ms, walker bridge %.3f ms (handbacks %.3f, of it "
                       "waiting for the native side %.3f), native thread %.3f ms, native GPU %.3f ms" % (
                           n, g(r"recorder_ms ([\d.]+)") / n, g(r"walker_ms bridge ([\d.]+)") / n,
                           g(r"r4 handbacks \d+ failed \d+ ms ([\d.]+)") / n,
                           g(r"waiting for the native side ([\d.]+)") / n, g(r"native_ms record_cpu ([\d.]+)") / n,
                           g(r"native_ms record_cpu [\d.]+ gpu ([\d.]+)") / n))
        print(" | ".join(out))


if __name__ == "__main__":
    main()
