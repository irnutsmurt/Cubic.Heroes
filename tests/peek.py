#!/usr/bin/env python3
"""Save the frame a running test last presented as <run>/peek.png (for review).

  tests/peek.py <run dir> [--scale 0.5]
"""
import argparse, os, shutil, time
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True
ap = argparse.ArgumentParser(); ap.add_argument("run"); ap.add_argument("--scale", type=float, default=1.0)
a = ap.parse_args()
src, tmp = os.path.join(a.run, "frame.ppm"), os.path.join(a.run, ".peek.ppm")
for _ in range(10):   # the backend rewrites the dump on every present: retry a torn copy
    shutil.copyfile(src, tmp)
    if os.path.getsize(tmp) >= 1280 * 720 * 3:
        break
    time.sleep(0.05)
im = Image.open(tmp).convert("RGB")
if a.scale != 1.0:
    im = im.resize((int(im.width * a.scale), int(im.height * a.scale)))
out = os.path.join(a.run, "peek.png"); im.save(out); os.remove(tmp); print(out)
