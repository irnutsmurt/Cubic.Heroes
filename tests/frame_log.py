#!/usr/bin/env python3
"""Frame times from DIAG_FRAMES (a store per frame, cheap enough to leave the
other diagnostics off), over a run's measured segment.

  tests/frame_log.py <run dir> [...] [--group-env NAME] [--from-mark 1 --to-mark 2]

Reads <run>/frames.log (DIAG_FRAMES=frames.log: "f <us>" a guest flip request,
"p <us>" a window present returned, on the frame trace's clock) and the run's
"[mark] <n> at <s> s" lines (DOTHEROES_DEV_KEYS: a drive's mark.trigger or the
tester's F8). The segment runs from the first mark to the second (the whole
log after 60 s when there are none). Per run, and per group of runs (by an
environment switch in run.json):
  guest frames     flip request to flip request: mean, p50 / p90 / p99 / p99.9,
                   max; the share over 17.5, 18, 20, 25 and 33.4 ms
  presents         window present to window present (present-call completion,
                   not display timing): the same
  spikes           guest frames over 25 ms per minute; the worst ten, when
Frames of 100 ms or more are counted apart as loads or transitions, not in
the percentiles.
"""
import argparse
import json
import os
import re
import sys

LOAD_MS = 100.0


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))] if v else float("nan")


def segment(d, m_from, m_to):
    marks = {}
    for line in open(os.path.join(d, "stderr.log"), encoding="utf-8", errors="replace"):
        m = re.match(r"\[mark\] (\d+) at ([\d.]+) s", line)
        if m:
            marks[int(m.group(1))] = float(m.group(2)) * 1e6
    lo = marks.get(m_from)
    hi = marks.get(m_to)
    return lo, hi


def intervals(ts, lo, hi):
    ts = [t for t in ts if (lo is None or t >= lo) and (hi is None or t <= hi)]
    return [(b - a) / 1000.0 for a, b in zip(ts, ts[1:])], ts


def stats(ms):
    n = max(len(ms), 1)
    body = [x for x in ms if x < LOAD_MS]
    return dict(n=len(ms), loads=len(ms) - len(body), mean=sum(body) / max(len(body), 1),
                p50=pct(body, .5), p90=pct(body, .9), p99=pct(body, .99), p999=pct(body, .999), mx=max(body, default=0),
                **{f"o{t}": 100 * sum(x > t for x in body) / n for t in (17.5, 18, 20, 25, 33.4)})


def line(s):
    return (f"{s['n']} frames, mean {s['mean']:.2f} ms, p50 {s['p50']:.1f} / p90 {s['p90']:.1f} / p99 {s['p99']:.1f} / "
            f"p99.9 {s['p999']:.1f}, max {s['mx']:.1f}; over 17.5 {s['o17.5']:.1f}%, 18 {s['o18']:.1f}%, 20 {s['o20']:.2f}%, "
            f"25 {s['o25']:.2f}%, 33.4 {s['o33.4']:.2f}%" + (f"; {s['loads']} of 100 ms+ apart" if s["loads"] else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--group-env")
    ap.add_argument("--from-mark", type=int, default=1)
    ap.add_argument("--to-mark", type=int, default=2)
    a = ap.parse_args()
    groups = {}
    for d in a.runs:
        p = os.path.join(d, "frames.log")
        if not os.path.exists(p):
            continue
        flips, pres = [], []
        for ln in open(p):
            if ln[:1] == "f":
                flips.append(int(ln.split()[1]))
            elif ln[:1] == "p":
                pres.append(int(ln.split()[1]))
        lo, hi = segment(d, a.from_mark, a.to_mark)
        if lo is None:
            lo = (flips[0] if flips else 0) + 60e6
        g_ms, g_ts = intervals(flips, lo, hi)
        p_ms, _ = intervals(pres, lo, hi)
        if not g_ms:
            continue
        sw = json.load(open(os.path.join(d, "run.json"))).get("switches", {}) if os.path.exists(os.path.join(d, "run.json")) else {}
        key = sw.get(a.group_env, "unset") if a.group_env else "all"
        secs = (g_ts[-1] - g_ts[0]) / 1e6
        spikes = sum(x > 25 for x in g_ms if x < LOAD_MS)
        worst = sorted(((x, (t - lo) / 1e6) for x, t in zip(g_ms, g_ts[1:]) if x < LOAD_MS), reverse=True)[:10]
        print(f"{os.path.basename(d)} [{a.group_env or ''}{'=' + key if a.group_env else ''}] {secs:.0f} s")
        print(f"   guest   {line(stats(g_ms))}")
        print(f"   present {line(stats(p_ms))}")
        print(f"   spikes over 25 ms: {spikes} ({60 * spikes / max(secs, 1):.1f} a minute); worst: " +
              ", ".join(f"{x:.1f} at +{t:.0f}s" for x, t in worst))
        groups.setdefault(key, [[], [], 0.0, 0])
        groups[key][0] += g_ms
        groups[key][1] += p_ms
        groups[key][2] += secs
        groups[key][3] += 1
    if a.group_env:
        for key, (g, p, secs, n) in sorted(groups.items()):
            print(f"\n== {a.group_env}={key}: {n} runs, {secs:.0f} s")
            print(f"   guest   {line(stats(g))}")
            print(f"   present {line(stats(p))}")
            sp = sum(x > 25 for x in g if x < LOAD_MS)
            print(f"   spikes over 25 ms: {sp} ({60 * sp / max(secs, 1):.1f} a minute)")


if __name__ == "__main__":
    main()
