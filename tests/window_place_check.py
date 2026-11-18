#!/usr/bin/env python3
"""The installed game remembers its window's place (M10.1; checked again after B-010's orderly end, M11.1b): an
installation's game started windowed, its window moved and resized (Win32), closed as a player closes it (WM_CLOSE:
the game's own end), config/window.json read; started again: the window where it was left. A window's close asks the in-game menu's exit
question first (M10.2); a second close at the question ends the game, as a player's second click does. On screen -- run only
with the owner's approval in force (tests/desktop_guard.py allow MINUTES "WHAT"); Windows only.

    tests/window_place_check.py --install DIR [--x 180 --y 140 --w 1100 --h 640]

The installation's own config/window.json is kept aside and put back afterwards.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import desktop_guard  # noqa: E402

u = ctypes.windll.user32
WM_CLOSE = 0x0010


def windows_of(pid):
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def each(h, _):
        p = wt.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u.IsWindowVisible(h):
            t = ctypes.create_unicode_buffer(256)
            u.GetWindowTextW(h, t, 256)
            if t.value:
                found.append(h)
        return True
    u.EnumWindows(each, 0)
    return found


def rect(h):
    r = wt.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return r.left, r.top, r.right - r.left, r.bottom - r.top


def start(exe, root):
    env = dict(os.environ)
    for k in list(env):
        if k.startswith(("DOTHEROES_", "PS3_", "PS3RECOMP_", "DIAG_")):
            del env[k]
    env["DOTHEROES_DISPLAY_MODE"] = "windowed"
    return subprocess.Popen([exe], cwd=root, env=env)


def wait_window(p, secs=60):
    t0 = time.time()
    while time.time() - t0 < secs:
        ws = windows_of(p.pid)
        if ws:
            return ws[0]
        if p.poll() is not None:
            return None
        time.sleep(0.5)
    return None


def close_game(p, h, secs=60):
    """The player's close: the window's close button (the exit question), then again at the question."""
    u.PostMessageW(h, WM_CLOSE, 0, 0)
    try:
        return p.wait(4)
    except subprocess.TimeoutExpired:
        pass
    u.PostMessageW(h, WM_CLOSE, 0, 0)
    return p.wait(secs)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--install", required=True)
    ap.add_argument("--x", type=int, default=180)
    ap.add_argument("--y", type=int, default=140)
    ap.add_argument("--w", type=int, default=1100)
    ap.add_argument("--h", type=int, default=640)
    a = ap.parse_args()
    if not desktop_guard.onscreen_allowed():
        sys.exit("on-screen runs are not approved (tests/desktop_guard.py allow MINUTES \"WHAT\")")
    root = os.path.abspath(a.install)
    act = json.load(open(os.path.join(root, "active.json"), encoding="utf-8"))["active"]
    exe = os.path.join(root, "builds", act, "dotheroes.exe")
    wj = os.path.join(root, "config", "window.json")
    kept = wj + ".kept-by-test"
    if os.path.exists(wj):
        shutil.copy2(wj, kept)
    crash = os.path.join(root, "logs", "crash")
    reports0 = set(os.listdir(crash)) if os.path.isdir(crash) else set()
    bad = []
    try:
        p = start(exe, root)
        h = wait_window(p)
        if not h:
            sys.exit("the game's window never appeared")
        time.sleep(8)   # past the logos' start
        u.ShowWindow(h, 9)   # SW_RESTORE
        u.SetWindowPos(h, 0, a.x, a.y, a.w, a.h, 0x0004)   # SWP_NOZORDER
        time.sleep(2)
        moved = rect(h)
        print("moved to", moved)
        try:
            code = close_game(p, h)
        except subprocess.TimeoutExpired:
            p.kill()
            bad.append("the game did not end within 60 s of the second WM_CLOSE")
            code = None
        print("exit status", code)
        if code not in (0, None):
            bad.append(f"the game's exit status {code}")
        saved = json.load(open(wj, encoding="utf-8")) if os.path.exists(wj) else None
        print("window.json", saved)
        if not saved:
            bad.append("config/window.json not written")
        p = start(exe, root)
        h = wait_window(p)
        if not h:
            bad.append("the second start's window never appeared")
        else:
            time.sleep(3)
            again = rect(h)
            print("second start at", again)
            if abs(again[0] - moved[0]) > 2 or abs(again[1] - moved[1]) > 2 or abs(again[2] - moved[2]) > 2 or \
                    abs(again[3] - moved[3]) > 2:
                bad.append(f"the window came back at {again}, left at {moved}")
            try:
                close_game(p, h)
            except subprocess.TimeoutExpired:
                p.kill()
                bad.append("the second start did not end within 60 s of the second WM_CLOSE")
    finally:
        if os.path.exists(kept):
            shutil.move(kept, wj)
    # The exit question holds the game (M10.2): no flips while it is up, and no hang report for it (M11.1).
    new = sorted((set(os.listdir(crash)) if os.path.isdir(crash) else set()) - reports0)
    if new:
        bad.append("crash or hang reports written: " + ", ".join(new))
    print("PASS" if not bad else "FAIL: " + "; ".join(bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
