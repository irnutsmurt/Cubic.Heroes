#!/usr/bin/env python3
"""The map sweep (M9.2g, docs/M92_SHADER_PLAN.md section 2h; since M11.0b through the developer tools, docs/M11_PLAN.md):
every map of the game visited headlessly through the game's own map change -- in developer mode, each map a `warp`
(port/devtools: Event:SetMap run in the game's own Lua state on its main thread, answered once the game has arrived)
from a generated drive -- offscreen (the desktop guard: tests/boot_smoke.py), no sound device, the pipelines each map
builds logged with the compile log's clock.

    tests/map_sweep.py run EVIDENCE [--bin EXE] [--shaders DIR] [--seconds 15] [--chunk 28] [--first N --last M]
                           [--only MAP,MAP...]
    tests/map_sweep.py report RUN_DIR [...]
    tests/map_sweep.py check EVIDENCE [--baseline FILE] [--write-baseline FILE] [--no-prepared]

`check` (the regression test; the owner, 2026-11-16: the sweep is part of the permanent regression tests): passes
only when every chunk's game ran to its planned end and exited cleanly; every map of every chunk was changed to and
*arrived* -- "[warp] ... arrived MAP area A frames F" (M9.2g's builds: "[sweep] ... arrived"): the guest's frames
advanced in the map, and, against a baseline (a TSV "map area" from an accepted run: --write-baseline), the area is
the one that map reached then, and not the area of the map before it (each map has its own number in the game's map
list: the same number means the change did not happen); and, with a preparation given (--shaders), no shader
translation was compiled in play. The pipelines met unprepared are reported, not failed: new content legitimately
meets new state.

`run`: the list of maps is made from the development data (data/map/pac_data; each map's arrival point from its
alignment file's arrival records, "-gDebug" where the map has one) -- it stays local, under EVIDENCE; the maps are
swept in chunks of --chunk, each its own game process (a fresh start: one map that ends the game ends only its chunk),
over a copy of --shaders (a preparation's shader folder), the warm-up off so every pipeline met is built and logged
prepared or unforeseen. `report`: per map, the pipelines first met there, how many were prepared, and the
translations compiled in play.
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


# a map file the game's map list lacks: the map change cannot reach it (it reloads the current map) [M: M11's first
# full sweep with the arrival check; C: no entry in the game's map list]. Field_99 is the character editor's world
# (the executable's map table pairs the editor with its alignment; the editor's runs load it): reached through the
# title's editor, not the map change (docs/M11_PLAN.md section 16)
NOT_IN_MAP_LIST = {"Field_99"}

# OpenAL Soft's wave writer in place of a device (`run --audio`): the whole mix into the run's folder, silent
ALSOFT_WAVE = """[general]
drivers = wave
frequency = 48000
sample-type = float32
channels = stereo

