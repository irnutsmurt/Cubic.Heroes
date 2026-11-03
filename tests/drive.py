#!/usr/bin/env python3
"""Closed-loop input driver for a running test (tests/boot_smoke.py).

Runs a step list against <run dir>: it waits for log markers and for the
game's state, presses buttons through the game's PAD_FILE (<run>/pad.txt),
and saves screenshots as <run>/drive/NN-<name>.png.

  tests/drive.py <run dir> <script>

Script lines (# comments allowed; `#!` lines are tests/boot_smoke.py's
scenario header):
  log <regex> [timeout]       wait for a NEW stdout/stderr line matching: only
                              output written since the last action (press,
                              hold, repeat, touch, segment) counts, so an old
                              line from an earlier cycle cannot satisfy it
  sleep <sec>
  press <BUTTON|0xMASK> [hold]   press; hold = guest pad reports (default 10)
  change [timeout]            wait until the frame differs from the frame at
                              the last press (the game reacted)
  shot <name>                 save a screenshot
  touch <file>                create <run>/<file> (e.g. a capture trigger)
  state <cond>[,<cond>] [timeout]  wait until the game's state (DOTHEROES_DEV_STATE=1,
                              <run>/state.txt) meets every condition, e.g.
                              `state area==18,life>0 60` or `state steps>+20` (a
                              signed value is relative to the step's start); a
                              timeout fails the step
  repeat <n> <BUTTON> <sec>   press n times, <sec> apart
  hold <BUTTON> <sec>         hold for <sec> of wall time (not guest reports:
                              those pass twice as fast at 60 fps); logs where
                              the hero went (hx/hz)
  unstick <DIR> [tries]       a short step in DIR; while the hero does not move (a
                              dialogue open), CROSS and again, up to tries (8)
  cross <DIR> <area> [timeout]  walk on in DIR until the field area is <area> (an
                              edge or a door), sidestepping when blocked; a timeout
                              (default 20 s) fails
  goto <x> <z> [tol] [timeout] [soft]  walk the hero to (x, z) in the area's
                              coordinates (hx/hz: RIGHT is +x, UP is +z), steering
                              by its position every ~0.3 s, within tol (default
                              1.0); blocked (no progress for 2 s), it tries each
                              axis alone, then fails ("stalled at ..."); a
                              timeout (default 30 s) fails too; `soft`: logged,
                              not a failure (back to a spot between fight rounds)
  expect [<cond>[,<cond>]]    invariants, checked every 0.1 s through every later
                              wait until the next `expect` (none: cleared) or the
                              end of the enclosing attempt: a violation fails the
                              step at once ("life>0" fails on a death, "area==18"
                              on leaving the area, "menu==3" on a menu left open:
                              menu is 3 in play, 1 in the system menu)
  actors [<cond>[,...]] [timeout]  the characters near the hero, counted on demand
                              (actors.trigger -> actors.txt: n = all within 40
                              units, c<vtable hex> = one class, e.g. cFE4500,
                              hp = the summed +0x5B8 of those); with conditions,
                              rescanned until they hold
  segment <kind> <name>       begin a named stretch (kind: fight, route, explore,
                              menu, transition, idle, load): a hitch mark
                              (DOTHEROES_DEV_KEYS=1) and the state are recorded
  endseg [<cond>[,...]]       end it; the conditions are on the change over the
                              segment (`endseg steps>+20`: debris physics ran;
                              `hpFE4500<-5`: those characters lost life -- a fight
                              or explore segment counts the characters near the
                              hero at both ends, outside its marks); a miss fails
                              the step. Each segment is a line of
                              <run>/segments.jsonl (tests/batch_report.py)
  block <name> ... end        define a block of steps (run with `call <name>`)
  call <name>
  when <cond>[,<cond>] <name> run block <name> if the state meets the conditions now
                              (a recovery's branches: dead, in a menu, in play)
  loop <n> ... end            run the steps n times (`cycle` in the records)
  attempt <n> [<block>] ... end   run the steps; on a failure, record it
                              (<run>/failures.jsonl), run <block> (the way back to
                              the checkpoint, e.g. back to the title and CONTINUE)
                              and try again, at most n times in all; then the
                              drive stops with status 1
  stop                        end the run now: stop.trigger -- the game writes its
                              logs (DIAG_FRAMES, the frame trace) and exits
  until_exit [<dir>]          wait until the game exits (a session someone plays:
                              the run lasts as long as they do); with <dir>, each
                              time the game saves (a save folder in GAMEDATA's
                              savedata changes), that folder is copied to
                              <dir>/<n>-area<area>/ with checkpoint.json (the
                              time, the state then) -- the game writes every
                              save into the slot it loaded, so each copy keeps
                              one point of progress as a test checkpoint

${NAME} or ${NAME:-default} in a line is replaced from the environment;
`include <file>` (relative to the script) puts another script's lines there
(tests/scenarios/common.drive: the blocks scenarios share).

Buttons: START SELECT CROSS CIRCLE SQUARE TRIANGLE UP DOWN LEFT RIGHT L1 R1 L2 R2 L3 R3,
or a mask (0x0009: SELECT and START together).
Exit status 0 when every step completed, 1 on a failure not recovered (the
step and the reason are in drive.log and failures.jsonl).
"""
import json, os, re, sys, time
from PIL import Image, ImageChops, ImageFile, ImageStat
ImageFile.LOAD_TRUNCATED_IMAGES = True

