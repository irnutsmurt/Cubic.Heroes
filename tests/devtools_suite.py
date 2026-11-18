#!/usr/bin/env python3
"""The developer tools' regression suite (M11.0, docs/M11_PLAN.md section 3.8), offscreen and silent: each scenario
run through tests/boot_smoke.py (the desktop guard keeps it off the screen), then its own checks.

    tests/devtools_suite.py [--bin EXE] [NAME ...]      (default: every scenario below)
    tests/devtools_suite.py --installed DIR              the install checks (a throwaway install whose settings
                                                         have DOTHEROES_DEVELOPER_MODE=1; never a player's own)

Scenarios (tests/scenarios/<name>.drive):
  devtools_basic   the channel, the Lua runner and its error paths, the run-time open points, the hold; `peek`
  devtools_off     developer mode off: dev.cmd never read, no dev.out, no [dev] line in the log
  devtools_warp    the catalog; a warp to each kind of place, each arriving at its own map number; refusals
  devtools_menu    the in-game menu's Developer pages (pictures dumped): Warp to D_1_1_1 chosen and arrived; Status
  devtools_progress  flags, the story's steps, quests, items; a boss room before and after `progress set`; the
                   first orb's three magic containers given once (`progress set 10` twice)
  devtools_encounter  a boss fought the game's way, the actors, a scene, an event by name, the map again
  devtools_diag    coverage before and after a warp, its export, frame times, the runtime's reports, test states
  devtools_player  life, magic, money through the game's functions; no death on a lethal hit; invulnerable among the
                   river's enemies, a control without, no death at low life
  devtools_boss    temple 1's guardian from its introduction to its defeat (devfight), the life vessel taken, the
                   sage's room and its event: the defeat's flags, more life and money, the orb's flag and items
  devtools_enemies every enemy type created by the tools' `spawn` (the game's own CreateNpc), a picture of each,
                   the session's pipelines exported
  devtools_ending  the ending the game's way from the last enemy's arena to the credits and the cleared title
  devtools_persist flags, the story's steps, money and an item's count set with the tools, the game's own SAVE, back
                   to the title and CONTINUE: the same values; then devtools_persist_load, a new process continuing
                   from those saves (kept as build/checkpoints/m111-persist): the same again
  --installed: devtools_install (the game as a player starts it boots on its empty test saves), then the worker's
    `devsaves --action copy-player` and devtools_install_play (CONTINUE loads the copy and plays); the player's save
    tree (data/gamedata) byte for byte the same after each, the test tree (data/devmode/gamedata) made; then
    devtools_install_off (M11.1: the install's setting at 0 for the run): a command, a life and a place left for it
    and the older test switches that change the game set -- none acted on, the switches named as ignored, no
    developer-mode folder, the player's saves unchanged
A run passes when its drive ran to its end (result.json: "drive done", exit 0, no failure) and its checks hold.
"""
import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(name, exe, installed=None, shaders=None):
    """one scenario through tests/boot_smoke.py; with `shaders` (a preparation's shader folder, M11.1) over a copy of
    it, the warm-up off, every pipeline built logged prepared or not and the new ones' recipes kept in the run"""
    import shutil
    import tempfile
    env = dict(os.environ)
    for k in ("PS3RECOMP_VK_WINDOW", "WINDOW", "VISIBLE", "TOPMOST", "INSTALLED", "DOTHEROES_DEVELOPER_MODE"):
        env.pop(k, None)
    tmp = None
    if shaders:
        tmp = tempfile.mkdtemp(prefix="suite-shaders-", dir=os.path.join(ROOT, "build", "test"))
        shutil.copytree(shaders, os.path.join(tmp, "shaders"))
        env.update({"PS3RECOMP_SPIRV_CACHE": os.path.join(tmp, "shaders"), "DOTHEROES_WARM": "0", "VK_COMPILE_LOG": "1",
                    "DOTHEROES_RECIPES": os.path.join(tmp, "recipes.bin")})
    tag = f"suite-{name}"
    env.update({"BIN": exe, "SCENARIO": name, "RUN_TAG": tag, "ALSOFT_DRIVERS": "null", "KEEP_RUNS": "80",
                "WINDOW": "0", "VISIBLE": "0", "TOPMOST": "0"})
    if installed:
        env["INSTALLED"] = installed
    p = subprocess.run([sys.executable, os.path.join(ROOT, "tests", "boot_smoke.py")], cwd=ROOT, env=env,
                       capture_output=True, text=True)
    runs = sorted(d for d in os.listdir(os.path.join(ROOT, "build", "test")) if d.endswith(tag))
    rd = os.path.join(ROOT, "build", "test", runs[-1]) if runs else None
    if tmp:   # the recipes the run recorded stay with it; the copied preparation goes
        if rd and os.path.exists(os.path.join(tmp, "recipes.bin")):
            shutil.copy2(os.path.join(tmp, "recipes.bin"), os.path.join(rd, "recipes.bin"))
        shutil.rmtree(tmp, ignore_errors=True)
    return rd, p


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
    peeks = [a["out"] for a in answers if a.get("ok") and isinstance(a.get("out"), list) and a["out"] and
             all(isinstance(x, int) for x in a["out"])]
    if len(peeks) < 2 or not peeks[0][0]:   # the Event object's address behind its userdata, then two of its words
        bad.append(f"`peek` (peekobj EventClass.Event): {peeks[:2]}")
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
    lists = [x["out"] for x in answers(rd) if x.get("ok") and isinstance(x.get("out"), list)]
    if not lists or not any(i.get("id") == 4 and i.get("count", 0) >= 1 for i in lists[-1]):
        bad.append("the Boomerang (4) was not given from the menu's Items page")
    return bad


