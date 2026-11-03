#!/usr/bin/env python3
"""A batch's report: what ran, what completed, what it measured.

  tests/batch_report.py <run dir> [...] [-o report.md] [--title <text>]

Per run (tests/boot_smoke.py's run directory): the build and settings
(run.json), how it ended and the drive's segments and failures
(result.json), the frame log (DIAG_FRAMES: frames.log) cut into those
segments, memory (memory.csv), errors (stderr.log, diag/), hitch captures
(hitch/). Fights and exploration are gameplay; routes, menus, transitions and
loads are timed apart, and frames of 100 ms or more are loads wherever they
fall. A run is INVALID -- listed first, its numbers kept out of the totals --
when it was not on screen throughout (guard), the desktop was locked, its
frame log is missing, or no segment it was meant to measure completed; a
fight segment without debris physics (collision tasks under 20) is an empty
fight and does not count. Totals are per settings (the switches that change
what is measured), so unmatched configurations are never pooled.
"""
import argparse
import json
import os
import re
import sys

LOAD_MS = 100.0
GAMEPLAY = ("fight", "explore", "idle", "attract")   # measured kinds ("attract": rendering only, reported apart)
# Switches that change what a frame costs: runs are grouped by them.
PERF_KEYS = ("DOTHEROES_NATIVE_CD", "DOTHEROES_NATIVE_SOLVER", "GCM_WRAP_LAP", "DOTHEROES_PRESENT_ONCE",
             "DOTHEROES_FPS", "DIAG_VBLANK_MS", "DOTHEROES_DEV_CANT_DIE", "PS3_FTRACE", "PS3_FTRACE_ROLL",
             "DOTHEROES_PHYS_STATS", "VK_GPU_TIME", "AUDIO", "DOTHEROES_STEP_CARRY")
VALIDATED = {"DOTHEROES_NATIVE_CD": "1", "DOTHEROES_NATIVE_SOLVER": "4,5,6", "GCM_WRAP_LAP": "1",
             "DOTHEROES_PRESENT_ONCE": "1"}


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))] if v else float("nan")


