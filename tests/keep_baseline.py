#!/usr/bin/env python3
"""Keep test runs as a baseline that later testing does not delete.

  tests/keep_baseline.py <name> <run dir> [<run dir> ...] [--note TEXT] [--ftrace]

Run directories under build/test are pruned to the last few (KEEP_RUNS in
tests/boot_smoke.py). This copies what is needed to compare against a run
later into build/baselines/<name>/<run>/, which nothing prunes:
  run.json      what ran: executable hash, source revisions, switches
  summary.txt   the run's fps lines, [native-cd] and other summary lines from
                its log, and tests/frame_times.py, frame_costs.py and
                phys_breakdown.py over its frame trace
  ftrace.txt.gz the frame trace itself, with --ftrace (a few MB)
  guard.json    a VISIBLE=1 run's on-screen guard (aborted, and why)
  grabs/        its window grabs, downscaled (the game's picture: local only)
and appends a line per run to build/baselines/INDEX.md. Nothing of the game's
data is in any of these but the grabs, which stay local (build/ is not in git).
"""
import argparse
import gzip
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "baselines")
SUMMARY_LINES = re.compile(r"^\[(diag\] .* flips|native-cd\]|phys-stats\]|fps\]|save\])")


def tool(name, trace):
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tests", name), trace], capture_output=True, text=True)
    return r.stdout + r.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name")
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--note", default="")
    ap.add_argument("--ftrace", action="store_true")
    a = ap.parse_args()
    base = os.path.join(OUT, a.name)
    os.makedirs(base, exist_ok=True)
    index = []
    for run in a.runs:
        run = os.path.abspath(run)
        dst = os.path.join(base, os.path.basename(run))
        os.makedirs(dst, exist_ok=True)
        ident = {}
        if os.path.exists(os.path.join(run, "run.json")):
            shutil.copy2(os.path.join(run, "run.json"), dst)
            ident = json.load(open(os.path.join(run, "run.json")))
        log = os.path.join(run, "stderr.log")
        lines = [l.rstrip("\n") for l in open(log, encoding="utf-8", errors="replace")] if os.path.exists(log) else []
        trace = os.path.join(run, "ftrace.txt")
        with open(os.path.join(dst, "summary.txt"), "w", encoding="utf-8") as f:
            f.write(f"{os.path.basename(run)}\n{a.note}\n\n")
            f.write("\n".join(l for l in lines if SUMMARY_LINES.match(l)) + "\n")
            if os.path.exists(trace):
                for t in ("frame_times.py", "frame_costs.py", "phys_breakdown.py"):
                    f.write(f"\n== {t}\n{tool(t, trace)}")
        if os.path.exists(os.path.join(run, "guard.json")):   # VISIBLE=1: was it on screen throughout
            shutil.copy2(os.path.join(run, "guard.json"), dst)
        grabs = sorted(os.listdir(os.path.join(run, "grabs"))) if os.path.isdir(os.path.join(run, "grabs")) else []
        if grabs:   # what was on screen, small (local only: the game's picture)
            from PIL import Image
            os.makedirs(os.path.join(dst, "grabs"), exist_ok=True)
            for g in grabs:
                im = Image.open(os.path.join(run, "grabs", g)).convert("RGB")
                im.thumbnail((480, 480))
                im.save(os.path.join(dst, "grabs", os.path.splitext(g)[0] + ".jpg"), quality=80)
        if a.ftrace and os.path.exists(trace):
            with open(trace, "rb") as src, gzip.open(os.path.join(dst, "ftrace.txt.gz"), "wb") as z:
                shutil.copyfileobj(src, z)
        exe = ident.get("exe", {})
        index.append(f"- {time.strftime('%Y-%m-%d')} `{a.name}/{os.path.basename(run)}`: exe {exe.get('sha256', '?')[:12]}, "
                     f"repo {(ident.get('repo') or {}).get('rev', '?')}, ps3recomp {(ident.get('ps3recomp') or {}).get('rev', '?')}"
                     f"{' -- ' + a.note if a.note else ''}")
        print(dst)
    with open(os.path.join(OUT, "INDEX.md"), "a", encoding="utf-8") as f:
        f.write("\n".join(index) + "\n")


if __name__ == "__main__":
    main()
