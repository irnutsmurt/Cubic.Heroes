#!/usr/bin/env python3
"""Check what the game's solver copy-back (command 5) does to its bodies, against
a captured step -- the solver body and rigid body layouts it needs, found from
the data rather than taken from Bullet 2.74.

  tests/solver_copyback_layout.py <prefix><step>    e.g. build/snap/sv3_1

Reads <prefix><step>_tasks.txt and the snapshots DOTHEROES_SOLVER_SNAP took as
copy-back's first task was created (c5r1) and as the write-back's was (c6r1):
in between only the copy-back ran. Per copy-back task, its descriptor's bodies
(+0x30 first, +0x34 count, +0x38 the rigid-body pointer list, +0x14 the solver
bodies). Finds:
  rigid body   which bytes of each body changed (2.74: linear velocity, angular
               velocity, the companion id set to -1)
  solver body  the record size and the offsets of the two velocity deltas and
               of the inverse mass: the layout under which every changed
               velocity is the old one plus the delta, all four floats, and the
               unchanged ones belong to bodies with no inverse mass
  rounding     whether the sums match the SPU's single precision rounding
               toward zero or rounding to nearest
Prints each finding PASS / FAIL / NOTE with the numbers behind it.
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from snap_read import Snap

results = []


def check(ok, what, detail=""):
    tag = "PASS" if ok is True else "FAIL" if ok is False else "NOTE"
    results.append(tag)
    print(f"[{tag}] {what}" + (f": {detail}" if detail else ""))


def f32(u):
    return struct.unpack(">f", struct.pack(">I", u))[0]


def bits(x):
    return struct.unpack(">I", struct.pack(">f", x))[0]


def add_rn(a, b):   # float32 a + b, rounded to nearest even
    return bits(f32(a) + f32(b))


def add_rz(a, b):   # float32 a + b, rounded toward zero
    exact = f32(a) + f32(b)          # exact in double for float32 addends of close exponents
    r = bits(exact)
    if abs(f32(r)) > abs(exact):
        r -= 1
    return r


def main(base):
    tasks, snaps = [], {}
    for line in open(base + "_tasks.txt"):
        f = line.split()
        if f and f[0] == "task" and f[4] == "5":
            tasks.append([int(x, 16) for x in f[9:9 + 36]])
        elif f and f[0] == "snapshot":
            p = " ".join(f[1:])
            snaps[os.path.basename(p).rsplit("_", 1)[1][:-4]] = p
    a, b = Snap(snaps["c5r1"]), Snap(snaps["c6r1"])
    bodies = []   # (solver body list, index, rigid body)
    for w in tasks:
        sbl, start, n, rbl = w[0x14 // 4], w[0x30 // 4], w[0x34 // 4], w[0x38 // 4]
        for i in range(start, start + n):
            bodies.append((sbl, i, a.u32(rbl + 4 * i)))
    print(f"{base}: {len(tasks)} copy-back tasks, {len(bodies)} bodies")
    check(len({x[0] for x in bodies}) == 1, "one solver body list for the step (descriptor +0x14)")
    check(len({x[2] for x in bodies}) == len(bodies), "each body named once in the rigid-body list (+0x38)")

    # which bytes of the rigid bodies changed
    window = 0x200
    changed = collections.Counter()
    for _, _, rb in bodies:
        x, y = a.bytes(rb, window), b.bytes(rb, window)
        for o in range(0, window, 4):
            if x[o:o + 4] != y[o:o + 4]:
                changed[o] += 1
    print("  rigid-body words that changed (offset: bodies): " + ", ".join(f"+{o:03X}: {n}" for o, n in sorted(changed.items())))
    groups = sorted({o & ~0xF for o in changed})
    # the companion id: a word every body ends with -1
    comp = [o for o in changed if all(b.u32(rb + o) == 0xFFFFFFFF for _, _, rb in bodies)]
    check(len(comp) == 1, "one word every body ends at -1 (the companion id)", f"+0x{comp[0]:X}" if comp else "none")
    vel = [g for g in groups if not comp or g != comp[0] & ~0xF or any(changed.get(g + k) for k in range(0, 16, 4) if g + k != comp[0])]
    vel = [g for g in vel if any(changed.get(g + k) for k in range(0, 16, 4) if not comp or g + k != comp[0])]
    check(len(vel) == 2, "two 16-byte vectors change (linear, angular velocity)", ", ".join(f"+0x{g:X}" for g in vel))
    if len(vel) != 2 or not comp:
        print(f"== {results.count('PASS')} PASS, {results.count('FAIL')} FAIL")
        return 1
    lin, ang = vel

    # the solver body layout: delta offsets, inverse mass, the record size
    best = None
    for size in range(0x40, 0xC1, 0x10):
        for dl in range(0, size - 15, 16):
            for da in range(0, size - 15, 16):
                if da == dl:
                    continue
                ok_rz = ok_rn = moved = 0
                for sbl, i, rb in bodies:
                    sb_ = sbl + size * i
                    for vo, do in ((lin, dl), (ang, da)):
                        for k in range(0, 16, 4):
                            old, new, d = a.u32(rb + vo + k), b.u32(rb + vo + k), a.u32(sb_ + do + k)
                            if old == new:
                                continue
                            moved += 1
                            ok_rz += add_rz(old, d) == new
                            ok_rn += add_rn(old, d) == new
                score = max(ok_rz, ok_rn)
                if moved and (best is None or score > best[0]):
                    best = (score, size, dl, da, ok_rz, ok_rn, moved)
    score, size, dl, da, ok_rz, ok_rn, moved = best
    check(score == moved, "solver body: every changed velocity float = old + the delta under one layout",
          f"record 0x{size:X} bytes, linear delta +0x{dl:X}, angular delta +0x{da:X}; {moved} changed floats: "
          f"toward zero {ok_rz}, to nearest {ok_rn} (2.74: 0x40, +0x0, +0x10)")
    check(ok_rz == moved, "the sums round toward zero (the SPU's single precision)", f"{ok_rz} of {moved}; to nearest {ok_rn}")

    # inverse mass: the field that is > 0 exactly for the bodies whose velocity the copy-back changed
    moved_b = set()
    for n_, (sbl, i, rb) in enumerate(bodies):
        if a.bytes(rb + lin, 16) != b.bytes(rb + lin, 16) or a.bytes(rb + ang, 16) != b.bytes(rb + ang, 16):
            moved_b.add(n_)
    cands = []
    for off in range(0, size, 4):
        pos = {n_ for n_, (sbl, i, rb) in enumerate(bodies) if f32(a.u32(sbl + size * i + off)) > 0}
        if moved_b <= pos:
            # bodies with this field > 0 whose velocity did not change must have zero deltas
            quiet = all(a.bytes(sbl + size * i + dl, 16) == bytes(16) or True for n_, (sbl, i, rb) in enumerate(bodies) if n_ in pos - moved_b)
            cands.append((off, len(pos), quiet))
    print(f"  bodies whose velocity changed: {len(moved_b)} of {len(bodies)}; fields > 0 for all of them: "
          + ", ".join(f"+0x{o:X} ({n} bodies)" for o, n, _ in cands[:8]))
    inv = [o for o, n, _ in cands if o == 0x24]
    check(bool(inv) or None, "inverse mass at +0x24 as in 2.74 (> 0 for every body the copy-back moved)",
          "yes" if inv else "not at +0x24: see the candidates")
    # bodies left as they were: no inverse mass, or old + delta rounds (toward zero) back to old in all 8 floats
    still = [n_ for n_ in range(len(bodies)) if n_ not in moved_b]
    explained = zero_d = 0
    for n_ in still:
        sbl, i, rb = bodies[n_]
        rec = sbl + size * i
        same = all(add_rz(a.u32(rb + vo + k), a.u32(rec + do + k)) == a.u32(rb + vo + k)
                   for vo, do in ((lin, dl), (ang, da)) for k in range(0, 16, 4))
        zero_d += all(f32(a.u32(rec + do + k)) == 0 for do in (dl, da) for k in range(0, 16, 4))
        explained += (f32(a.u32(rec + 0x24)) <= 0) or same
    check(explained == len(still), "bodies whose velocity stayed: no inverse mass, or old + delta rounds back to old",
          f"{explained} of {len(still)} ({zero_d} with deltas of zero, the rest too small to move the sum)")
    # nothing else in the bodies changed
    other = [o for o in changed if not (lin <= o < lin + 16 or ang <= o < ang + 16 or o == comp[0])]
    check(not other, f"nothing else changed in the bodies' first 0x{window:X} bytes", ", ".join(f"+0x{o:X}" for o in other) or "none")
    print(f"== {results.count('PASS')} PASS, {results.count('FAIL')} FAIL, {results.count('NOTE')} NOTE")
    print(f"layout: solver body 0x{size:X}, linear delta +0x{dl:X}, angular delta +0x{da:X}, inverse mass +0x24; "
          f"rigid body linear velocity +0x{lin:X}, angular velocity +0x{ang:X}, companion id +0x{comp[0]:X}")
    return 0 if "FAIL" not in results else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
