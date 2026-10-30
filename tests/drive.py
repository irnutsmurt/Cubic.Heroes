#!/usr/bin/env python3
"""Closed-loop input driver for a running test (tests/boot_smoke.sh).

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
  repeat <n> <BUTTON> <sec>   press n times, <sec> apart

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
def press(mask, hold=10):
    last_press_frame[0] = frame()
    with open(os.path.join(run, "pad.txt"), "w") as f:
        f.write(f"0x{mask:04X} {hold}\n")
    # wait for the game to take it (the file is emptied on admission)
    t0 = time.time()
    while time.time() - t0 < 10:
        if os.path.getsize(os.path.join(run, "pad.txt")) == 0: return True
        time.sleep(0.05)
    return False

def mask_of(tok):
    return int(tok, 0) if tok.startswith("0x") else BTN[tok.upper()]

for lineno, raw in enumerate(open(script), 1):
    line = raw.split("#", 1)[0].strip()
    if not line: continue
    op, *a = line.split()
    print(f"[drive] {lineno}: {line}", flush=True)
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
    elif op == "touch":
        open(os.path.join(run, a[0]), "w").close()
    elif op == "shot":
        shot(a[0] if a else f"line{lineno}")
    else:
        sys.exit(f"[drive] bad op {op!r} at line {lineno}")
print("[drive] done"); shot("end")
