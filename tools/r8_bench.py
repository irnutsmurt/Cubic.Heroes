#!/usr/bin/env python3
"""R8: a measurement batch's runs summarised by arm (docs/NATIVE_R8.md
section 8): the guest frames in the frame trace's window (flip requests,
kind 9) -- rate, p50/p90/p99/p99.9, the share over 6.94 ms (144 fps) and over
5.00 ms (200 fps) -- the game thread's busy time a frame (tools/r7_measure.py's
busy: the frame less the main thread's waits), and the same frame times for
the heavy-debris frames (an interval whose collision tasks saw 48 or more
awake bodies) and the rest.

  tools/r8_bench.py <prefix> [--arms a,b,c] [--bodies N]

Runs are build/r8/<prefix>-<arm>-<round> (e.g. ab3-river-stA-1); each arm's
rounds are listed, then their range.
"""
import glob, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r7_measure as r7  # noqa: E402
import r6_measure as m6  # noqa: E402


def frames(run):
    f = os.path.join(run, "ftrace.txt")
    reqs = []
    for ln in open(f, encoding="utf-8", errors="replace"):
        x = ln.split()
        if len(x) == 6 and x[0] == "9":
            reqs.append(float(x[4]))
    reqs.sort()
    return [(b - a) / 1000.0 for a, b in zip(reqs, reqs[1:])]


def stats(iv):
    s = sorted(iv)
    n = len(s)
    return {"fps": 1000.0 / (sum(s) / n), "p50": m6.pct(s, .5), "p90": m6.pct(s, .9), "p99": m6.pct(s, .99),
            "p999": m6.pct(s, .999), "max": s[-1], "o144": 100.0 * sum(1 for x in s if x > 1000 / 144) / n,
            "o200": 100.0 * sum(1 for x in s if x > 5.0) / n, "n": n}


def row(run, heavy_at):
    iv = frames(run)
    if len(iv) < 50:
        return None
    r = stats(iv)
    b = r7.busy(run)
    r["busy"] = b["busy"] if b else float("nan")
    d = r7.debris(run, heavy_at)
    if d:
        heavy, light, _ = d
        if len(heavy) >= 20:
            h = stats(heavy)
            r.update({"h_fps": h["fps"], "h_p50": h["p50"], "h_p99": h["p99"], "h_n": h["n"]})
        if len(light) >= 20:
            l = stats(light)
            r.update({"l_fps": l["fps"], "l_p99": l["p99"]})
    return r


COLS = [("fps", "%.1f"), ("p50", "%.2f"), ("p90", "%.2f"), ("p99", "%.2f"), ("p999", "%.2f"), ("o144", "%.1f%%"),
        ("o200", "%.1f%%"), ("busy", "%.2f"), ("h_fps", "%.0f"), ("h_p50", "%.2f"), ("h_p99", "%.2f"), ("l_p99", "%.2f")]


def main():
    args = sys.argv[1:]
    heavy_at = 48
    if "--bodies" in args:
        i = args.index("--bodies"); heavy_at = int(args[i + 1]); del args[i:i + 2]
    arms = None
    if "--arms" in args:
        i = args.index("--arms"); arms = args[i + 1].split(","); del args[i:i + 2]
    prefix = args[0]
    runs = sorted(glob.glob(os.path.join("build", "r8", prefix + "-*")))
    by_arm = {}
    for d in runs:
        name = os.path.basename(d)
        arm, rnd = name[len(prefix) + 1:].rsplit("-", 1)
        by_arm.setdefault(arm, []).append((rnd, os.path.join(d, "run")))
    print("arm    round " + " ".join("%7s" % c for c, _ in COLS) + "   (ms; o144/o200: frames over 6.94/5.00 ms; h_: heavy debris, l_: the rest)")
    for arm in (arms or sorted(by_arm)):
        rows = []
        for rnd, run in sorted(by_arm.get(arm, [])):
            r = row(run, heavy_at)
            if not r:
                print("%-6s %-5s no frames" % (arm, rnd)); continue
            rows.append(r)
            print("%-6s %-5s " % (arm, rnd) + " ".join("%7s" % ((f % r[c]) if c in r else "-") for c, f in COLS))
        if len(rows) > 1:
            rng = []
            for c, f in COLS:
                v = [r[c] for r in rows if c in r]
                rng.append(("%s-%s" % (f % min(v), f % max(v))).replace("%", "") if v else "-")
            print("%-6s range " % arm + " ".join("%7s" % x for x in rng))


if __name__ == "__main__":
    main()
