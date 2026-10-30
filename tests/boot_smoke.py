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
                     dump (frame.ppm) is deleted when a run ends.
  PS3RECOMP_VK_DUMP_EVERY=<n>  refresh frame.ppm every n-th present (default
                     15; dumping every frame costs several fps).
  DUMP=0             no frame.ppm at all, for timing runs: the dump is written
                     on the present path and skews it. No screenshots then,
                     and a drive script may not use `change` or `shot`.
  DRIVE=<script>     run tests/drive.py with this script against the game
                     (closed-loop input; see tests/scripts/).
  SAVES=<dir>        seed the run's save data with a copy of this game's save
                     folders (BLUS30490*) found in <dir>, e.g. an emulator's
                     dev_hdd0/home/00000001/savedata. <dir> is only read.
  WINDOW=1           show the game's window (PS3RECOMP_VK_WINDOW=1). By default
                     nothing appears on the desktop: rendering is offscreen.
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


def rd(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def main():
    if not os.path.isfile(BIN):
        print(f"no {BIN} (build port/ first)")
        return 2
    # Retention: test runs (logs, screenshots) must not pile up on disk.
    keep = int(os.environ.get("KEEP_RUNS", "5"))
    old = sorted(glob.glob(os.path.join(ROOT, "build", "test", "boot-*")))
    for d in old[:max(0, len(old) - max(keep - 1, 0))]:
        shutil.rmtree(d, ignore_errors=True)
    gamedata = os.path.join(RUN, "gamedata")
    for d in ("dev_flash", "shots", gamedata):
        os.makedirs(os.path.join(RUN, d), exist_ok=True)
    open(os.path.join(RUN, "pad.txt"), "w").close()
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
        "SDL_AUDIODRIVER": "dummy",
        "DIAG_CRASH_DIR": os.path.join(RUN, "diag"),
        "DIAG_HANG_EXIT": "1",
        "DIAG_BOOT_SECS": env.get("DIAG_BOOT_SECS", "60"),
    })
    if window:
        env["PS3RECOMP_VK_WINDOW"] = "1"
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
