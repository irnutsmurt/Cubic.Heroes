#!/usr/bin/env python3
"""The regions of guest memory that differ between two snapshots (SPURS_SNAP / DOTHEROES_SOLVER_SNAP files): what a
command wrote, besides whatever the PPU wrote meanwhile.

  tests/snap_regions.py <a.bin> <b.bin> [--gap 64] [--top 40]

Lists each run of changed bytes (runs closer than --gap bytes merged): its address, length and how many bytes in it
differ, largest first.
"""
import argparse
import struct


def load(path):
    out = {}
    with open(path, "rb") as f:
        data = f.read()
    o = 0
    while o + 8 <= len(data):
        ea, n = struct.unpack_from("<II", data, o)
        o += 8
        out[ea] = data[o:o + n]
        o += n
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--gap", type=int, default=64)
    ap.add_argument("--top", type=int, default=40)
    a = ap.parse_args()
    A, B = load(a.a), load(a.b)
    runs = []
    for ea, x in A.items():
        y = B.get(ea)
        if y is None or len(y) != len(x):
            continue
        blk = 4096
        cur = None
        for o in range(0, len(x), blk):
            if x[o:o + blk] == y[o:o + blk]:
                continue
            for k in range(o, min(o + blk, len(x))):
                if x[k] != y[k]:
                    p = ea + k
                    if cur and p - cur[1] <= a.gap:
                        cur[1] = p + 1
                        cur[2] += 1
                    else:
                        if cur:
                            runs.append(cur)
                        cur = [p, p + 1, 1]
        if cur:
            runs.append(cur)
    runs.sort(key=lambda r: -(r[1] - r[0]))
    print("%d changed regions, %d bytes differ in all" % (len(runs), sum(r[2] for r in runs)))
    for s, e, n in runs[:a.top]:
        print("  %08X..%08X  %7d bytes, %7d differ" % (s, e, e - s, n))


if __name__ == "__main__":
    main()
