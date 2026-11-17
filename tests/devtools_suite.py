#!/usr/bin/env python3
"""The developer tools' regression suite (M11.0, docs/M11_PLAN.md section 3.8), offscreen and silent: each scenario
run through tests/boot_smoke.py (the desktop guard keeps it off the screen), then its own checks.

    tests/devtools_suite.py [--bin EXE] [NAME ...]      (default: every scenario below)
    tests/devtools_suite.py --installed DIR              the install checks (a throwaway install whose settings
                                                         have DOTHEROES_DEVELOPER_MODE=1; never a player's own)

Scenarios (tests/scenarios/<name>.drive):
  devtools_basic   the channel, the Lua runner and its error paths, the run-time open points, the hold
  devtools_off     developer mode off: dev.cmd never read, no dev.out, no [dev] line in the log
  devtools_warp    the catalog; a warp to each kind of place, each arriving at its own map number; refusals
  devtools_menu    the in-game menu's Developer pages (pictures dumped): Warp to D_1_1_1 chosen and arrived; Status
  --installed: devtools_install (the game as a player starts it boots on its empty test saves), then the worker's
    `devsaves --action copy-player` and devtools_install_play (CONTINUE loads the copy and plays); the player's save
    tree (data/gamedata) byte for byte the same after each, the test tree (data/devmode/gamedata) made
A run passes when its drive ran to its end (result.json: "drive done", exit 0, no failure) and its checks hold.
"""
import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(name, exe, installed=None):
    env = dict(os.environ)
    for k in ("PS3RECOMP_VK_WINDOW", "WINDOW", "VISIBLE", "TOPMOST", "INSTALLED", "DOTHEROES_DEVELOPER_MODE"):
        env.pop(k, None)
    tag = f"suite-{name}"
    env.update({"BIN": exe, "SCENARIO": name, "RUN_TAG": tag, "ALSOFT_DRIVERS": "null", "KEEP_RUNS": "80",
                "WINDOW": "0", "VISIBLE": "0", "TOPMOST": "0"})
    if installed:
        env["INSTALLED"] = installed
    p = subprocess.run([sys.executable, os.path.join(ROOT, "tests", "boot_smoke.py")], cwd=ROOT, env=env,
                       capture_output=True, text=True)
    runs = sorted(d for d in os.listdir(os.path.join(ROOT, "build", "test")) if d.endswith(tag))
    return os.path.join(ROOT, "build", "test", runs[-1]) if runs else None, p


def drive_failures(rd):
    try:
        with open(os.path.join(rd, "result.json"), encoding="utf-8") as f:
            res = json.load(f)
    except OSError:
        return ["no result.json"]
    fails = []
    for f in res.get("failures", []):
        fails.append(f"line {f.get('line')}: {f.get('step')}: {f.get('why')}")
    if res.get("drive_status") not in (None, 0):
        fails.append(f"drive status {res.get('drive_status')!r}")
    if not str(res.get("ended_by", "")).startswith("drive done") or res.get("exit") != 0:
        fails.append(f"ended by {res.get('ended_by')!r}, exit {res.get('exit')}")
    return fails


