#!/usr/bin/env python3
"""Tile a run's screenshots into one image for review.

  tests/contact_sheet.py <run dir> [out.png] [--cols N] [--width W]
"""
import argparse, glob, os
from PIL import Image, ImageDraw, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

ap = argparse.ArgumentParser()
ap.add_argument("run"); ap.add_argument("out", nargs="?")
ap.add_argument("--cols", type=int, default=4); ap.add_argument("--width", type=int, default=320)
ap.add_argument("--last", type=int, default=0, help="only the last N shots")
a = ap.parse_args()
files = sorted(glob.glob(os.path.join(a.run, "shots", "[0-9]*.png")))
if a.last: files = files[-a.last:]
w, h = a.width, a.width * 9 // 16
tiles = []
for f in files:
    try: tiles.append((f, Image.open(f).convert("RGB").resize((w, h))))
    except Exception: pass
rows = (len(tiles) + a.cols - 1) // a.cols or 1
sheet = Image.new("RGB", (w * a.cols, h * rows), (40, 40, 40))
d = ImageDraw.Draw(sheet)
for i, (f, im) in enumerate(tiles):
    x, y = (i % a.cols) * w, (i // a.cols) * h
    sheet.paste(im, (x, y)); d.text((x + 4, y + 4), os.path.basename(f)[:-4] + "s", fill=(255, 255, 0))
out = a.out or os.path.join(a.run, "sheet.png")
sheet.save(out); print(out, len(tiles), "shots")
