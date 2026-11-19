#!/usr/bin/env python3
"""CI's build (M11.4, .github/workflows/ci.yml), the same steps on Windows and Linux, runnable locally too.

    python3 tools/ci_build.py [--steps zig,ps3recomp,deps,runtime,launcher,smoke]

1. zig        the pinned Zig (tools/fetch_zig.py, SHA-256 checked)
2. ps3recomp  the pinned ps3recomp with this project's patch series (tools/pipeline.py's toolchain stage)
3. deps       the third-party libraries from their pinned sources (tools/build_deps.py)
4. runtime    the runtime library, the native code and the HLE NID table in the release configuration
              (tools/zig_build.py build_prebuilt) against a stub lift (tools/ci_stub_lift.py: no game in it)
5. launcher   CubicHeroes (tools/build_launcher.py)
6. smoke      CubicHeroes's command line on a throwaway root: --version, --status --json, the usage and
              not-installed exit codes

Nothing here needs the game, its keys or its data, and nothing of the game is made: what this proves is that the
shared code compiles and links on the platform -- not that the game behaves (that is the regression suite's, on a
player's own installation).
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
PLATFORM = "windows" if os.name == "nt" else "linux"
STEPS = ["zig", "ps3recomp", "deps", "runtime", "launcher", "smoke"]
STUB = os.path.join(ROOT, "build", "ci", "stub")
LAUNCHER = os.path.join(ROOT, "build", "ci", "launcher")


def step(name, fn):
    t0 = time.monotonic()
    print(f"::group::{name}" if os.environ.get("GITHUB_ACTIONS") else f"== {name}", flush=True)
    try:
        fn()
    finally:
        if os.environ.get("GITHUB_ACTIONS"):
            print("::endgroup::", flush=True)
    print(f"[ci] {name}: done in {time.monotonic() - t0:.0f} s", flush=True)


def s_zig():
    import fetch_zig
    fetch_zig.fetch(PLATFORM, os.path.join(ROOT, "build", "toolchain"))


def s_ps3recomp():
    import pipeline
    pipeline.toolchain()


def s_deps():
    import build_deps
    build_deps.build(PLATFORM)


def s_runtime():
    import zig_build
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "ci_stub_lift.py"), STUB])
    zig_build.build_prebuilt(PLATFORM, os.path.join(ROOT, "build", "ref", "ps3recomp"), STUB)


def s_launcher():
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "build_launcher.py"), "--out", LAUNCHER])


def s_smoke():
    exe = os.path.join(LAUNCHER, "CubicHeroes.exe" if PLATFORM == "windows" else "CubicHeroes")
    env = dict(os.environ)
    if PLATFORM == "linux":   # the command line never opens a window (tests/desktop_guard.py's rule)
        env.pop("DISPLAY", None)
        env["WAYLAND_DISPLAY"] = "desktop-guard-no-display"
    with tempfile.TemporaryDirectory(prefix="ci root ") as root:
        def run(*args):
            r = subprocess.run([exe, *args], capture_output=True, text=True, env=env, timeout=120)
            return r.returncode, r.stdout
        rc, out = run("--version")
        assert rc == 0 and "CubicHeroes" in out, (rc, out)
        rc, out = run("--install-dir", root, "--status", "--json")
        st = json.loads(out.strip().splitlines()[-1])
        assert rc == 0 and st["ok"] and not st["installed"] and st["writable"], (rc, out)
        rc, _ = run("--no-such-option")
        assert rc == 2, rc
        rc, out = run("--install-dir", root, "--play", "--json")
        assert rc == 3, (rc, out)
    print(f"[ci] smoke: {exe} --version, --status, exit codes 2 and 3: pass")


def main():
    ap = argparse.ArgumentParser(description="CI's build")
    ap.add_argument("--steps", default=",".join(STEPS))
    a = ap.parse_args()
    fns = {"zig": s_zig, "ps3recomp": s_ps3recomp, "deps": s_deps, "runtime": s_runtime, "launcher": s_launcher,
           "smoke": s_smoke}
    for name in a.steps.split(","):
        step(name, fns[name])
    return 0


if __name__ == "__main__":
    sys.exit(main())