def check_player(rd):
    """the player scenario's answers, in its order"""
    bad = []
    a = [x for x in answers(rd) if x.get("ok") and isinstance(x.get("out"), dict) and "in_play" in x["out"]]
    if len(a) < 17:   # player, life, maxlife, magic, money, heal, nodeath, damage, nodeath, heal, life, invulnerable,
        return [f"{len(a)} player answers, 17 expected"]   # player, invulnerable, nodeath, life, player
    life4, money, damage = a[1]["out"], a[4]["out"], a[7]["out"]
    if life4.get("life") != 4:
        bad.append(f"life 4 read back as {life4.get('life')}")
    if money.get("money") != 1234:
        bad.append(f"money 1234 read back as {money.get('money')}")
    if damage.get("life") != 1 or damage.get("nodeath_saves", 0) < 1:
        bad.append(f"a lethal hit with no death on left life {damage.get('life')} ({damage.get('nodeath_saves')} held)")
    invuln, control = a[12]["out"], a[16]["out"]
    if invuln.get("life") != 10:
        bad.append(f"invulnerable among the enemies: life 10 became {invuln.get('life')}")
    if not 1 <= control.get("life", 0) < 10:
        bad.append(f"the control (invulnerable off, no death on): life {control.get('life')} -- wanted hits landing "
                   f"(below 10) and no death (at least 1)")
    return bad


def check_progress(rd):
    """the progression scenario's answers"""
    bad = []
    a = [x["out"] for x in answers(rd) if x.get("ok")]
    flags = next((o for o in a if isinstance(o, list) and o and "flag" in o[0]), [])
    if not any(f.get("flag") == 0 and f.get("name") for f in flags):
        bad.append("the flags list has no named flag 0 (the scripts' EventFlag names)")
    sets = [o for o in a if isinstance(o, dict) and o.get("flag") == 300 and "value" in o]
    if [s["value"] for s in sets][:3] != [1, 1, 0]:
        bad.append(f"flag 300 set, read, set back: {[s['value'] for s in sets]}")
    one = [o for o in a if isinstance(o, dict) and "id" in o and "count" in o]
    if [(o["id"], o["count"]) for o in one][:3] != [(4, 1), (10, 30), (4, 0)]:
        bad.append(f"give 4, count 10 30, remove 4: {[(o['id'], o['count']) for o in one][:3]}")
    quests = next((o for o in a if isinstance(o, list) and o and "function" in o[0]), [])
    if len(quests) < 20:
        bad.append(f"{len(quests)} quest functions (20 or more expected)")
    st = [o for o in a if isinstance(o, dict) and "event_state" in o and o.get("area") == 38]
    if len(st) < 2 or st[0]["event_state"] == 0 or st[1]["event_state"] != 0:
        bad.append("temple 1's boss room: its event before the story was set past it, none after: "
                   f"{[s['event_state'] for s in st]}")
    snake = [o for o in a if isinstance(o, dict) and o.get("flag") == 6]
    if [s.get("value") for s in snake] != [0, 0, 1]:
        bad.append(f"flag 6 before, after the boss room, after `progress set 9`: {[s.get('value') for s in snake]}")
    mm = [o["max_magic"] for o in a if isinstance(o, dict) and "max_magic" in o][-3:]
    if len(mm) != 3 or mm[1] != mm[0] + 3 or mm[2] != mm[1]:   # the yellow orb's 3 magic containers, given once
        bad.append(f"max magic before `progress set 10`, after it, after it again: {mm}")
    return bad


