#!/usr/bin/env python3
"""The frames around each map pack's inflate: an area change's cost, first
visit against return, run against run.

  tests/inflate_frames.py <run dir> [...] [--window 1.5] [--group-env NAME]

Reads <run>/stderr.log's "[edge-inflate] #n native: <out> bytes from <in> in
<ms> ms ... at <s> s" lines (DOTHEROES_INFLATE_LOG=1: every inflate logged
with its start on the frame trace's clock; "(kept pack copied)" a hit of the
pack cache tried 2026-11-03, build/baselines/2026-11-03-pack-cache-ab) and
<run>/frames.log (DIAG_FRAMES: guest flip requests).
Per inflate: the pack (its output size), its visit (1 = the first in the run),
the inflate's own time, and the longest guest frame (flip request to flip
request) that ends within <window> s after the inflate began. Per group of
runs (by an environment switch in run.json, --group-env):
the same pack's visits side by side, so a revisit is compared with the same
revisit.
"""
import argparse
import gzip
import json
import os
import re
import sys

INFL = re.compile(r"\[edge-inflate\] #(\d+) native: (\d+) bytes from (\d+) in ([\d.]+) ms(.*) at ([\d.]+) s")


def flips(d):
    p = os.path.join(d, "frames.log")
    f = open(p, encoding="utf-8") if os.path.exists(p) else gzip.open(p + ".gz", "rt", encoding="utf-8")
    return [int(ln.split()[1]) / 1e6 for ln in f if ln.startswith("f ")]


def inflates(d):
    out = []
    for ln in open(os.path.join(d, "stderr.log"), encoding="utf-8", errors="replace"):
        m = INFL.search(ln)
        if m:
            out.append({"out": int(m.group(2)), "in": int(m.group(3)), "ms": float(m.group(4)),
                        "hit": "kept pack copied" in m.group(5), "t": float(m.group(6))})
    return out


def run_rows(d, window):
    fl = flips(d)
    seen = {}
    rows = []
    for x in inflates(d):
        key = (x["out"], x["in"])
        seen[key] = seen.get(key, 0) + 1
        worst, at = 0.0, None
        for a, b in zip(fl, fl[1:]):
            if x["t"] <= b <= x["t"] + window and (b - a) * 1e3 > worst:
                worst, at = (b - a) * 1e3, b
        rows.append({**x, "visit": seen[key], "frame_ms": worst, "frame_at": at})
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--window", type=float, default=1.5)
    ap.add_argument("--group-env", default="DOTHEROES_INFLATE_CACHE")
    ap.add_argument("--min-mb", type=float, default=20.0, help="packs at least this big (output MB)")
    o = ap.parse_args()
    groups = {}
    for d in o.runs:
        try:
            sw = json.load(open(os.path.join(d, "run.json"))).get("switches", {})
        except OSError:
            sw = {}
        g = sw.get(o.group_env, "(unset)")
        rows = [r for r in run_rows(d, o.window) if r["out"] >= o.min_mb * 1e6]
        print(f"{os.path.basename(os.path.normpath(d))}  {o.group_env}={g}")
        for r in rows:
            print(f"  pack {r['out'] / 1e6:6.1f} MB visit {r['visit']}: inflate {r['ms']:5.1f} ms"
                  f"{' (kept)' if r['hit'] else '       '}  worst frame {r['frame_ms']:6.1f} ms")
        groups.setdefault(g, []).append(rows)
    if len(groups) < 2:
        return 0
    print(f"\nworst frame by pack and visit, per {o.group_env} (each run's value)")
    keys = sorted({(r["out"], r["visit"]) for runs in groups.values() for rows in runs for r in rows})
    names = sorted(groups)
    print(f"  {'pack MB':>8} {'visit':>5}  " + "  ".join(f"{n:>22}" for n in names))
    for out, v in keys:
        cells = []
        for n in names:
            vals = [r["frame_ms"] for rows in groups[n] for r in rows if (r["out"], r["visit"]) == (out, v)]
            cells.append(" ".join(f"{x:5.1f}" for x in vals) or "-")
        print(f"  {out / 1e6:8.1f} {v:5d}  " + "  ".join(f"{c:>22}" for c in cells))
    return 0


if __name__ == "__main__":
    sys.exit(main())
