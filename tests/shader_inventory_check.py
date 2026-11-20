#!/usr/bin/env python3
"""`dotheroes --shader-inventory --recipes FILE` answers (M11.4, the review's W-3: the recipes' depth format was made
the device's before any device existed, and the command crashed -- exit 139).

    python3 tests/shader_inventory_check.py --bin GAME --disc USRDIR --recipes FILE

Pass: exit 0, an "inventory" JSON with the disc's programs and the recipes counted. No window, no device.
"""
import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import desktop_guard  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description="the shader inventory with recipes, before any device")
    ap.add_argument("--bin", required=True)
    ap.add_argument("--disc", required=True)
    ap.add_argument("--recipes", required=True)
    a = ap.parse_args()
    # through the desktop guard like every game the tests start: the inventory opens no window, and if a change ever
    # made it, the game would render offscreen and the Watch would end it
    env = dict(os.environ)
    desktop_guard.game_env(env, False, quiet=True)
    p = subprocess.Popen([a.bin, "--shader-inventory", "--disc", a.disc, "--recipes", a.recipes], env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    watch = desktop_guard.Watch(p, "the game").start()
    try:
        out, err = p.communicate(timeout=600)
    except subprocess.TimeoutExpired:
        p.kill()
        out, err = p.communicate()
    watch.stop()
    r = subprocess.CompletedProcess(p.args, p.returncode, out, err)
    if watch.violation:
        print("FAIL: " + watch.violation)
        return 1
    inv, rec = None, None
    for ln in r.stdout.splitlines():
        if ln.startswith("{"):
            try:
                d = json.loads(ln)
                inv, rec = d.get("inventory"), d.get("recipes")
            except ValueError:
                pass
    bad = []
    if r.returncode != 0:
        bad.append(f"exit {r.returncode}")
    if not inv or not inv.get("fragment", {}).get("programs") or not inv.get("sources"):
        bad.append("no inventory with the disc's programs")
    if not rec:
        bad.append("the recipes were not read (no \"recipes\" in the answer)")
    print(("PASS" if not bad else "FAIL") + ": the shader inventory with recipes" +
          (f" -- {'; '.join(bad)}" if bad else f" ({inv['fragment']['programs']} fragment programs; recipes "
                                                f"{json.dumps(rec)[:120]})"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