def check_encounter(rd):
    """the encounter scenario's answers"""
    bad = []
    a = [x["out"] for x in answers(rd) if x.get("ok")]
    bosses = next((o for o in a if isinstance(o, list) and o and "label" in o[0]), [])
    if len(bosses) < 15:
        bad.append(f"{len(bosses)} bosses listed (15 expected)")
    i = next((k for k, o in enumerate(a) if isinstance(o, dict) and o.get("map") == "D_1_2"), None)
    if i is None or not isinstance(a[i + 1], dict) or not a[i + 1].get("event_state"):
        bad.append("temple 1's guardian: no event started in its arena")
    actors = next((o for o in a if isinstance(o, list) and o and "kind" in o[0]), [])
    if not any(x["kind"] == "ENEMY" for x in actors):
        bad.append("no enemy actor in the guardian's arena")
    for what in ("scene", "event"):
        if not any(isinstance(o, dict) and o.get(what) is not None and o.get("started") for o in a):
            bad.append(f"no {what} started")
    resp = [o for o in a if isinstance(o, dict) and o.get("map") == "Field_1"]
    if len(resp) < 2 or resp[-1].get("area") != resp[-1].get("expected"):
        bad.append(f"respawn: {resp[-1] if resp else None}")
    return bad


def check_diag(rd):
    """the diagnostics scenario's answers, and its export"""
    bad = []
    a = [x["out"] for x in answers(rd) if x.get("ok")]
    cov = [o for o in a if isinstance(o, dict) and "built_unforeseen" in o]
    if len(cov) < 2 or cov[1]["built_unforeseen"] <= cov[0]["built_unforeseen"] or not cov[1]["unforeseen_listed"]:
        bad.append(f"coverage before and after a warp: {[(c.get('built_unforeseen'), c.get('unforeseen_listed')) for c in cov]}")
    exp = next((o for o in a if isinstance(o, dict) and "dir" in o), None)
    if not exp or not exp.get("pipelines"):
        bad.append(f"the export: {exp}")
    else:
        try:
            with open(os.path.join(exp["dir"], "coverage.json"), encoding="utf-8") as f:
                listed = json.load(f)
            if not any(p.get("map") == "D_1_1_1" for p in listed):
                bad.append("the export names no pipeline met in D_1_1_1")
        except (OSError, ValueError) as e:
            bad.append(f"the export's coverage.json: {e}")
    perf = next((o for o in a if isinstance(o, dict) and "fps" in o), {})
    if perf.get("frames", 0) < 100 or perf.get("fps", 0) <= 0:
        bad.append(f"perf: {perf}")
    rt = next((o for o in a if isinstance(o, dict) and "written_to_the_log" in o), {})
    if not rt.get("written_to_the_log"):
        bad.append("no native report written")
    if not any(isinstance(o, dict) and o.get("kept") for o in a) or not any(isinstance(o, dict) and o.get("restored") for o in a):
        bad.append("a test state not kept and restored")
    return bad


def persist_reads(rd):
    """the persist run's two reads of the same values: before the save and after the load"""
    reads, cur = [], {}
    for x in answers(rd):
        o = x.get("out")
        if not x.get("ok"):
            continue
        if isinstance(o, list) and o and "flag" in o[0]:
            cur = {"flags": {f["flag"]: f["value"] for f in o}}
            reads.append(cur)
        elif isinstance(o, list) and o and "count" in o[0] and cur:
            cur["items"] = {i["id"]: i["count"] for i in o}
        elif isinstance(o, list) and o and "step" in o[0] and cur:
            cur["steps"] = [p["step"] for p in o if p.get("done")]
        elif isinstance(o, dict) and "money" in o and "nodeath" in o and cur:
            cur["money"] = o["money"]
    return reads


