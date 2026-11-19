#!/usr/bin/env python3
"""The graphics' start watched, as the owner set it for the Windows review's W-2 (port/main.cpp startup_watch):
Windows allows 120 s and never ends a start while a debugger is attached, Linux 30 s; a start that never finishes
ends with status 70, its log naming the stage it stalled at and a startup-*.txt report written; the watch stops at the
graphics' success and never covers shader preparation.

    python tests/startup_watch_check.py --bin GAME [--install ROOT] [--only CASE ...]

Cases:
  normal    the default limit (120 s on Windows, 30 s on Linux) named in the log's "graphics started" line; the game
            goes on running
  after     a 5 s limit: the game still runs 15 s after it started (the warm-up and the game's own loading come after
            the graphics' start, outside the watch)
  timeout   a test's stall inside the watched start (DOTHEROES_STARTUP_TEST_STALL_S=30) with a 3 s limit: ended with
            status 70 within seconds, the log naming the stall's stage, a startup-*.txt report
  debugger  (Windows) the same stall, with this script attached as the game's debugger (the Win32 debug API): no end
            after the limit, the log saying a debugger is attached; the graphics start once the stall ends
  play70    (with --launcher and --install) `CubicHeroes --play` on the same stall: status 70 passed on, explained on
            stderr and in the JSON ended event's "why"
  prepare   (with --install) the installation's shader preparation, rebuilt, with a 1 s limit: done, exit 0
Every run is offscreen (the desktop guard: tests/desktop_guard.py), with its own save root; the game is ended by this
script once a case has what it needs. Exit 1 if any case fails.
"""
import argparse
import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import desktop_guard  # noqa: E402

WIN = os.name == "nt"
DEFAULT_S = 120 if WIN else 30
ELF = os.path.join(ROOT, "build", "disc", "PS3_GAME", "USRDIR", "EBOOT.elf")
results = []


def case(name, bad, note="", lines=None):
    results.append((name, bad))
    print(("PASS" if not bad else "FAIL") + f": {name}" + (f" ({note})" if note else "") +
          (f" -- {'; '.join(bad)}" if bad else ""), flush=True)
    if bad and lines:   # the game's own last words (its log on the console), so a failure can be read
        for ln in [x for x in lines if "[startup]" in x or "[exit]" in x][-8:] + lines[-25:]:
            print("    | " + ln[:220], flush=True)


def game_env(run, **extra):
    env = dict(os.environ)
    desktop_guard.game_env(env, False, quiet=True)   # offscreen, whatever the caller's environment asks
    # the file system boot_smoke gives the game: its own save root, flash and an empty pad script in the run folder
    # (with the game's file system only half set up, it ends itself at the title)
    gamedata = os.path.join(run, "gamedata")
    for d in (gamedata, os.path.join(run, "dev_flash")):
        os.makedirs(d, exist_ok=True)
    open(os.path.join(run, "pad.txt"), "w").close()
    env.update(DIAG_CRASH_DIR=run, DOTHEROES_LOG_TO_CONSOLE="1", ALSOFT_DRIVERS="null", SDL_AUDIODRIVER="dummy",
               PS3_STRICT="1", PS3_GAMEDATA_ROOT=gamedata, PS3_HDD0_ROOT=os.path.join(gamedata, "dev_hdd0"),
               PS3_HDD1_ROOT=os.path.join(gamedata, "dev_hdd1"), PS3_DEV_FLASH=os.path.join(run, "dev_flash"),
               PS3RECOMP_VK_GUEST_PROGRAMS="1", PS3_SAVEDATA_PICKER="0", PAD_FILE=os.path.join(run, "pad.txt"),
               PS3RECOMP_SPIRV_CACHE="off")
    for k in ("DOTHEROES_STARTUP_TIMEOUT_S", "DOTHEROES_STARTUP_TEST_STALL_S"):
        env.pop(k, None)
    env.update({k: str(v) for k, v in extra.items()})
    return env


