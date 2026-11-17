#!/usr/bin/env python3
"""Every boss fought to its defeat (M11.1b, docs/M11_PLAN.md section 22), offscreen and silent: each through
tests/scenarios/devtools_bossfight.drive (tests/devtools_suite.py's runner), the story set up to the step before it,
the hero armed as the story arms him, until the game sets the defeat's flag.

    tests/devtools_bosses.py --bin EXE [--shaders DIR] [BOSS ...]     (default: every one below)

A boss passes when its drive ran to its end and the game set its defeat's flag; each run's pipelines not prepared
are exported (exports/boss-<id> in the run) for the preparation.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import devtools_suite as suite  # noqa: E402

# (the tools' boss id, the story's step before it, the flag its defeat sets) -- port/devtools/devtools.cpp's bosses;
# FIGHT_MODE per boss: the kraken comes to the hero across its water -- fought from the platform its arms reach
BOSSES = [
    ("temple1", 8, 6), ("temple2", 10, 11), ("temple3", 12, 22), ("temple4", 14, 33), ("temple5", 16, 42),
    ("temple6", 18, 50), ("castle", 20, 64), ("tower1", 23, 311), ("tower2", 23, 313), ("tower3", 23, 315),
    ("tower4", 23, 317), ("tower5", 23, 319), ("tower6", 23, 321), ("tower7", 24, None), ("last", 24, 74),
]
# the priest's second fight sets no flag (its scene flags are named, never set): fought until no enemy is left, its
# after-event moves the hero on to the last enemy's room (D_7_2_8, area 77) -- that arrival is its check. Before it
# the priest asks the hero twice to join him: the first answer twice is the game's own game over, so the
# introduction is answered with the second (PAGE: `devpage ... no`)
ARRIVE = {"tower7": 77}
PAGE = {"tower7": "no"}


# the kraken (its body at about (23.2, -11.2), an arm's base each side; the arms grow back 40 s after they are cut,
# the body takes the damage) across the water, out of the short sword's reach: fought from the platform's end below
# its body -- the body then the nearest part, faced and struck -- as a player the kraken never hurts fights it: life
# full, the sword's full-life beam (the tools' invulnerability, the game's own invincibility flag)
FIGHT_MODE = {"temple4": "hold@21.6,-20.8", "tower4": "hold@21.6,-20.8"}
INVULNERABLE = {"temple4", "tower4"}


def check(rd, flag, arrive=None):
    bad = suite.drive_failures(rd)
    if flag is None:   # no flag: the after-event's arrival (ARRIVE)
        areas = [x["out"].get("area") for x in suite.answers(rd) if x.get("ok") and isinstance(x.get("out"), dict)
                 and "event_state" in x["out"]]
        if not areas or areas[-1] != arrive:
            bad.append(f"area after the fight: {areas[-1] if areas else None} ({arrive} expected)")
        return bad
    flags = [x["out"] for x in suite.answers(rd) if x.get("ok") and isinstance(x.get("out"), list) and x["out"]
             and "flag" in x["out"][0]]
    if len(flags) < 2:
        return bad + [f"{len(flags)} flag reads (2 expected)"]
    before = {f["flag"]: f["value"] for f in flags[0]}
    after = {f["flag"]: f["value"] for f in flags[-1]}
    if before.get(flag):
        bad.append(f"flag {flag} set before the fight")
    seen = False   # the fight's own observation (the last enemy's flag is set again to 0 by the ending it starts)
    try:
        with open(os.path.join(rd, "drive.log"), encoding="utf-8", errors="replace") as f:
            seen = any(f"devfight: flag {flag} set" in ln for ln in f)
    except OSError:
        pass
    if not seen and after.get(flag) != 1:
        bad.append(f"flag {flag} after the fight: {after.get(flag)} (1 expected)")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--shaders")
    ap.add_argument("bosses", nargs="*")
    a = ap.parse_args()
    todo = [b for b in BOSSES if not a.bosses or b[0] in a.bosses]
    failed = 0
    for boss, step, flag in todo:
        os.environ.update({"BOSS": boss, "STEP": str(step), "FLAG": "" if flag is None else str(flag),
                           "FIGHT_MODE": FIGHT_MODE.get(boss, ""), "INVULNERABLE": "on" if boss in INVULNERABLE else "off",
                           "PAGE": PAGE.get(boss, "")})
        rd, _ = suite.run("devtools_bossfight", a.bin, shaders=a.shaders)
        bad = ["no run directory"] if not rd else check(rd, flag, ARRIVE.get(boss))
        print(f"{boss}: {'PASS' if not bad else 'FAIL'} ({rd})", flush=True)
        for b in bad:
            print("  " + b, flush=True)
        failed += bool(bad)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
