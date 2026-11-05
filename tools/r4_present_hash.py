#!/usr/bin/env python3
"""R4's same-frame comparison (docs/NATIVE_R4.md section 7): the presented
images of a run alternating R3 and R4 (DOTHEROES_PPFX_BYPASS=alt:<n>) with
DOTHEROES_PPFX_PRESENT_HASH. Where the scene is still -- the same image
presented again and again, as under a menu that stops the game -- the R3
frames (the engine drew PPFX) and the R4 frames (the native renderer did,
handed back) show the same game state, so their images must be identical.

At each switch between the modes: the hashes of the k presents before it
(all one mode) and the k after (the other). A switch is
  still-identical   all 2k equal: the same image either way
  still-differs     the k before equal each other, the k after equal each
                    other, but not the same hash: a difference R4 made
  moving            the image changes within either side: no conclusion
Also the same for the mode's own frames: runs of equal hashes within one
mode, to show what "still" looks like.

  tools/r4_present_hash.py <present_hash.txt> [--k 4]
"""
import argparse


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--k", type=int, default=4)
    a = ap.parse_args()
    rows = []
    for ln in open(a.path, encoding="utf-8"):
        if ln.startswith("#"):
            continue
        f = ln.split()
        if len(f) >= 4:
            rows.append((int(f[0]), int(f[1]), int(f[2]), f[3]))
    k = a.k
    counts = {"still-identical": 0, "still-differs": 0, "moving": 0}
    differs = []
    for i in range(k, len(rows) - k + 1):
        if rows[i][2] == rows[i - 1][2]:
            continue
        before = rows[i - k:i]
        after = rows[i:i + k]
        if len({r[2] for r in before}) != 1 or len({r[2] for r in after}) != 1:
            continue   # a switch too close to another
        hb, ha = {r[3] for r in before}, {r[3] for r in after}
        if len(hb) == 1 and len(ha) == 1:
            if hb == ha:
                counts["still-identical"] += 1
            else:
                counts["still-differs"] += 1
                differs.append((rows[i - 1], rows[i]))
        else:
            counts["moving"] += 1
    still_runs = sum(1 for i in range(1, len(rows)) if rows[i][3] == rows[i - 1][3])
    print("# %s: %d presents (%d R4), %d presented the same image as the one before" % (
        a.path, len(rows), sum(r[2] for r in rows), still_runs))
    print("switches R3<->R4 with %d presents each side: still and identical %d | still and different %d | "
          "moving %d" % (k, counts["still-identical"], counts["still-differs"], counts["moving"]))
    for b, c in differs[:20]:
        print("  differs: present %d (frame %d, %s) %s -> present %d (frame %d, %s) %s" % (
            b[0], b[1], "R4" if b[2] else "R3", b[3], c[0], c[1], "R4" if c[2] else "R3", c[3]))


if __name__ == "__main__":
    main()
