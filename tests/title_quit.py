#!/usr/bin/env python3
"""The title menu's QUIT GAME (M11.2d, docs/M11_PLAN.md section 31): tests/scripts/title_quit.drive on the normal
title (a save: build/checkpoints/castle-front) and on a cleared game's title (build/checkpoints/m111-cleared, its
six items), offscreen through tests/boot_smoke.py (the desktop guard keeps it off the screen).

    tests/title_quit.py --bin EXE [normal|cleared ...]

Each run passes when its log shows QUIT GAME's label set, the question asked twice (the first cancelled: the game
went on), and the game's own orderly end with status 0 (no crash report), within the drive's time.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import desktop_guard  # noqa: E402

CASES = {"normal": ("build/checkpoints/castle-front", 3), "cleared": ("build/checkpoints/m111-cleared", 3)}


def newest_run(tag):
    base = os.path.join(ROOT, "build", "test")
    runs = sorted((d for d in os.listdir(base) if d.endswith(tag)), reverse=True)
    return os.path.join(base, runs[0]) if runs else None


def run_case(exe, name):
    saves, downs = CASES[name]
    env = dict(os.environ)
    desktop_guard.game_env(env, False)
    env.update({"BIN": exe, "DRIVE": os.path.join("tests", "scripts", "title_quit.drive"), "SAVES": saves,
                "RUN_TAG": f"title-quit-{name}", "QUIT": str(downs), "DUMP": "1", "SHOT_EVERY": "0",
                "ALSOFT_DRIVERS": "null"})
    r = subprocess.run([sys.executable, os.path.join(HERE, "boot_smoke.py"), "240"], cwd=ROOT, env=env,
                       capture_output=True, text=True)
    rd = newest_run(f"title-quit-{name}")
    if not rd:
        print(f"{name}: FAIL: no run folder ({(r.stdout + r.stderr).strip()[-300:]})")
        return ["no run folder"]
    log = open(os.path.join(rd, "stderr.log"), encoding="utf-8", errors="replace").read()
    bad = []
    if "QUIT GAME's label set" not in log:
        bad.append("QUIT GAME's label was never set")
    asked = log.count("[title] QUIT GAME chosen")
    if asked != 2:
        bad.append(f"the question was asked {asked} time(s) (2 expected: cancelled, then quit)")
    m = re.search(r"\[exit\] the game's end \(status (-?\d+)\)", log)
    if not m:
        bad.append("the game did not end itself")
    elif m.group(1) != "0":
        bad.append(f"the game ended with status {m.group(1)}")
    crash = os.path.join(rd, "diag")
    if os.path.isdir(crash) and any(f.startswith("crash") for f in os.listdir(crash)):
        bad.append("a crash report was written")
    print(f"{name}: {'PASS' if not bad else 'FAIL: ' + '; '.join(bad)} ({rd})")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("cases", nargs="*", help="normal, cleared (default: both)")
    a = ap.parse_args()
    cases = a.cases or list(CASES)
    for c in cases:
        if c not in CASES:
            ap.error(f"unknown case {c} (normal, cleared)")
    failed = [c for c in cases if run_case(os.path.abspath(a.bin), c)]
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