class Game:
    """the game started from its own run folder; its output read line by line on a thread"""

    def __init__(self, exe, env, run, debugged=False):
        self.lines, self.q = [], queue.Queue()
        flags = 0x2 if debugged else 0   # DEBUG_ONLY_THIS_PROCESS: this thread is the game's debugger
        self.p = subprocess.Popen([os.path.abspath(exe), ELF], cwd=run, env=env, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, creationflags=flags)
        self.watch = desktop_guard.Watch(self.p, "the game").start()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for raw in iter(self.p.stdout.readline, b""):
            ln = raw.decode("utf-8", "replace").rstrip()
            self.lines.append(ln)
            self.q.put(ln)
        self.q.put(None)

    def wait_line(self, needle, secs, pump=None):
        """the first output line holding needle within secs, or None (the game ended, or time ran out)"""
        end = time.time() + secs
        for ln in self.lines:
            if needle in ln:
                return ln
        while time.time() < end:
            if pump:
                pump()
            try:
                ln = self.q.get(timeout=0.1)
            except queue.Empty:
                continue
            if ln is None:
                return None
            if needle in ln:
                return ln
        return None

    def end(self):
        if self.p.poll() is None:
            self.p.kill()
        try:
            self.p.wait(30)
        except subprocess.TimeoutExpired:
            pass
        self.watch.stop()


class Debugger:
    """the Win32 debug loop for a process started with DEBUG_ONLY_THIS_PROCESS (on the thread that started it): every
    exception passed back to the game (first chance: its own handlers run), the loader's breakpoint continued"""

    def __init__(self, pid):
        import ctypes
        import ctypes.wintypes as wt
        self.ct, self.k32, self.pid, self.gone = ctypes, ctypes.WinDLL("kernel32", use_last_error=True), pid, False

        class U(ctypes.Union):
            _fields_ = [("raw", ctypes.c_ubyte * 160), ("align", ctypes.c_uint64)]

        class EV(ctypes.Structure):
            _fields_ = [("code", wt.DWORD), ("pid", wt.DWORD), ("tid", wt.DWORD), ("u", U)]
        self.ev = EV()

    def pump(self, ms=50):
        if self.gone:
            return
        ct, k32, ev = self.ct, self.k32, self.ev
        while k32.WaitForDebugEvent(ct.byref(ev), ms):
            ms = 0
            cont = 0x00010002   # DBG_CONTINUE
            if ev.code == 1:   # EXCEPTION_DEBUG_EVENT
                exc = int.from_bytes(bytes(ev.u.raw[:4]), "little")
                if exc not in (0x80000003, 0x4000001F):   # not the loader's breakpoint: the game's own
                    cont = 0x80010001   # DBG_EXCEPTION_NOT_HANDLED
            elif ev.code in (3, 6):   # CREATE_PROCESS, LOAD_DLL: their file handle is the debugger's to close
                h = int.from_bytes(bytes(ev.u.raw[:8]), "little")
                if h:
                    k32.CloseHandle(ct.c_void_p(h))
            elif ev.code == 5 and ev.pid == self.pid:   # EXIT_PROCESS
                self.gone = True
            k32.ContinueDebugEvent(ev.pid, ev.tid, cont)
            if self.gone:
                return


def run_case(name, exe, fn):
    run = tempfile.mkdtemp(prefix=f"startup-{name}-", dir=os.path.join(ROOT, "build", "tmp")
                           if os.path.isdir(os.path.join(ROOT, "build", "tmp")) else None)
    try:
        fn(exe, run)
    except Exception as e:   # a case that breaks is a failure, the others still run
        case(name, [f"{type(e).__name__}: {e}"])
    finally:
        shutil.rmtree(run, ignore_errors=True)


