#!/usr/bin/env python3
"""R7's picture check (docs/NATIVE_R7.md): the window grabs of runs (GRAB_EVERY=5,
run/grabs/NNNN.png) side by side, grab for grab by time -- each grab's mean
brightness, its share of pixels above black (16 of 255: a scene drawn black,
R6's defect, shows as a share near 0) and, between two runs, the mean
absolute difference of the grabs at the same second (scenes move between
runs; a static screen should be near 0).

  tools/r7_grabs.py <run dir> [<reference run dir>]     (a dir holding run/grabs, or the grabs dir)
"""
import os
import sys

from PIL import Image, ImageChops, ImageStat


def grabs(d):
    for c in (os.path.join(d, "run", "grabs"), os.path.join(d, "grabs"), d):
        if os.path.isdir(c) and any(f.endswith(".png") for f in os.listdir(c)):
            return c, sorted(f for f in os.listdir(c) if f.endswith(".png"))
    return None, []


def stats(path):
    im = Image.open(path).convert("L")
    h = im.histogram()
    return ImageStat.Stat(im).mean[0], sum(h[16:]) / max(1, sum(h)), im


def main():
    a_dir, a = grabs(sys.argv[1])
    if not a:
        print("no grabs in", sys.argv[1])
        return 1
    b_dir, b = grabs(sys.argv[2]) if len(sys.argv) > 2 else (None, [])
    bset = set(b)
    dark = 0
    print("grab   mean  above-black" + ("   ref-mean ref-above  mean|diff|" if b else ""))
    for f in a:
        m, ab, im = stats(os.path.join(a_dir, f))
        dark += ab < 0.5
        line = f"{f[:-4]:>5} {m:6.1f} {ab:8.3f}"
        if b and f in bset:
            m2, ab2, im2 = stats(os.path.join(b_dir, f))
            if im2.size != im.size:
                im2 = im2.resize(im.size)
            d = ImageStat.Stat(ImageChops.difference(im, im2)).mean[0]
            line += f"   {m2:8.1f} {ab2:8.3f}  {d:9.1f}"
        print(line)
    print(f"{len(a)} grabs, {dark} with under half the pixels above black")
    return 0


if __name__ == "__main__":
    sys.exit(main())
