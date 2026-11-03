#!/usr/bin/env python3
"""port/native/inflate.c, byte for byte against zlib, on every gzip file of the
game's disc (the map packs and the rest), with its speed next to zlib's.

  tests/native/inflate_check.py [<dir>] [--limit N]

Needs build/native-test/inflate_test.exe (tests/native/inflate_test.c says how
to build it). Prints a line per file that differs or fails, then totals.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ap = argparse.ArgumentParser()
ap.add_argument("dir", nargs="?", default=os.path.join(ROOT, "build", "disc", "PS3_GAME", "USRDIR"))
ap.add_argument("--limit", type=int, default=0)
a = ap.parse_args()
exe = os.path.join(ROOT, "build", "native-test", "inflate_test.exe")
files = sorted(os.path.join(d, f) for d, _, fs in os.walk(a.dir) for f in fs if f.endswith(".gz"))
if a.limit:
    files = files[:a.limit]
ok = bad = 0
mb = ours = theirs = 0.0
with tempfile.TemporaryDirectory() as tmp:
    out = os.path.join(tmp, "out.bin")
    for path in files:
        data = open(path, "rb").read()
        t = time.perf_counter()
        want = zlib.decompress(data, 31)
        theirs += time.perf_counter() - t
        r = subprocess.run([exe, path, out], capture_output=True, text=True)
        got = open(out, "rb").read() if r.returncode == 0 and os.path.exists(out) else None
        if got != want:
            bad += 1
            print(f"DIFFERS: {path}: {r.stdout.strip() or r.stderr.strip()}")
        else:
            ok += 1
            ours += float(r.stdout.split(" in ")[1].split(" ms")[0]) / 1000
            mb += len(want) / 1e6
        if os.path.exists(out):
            os.remove(out)
print(f"{ok} of {len(files)} identical to zlib, {bad} not; {mb:.0f} MB out: "
      f"{mb / max(ours, 1e-9):.0f} MB/s here (best of 5), zlib {mb / max(theirs, 1e-9):.0f} MB/s (once)")
sys.exit(1 if bad else 0)