def persist_differences(a, b):
    bad = []
    for k in ("flags", "items", "money", "steps"):
        if a.get(k) != b.get(k):
            if isinstance(a.get(k), dict) and isinstance(b.get(k), dict):
                d = {i: (a[k].get(i), b[k].get(i)) for i in set(a[k]) | set(b[k]) if a[k].get(i) != b[k].get(i)}
                bad.append(f"{k} differ: {dict(sorted(d.items())[:8])}")
            else:
                bad.append(f"{k} differ: {a.get(k)} / {b.get(k)}")
    return bad


def check_persist(rd):
    """devtools_persist: the values set before the save, the same after the game's own load"""
    reads = persist_reads(rd)
    if len(reads) < 2:
        return [f"{len(reads)} reads of the values (2 expected)"]
    saved, loaded = reads[-2], reads[-1]
    bad = []
    want = {145: 9, 221: 5, 250: 1}
    got = {k: saved["flags"].get(k) for k in want}
    if got != want:
        bad.append(f"the flags as set: {got} (wanted {want})")
    if saved.get("money") != 1234:
        bad.append(f"money {saved.get('money')} (1234 set)")
    if saved.get("items", {}).get(10) != 23:
        bad.append(f"item 10's count {saved.get('items', {}).get(10)} (23 set)")
    if not set(range(1, 5)) <= set(saved.get("steps", [])):
        bad.append(f"story steps done {saved.get('steps')} (1-4 set)")
    return bad + persist_differences(saved, loaded)


def check_ending(rd):
    """devtools_ending: the maps the game's ending chain loaded, in order, the credits movie, the cleared title"""
    bad = []
    lines = log_lines(rd)
    maps = []
    for ln in lines:
        m = re.search(r"open '/dev_bdvd/PS3_GAME/USRDIR/data/map/map_alignment/(\w+)\.dat'", ln)
        if m and (not maps or maps[-1] != m.group(1)):
            maps.append(m.group(1))
    after = maps[maps.index("D_7_2_8") + 1:] if "D_7_2_8" in maps else []
    for want in ("D_7_2_9", "Field_90", "Field_91", "D_20_9"):
        if want not in after:
            bad.append(f"the ending never loaded {want} (maps after the last arena: {after})")
    credits = [i for i, ln in enumerate(lines) if re.search(r"CreditMovie_eng\.bik", ln)]
    if not credits:
        bad.append("the credits movie never opened")
    if credits and not any(re.search(r"TitleMovie_Ura", ln) for ln in lines[credits[0]:]):
        bad.append("no cleared-game title movie after the credits")
    return bad


def check_enemies(rd):
    """devtools_enemies: every enemy type created (its `spawn` answered) and an enemy of the game's actor list near
    the hero after each; the coverage exported"""
    bad, spawned, missing = [], [], []
    types = None
    pending = None
    for x in answers(rd):
        o = x.get("out")
        if isinstance(o, list) and o and "rare" in o[0]:
            types = [t["name"] for t in o if not t.get("minigame")]
        elif isinstance(o, dict) and "spawn" in o:
            pending = o["spawn"]
            spawned.append(pending)
        elif isinstance(o, list) and o and "kind" in o[0] and pending:
            hero = next((a for a in o if a["kind"] == "PLAYER"), None)
            near = [a for a in o if a["kind"] == "ENEMY" and a.get("visible") and hero and
                    ((a["x"] - hero["x"]) ** 2 + (a["z"] - hero["z"]) ** 2) ** 0.5 < 14]
            if not near:
                missing.append(pending)
            pending = None
    if not types:
        bad.append("no `spawnables` answer")
    elif set(types) != set(spawned):
        bad.append(f"types not created: {sorted(set(types) - set(spawned))}; created but not listed: "
                   f"{sorted(set(spawned) - set(types))}")
    if missing:
        bad.append(f"no enemy near the hero after creating: {missing}")
    if not any(isinstance(x.get("out"), dict) and "dir" in x["out"] for x in answers(rd)):
        bad.append("no coverage export")
    return bad