BTN = {"SELECT": 0x0001, "L3": 0x0002, "R3": 0x0004, "START": 0x0008, "UP": 0x0010, "RIGHT": 0x0020, "DOWN": 0x0040,
       "LEFT": 0x0080, "L2": 0x0100, "R2": 0x0200, "L1": 0x0400, "R1": 0x0800,
       "TRIANGLE": 0x1000, "CIRCLE": 0x2000, "CROSS": 0x4000, "SQUARE": 0x8000}

run, script = sys.argv[1], sys.argv[2]
TIMING = os.environ.get("DRIVE_TIMING") == "1"   # each step's time and each press's waits in drive.log
# ${NAME} or ${NAME:-default}: from the environment (a scenario's cycle count, say)
def load(path, depth=0):
    """The script's lines; `include <file>` (relative to the including script)
    brings another's in its place -- the blocks scenarios share."""
    lines = []
    for n, raw in enumerate(open(path), 1):
        line = re.sub(r"\$\{(\w+)(?::-([^}]*))?\}", lambda m: os.environ.get(m.group(1), m.group(2) or ""),
                      raw.split("#", 1)[0].strip())
        if line.startswith("include ") and depth < 4:
            lines += load(os.path.join(os.path.dirname(path), line.split(None, 1)[1]), depth + 1)
        else:
            lines.append((n, line))
    return lines


LINES = load(script)
# A press keeps the frame it was made on only for a `change` step to compare against. Kept
# for every press, it cost each press ~1 s when frame dumps are off (DUMP=0: no frame.ppm, and
# frame() retries 20 x 50 ms) -- a fight's swings came every ~1.4 s instead of ~0.4 s.
NEEDS_FRAME = any(l.split()[:1] == ["change"] for _, l in LINES)
out = os.path.join(run, "drive"); os.makedirs(out, exist_ok=True)
nshot = [0]
T0 = time.time()


class Fail(Exception):
    """A step that did not do what it should: the reason, for the records."""


def say(text):
    print(f"[drive] {text}", flush=True)


def alive():
    # boot_smoke writes .done only after the game exits
    return not os.path.exists(os.path.join(run, ".done"))


def frame():
    p = os.path.join(run, "frame.ppm")
    for _ in range(20):
        try:
            data = open(p, "rb").read()
            if len(data) >= 1280 * 720 * 3:
                im = Image.open(p); im.load(); return im.convert("RGB")
        except Exception:
            pass
        time.sleep(0.05)
    return None


def shot(name):
    im = frame()
    nshot[0] += 1
    if im:
        im.resize((640, 360)).save(os.path.join(out, f"{nshot[0]:02d}-{name}.png"))


def diff(a, b):
    if a is None or b is None: return 0.0
    d = ImageChops.difference(a.resize((160, 90)), b.resize((160, 90)))
    return sum(ImageStat.Stat(d).mean) / 3.0


# Log waits see only output written since the last action: these are the logs' sizes then.
LOGS = ("stdout.log", "stderr.log")
log_from = {}


def log_sizes():
    sizes = {}
    for f in LOGS:
        try: sizes[f] = os.path.getsize(os.path.join(run, f))
        except OSError: sizes[f] = 0
    return sizes


def acted():
    log_from.update(log_sizes())


