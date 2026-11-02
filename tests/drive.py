#!/usr/bin/env python3
"""Closed-loop input driver for a running test (tests/boot_smoke.py).

Runs a step list against <run dir>: it waits for log markers, presses buttons
through the game's PAD_FILE (<run>/pad.txt), waits for the screen to respond,
and saves a screenshot after every step as <run>/drive/NN-<name>.png.

  tests/drive.py <run dir> <script>

Script lines (# comments allowed):
  log <regex> [timeout]       wait until stdout/stderr matches
  sleep <sec>
  press <BUTTON|0xMASK> [hold]   press; hold = guest pad reports (default 10)
  change [timeout]            wait until the frame differs from the frame at
                              the last press (the game reacted)
  shot <name>                 save a screenshot
  touch <file>                create <run>/<file> (e.g. a capture trigger)
  state <cond>[,<cond>] [timeout]  wait until the game's state (DOTHEROES_DEV_STATE=1,
                              <run>/state.txt: t life x z steps topups area) meets every
                              condition, e.g. `state area==18,life>0 60` or `state steps>+20`
                              (a signed value is relative to the step's start); a timeout
                              ends the drive with status 1
  repeat <n> <BUTTON> <sec>   press n times, <sec> apart
  hold <BUTTON> <sec>         hold for <sec> of wall time (not guest reports:
                              those pass twice as fast at 60 fps)

Buttons: START SELECT CROSS CIRCLE SQUARE TRIANGLE UP DOWN LEFT RIGHT L1 R1 L2 R2.
Exit status 0 when every step completed, 1 on a timeout (the step is named).
"""
import os, re, sys, time
from PIL import Image, ImageChops, ImageFile, ImageStat
ImageFile.LOAD_TRUNCATED_IMAGES = True

BTN = {"SELECT": 0x0001, "START": 0x0008, "UP": 0x0010, "RIGHT": 0x0020, "DOWN": 0x0040,
       "LEFT": 0x0080, "L2": 0x0100, "R2": 0x0200, "L1": 0x0400, "R1": 0x0800,
       "TRIANGLE": 0x1000, "CIRCLE": 0x2000, "CROSS": 0x4000, "SQUARE": 0x8000}

run, script = sys.argv[1], sys.argv[2]
TIMING = os.environ.get("DRIVE_TIMING") == "1"   # each step's time and each press's waits in drive.log
# A press keeps the frame it was made on only for a `change` step to compare against. Kept
# for every press, it cost each press ~1 s when frame dumps are off (DUMP=0: no frame.ppm, and
# frame() retries 20 x 50 ms) -- a fight's swings came every ~1.4 s instead of ~0.4 s.
NEEDS_FRAME = any(l.split("#", 1)[0].split()[:1] == ["change"] for l in open(script))
out = os.path.join(run, "drive"); os.makedirs(out, exist_ok=True)
nshot = [0]

def alive():
    # boot_smoke writes the summary only after the game exits
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

def logtext():
    t = ""
    for f in ("stdout.log", "stderr.log"):
        try: t += open(os.path.join(run, f), errors="ignore").read()
        except OSError: pass
    return t

last_press_frame = [None]
T0 = time.time()
def press(mask, hold=10, ms=0):
    t_frame = time.time()
    if NEEDS_FRAME:   # only `change` compares with it; with frame dumps off, frame() waits ~1 s for nothing
        last_press_frame[0] = frame()
    t_write = time.time()
    with open(os.path.join(run, "pad.txt"), "w") as f:
        f.write(f"0x{mask:04X} {hold} {ms}\n" if ms else f"0x{mask:04X} {hold}\n")
    # wait for the game to take it (the file is emptied on admission)
    t0 = time.time()
    while time.time() - t0 < 10:
        if os.path.getsize(os.path.join(run, "pad.txt")) == 0:
            if TIMING:
                print(f"[drive]   +{t_write - T0:.3f} s 0x{mask:04X} {hold} {ms}: frame grab {1000 * (t_write - t_frame):.0f} ms, "
                      f"taken after {1000 * (time.time() - t0):.0f} ms", flush=True)
            return True
        time.sleep(0.05)
    return False

