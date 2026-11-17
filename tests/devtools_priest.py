#!/usr/bin/env python3
"""The priest's offer in the tower, its three outcomes (M11.1b, B-009; docs/M11_PLAN.md section 27), offscreen and silent,
each through tests/scenarios/devtools_priest.drive (tests/devtools_suite.py's runner):

  accept-continue   his offer accepted twice, the game's own game over, "Continue?" YES: the game goes on (it does not
                    end itself), the hero back at the tower's entrance (D_7_1_1, area 63) with his life, his items, his
                    money and the story's flags as before the room; the saves as the run began with them
  accept-decline    accepted twice, "Continue?" NO, "Save your progress?" NO: the game goes on, to its title (the hero
                    no longer in play); the saves as the run began with them
  refuse            refused twice: the fight begins (the priest and his summons among the game's actors), the game goes on
  bag-door          a sleeping bag tried outside his door first: the game refuses it there ("It won't work here": the
                    bag kept), then as accept-continue -- the tower's one resurrection point, its entrance (area 63)

    tests/devtools_priest.py --bin EXE [--shaders DIR] [CASE ...]     (default: all three)
"""
import argparse
import filecmp
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import devtools_suite as suite  # noqa: E402

# K1 brings up "Continue?" over the game over; the rest answer it (CROSS takes the choice under the cursor, YES first)
CASES = {
    "accept-continue": {"PAGE": "", "K1": "CROSS", "K2": "CROSS", "K3": "CROSS", "K4": "CROSS", "K5": "CROSS"},
    "accept-decline": {"PAGE": "", "K1": "CROSS", "K2": "DOWN", "K3": "CROSS", "K4": "DOWN", "K5": "CROSS"},
    "refuse": {"PAGE": "no", "K1": "CROSS", "K2": "CROSS", "K3": "CROSS", "K4": "CROSS", "K5": "CROSS"},
    "bag-door": {"PAGE": "", "BAG": "door", "K1": "CROSS", "K2": "CROSS", "K3": "CROSS", "K4": "CROSS", "K5": "CROSS"},
}
DEFAULT = ("accept-continue", "accept-decline", "refuse", "bag-door")
TOWER_ENTRANCE = 63   # D_7_1_1
SAVES = os.path.join(suite.ROOT, "build", "checkpoints", "castle-front")   # the scenario's `#! saves`


def saves_changed(rd):
    """the game's save files (not its system file) that differ from those the run began with"""
    home = os.path.join(rd, "gamedata", "dev_hdd0", "home", "00000001", "savedata")
    out = []
    for d in sorted(os.listdir(SAVES)):
        if not os.path.isdir(os.path.join(SAVES, d)) or d.endswith("SYSTEM"):
            continue
        for f in sorted(os.listdir(os.path.join(SAVES, d))):
            a, b = os.path.join(SAVES, d, f), os.path.join(home, d, f)
            if not os.path.exists(b) or not filecmp.cmp(a, b, shallow=False):
                out.append(f"{d}/{f}")
    return out


def outs(rd, key):
    return [x["out"] for x in suite.answers(rd) if x.get("ok") and isinstance(x.get("out"), (dict, list)) and key(x["out"])]


def check(rd, case):
    bad = []
    try:
        res = json.load(open(os.path.join(rd, "result.json"), encoding="utf-8"))
    except OSError:
        return ["no result.json"]
    if res.get("ended_by") == "game exited":
        bad.append("the game ended itself")
    bad += [f for f in suite.drive_failures(rd) if "the game exited" not in f]
    status = outs(rd, lambda o: isinstance(o, dict) and "event_state" in o)
    player = outs(rd, lambda o: isinstance(o, dict) and "max_life" in o)
    flags = outs(rd, lambda o: isinstance(o, list) and o and "flag" in o[0])
    items = outs(rd, lambda o: isinstance(o, list) and o and "count" in o[0])
    enemies = outs(rd, lambda o: isinstance(o, list) and o and "kind" in o[0])
    if case.startswith("accept"):
        changed = saves_changed(rd)
        if changed:
            bad.append(f"saves changed: {changed}")
    if case == "accept-decline":   # at the title the tools' player answer is short: in_play false, no life
        playing = outs(rd, lambda o: isinstance(o, dict) and "in_play" in o)
        if not playing or playing[-1]["in_play"] is not False:
            bad.append(f"the hero still in play after declining ({playing[-1]['in_play'] if playing else None})")
    if case in ("accept-continue", "bag-door"):
        if not status or status[-1].get("area") != TOWER_ENTRANCE:
            bad.append(f"area after continuing: {status[-1].get('area') if status else None} ({TOWER_ENTRANCE} expected)")
        if len(flags) >= 2:
            before = {f["flag"]: f["value"] for f in flags[0]}
            after = {f["flag"]: f["value"] for f in flags[-1]}
            changed = {k: (before.get(k), v) for k, v in after.items() if before.get(k) != v}
            if changed:
                bad.append(f"flags changed: {changed}")
        else:
            bad.append(f"{len(flags)} flag reads")
        # the last reads before the room (after the bag was tried, in bag-door) against those after continuing
        if len(items) >= 2 and items[-2] != items[-1]:
            bad.append("items changed")
        bags = [sum(i["count"] for i in x if i["name"] == "SLEEPING_BAG") for x in items]
        if case == "bag-door" and (len(bags) < 3 or bags[-2] != bags[0] + 1):
            bad.append(f"the sleeping bag used outside the door, or not given (sleeping bags: {bags})")
        if len(player) >= 2:
            if player[-1].get("money") != player[-2].get("money"):
                bad.append(f"money {player[-2].get('money')} -> {player[-1].get('money')}")
            if player[-1].get("life") != player[-1].get("max_life"):
                bad.append(f"life {player[-1].get('life')} of {player[-1].get('max_life')} after continuing")
    elif case == "refuse":
        if not enemies or not any(e["kind"] == "ENEMY" and e.get("visible") for e in enemies[-1]):
            bad.append("no enemy in the room after refusing (the fight did not begin)")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--shaders")
    ap.add_argument("cases", nargs="*")
    a = ap.parse_args()
    failed = 0
    for case in a.cases or DEFAULT:
        os.environ.update(CASES[case])
        rd, _ = suite.run("devtools_priest", a.bin, shaders=a.shaders)
        bad = ["no run directory"] if not rd else check(rd, case)
        print(f"{case}: {'PASS' if not bad else 'FAIL'} ({rd})", flush=True)
        for b in bad:
            print("  " + b, flush=True)
        failed += bool(bad)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
