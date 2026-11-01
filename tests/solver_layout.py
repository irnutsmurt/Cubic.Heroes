#!/usr/bin/env python3
"""Check the game's Bullet solver structures against a captured step, before any
native code relies on them (Bullet 2.74 is only a provisional reference: the game
is built on 2.73).

  tests/solver_layout.py <prefix><step>    e.g. build/snap/sv_150

Reads <prefix><step>_tasks.txt and the snapshots DOTHEROES_SOLVER_SNAP wrote
(port/native/bullet_solver_task.cpp). Checks, each against the data:
  descriptor  per command, which fields are set and to what (2.74's
              SpuSolverTaskDesc: +0 command, +4 task id, +0x10 hash, +0x14 solver
              bodies, +0x18 contact constraints, +0x1C other constraints, +0x20
              body offsets, +0x30 the command's data)
  hash        the cells (8 halfwords: local bodies, body offset list offset,
              manifolds, manifold list offset, contacts, contact constraint offset,
              constraints, constraint offset) by the PPU's own running sums; the
              dependency matrix (symmetric); the processed-cell mask preset with the
              cells that have nothing to do, the task rows clear
  constraints the record size and the offsets of the applied impulse and of the
              pointer back to the manifold point, found from what the first
              write-back pass changed: the record layout that names exactly the
              changed floats, with min(3, applied impulse) written to each
  write-back  the 16 bytes at each pointer: only the first float changes; passes
              2-10 leave what pass 1 wrote; the inputs unchanged meanwhile
  lock        the spin lock the descriptor names, as the PPU leaves it per round
Prints each check PASS / FAIL / NOTE with the numbers behind it.
"""
import collections
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from snap_read import Snap

NCELLS, NDW, MAXSPU = 128, 4, 6
results = []


def check(ok, what, detail=""):
    tag = "PASS" if ok is True else "FAIL" if ok is False else "NOTE"
    results.append(tag)
    print(f"[{tag}] {what}" + (f": {detail}" if detail else ""))


def f32(u):
    return struct.unpack(">f", struct.pack(">I", u))[0]