def c_normal(exe, run):
    g = Game(exe, game_env(run), run)
    try:
        ln = g.wait_line("[startup] the graphics started", 120)
        bad = []
        if not ln:
            bad.append("no 'graphics started' line" + (f" (ended with {g.p.poll()})" if g.p.poll() is not None else ""))
        elif f"(the start-up watch: {DEFAULT_S} s)" not in ln:
            bad.append(f"the default limit is not {DEFAULT_S} s: {ln}")
        time.sleep(3)
        if g.p.poll() is not None:
            bad.append(f"the game ended (status {g.p.poll()})")
        if g.watch.violation:
            bad.append(g.watch.violation)
        case("normal", bad, ln.split("[startup] ")[-1] if ln else "", g.lines)
    finally:
        g.end()


def c_after(exe, run):
    g = Game(exe, game_env(run, DOTHEROES_STARTUP_TIMEOUT_S=5), run)
    try:
        ln = g.wait_line("[startup] the graphics started", 60)
        bad = [] if ln else ["no 'graphics started' line" + (f" (status {g.p.poll()})" if g.p.poll() is not None else "")]
        if ln:
            time.sleep(15)
            if g.p.poll() is not None:
                bad.append(f"the game ended {15} s after its graphics started (status {g.p.poll()})")
            if any("did not finish starting" in x for x in g.lines):
                bad.append("the watch fired after the graphics had started")
        case("after", bad, "running 15 s past a 5 s limit" if not bad else "", g.lines)
    finally:
        g.end()


def c_timeout(exe, run):
    t0 = time.time()
    g = Game(exe, game_env(run, DOTHEROES_STARTUP_TIMEOUT_S=3, DOTHEROES_STARTUP_TEST_STALL_S=30), run)
    try:
        g.p.wait(60)
    except subprocess.TimeoutExpired:
        pass
    secs = time.time() - t0
    g.wait_line("\0never", 1)
    status = g.p.poll()
    g.end()
    log = "\n".join(g.lines)
    reports = [f for f in os.listdir(run) if f.startswith("startup-")]
    bad = []
    if status != 70:
        bad.append(f"status {status}, not 70")
    if secs > 25:
        bad.append(f"ended after {secs:.0f} s (the limit 3 s)")
    if "stalled at a test's stall" not in log:
        bad.append("the log does not name the stage it stalled at")
    if not reports:
        bad.append("no startup-*.txt report")
    elif "stalled at" not in open(os.path.join(run, reports[0]), encoding="utf-8", errors="replace").read():
        bad.append("the report does not name the stage")
    case("timeout", bad, f"status {status} after {secs:.1f} s, report {reports}", g.lines)


def c_debugger(exe, run):
    if not WIN:
        case("debugger", [], "not applicable: the debugger exemption is Windows' (Linux keeps 30 s)")
        return
    g = Game(exe, game_env(run, DOTHEROES_STARTUP_TIMEOUT_S=3, DOTHEROES_STARTUP_TEST_STALL_S=12), run, debugged=True)
    dbg = Debugger(g.p.pid)
    try:
        # a debugged start is slow (every exception the game raises passes through this loop first): the graphics'
        # start waited for, then the whole output judged
        t0 = time.time()
        started = g.wait_line("[startup] the graphics started", 240, pump=dbg.pump)
        end = time.time() + 3
        while time.time() < end:
            dbg.pump(100)
        told = next((x for x in g.lines if "a debugger is attached" in x), None)
        bad = []
        if not told:
            bad.append(f"no 'a debugger is attached' line ({time.time() - t0:.0f} s)")
        elif started and g.lines.index(told) > g.lines.index(started):
            bad.append("the debugger line came after the graphics' start")
        if not started:
            bad.append("the graphics did not start after the stall")
        if g.p.poll() == 70 or any("did not finish starting" in x for x in g.lines):
            bad.append("ended by the watch (status 70) with a debugger attached")
        case("debugger", bad, "the limit passed under a debugger; started after the stall" if not bad else "", g.lines)
    finally:
        g.p.kill()
        end = time.time() + 15
        while not dbg.gone and time.time() < end:
            dbg.pump(100)
        g.end()