def new_log():
    t = ""
    for f in LOGS:
        try:
            with open(os.path.join(run, f), "rb") as h:
                h.seek(log_from.get(f, 0))
                t += h.read().decode("utf-8", "ignore")
        except OSError:
            pass
    return t


def tail_log(n=20):
    try:
        with open(os.path.join(run, "stderr.log"), "rb") as h:
            h.seek(max(0, os.path.getsize(os.path.join(run, "stderr.log")) - 8192))
            return h.read().decode("utf-8", "ignore").splitlines()[-n:]
    except OSError:
        return []


last_press_frame = [None]


def press(mask, hold=10, ms=0):
    t_frame = time.time()
    if NEEDS_FRAME:   # only `change` compares with it; with frame dumps off, frame() waits ~1 s for nothing
        last_press_frame[0] = frame()
    t_write = time.time()
    acted()
    with open(os.path.join(run, "pad.txt"), "w") as f:
        f.write(f"0x{mask:04X} {hold} {ms}\n" if ms else f"0x{mask:04X} {hold}\n")
    # wait for the game to take it (the file is emptied on admission)
    t0 = time.time()
    while time.time() - t0 < 10:
        if os.path.getsize(os.path.join(run, "pad.txt")) == 0:
            if TIMING:
                say(f"  +{t_write - T0:.3f} s 0x{mask:04X} {hold} {ms}: frame grab {1000 * (t_write - t_frame):.0f} ms, "
                    f"taken after {1000 * (time.time() - t0):.0f} ms")
            return True
        check()
        time.sleep(0.05)
    return False


def mask_of(tok):
    return int(tok, 0) if tok.startswith("0x") else BTN[tok.upper()]


# DOTHEROES_DEV_STATE=1 (port/native/dev_state.cpp): <run>/state.txt, rewritten every 100 ms.
# Older builds write only the first seven.
STATE_KEYS = ("t", "life", "x", "z", "steps", "topups", "area", "hx", "hy", "hz", "menu", "scene", "frames")
CMP = {"<=": lambda a, b: a <= b, ">=": lambda a, b: a >= b, "==": lambda a, b: a == b,
       "!=": lambda a, b: a != b, "<": lambda a, b: a < b, ">": lambda a, b: a > b}


def state():
    try:
        f = open(os.path.join(run, "state.txt")).read().split()
        return dict(zip(STATE_KEYS, map(float, f))) if len(f) >= 7 else None
    except (OSError, ValueError):
        return None


def fmt_state(s):
    return " ".join(f"{k}={s[k]:g}" for k in STATE_KEYS if s and k in s) if s else "none"


def cond(text):   # key op value; a value with a sign (+5, -3) is relative to a reference state
    m = re.fullmatch(r"(\w+)(<=|>=|==|!=|<|>)([+-]?)([\dx.A-Fa-f]+)", text)
    if not m:
        sys.exit(f"[drive] bad condition {text!r}")
    k, o, sign, v = m.groups()
    return k, o, float(int(v, 16) if v.startswith("0x") else float(v)) * (-1 if sign == "-" else 1), bool(sign)


def conds(text):
    return [cond(c) for c in text.split(",") if c]


def holds(cs, s, ref=None):
    """The conditions that fail on s (relative ones against ref): a list of texts."""
    bad = []
    for k, o, v, rel in cs:
        if k not in s and re.fullmatch(r"(c|hp)[0-9A-F]+", k):
            s = dict(s, **{k: 0.0})   # a class not counted: none of them near
        if k not in s:
            bad.append(f"{k}: not reported"); continue
        want = (ref[k] + v) if rel and ref and k in ref else v
        if not CMP[o](s[k], want):
            bad.append(f"{k}={s[k]:g} (wanted {o}{want:g})")
    return bad


# Invariants (`expect`), checked through every wait.
expecting = []


def check():
    if not alive():
        raise Fail("the game exited")
    if expecting:
        s = state()
        if s:
            bad = holds(expecting, s)
            if bad:
                raise Fail("expected " + ", ".join(bad))


def wait(sec):
    end = time.time() + sec
    while True:
        check()
        left = end - time.time()
        if left <= 0:
            return
        time.sleep(min(0.1, left))


