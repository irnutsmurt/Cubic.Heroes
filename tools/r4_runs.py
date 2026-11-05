#!/usr/bin/env python3
"""R4's runs in one table (docs/NATIVE_R4.md section 6): per build/r4/<tag>,
the scenario, frames with records, records and how they reached the walker,
calls bypassed and kept, handbacks and their checks, comparisons (R3 frames
of an alternating run), and whether the run ended by its own stop.

  tools/r4_runs.py tag [tag ...]
"""
import json
import os
import re
import sys


def num(rx, text, i=1):
    m = re.search(rx, text)
    return int(m.group(i)) if m else 0


def main():
    print("| Run | Scenario | Switches | Frames with records | Records (met at markers / matched by position / missed) | "
          "Calls bypassed / kept | Handbacks (checked: equal / differing) | R3 comparisons equal / differing | Ended |")
    print("|---|---|---|---|---|---|---|---|---|")
    for tag in sys.argv[1:]:
        d = os.path.join("build", "r4", tag)
        try:
            rep = open(os.path.join(d, "report.txt"), encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        scen, sw = "?", ""
        try:
            j = json.load(open(os.path.join(d, "run", "run.json"), encoding="utf-8"))
            e = j.get("env") or j.get("switches") or {}
            scen = e.get("SCENARIO", "?")
            keys = ("DOTHEROES_PPFX_BYPASS", "DOTHEROES_FPS", "DIAG_VBLANK_MS", "DOTHEROES_PPFX_REALLOC_EVERY",
                    "DOTHEROES_PPFX_DELAY_MS", "VK_INSTANCE_LAYERS", "RUNS")
            sw = " ".join("%s=%s" % (k.replace("DOTHEROES_PPFX_", "").replace("DOTHEROES_", ""), e[k])
                          for k in keys if k in e)
        except (OSError, ValueError):
            pass
        frames = num(r"native frames_with_records (\d+)", rep)
        records = num(r"records draws (\d+) clears (\d+)", rep) + num(r"records draws \d+ clears (\d+)", rep)
        met = num(r"markers written \d+ wraps \d+ failed \d+ met (\d+)", rep)
        matched = num(r"walker matched (\d+)", rep)
        missed = num(r"walker matched \d+ missed (\d+)", rep)
        byp = num(r"calls_bypassed (\d+)", rep)
        kept = num(r"calls_kept (\d+)", rep)
        hb = num(r"r4 handbacks (\d+)", rep)
        hbf = num(r"r4 handbacks \d+ failed (\d+)", rep)
        ceq = num(r"checked \d+ equal (\d+)", rep)
        cdf = num(r"checked \d+ equal \d+ differ (\d+)", rep)
        eq = num(r"compare wanted \d+ equal (\d+)", rep)
        df = num(r"compare wanted \d+ equal \d+ differ (\d+)", rep)
        ex = open(os.path.join(d, "exit.txt")).read().split()[-1] if os.path.exists(os.path.join(d, "exit.txt")) else "?"
        print("| %s | %s | %s | %d | %d (%d / %d / %d) | %.2fM / %.2fM | %d%s (%d / %d) | %d / %d | %s |" % (
            tag, scen, sw, frames, records, met, matched, missed, byp / 1e6, kept / 1e6, hb,
            (", %d failed" % hbf) if hbf else "", ceq, cdf, eq, df, "own stop" if ex == "0" else "exit " + ex))


if __name__ == "__main__":
    main()