def check_boss(rd):
    """devtools_boss: the defeat, the reward and the progression, as the game's own state says"""
    bad = []
    flags, players, items = [], [], []
    for x in answers(rd):
        o = x.get("out")
        if not x.get("ok"):
            continue
        if isinstance(o, list) and o and "flag" in o[0]:
            flags.append({f["flag"]: f["value"] for f in o})
        elif isinstance(o, list) and o and "count" in o[0]:
            items.append({i["id"]: i["count"] for i in o})
        elif isinstance(o, dict) and "max_life" in o:
            players.append(o)
    if len(flags) < 3 or len(players) < 3 or len(items) < 2:
        return [f"{len(flags)} flag reads, {len(players)} player reads, {len(items)} item reads (3, 3, 2 expected)"]
    before, defeated, after = flags[0], flags[1], flags[-1]
    if before.get(6) or before.get(8):
        bad.append(f"the defeat's flags were set before the fight: 6={before.get(6)}, 8={before.get(8)}")
    if defeated.get(6) != 1 or defeated.get(8) != 1:
        bad.append(f"after the fight the defeat's flags 6, 8 = {defeated.get(6)}, {defeated.get(8)} (1, 1 expected)")
    if after.get(7) != 1:
        bad.append(f"after the sage's room the orb's flag 7 = {after.get(7)} (1 expected)")
    p0, p_vessel = players[0], players[-1]   # before the arena, after the sage's room
    if p_vessel.get("max_life", 0) <= p0.get("max_life", 0):
        bad.append(f"the life vessel: max life {p0.get('max_life')} -> {p_vessel.get('max_life')} (more expected)")
    if (p_vessel.get("money") or 0) <= (p0.get("money") or 0):
        bad.append(f"the boss's money: {p0.get('money')} -> {p_vessel.get('money')} (more expected)")
    new_items = sorted(set(items[-1]) - set(items[0]))
    if not new_items:
        bad.append("no item gained in the sage's room")
    return bad


def check_forced(rd):
    """devtools_forced: more of the game's enemies on field 13 with the rare and stronger outcomes forced than with
    the ordinary ones"""
    lists = [x["out"] for x in answers(rd) if x.get("ok") and isinstance(x.get("out"), list) and x["out"]
             and "kind" in x["out"][0]]
    if len(lists) < 4:
        return [f"{len(lists)} actor lists (4 expected)"]
    count = [sum(1 for a in o if a["kind"] == "ENEMY" and a.get("visible")) for o in lists]
    if count[1] <= count[3]:
        return [f"enemies forced {count[1]}, ordinary {count[3]} (more expected forced)"]
    print(f"  enemies on field 13: forced {count[0]}/{count[1]}, ordinary {count[2]}/{count[3]}")
    return []


def check_cubemap(rd):
    """devtools_cubemap (B-008): the cube-mapped material drawn under the Vulkan validation layer with no error; the
    slime created and acted"""
    bad = drive_failures(rd)
    try:
        with open(os.path.join(rd, "stderr.log"), encoding="utf-8", errors="replace") as f:
            log = f.read()
    except OSError:
        return bad + ["no stderr.log"]
    if "vkCreateInstance with the driver's messages failed" in log:
        bad.append("the validation layer did not load (install the Vulkan SDK to run this check)")
    errors = [ln for ln in log.splitlines() if ln.startswith("[vulkan] error")]
    if errors:
        bad.append(f"{len(errors)} Vulkan validation errors, the first: {errors[0][:200]}")
    if "[vk-texture]" in log and " cube " not in log:
        bad.append("no cube map drawn")
    return bad


CHECKS = {"devtools_basic": check_basic, "devtools_off": check_off, "devtools_warp": check_warp,
          "devtools_menu": check_menu, "devtools_player": check_player, "devtools_progress": check_progress,
          "devtools_encounter": check_encounter, "devtools_diag": check_diag, "devtools_persist": check_persist,
          "devtools_ending": check_ending, "devtools_enemies": check_enemies, "devtools_boss": check_boss,
          "devtools_forced": check_forced, "devtools_cubemap": check_cubemap}


