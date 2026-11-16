#!/usr/bin/env python3
"""The map sweep (M9.2g, docs/M92_SHADER_PLAN.md section 2h): every map of the game visited headlessly through the
game's own map change -- the build's DOTHEROES_MAP_SWEEP (port/native/render/ppfx_recorder.cpp: the game's native
Event:SetMap called on its own thread at a frame's flip) on tests/scenarios/map_sweep.drive -- offscreen (the desktop
guard: tests/boot_smoke.py), no sound device, the pipelines each map builds logged with the compile log's clock.

    tests/map_sweep.py run EVIDENCE [--bin EXE] [--shaders DIR] [--seconds 15] [--chunk 28] [--first N --last M]
    tests/map_sweep.py report RUN_DIR [...]

`run`: the list of maps is made from the development data (data/map/pac_data, each map's entry point from its
alignment file: "-gDebug" where the map has the debug entry, else its first named group) -- it stays local, under
EVIDENCE; the maps are swept in chunks of --chunk, each its own game process (a fresh start: one map that ends the
game ends only its chunk), over a copy of --shaders (a preparation's shader folder), the warm-up off so every
pipeline met is built and logged prepared or unforeseen. `report`: per map, the pipelines first met there, how many
were prepared, and the translations compiled in play.
"""
import argparse
import collections
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "build", "disc", "PS3_GAME", "USRDIR", "data")


def map_list(secs, first=None, last=None):
    pac = os.path.join(DATA, "map", "pac_data")
    ali = os.path.join(DATA, "map", "map_alignment")
    maps = sorted(f[:-len(".ps3.pac.gz")] for f in os.listdir(pac) if f.endswith(".ps3.pac.gz"))[first:last]
    out = []
    for m in maps:
        gs = []
        p = os.path.join(ali, m + ".dat")
        if os.path.exists(p):
            with open(p, "rb") as f:
                gs = list(dict.fromkeys(x.decode() for x in re.findall(rb"-g[A-Za-z0-9_]{1,30}", f.read())))
        if "-gDebugExit" in gs:
            e = "-gDebug"
        else:
            named = [g for g in gs if not g.endswith("Exit") and not g[2:].isdigit()]
            e = named[0] if named else (gs[0] if gs else "-gDebug")
        out.append(f"{m} {e} {secs:g}")
    return out


def run(a):
    os.makedirs(a.evidence, exist_ok=True)
    maps = map_list(a.seconds, a.first, a.last)
    for c in range(0, len(maps), a.chunk):
        part = maps[c:c + a.chunk]
        r = os.path.join(a.evidence, f"chunk{c // a.chunk}")
        if os.path.exists(os.path.join(r, "done")):
            continue
        shutil.rmtree(r, ignore_errors=True)
        os.makedirs(r)
        lst = os.path.join(r, "maps.txt")
        with open(lst, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(part) + "\n")
        if a.shaders:
            shutil.copytree(a.shaders, os.path.join(r, "shaders"))
        os.makedirs(os.path.join(r, "nv"))
        env = dict(os.environ)
        for k in ("PS3RECOMP_VK_WINDOW", "WINDOW", "VISIBLE", "TOPMOST", "INSTALLED"):
            env.pop(k, None)
        tag = f"map-sweep-{os.path.basename(os.path.normpath(a.evidence))}-{c // a.chunk}"
        env.update({
            "BIN": a.bin, "PS3RECOMP_SPIRV_CACHE": os.path.join(r, "shaders"),
            "DOTHEROES_RECIPES": os.path.join(r, "recipes.bin"), "__GL_SHADER_DISK_CACHE_PATH": os.path.join(r, "nv"),
            "DOTHEROES_MAP_SWEEP": lst, "DOTHEROES_MAP_SWEEP_START": "60", "DOTHEROES_WARM": "0",
            "VK_COMPILE_LOG": "1", "DIAG_FRAMES": "frames.log", "ALSOFT_DRIVERS": "null", "SCENARIO": "map_sweep",
            "DUMP": "0", "RUN_TAG": tag, "KEEP_RUNS": "80",
        })
        secs = 70 + int(len(part) * a.seconds) + 20
        t0 = time.monotonic()
        p = subprocess.run([sys.executable, os.path.join(ROOT, "tests", "boot_smoke.py"), str(secs)], cwd=ROOT, env=env,
                           capture_output=True, text=True, timeout=secs + 1200)
        runs = sorted(d for d in os.listdir(os.path.join(ROOT, "build", "test")) if d.endswith(tag))
        if runs:
            shutil.move(os.path.join(ROOT, "build", "test", runs[-1]), os.path.join(r, "run"))
        shutil.rmtree(os.path.join(r, "shaders"), ignore_errors=True)
        shutil.rmtree(os.path.join(r, "nv"), ignore_errors=True)
        open(os.path.join(r, "done"), "w").close()
        print(f"chunk {c // a.chunk} ({len(part)} maps): exit {p.returncode}, {time.monotonic() - t0:.0f} s", flush=True)
    return 0


def report(dirs):
    tot = collections.Counter()
    for run_dir in dirs:
        log = os.path.join(run_dir, "run", "stderr.log")
        if not os.path.exists(log):
            continue
        cur = None
        per = collections.OrderedDict()
        for ln in open(log, encoding="utf-8", errors="replace"):
            m = re.search(r"\[sweep\] t=[\d.]+ map \d+/\d+ (\S+)", ln)
            if m:
                cur = m.group(1)
                per[cur] = collections.Counter()
                continue
            if cur is None:
                continue
            m = re.search(r"\[vk-compile\] t=[\d.]+ pipeline \w+ key [0-9A-F]+ total ([\d.]+) ms .* (prepared|unforeseen)", ln)
            if m:
                per[cur]["met"] += 1
                per[cur][m.group(2)] += 1
                if m.group(2) == "unforeseen":
                    per[cur]["ms"] += float(m.group(1))
            elif re.search(r"\[vk-compile\] t=[\d.]+ translate \S+ \d+ words [\d.]+ ms glslang", ln):
                per[cur]["translated"] += 1
        for name, c in per.items():
            print(f"{os.path.basename(run_dir):10s} {name:12s} met {c['met']:3d} prepared {c['prepared']:3d} unforeseen "
                  f"{c['unforeseen']:3d} ({c['ms']:6.1f} ms) translated {c['translated']}")
            for k in ("met", "prepared", "unforeseen", "translated"):
                tot[k] += c[k]
            tot["ms"] += c["ms"]
            tot["maps"] += 1
            tot["maps with unforeseen"] += 1 if c["unforeseen"] else 0
    print(f"{tot['maps']} maps: {tot['met']} pipelines first met after the start, {tot['prepared']} prepared, "
          f"{tot['unforeseen']} unforeseen ({tot['ms']:.1f} ms, in {tot['maps with unforeseen']} maps); "
          f"{tot['translated']} translations compiled in play")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("evidence")
    r.add_argument("--bin", default=os.path.join("build", "port-win", "dotheroes.exe"))
    r.add_argument("--shaders")
    r.add_argument("--seconds", type=float, default=15.0)
    r.add_argument("--chunk", type=int, default=28)
    r.add_argument("--first", type=int)
    r.add_argument("--last", type=int)
    p = sub.add_parser("report")
    p.add_argument("dirs", nargs="+")
    a = ap.parse_args()
    return run(a) if a.cmd == "run" else report(a.dirs)


if __name__ == "__main__":
    sys.exit(main())