def load_json(p, default=None):
    try:
        with open(p, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def frames(d):
    flips = []
    try:
        for ln in open(os.path.join(d, "frames.log")):
            if ln[:1] == "f":
                flips.append(int(ln.split()[1]))
    except OSError:
        return None
    return flips


def cut(flips, t0, t1):
    """Frame times (ms) of the guest frames ending in [t0, t1] (seconds, trace clock), with their end times."""
    lo, hi = t0 * 1e6, t1 * 1e6
    out = []
    for a, b in zip(flips, flips[1:]):
        if lo <= b <= hi and a >= lo:
            out.append(((b - a) / 1000.0, b / 1e6))
    return out


def stats(ft):
    body = [x for x, _ in ft if x < LOAD_MS]
    secs = sum(x for x, _ in ft) / 1000.0
    return {"frames": len(ft), "secs": secs, "loads": len(ft) - len(body),
            "mean": sum(body) / max(len(body), 1), "p50": pct(body, .5), "p99": pct(body, .99),
            "p999": pct(body, .999), "max": max(body, default=0.0),
            "o18": sum(x > 18 for x in body), "o25": sum(x > 25 for x in body), "o33": sum(x > 33.4 for x in body),
            "worst": sorted(((x, t) for x, t in ft if x < LOAD_MS), reverse=True)[:5],
            "load_ms": [x for x, _ in ft if x >= LOAD_MS]}


def settings(sw):
    key = {k: sw[k] for k in PERF_KEYS if k in sw}
    label = "validated-60" if all(sw.get(k) == v for k, v in VALIDATED.items()) else "other"
    extra = [f"{k}={v}" for k, v in key.items() if k not in VALIDATED]
    return label + (" " + " ".join(extra) if extra else ""), json.dumps(key, sort_keys=True)


def memory(d):
    rows = []
    try:
        for ln in open(os.path.join(d, "memory.csv")):
            m = re.match(r"([\d.]+),([\d.]+)", ln)
            if m:
                rows.append((float(m.group(1)), float(m.group(2))))
    except OSError:
        pass
    return rows


def errors(d):
    err = ""
    try:
        err = open(os.path.join(d, "stderr.log"), encoding="utf-8", errors="replace").read()
    except OSError:
        pass
    out = []
    m = re.search(r"^\[strict\] FATAL.*$", err, re.M)
    if m:
        out.append(m.group(0))
    n = err.count("UNRESOLVED NID")
    if n:
        out.append(f"{n} unresolved NID(s)")
    diag = os.path.join(d, "diag")
    if os.path.isdir(diag) and os.listdir(diag):
        out.append("diag reports: " + " ".join(sorted(os.listdir(diag))))
    return out


def fmt_stats(s):
    mins = max(s["secs"] / 60.0, 1e-9)
    return (f"{s['secs']:.0f} s, {s['frames']} frames: mean {s['mean']:.2f} ms, p50 {s['p50']:.1f}, p99 {s['p99']:.1f}, "
            f"p99.9 {s['p999']:.1f}, max {s['max']:.1f}; >18 ms {100 * s['o18'] / max(s['frames'], 1):.2f}%, "
            f">25 ms {s['o25']} ({s['o25'] / mins:.1f}/min), >33.4 ms {s['o33']}"
            + (f"; {s['loads']} load frame(s) of 100 ms+ apart" if s["loads"] else ""))


def report(runs, title):
    lines = [f"# {title}", ""]
    groups, bad, good = {}, [], []
    for d in runs:
        rj, res = load_json(os.path.join(d, "run.json"), {}), load_json(os.path.join(d, "result.json"), {})
        sw = rj.get("switches", {})
        label, key = settings(sw)
        flips = frames(d)
        segs = res.get("segments", [])
        why = []
        diag = os.path.join(d, "diag")
        if res.get("crash") or (os.path.isdir(diag) and any(f.startswith("crash-") for f in os.listdir(diag))):
            why.append("CRASH: " + (res.get("crash") or "see diag/"))
        guard = res.get("guard") or load_json(os.path.join(d, "guard.json"), {}) or {}
        if guard.get("aborted"):
            why.append(f"guard: {guard['aborted']}")
        if rj.get("desktop_locked"):
            why.append("desktop locked")
        if flips is None:
            why.append("no frame log")
        measured = []
        for s in segs:
            s1, s0 = s.get("s1") or {}, s.get("s0") or {}
            s["work"] = {k: s1.get(k, 0) - s0.get(k, 0) for k in ("steps", "topups", "frames") if k in s1 and k in s0}
            if s["kind"] == "fight" and s.get("ok") and s["work"].get("steps", 0) <= 20:
                s["ok"], s["why"] = False, "empty fight: no debris physics"
            if s.get("ok") and s["kind"] in GAMEPLAY:
                measured.append(s)
        if not measured and not why:
            why.append("no gameplay segment completed")
        rec = {"dir": d, "label": label, "key": key, "res": res, "rj": rj, "segs": segs, "flips": flips, "why": why}
        (bad if why else good).append(rec)
        if not why:
            groups.setdefault(key, {"label": label, "runs": [], "kinds": {}})
            groups[key]["runs"].append(rec)
            for s in measured:
                if s.get("t0") is not None and s.get("t1") is not None:
                    groups[key]["kinds"].setdefault(s["kind"], []).extend(cut(flips, s["t0"], s["t1"]))
    if bad:
        lines += ["## INVALID runs (not in the totals)", ""]
        for r in bad:
            lines.append(f"- **{os.path.basename(r['dir'])}**: {'; '.join(r['why'])} "
                         f"(ended by {r['res'].get('ended_by', '?')})")
        lines.append("")
    lines += ["## Totals by settings", ""]
    for key, g in groups.items():
        lines.append(f"### {g['label']} ({len(g['runs'])} run(s))")
        for kind, ft in sorted(g["kinds"].items()):
            lines.append(f"- {kind}: {fmt_stats(stats(ft))}")
        lines.append("")
    lines += ["## Runs", ""]
    for r in good + bad:
        rj, res = r["rj"], r["res"]
        exe = rj.get("exe", {})
        lines.append(f"### {os.path.basename(r['dir'])}{' (INVALID)' if r['why'] else ''}")
        lines.append(f"- build: exe {exe.get('sha256', '?')[:12]} ({exe.get('mtime', '?')}), repo "
                     f"{(rj.get('repo') or {}).get('rev', '?')} (+{(rj.get('repo') or {}).get('dirty_files', '?')} dirty), "
                     f"ps3recomp {(rj.get('ps3recomp') or {}).get('rev', '?')}, {rj.get('patches', '?')} patches")
        lines.append(f"- settings: {r['label']}; drive {rj.get('drive')}; ended by {res.get('ended_by', '?')}, "
                     f"{res.get('secs', '?')} s")
        segs = r["segs"]
        done = [s for s in segs if s.get("ok")]
        lines.append(f"- segments: {len(done)} of {len(segs)} completed; failures: {len(res.get('failures', []))}")
        cycles = sorted({s.get("cycle") for s in segs if s.get("cycle")})
        if cycles:
            fights = any(s["kind"] == "fight" for s in segs)
            ok_c = [c for c in cycles if any(s.get("cycle") == c and s.get("ok") for s in segs)
                    and all(s.get("ok") or s.get("attempt", 1) and any(t.get("cycle") == c and t.get("kind") == s["kind"]
                                                                   and t.get("name") == s["name"] and t.get("ok")
                                                                   for t in segs) for s in segs if s.get("cycle") == c)
                    and (not fights or any(s["kind"] == "fight" and s.get("ok") for s in segs if s.get("cycle") == c))]
            lines.append(f"- cycles: {len(ok_c)} of {len(cycles)} with every segment completed (a failed try retried "
                         f"successfully counts){' and a real fight' if fights else ''}")
        by_kind = {}
        for s in segs:
            w = (s.get("wall1") or 0) - (s.get("wall0") or 0)
            by_kind.setdefault(s["kind"], [0, 0.0])
            by_kind[s["kind"]][0] += 1
            by_kind[s["kind"]][1] += w
        if by_kind:
            lines.append("- time by kind: " + ", ".join(f"{k} {n}x {w:.0f} s" for k, (n, w) in sorted(by_kind.items())))
        for s in segs:
            w = s.get("work", {})
            ft = cut(r["flips"], s["t0"], s["t1"]) if r["flips"] and s.get("t0") is not None and s.get("t1") is not None else []
            st = stats(ft) if ft else None
            act = s.get("actors") or []
            lines.append(f"  - {s['kind']} {s['name']}" + (f" c{s['cycle']}" if s.get("cycle") else "")
                         + (f" try {s['attempt']}" if s.get("attempt") and s["attempt"] > 1 else "")
                         + f": {'ok' if s.get('ok') else 'FAILED (' + str(s.get('why')) + ')'}"
                         + (f"; collision tasks +{w.get('steps', 0):g}, top-ups +{w.get('topups', 0):g}" if w else "")
                         + (f"; actors {act[0].get('n', '?'):g}" if act else "")
                         + (f"; {fmt_stats(st)}" if st and s["kind"] in GAMEPLAY else
                            f"; {st['secs']:.0f} s, max {st['max']:.0f} ms, loads {st['loads']}" if st else ""))
            if st and st["worst"] and s["kind"] in GAMEPLAY:
                lines.append("    worst: " + ", ".join(f"{x:.1f} ms at {t:.1f} s" for x, t in st["worst"]))
        for f in res.get("failures", []):
            lines.append(f"  - FAILURE line {f.get('line')} ({f.get('step')}): {f.get('why')}"
                         + (f", cycle {f['cycle']}" if f.get("cycle") else "")
                         + (f", attempt {f['attempt']}" if f.get("attempt") else ""))
        mem = memory(r["dir"])
        if len(mem) >= 2:
            (t0, m0), (t1, m1) = mem[min(2, len(mem) - 1)], mem[-1]
            lines.append(f"- memory: {mem[0][1]:.0f} MB at start, {m0:.0f} MB at {t0:.0f} s -> {m1:.0f} MB at "
                         f"{t1:.0f} s ({(m1 - m0) / max((t1 - t0) / 60, 1e-9):+.1f} MB/min after the first minute)")
            # The same point of each cycle (its first segment's start): the first
            # cycle loads what the route needs for the first time (caches); growth
            # from cycle to cycle after it is what repeated visits accumulate.
            starts = {}
            for s in segs:
                if s.get("cycle") and s.get("wall0") is not None:
                    starts.setdefault(s["cycle"], s["wall0"])
            at = [(c, max((m for tm, m in mem if tm <= w), default=None)) for c, w in sorted(starts.items())]
            at = [(c, m) for c, m in at if m is not None]
            if len(at) >= 3:
                steps = [b[1] - a[1] for a, b in zip(at[1:], at[2:])]
                lines.append("  by cycle (working set at its start): " +
                             ", ".join(f"c{c} {m:.0f}" for c, m in at) +
                             f"; after cycle 1: {sum(steps) / len(steps):+.1f} MB a cycle"
                             + (f" (last {len(steps[-3:])}: {', '.join(f'{x:+.0f}' for x in steps[-3:])})"))
        for e in errors(r["dir"]):
            lines.append(f"- error: {e}")
        hd = os.path.join(r["dir"], "hitch")
        if os.path.isdir(hd):
            caps = sorted(f for f in os.listdir(hd) if f.endswith(".txt"))
            if caps:
                lines.append(f"- hitch captures: {len(caps)} ({', '.join(caps[:6])}{' ...' if len(caps) > 6 else ''})")
        lines.append("")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+")
    ap.add_argument("-o", "--out")
    ap.add_argument("--title", default="Test batch")
    a = ap.parse_args()
    text = report(a.runs, a.title)
    if a.out:
        with open(a.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text + "\n")
    print(text)


if __name__ == "__main__":
    sys.exit(main())