def persist_load(rd, exe):
    """part 2: a new process CONTINUEs from part 1's saves (kept as build/checkpoints/m111-persist) and reads them"""
    import shutil
    src = os.path.join(rd, "gamedata", "dev_hdd0", "home", "00000001", "savedata")
    ck = os.path.join(ROOT, "build", "checkpoints", "m111-persist")
    if not os.path.isdir(src):
        return [f"no saves in {src}"]
    shutil.rmtree(ck, ignore_errors=True)
    shutil.copytree(src, ck)
    rd2, _ = run("devtools_persist_load", exe)
    bad = ["no run directory"] if not rd2 else drive_failures(rd2)
    if rd2:
        reads = persist_reads(rd)
        again = persist_reads(rd2)
        if not reads or not again:
            bad.append("no values read in one of the two runs")
        else:
            bad += [f"after a new process: {b}" for b in persist_differences(reads[-1], again[-1])]
    print(f"devtools_persist_load: {'PASS' if not bad else 'FAIL'} ({rd2})")
    return bad


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
    failed += installed_off(inst, exe, player, before)
    return 1 if failed else 0


def check_install_off(rd):
    """developer mode off in an install: the command, the life and the place left alone, the switches ignored"""
    bad = check_off(rd)
    for t in ("life.trigger", "pos.trigger"):
        if not os.path.exists(os.path.join(rd, t)):
            bad.append(f"{t} was taken with developer mode off")
    lines = log_lines(rd)
    for pat, why in ((r"\[dev-state\] life set", "a life was set"), (r"\[dev-state\] hero placed", "the hero was placed"),
                     (r"\[cant-die\] on", "can't-die was on"), (r"\[dev-waver\]", "the waver switch was taken"),
                     (r"\[dev-credits\] (?!the game's Ending (at|on))", "the credits switch acted")):
        hit = [ln for ln in lines if re.search(pat, ln)]
        if hit:
            bad.append(f"{why}: {hit[0][:120]}")
    ign = [ln for ln in lines if "[dev-switches] developer mode is off" in ln]
    if not ign or not all(k in ign[0] for k in ("DOTHEROES_DEV_CANT_DIE", "DOTHEROES_DEV_CREDITS", "DOTHEROES_DEV_WAVER")):
        bad.append("no [dev-switches] line naming the ignored switches")
    if not os.path.exists(os.path.join(rd, "state.txt")):
        bad.append("no state.txt: the read-only state a test follows should still work")
    return bad


def installed_off(inst, exe, player, before):
    """devtools_install_off: the same install with developer mode off (its setting set to 0 for the run, then back)"""
    import shutil
    cfg = os.path.join(inst, "config", "dotheroes.cfg")
    with open(cfg, encoding="utf-8") as f:
        text = f.read()
    devmode = os.path.join(inst, "data", "devmode")
    shutil.rmtree(devmode, ignore_errors=True)
    try:
        with open(cfg, "w", encoding="utf-8", newline="\n") as f:
            f.write(text.replace("DOTHEROES_DEVELOPER_MODE=1", "DOTHEROES_DEVELOPER_MODE=0"))
        rd, _ = run("devtools_install_off", exe, installed=inst)
    finally:
        with open(cfg, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    bad = ["no run directory"] if not rd else drive_failures(rd) + check_install_off(rd)
    after = tree_digest(player)
    if after != before:
        bad.append(f"the player's saves changed ({before[1]} files -> {after[1]})")
    if os.path.exists(devmode):
        bad.append("a developer-mode folder was made with developer mode off")
    print(f"devtools_install_off: {'PASS' if not bad else 'FAIL'} ({rd}); player's saves unchanged: {after == before}")
    for b in bad:
        print("  " + b)
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", default=os.path.join("build", "port-win", "dotheroes.exe"))
    ap.add_argument("--installed", help="a throwaway developer install: the install checks only")
    ap.add_argument("--shaders", help="a preparation's shader folder: the scenarios over a copy of it (M11.1)")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    if a.installed:
        return installed_suite(os.path.abspath(a.installed), a.bin)
    names = a.names or list(CHECKS)
    failed = 0
    for n in names:
        rd, p = run(n, a.bin, shaders=a.shaders)
        bad = ["no run directory"] if not rd else drive_failures(rd)
        if rd:
            bad += CHECKS[n](rd)
        if rd and n == "devtools_persist" and not bad:
            bad += persist_load(rd, a.bin)
        print(f"{n}: {'PASS' if not bad else 'FAIL'} ({rd})")
        for b in bad:
            print("  " + b)
        failed += bool(bad)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