# Characters near the hero, on demand (actors.trigger -> actors.txt).
def actors():
    p, t = os.path.join(run, "actors.txt"), os.path.join(run, "actors.trigger")
    try: before = os.path.getmtime(p)
    except OSError: before = 0
    open(t, "w").close()
    t0 = time.time()
    while time.time() - t0 < 5:
        try:
            if os.path.getmtime(p) != before and not os.path.exists(t):
                vals = {}
                for tok in open(p).read().split():
                    k, _, v = tok.partition("=")
                    vals[k] = float(v)
                return vals
        except (OSError, ValueError):
            pass
        time.sleep(0.05)
    return None


# Segments: hitch marks (with DOTHEROES_DEV_KEYS=1) and the state at both ends.
def mark():
    """A hitch mark; its time on the trace's clock (from its fresh log line), or None."""
    acted()
    open(os.path.join(run, "mark.trigger"), "w").close()
    t0 = time.time()
    while time.time() - t0 < 2:
        m = re.search(r"\[mark\] (\d+) at ([\d.]+) s", new_log())
        if m:
            return int(m.group(1)), float(m.group(2))
        time.sleep(0.02)
    return None, None


ctx = {"cycle": None, "attempt": None, "segment": None}


def record(name, rec):
    with open(os.path.join(run, name), "a") as f:
        f.write(json.dumps(rec) + "\n")


COUNTED = ("fight", "explore")   # segments whose characters are counted at both ends


def seg_begin(kind, name):
    a0 = actors() if kind in COUNTED else None   # before the mark: the heap scan stays out of the segment
    n, t = mark()
    ctx["segment"] = {"kind": kind, "name": name, "cycle": ctx["cycle"], "attempt": ctx["attempt"],
                      "mark0": n, "t0": t, "wall0": round(time.time() - T0, 3), "s0": state(), "a0": a0}


def seg_end(text, lineno):
    seg = ctx["segment"]
    if not seg:
        raise Fail("endseg without a segment")
    n, t = mark()
    s1 = state()
    a1 = actors() if seg["kind"] in COUNTED else None
    seg.update(mark1=n, t1=t, wall1=round(time.time() - T0, 3), s1=s1, a1=a1, ok=True)
    # conditions on the state and on the characters counted (c<class>, hp<class>, n) alike
    end, start = dict(s1 or {}, **(a1 or {})), dict(seg["s0"] or {}, **(seg["a0"] or {}))
    bad = holds(conds(text), end, start) if text and s1 else ([] if not text else ["no state"])
    if bad:
        seg.update(ok=False, why="ended without " + ", ".join(bad))
    record("segments.jsonl", seg)
    ctx["segment"] = None
    say(f"segment {seg['kind']} {seg['name']}: {seg['wall1'] - seg['wall0']:.1f} s, "
        f"{'ok' if seg['ok'] else 'FAILED: ' + seg['why']} ({fmt_state(s1)})")
    if bad:
        raise Fail(seg["why"])


def fail_record(lineno, line, why):
    s = state()
    rec = {"wall": round(time.time() - T0, 3), "line": lineno, "step": line, "why": why, "state": s,
           "cycle": ctx["cycle"], "attempt": ctx["attempt"],
           "segment": ctx["segment"] and {k: ctx["segment"][k] for k in ("kind", "name", "wall0")},
           "log": tail_log(12)}
    grabs = sorted(os.listdir(os.path.join(run, "grabs"))) if os.path.isdir(os.path.join(run, "grabs")) else []
    rec["last_grab"] = grabs[-1] if grabs else None
    record("failures.jsonl", rec)
    if ctx["segment"]:   # the open segment, recorded as failed
        seg = dict(ctx["segment"], wall1=rec["wall"], s1=s, ok=False, why=why)
        record("segments.jsonl", seg)
        ctx["segment"] = None
    say(f"FAILED line {lineno}: {line}: {why} ({fmt_state(s)})")


def unstick(direction, tries):
    """A dialogue open freezes the hero (the system menu does not open then
    either): try a short step in `direction`; while the hero does not move,
    page with CROSS and try again. Fails when still frozen after `tries`."""
    for k in range(tries):
        s0 = state()
        press(BTN[direction], 1, 200); wait(0.4)
        s1 = state()
        if s0 and s1 and "hx" in s0 and abs(s1["hx"] - s0["hx"]) + abs(s1["hz"] - s0["hz"]) > 0.3:
            if k:
                say(f"  free after {k} page(s)")
            return
        press(BTN["CROSS"]); wait(0.7)
    raise Fail(f"unstick {direction}: the hero did not move after {tries} tries (a dialogue that does not close?)")


