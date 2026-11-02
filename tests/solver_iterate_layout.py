#!/usr/bin/env python3
"""Check what the game's solver iterate (command 4) does in a round, against a
captured step: the structures port/native/bullet_solver.cpp's iterate uses,
found in the data rather than taken from Bullet 2.74.

  tests/solver_iterate_layout.py <prefix><step>    e.g. build/snap/sv4_300

Reads <prefix><step>_tasks.txt and the snapshots DOTHEROES_SOLVER_SNAP took as
rounds started: c4r1 and c4r2 (and every round's with
DOTHEROES_SOLVER_SNAP_ROUNDS=1), and c5r1 after the last; between two of them
one round ran (the last pair: from the last round's start to the copy-back).
From an iterate task's descriptor: +0x10 hash, +0x14 solver bodies, +0x18
contact rows, +0x20 the cells' body index lists. Per round:
  rows      which words of each contact row change: the impulse (+0x54) in
            every row, the limits (+0x84, +0x88) in the friction rows only
  bodies    each row's two body ids (+0x58, +0x5C) index its cell's list,
            the same in a contact's three rows
  friction  each friction row's limits after the round: friction (+0x60) times
            -/+ the contact's impulse after it, in the SPU's single precision
            (truncated); the normal rows' impulses within their limits
  solver    in the solver bodies, only the x, y, z of the two deltas (+0x0,
            +0x10) change, and only in bodies of nonzero inverse mass (+0x24);
            bodies no cell lists unchanged
Also the shape: cells with work, contacts, bodies in more than one cell's
list, the largest cell against one packet's room. PASS / FAIL / NOTE.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from snap_read import Snap

results = []
SIGN = 0x80000000


def check(ok, what, detail=""):
    tag = "PASS" if ok is True else "FAIL" if ok is False else "NOTE"
    results.append(tag)
    print(f"[{tag}] {what}" + (f": {detail}" if detail else ""))


def f32(u):
    return struct.unpack(">f", struct.pack(">I", u))[0]


def key(u):   # the SPU's compare order: any zero (exponent 0) is 0
    if not (u & 0x7F800000):
        return 0
    m = u & 0x7FFFFFFF
    return -m if u & SIGN else m


def fm_rz(a, b):   # SPU single multiply: exact product truncated, below 2^-126 is +0
    if not (a & 0x7F800000) or not (b & 0x7F800000):
        return 0
    p = f32(a & 0x7FFFFFFF) * f32(b & 0x7FFFFFFF)   # two 24-bit significands: exact in double
    d = struct.unpack("<Q", struct.pack("<d", p))[0]
    e = ((d >> 52) & 0x7FF) - 1023
    if e < -126:
        return 0
    s = (a ^ b) & SIGN
    if e > 128:
        return s | 0x7FFFFFFF
    return s | ((e + 127) << 23) | ((d >> 29) & 0x7FFFFF)


def words(s, ea, n):
    return struct.unpack(f">{n}I", s.bytes(ea, 4 * n))


def round_pair(a, b, d, label):
    hash_, sbl, rows, offs = d[0x10 // 4], d[0x14 // 4], d[0x18 // 4], d[0x20 // 4]
    cells, listed = [], {}
    for i in range(128):
        c = hash_ + 0x870 + 16 * i
        nb, nc, ncons = a.u16(c), a.u16(c + 8), a.u16(c + 12)
        if not nc and not ncons:
            continue
        lst = [a.u32(offs + 4 * (a.u16(c + 2) + k)) for k in range(nb)]
        cells.append((i, nb, lst, nc, a.u16(c + 10)))
        for x in lst:
            listed[x] = listed.get(x, 0) + 1
    contacts = sum(c[3] for c in cells)
    room = [((0x25800 - (((4 * nb + 15) & ~15) + 0x40 * nb)) // 0x1B0, nc) for _, nb, _, nc, _ in cells]
    biggest = max(room, key=lambda r: r[1] / max(r[0], 1)) if room else (0, 0)
    print(f"{label}: {len(cells)} cells with work, {contacts} contacts, {len(listed)} bodies listed, "
          f"{sum(1 for v in listed.values() if v > 1)} in more than one cell's list; largest cell {biggest[1]} contacts "
          f"against {biggest[0]} a packet")

    changed = {0: {}, 1: {}, 2: {}}
    ids_in = ids_same = lim_ok = lim_n = within = 0
    for i, nb, lst, nc, first in cells:
        for j in range(nc):
            base = rows + (first + 3 * j) * 0x90
            w0 = [words(a, base + k * 0x90, 36) for k in range(3)]
            w1 = [words(b, base + k * 0x90, 36) for k in range(3)]
            for k in range(3):
                for o in range(36):
                    if w0[k][o] != w1[k][o]:
                        changed[k][4 * o] = changed[k].get(4 * o, 0) + 1
            ida, idb = w0[0][0x58 // 4], w0[0][0x5C // 4]
            ids_in += ida < nb and idb < nb
            ids_same += all(w0[k][0x58 // 4] == ida and w0[k][0x5C // 4] == idb for k in (1, 2))
            imp = w1[0][0x54 // 4]
            for k in (1, 2):
                fr = w1[k][0x60 // 4]
                lim_n += 1
                lim_ok += w1[k][0x84 // 4] == fm_rz(fr, imp ^ SIGN) and w1[k][0x88 // 4] == fm_rz(fr, imp)
            lo, up = w1[0][0x84 // 4], w1[0][0x88 // 4]
            within += key(lo) <= key(imp) <= key(up)
    fmt = lambda m: ", ".join(f"+0x{o:02X} x{n}" for o, n in sorted(m.items())) or "none"
    check(set(changed[0]) <= {0x54}, f"{label} rows: a contact's normal row changes only its impulse", fmt(changed[0]))
    check(set(changed[1]) | set(changed[2]) <= {0x54, 0x84, 0x88},
          f"{label} rows: friction rows change only impulse and limits", f"{fmt(changed[1])}; {fmt(changed[2])}")
    check(ids_in == contacts and ids_same == contacts, f"{label} rows: body ids index the cell's list, the same in all three",
          f"{ids_in} and {ids_same} of {contacts}")
    check(lim_ok == lim_n, f"{label} friction: limits = friction x -/+ the contact's impulse after the round (truncated)",
          f"{lim_ok} of {lim_n} rows")
    check(within == contacts, f"{label} normal rows: impulse within [lower, upper] after the round", f"{within} of {contacts}")

    top = max(listed) if listed else -1
    moved = {}
    bad_w = bad_static = still_unlisted = 0
    for x in range(top + 1):
        w0, w1 = words(a, sbl + 0x40 * x, 16), words(b, sbl + 0x40 * x, 16)
        diff = [4 * o for o in range(16) if w0[o] != w1[o]]
        if x not in listed:
            still_unlisted += not diff
            continue
        for o in diff:
            moved[o] = moved.get(o, 0) + 1
        bad_w += any(o not in (0, 4, 8, 0x10, 0x14, 0x18) for o in diff)
        bad_static += bool(diff) and key(w0[0x24 // 4]) == 0
    unlisted = top + 1 - len(listed)
    check(bad_w == 0, f"{label} solver bodies: only deltas' x, y, z change", fmt(moved))
    check(bad_static == 0, f"{label} solver bodies: none of zero inverse mass changes", f"{bad_static} did")
    check(still_unlisted == unlisted, f"{label} solver bodies no cell lists: unchanged", f"{still_unlisted} of {unlisted}")
    return len(cells), contacts


def main(base):
    desc, snaps = None, {}
    for line in open(base + "_tasks.txt"):
        f = line.split()
        if f and f[0] == "task" and f[4] == "4" and desc is None:
            desc = [int(x, 16) for x in f[9:9 + 36]]
        elif f and f[0] == "snapshot":
            p = " ".join(f[1:])
            snaps[os.path.basename(p).rsplit("_", 1)[1][:-4]] = p
    if desc is None:
        sys.exit(f"{base}: no iterate task logged")
    order = sorted((k for k in snaps if k.startswith("c4r")), key=lambda k: int(k[3:]))
    if "c5r1" in snaps and order and order[-1] == "c4r10":
        order.append("c5r1")
    pairs = [(order[k], order[k + 1]) for k in range(len(order) - 1) if order[k + 1] == "c5r1" or
             int(order[k + 1][3:]) == int(order[k][3:]) + 1]
    print(f"{base}: rounds captured {', '.join(order)}")
    for x, y in pairs:
        round_pair(Snap(snaps[x]), Snap(snaps[y]), desc, f"{x}->{y}")
    print(f"{base}: {results.count('PASS')} PASS, {results.count('FAIL')} FAIL, {results.count('NOTE')} NOTE")
    return 1 if "FAIL" in results else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