def log_lines(rd):
    try:
        with open(os.path.join(rd, "stderr.log"), encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


def check_off(rd):
    bad = []
    if not os.path.exists(os.path.join(rd, "dev.cmd")):
        bad.append("dev.cmd was taken with developer mode off")
    if os.path.exists(os.path.join(rd, "dev.out")):
        bad.append("dev.out was written with developer mode off")
    dev = [ln for ln in log_lines(rd) if re.search(r"\[dev\]", ln)]
    if dev:
        bad.append(f"{len(dev)} [dev] log line(s) with developer mode off: {dev[0][:120]}")
    return bad


def check_basic(rd):
    bad = []
    answers = []
    try:
        with open(os.path.join(rd, "dev.out"), encoding="utf-8") as f:
            answers = [json.loads(ln) for ln in f if ln.strip()]
    except OSError:
        bad.append("no dev.out")
    held = [a for a in answers if isinstance(a.get("out"), dict) and a["out"].get("held") == 1]
    if not held:
        bad.append("no command answered while the in-game menu held the game")
    checks = [a["out"] for a in answers if isinstance(a.get("out"), dict) and "same" in a["out"]]
    if not checks or not all(c.get("same") for c in checks):
        bad.append("the scripts' Event object is not the game manager's (or no check answered)")
    if any(c.get("hold_other_thread") for c in checks):
        bad.append("the hold's hook ran on another thread than the flip's")
    return bad


def answers(rd):
    try:
        with open(os.path.join(rd, "dev.out"), encoding="utf-8") as f:
            return [json.loads(ln) for ln in f if ln.strip()]
    except OSError:
        return []


def check_warp(rd):
    bad = []
    warps = [a["out"] for a in answers(rd) if a.get("ok") and isinstance(a.get("out"), dict) and "expected" in a["out"]]
    if len(warps) < 8:
        bad.append(f"{len(warps)} warps answered, 8 expected")
    for w in warps:
        if w["area"] != w["expected"] and "note" not in w:
            bad.append(f"warp {w['map']}: arrived in area {w['area']}, its number is {w['expected']}")
    maps = [a["out"] for a in answers(rd) if a.get("ok") and isinstance(a.get("out"), list)]
    if not maps or len(maps[0]) < 150:
        bad.append(f"the catalog lists {len(maps[0]) if maps else 0} maps (150 or more expected)")
    return bad


def check_menu(rd):
    bad = []
    dumps = [f for f in os.listdir(rd) if f.startswith("overlay-") and f.endswith(".ppm")]
    if len(dumps) < 6:
        bad.append(f"{len(dumps)} menu pictures dumped (6 or more expected)")
    if not any("[warp]" in ln and "arrived D_1_1_1" in ln for ln in log_lines(rd)):
        bad.append("no warp to D_1_1_1 arrived from the menu")
    return bad


CHECKS = {"devtools_basic": check_basic, "devtools_off": check_off, "devtools_warp": check_warp,
          "devtools_menu": check_menu}


def tree_digest(tree):
    import hashlib
    h = hashlib.sha256()
    n = 0
    for dp, dn, fn in sorted(os.walk(tree)):
        dn.sort()
        for f in sorted(fn):
            p = os.path.join(dp, f)
            h.update(os.path.relpath(p, tree).replace(os.sep, "/").encode() + b"\0")
            with open(p, "rb") as x:
                h.update(x.read())
            n += 1
    return h.hexdigest(), n


def installed_suite(inst, exe):
    """the install checks; 0 when every one held"""
    import shutil
    cfg = os.path.join(inst, "config", "dotheroes.cfg")
    with open(cfg, encoding="utf-8") as f:
        if "DOTHEROES_DEVELOPER_MODE=1" not in f.read():
            print(f"{cfg}: no DOTHEROES_DEVELOPER_MODE=1 -- refused (this suite needs a throwaway developer install)")
            return 2
    player = os.path.join(inst, "data", "gamedata")
    tests = os.path.join(inst, "data", "devmode", "gamedata")
    if os.path.exists(os.path.join(inst, "data", "devmode")):
        shutil.rmtree(os.path.join(inst, "data", "devmode"))
    failed = 0
    before = tree_digest(player)
    for name in ("devtools_install", "devtools_install_play"):
        if name == "devtools_install_play":
            p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "worker.py"), "devsaves", "--install", inst,
                                "--action", "copy-player"], capture_output=True, text=True)
            res = [json.loads(ln) for ln in p.stdout.splitlines() if ln.startswith("{")]
            if not res or not res[-1].get("ok"):
                print(f"devsaves copy-player: FAIL {res[-1] if res else p.stderr[-300:]}")
                return 1
            print(f"devsaves copy-player: {res[-1]['data']}")
        rd, _ = run(name, exe, installed=inst)
        bad = ["no run directory"] if not rd else drive_failures(rd)
        after = tree_digest(player)
        if after != before:
            bad.append(f"the player's saves changed ({before[1]} files -> {after[1]})")
        if not os.path.isdir(tests):
            bad.append("no test saves' tree (data/devmode/gamedata)")
        print(f"{name}: {'PASS' if not bad else 'FAIL'} ({rd}); player's saves {after[1]} files unchanged: "
              f"{after == before}; test saves {tree_digest(tests)[1] if os.path.isdir(tests) else 0} files")
        for b in bad:
            print("  " + b)
        failed += bool(bad)
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", default=os.path.join("build", "port-win", "dotheroes.exe"))
    ap.add_argument("--installed", help="a throwaway developer install: the install checks only")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    if a.installed:
        return installed_suite(os.path.abspath(a.installed), a.bin)
    names = a.names or list(CHECKS)
    failed = 0
    for n in names:
        rd, p = run(n, a.bin)
        bad = ["no run directory"] if not rd else drive_failures(rd)
        if rd:
            bad += CHECKS[n](rd)
        print(f"{n}: {'PASS' if not bad else 'FAIL'} ({rd})")
        for b in bad:
            print("  " + b)
        failed += bool(bad)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
