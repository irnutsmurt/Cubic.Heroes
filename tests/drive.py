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
  pressuntil <regex> <timeout> <BUTTON> [<every>]  press BUTTON every <every> s (1.5) until a new log
                              line matches (a long event chain paged through to a marker; nothing is
                              pressed after it); a timeout fails
  press <BUTTON|0xMASK> [hold]   press; hold = guest pad reports (default 10)
  change [timeout]            wait until the frame differs from the frame at
                              the last press (the game reacted)
  shot <name>                 save a screenshot
  touch <file>                create <run>/<file> (e.g. a capture trigger)
  life <n>                    set the hero's life to n half apples, once (DOTHEROES_DEV_STATE;
                              route discovery: full life's long sword reaches targets the game
                              puts out of reach -- never in a measured run)
  state <cond>[,<cond>] [timeout]  wait until the game's state (DOTHEROES_DEV_STATE=1,
                              <run>/state.txt) meets every condition, e.g.
                              `state area==18,life>0 60` or `state steps>+20` (a
                              signed value is relative to the step's start); a
                              timeout fails the step
  dev <command ...> [@<timeout>]  a developer-tools command (M11.0, docs/M11_PLAN.md:
                              DOTHEROES_DEVELOPER_MODE=1; tests/devtools.py): its answer
                              logged; a refusal or no answer (30 s) fails the step
  devtry <command ...> [@<timeout>]  as `dev`, but a refusal or no answer is logged and the script
                              goes on (a sweep's map that does not arrive: tests/map_sweep.py's check fails it)
  devfail <command ...>       a developer-tools command that must be refused (an error path)
  devpage <timeout> [soft] [ifany] [no]  CROSS every 1.2 s until the game runs no event (the tools' status
                              event_state 0); a timeout fails (`soft`: logged); `ifany`: no press at all when no
                              event runs (a hero standing in a fight: a press would swing the sword); `no`: a
                              question (event_state 6, Event:Select) answered with its second choice (DOWN, CROSS)
  devcollect [timeout]        walk over every visible ITEM the tools' `enemies` lists (dropped money, a life
                              vessel), nearest first; one not reached in 6 s passed over
  devfight <timeout> [flagN] [soft] [hold@X,Z] [face@DIR] [aim[@REACH] [wire] [map@FILE]]  fight what the tools'
                              `enemies` lists (the game's
                              own actors: a boss's parts too): the nearest visible ENEMY, walked to and swung at, until
                              none is left or the event flag N is set (a boss's defeat); a timeout fails (`soft`:
                              logged); hold@X,Z: from that spot, only what comes within reach (a boss that comes to
                              the hero); face@DIR (with hold: UP, DOWN, LEFT or RIGHT): every swing that way (a
                              boss's body in line with the spot); aim: the boss's body -- the first visible ENEMY when
                              the fight starts, its parts made after it -- followed: the hero steps toward its row or
                              column (the other way when a step gets nowhere), turns to it and swings, the sword's
                              reach along the line (a piercing sword through the parts between); aim@REACH: within
                              REACH units of it, off its line, the swings sweep toward it from both ways; wire: out of
                              reach, the Wire Rod (selected in the circle slot) fired at an anchor pile in line whose
                              platform is nearer it (into the pile's line first); map@FILE (with aim@REACH): the
                              arena's map (devexplore's) -- out of reach, the fewest walks and pulls to a cell in
                              reach, followed; cast@ICON,SECS: the spell the icon shows (beside the drive) put in the
                              circle slot and cast at once and every SECS s (CIRCLE); collect: an item dropped within
                              6 units walked over; farm@COST (with cast@): the hero's magic read, the spell cast only
                              with COST, below it and the light gone the boss's summons fought (the actors that came
                              after the fight began: their drops), collect picking up; cycle@SECS: a body that took no
                              wound for SECS s passed over for the boss's next actor (a wound: life@OFF, the body
                              actor's own life at OFF -- the priest's +0x5BC -- lower than at its last read); in hold and
                              aim modes each change in the number of visible enemies is logged (a boss's parts cut,
                              grown again, added)
  select <BUTTON> <ref.png> [max]  BUTTON pressed (R2: the circle slot's next) until the HUD's circle slot shows the
                              reference picture (beside the drive: a frame's slot, 640x360 at SLOT_BOX), at most max
                              times (12); not shown: a failure
  devexplore <out.json> [timeout]  the arena's ways, for devfight's map@: from the hero's cell each neighbour walked
                              to once, and from each known cell in line with an anchor pile within the Wire Rod's
                              reach the rod fired once (the circle slot holding it); <run>/<out.json>
  write <file> <text ...>     <run>/<file> written with the text (whole, at once)
  devstate <cond>[,<cond>] [timeout]  as `state`, on the developer tools' `status`
                              (area, frames, hud_life, money, event_state, ...); flagN is
                              the game's event flag N (`flag get N`), e.g. devstate flag42==1
  repeat <n> <BUTTON> <sec>   press n times, <sec> apart
  hold <BUTTON> <sec>         hold for <sec> of wall time (not guest reports:
                              those pass twice as fast at 60 fps); logs where
                              the hero went (hx/hz)
  unstick <DIR> [tries] [soft]  a short step in DIR; while the hero does not move (a
                              dialogue open), CROSS and again, up to tries (8)
  actors list                 every character and object within 40 units, nearest
                              first: class, position, life
  open <class> [tries] [soft] open what objects of the class are (a chest): up to
                              each from the south, facing it, CROSS, until the
                              class's count falls (its item taken); tries (10);
                              `soft`: none there or none opened is logged, not a failure
  fight <class>|any [timeout] [reach]  clear one class (or every one) of characters near the hero
                              (e.g. FE4500): walk to the nearest with life left, turn
                              to it and swing, until none within 40 units has life;
                              fails after timeout (60 s) with how many are left;
                              only those within 25 units of where it began, and
                              not one 25 swings did not hurt or 6 s brought no
                              nearer; `soft`: a timeout is logged, not a failure
  cross <DIR> <area|any> [timeout] [soft]  walk on in DIR until the field area is <area> (`any`: another; an
                              edge or a door), sidestepping when blocked; a timeout
                              (default 20 s) fails
  switches                    the boomerang at each wall switch (FE6CC0) in the hero's
                              room, from in line with it (`rooms` grid needed)
  rooms <w> <h> | off         a dungeon of w x h rooms, a door at the middle of each
                              wall: a goto into the next room that stalls fights what is
                              left near (an enemy left bars a room's doors), then goes
                              through that wall's door (a point before it, one past) and on
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
                              one point of progress as a test checkpoint; the
                              edit data (BLJM60180*: edited heroes) goes with it

${NAME} or ${NAME:-default} in a line is replaced from the environment;
`include <file>` (relative to the script) puts another script's lines there
(tests/scenarios/common.drive: the blocks scenarios share).

Buttons: START SELECT CROSS CIRCLE SQUARE TRIANGLE UP DOWN LEFT RIGHT L1 R1 L2 R2 L3 R3,
or a mask (0x0009: SELECT and START together).
Exit status 0 when every step completed, 1 on a failure not recovered (the
step and the reason are in drive.log and failures.jsonl).
"""
import io, json, os, re, sys, time
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
    for _ in range(40):
        try:
            data = open(p, "rb").read()
            # the whole frame the header names (M11.2f: any rendering resolution; a dump still being written waits)
            head = data[:64].split(None, 4)
            if len(head) >= 4 and head[0] == b"P6":
                w, h = int(head[1]), int(head[2])
                if len(data) < w * h * 3 + len(b"P6\n%d %d\n255\n" % (w, h)):
                    raise ValueError("a dump still being written")
                im = Image.open(io.BytesIO(data)); im.load(); return im.convert("RGB")
        except Exception:
            pass
        time.sleep(0.05)
    return None


def shot(name):
    im = frame()
    nshot[0] += 1
    if im:
        # SHOT_FULL=1: the frame as the game drew it (documentation pictures); else a 640x360 picture
        (im if os.environ.get("SHOT_FULL") == "1" else im.resize((640, 360))).save(
            os.path.join(out, f"{nshot[0]:02d}-{name}.png"))


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


def cond(text):   # key op value; a value with a sign (+5, -3) is relative to a reference state,
    # unless it starts with @ (hz>@-95.5: north of z -95.5, wherever it began)
    m = re.fullmatch(r"(\w+)(<=|>=|==|!=|<|>)(@?)([+-]?)([\dx.A-Fa-f]+)", text)
    if not m:
        sys.exit(f"[drive] bad condition {text!r}")
    k, o, at, sign, v = m.groups()
    return k, o, float(int(v, 16) if v.startswith("0x") else float(v)) * (-1 if sign == "-" else 1), bool(sign) and not at


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
dev_channel = None   # the developer tools' channel (tests/devtools.py), made at the first `dev`


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
last_actor_list = []   # (class, x, z, life) of each with life left, from the last actors()
last_object_list = []  # (class, x, y, z) of every one near, from the last actors()


def actors():
    p, t = os.path.join(run, "actors.txt"), os.path.join(run, "actors.trigger")
    try: before = os.path.getmtime(p)
    except OSError: before = 0
    open(t, "w").close()
    t0 = time.time()
    while time.time() - t0 < 5:
        try:
            if os.path.getmtime(p) != before and not os.path.exists(t):
                lines = open(p).read().splitlines()
                vals = {}
                for tok in lines[0].split() if lines else []:
                    k, _, v = tok.partition("=")
                    vals[k] = float(v)
                # then one line per character with life left: a <class> <x> <z> <life>
                last_actor_list[:] = [(f[1].upper(), float(f[2]), float(f[3]), int(f[4]))
                                      for f in (ln.split() for ln in lines[1:]) if len(f) == 5 and f[0] == "a"]
                # and every one, life or not: o <class> <x> <y> <z>
                last_object_list[:] = [(f[1].upper(), float(f[2]), float(f[3]), float(f[4]))
                                       for f in (ln.split() for ln in lines[1:]) if len(f) == 5 and f[0] == "o"]
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


def fail_record(lineno, line, why, keep_segment=False):
    """A failure, in failures.jsonl; the open segment ends with it as failed,
    unless keep_segment (an attempt inside a segment it did not open: a retry
    there is part of the segment)."""
    s = state()
    rec = {"wall": round(time.time() - T0, 3), "line": lineno, "step": line, "why": why, "state": s,
           "cycle": ctx["cycle"], "attempt": ctx["attempt"],
           "segment": ctx["segment"] and {k: ctx["segment"][k] for k in ("kind", "name", "wall0")},
           "log": tail_log(12)}
    grabs = sorted(os.listdir(os.path.join(run, "grabs"))) if os.path.isdir(os.path.join(run, "grabs")) else []
    rec["last_grab"] = grabs[-1] if grabs else None
    record("failures.jsonl", rec)
    if ctx["segment"] and not keep_segment:   # the open segment, recorded as failed
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
    a door; None: any other than the one it started in): short holds, and when
    the hero stops moving -- an enemy, a corner -- a step to each side in turn
    before going on."""
    t0, last, still = time.time(), None, time.time()
    if area is None:
        s0 = state()
        if not s0:
            raise Fail("cross: no state")
        start = s0["area"]
    side = ("UP", "DOWN") if direction in ("LEFT", "RIGHT") else ("LEFT", "RIGHT")
    turn = 0
    while True:
        s = state()
        if s and (s["area"] == area if area is not None else s["area"] != start):
            say(f"  in area {s['area']:g}")
            return
        if time.time() - t0 > tmo:
            raise Fail(f"cross {direction} into area {'another' if area is None else format(area, 'g')}: still in area {s['area']:g} at "
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


def door_path(hx, hz, x, z):
    """`rooms <w> <h>`: a dungeon of w x h rooms, a door at the middle of a
    wall between two. When (x, z) is in the room next to the hero's: a point
    before that wall's door and one past it (a straight goto stops on the wall
    beside a door, east17); otherwise none -- not every wall has a door, and
    which way round is the route's to say (east18 went east into a wall)."""
    import math
    if hx is None or hz is None:
        return []
    w, h = ctx["rooms"]
    cx, cz = math.floor(hx / w), math.floor(hz / h)
    tx, tz = math.floor(x / w), math.floor(z / h)
    pts = []
    if abs(tx - cx) + abs(tz - cz) != 1:
        return pts
    while (cx, cz) != (tx, tz):
        if cx != tx:
            d = 1 if tx > cx else -1
            wall, mid = max(cx, cx + d) * w, cz * h + h / 2
            pts += [(wall - 2.0 * d, mid), (wall + 2.0 * d, mid)]
            cx += d
        else:
            d = 1 if tz > cz else -1
            wall, mid = max(cz, cz + d) * h, cx * w + w / 2
            pts += [(mid, wall - 2.0 * d), (mid, wall + 2.0 * d)]
            cz += d
    if pts:
        say("  through the door" + ("s" if len(pts) > 2 else "") + " at " +
            ", ".join(f"({px:.1f}, {pz:.1f})" for px, pz in pts[1::2]))
    return pts


def hit_switches(cls="FE6CC0", reach=5.0):
    """`switches`: every wall switch (class FE6CC0, the red cylinders the
    boomerang hits) in the hero's room (`rooms` cells), each from `reach` units
    out from its wall in line with it, facing it, one CIRCLE: from 1.6 off its
    line a throw misses (the east room's, east22-east23). A lava strip or a
    pillar short of the spot leaves the hero in line all the same."""
    import math
    if actors() is None:
        raise Fail("switches: no actors.txt (DOTHEROES_DEV_STATE=1?)")
    if not ctx.get("rooms"):
        raise Fail("switches: no `rooms` grid set")
    w, h = ctx["rooms"]
    s = state() or {}
    cx, cz = math.floor(s.get("hx", 0) / w), math.floor(s.get("hz", 0) / h)
    sws = [o for o in last_object_list if o[0] == cls and (math.floor(o[1] / w), math.floor(o[3] / h)) == (cx, cz)]
    if not sws:
        say(f"  switches: none in room {cx},{cz}")
        return
    for _, x, _, z in sws:
        walls = {"LEFT": x - cx * w, "RIGHT": (cx + 1) * w - x, "DOWN": z - cz * h, "UP": (cz + 1) * h - z}
        face = min(walls, key=walls.get)
        sx, sz = {"LEFT": (x + reach, z), "RIGHT": (x - reach, z), "DOWN": (x, z + reach), "UP": (x, z - reach)}[face]
        say(f"  switch at ({x:.1f}, {z:.1f}) on the {face.lower()} wall: from ({sx:.1f}, {sz:.1f})")
        try:
            goto(sx, sz, 0.3, 10)
        except Fail as e:
            say(f"  (soft) {e}")
        press(BTN[face], 1, 100); wait(0.15)
        press(BTN["CIRCLE"]); wait(1.8)


def fight(cls, tmo, reach, radius=25.0, soft=False):
    """Clear the characters of one class near the hero: walk to the nearest
    with life left until within `reach`, turn to it (one pad report of its
    direction) and swing (CROSS); again until none of the class within
    `radius` of where the fight began has life, or fail after `tmo` s with how
    many are left. Positions are re-read before every move: enemies walk, and
    knock the hero back. One that keeps its life through 25 swings is left
    alone (the first dungeon has one of the class at its door, life 1, that
    no swing reaches; a hovering one, also life 1, in its east room takes a
    while to catch low -- and its chest waits for it)."""
    t0, swings, last, home = time.time(), 0, None, None
    tried = {}   # (x, z) rounded -> [swings at it, its life then]
    near = {}    # class and life -> [the closest the hero got, when]: no nearer in 6 s, out of reach
    while True:
        if actors() is None:
            raise Fail("fight: no actors.txt (DOTHEROES_DEV_STATE=1?)")
        s = state()
        if home is None and s and "hx" in s:
            home = (s["hx"], s["hz"])
        def key(a): return (round(a[1]), round(a[2]))
        left = [a for a in last_actor_list if (cls == "ANY" or a[0] == cls) and
                (home is None or ((a[1] - home[0]) ** 2 + (a[2] - home[1]) ** 2) ** 0.5 <= radius) and
                not (key(a) in tried and tried[key(a)][0] >= 25 and tried[key(a)][1] <= a[3])]
        if not left and last is None and time.time() - t0 < 5:
            wait(0.5)   # none yet: a room's enemies appear as the hero comes in
            continue
        if not left:
            say(f"  fight {cls}: none left with life ({swings} swings, {time.time() - t0:.0f} s)"
                if last is not None else f"  fight {cls}: none appeared in 5 s")
            return
        if time.time() - t0 > tmo:
            why = f"fight {cls}: {len(left)} left with life {sum(a[3] for a in left)} after {tmo:g} s ({swings} swings)"
            if soft:
                say(f"  (soft) {why}")
                return
            raise Fail(why)
        if not s or "hx" not in s:
            wait(0.2); continue
        tgt = min(left, key=lambda a: (a[1] - s["hx"]) ** 2 + (a[2] - s["hz"]) ** 2)
        if last != len(left):
            say(f"  fight {cls}: {len(left)} with life, the nearest at ({tgt[1]:.1f}, {tgt[2]:.1f}) life {tgt[3]}")
            last = len(left)
        dx, dz = tgt[1] - s["hx"], tgt[2] - s["hz"]
        dist = (dx * dx + dz * dz) ** 0.5
        g = near.setdefault((tgt[0], tgt[3], key(tgt)), [dist, time.time()])
        if dist < g[0] - 0.5:
            g[0], g[1] = dist, time.time()
        elif dist > reach and time.time() - g[1] > 6:   # a wall between: set it aside like an unhurt one
            tried[key(tgt)] = [25, tgt[3]]
            say(f"  fight {cls}: the one at ({tgt[1]:.1f}, {tgt[2]:.1f}) is out of reach")
            continue
        face = (BTN["RIGHT"] if dx > 0 else BTN["LEFT"]) if abs(dx) >= abs(dz) else (BTN["UP"] if dz > 0 else BTN["DOWN"])
        if dist > reach:
            mask = 0
            if abs(dx) > reach / 3: mask |= BTN["RIGHT"] if dx > 0 else BTN["LEFT"]
            if abs(dz) > reach / 3: mask |= BTN["UP"] if dz > 0 else BTN["DOWN"]
            secs = max(0.05, min(0.25, (dist - reach) / 9.0))
            press(mask or face, 1, int(secs * 1000))
            wait(secs)
            continue
        press(face, 1)          # turn to it
        wait(0.05)
        press(BTN["CROSS"])
        swings += 1
        t = tried.setdefault(key(tgt), [0, tgt[3]])
        t[0] = t[0] + 1 if tgt[3] >= t[1] else 0   # its life fell: count again from here
        t[1] = tgt[3]
        wait(0.35)


def guard_face(secs):
    """Guard (R1) for `secs` s facing the nearest character with life: every
    0.5 s its position re-read, the hero turned to it (one pad report of its
    direction) and R1 held 0.45 s -- a teleporting caster is faced again. The
    hero's life at the start and the end, and every fall, are logged."""
    t0, falls, life0, life = time.time(), 0, None, None
    while time.time() - t0 < secs:
        if actors() is None:
            raise Fail("guardface: no actors.txt (DOTHEROES_DEV_STATE=1?)")
        s = state() or {}
        if "life" in s:
            if life is not None and s["life"] < life:
                falls += 1
                say(f"  guardface: life {life:g} -> {s['life']:g} at {time.time() - t0:.1f} s")
            life = s["life"]
            life0 = life if life0 is None else life0
        left = [x for x in last_actor_list if x[3] > 0]
        if left and "hx" in s:
            tgt = min(left, key=lambda x: (x[1] - s["hx"]) ** 2 + (x[2] - s["hz"]) ** 2)
            dx, dz = tgt[1] - s["hx"], tgt[2] - s["hz"]
            face = (BTN["RIGHT"] if dx > 0 else BTN["LEFT"]) if abs(dx) >= abs(dz) else (BTN["UP"] if dz > 0 else BTN["DOWN"])
            press(face, 1)
            wait(0.05)
        press(BTN["R1"], 1, 450)
        wait(0.45)
    say(f"  guardface: {secs:g} s, life {life0} -> {life}, {falls} falls")


AIM_LINE = 2.0   # aim mode: as near its column or row as this, the body is in the sword's line (it is broad)
SLOT_BOX = (418, 22, 456, 58)   # `select`: the HUD's circle slot icon, in a 640x360 picture of the frame
WIRE_RANGE = 8.0   # aim mode with `wire`: the farthest anchor pile the Wire Rod is fired at (it caught at 6.4, not at 11.2)


def obj_words(chan, expr, off, n):
    """N guest words at OFF in the native object behind a script object (EXPR: its userdata, whose payload the game's
    binding fills with the object's address -- tostring gives the payload's own); `peek` (developer mode)"""
    o = chan.send(f"lua return tostring({expr})", timeout=10, alive=alive)["out"]
    m = re.match(r"userdata: (?:0x)?([0-9a-fA-F]+)$", str(o).strip())
    if not m:
        raise Fail(f"obj_words: {expr} is not a native object ({o})")
    ptr = chan.send(f"peek 0x{int(m.group(1), 16):X}", timeout=10, alive=alive)["out"]
    ptr = (ptr if isinstance(ptr, list) else json.loads(ptr))[0]
    w = chan.send(f"peek 0x{ptr + off:X} {n}", timeout=10, alive=alive)["out"]
    return ptr, (w if isinstance(w, list) else json.loads(w))


def select_slot(button, ref_name, tries=12):
    """`select`: BUTTON pressed until the HUD's circle slot shows the reference picture (beside the drive)"""
    ref = Image.open(os.path.join(os.path.dirname(os.path.abspath(script)), ref_name)).convert("RGB")
    for k in range(tries + 1):
        im = frame()
        if im:
            got = im.convert("RGB").resize((640, 360)).crop(SLOT_BOX)
            off = sum(ImageStat.Stat(ImageChops.difference(got, ref.resize(got.size))).mean) / 3.0
            if off < 14.0:
                say(f"  select: the circle slot shows it after {k} press(es) (difference {off:.1f})")
                return k
        if k == tries:
            raise Fail(f"select: not in the circle slot after {tries} presses of {button}")
        press(mask_of(button), 3)
        wait(1.0)


# ---- an arena's ways: its cells walked and the Wire Rod's pulls (devexplore; devfight's map@) ----
CELL = 1.6
NAV_DIRS = {"RIGHT": (1, 0), "LEFT": (-1, 0), "UP": (0, -1), "DOWN": (0, 1)}   # UP: z larger, the row before


def cell_of(x, z):
    return (int(round(x / CELL - 0.5)), int(round(-z / CELL - 0.5)))


def cell_center(c):
    return ((c[0] + 0.5) * CELL, -(c[1] + 0.5) * CELL)


def hero_cell():
    st = state()
    return cell_of(st["hx"], st["hz"]) if st and "hx" in st and (st["hx"] or st["hz"]) else None


def walk_to(c, tmo=3.0):
    """onto cell c's centre (a neighbour: one way); True when there"""
    x, z = cell_center(c)
    try:
        goto(x, z, 0.45, tmo)
    except Fail:
        return False
    return hero_cell() == c


def fire_wire(way):
    """the Wire Rod (the circle slot's) fired one way; the cell the hero is in after"""
    press(BTN[way], 3)
    wait(0.05)
    press(BTN["CIRCLE"])
    wait(1.8)
    return hero_cell()


class Nav:
    """the cells known walkable, the walks between them (both ways) and the Wire Rod's pulls (one way)"""

    def __init__(self, data=None):
        data = data or {}
        self.start = tuple(data["start"]) if data.get("start") else None   # the cell the map was begun from
        self.cells = {tuple(c) for c in data.get("cells", [])}
        self.walks = {(tuple(a), tuple(b)) for a, b in data.get("walks", [])}
        self.hooks = {(tuple(a), tuple(b)): w for a, w, _, b in data.get("hooks", [])}
        self.hook_list = [(tuple(a), w, sl, tuple(b)) for a, w, sl, b in data.get("hooks", [])]
        self.bad = {}   # a step that just failed (an arm across the way) -> until when the paths go round it

    def add_walk(self, a, b):
        self.cells |= {a, b}
        self.walks |= {(a, b), (b, a)}

    def add_hook(self, a, way, slot, b):
        self.cells |= {a, b}
        self.hooks[(a, b)] = way
        self.hook_list.append((a, way, slot, b))

    def returnable(self):
        """the cells from which the map leads back to its start: a map left unfinished has platforms whose ways out
        were never tried -- a path never goes onto one"""
        from collections import deque
        if self.start is None or self.start not in self.cells:
            return set(self.cells)
        back = {}
        for a, b in list(self.walks) + list(self.hooks):
            back.setdefault(b, []).append(a)
        seen, q = {self.start}, deque([self.start])
        while q:
            c = q.popleft()
            for n in back.get(c, []):
                if n not in seen:
                    seen.add(n)
                    q.append(n)
        return seen

    def path(self, src, goal):
        """the fewest steps (walks and pulls) from src to a cell goal(c) accepts, or None; only through cells that lead
        back to the map's start"""
        from collections import deque
        if src is None:
            return None
        if src not in self.cells:   # off the map (knocked): the nearest known cell first
            if not self.cells:
                return None
            near = min(self.cells, key=lambda c: abs(c[0] - src[0]) + abs(c[1] - src[1]))
            if abs(near[0] - src[0]) + abs(near[1] - src[1]) > 2:
                return None
            rest = self.path(near, goal)
            return [src] + rest if rest else None
        nxt = {}
        safe = self.returnable()
        now = time.time()
        for a, b in self.walks:
            if b in safe and self.bad.get((a, b), 0) < now:
                nxt.setdefault(a, []).append(b)
        for a, b in self.hooks:
            if b in safe and self.bad.get((a, b), 0) < now:
                nxt.setdefault(a, []).append(b)
        seen, q = {src: None}, deque([src])
        while q:
            c = q.popleft()
            if goal(c):
                out = [c]
                while seen[out[-1]] is not None:
                    out.append(seen[out[-1]])
                return out[::-1]
            for n in nxt.get(c, []):
                if n not in seen:
                    seen[n] = c
                    q.append(n)
        return None

    def follow(self, p):
        """the path walked and pulled along; False at the first step that does not land where it should"""
        for a, b in zip(p, p[1:]):
            if (a, b) in self.hooks:
                if hero_cell() != a and not walk_to(a):
                    return False
                if fire_wire(self.hooks[(a, b)]) != b:
                    self.bad[(a, b)] = time.time() + 20.0
                    return False
            elif not walk_to(b):
                self.bad[(a, b)] = time.time() + 20.0
                return False
        return True

    def dump(self, path, piles):
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"start": self.start, "cells": sorted(self.cells), "walks": sorted(self.walks),
                       "hooks": [[a, w, sl, b] for a, w, sl, b in self.hook_list], "piles": piles}, f)


def devexplore(chan, out_path, tmo):
    """M11.1b: an arena's ways for devfight's map@ -- from the hero's cell, every neighbour tried by walking (one cell,
    one way, once from each cell), and from every known cell in line with an anchor pile within the Wire Rod's reach
    (WIRE_RANGE) the rod fired at it once (a pull: a way, one way); until nothing is left untried or `tmo` s"""
    o = chan.send("enemies", timeout=10, alive=alive)["out"]
    o = o if isinstance(o, list) else json.loads(o)
    piles = [(a["slot"], a["x"], a["z"]) for a in o if a["kind"] == "ANCHOR_PILE"]
    nav, t0 = Nav(), time.time()
    tried_walk, tried_hook = {}, set()   # (cell, way) -> tries (a second once all else is done: an arm in the way)
    lost = 0
    start = hero_cell()
    if start is None or not walk_to(start):
        raise Fail("devexplore: the hero's cell is not known")
    nav.cells.add(start)
    nav.start = start
    say(f"  devexplore: from cell {start}, {len(piles)} anchor piles")

    def hook_ways(c):
        cx, cz = cell_center(c)
        for sl, px, pz in piles:
            d = ((px - cx) ** 2 + (pz - cz) ** 2) ** 0.5
            if d < 1.2 or d > WIRE_RANGE or (c, sl) in tried_hook:
                continue
            if abs(px - cx) < 0.2:
                yield sl, ("UP" if pz > cz else "DOWN")
            elif abs(pz - cz) < 0.2:
                yield sl, ("RIGHT" if px > cx else "LEFT")

    tries = 1
    while time.time() - t0 < tmo:
        cur = hero_cell()
        if cur is None:   # a read without his place: not the end of the map
            lost += 1
            if lost > 50:
                break
            wait(0.3)
            continue
        lost = 0

        def untried(c):   # first every way once; then the ways that failed (their neighbour still unknown), once more
            return [w for w in NAV_DIRS if tried_walk.get((c, w), 0) == 0 or
                    (tries == 2 and tried_walk.get((c, w), 0) < 2 and
                     (c[0] + NAV_DIRS[w][0], c[1] + NAV_DIRS[w][1]) not in nav.cells)]
        p = nav.path(cur, lambda c: bool(untried(c)))
        if p is not None:
            if not nav.follow(p):
                continue
            c = p[-1]
            ways = untried(c)
            if not ways:
                continue
            way = ways[0]
            tried_walk[(c, way)] = tried_walk.get((c, way), 0) + 1
            n = (c[0] + NAV_DIRS[way][0], c[1] + NAV_DIRS[way][1])
            if n in nav.cells:   # known already: the walk only
                if walk_to(n, 2.5):
                    nav.add_walk(c, n)
                else:
                    walk_to(c)
                continue
            if walk_to(n, 2.5):
                nav.add_walk(c, n)
            else:
                walk_to(c)
            continue
        p = nav.path(cur, lambda c: any(True for _ in hook_ways(c)))
        if p is None:
            if tries == 1:   # every way tried once: the walks that failed, once more
                tries = 2
                say(f"  devexplore: {len(nav.cells)} cells, {len(nav.hook_list)} pulls; the failed walks once more")
                continue
            break
        if not nav.follow(p):
            continue
        c = p[-1]
        sl, way = next(hook_ways(c))
        tried_hook.add((c, sl))
        n = fire_wire(way)
        if n is not None and n != c:
            nav.add_hook(c, way, sl, n)
            say(f"  devexplore: the Wire Rod {way.lower()} from {c} to the pile {sl}: pulled to {n}")
            walk_to(n)
        else:
            say(f"  devexplore: the Wire Rod {way.lower()} from {c} to the pile {sl}: not pulled ({n})")
    nav.dump(os.path.join(run, out_path) if not os.path.isabs(out_path) else out_path, piles)
    say(f"  devexplore: {len(nav.cells)} cells, {len(nav.walks) // 2} walks, {len(nav.hook_list)} pulls "
        f"({time.time() - t0:.0f} s) -> {out_path}")
    return nav


def devfight(chan, tmo, flag=None, reach=2.4, soft=False, hold=None, face=None, aim=False, aim_reach=0.0, wire=False,
             nav=None, cast=None, collect=False, farm=None, cycle=None, life_off=None):
    """M11.1: fight what the developer tools' `enemies` lists -- the game's actor manager's visible ENEMY actors (a
    boss's parts among them, which the actor scan behind `fight` does not see): walk to the nearest, turn to it and
    swing, until none is left or, with `flag` (an event flag number), until the game sets that flag (a boss's
    defeat); fails after `tmo` s (`soft`: logged)."""
    t0, swings, said, seen = time.time(), 0, None, False
    near, aside = {}, {}   # slot -> [closest distance, when]; slot -> until when it is set aside (out of reach)
    counted = None   # hold and aim modes: the number of visible enemies last logged
    quiet_since = None   # none visible since (a boss's after-event: its words paged until its flag is set)
    body = None      # aim mode: the slot of the boss's body
    step_axis, stalls = None, 0   # aim mode: the way the last step went, the steps in a row that went nowhere
    sweep = 0        # aim mode within reach: which of the two ways toward the body the next swing faces
    wires = 0        # aim mode with `wire`: the Wire Rod's uses
    no_pull = {}     # aim mode with `wire`: (the hero's cell, the pile's slot) -> until when that pile is passed over
    life_at, life_was = 0.0, None   # aim mode: the boss's life (a drive's __dt_boss, its Init's object), read each 15 s
    cast_at, casts = 0.0, 0   # cast@: when the circle slot's spell was last cast, how often
    lit_until = 0.0   # cast@: until when the spell last cast lasts (its interval and 2 s)
    magic, magic_at = None, 0.0   # farm@COST: the hero's magic, read each 5 s
    bodies, farming = None, False   # farm@COST: the boss's actors when the fight began (the rest: his summons); now on them
    body_since, body_aside = 0.0, {}   # cycle@SECS: since when the body hurt nothing; bodies passed over -> until when
    if cast:   # cast@ICON,SECS: the spell the icon shows put in the circle slot, cast at once and every SECS s
        select_slot("R2", cast[0], 14)
    if hold:   # hold mode: from this spot, strike only what comes within reach (a boss that comes to the hero)
        try:
            goto(hold[0], hold[1], 1.0, 30)
        except Fail as e:
            say(f"  devfight: hold spot not reached ({e})")
        if face:   # the hero stands on a cell's centre: held where he settled (a spot on a cell's edge is not one)
            s = state()
            if s and "hx" in s and abs(s["hx"] - hold[0]) <= 1.0 and abs(s["hz"] - hold[1]) <= 1.0:
                hold = [s["hx"], s["hz"]]
            say(f"  devfight: held at ({hold[0]:.2f}, {hold[1]:.2f}), every swing {face.lower()}")
    while True:
        if flag is not None:
            o = chan.send(f"flag get {flag}", timeout=10, alive=alive)["out"]
            if (o if isinstance(o, dict) else json.loads(o)).get("value", 0) >= 1:
                say(f"  devfight: flag {flag} set ({swings} swings, {time.time() - t0:.0f} s)")
                return
        o = chan.send("enemies", timeout=10, alive=alive)["out"]
        o = o if isinstance(o, list) else json.loads(o)
        hero = next((x for x in o if x["kind"] == "PLAYER"), None)
        es = [x for x in o if x["kind"] == "ENEMY" and x.get("visible")]
        seen = seen or bool(es)
        if not es and flag is None:
            if not seen and time.time() - t0 < 15:   # none yet: a boss created as its event ends
                wait(0.5)
                continue
            say(f"  devfight: none left ({swings} swings, {time.time() - t0:.0f} s)" if seen else
                "  devfight: none appeared in 15 s")
            return
        if time.time() - t0 > tmo:
            why = f"devfight: {len(es)} enemies left after {tmo:g} s ({swings} swings)" + (f", flag {flag} not set" if flag is not None else "")
            if soft:
                say(f"  (soft) {why}")
                return
            raise Fail(why)
        if not es or not hero:
            if not es and flag is not None and seen:   # the boss gone, its flag not yet: its after-event's words paged
                if quiet_since is None:
                    quiet_since = time.time()
                elif time.time() - quiet_since > 4.0:
                    press(BTN["CROSS"])
                    wait(1.0)
                    continue
            wait(0.5)
            continue
        quiet_since = None
        now = time.time()
        dist_of = lambda a: ((a["x"] - hero["x"]) ** 2 + (a["z"] - hero["z"]) ** 2) ** 0.5  # noqa: E731
        if farm and time.time() - magic_at > 5:
            magic_at = time.time()
            try:
                pl = chan.send("player", timeout=10, alive=alive)["out"]
                magic = (pl if isinstance(pl, dict) else json.loads(pl)).get("magic")
            except Exception:   # noqa: BLE001 -- unread: as it was
                pass
        if cast and time.time() - cast_at >= cast[1] and (not farm or magic is None or magic >= farm):
            press(BTN["CIRCLE"])
            wait(1.2)
            cast_at, casts = time.time(), casts + 1
            lit_until = cast_at + cast[1] + 2.0
            magic = None if farm else magic   # read again before the next
            if casts == 1 or casts % 10 == 0:
                say(f"  devfight: the circle slot's spell cast ({casts} times, {time.time() - t0:.0f} s)")
        if collect and hero:   # collect: an item dropped near (an apple: the magic back) walked over
            near_items = [q for q in o if q["kind"] == "ITEM" and q.get("visible") and
                          ((q["x"] - hero["x"]) ** 2 + (q["z"] - hero["z"]) ** 2) ** 0.5 <= 6.0]
            if near_items:
                q = min(near_items, key=lambda q: (q["x"] - hero["x"]) ** 2 + (q["z"] - hero["z"]) ** 2)
                try:
                    goto(q["x"], q["z"], 0.5, 3)
                except Fail:
                    pass
                continue
        if aim:
            lv_changed = False
            if time.time() - life_at > (5 if life_off is not None else 15):
                # life@OFF: the body's own life, its actor's word at OFF (the priest's: +0x5BC); else the life its
                # Init was given (a drive's __dt_boss: m_info.hp -- the starting life, never lowered by a wound)
                life_at = time.time()
                try:
                    if life_off is not None and body is not None:
                        lv = (body, obj_words(chan, f"actor_manager.gActorManager:getActor({body})", life_off, 1)[1][0])
                    else:
                        lv = (None, chan.send('lua return __dt_boss and __dt_boss.m_info and __dt_boss.m_info.hp or "none"',
                                              timeout=10, alive=alive)["out"])
                except Exception as e:   # noqa: BLE001 -- a refused chunk: no life to log
                    lv = (body, f"unread ({str(e)[:60]})")
                if lv != life_was:
                    say(f"  devfight: {'slot ' + str(lv[0]) + chr(39) + 's life' if lv[0] is not None else 'the boss' + chr(39) + 's starting life'}"
                        f" {lv[1]} ({time.time() - t0:.0f} s)")
                    lv_changed = (life_was is not None and life_was[0] == lv[0] and isinstance(lv[1], int) and
                                  isinstance(life_was[1], int) and lv[1] < life_was[1])
                    life_was = lv
            if len(es) != counted:
                say(f"  devfight: {len(es)} visible enemies ({time.time() - t0:.0f} s, {swings} swings)")
                counted = len(es)
            if bodies is None:
                bodies = {a["slot"] for a in es}
                if farm or cycle:
                    say(f"  devfight: the boss's actors {sorted(bodies)}")
            if cycle and lv_changed:   # a wound: this body is the boss's
                body_since = time.time()
            if cycle and body is not None and time.time() - body_since > cycle:   # it hurt nothing: the next one
                say(f"  devfight: slot {body}: no wound in {cycle:g} s, the next")
                body_aside[body] = time.time() + 3 * cycle
                body = None
            tgt = next((a for a in es if a["slot"] == body), None)
            if tgt is None:   # the body: the first of the boss's actors (again if it went: a new body, a new phase)
                pool = [a for a in es if body_aside.get(a["slot"], 0) <= time.time() and
                        (not cycle or a["slot"] in bodies)] or es
                tgt = min(pool, key=lambda a: a["slot"])
                say(f"  devfight: aiming at slot {tgt['slot']} at ({tgt['x']:.1f}, {tgt['z']:.1f})")
                body, body_since = tgt["slot"], time.time()
            if farm:   # the magic too low for the spell, its light gone: his summons fought for what they drop
                low = magic is not None and magic < farm and time.time() > lit_until
                prey = [a for a in es if a["slot"] not in bodies] if low else []
                if bool(prey) != farming:
                    farming = bool(prey)
                    say(f"  devfight: {'on his summons (magic ' + str(magic) + ')' if farming else 'on the boss'} "
                        f"({time.time() - t0:.0f} s)")
                if prey:
                    tgt = min(prey, key=dist_of)
                    body_since = time.time()   # the body was not tried meanwhile
            dx, dz = tgt["x"] - hero["x"], tgt["z"] - hero["z"]
            if aim_reach and (dx * dx + dz * dz) ** 0.5 <= aim_reach and min(abs(dx), abs(dz)) > AIM_LINE:
                # within the blade's reach off its line: a swing sweeps an arc -- the two ways toward it in turn
                sweep ^= 1
                way = ((BTN["RIGHT"] if dx > 0 else BTN["LEFT"]) if sweep else (BTN["UP"] if dz > 0 else BTN["DOWN"]))
                press(way, 3)
                wait(0.05)
                press(BTN["CROSS"])
                swings += 1
                wait(0.35)
                continue
            far = aim_reach and (dx * dx + dz * dz) ** 0.5 > aim_reach + 4.0   # in line, but past the blade
            if nav is not None and aim_reach and (far or min(abs(dx), abs(dz)) > AIM_LINE):
                # the arena's map (devexplore): the fewest walks and pulls to a cell from which it is in reach
                def in_reach(c, tx=tgt["x"], tz=tgt["z"]):
                    cx, cz = cell_center(c)
                    ex, ez = tx - cx, tz - cz
                    dd = (ex * ex + ez * ez) ** 0.5
                    return dd <= aim_reach or (min(abs(ex), abs(ez)) <= AIM_LINE and dd <= aim_reach + 4.0)
                way = nav.path(hero_cell(), in_reach)
                if way and len(way) > 1:
                    say(f"  devfight: to ({tgt['x']:.1f}, {tgt['z']:.1f}): {len(way) - 1} steps "
                        f"({sum(1 for q in zip(way, way[1:]) if q in nav.hooks)} pulls) from {way[0]} to {way[-1]}")
                    if not nav.follow(way):
                        say(f"  devfight: the way broke at {hero_cell()}")
                    continue
            if wire and (far or (aim_reach and min(abs(dx), abs(dz)) > AIM_LINE)):
                # the Wire Rod (the circle slot's): to an anchor pile (the posts its anchor catches) whose platform is
                # nearer the body -- fired when the hero is in the pile's line, else first into that line
                d_t = (dx * dx + dz * dz) ** 0.5
                best = None
                for pl in (q for q in o if q["kind"] == "ANCHOR_PILE"):
                    px, pz = pl["x"] - hero["x"], pl["z"] - hero["z"]
                    d_p = (px * px + pz * pz) ** 0.5
                    gain = d_t - ((tgt["x"] - pl["x"]) ** 2 + (tgt["z"] - pl["z"]) ** 2) ** 0.5
                    if d_p < 1.2 or d_p > WIRE_RANGE or gain < 2.0:
                        continue
                    if no_pull.get((round(hero["x"] / 1.6), round(hero["z"] / 1.6), pl["slot"]), 0) > time.time():
                        continue   # it did not pull him from here a moment ago
                    off = min(abs(px), abs(pz))
                    score = gain - 3.0 * off   # nearer the body, the less walk to its line the better
                    if best is None or score > best[0]:
                        best = (score, pl, px, pz, off)
                if best:
                    _, pl, px, pz, off = best
                    if off <= 0.4:
                        way = (BTN["UP"] if pz > 0 else BTN["DOWN"]) if abs(px) <= abs(pz) else (BTN["RIGHT"] if px > 0 else BTN["LEFT"])
                        press(way, 3)
                        wait(0.05)
                        press(BTN["CIRCLE"])
                        wait(1.8)
                        s_now = state()
                        if not s_now or "hx" not in s_now or \
                                abs(s_now["hx"] - hero["x"]) + abs(s_now["hz"] - hero["z"]) < 1.0:   # not pulled
                            no_pull[(round(hero["x"] / 1.6), round(hero["z"] / 1.6), pl["slot"])] = time.time() + 30
                        say(f"  devfight: the Wire Rod at the pile at ({pl['x']:.1f}, {pl['z']:.1f}) from ({hero['x']:.1f}, "
                            f"{hero['z']:.1f}): now at ({s_now.get('hx', 0):.1f}, {s_now.get('hz', 0):.1f})" if s_now else
                            f"  devfight: the Wire Rod at the pile at ({pl['x']:.1f}, {pl['z']:.1f})")
                        wires += 1
                        continue
                    try:   # into the pile's line along the shorter way
                        goto(pl["x"], hero["z"], 0.3, 4) if abs(px) <= abs(pz) else goto(hero["x"], pl["z"], 0.3, 4)
                        continue
                    except Fail:
                        pass
            if min(abs(dx), abs(dz)) <= AIM_LINE and not far:   # in its column or row (the body is broad): turn, swing
                way = (BTN["UP"] if dz > 0 else BTN["DOWN"]) if abs(dx) <= abs(dz) else (BTN["RIGHT"] if dx > 0 else BTN["LEFT"])
                press(way, 3)
                wait(0.05)
                press(BTN["CROSS"])
                swings += 1
                wait(0.35)
                continue
            # a short step toward its column or row (the nearer first; a step that gets nowhere -- a post, the
            # water's edge -- turns to the other way next time; both nowhere: a swing at the nearest part)
            if step_axis is None:
                step_axis = "x" if abs(dx) <= abs(dz) else "z"
            way = ((BTN["RIGHT"] if dx > 0 else BTN["LEFT"]) if step_axis == "x" else
                   (BTN["UP"] if dz > 0 else BTN["DOWN"]))
            before = (hero["x"], hero["z"])
            press(way, 4)
            wait(0.1)
            s_now = state()
            moved = s_now and "hx" in s_now and abs(s_now["hx"] - before[0]) + abs(s_now["hz"] - before[1]) > 0.2
            if moved:
                stalls = 0
                continue
            stalls += 1
            step_axis = "z" if step_axis == "x" else "x"
            if stalls >= 2:   # neither way: a swing at the nearest part meanwhile
                stalls = 0
                near_one = min(es, key=dist_of)
                dxn, dzn = near_one["x"] - hero["x"], near_one["z"] - hero["z"]
                press((BTN["RIGHT"] if dxn > 0 else BTN["LEFT"]) if abs(dxn) >= abs(dzn) else
                      (BTN["UP"] if dzn > 0 else BTN["DOWN"]), 3)
                wait(0.05)
                press(BTN["CROSS"])
                swings += 1
                wait(0.35)
            continue
        if hold:
            if len(es) != counted:
                say(f"  devfight: {len(es)} visible enemies ({time.time() - t0:.0f} s, {swings} swings)")
                counted = len(es)
            close = [a for a in es if dist_of(a) <= reach * 2]
            off = (abs(hero["x"] - hold[0]) + abs(hero["z"] - hold[1]) if face else
                   ((hero["x"] - hold[0]) ** 2 + (hero["z"] - hold[1]) ** 2) ** 0.5)
            if off > (0.6 if face else 3.0):
                try:
                    goto(hold[0], hold[1], 0.4 if face else 1.0, 10)   # knocked away: back to the spot
                except Fail:
                    pass
                continue
            if face:   # held three reads: the hero turns over several frames, one read leaves him as he faced
                press(BTN[face], 3)
            else:
                tgt = min(close or es, key=dist_of)   # the nearest anyway: at full life the sword's beam reaches it
                dxh, dzh = tgt["x"] - hero["x"], tgt["z"] - hero["z"]
                press((BTN["RIGHT"] if dxh > 0 else BTN["LEFT"]) if abs(dxh) >= abs(dzh) else (BTN["UP"] if dzh > 0 else BTN["DOWN"]), 1)
            wait(0.05)
            press(BTN["CROSS"])
            swings += 1
            wait(0.35)
            continue
        reach_now = [a for a in es if aside.get(a["slot"], 0) <= now or dist_of(a) <= reach]
        if not reach_now:   # all out of reach (across water, up in the air): hold here, swing at what comes near
            tgt = min(es, key=dist_of)
            dxh, dzh = tgt["x"] - hero["x"], tgt["z"] - hero["z"]
            face = (BTN["RIGHT"] if dxh > 0 else BTN["LEFT"]) if abs(dxh) >= abs(dzh) else (BTN["UP"] if dzh > 0 else BTN["DOWN"])
            press(face, 1)
            wait(0.05)
            press(BTN["CROSS"])
            swings += 1
            wait(0.5)
            continue
        tgt = min(reach_now, key=dist_of)
        if said != len(es):
            say(f"  devfight: {len(es)} enemies, the nearest (slot {tgt['slot']}) at ({tgt['x']:.1f}, {tgt['z']:.1f})")
            said = len(es)
        dx, dz = tgt["x"] - hero["x"], tgt["z"] - hero["z"]
        dist = (dx * dx + dz * dz) ** 0.5
        g = near.setdefault(tgt["slot"], [dist, now])
        if dist < g[0] - 0.5:
            g[0], g[1] = dist, now
        elif dist > reach and now - g[1] > 5:   # no nearer in 5 s: set aside for 10 s
            aside[tgt["slot"]] = now + 10
            near.pop(tgt["slot"], None)
            continue
        face = (BTN["RIGHT"] if dx > 0 else BTN["LEFT"]) if abs(dx) >= abs(dz) else (BTN["UP"] if dz > 0 else BTN["DOWN"])
        if dist > reach:
            mask = 0
            if abs(dx) > reach / 3: mask |= BTN["RIGHT"] if dx > 0 else BTN["LEFT"]
            if abs(dz) > reach / 3: mask |= BTN["UP"] if dz > 0 else BTN["DOWN"]
            secs = max(0.05, min(0.25, (dist - reach) / 9.0))
            press(mask or face, 1, int(secs * 1000))
            wait(secs)
            continue
        press(face, 1)
        wait(0.05)
        press(BTN["CROSS"])
        swings += 1
        wait(0.35)


def devcollect(chan, tmo):
    """M11.1: walk over every visible ITEM the tools' `enemies` lists (the game's actor manager: dropped money, a
    life vessel), nearest first, until none is left; one the hero gets no nearer to in 6 s is passed over. Fails
    after `tmo` s with how many are left.
    An item's message (the life vessel's "get") holds the hero still until its button. When the hero has not moved
    for 2 s while the drive walks him, CROSS is pressed: it pages a message, and outside one it is only a swing.
    When the game runs an event then (the tools' status), the items' reach is also measured afresh: the time he was
    held is not counted against them. A hero held by a wall, with no event, is passed over as before (0.2.0-beta.6
    qualification, temple 3: the vessel taken first, its message up for the rest of the step, every coin passed
    over as out of reach)."""
    import devtools
    t0, skipped, near = time.time(), set(), {}
    still, pages = [None, time.time()], 0   # the hero's last position and since when; CROSS presses
    while True:
        o = chan.send("enemies", timeout=10, alive=alive)["out"]
        o = o if isinstance(o, list) else json.loads(o)
        hero = next((x for x in o if x["kind"] == "PLAYER"), None)
        items = [x for x in o if x["kind"] == "ITEM" and x.get("visible") and x["slot"] not in skipped]
        if not items:
            say(f"  devcollect: none left ({len(skipped)} passed over, {pages} held-still presses, "
                f"{time.time() - t0:.0f} s)")
            return
        if time.time() - t0 > tmo:
            raise Fail(f"devcollect: {len(items)} items left after {tmo:g} s ({pages} held-still presses)")
        if not hero:
            wait(0.5)
            continue
        pos = (round(hero["x"], 1), round(hero["z"], 1))
        if pos != still[0]:
            still[0], still[1] = pos, time.time()
        elif time.time() - still[1] > 2:
            still[1] = time.time()
            try:
                ev = chan.status(timeout=10, alive=alive).get("event_state")
            except (devtools.Refused, TimeoutError):
                ev = None
            press(BTN["CROSS"])
            wait(0.8)
            pages += 1
            if ev:
                near.clear()
            say(f"  devcollect: the hero held still 2 s (event state {ev}): CROSS")
            continue
        tgt = min(items, key=lambda a: (a["x"] - hero["x"]) ** 2 + (a["z"] - hero["z"]) ** 2)
        dx, dz = tgt["x"] - hero["x"], tgt["z"] - hero["z"]
        dist = (dx * dx + dz * dz) ** 0.5
        g = near.setdefault(tgt["slot"], [dist, time.time()])
        if dist < g[0] - 0.3:
            g[0], g[1] = dist, time.time()
        elif time.time() - g[1] > 6:
            skipped.add(tgt["slot"])
            say(f"  devcollect: the item in slot {tgt['slot']} at ({tgt['x']:.1f}, {tgt['z']:.1f}) out of reach")
            continue
        mask = 0
        if abs(dx) > 0.3: mask |= BTN["RIGHT"] if dx > 0 else BTN["LEFT"]
        if abs(dz) > 0.3: mask |= BTN["UP"] if dz > 0 else BTN["DOWN"]
        if not mask:
            mask = BTN["UP"]
        secs = max(0.05, min(0.25, dist / 9.0))
        press(mask, 1, int(secs * 1000))
        wait(secs)


def open_obj(cls, tries):
    """Open what the objects of one class are (a chest): walk up to each, nearest first, from
    the south, at small offsets across it, facing it, and press CROSS (then
    twice more for an item's message), until the class's count near the hero
    falls -- the first dungeon's chest is two objects of class FE3C70 until
    its key is taken, then one. Fails after `tries` approaches."""
    t0 = time.time()
    while True:   # a chest can take a few seconds to appear once a room is cleared
        if actors() is None:
            raise Fail("open: no actors.txt (DOTHEROES_DEV_STATE=1?)")
        objs = [o for o in last_object_list if o[0] == cls]
        if objs or time.time() - t0 > 8:
            break
        wait(0.5)
    n0 = len(objs)
    if not objs:
        raise Fail(f"open {cls}: none within 40 units after 8 s")
    s = state() or {}
    hx, hz = s.get("hx", 0.0), s.get("hz", 0.0)   # the nearest first: the chest the route walked up to
    near = sorted(objs, key=lambda o: (o[1] - hx) ** 2 + (o[3] - hz) ** 2)
    spots = [(o[1] + dx, o[3]) for o in near for dx in (0.0, -0.6, 0.6, -1.2, 1.2)]
    for i, (x, z) in enumerate(spots[:tries]):
        try:
            goto(x, z - 1.6, 0.25, 8)
        except Fail as e:
            say(f"  open {cls}: {e}")
        press(BTN["UP"], 1, 350); wait(0.35)
        wait(0.4)
        for pause in (2.5, 1.2, 0.8):
            press(BTN["CROSS"]); wait(pause)
        if actors() is None:
            raise Fail("open: no actors.txt")
        n = sum(1 for o in last_object_list if o[0] == cls)
        if n < n0:
            say(f"  open {cls}: opened from ({x:.1f}, {z - 1.6:.1f}), try {i + 1} ({n0} -> {n})")
            return
    raise Fail(f"open {cls}: still {n0} after {min(tries, len(spots))} tries")


def keep_saves(dest):
    """until_exit: wait for the game to exit; with dest, copy each save the game
    makes (its folder under the game's savedata, once its files stop changing).
    The game's data root is GAMEDATA when the session plays one in place, else
    the run's own gamedata/ (a '#! saves' session plays a fresh copy there).
    A relative dest is the project's."""
    import shutil
    project = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    gamedata = os.path.join(project, os.environ["GAMEDATA"]) if os.environ.get("GAMEDATA") else os.path.join(run, "gamedata")
    root = os.path.join(gamedata, "dev_hdd0", "home", "00000001", "savedata")
    if dest:
        dest = os.path.join(project, dest)

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
        if not dest:
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
            # and the edit data (characters made in the game's editor, named with
            # the Japanese release's ID in every region): a save whose hero is an
            # edited one loads it at the start, and without it the hero has no body
            # (only the shield: owner-play-r8, 2026-11-07)
            edits = [e for e in sorted(os.listdir(root)) if e.startswith("BLJM60180") and os.path.isdir(os.path.join(root, e))]
            for e in edits:
                shutil.copytree(os.path.join(root, e), os.path.join(out, e), dirs_exist_ok=True)
            with open(os.path.join(out, "checkpoint.json"), "w") as f:
                json.dump({"saved": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t)), "folder": d,
                           "edit_data": edits, "state": s, "run": os.path.basename(run),
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
            outer_seg = ctx["segment"]   # open before the attempt: a retry stays inside it
            for k in range(1, tries + 1):
                ctx["attempt"] = k
                try:
                    run_steps(inner)
                    break
                except Fail as e:
                    fail_record(*getattr(e, "where", (n, line)), str(e),
                                keep_segment=outer_seg is not None and ctx["segment"] is outer_seg)
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
    global expecting, dev_channel
    if op == "log":
        rx, tmo = re.compile(a[0]), float(a[1]) if len(a) > 1 else 120
        t0 = time.time()
        while not rx.search(new_log()):
            if time.time() - t0 > tmo:
                shot("timeout"); raise Fail(f"no new log line matching {a[0]!r} in {tmo:g} s")
            wait(0.25)
    elif op == "sleep":
        wait(float(a[0]))
    elif op == "pressuntil":   # M11.1: <regex> <timeout> <BUTTON> [<every>]: page an event chain until a log line
        rx, tmo, btn = re.compile(a[0]), float(a[1]), mask_of(a[2])
        every = float(a[3]) if len(a) > 3 else 1.5
        t0 = time.time()
        while True:
            t1 = time.time()
            while time.time() - t1 < every:   # the lines since the last press (or the step's start)
                if rx.search(new_log()):
                    break
                wait(0.25)
            else:
                if time.time() - t0 > tmo:
                    shot("timeout"); raise Fail(f"no new log line matching {a[0]!r} in {tmo:g} s of {a[2]} presses")
                press(btn)
                continue
            break
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
    elif op == "device":   # device <keyboard|playstation|xbox|switch|steam|generic>: the last input's device (M11.2g)
        kinds = ["keyboard", "playstation", "xbox", "switch", "steam", "generic"]
        acted()
        with open(os.path.join(run, "pad.txt"), "w") as f:
            f.write(f"0x0000 1 0 dev={kinds.index(a[0])}\n")
        wait(0.3)
        say(f"device: {a[0]}")
    elif op == "guardface":   # guardface <secs>: guard (R1) turned to the nearest character with life, re-aimed every 0.5 s
        guard_face(float(a[0]))
    elif op == "fight":
        soft = "soft" in a
        a = [x for x in a if x != "soft"]
        fight(a[0].upper().lstrip("C") if a[0].upper().startswith("C") else a[0].upper(), float(a[1]) if len(a) > 1 else 60,
              float(a[2]) if len(a) > 2 else 2.5, soft=soft)
    elif op == "open":
        soft = a[-1] == "soft"
        a = a[:-1] if soft else a
        try:
            open_obj(a[0].upper(), int(a[1]) if len(a) > 1 else 10)
        except Fail as e:
            if not soft or not alive():
                raise
            say(f"  (soft) {e}")
    elif op == "unstick":
        soft = a[-1] == "soft"
        a = a[:-1] if soft else a
        try:
            unstick(a[0].upper(), int(a[1]) if len(a) > 1 else 8)
        except Fail as e:
            if not soft or not alive():
                raise
            say(f"  (soft) {e}")
    elif op == "cross":
        soft = a[-1] == "soft"
        a = a[:-1] if soft else a
        try:
            cross(a[0].upper(), None if a[1] == "any" else float(a[1]), float(a[2]) if len(a) > 2 else 20)
        except Fail as e:
            if not soft or not alive():
                raise
            say(f"  (soft) {e}")
    elif op == "goto":
        soft = a[-1] == "soft"
        a = a[:-1] if soft else a
        try:
            x, z = float(a[0]), float(a[1])
            tol, tmo = float(a[2]) if len(a) > 2 else 1.0, float(a[3]) if len(a) > 3 else 30
            try:
                goto(x, z, tol, tmo)
            except Fail as e:   # into the next room: through the door at its wall's middle, then again
                s = state() or {}
                pts = door_path(s.get("hx"), s.get("hz"), x, z) if ctx.get("rooms") and alive() else []
                if not pts:
                    raise
                say(f"  {e}")
                fight("ANY", 30, 2.5, soft=True)   # an enemy left bars the room's doors (east13, east26)
                for dx, dz in pts:
                    goto(dx, dz, 0.6, 10)
                goto(x, z, tol, tmo)
        except Fail as e:
            if not soft or not alive():
                raise
            say(f"  (soft) {e}")
    elif op == "switches":
        hit_switches()
    elif op == "rooms":
        ctx["rooms"] = None if a[0] == "off" else (float(a[0]), float(a[1]))
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
    elif op == "dev":
        import devtools   # tests/devtools.py
        rest = line.split(None, 1)[1] if len(line.split(None, 1)) > 1 else ""
        tmo = 30.0
        m = re.search(r"\s@(\d+(?:\.\d+)?)$", rest)
        if m:
            tmo, rest = float(m.group(1)), rest[:m.start()]
        if dev_channel is None:
            dev_channel = devtools.Channel(run)
        try:
            ans = dev_channel.send(rest, tmo, alive)
        except devtools.Refused as e:
            raise Fail(f"dev {rest}: refused: {e}")
        except TimeoutError as e:
            raise Fail(f"dev {rest}: {e}")
        out = ans["out"]
        say(f"dev {rest}: {out if isinstance(out, str) else json.dumps(out)}"[:600])
    elif op == "peekobj":   # peekobj <lua expr> <offset> [n]: words of the native object behind a script object (logged)
        import devtools
        import struct
        if dev_channel is None:
            dev_channel = devtools.Channel(run)
        off = int(a[1], 0)
        ptr, ws = obj_words(dev_channel, a[0], off, int(a[2]) if len(a) > 2 else 8)
        say(f"  peekobj {a[0]} at 0x{ptr:08X}: " + " ".join(
            f"+0x{off + 4 * k:X}={v}/{struct.unpack('>f', struct.pack('>I', v))[0]:.4g}" for k, v in enumerate(ws)))
    elif op == "devtry":   # as `dev`, but a refusal or no answer is logged and the script goes on (a sweep's map)
        import devtools
        rest = line.split(None, 1)[1] if len(line.split(None, 1)) > 1 else ""
        tmo = 30.0
        m = re.search(r"\s@(\d+(?:\.\d+)?)$", rest)
        if m:
            tmo, rest = float(m.group(1)), rest[:m.start()]
        if dev_channel is None:
            dev_channel = devtools.Channel(run)
        try:
            out = dev_channel.send(rest, tmo, alive)["out"]
            say(f"devtry {rest}: {out if isinstance(out, str) else json.dumps(out)}"[:600])
        except (devtools.Refused, TimeoutError) as e:
            if not alive():
                raise Fail(f"devtry {rest}: the game exited")
            say(f"devtry {rest}: not done (going on): {e}"[:600])
    elif op == "devfail":   # a developer-tools command that must be refused (its error path)
        import devtools
        rest = line.split(None, 1)[1] if len(line.split(None, 1)) > 1 else ""
        if dev_channel is None:
            dev_channel = devtools.Channel(run)
        try:
            ans = dev_channel.send(rest, 30.0, alive)
            raise Fail(f"devfail {rest}: answered, not refused: {ans['out']}")
        except devtools.Refused as e:
            say(f"devfail {rest}: refused as expected: {e}"[:600])
        except TimeoutError as e:
            raise Fail(f"devfail {rest}: {e}")
    elif op == "devfight":   # devfight <timeout> [flagN] [soft]
        import devtools
        if dev_channel is None:
            dev_channel = devtools.Channel(run)
        flag = next((int(x[4:]) for x in a[1:] if re.fullmatch(r"flag\d+", x)), None)
        hold = next(([float(v) for v in x[5:].split(",")] for x in a[1:] if x.startswith("hold@")), None)
        face = next((x[5:].upper() for x in a[1:] if x.startswith("face@")), None)
        if face and (face not in ("UP", "DOWN", "LEFT", "RIGHT") or not hold):
            raise Fail(f"devfight: face@{face} wants UP, DOWN, LEFT or RIGHT, with hold@X,Z")
        aim_at = next((x for x in a[1:] if x == "aim" or x.startswith("aim@")), None)
        cast_at = next((x[5:].split(",") for x in a[1:] if x.startswith("cast@")), None)
        cast = (cast_at[0], float(cast_at[1]) if len(cast_at) > 1 else 30.0) if cast_at else None
        farm_at = next((int(x[5:]) for x in a[1:] if x.startswith("farm@")), None)
        cycle_at = next((float(x[6:]) for x in a[1:] if x.startswith("cycle@")), None)
        life_at_off = next((int(x[5:], 0) for x in a[1:] if x.startswith("life@")), None)
        map_at = next((x[4:] for x in a[1:] if x.startswith("map@")), None)
        nav = None
        if map_at:   # beside the drive, or in the run
            mp = next((q for q in (os.path.join(os.path.dirname(os.path.abspath(script)), map_at), os.path.join(run, map_at))
                       if os.path.exists(q)), None)
            if not mp:
                raise Fail(f"devfight: no map {map_at}")
            nav = Nav(json.load(open(mp, encoding="utf-8")))
            say(f"  devfight: the arena's map {map_at}: {len(nav.cells)} cells, {len(nav.hook_list)} pulls")
        devfight(dev_channel, float(a[0]) if a else 120, flag, soft="soft" in a, hold=hold, face=face, aim=bool(aim_at),
                 aim_reach=float(aim_at[4:]) if aim_at and aim_at.startswith("aim@") else 0.0, wire="wire" in a, nav=nav,
                 cast=cast, collect="collect" in a, farm=farm_at, cycle=cycle_at, life_off=life_at_off)
    elif op == "devexplore":   # devexplore <out.json> [timeout]: the arena's walkable cells and the Wire Rod's pulls
        import devtools
        if dev_channel is None:
            dev_channel = devtools.Channel(run)
        devexplore(dev_channel, a[0], float(a[1]) if len(a) > 1 else 900)
    elif op == "devpage":   # devpage <timeout> [soft]: CROSS until the game runs no event (the tools' status)
        import devtools
        if dev_channel is None:
            dev_channel = devtools.Channel(run)
        tmo, t0, presses = float(a[0]) if a else 60, time.time(), 0
        while True:
            try:
                st = dev_channel.status(timeout=10, alive=alive)
            except (devtools.Refused, TimeoutError):
                st = {}
            if st.get("event_state") == 0 and (presses or "ifany" in a):
                if presses:
                    say(f"  devpage: the event ended ({presses} presses, {time.time() - t0:.1f} s)")
                break
            if time.time() - t0 > tmo:
                why = f"devpage: an event still running after {tmo:g} s (state {st.get('event_state')})"
                if "soft" in a:
                    say(f"  (soft) {why}")
                    break
                raise Fail(why)
            if "no" in a and st.get("event_state") == 6:   # a question: its second choice
                say("  devpage: a question, answered with its second choice")
                press(BTN["DOWN"])
                wait(0.4)
            press(BTN["CROSS"])
            presses += 1
            wait(1.2)
    elif op == "devcollect":   # devcollect [timeout]
        import devtools
        if dev_channel is None:
            dev_channel = devtools.Channel(run)
        devcollect(dev_channel, float(a[0]) if a else 120)
    elif op == "devstate":
        import devtools
        cs, tmo = conds(a[0]), float(a[1]) if len(a) > 1 else 60
        if dev_channel is None:
            dev_channel = devtools.Channel(run)
        t0, s0, s = time.time(), None, None
        while True:
            try:
                st = dev_channel.status(timeout=10, alive=alive)
                s = {k: float(v) for k, v in st.items() if isinstance(v, (int, float))}
                for k, _, _, _ in cs:   # M11.1: flagN -- the game's event flag N, through `flag get N`
                    if re.fullmatch(r"flag\d+", k):
                        o = dev_channel.send(f"flag get {k[4:]}", timeout=10, alive=alive)["out"]
                        s[k] = float((o if isinstance(o, dict) else json.loads(o))["value"])
            except (devtools.Refused, TimeoutError):
                s = None
            if s is not None:
                s0 = s0 or s
                if not holds(cs, s, s0):
                    say("devstate " + " ".join(f"{k}={v:g}" for k, v in s.items()))
                    break
            if time.time() - t0 > tmo:
                raise Fail(f"devstate not reached in {tmo:g} s: " + ", ".join(holds(cs, s, s0) if s else ["no status"]))
            wait(0.5)
    elif op == "expect":
        expecting = conds(a[0]) if a else []
        check()
    elif op == "actors" and a and a[0] == "list":
        # every character and object within 40 units, nearest first
        if actors() is None:
            raise Fail("actors list: no actors.txt (DOTHEROES_DEV_STATE=1?)")
        s = state() or {}
        hx, hz = s.get("hx", 0.0), s.get("hz", 0.0)
        life = {(c, round(x, 1), round(z, 1)): h for c, x, z, h in last_actor_list}
        for c, x, y, z in sorted(last_object_list, key=lambda o: (o[1] - hx) ** 2 + (o[3] - hz) ** 2):
            d = ((x - hx) ** 2 + (z - hz) ** 2) ** 0.5
            h = life.get((c, round(x, 1), round(z, 1)))
            say(f"  {c} at ({x:.1f}, {y:.1f}, {z:.1f}), {d:.1f} away" + (f", life {h}" if h else ""))
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
    elif op == "touchhold":   # touchhold <file> <secs>: as touch, the file kept open SECS s (Python shares no delete:
        acted()               # the game's removal fails meanwhile -- a trigger must still act once, devtools_triggers)
        with open(os.path.join(run, a[0]), "w"):
            wait(float(a[1]) if len(a) > 1 else 0.3)
    elif op == "write":   # write <file> <text ...>: the text as one line (the in-game menu's overlay.input, say)
        acted()
        parts = line.split(None, 2)
        with open(os.path.join(run, a[0]) + ".tmp", "w", newline="\n") as f:
            f.write((parts[2] if len(parts) > 2 else "") + "\n")
        os.replace(os.path.join(run, a[0]) + ".tmp", os.path.join(run, a[0]))
    elif op == "life":   # set the hero's life once (DOTHEROES_DEV_STATE): route discovery, not measurement
        acted()
        with open(os.path.join(run, "life.trigger"), "w") as f:
            f.write(f"{int(a[0])}\n")
        wait(0.5)
        say(f"life set to {int(a[0])} half apples")
    elif op == "place":   # put the hero at x z (DOTHEROES_DEV_STATE): a camera matched to another capture's
        acted()
        with open(os.path.join(run, "pos.trigger"), "w") as f:
            f.write(f"{float(a[0])} {float(a[1])}\n")
        wait(0.5)
        say(f"hero placed at ({float(a[0])}, {float(a[1])})")
    elif op == "select":   # select <BUTTON> <ref.png> [max]: the circle slot cycled to the item the reference shows
        select_slot(a[0], a[1], int(a[2]) if len(a) > 2 else 12)
    elif op == "shot":
        shot(a[0] if a else f"line{lineno}")
    elif op == "stop":
        open(os.path.join(run, "stop.requested"), "w").close()   # for the guard: the game removes stop.trigger
        open(os.path.join(run, "stop.trigger"), "w").close()
        say("stop")
    elif op == "quit":   # the game asked to exit as closing the window asks it (quit.trigger): its own shutdown
        open(os.path.join(run, "quit.trigger"), "w").close()
        say("quit")
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
