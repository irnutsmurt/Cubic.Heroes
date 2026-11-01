#!/usr/bin/env python3
"""Boot smoke test: run the port isolated from the desktop and the player's
settings, in strict mode, and summarize how far it got. Linux and Windows.

  python tests/boot_smoke.py [seconds]        (default 90)

Environment
  SHOT_EVERY=<sec>   save a screenshot of the last presented frame every <sec>
                     seconds to <run>/shots/NNNN.png (the backend's
                     PS3RECOMP_VK_DUMP frame). Default 10; 0 = off.
  RUN_TAG=<name>     suffix for the run directory.
  KEEP_RUNS=<n>      run directories kept under build/test (default 5; the
                     oldest are deleted when a run starts). The live frame
                     dump (frame.ppm) is deleted when a run ends. Each run
                     records what it ran in run.json (executable hash, source
                     revisions, switches); tests/keep_baseline.py keeps a
                     run's summary in build/baselines/, which no run deletes.
  PS3RECOMP_VK_DUMP_EVERY=<n>  refresh frame.ppm every n-th present (default
                     15; dumping every frame costs several fps).
  VISIBLE=1          (Windows, with WINDOW=1) a run valid only while the game is
                     on screen: not started on a locked desktop, and stopped,
                     with guard.json saying why, if the desktop locks, the
                     game's window is minimised, hidden or gone, or another
                     window covers more than a tenth of it for 3 s. A covered
                     window is no longer guaranteed the 1 ms timer (Windows
                     11): the game ran at ~35 fps behind the Claude app's
                     window, at ~59 in front of it.
  TOPMOST=1          with VISIBLE=1: the game's window kept above other windows
                     (without taking the focus), so another window coming to
                     the front does not cover it.
  GRAB_EVERY=<s>     with VISIBLE=1: the window's screen area to grabs/ every
                     <s> seconds -- what was actually on screen.
  DUMP=0             no frame.ppm at all, for timing runs: the dump is written
                     on the present path and skews it. No screenshots then,
                     and a drive script may not use `change` or `shot`.
  DRIVE=<script>     run tests/drive.py with this script against the game
                     (closed-loop input; see tests/scripts/).
  SAVES=<dir>        seed the run's save data with a copy of this game's save
                     folders (BLUS30490*) found in <dir>, e.g. an emulator's
                     dev_hdd0/home/00000001/savedata. <dir> is only read.
  GAMEDATA=<dir>     use <dir> as the game's data root (dev_hdd0, saves,
                     trophies) in place, read and written, instead of a fresh
                     per-run root: for play sessions whose saves must last, e.g.
                     %LOCALAPPDATA%/dotheroes/gamedata. Not with SAVES.
  WINDOW=1           show the game's window (PS3RECOMP_VK_WINDOW=1). By default
                     nothing appears on the desktop: rendering is offscreen.
  AUDIO=1            play through the real audio device instead of SDL's dummy
                     driver.
  BIN=<exe>          the game (default build/port/dotheroes, or
                     build/port-win/dotheroes.exe on Windows).
  WRAP=<json list>   launch the game through this command prefix, e.g. a
                     profiler (tests/nsys_capture.py); the wrapper ends the
                     run itself, so give a generous <seconds>.
  PASS_DUMP=1        capture a frame pass by pass into <run>/passes/ when
                     <run>/passes.trigger appears (a drive script's
                     `touch passes.trigger`), with every pipeline's shaders
                     (pipe<N>.ps.hlsl); see RSX_ENGINE_PASS_DUMP and
                     RSX_ENGINE_SHADER_DUMP in the runtime's rsx_draw_engine.c.

Input: <run>/pad.txt is the game's PAD_FILE. Writing a button mask there
presses it (e.g. `echo 0x0008 > pad.txt` = START), so a driver outside the
process can look at the screenshots and decide the next button.

Isolation: its own cwd and save root under build/test/, SDL dummy audio
(nothing reaches the speakers), no window unless WINDOW=1, no crash dialogs
or core dumps, and only the process it started is ever stopped.
"""
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WIN = os.name == "nt"
SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 90
BIN = os.environ.get("BIN") or os.path.join(ROOT, "build", *(("port-win", "dotheroes.exe") if WIN
                                                             else ("port", "dotheroes")))