def cross(direction, area, tmo):
    """Walk on in `direction` until the field area is `area` (an area's edge or
    a door): short holds, and when the hero stops moving -- an enemy, a corner
    -- a step to each side in turn before going on."""
    t0, last, still = time.time(), None, time.time()
    side = ("UP", "DOWN") if direction in ("LEFT", "RIGHT") else ("LEFT", "RIGHT")
    turn = 0
    while True:
        s = state()
        if s and s["area"] == area:
            say(f"  in area {area:g}")
            return
        if time.time() - t0 > tmo:
            raise Fail(f"cross {direction} into area {area:g}: still in area {s['area']:g} at "
                       f"({s.get('hx', 0):.2f}, {s.get('hz', 0):.2f}) after {tmo:g} s" if s else "cross: no state")
        here = (s["hx"], s["hz"]) if s and "hx" in s else None
        if here and last and abs(here[0] - last[0]) + abs(here[1] - last[1]) > 0.2:
            still = time.time()
        last = here
        if time.time() - still > 1.5:   # blocked: a sidestep, the other way next time
            press(BTN[side[turn % 2]], 1, 400); wait(0.4)
            turn += 1
            still = time.time()
        press(BTN[direction], 1, 300)
        wait(0.3)


def goto(x, z, tol, tmo):
    """Steer the hero to (x, z): short holds toward it (diagonals as two
    directions), re-aimed from its position each time. Blocked for 2 s, it
    walks one axis alone (the longer first, then the other); still blocked, it
    fails with where the hero stopped."""
    t0, best, best_t, axis = time.time(), None, time.time(), None
    while True:
        s = state()
        if not s or "hx" not in s or (s["hx"] == 0 and s["hz"] == 0):
            if time.time() - t0 > tmo:
                raise Fail("goto: the hero's position is not known")
            wait(0.2); continue
        dx, dz = x - s["hx"], z - s["hz"]
        dist = (dx * dx + dz * dz) ** 0.5
        if abs(dx) <= tol and abs(dz) <= tol:
            say(f"  at ({s['hx']:.2f}, {s['hz']:.2f}) area {s['area']:g}")
            return
        if time.time() - t0 > tmo:
            raise Fail(f"goto ({x:g}, {z:g}): timed out at ({s['hx']:.2f}, {s['hz']:.2f})")
        if best is None or dist < best - 0.2:
            best, best_t = dist, time.time()
        elif time.time() - best_t > 2.0:   # blocked: one axis alone, the longer first, then the other
            axis = {None: "x" if abs(dx) >= abs(dz) else "z", "x": "z", "z": "x"}.get(axis, "stuck")
            if axis == "stuck" or (axis == "x" and abs(dx) <= tol) or (axis == "z" and abs(dz) <= tol):
                raise Fail(f"goto ({x:g}, {z:g}): stalled at ({s['hx']:.2f}, {s['hz']:.2f}), {dist:.1f} away")
            say(f"  blocked at ({s['hx']:.2f}, {s['hz']:.2f}): along {axis} alone")
            best, best_t = dist, time.time()
        mask = 0
        # A small offset on the other axis waits until the long one is nearly
        # done, as a player walks: cutting the corner diagonally left the
        # owner's line through town and ran into a fence.
        lead = None if axis else ("x" if abs(dz) < 0.25 * abs(dx) and abs(dx) > 3 * tol else
                                  "z" if abs(dx) < 0.25 * abs(dz) and abs(dz) > 3 * tol else None)
        if axis != "z" and lead != "z" and abs(dx) > tol / 2:
            mask |= BTN["RIGHT"] if dx > 0 else BTN["LEFT"]
        if axis != "x" and lead != "x" and abs(dz) > tol / 2:
            mask |= BTN["UP"] if dz > 0 else BTN["DOWN"]
        if not mask:   # the chosen axis is done: back to both
            axis = None
            continue
        left = (abs(dx) if mask & (BTN["RIGHT"] | BTN["LEFT"]) and not mask & (BTN["UP"] | BTN["DOWN"])
                else abs(dz) if not mask & (BTN["RIGHT"] | BTN["LEFT"]) else dist)
        if left < 1.0:   # the last stretch in pad reports (~0.15 units each at 9 a second): narrow ways want the lane
            press(mask, max(1, int(left / 0.15)))
            wait(0.15)
            continue
        secs = max(0.05, min(0.3, left / 9.0))
        press(mask, 1, int(secs * 1000))
        wait(secs)


