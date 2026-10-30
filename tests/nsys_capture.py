#!/usr/bin/env python3
"""Nsight Systems capture of the game in play (Windows, run as Administrator:
CPU sampling and context switches need it; the Vulkan trace does not).

  python tests/nsys_capture.py --saves <savedata dir> [--delay 80] [--secs 15]

Runs tests/boot_smoke.py with tests/scripts/load_save.drive (the save is copied
into the run, never touched) under `nsys profile`: collection starts --delay
seconds after launch, once the drive has reached the overworld, and lasts
--secs seconds, then nsys ends the game. The report lands in the run directory
as nsys.nsys-rep; `nsys stats` reads it without elevation.
"""
import argparse
import ctypes
import glob
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--saves", required=True, help="a savedata folder holding this game's saves")
ap.add_argument("--delay", type=int, default=80)
ap.add_argument("--secs", type=int, default=15)
a = ap.parse_args()

nsys = sorted(glob.glob(r"C:\Program Files\NVIDIA Corporation\Nsight Systems *\target-windows-x64\nsys.exe"))
if not nsys:
    sys.exit("Nsight Systems not found under C:\\Program Files\\NVIDIA Corporation")
admin = bool(ctypes.windll.shell32.IsUserAnAdmin())
print(f"nsys: {nsys[-1]}\nadministrator: {admin}" + ("" if admin else " (CPU sampling and context switches off)"))

wrap = [nsys[-1], "profile", "--output", "nsys", "--force-overwrite", "true",
        "--trace", "vulkan,nvtx", "--vulkan-gpu-workload", "true",
        "--delay", str(a.delay), "--duration", str(a.secs)]
wrap += ["--sample", "process-tree", "--cpuctxsw", "process-tree", "--backtrace", "auto"] if admin else \
        ["--sample", "none", "--cpuctxsw", "none"]
env = dict(os.environ, SAVES=a.saves, DRIVE=os.path.join(ROOT, "tests", "scripts", "load_save.drive"),
           RUN_TAG="nsys", KEEP_RUNS=os.environ.get("KEEP_RUNS", "8"), SHOT_EVERY="0",
           DIAG_FPS="1", VK_GPU_TIME="1", WRAP=json.dumps(wrap))
sys.exit(subprocess.call([sys.executable, os.path.join(ROOT, "tests", "boot_smoke.py"),
                          str(a.delay + a.secs + 240)], cwd=ROOT, env=env))