ELF = os.path.join(ROOT, "build", "disc", "PS3_GAME", "USRDIR", "EBOOT.elf")
TAG = os.environ.get("RUN_TAG")
RUN = os.path.join(ROOT, "build", "test", "boot-" + time.strftime("%Y%m%d-%H%M%S") + (f"-{TAG}" if TAG else ""))
SHOT_EVERY = int(os.environ.get("SHOT_EVERY", "10"))
VISIBLE = os.environ.get("VISIBLE") == "1"
TOPMOST = os.environ.get("TOPMOST") == "1"
GRAB_EVERY = float(os.environ.get("GRAB_EVERY", "0"))


def game_window(pid):
    """Windows: the game's own titled top-level window, or None."""
    import ctypes, ctypes.wintypes as wt
    u, found = ctypes.WinDLL("user32"), []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        p = wt.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u.GetWindowTextLengthW(h) > 0:
            found.append(h)
        return True
    u.EnumWindows(cb, 0)
    return found[0] if found else None


def rd(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def desktop_locked():
    """Windows: is this session's desktop locked? A locked desktop shows no
    window, so window grabs see the lock screen and a windowed run's
    presents reach no display (their pacing may differ from a visible run)."""
    if not WIN:
        return None
    try:
        import ctypes, ctypes.wintypes as wt, struct
        wts, k = ctypes.WinDLL("wtsapi32"), ctypes.WinDLL("kernel32")
        sid, buf, n = wt.DWORD(), ctypes.c_void_p(), wt.DWORD()
        k.ProcessIdToSessionId(k.GetCurrentProcessId(), ctypes.byref(sid))
        if not wts.WTSQuerySessionInformationW(None, sid.value, 25, ctypes.byref(buf), ctypes.byref(n)):
            return None
        # WTSINFOEXW: DWORD Level, then the union at offset 8 (it holds 8-byte
        # members): SessionId, SessionState, SessionFlags (0 locked, 1 not).
        # Offset 12 is SessionState, "active" whether locked or not -- read
        # as the flag, every run until 2026-10-31 18:45 said locked.
        sess, flags = struct.unpack_from("<I4xi", ctypes.string_at(buf, n.value), 8)
        if sess != sid.value:
            return None
        wts.WTSFreeMemory(buf)
        return {0: True, 1: False}.get(flags)
    except (OSError, struct.error):
        return None


def build_identity():
    """What this run ran: the executable's hash and the two source trees'
    revisions (dirty = files changed since), and the switches it was given.
    Written to <run>/run.json; tests/keep_baseline.py keeps it with a summary."""
    import hashlib, json, subprocess
    h = hashlib.sha256()
    with open(BIN, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)

    def git(tree):
        try:
            rev = subprocess.run(["git", "-C", tree, "rev-parse", "--short=12", "HEAD"], capture_output=True,
                                 text=True).stdout.strip()
            dirty = subprocess.run(["git", "-C", tree, "status", "--porcelain", "-uno"], capture_output=True,
                                   text=True).stdout.count("\n")
            return {"rev": rev, "dirty_files": dirty}
        except OSError:
            return None
    ps3 = os.environ.get("PS3RECOMP_DIR", os.path.join(ROOT, "build", "ref", "ps3recomp"))
    prefixes = ("DOTHEROES_", "PS3_", "PS3RECOMP_", "SPU_", "VK_", "RSX_", "SPURS_", "DIAG_", "GCM_")
    return {
        "started": time.strftime("%Y-%m-%d %H:%M:%S"),
        "exe": {"name": os.path.basename(BIN), "sha256": h.hexdigest(), "bytes": os.path.getsize(BIN),
                "mtime": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(os.path.getmtime(BIN)))},
        "repo": git(ROOT), "ps3recomp": git(ps3),
        "patches": len(glob.glob(os.path.join(ROOT, "patches", "ps3recomp", "*.patch"))),
        "desktop_locked": desktop_locked(),
        "secs": SECS, "drive": os.path.relpath(os.environ["DRIVE"], ROOT) if os.environ.get("DRIVE") else None,
        "switches": {k: v for k, v in sorted(os.environ.items())
                     if k.startswith(prefixes) or k in ("DUMP", "WINDOW", "AUDIO", "SHOT_EVERY")},
    }


