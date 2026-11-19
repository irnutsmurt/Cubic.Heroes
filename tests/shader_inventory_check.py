#!/usr/bin/env python3
"""`dotheroes --shader-inventory --recipes FILE` answers (M11.4, the review's W-3: the recipes' depth format was made
the device's before any device existed, and the command crashed -- exit 139).

    python3 tests/shader_inventory_check.py --bin GAME --disc USRDIR --recipes FILE

Pass: exit 0, an "inventory" JSON with the disc's programs and the recipes counted. No window, no device.
"""
import argparse
import json
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser(description="the shader inventory with recipes, before any device")
    ap.add_argument("--bin", required=True)
    ap.add_argument("--disc", required=True)
    ap.add_argument("--recipes", required=True)
    a = ap.parse_args()
    r = subprocess.run([a.bin, "--shader-inventory", "--disc", a.disc, "--recipes", a.recipes], capture_output=True,
                       text=True, timeout=600)
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
