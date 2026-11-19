#!/usr/bin/env python3
"""A timed gameplay run with the machine's own counters (M11.4, P7: AMD on Linux).

    python3 tests/perf_run.py --bin GAME --drive DRIVE --tag TAG [--fps 60|0] [--res WxH|native]
                              [--trace START:SECS] [--secs N] [NAME=value ...]

Runs tests/boot_smoke.py with the drive (offscreen, DUMP=0, PS3_FTRACE=START:SECS), and while the trace is recording
samples every 0.5 s: the GPU's busy percentage and its memory (amdgpu's sysfs: VRAM, the carve-out of an APU, and GTT,
the system memory it maps -- each as growth over the idle machine's before the run), and the game's CPU time (all its
threads; 100% is one core). Then the trace's frame times (the guest's flip requests, as tests/frame_times.py reads them):
fps, the median, p95, p99, the worst frame, the 1% low (the frame rate of the slowest 1% of frames) and the frames over
25 and 33 ms. Writes <run>/perf.json and prints one line. The game's speed does not depend on its frame rate (M11.1b),
so a slower run is a slower picture, not a slower game.
"""
import argparse
import glob
import gzip
import json
import os
import statistics
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FLIP_REQ = 9


def gpu_dir():
    for d in sorted(glob.glob("/sys/class/drm/card*/device")):
        if os.path.exists(os.path.join(d, "gpu_busy_percent")):
            return d
    return None


def rd(d, f):
    try:
        with open(os.path.join(d, f)) as fh:
            return int(fh.read().strip())
    except (OSError, ValueError):
        return None


def game_pid():
    r = subprocess.run(["pgrep", "-x", "dotheroes"], capture_output=True, text=True).stdout.split()
    return int(r[0]) if r else None