def keep_saves(dest):
    """until_exit: wait for the game to exit; with dest, copy each save the game
    makes (its folder under GAMEDATA's savedata, once its files stop changing)."""
    import shutil
    root = os.path.join(os.environ.get("GAMEDATA", ""), "dev_hdd0", "home", "00000001", "savedata")

    def stamps():
        out = {}
        try:
            for d in os.listdir(root):
                p = os.path.join(root, d)
                if d.startswith("BLUS30490") and os.path.isdir(p):
                    out[d] = max((os.path.getmtime(os.path.join(p, f)) for f in os.listdir(p)), default=0)
        except OSError:
            pass
        return out
    seen, pending = stamps(), {}
    n = len(os.listdir(dest)) if dest and os.path.isdir(dest) else 0   # numbering goes on across sessions
    while alive():
        time.sleep(1)
        if not dest or not os.environ.get("GAMEDATA"):
            continue
        now = stamps()
        for d, t in now.items():
            if t != seen.get(d) and pending.get(d, (None,))[0] != t:   # new, or changed again: wait from now
                pending[d] = (t, time.time())
        for d, (t, since) in list(pending.items()):
            if time.time() - since < 3 or now.get(d) != t:   # still being written
                continue
            del pending[d]
            seen[d] = t
            s = state() or {}
            n += 1
            out = os.path.join(dest, f"{n:02d}-area{int(s.get('area', 0))}")
            os.makedirs(out, exist_ok=True)
            shutil.copytree(os.path.join(root, d), os.path.join(out, d), dirs_exist_ok=True)
            with open(os.path.join(out, "checkpoint.json"), "w") as f:
                json.dump({"saved": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t)), "folder": d,
                           "state": s, "run": os.path.basename(run),
                           "start": "title -> CONTINUE loads the newest save: copy this folder (and the "
                                    "BLUS30490SYSTEM folder of the session's root) into a run's savedata"}, f, indent=1)
            say(f"save {n}: {d} -> {out} ({fmt_state(s)})")


# The script as a tree: blocks, loops and attempts close with `end`.
def parse(i=0, closing=False):
    body = []
    while i < len(LINES):
        n, line = LINES[i]
        i += 1
        if not line or line.startswith("!"):
            continue
        op, *a = line.split()
        if op == "end":
            if not closing: sys.exit(f"[drive] `end` without a block at line {n}")
            return body, i
        if op in ("block", "loop", "attempt"):
            inner, i = parse(i, True)
            body.append((n, line, op, a, inner))
        else:
            body.append((n, line, op, a, None))
    if closing: sys.exit("[drive] a block, loop or attempt without `end`")
    return body, i


blocks = {}


def run_steps(steps):
    global expecting
    for n, line, op, a, inner in steps:
        if op == "block":
            blocks[a[0]] = inner; continue
        say(f"{n}: {line}" + (f"  (+{time.time() - T0:.3f} s)" if TIMING else ""))
        if op == "loop":
            outer = ctx["cycle"] is None   # the outermost loop counts the cycles
            for c in range(int(a[0])):
                if outer:
                    ctx["cycle"] = c + 1
                    say(f"cycle {c + 1} of {a[0]}")
                run_steps(inner)
            if outer:
                ctx["cycle"] = None
        elif op == "attempt":
            tries, recover = int(a[0]), a[1] if len(a) > 1 else None
            saved = expecting
            for k in range(1, tries + 1):
                ctx["attempt"] = k
                try:
                    run_steps(inner)
                    break
                except Fail as e:
                    fail_record(*getattr(e, "where", (n, line)), str(e))
                    expecting = []
                    if k == tries or not alive():
                        ctx["attempt"] = None
                        raise Fail(f"attempt at line {n} failed {k} time(s): {e}")
                    if recover:
                        say(f"recovering: {recover} (attempt {k + 1} of {tries} next)")
                        run_steps(blocks[recover])
                finally:
                    expecting = saved
            ctx["attempt"] = None
        elif op == "call":
            run_steps(blocks[a[0]])
        elif op == "when":
            s = state()
            if s and not holds(conds(a[0]), s):
                run_steps(blocks[a[1]])
        else:
            try:
                step(n, line, op, a)
            except Fail as e:
                if not hasattr(e, "where"):
                    e.where = (n, line)
                raise


