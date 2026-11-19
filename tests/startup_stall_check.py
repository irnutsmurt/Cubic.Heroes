#!/usr/bin/env python3
"""The graphics' start watched (M11.4): a window system that never answers ends the game with a report and status 70,
not a hang with no window and no word (port/main.cpp startup_watch).

    python3 tests/startup_stall_check.py --bin GAME [--timeout 10]

A fake X server -- a socket at a free display number that accepts a client and never answers it, as a stalled
XWayland did -- is the window system; the game is started for it (SDL's x11 driver, a window asked for), with
DOTHEROES_STARTUP_TIMEOUT_S=--timeout. Pass: the game ends by itself within the timeout and a margin, with status 70,
its log naming the stall, a startup-*.txt report written. Nothing reaches the desktop's display: the run's only
display is the fake one (the desktop guard's approval is still asked for, as the game connects to an X socket).
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def fake_x_server():
    """a listening X socket that accepts and never replies; returns (display number, closer)"""
    for n in range(90, 200):
        path = f"/tmp/.X11-unix/X{n}"
        if not os.path.exists(path) and not os.path.exists(f"/tmp/.X{n}-lock"):
            break
    else:
        raise SystemExit("no free X display number")
    srv = socket.socket(socket.AF_UNIX)
    srv.bind(path)
    srv.listen(8)
    held = []

    def serve():
        try:
            while True:
                held.append(srv.accept()[0])   # held open, never read from or written to
        except OSError:
            pass
    threading.Thread(target=serve, daemon=True).start()

    def close():
        for c in held:
            c.close()
        srv.close()
        os.remove(path)
    return n, close


def main():
    ap = argparse.ArgumentParser(description="the graphics' start watched: a stalled window system")
    ap.add_argument("--bin", required=True)
    ap.add_argument("--timeout", type=int, default=10)
    a = ap.parse_args()
    n, close = fake_x_server()
    run = tempfile.mkdtemp(prefix="stall-check-")
    try:
        env = {k: v for k, v in os.environ.items() if k not in ("WAYLAND_DISPLAY", "WAYLAND_SOCKET")}
        env.update(DISPLAY=f":{n}", WAYLAND_DISPLAY="no-wayland-here", SDL_VIDEODRIVER="x11", PS3RECOMP_VK_WINDOW="1",
                   DOTHEROES_STARTUP_TIMEOUT_S=str(a.timeout), DIAG_CRASH_DIR=run, DOTHEROES_LOG_TO_CONSOLE="1",
                   PS3_GAMEDATA_ROOT=os.path.join(run, "gamedata"))
        elf = os.path.join(ROOT, "build", "disc", "PS3_GAME", "USRDIR", "EBOOT.elf")
        t0 = time.time()
        p = subprocess.run([os.path.abspath(a.bin), elf], cwd=run, env=env, capture_output=True, text=True,
                           timeout=a.timeout + 60)
        secs = time.time() - t0
        log = p.stdout + p.stderr
        reports = [f for f in os.listdir(run) if f.startswith("startup-")]
        bad = []
        if p.returncode != 70:
            bad.append(f"status {p.returncode}, not 70")
        if secs > a.timeout + 30:
            bad.append(f"ended after {secs:.0f} s (the timeout {a.timeout} s)")
        if "the graphics did not finish starting" not in log:
            bad.append("the log does not name the stall")
        if not reports:
            bad.append("no startup-*.txt report")
        print(("PASS" if not bad else "FAIL") + f": a stalled window system: ended in {secs:.1f} s with status "
              f"{p.returncode}; report {reports}" + (f" -- {'; '.join(bad)}" if bad else ""))
        for ln in log.splitlines():
            if "[startup]" in ln or "startup:" in ln:
                print("  " + ln[:200])
        return 1 if bad else 0
    finally:
        close()
        shutil.rmtree(run, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