def c_play70(launcher, install):
    """CubicHeroes --play on a start that never finishes: status 70 passed on, explained on stderr and in the JSON
    event's "why" (launcher/game_exit.h)"""
    exe = os.path.join(launcher, "CubicHeroes.exe" if WIN else "CubicHeroes")
    run = tempfile.mkdtemp(prefix="startup-play70-")
    try:
        env = game_env(run, DOTHEROES_STARTUP_TIMEOUT_S=3, DOTHEROES_STARTUP_TEST_STALL_S=30)
        for k in ("PS3_GAMEDATA_ROOT", "DIAG_CRASH_DIR"):   # the installation's own (logs/crash), as a player's
            env.pop(k, None)
        t0 = time.time()
        p = subprocess.run([exe, "--play", "--json", "--install-dir", install], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", env=env, timeout=180)
        secs = time.time() - t0
        ended = [json.loads(ln) for ln in p.stdout.splitlines() if ln.startswith("{") and '"ended"' in ln]
        bad = []
        if p.returncode != 70:
            bad.append(f"exit {p.returncode}, not 70: {p.stdout[-300:]} {p.stderr[-300:]}")
        if "graphics didn't start" not in p.stderr:
            bad.append("no explanation on stderr")
        if not ended or ended[-1].get("exit") != 70 or "graphics didn't start" not in ended[-1].get("why", ""):
            bad.append(f"the JSON ended event: {ended}")
        case("play70", bad, f"--play ended with {p.returncode} after {secs:.1f} s")
    finally:
        shutil.rmtree(run, ignore_errors=True)


def c_prepare(install):
    """the installation's preparation (the game's --prepare, through the worker) with a 1 s start-up limit"""
    worker = os.path.join(install, "worker", "DotHeroes-Setup.exe" if WIN else "DotHeroes-Setup")
    cmd = [worker, "--worker"] if os.path.exists(worker) else [sys.executable, os.path.join(ROOT, "tools", "worker.py")]
    env = dict(os.environ, DOTHEROES_STARTUP_TIMEOUT_S="1")
    desktop_guard.no_display(env)
    t0 = time.time()
    p = subprocess.run(cmd + ["prepare", "--install", install, "--rebuild"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", env=env, timeout=1800)
    secs = time.time() - t0
    res = {}
    for ln in p.stdout.splitlines():
        try:
            e = json.loads(ln)
        except ValueError:
            continue
        if e.get("ev") == "result":
            res = e
    d = res.get("data", {})
    bad = [] if p.returncode == 0 and res.get("ok") and not d.get("failed") else [f"exit {p.returncode}: {str(res)[:300]}"]
    if secs < 1.5:
        bad.append(f"done in {secs:.1f} s: not long enough to show the 1 s limit is not applied")
    case("prepare", bad, f"{d.get('pipelines', '?')} pipelines in {secs:.1f} s under a 1 s start-up limit")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bin", required=True)
    ap.add_argument("--install")
    ap.add_argument("--launcher", help="a folder with CubicHeroes: the play70 case (with --install)")
    ap.add_argument("--elf", help="the decrypted EBOOT.elf (default: this checkout's build/disc copy)")
    ap.add_argument("--only", nargs="*")
    a = ap.parse_args()
    global ELF
    ELF = os.path.abspath(a.elf) if a.elf else ELF
    want = set(a.only or ["normal", "after", "timeout", "debugger", "play70", "prepare"])
    for name, fn in (("normal", c_normal), ("after", c_after), ("timeout", c_timeout), ("debugger", c_debugger)):
        if name in want:
            run_case(name, a.bin, fn)
    if "play70" in want:
        if a.launcher and a.install:
            try:
                c_play70(os.path.abspath(a.launcher), os.path.abspath(a.install))
            except Exception as e:
                case("play70", [f"{type(e).__name__}: {e}"])
        else:
            print("  (play70: --launcher and --install needed, not run)")
    if "prepare" in want:
        if a.install:
            try:
                c_prepare(os.path.abspath(a.install))
            except Exception as e:
                case("prepare", [f"{type(e).__name__}: {e}"])
        else:
            print("  (prepare: no --install given, not run)")
    failed = [n for n, b in results if b]
    print(f"{len(results) - len(failed)} of {len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