def cpu_ticks(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            v = f.read().rsplit(")", 1)[1].split()
        return int(v[11]) + int(v[12])   # utime + stime of the whole process
    except (OSError, IndexError, ValueError):
        return None


def frames(trace):
    t = []
    opener = gzip.open if trace.endswith(".gz") else open
    with opener(trace, "rt", encoding="utf-8", errors="replace") as f:
        for line in f:
            p = line.split()
            if len(p) == 6 and p[0].isdigit() and int(p[0]) == FLIP_REQ:
                t.append(float(p[4]))
    t.sort()
    return [(b - a) / 1000.0 for a, b in zip(t, t[1:])]   # microseconds -> ms, as tests/frame_times.py reads them


def summary(ms):
    if not ms:
        return {}
    s = sorted(ms)
    q = lambda p: s[min(len(s) - 1, int(p * len(s)))]
    worst = s[-max(1, len(s) // 100):]
    return {"frames": len(ms), "fps": round(1000 * len(ms) / sum(ms), 1), "median_ms": round(statistics.median(ms), 2),
            "p95_ms": round(q(0.95), 2), "p99_ms": round(q(0.99), 2), "max_ms": round(s[-1], 2),
            "low1_fps": round(1000 / statistics.mean(worst), 1),
            "over25": sum(1 for x in ms if x > 25), "over33": sum(1 for x in ms if x > 33)}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bin", required=True)
    ap.add_argument("--drive", required=True)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--fps", default="60")
    ap.add_argument("--res", default=None)
    ap.add_argument("--trace", default="24:27")
    ap.add_argument("--secs", default="300")
    ap.add_argument("extra", nargs="*")
    a = ap.parse_args()
    start, length = (float(x) for x in a.trace.split(":"))
    env = dict(os.environ, BIN=a.bin, DRIVE=a.drive, RUN_TAG=a.tag, KEEP_RUNS="60", DUMP="0", SHOT_EVERY="0",
               DOTHEROES_FPS=a.fps, PS3_FTRACE=f"{a.trace}:ftrace.txt")
    if a.res:
        if a.res in ("native", "1280x720", "1920x1080", "2560x1440", "3840x2160"):
            env["DOTHEROES_RENDER_RES"] = a.res
        else:
            env.update(DOTHEROES_RENDER_RES="custom", DOTHEROES_RENDER_RES_CUSTOM=a.res)
    for kv in a.extra:
        k, v = kv.split("=", 1)
        env[k] = v
    g = gpu_dir()
    idle = {"vram": rd(g, "mem_info_vram_used"), "gtt": rd(g, "mem_info_gtt_used")} if g else {}
    samples, stop = [], threading.Event()

    def sample():
        t0, pid, last = None, None, None
        while not stop.wait(0.5):
            pid = pid or game_pid()
            if not pid:
                continue
            t0 = t0 or time.time()
            el = time.time() - t0
            tick = cpu_ticks(pid)
            cpu = None if tick is None or last is None else (tick - last[0]) / os.sysconf("SC_CLK_TCK") / \
                (time.time() - last[1]) * 100
            last = (tick, time.time()) if tick is not None else last
            if g and start <= el <= start + length:
                samples.append({"busy": rd(g, "gpu_busy_percent"), "vram": rd(g, "mem_info_vram_used"),
                                "gtt": rd(g, "mem_info_gtt_used"), "cpu": cpu})
    th = threading.Thread(target=sample, daemon=True)
    th.start()
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tests", "boot_smoke.py"), a.secs], cwd=ROOT, env=env,
                       capture_output=True, text=True)
    stop.set()
    run = sorted(glob.glob(os.path.join(ROOT, "build", "test", f"boot-*-{a.tag}")))[-1]
    trace = os.path.join(run, "ftrace.txt")
    out = {"tag": a.tag, "fps_limit": a.fps, "res": a.res or "default", "exit": r.stdout.splitlines()[1] if
           len(r.stdout.splitlines()) > 1 else r.stdout, "frames": summary(frames(trace)) if os.path.exists(trace) else {}}
    # a route that did not run is no measurement (M11.4: an installed game with developer mode off ignored the drive's
    # state probe, its drive failed at the title, and the "frames" were the title screen's)
    try:
        with open(os.path.join(run, "result.json"), encoding="utf-8") as f:
            ended = json.load(f).get("ended_by", "")
    except (OSError, ValueError):
        ended = "no result.json"
    out["valid"] = "failed" not in ended and "no result" not in ended
    out["ended_by"] = ended
    mb = lambda v: round(v / 2**20) if v is not None else None
    if samples:
        busy = [s["busy"] for s in samples if s["busy"] is not None]
        cpu = [s["cpu"] for s in samples if s["cpu"] is not None]
        out["gpu_busy_pct"] = {"mean": round(statistics.mean(busy)), "max": max(busy)} if busy else None
        out["game_cpu_pct"] = {"mean": round(statistics.mean(cpu)), "max": round(max(cpu))} if cpu else None
        out["gpu_mem_mb"] = {"vram_over_idle": mb(max(s["vram"] for s in samples) - idle["vram"]),
                             "gtt_over_idle": mb(max(s["gtt"] for s in samples) - idle["gtt"])}
    with open(os.path.join(run, "perf.json"), "w") as f:
        json.dump(out, f, indent=1)
    fr = out["frames"]
    print(("" if out["valid"] else f"INVALID ({ended}): ") + f"{a.tag}: {fr.get('fps')} fps (median {fr.get('median_ms')}, p95 {fr.get('p95_ms')}, p99 "
          f"{fr.get('p99_ms')}, worst {fr.get('max_ms')} ms; 1% low {fr.get('low1_fps')} fps; >25 ms "
          f"{fr.get('over25')}, >33 ms {fr.get('over33')}) | GPU busy {out.get('gpu_busy_pct')} | game CPU "
          f"{out.get('game_cpu_pct')} | GPU memory {out.get('gpu_mem_mb')}")
    return 0 if out["valid"] else 1


if __name__ == "__main__":
    sys.exit(main())