def main():
    if not os.path.isfile(BIN):
        print(f"no {BIN} (build port/ first)")
        return 2
    if VISIBLE and desktop_locked():
        print("VISIBLE=1: the desktop is locked; not starting a run that would not be on screen")
        return 3
    # Retention: test runs (logs, screenshots) must not pile up on disk.
    keep = int(os.environ.get("KEEP_RUNS", "5"))
    old = sorted(glob.glob(os.path.join(ROOT, "build", "test", "boot-*")))
    for d in old[:max(0, len(old) - max(keep - 1, 0))]:
        shutil.rmtree(d, ignore_errors=True)
    # GAMEDATA=<dir>: play on a persistent save root in place; nothing is copied
    # into the run, and retention never touches it.
    gamedata = os.path.abspath(os.environ["GAMEDATA"]) if os.environ.get("GAMEDATA") else os.path.join(RUN, "gamedata")
    if os.environ.get("GAMEDATA") and os.environ.get("SAVES"):
        print("GAMEDATA and SAVES exclude each other: GAMEDATA is played in place")
        return 2
    for d in ("dev_flash", "shots", gamedata):
        os.makedirs(os.path.join(RUN, d), exist_ok=True)
    open(os.path.join(RUN, "pad.txt"), "w").close()
    import json
    with open(os.path.join(RUN, "run.json"), "w") as f:
        json.dump(build_identity(), f, indent=1)
    if os.environ.get("SAVES"):
        dst = os.path.join(gamedata, "dev_hdd0", "home", "00000001", "savedata")
        for name in sorted(os.listdir(os.environ["SAVES"])):
            src = os.path.join(os.environ["SAVES"], name)
            if name.startswith("BLUS30490") and os.path.isdir(src):
                shutil.copytree(src, os.path.join(dst, name))
    drive = os.path.abspath(os.environ["DRIVE"]) if os.environ.get("DRIVE") else None

    env = dict(os.environ)
    window = os.environ.get("WINDOW") == "1"
    for k in ("PS3RECOMP_VK_WINDOW",) + (() if window else ("DISPLAY", "WAYLAND_DISPLAY")):
        env.pop(k, None)
    env.update({
        "PS3_STRICT": env.get("PS3_STRICT", "1"),
        "PS3_GAMEDATA_ROOT": gamedata,
        "PS3_HDD0_ROOT": os.path.join(gamedata, "dev_hdd0"),
        "PS3_HDD1_ROOT": os.path.join(gamedata, "dev_hdd1"),
        "PS3_DEV_FLASH": os.path.join(RUN, "dev_flash"),
        "PS3RECOMP_VK_GUEST_PROGRAMS": env.get("PS3RECOMP_VK_GUEST_PROGRAMS", "1"),
        "PS3RECOMP_VK_DUMP": os.path.join(RUN, "frame.ppm"),
        "PS3RECOMP_VK_DUMP_EVERY": env.get("PS3RECOMP_VK_DUMP_EVERY", "15"),
        "PAD_FILE": os.path.join(RUN, "pad.txt"),
        "DIAG_CRASH_DIR": os.path.join(RUN, "diag"),
        "DIAG_HANG_EXIT": env.get("DIAG_HANG_EXIT", "1"),
        "DIAG_BOOT_SECS": env.get("DIAG_BOOT_SECS", "60"),
    })
    if window:
        env["PS3RECOMP_VK_WINDOW"] = "1"
    if os.environ.get("AUDIO") != "1":
        env["SDL_AUDIODRIVER"] = "dummy"
    if os.environ.get("DUMP") == "0":
        env.pop("PS3RECOMP_VK_DUMP", None)
    if os.environ.get("PASS_DUMP") == "1":
        os.makedirs(os.path.join(RUN, "passes"))
        env["RSX_ENGINE_PASS_DUMP"] = os.path.join(RUN, "passes")
        env["RSX_ENGINE_PASS_DUMP_TRIGGER"] = os.path.join(RUN, "passes.trigger")
        env.setdefault("RSX_ENGINE_PASS_DUMP_DRAWS", "1")
        env.setdefault("RSX_ENGINE_SHADER_DUMP", os.path.join(RUN, "passes"))  # pipe<N>.ps.hlsl

    kw = {}
    if WIN:
        # No Windows Error Reporting dialog on a crash (the error mode is
        # inherited) and no console window: our crash reports carry the context.
        import ctypes
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002)  # FAILCRITICALERRORS | NOGPFAULTERRORBOX
        kw["creationflags"] = 0x08000000  # CREATE_NO_WINDOW
    elif os.environ.get("DIAG_CORE") != "1":
        # A kernel core of a 4 GB guest address space takes systemd-coredump
        # tens of seconds. DIAG_CORE=1 keeps it.
        import resource
        kw["preexec_fn"] = lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with open(os.path.join(RUN, "stdout.log"), "wb") as out, open(os.path.join(RUN, "stderr.log"), "wb") as err:
        wrap = json.loads(os.environ["WRAP"]) if os.environ.get("WRAP") else []
        proc = subprocess.Popen(wrap + [BIN, ELF], cwd=RUN, env=env, stdout=out, stderr=err, **kw)

    stop = threading.Event()

    def shooter():
        from PIL import Image, ImageFile
        ImageFile.LOAD_TRUNCATED_IMAGES = True
        n, frame, tmp = 0, os.path.join(RUN, "frame.ppm"), os.path.join(RUN, "shots", ".tmp.ppm")
        while not stop.wait(SHOT_EVERY):
            n += SHOT_EVERY
            try:
                # Copy first: the backend overwrites the dump on every present.
                if os.path.getsize(frame):
                    shutil.copyfile(frame, tmp)
                    Image.open(tmp).save(os.path.join(RUN, "shots", f"{n:04d}.png"))
            except (OSError, ValueError, SyntaxError):
                pass
    shots = threading.Thread(target=shooter, daemon=True) if SHOT_EVERY else None
    if shots:
        shots.start()

    guard_state = {"aborted": None}
    ending = threading.Event()   # the run's own stop at SECS: not a guard failure

    def guard():
        """VISIBLE=1 (Windows, with WINDOW=1): the run is only valid while the game is
        on screen. Every 0.5 s: the desktop unlocked, the game's window present, shown
        and not minimised -- else the game is stopped and the run marked aborted
        (guard.json). GRAB_EVERY=<s>: the window's screen area saved to grabs/, what
        was actually on screen."""
        import ctypes
        from PIL import ImageGrab
        import ctypes.wintypes as wt
        u = ctypes.WinDLL("user32")
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(2)
        except OSError:
            pass
        t0, seen, samples, front, grabs = time.monotonic(), False, 0, 0, 0
        covered_n, covered_since, placed = 0, None, False
        next_grab = GRAB_EVERY
        dwm = ctypes.WinDLL("dwmapi")

        def covered(h):
            # another shown window above the game's covering more than a tenth of it
            r = wt.RECT()
            u.GetWindowRect(h, ctypes.byref(r))
            area = max(1, (r.right - r.left) * (r.bottom - r.top))
            w = u.GetTopWindow(None)
            while w and w != h:
                cloaked = ctypes.c_int(0)
                dwm.DwmGetWindowAttribute(w, 14, ctypes.byref(cloaked), 4)   # DWMWA_CLOAKED
                if u.IsWindowVisible(w) and not u.IsIconic(w) and not cloaked.value:
                    o = wt.RECT()
                    u.GetWindowRect(w, ctypes.byref(o))
                    ix = max(0, min(r.right, o.right) - max(r.left, o.left))
                    iy = max(0, min(r.bottom, o.bottom) - max(r.top, o.top))
                    if ix * iy > area // 10:
                        return True
                w = u.GetWindow(w, 2)   # GW_HWNDNEXT
            return False
        os.makedirs(os.path.join(RUN, "grabs"), exist_ok=True)
        while not stop.wait(0.5):
            if ending.is_set():
                break
            now = time.monotonic() - t0
            h = game_window(proc.pid)
            why = None
            if desktop_locked():
                why = "desktop locked"
            elif h:
                seen = True
                if TOPMOST and not placed:   # HWND_TOPMOST, no move, no resize, no activation
                    u.SetWindowPos(h, wt.HWND(-1), 0, 0, 0, 0, 0x2 | 0x1 | 0x10)
                    placed = True
                if u.IsIconic(h):
                    why = "game window minimised"
                elif not u.IsWindowVisible(h):
                    why = "game window hidden"
                elif covered(h):
                    covered_n += 1
                    covered_since = covered_since if covered_since is not None else now
                    if now - covered_since >= 3:
                        why = "game window covered by another window"
                else:
                    covered_since = None
            elif seen and proc.poll() is None:
                why = "game window gone"
            if why and ending.is_set():
                break   # the window went with the run's own stop, after the check began
            if why:
                guard_state.update(aborted=why, at_s=round(now, 1))
                proc.terminate()
                break
            if h:
                samples += 1
                front += u.GetForegroundWindow() == h
                if GRAB_EVERY and now >= next_grab:
                    next_grab += GRAB_EVERY
                    r, pt = wt.RECT(), wt.POINT(0, 0)
                    u.GetClientRect(h, ctypes.byref(r))
                    u.ClientToScreen(h, ctypes.byref(pt))
                    try:
                        ImageGrab.grab(bbox=(pt.x, pt.y, pt.x + r.right, pt.y + r.bottom), all_screens=True).save(
                            os.path.join(RUN, "grabs", f"{int(now):04d}.png"))
                        grabs += 1
                    except OSError:
                        pass
        guard_state.update(samples=samples, foreground_share=round(front / samples, 3) if samples else None,
                           covered_share=round(covered_n / samples, 3) if samples else None, topmost=TOPMOST,
                           grabs=grabs)
    guarded = threading.Thread(target=guard, daemon=True) if VISIBLE and WIN else None
    if guarded:
        guarded.start()
    dr = None
    if drive:
        with open(os.path.join(RUN, "drive.log"), "wb") as log:
            dr = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "drive.py"), RUN, drive],
                                  stdout=log, stderr=subprocess.STDOUT, **({"creationflags": 0x08000000} if WIN else {}))

    timed_out = False
    try:
        rc = proc.wait(timeout=SECS)
    except subprocess.TimeoutExpired:
        timed_out = True
        ending.set()
        proc.terminate()   # SIGTERM (the port exits at once) / TerminateProcess
        try:
            rc = proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            rc = proc.wait()
    open(os.path.join(RUN, ".done"), "w").close()
    stop.set()
    if shots:
        shots.join(15)
    if guarded:
        guarded.join(5)
        with open(os.path.join(RUN, "guard.json"), "w") as f:
            json.dump(guard_state, f)
        if guard_state["aborted"]:
            print(f"ABORTED : {guard_state['aborted']} at {guard_state.get('at_s')} s -- not a valid on-screen run")
    for f in ("frame.ppm", os.path.join("shots", ".tmp.ppm")):
        try:
            os.remove(os.path.join(RUN, f))
        except OSError:
            pass
    if dr:
        time.sleep(1)
        if dr.poll() is None:
            dr.kill()

    stderr = rd(os.path.join(RUN, "stderr.log"))
    diag = os.path.join(RUN, "diag")
    reports = sorted(os.listdir(diag)) if os.path.isdir(diag) else []
    flips = re.findall(r"frames presented \(guest flips\): [0-9]+",
                       stderr + "".join(rd(os.path.join(diag, r)) for r in reports))
    strict = re.search(r"^\[strict\] FATAL.*$", stderr, re.M)
    how = f" (signal {-rc})" if rc < 0 else ""
    if timed_out:
        how += f" (stopped after {SECS:g} s)"
    print(f"run dir : {os.path.relpath(RUN, ROOT)}")
    print(f"exit    : {rc}{how}")
    print(f"strict  : {strict.group(0) if strict else 'none'}")
    print(f"hle     : {stderr.count('UNRESOLVED NID')} unresolved NID(s) reported")
    print(f"flips   : {flips[-1] if flips else ''}")
    print(f"reports : {' '.join(reports)}")
    print(f"shots   : {len(os.listdir(os.path.join(RUN, 'shots')))}")
    if dr:
        lines = rd(os.path.join(RUN, "drive.log")).splitlines()
        print(f"drive   : {lines[-1] if lines else ''}")
    print("--- last stderr lines")
    print("\n".join(stderr.splitlines()[-25:]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
