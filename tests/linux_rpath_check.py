#!/usr/bin/env python3
"""The bundled libraries come before an injected LD_LIBRARY_PATH (M11.4: under Lutris, which puts /usr/lib first, the
game ran on the system's SDL2; the binaries found theirs through DT_RUNPATH, which LD_LIBRARY_PATH outranks).

    python3 tests/linux_rpath_check.py --package DIR [--game BUILD_DIR]

DIR: a Linux package's folder (or an installation root). BUILD_DIR: a game build the worker made (an installation's
builds/<id>/). Checks:
  1. readelf: the launcher, the game and the bundled libraries that load others of ours (OpenAL Soft: SDL2; FFmpeg's
     three) carry DT_RPATH naming only $ORIGIN or $ORIGIN/lib -- no DT_RUNPATH, no other folder;
  2. LD_DEBUG=libs, with no LD_LIBRARY_PATH and with LD_LIBRARY_PATH=/usr/lib:/usr/lib64:/usr/lib/x86_64-linux-gnu
     (a launcher's): the launcher's `--version` and the game's `--info` load libSDL2, libopenal and FFmpeg's libraries
     from the package, and the system's own (glibc, the Vulkan loader when it is loaded) from the system.
Offscreen (no display). Exit 1 on any finding.
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import desktop_guard  # noqa: E402

OURS = ("libSDL2-2.0.so.0", "libopenal.so.1", "libavcodec.so", "libavformat.so", "libavutil.so")
HOSTILE = "/usr/lib:/usr/lib64:/usr/lib/x86_64-linux-gnu"
bad = []


def dyn(path):
    out = subprocess.run(["readelf", "-d", path], capture_output=True, text=True).stdout
    rp = re.findall(r"\((RPATH|RUNPATH)\)\s+Library r(?:un)?path: \[([^\]]*)\]", out)
    return rp


def check_tags(path, want):
    tags = dyn(path)
    name = os.path.relpath(path)
    if any(t == "RUNPATH" for t, _ in tags):
        bad.append(f"{name}: DT_RUNPATH {tags} (LD_LIBRARY_PATH outranks it)")
    rp = [v for t, v in tags if t == "RPATH"]
    if rp != [want]:
        bad.append(f"{name}: DT_RPATH {rp}, expected ['{want}']")
    else:
        print(f"  {name}: DT_RPATH {want}")


def loaded(cmd, env, cwd):
    """the libraries the dynamic loader opened, by name -> path (LD_DEBUG=libs)"""
    r = subprocess.run(cmd, env=dict(env, LD_DEBUG="libs"), cwd=cwd, capture_output=True, text=True, timeout=180)
    got = {}
    for ln in r.stderr.splitlines():
        m = re.search(r"calling init: (.+?)\s*$", ln)
        if m:
            got[os.path.basename(m.group(1))] = m.group(1)
    return got


def check_run(label, cmd, env, cwd, lib_dir):
    for llp in (None, HOSTILE):
        e = dict(env)
        e.pop("LD_LIBRARY_PATH", None)
        if llp:
            e["LD_LIBRARY_PATH"] = llp
        got = loaded(cmd, e, cwd)
        tag = f"{label} ({'LD_LIBRARY_PATH=' + llp if llp else 'no LD_LIBRARY_PATH'})"
        ours = {n: p for n, p in got.items() if n.startswith(OURS)}
        if not ours:
            bad.append(f"{tag}: none of the bundled libraries was loaded")
        for n, p in sorted(ours.items()):
            if os.path.dirname(os.path.realpath(p)) != os.path.realpath(lib_dir):
                bad.append(f"{tag}: {n} from {p}, not the package's {lib_dir}")
        for n in ("libc.so.6", "libvulkan.so.1"):
            if n in got and os.path.realpath(os.path.dirname(got[n])) == os.path.realpath(lib_dir):
                bad.append(f"{tag}: {n} from the package's folder (only the bundled libraries belong there)")
        print(f"  {tag}: " + ", ".join(f"{n} <- {os.path.dirname(p)}" for n, p in sorted(ours.items())) +
              (f"; libvulkan.so.1 <- {os.path.dirname(got['libvulkan.so.1'])}" if "libvulkan.so.1" in got else ""))


def main():
    ap = argparse.ArgumentParser(description="the bundled libraries before an injected LD_LIBRARY_PATH")
    ap.add_argument("--package", required=True)
    ap.add_argument("--game")
    a = ap.parse_args()
    pkg = os.path.abspath(a.package)
    env = desktop_guard.no_display(dict(os.environ))
    env["SDL_VIDEODRIVER"] = "offscreen"
    print("static (readelf):")
    check_tags(os.path.join(pkg, "CubicHeroes"), "$ORIGIN/lib")
    wl = os.path.join(pkg, "worker", "lib")
    for n in sorted(os.listdir(wl)):
        if n.startswith(("libopenal.so", "libavcodec.so", "libavformat.so", "libavutil.so")):
            check_tags(os.path.join(wl, n), "$ORIGIN")
    if a.game:
        g = os.path.abspath(a.game)
        check_tags(os.path.join(g, "dotheroes"), "$ORIGIN/lib")
        for n in sorted(os.listdir(os.path.join(g, "lib"))):
            if n.startswith(("libopenal.so", "libavcodec.so", "libavformat.so", "libavutil.so")):
                check_tags(os.path.join(g, "lib", n), "$ORIGIN")
    print("at run time (LD_DEBUG=libs):")
    check_run("the launcher --version", [os.path.join(pkg, "CubicHeroes"), "--version"], env, pkg,
              os.path.join(pkg, "lib"))
    if a.game:
        check_run("the game --info", [os.path.join(g, "dotheroes"), "--info"], env, g, os.path.join(g, "lib"))
    for b in bad:
        print("FINDING: " + b)
    print(("PASS" if not bad else "FAIL") + f": {len(bad)} finding(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