def step(lineno, line, op, a):
    global expecting
    if op == "log":
        rx, tmo = re.compile(a[0]), float(a[1]) if len(a) > 1 else 120
        t0 = time.time()
        while not rx.search(new_log()):
            if time.time() - t0 > tmo:
                shot("timeout"); raise Fail(f"no new log line matching {a[0]!r} in {tmo:g} s")
            wait(0.25)
    elif op == "sleep":
        wait(float(a[0]))
    elif op == "press":
        if not press(mask_of(a[0]), int(a[1]) if len(a) > 1 else 10):
            say(f"press not taken at line {lineno}")
    elif op == "hold":
        s0 = state()
        press(mask_of(a[0]), 1, int(float(a[1]) * 1000)); wait(float(a[1]))
        s1 = state()
        if s0 and s1 and "hx" in s0:
            say(f"  moved {s1['hx'] - s0['hx']:+.2f} {s1['hz'] - s0['hz']:+.2f} to ({s1['hx']:.2f}, {s1['hz']:.2f})"
                f" area {s1['area']:g}")
    elif op == "unstick":
        unstick(a[0].upper(), int(a[1]) if len(a) > 1 else 8)
    elif op == "cross":
        cross(a[0].upper(), float(a[1]), float(a[2]) if len(a) > 2 else 20)
    elif op == "goto":
        soft = a[-1] == "soft"
        a = a[:-1] if soft else a
        try:
            goto(float(a[0]), float(a[1]), float(a[2]) if len(a) > 2 else 1.0, float(a[3]) if len(a) > 3 else 30)
        except Fail as e:
            if not soft or not alive():
                raise
            say(f"  (soft) {e}")
    elif op == "repeat":
        for _ in range(int(a[0])):
            press(mask_of(a[1])); wait(float(a[2]))
    elif op == "change":
        tmo = float(a[0]) if a else 20; t0 = time.time()
        # A small dialog over a static screen changes few pixels: low threshold,
        # and a miss is reported but not fatal (the next step may still apply).
        while diff(frame(), last_press_frame[0]) < 1.0:
            if time.time() - t0 > tmo:
                say(f"no screen change after line {lineno} (continuing)"); break
            wait(0.25)
    elif op == "state":
        cs, tmo = conds(a[0]), float(a[1]) if len(a) > 1 else 60
        t0, s0, s = time.time(), None, None
        while True:
            s = state()
            if s is not None:
                s0 = s0 or s
                if not holds(cs, s, s0):
                    say(f"state {fmt_state(s)}")
                    break
            if time.time() - t0 > tmo:
                raise Fail(f"state not reached in {tmo:g} s: " + ", ".join(holds(cs, s, s0) if s else ["no state"]))
            wait(0.1)
    elif op == "expect":
        expecting = conds(a[0]) if a else []
        check()
    elif op == "actors":
        cs = conds(a[0]) if a and not a[0][0].isdigit() else []
        tmo = float(a[-1]) if a and a[-1][0].isdigit() else 5
        t0 = time.time()
        while True:
            v = actors()
            if v is None:
                raise Fail("no actors.txt (DOTHEROES_DEV_STATE=1?)")
            bad = holds(cs, v)
            if not bad:
                say("actors " + " ".join(f"{k}={x:g}" for k, x in v.items()))
                if ctx["segment"] is not None:
                    ctx["segment"].setdefault("actors", []).append(v)
                ctx["last_actors"] = v
                break
            if time.time() - t0 > tmo:
                raise Fail("actors: " + ", ".join(bad))
            wait(0.5)
    elif op == "segment":
        seg_begin(a[0], a[1] if len(a) > 1 else a[0])
    elif op == "endseg":
        seg_end(a[0] if a else "", lineno)
    elif op == "touch":
        acted()
        open(os.path.join(run, a[0]), "w").close()
    elif op == "shot":
        shot(a[0] if a else f"line{lineno}")
    elif op == "stop":
        open(os.path.join(run, "stop.trigger"), "w").close()
        say("stop")
    elif op == "until_exit":
        keep_saves(a[0] if a else None)
    else:
        sys.exit(f"[drive] bad op {op!r} at line {lineno}")


steps, _ = parse()
acted()
status = 0
try:
    run_steps(steps)
    say("done")
except Fail as e:
    if ctx["attempt"] is None and not str(e).startswith("attempt at line"):
        fail_record(*getattr(e, "where", (0, "?")), str(e))
    say(f"STOPPED: {e}")
    status = 1
shot("end")
sys.exit(status)
