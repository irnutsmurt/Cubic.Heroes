#!/usr/bin/env python3
"""R7's movie comparison (docs/NATIVE_R7.md): the attract mode's demo movies
(Bink videos the game draws with its own draw-arrays) as two builds show
them, frame by frame. Each run grabs the window fast (GRAB_EVERY=0.1 with
VISIBLE=1: grabs/<ms>.png). The runs reach the movies at different moments,
so every grab of the first run is matched by content against every grab of
the second (a 64x36 thumbnail of the picture below the frame-rate readout's
strip); a pair whose thumbnails are nearly equal is the same movie frame
shown by both builds. For those pairs, what remains is how each build drew
that frame: the mean absolute difference of the full images (of 255), its
distribution, and the worst pairs side by side.

  tools/r7_bink_compare.py <run A> <run B> [--same 1.5] [--worst N] [--out DIR]
"""
import argparse
import os

from PIL import Image, ImageChops, ImageStat

TOP = 40   # the frame-rate readout's strip (each build's own overlay)


def grabs(d):
    g = os.path.join(d, "run", "grabs") if os.path.isdir(os.path.join(d, "run", "grabs")) else d
    return [(int(f[:-4]) / 1000.0, os.path.join(g, f)) for f in sorted(os.listdir(g))
            if f.endswith(".png") and f[:-4].isdigit() and len(f) == 12]


def body(path):
    im = Image.open(path).convert("RGB")
    return im.crop((0, TOP, im.size[0], im.size[1]))


def thumb(path):
    return body(path).convert("L").resize((64, 36), Image.BILINEAR)


def mad(a, b):
    return ImageStat.Stat(ImageChops.difference(a, b).convert("L")).mean[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a"); ap.add_argument("b")
    ap.add_argument("--same", type=float, default=1.5, help="thumbnail difference under which two grabs are one frame")
    ap.add_argument("--worst", type=int, default=5)
    ap.add_argument("--out")
    o = ap.parse_args()
    A, B = grabs(o.a), grabs(o.b)
    ta = [thumb(p) for _, p in A]
    tb = [thumb(p) for _, p in B]
    rows = []
    for (t, pa), x in zip(A, ta):
        if ImageStat.Stat(x).mean[0] < 8:   # a black frame (a fade, a load): nothing to compare
            continue
        j, d = min(((j, mad(x, y)) for j, y in enumerate(tb)), key=lambda q: q[1])
        if d > o.same:
            continue
        full = mad(body(pa), body(B[j][1]))
        rows.append((full, d, t, B[j][0], pa, B[j][1]))
    if not rows:
        print("no frame of run A found in run B"); return 1
    ds = sorted(r[0] for r in rows)
    pct = lambda q: ds[min(len(ds) - 1, int(len(ds) * q))]
    print("%d of %d grabs of run A are a frame run B also grabbed (thumbnails within %.1f)" % (len(rows), len(A), o.same))
    print("full-image mean |difference| for those pairs (of 255): p50 %.2f p90 %.2f p99 %.2f max %.2f" % (
        pct(0.5), pct(0.9), pct(0.99), ds[-1]))
    print("pairs over 2: %d, over 4: %d" % (sum(1 for d in ds if d > 2), sum(1 for d in ds if d > 4)))
    for full, d, t, tb_, pa, pb in sorted(rows, reverse=True)[:o.worst]:
        print("  worst: %.2f (thumbnails %.2f) at A %.2f s / B %.2f s" % (full, d, t, tb_))
        if o.out:
            os.makedirs(o.out, exist_ok=True)
            ia, ib = body(pa), body(pb)
            w, h = ia.size
            c = Image.new("RGB", (w * 2, h)); c.paste(ia, (0, 0)); c.paste(ib.resize(ia.size), (w, 0))
            c.resize((w, h // 2)).save(os.path.join(o.out, "worst_%.2f_%07.2f.png" % (full, t)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