def main(base):
    tasks, snaps = [], {}
    for line in open(base + "_tasks.txt"):
        f = line.split()
        if f and f[0] == "task":
            words = [int(x, 16) for x in f[9:9 + 0x90 // 4]]
            tasks.append(dict(slot=int(f[2]), cmd=int(f[4]), round=int(f[6]), desc=int(f[8].rstrip(":"), 16), words=words))
        elif f and f[0] == "snapshot":
            p = " ".join(f[1:])
            label = os.path.basename(p).rsplit("_", 1)[1][:-4]
            snaps[label] = p
    print(f"{base}: {len(tasks)} solver tasks, snapshots {sorted(snaps)}")
    S = {k: Snap(v) for k, v in snaps.items()}

    # --- descriptors
    by_cmd = collections.defaultdict(list)
    for t in tasks:
        by_cmd[t["cmd"]].append(t)
    counts = {c: len(v) for c, v in sorted(by_cmd.items())}
    rounds = {c: max(t["round"] for t in v) for c, v in sorted(by_cmd.items())}
    check(set(counts) == {1, 2, 4, 5, 6} and rounds[4] == 10 and rounds[6] == 10 and counts[4] == 30 and counts[6] == 30,
          "commands in a step: 1, 2, 4 x10 rounds of 3, 5, 6 x10 rounds of 3", f"tasks {counts}, rounds {rounds}")
    w = lambda t, off: t["words"][off // 4]
    hashes = {w(t, 0x10) for t in tasks}
    check(len(hashes) == 1, "one hash for the step at +0x10", f"{[hex(h) for h in hashes]}")
    h = hashes.pop()
    check(all(w(t, 4) == t["slot"] for t in tasks), "+4 task id = the slot")
    il = {w(t, 0x18) for t in by_cmd[6]} | {w(t, 0x18) for t in by_cmd[2]} | {w(t, 0x18) for t in by_cmd[4]}
    check(len(il) == 1, "one contact constraint list at +0x18 for manifold setup, iterate, write-back", f"{[hex(x) for x in il]}")
    ilist = il.pop()
    locks6 = sorted({w(t, 0x30) for t in by_cmd[6]})
    locks4 = sorted({w(t, 0x30) for t in by_cmd[4]})
    check(len(locks6) == 1 and len(locks4) == 1 and locks6[0] % 128 == 0 and locks4[0] % 128 == 0,
          "iterate and write-back: one spin lock each at +0x30, 128-aligned", f"iterate {[hex(x) for x in locks4]}, write-back {[hex(x) for x in locks6]}")
    print("  descriptor words set, by command (offset: value):")
    for c, v in sorted(by_cmd.items()):
        t = v[0]
        print(f"    {c}: " + ", ".join(f"+{o * 4:02X}={x:08X}" for o, x in enumerate(t["words"]) if x and o >= 2))

    # --- hash
    s = S["c6r1"]
    cells = []
    for i in range(NCELLS):
        b = s.bytes(h + 0x870 + 16 * i, 16)
        cells.append(struct.unpack(">8H", b))
    nb, bo, nm, mo, nc, co, nk, ko = zip(*cells)
    run_m = run_c = run_k = 0
    ok_m = ok_c = ok_k = True
    for i in range(NCELLS):
        if nm[i]:
            ok_m &= mo[i] == run_m; run_m += nm[i]
        if nc[i]:
            ok_c &= co[i] == run_c * 3; run_c += nc[i]
        if nk[i]:
            ok_k &= ko[i] == run_k; run_k += nk[i]
    busy = [i for i in range(NCELLS) if nc[i] or nk[i]]
    check(ok_m and ok_c and ok_k, "cells at hash +0x870, 16 bytes: manifold, contact-constraint (x3) and constraint offsets are the PPU's running sums",
          f"{len(busy)} cells in use, {sum(nm)} manifolds, {sum(nc)} contacts, {sum(nk)} constraints, {sum(nb)} local bodies")
    # body offsets: (manifolds + constraints) * 2 each, rounded up to 4
    run_b, ok_b = 0, True
    for i in range(NCELLS):
        ok_b &= bo[i] == run_b
        run_b += nm[i] * 2 + nk[i] * 2
        run_b = (run_b + 3) & ~3
    check(ok_b, "body offset list offsets: (manifolds + constraints) x 2 a cell, rounded up to 4")
    dep = [[s.u32(h + 16 * i + 4 * k) for k in range(NDW)] for i in range(NCELLS)]
    bit = lambda m, j: (m[j >> 5] >> (j & 31)) & 1
    sym = all(bit(dep[i], j) == bit(dep[j], i) for i in range(NCELLS) for j in range(NCELLS))
    nset = sum(bit(dep[i], j) for i in range(NCELLS) for j in range(NCELLS))
    check(sym and nset > 0, "dependency matrix at hash +0, 128 x 4 words, symmetric", f"{nset} bits set, diagonal {sum(bit(dep[i], i) for i in range(NCELLS))}")
    for label in ("c4r1", "c6r1", "c6r2"):
        m = [[S[label].u32(h + 0x800 + 16 * r + 4 * k) for k in range(NDW)] for r in range(MAXSPU + 1)]
        empty = [i for i in range(NCELLS) if not nc[i] and not nk[i]]
        want = [0] * NDW
        for i in empty:
            want[i >> 5] |= 1 << (i & 31)
        check(m[0] == want and not any(any(r) for r in m[1:]),
              f"{label}: processed-cell mask at hash +0x800 = the cells with nothing to do; task rows +0x810.. clear",
              f"mask {' '.join(f'{x:08X}' for x in m[0])}")

    # --- constraint layout, from what the first write-back pass changed
    a, b = S["c6r1"], S["c6r2"]
    contacts = [(i, j) for i in range(NCELLS) for j in range(nc[i])]
    best = None
    for size in (0x80, 0x90, 0xA0, 0xB0):
        for poff in range(0, size, 4):
            ptrs = []
            for i, j in contacts[:400]:
                ea = ilist + (co[i] + 3 * j) * size + poff
                ptrs.append(a.u32(ea) if a.valid(ea) else 0)
            good = sum(1 for p in ptrs if p and a.valid(p, 16))
            if not ptrs or good < 0.9 * len(ptrs):
                continue
            changed = sum(1 for p in ptrs if p and a.valid(p, 16) and a.u32(p) != b.u32(p))
            for ioff in range(0, size, 4):
                if ioff == poff:
                    continue
                match = 0
                for k, (i, j) in enumerate(contacts[:400]):
                    p = ptrs[k]
                    if not p or not a.valid(p, 16):
                        continue
                    imp = f32(a.u32(ilist + (co[i] + 3 * j) * size + ioff))
                    want = struct.pack(">f", min(3.0, imp))
                    match += b.bytes(p, 4) == want
                if best is None or match > best[0]:
                    best = (match, size, poff, ioff, changed, len(ptrs))
    match, size, poff, ioff, changed, n = best
    check(match == n, "contact constraint: the record whose pointer field names each written float, and whose impulse field gives it",
          f"record 0x{size:X} bytes, pointer at +0x{poff:X}, applied impulse at +0x{ioff:X}: {match} of {n} contacts written "
          f"min(3, impulse) ({changed} changed value); 2.74 has 0x90, +0x70, +0x54")

    # every contact, all three snapshots
    ptr = lambda snap, i, j: snap.u32(ilist + (co[i] + 3 * j) * size + poff)
    imp = lambda snap, i, j: snap.bytes(ilist + (co[i] + 3 * j) * size + ioff, 4)
    e = S["end"]
    same_inputs = all(ptr(a, i, j) == ptr(b, i, j) == ptr(e, i, j) and imp(a, i, j) == imp(b, i, j) == imp(e, i, j)
                      for i, j in contacts)
    check(same_inputs, "write-back inputs (pointer, applied impulse) the same before pass 1, before pass 2 and after the step",
          f"{len(contacts)} contacts")
    first_ok = rest_same = pass_same = 0
    targets = set()
    for i, j in contacts:
        p = ptr(a, i, j)
        targets.add(p)
        want = struct.pack(">f", min(3.0, f32(struct.unpack(">I", imp(a, i, j))[0])))
        first_ok += b.bytes(p, 4) == want
        rest_same += a.bytes(p + 4, 12) == b.bytes(p + 4, 12)
        pass_same += b.bytes(p, 16) == e.bytes(p, 16)
    check(first_ok == len(contacts), "pass 1 writes min(3, applied impulse) to each pointer's first float", f"{first_ok} of {len(contacts)}")
    check(rest_same == len(contacts), "... and leaves the other 12 of its 16 bytes as they were", f"{rest_same} of {len(contacts)}")
    check(len(targets) == len(contacts), "every contact names its own 16 bytes (no two passes' writes overlap)",
          f"{len(targets)} distinct of {len(contacts)}")
    check(True if pass_same == len(contacts) else None,
          "at the step's end the 16 bytes are as pass 1 left them (the end snapshot is taken at the next collision task: "
          "it includes the PPU's work after the solver, where a manifold made again for a new pair starts its points at zero)",
          f"{pass_same} of {len(contacts)}")
    # the friction rows: their pointer field
    fr = sum(1 for i, j in contacts for r in (1, 2) if a.u32(ilist + (co[i] + 3 * j + r) * size + poff) == 0)
    check(fr == 2 * len(contacts), "friction rows (2nd, 3rd of each contact) have no pointer", f"{fr} of {2 * len(contacts)}")

    # --- the spin lock and the masks while tasks run (the "start"/"end" lines: as each iterate or
    # write-back task starts and returns; the lock itself is freed and reused by the step's end)
    l0 = S["c6r1"].bytes(locks6[0], 4)
    check(l0 == bytes(4), "spin lock zero as write-back round 1 is created (the PPU's init)", l0.hex())
    states = []
    for line in open(base + "_tasks.txt"):
        f = line.split()
        if f and f[0] in ("start", "end"):
            lockw = int(f[7], 16)
            masks = [int(x, 16) for x in f[10:10 + 28]]
            states.append(dict(when=f[0], slot=int(f[2]), cmd=int(f[4]), rel=lockw >> 16, acq=lockw & 0xFFFF,
                               m0=masks[0:4], rows=[masks[4 * r:4 * r + 4] for r in range(1, MAXSPU + 1)]))
    if not states:
        check(None, "no task states in the log (captured before they were recorded)")
    else:
        ok_order = all(0 <= (s_["acq"] - s_["rel"]) % 65536 <= 3 for s_ in states)
        check(ok_order, "spin lock as tasks run: two 16-bit counters, +0 now serving <= +2 next ticket, at most 3 apart",
              f"{len(states)} readings, {sum(s_['rel'] != s_['acq'] for s_ in states)} held")
        rows_ok = all(not any(r) for s_ in states for k, r in enumerate(s_["rows"]) if k > 2)
        own = collections.Counter((s_["slot"], k) for s_ in states for k, r in enumerate(s_["rows"]) if any(r))
        check(rows_ok, "task rows used: 1-3 only (row 1 + task id), rows 4-6 never", f"rows seen held: {sorted(own)}")
        full = [s_ for s_ in states if s_["m0"] == [0xFFFFFFFF] * 4]
        done = [s_ for s_ in full if not any(any(r) for r in s_["rows"])]
        released = [s_ for s_ in done if s_["rel"] == s_["acq"]]
        check(len(released) > 0, "a round's end state seen: every cell processed, every row clear, the lock released",
              f"{len(released)} readings; {len(done) - len(released)} more with every cell processed and every row clear but "
              f"the lock held -- another task of the round between taking its ticket and finding nothing left; "
              f"{len(full) - len(done)} with a task still on its last cell")
    print(f"== {results.count('PASS')} PASS, {results.count('FAIL')} FAIL, {results.count('NOTE')} NOTE")
    return 0 if "FAIL" not in results else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