[wave]
file = mix.wav
"""
SILENT_DB = -60.0   # a map whose mix stays below this after its first 2 s is silent


def audio_per_map(run_dir, arrivals):
    """M11.1b: for each arrival (map, guest frame), the music the game started in its stay (the sound manager's
    playBGM ids, port/audio_hooks.cpp's trace) and the mix's level after its first 2 s (dBFS); {} with no trace"""
    import array
    import math
    trace, mixf = os.path.join(run_dir, "audio_trace.txt"), os.path.join(run_dir, "mix.wav")
    if not os.path.exists(trace) or not os.path.exists(mixf):
        return {}
    marks, bgm = [], []   # (frame, us) from every line; (frame, id) of each playBGM
    with open(trace, encoding="utf-8", errors="replace") as f:
        for ln in f:
            p = ln.split()
            if len(p) < 4 or not p[0].isdigit():
                continue
            us, fr = int(p[0]), int(p[1])
            if not marks or fr != marks[-1][0]:
                marks.append((fr, us))
            if p[3] == "SM.playBGM" and len(p) > 4:
                bgm.append((fr, p[4]))
    with open(mixf, "rb") as f:
        data = f.read()
    at = data.find(b"data")
    samples = array.array("f")
    samples.frombytes(data[at + 8:at + 8 + (len(data) - at - 8) // 8 * 8] if at >= 0 else b"")

    def us_at(frame):
        for fr, us in marks:
            if fr >= frame:
                return us
        return marks[-1][1] if marks else 0
    out = {}
    for i, (m, frame) in enumerate(arrivals):
        end_frame = arrivals[i + 1][1] if i + 1 < len(arrivals) else None
        ids = sorted({b for fr, b in bgm if fr >= frame and (end_frame is None or fr < end_frame)})
        t0 = us_at(frame) / 1e6 + 2.0
        t1 = (us_at(end_frame) / 1e6 - 3.0) if end_frame else t0 + 10.0   # before the next change's fade
        i0, i1 = max(0, int(t0 * 48000) * 2), min(len(samples), int(t1 * 48000) * 2)
        seg = samples[i0:i1] if i1 > i0 else array.array("f")
        ms = sum(x * x for x in seg) / len(seg) if seg else 0.0
        out[m] = (ids, 10 * math.log10(ms) if ms > 0 else -200.0)
    return out

# draws the renderer leaves out that are known and tracked (docs/M11_VALIDATION.md): listed, not failed; any other
# reason fails the check
KNOWN_LEFT_OUT = [
    # (B-006, a draw whose uploads no longer fit its batch, is fixed: such a draw is recorded again in the next one)
    (r"^unit 0 at 0:0[0-9A-F]{7} \(format A5 1280x720 pitch 5120\) has no native source$", "B-007"),
]


def map_list(secs, first=None, last=None):
    pac = os.path.join(DATA, "map", "pac_data")
    ali = os.path.join(DATA, "map", "map_alignment")
    maps = sorted(f[:-len(".ps3.pac.gz")] for f in os.listdir(pac) if f.endswith(".ps3.pac.gz")
                  and f[:-len(".ps3.pac.gz")] not in NOT_IN_MAP_LIST)[first:last]
    out = []
    for m in maps:
        es = arrivals(os.path.join(ali, m + ".dat"))
        out.append(f"{engine_name(m)} {es[0] if es else '-gDebug'} {secs:g}")
    return out


def arrivals(path):
    """a map's arrival points, the default first (as port/devtools/devtools.cpp map_entries reads them): its alignment
    file's arrival records -- u32 type 0x1005, u32 0, u32 name length, u32 kind length, the name (what the map change
    takes: -gDebug, -gfrom_d_1, a door's -g212;-o212;), the kind (Exit, WarpDoor, ...), little-endian -- the
    developers' -gDebug first where the map has one"""
    import struct
    try:
        with open(path, "rb") as f:
            d = f.read()
    except OSError:
        return []
    out = []
    at = d.find(b"\x05\x10\x00\x00\x00\x00\x00\x00")
    while at >= 0 and at + 16 < len(d):
        la, lb = struct.unpack_from("<II", d, at + 8)
        if 3 <= la <= 64 and 0 < lb <= 32 and at + 16 + la + lb <= len(d):
            name = d[at + 16:at + 16 + la]
            if name.startswith(b"-g") and all(0x20 < c < 0x7F for c in name) and name.decode() not in out:
                out.append(name.decode())
        at = d.find(b"\x05\x10\x00\x00\x00\x00\x00\x00", at + 1)
    if "-gDebug" in out:
        out.remove("-gDebug")
        out.insert(0, "-gDebug")
    return out


def engine_name(stem):
    """the name the game's map change takes for a map file: the first nine fields' files are Field_01..Field_09, but
    the engine (its links between maps, MapParameter) calls them Field_1..Field_9 -- SetMap with the file's name
    reloads the current map (M11's arrival check found it: the M9.2g sweep never reached those nine)"""
    m = re.fullmatch(r"Field_0([1-9])", stem)
    return f"Field_{m.group(1)}" if m else stem


# the sweep's drive (per chunk; the warps appended): CONTINUE from the kept checkpoint castle-front, then each map in
# turn through `warp` -- `devtry`: a map that does not arrive is logged and the chunk goes on (the check fails it)
SWEEP_DRIVE_HEAD = """# generated by tests/map_sweep.py (M11.0b): the map sweep through the developer tools' warp
#! saves build/checkpoints/castle-front
#! env DOTHEROES_DEVELOPER_MODE=1 DOTHEROES_NATIVE_CD=1 DOTHEROES_NATIVE_SOLVER=1,2,4,5,6 GCM_WRAP_LAP=1
#! env DOTHEROES_PRESENT_ONCE=1 DOTHEROES_DEV_STATE=1 PS3_VERBOSE=0 DOTHEROES_DEV_CANT_DIE=1 SHOT_EVERY=0
include {common}

log TitleMovie 120
sleep 5
call continue
state life>0 60
sleep 20
"""


def run(a):
    os.makedirs(a.evidence, exist_ok=True)
    maps = map_list(a.seconds, a.first, a.last)
    if a.only:
        want = set(a.only.split(","))
        maps = [ln for ln in maps if ln.split()[0] in want]
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
        # M11.0b: each map through the developer tools' `warp` (the game's own Event:SetMap, answered on arrival)
        drive = os.path.join(r, "sweep.drive")
        common = os.path.join(ROOT, "tests", "scenarios", "common.drive").replace(os.sep, "/")
        with open(drive, "w", encoding="utf-8", newline="\n") as f:
            f.write(SWEEP_DRIVE_HEAD.format(common=common))
            for ln in part:
                m, e, secs = ln.split()
                f.write(f"devtry warp {m} {e} @150\nsleep {float(secs):g}\n")
        env = dict(os.environ)
        for k in ("PS3RECOMP_VK_WINDOW", "WINDOW", "VISIBLE", "TOPMOST", "INSTALLED"):
            env.pop(k, None)
        tag = f"map-sweep-{os.path.basename(os.path.normpath(a.evidence))}-{c // a.chunk}"
        env.update({
            "BIN": a.bin, "PS3RECOMP_SPIRV_CACHE": os.path.join(r, "shaders"),
            "DOTHEROES_RECIPES": os.path.join(r, "recipes.bin"), "__GL_SHADER_DISK_CACHE_PATH": os.path.join(r, "nv"),
            "DOTHEROES_WARM": "0", "VK_COMPILE_LOG": "1", "DIAG_FRAMES": "frames.log", "ALSOFT_DRIVERS": "null",
            "DRIVE": drive, "DUMP": "0", "RUN_TAG": tag, "KEEP_RUNS": "80",
        })
        if a.audio:   # M11.1b: the game's audio calls traced and the mix written by OpenAL Soft's wave writer (silent)
            ini = os.path.join(r, "alsoft_wave.ini")
            with open(ini, "w", encoding="utf-8", newline="") as f:
                f.write(ALSOFT_WAVE)
            env.pop("ALSOFT_DRIVERS", None)
            env.update({"ALSOFT_CONF": ini, "DOTHEROES_AUDIO_TRACE": "audio_trace.txt"})
        secs = 120 + int(len(part) * (a.seconds + 15)) + 60
        t0 = time.monotonic()
        p = subprocess.run([sys.executable, os.path.join(ROOT, "tests", "boot_smoke.py"), str(secs)], cwd=ROOT, env=env,
                           capture_output=True, text=True, timeout=secs + 1200)
        runs = sorted(d for d in os.listdir(os.path.join(ROOT, "build", "test")) if d.endswith(tag))
        if runs:
            shutil.move(os.path.join(ROOT, "build", "test", runs[-1]), os.path.join(r, "run"))
        shutil.rmtree(os.path.join(r, "shaders"), ignore_errors=True)
        shutil.rmtree(os.path.join(r, "nv"), ignore_errors=True)
        print(f"chunk {c // a.chunk} ({len(part)} maps): exit {p.returncode}, {time.monotonic() - t0:.0f} s", flush=True)
        if not runs:   # the harness refused to start: its words, and the chunk left to run again
            print((p.stdout + p.stderr)[-1500:], flush=True)
            continue
        open(os.path.join(r, "done"), "w").close()
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
            m = re.search(r"\[(?:sweep|warp)\] t=[\d.]+ map (?:\d+/\d+ )?(\S+)", ln)
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


def chunk_dirs(evidence):
    return sorted((os.path.join(evidence, d) for d in os.listdir(evidence) if re.fullmatch(r"chunk\d+", d)),
                  key=lambda p: int(re.search(r"\d+$", p).group()))


def check(a):
    """0 when the sweep passes; 1 with each failure listed"""
    import json
    fails, rows = [], []
    base = {}
    if a.baseline:
        with open(a.baseline, encoding="utf-8") as f:
            for ln in f:
                t = ln.split()
                if len(t) >= 2 and not ln.startswith("#"):
                    base[t[0]] = t[1:]
    chunks = chunk_dirs(a.evidence)
    if not chunks:
        fails.append(f"no chunk under {a.evidence}")
    unforeseen = translated = planned_total = 0
    kinds = {}   # M11.1b: the texture kinds B-002/B-004 changed ([vk-texture]): map -> {kind}
    audio = {}   # M11.1b (a sweep run with --audio): map -> (music ids started, the mix's level)
    for r in chunks:
        name = os.path.basename(r)
        with open(os.path.join(r, "maps.txt"), encoding="utf-8") as f:
            planned = [ln.split() for ln in f if ln.strip()]
        planned_total += len(planned)
        log = os.path.join(r, "run", "stderr.log")
        res = os.path.join(r, "run", "result.json")
        if not os.path.exists(log) or not os.path.exists(res):
            fails.append(f"{name}: no run (stderr.log or result.json missing)")
            continue
        with open(res, encoding="utf-8") as f:
            result = json.load(f)
        if result.get("exit") != 0 or not str(result.get("ended_by", "")).startswith(("time", "drive done")):
            fails.append(f"{name}: the game did not run to its planned end (ended by {result.get('ended_by')!r}, "
                         f"exit {result.get('exit')})")
        changed, arrived, swept = set(), {}, False
        current = None
        with open(log, encoding="utf-8", errors="replace") as f:
            log_lines = f.readlines()
        for ln in log_lines:
            m = re.search(r"\[vk-texture\] t=[\d.]+ (\S+) (\d+x\d+) levels (\d+)", ln)
            if m:
                kinds.setdefault(current or "(start)", set()).add(f"{m.group(1)} {m.group(2)}/{m.group(3)}")
                continue
            m = re.search(r"\[(?:sweep|warp)\] t=[\d.]+ map (?:\d+/\d+ )?(\S+)", ln)
            if m:
                changed.add(m.group(1))
                swept = True
                continue
            m = re.search(r"\[(?:sweep|warp)\] t=[\d.]+ arrived (?:\d+/\d+ )?(\S+) area (\d+) frames (\d+)", ln)
            if m:
                arrived[m.group(1)] = (int(m.group(2)), int(m.group(3)))
                current = m.group(1)
                continue
            if not swept:
                continue
            m = re.search(r"\[vk-compile\] t=[\d.]+ pipeline \w+ key [0-9A-F]+ total ([\d.]+) ms .* (prepared|unforeseen)", ln)
            if m:
                unforeseen += m.group(2) == "unforeseen"
            elif re.search(r"\[vk-compile\] t=[\d.]+ translate \S+ \d+ words [\d.]+ ms glslang", ln):
                translated += 1
        # M11.1b: draws the renderer left out (its report): none but the first frame's records without a clip
        try:
            with open(os.path.join(r, "run", "ppfx_report.txt"), encoding="utf-8", errors="replace") as f:
                for ln in f:
                    m = re.match(r"not rendered: (\d+) (.*)", ln)
                    if not m or m.group(2).strip() == "asp: no clip":
                        continue
                    why = m.group(2).strip()
                    bug = next((b for rx, b in KNOWN_LEFT_OUT if re.search(rx, why)), None)
                    if bug:
                        print(f"{name}: {m.group(1)} draw(s) not rendered, known ({bug}): {why}")
                    else:
                        fails.append(f"{name}: {m.group(1)} draws not rendered: {why}")
        except OSError:
            pass   # no report: nothing to read (a run that did not end cleanly fails above)
        # the frame counter at each arrival: the previous map's arrival (or the first change) to this one
        prev_frames = prev_area = None
        for i, (m, entry, secs) in enumerate(planned):
            if m not in changed:
                fails.append(f"{name}: {m} never changed to")
                continue
            if m not in arrived:
                fails.append(f"{name}: {m} changed to, no arrival logged (the build lacks the arrival line?)")
                continue
            area, frames = arrived[m]
            ran = None if prev_frames is None else frames - prev_frames
            prev_frames = frames
            same = prev_area is not None and area == prev_area
            prev_area = area
            ok_frames = ran is None or ran >= float(secs) * 10   # the guest's frames: 30 or 60 a second in play
            note = ""
            if not ok_frames:
                fails.append(f"{name}: {m}: the game hardly ran there ({ran} guest frames in {secs} s)")
                note = "STALLED"
            if same:   # every map has its own number in the game's map list: the change did not happen
                fails.append(f"{name}: {m} arrived in the area of the map before it ({area}): not reached")
                note = "SAME"
            if m in base and base[m][0] != str(area):
                fails.append(f"{name}: {m} arrived in area {area}, the baseline's is {base[m][0]}")
                note = "AREA"
            rows.append((m, entry, area, ran, note))
        order = sorted(((m, arrived[m][1]) for m in arrived), key=lambda x: x[1])
        for m, (ids, db) in audio_per_map(os.path.join(r, "run"), order).items():
            audio[m] = (ids, db)
            if db < SILENT_DB:
                fails.append(f"{name}: {m}: the mix silent there ({db:.0f} dBFS)")
    if a.write_baseline:
        with open(a.write_baseline, "w", encoding="utf-8", newline="\n") as f:
            f.write("# map area (tests/map_sweep.py check --write-baseline): where each map arrived in an accepted sweep\n")
            for m, entry, area, ran, note in rows:
                f.write(f"{m} {area}\n")
        print(f"baseline: {len(rows)} maps -> {a.write_baseline}")
    if a.shaders_prepared and translated:
        fails.append(f"{translated} shader translation(s) compiled in play (a preparation was given)")
    print(f"{len(rows)} maps arrived of {planned_total} planned; {unforeseen} pipelines met unprepared; {translated} "
          f"translations compiled in play")
    if audio:
        print(f"audio on {len(audio)} maps (the music the game started there; the mix's level after 2 s):")
        for m in sorted(audio):
            print(f"  {m}: music {','.join(audio[m][0]) or '(kept)'}; {audio[m][1]:.1f} dBFS")
    if kinds:   # where cube maps and linear mipmapped textures are drawn (the fixes' regression coverage)
        print(f"cube-map and linear mipmapped textures, first met on {len(kinds)} maps:")
        for m in sorted(kinds):
            print(f"  {m}: {', '.join(sorted(kinds[m]))}")
    if fails:
        print(f"FAIL ({len(fails)}):")
        for x in fails:
            print("  " + x)
        return 1
    print("PASS")
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
    r.add_argument("--only", help="these maps only (names as the game's map change takes them, comma-separated)")
    r.add_argument("--audio", action="store_true",
                   help="M11.1b: trace the game's audio calls and write the mix (silent): `check` then checks each map's")
    p = sub.add_parser("report")
    p.add_argument("dirs", nargs="+")
    c = sub.add_parser("check")
    c.add_argument("evidence")
    c.add_argument("--baseline", help="TSV 'map area' from an accepted sweep: each map must arrive where it did then")
    c.add_argument("--write-baseline", help="write this sweep's arrivals as a baseline (after reviewing it)")
    c.add_argument("--no-prepared", dest="shaders_prepared", action="store_false",
                   help="the sweep ran without a preparation: translations in play are not a failure")
    a = ap.parse_args()
    if a.cmd == "check":
        return check(a)
    return run(a) if a.cmd == "run" else report(a.dirs)


if __name__ == "__main__":
    sys.exit(main())
