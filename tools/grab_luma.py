#!/usr/bin/env python3
"""The brightness over a run's window grabs (tests/boot_smoke.py GRAB_EVERY
under 0.5 s: grabs/<ms>.png): each grab's mean luma (Rec. 601, 0-255), and
the curve after the first dark stretch -- an area transition's fade -- so
two builds' transitions can be laid side by side (docs/NATIVE_R4.md
section 6).

  tools/grab_luma.py <run dir> [...] [--dark 12] [--after 6]

Prints, per run, the luma from the end of the first fade (the first grab
brighter than --dark after one at or below it) for --after seconds.
"""
import argparse
import glob
import os

from PIL import Image, ImageStat


def luma(path):
    im = Image.open(path).convert("RGB")
    im.thumbnail((320, 180))
    r, g, b = ImageStat.Stat(im).mean
    return 0.299 * r + 0.587 * g + 0.114 * b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--dark", type=float, default=12)
    ap.add_argument("--after", type=float, default=6)
    ap.add_argument("--fade", type=int, default=1, help="which fade: the n-th dark stretch")
    a = ap.parse_args()
    for d in a.runs:
        files = sorted(glob.glob(os.path.join(d, "grabs", "*.png")))
        pts = [(int(os.path.basename(f)[:-4]) / 1000.0, luma(f)) for f in files]
        if not pts:
            print("%s: no grabs" % d)
            continue
        # the n-th fade's end: dark, then the first bright grab
        n, start, dark = 0, None, False
        for t, y in pts:
            if y <= a.dark:
                dark = True
            elif dark:
                dark = False
                n += 1
                if n == a.fade:
                    start = t
                    break
        print("%s: %d grabs; fade %d ends at %s" % (d, len(pts), a.fade, "%.2f s" % start if start is not None else "-"))
        if start is None:
            continue
        line = []
        for t, y in pts:
            if start - 0.5 <= t <= start + a.after:
                line.append("%+.1f:%.0f" % (t - start, y))
        print("  " + " ".join(line))


if __name__ == "__main__":
    main()
