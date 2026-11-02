#!/usr/bin/env python3
"""Window grabs of the game running in RPCS3, for side-by-side comparisons
with the port (colour, the post-processing): an offline reference only.

  tests/rpcs3_capture.py <rpcs3 dir> <game: disc image or EBOOT> <out dir>
      [--config <rpcs3 config.yml>] [--secs 90] [--every 5]

Boots the game with `rpcs3 --no-gui` (with --config, that file for this boot
only: e.g. a copy of the game's own config with fullscreen off), keeps its
window above the others without taking the focus, saves the window's client
area to <out dir>/NNNN.png every --every seconds, then stops the RPCS3 it
started. Nothing is sent to the game: what it shows on its own (logos, the
title, the attract mode). Windows only. The grabs show the game's picture:
keep them local.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

from PIL import ImageGrab

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("rpcs3_dir")
ap.add_argument("game")
ap.add_argument("out")
ap.add_argument("--config")
ap.add_argument("--secs", type=float, default=90)
ap.add_argument("--every", type=float, default=5)
a = ap.parse_args()

u = ctypes.windll.user32
u.SetProcessDPIAware()
os.makedirs(a.out, exist_ok=True)
cmd = [os.path.join(a.rpcs3_dir, "rpcs3.exe"), "--no-gui"]
if a.config:
    cmd += ["--config", os.path.abspath(a.config)]
cmd.append(a.game)
proc = subprocess.Popen(cmd, cwd=a.rpcs3_dir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def game_window(pid):
    """The process's game window: the visible top-level window whose title
    carries RPCS3's frame rate ("FPS"); the boot's shader-compile splash is
    another, larger window."""
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def each(h, _):
        p = wt.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u.IsWindowVisible(h):
            title = ctypes.create_unicode_buffer(256)
            u.GetWindowTextW(h, title, 256)
            if "FPS" in title.value:
                found.append(h)
        return True
    u.EnumWindows(each, 0)
    return found[0] if found else None


HWND_TOPMOST, SWP_NOMOVE, SWP_NOSIZE, SWP_NOACTIVATE = -1, 0x2, 0x1, 0x10
t0, n, next_grab = time.time(), 0, a.every
try:
    while time.time() - t0 < a.secs and proc.poll() is None:
        time.sleep(0.5)
        if time.time() - t0 < next_grab:
            continue
        next_grab += a.every
        h = game_window(proc.pid)
        if not h:
            continue
        u.SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
        r, pt = wt.RECT(), wt.POINT(0, 0)
        u.GetClientRect(h, ctypes.byref(r))
        u.ClientToScreen(h, ctypes.byref(pt))
        if r.right < 64 or r.bottom < 64:
            continue
        ImageGrab.grab(bbox=(pt.x, pt.y, pt.x + r.right, pt.y + r.bottom), all_screens=True).save(
            os.path.join(a.out, f"{int(time.time() - t0):04d}.png"))
        n += 1
finally:
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
print(f"{n} grabs in {a.out}")
sys.exit(0 if n else 1)
