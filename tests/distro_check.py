#!/usr/bin/env python3
"""The Linux packages on other distributions (M11.4, A18): the shipped files run in clean containers of distributions
of the glibc baseline and after, not only on the machine that made them.

    python3 tests/distro_check.py --package DIR [--appimage FILE] [--game BUILD_DIR] [--images IMAGE ...]

DIR: the Linux package's folder (build/dist/DotHeroes-Setup-linux-x86_64, or an unpacked tarball); FILE: the AppImage;
BUILD_DIR: a game build made by the worker (an installation's builds/<id>/: the player's own program, built here --
never published, and only mounted read-only into the containers). In each image, with nothing installed in it:
  - CubicHeroes --version; --status --json on an empty folder; an unknown option's exit code (2);
  - the frozen worker: `DotHeroes-Setup --worker version`;
  - the AppImage with --appimage-extract-and-run (a container has no FUSE: the FUSE-less way), --version;
  - the game: --memtest (its guest-memory checks) and --info (FFmpeg loaded from its lib/).
No display is given to any container (the desktop guard's rule holds: nothing can open a window). Images are pulled
from Docker Hub by name; the run records each image's digest and glibc. Exit 1 if any check fails anywhere.
"""
import argparse
import json
import os
import subprocess
import sys

IMAGES = ["debian:bullseye", "ubuntu:20.04", "ubuntu:24.04", "fedora:41"]


def sh(image, mounts, script):
    cmd = ["docker", "run", "--rm", "--network", "none", "-e", "HOME=/tmp"]
    for src, dst in mounts:
        cmd += ["-v", f"{src}:{dst}:ro"]
    cmd += [image, "sh", "-c", script]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    return r.returncode, r.stdout, r.stderr


def check(image, pkg, appimage, game):
    subprocess.run(["docker", "pull", "-q", image], capture_output=True, check=True)
    digest = subprocess.run(["docker", "image", "inspect", image, "--format", "{{index .RepoDigests 0}}"],
                            capture_output=True, text=True).stdout.strip()
    mounts = [(pkg, "/pkg")] + ([(os.path.dirname(appimage), "/ai")] if appimage else []) + \
        ([(game, "/game")] if game else [])
    steps = [
        ("glibc", "ldd --version 2>&1 | head -n 1", lambda rc, o: rc == 0),
        ("launcher --version", "/pkg/CubicHeroes --version", lambda rc, o: rc == 0 and "CubicHeroes" in o),
        ("launcher --status", "mkdir -p '/tmp/a root é' && /pkg/CubicHeroes --install-dir '/tmp/a root é' --status --json",
         lambda rc, o: rc == 0 and json.loads(o.strip().splitlines()[-1]).get("installed") is False),
        ("usage exit code", "/pkg/CubicHeroes --no-such-option; echo rc=$?", lambda rc, o: "rc=2" in o),
        ("worker", "/pkg/worker/DotHeroes-Setup --worker version",
         lambda rc, o: rc == 0 and '"ok":true' in o.replace(" ", "")),
    ]
    if appimage:
        name = os.path.basename(appimage)
        steps.append(("AppImage, extract-and-run", f"cd /tmp && '/ai/{name}' --appimage-extract-and-run --version",
                      lambda rc, o: rc == 0 and "CubicHeroes" in o))
    if game:
        steps += [("game --memtest", "cd /tmp && /game/dotheroes --memtest 2>&1 | grep 'checks, '",
                   lambda rc, o: "0 failed: PASS" in o),
                  ("game --info", "cd /tmp && /game/dotheroes --info 2>/dev/null | tail -n 1",
                   lambda rc, o: rc == 0 and json.loads(o.strip().splitlines()[-1])["ffmpeg"]["ok"])]
    out = {"image": image, "digest": digest, "checks": {}}
    for label, script, ok in steps:
        rc, o, e = sh(image, mounts, script)
        try:
            good = bool(ok(rc, o))
        except (ValueError, KeyError, IndexError):
            good = False
        out["checks"][label] = {"ok": good, "out": (o.strip() or e.strip())[-200:]}
        if label == "glibc":
            out["glibc"] = o.strip()
    return out


def main():
    ap = argparse.ArgumentParser(description="the Linux packages on other distributions")
    ap.add_argument("--package", required=True)
    ap.add_argument("--appimage")
    ap.add_argument("--game")
    ap.add_argument("--images", nargs="*", default=IMAGES)
    ap.add_argument("--json")
    a = ap.parse_args()
    pkg = os.path.abspath(a.package)
    results, failed = [], 0
    for image in a.images:
        r = check(image, pkg, os.path.abspath(a.appimage) if a.appimage else None,
                  os.path.abspath(a.game) if a.game else None)
        results.append(r)
        bad = [k for k, v in r["checks"].items() if not v["ok"]]
        failed += bool(bad)
        print(f"[distro] {image} ({r.get('glibc', '?')}): {len(r['checks']) - len(bad)} of {len(r['checks'])} pass"
              + (f" -- failed: {', '.join(bad)}" if bad else ""))
        for k in bad:
            print(f"    {k}: {r['checks'][k]['out']}")
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(results, f, indent=1)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