def mask_of(tok):
    return int(tok, 0) if tok.startswith("0x") else BTN[tok.upper()]

# DOTHEROES_DEV_STATE=1 (port/native/dev_state.cpp): <run>/state.txt, rewritten every 100 ms
STATE_KEYS = ("t", "life", "x", "z", "steps", "topups", "area")
CMP = {"<=": lambda a, b: a <= b, ">=": lambda a, b: a >= b, "==": lambda a, b: a == b,
       "!=": lambda a, b: a != b, "<": lambda a, b: a < b, ">": lambda a, b: a > b}

def state():
    try:
        f = open(os.path.join(run, "state.txt")).read().split()
        return dict(zip(STATE_KEYS, map(float, f))) if len(f) == len(STATE_KEYS) else None
    except (OSError, ValueError):
        return None

def cond(text):   # key op value; a value with a sign (+5, -3) is relative to the state as the step began
    m = re.fullmatch(r"(\w+)(<=|>=|==|!=|<|>)([+-]?)([\d.]+)", text)
    if not m or m.group(1) not in STATE_KEYS:
        sys.exit(f"[drive] bad state condition {text!r}")
    k, o, sign, v = m.groups()
    return k, o, float(v) * (-1 if sign == "-" else 1), bool(sign)

for lineno, raw in enumerate(open(script), 1):
    line = raw.split("#", 1)[0].strip()
    if not line: continue
    op, *a = line.split()
    print(f"[drive] {lineno}: {line}" + (f"  (+{time.time() - T0:.3f} s)" if TIMING else ""), flush=True)
    if op == "log":
        rx, tmo = re.compile(a[0]), float(a[1]) if len(a) > 1 else 120
        t0 = time.time()
        while not rx.search(logtext()):
            if time.time() - t0 > tmo:
                print(f"[drive] TIMEOUT line {lineno}: {line}"); shot("timeout"); sys.exit(1)
            time.sleep(0.5)
    elif op == "sleep":
        time.sleep(float(a[0]))
    elif op == "press":
        if not press(mask_of(a[0]), int(a[1]) if len(a) > 1 else 10):
            print(f"[drive] press not taken at line {lineno}")
    elif op == "hold":
        press(mask_of(a[0]), 1, int(float(a[1]) * 1000)); time.sleep(float(a[1]))
    elif op == "repeat":
        for _ in range(int(a[0])):
            press(mask_of(a[1])); time.sleep(float(a[2]))
    elif op == "change":
        tmo = float(a[0]) if a else 20; t0 = time.time()
        # A small dialog over a static screen changes few pixels: low threshold,
        # and a miss is reported but not fatal (the next step may still apply).
        while diff(frame(), last_press_frame[0]) < 1.0:
            if time.time() - t0 > tmo:
                print(f"[drive] no screen change after line {lineno} (continuing)"); break
            time.sleep(0.25)
    elif op == "state":
        conds, tmo = [cond(c) for c in a[0].split(",")], float(a[1]) if len(a) > 1 else 60
        t0, s0 = time.time(), None
        while True:
            s = state()
            if s is not None:
                s0 = s0 or s
                if all(CMP[o](s[k], (s0[k] + v) if rel else v) for k, o, v, rel in conds):
                    print(f"[drive] state {' '.join(f'{k}={s[k]:g}' for k in STATE_KEYS)}", flush=True)
                    break
            if time.time() - t0 > tmo:
                print(f"[drive] TIMEOUT line {lineno}: {line} (state {s})"); sys.exit(1)
            time.sleep(0.1)
    elif op == "touch":
        open(os.path.join(run, a[0]), "w").close()
    elif op == "shot":
        shot(a[0] if a else f"line{lineno}")
    else:
        sys.exit(f"[drive] bad op {op!r} at line {lineno}")
print("[drive] done"); shot("end")
