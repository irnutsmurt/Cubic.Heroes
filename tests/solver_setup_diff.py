#!/usr/bin/env python3
"""What the solver's command 1 (body set-up) changes, from two DOTHEROES_SOLVER_SNAP snapshots of one step: the one taken
as its first task was created (c1r1) and the one taken as command 2's first was (c2r1). Reads the command-1 task's
descriptor from <tasks.txt>: +0x14 the solver bodies (0x40 each), +0x30 the first body, +0x34 the count, +0x38 the list
of the game's btRigidBody pointers.

  tests/solver_setup_diff.py <c1r1.bin> <c2r1.bin> <tasks.txt> [--bodies N]

Prints, for each word of a solver body and of a rigid body, how many bodies it changed in and examples, so that what
the command writes can be told from what the PPU wrote meanwhile (those change outside these structures, or not in
every body).
"""
import collections
import struct
import sys


class Space:
    def __init__(self, path):
        self.ranges = []   # (ea, bytes)
        with open(path, "rb") as f:
            data = f.read()
        o = 0
        while o + 8 <= len(data):
            ea, n = struct.unpack_from("<II", data, o)
            o += 8
            self.ranges.append((ea, data[o:o + n]))
            o += n

    def read(self, ea, n):
        for a, b in self.ranges:
            if a <= ea and ea + n <= a + len(b):
                return b[ea - a:ea - a + n]
        return None

    def u32(self, ea):
        b = self.read(ea, 4)
        return struct.unpack(">I", b)[0] if b else None


def main():
    a, b = Space(sys.argv[1]), Space(sys.argv[2])
    desc = None
    for ln in open(sys.argv[3]):
        x = ln.split()
        if len(x) > 8 and x[0] == "task" and x[4] == "1":
            words = [int(w, 16) for w in x[9:9 + 36]]
            desc = words
            break
    if not desc:
        sys.exit("no command-1 task in the list")
    bodies, first, count, lst = desc[5], desc[12], desc[13], desc[14]
    print("command 1: solver bodies %08X, first %u, count %u, list %08X" % (bodies, first, count, lst))
    sb_changes = collections.defaultdict(list)
    rb_changes = collections.defaultdict(list)
    for i in range(first, first + count):
        rb = a.u32(lst + 4 * i)
        sa, sb = a.read(bodies + 0x40 * i, 0x40), b.read(bodies + 0x40 * i, 0x40)
        if sa is None or sb is None:
            print("solver body %d not in the snapshots" % i)
            continue
        for w in range(16):
            x, y = sa[4 * w:4 * w + 4], sb[4 * w:4 * w + 4]
            sb_changes[w * 4].append((i, x.hex(), y.hex()))
        ra, rbb = a.read(rb, 0x200), b.read(rb, 0x200)
        if ra and rbb:
            for w in range(0x80):
                if ra[4 * w:4 * w + 4] != rbb[4 * w:4 * w + 4]:
                    rb_changes[w * 4].append((i, rb, ra[4 * w:4 * w + 4].hex(), rbb[4 * w:4 * w + 4].hex()))
    print("solver body words after the command (offset: distinct values, changed in n bodies, examples before->after):")
    for off in sorted(sb_changes):
        v = sb_changes[off]
        changed = [e for e in v if e[1] != e[2]]
        print("  +%02X: %d distinct after, changed in %d of %d; e.g. %s" % (
            off, len(set(e[2] for e in v)), len(changed), len(v), ", ".join("%d:%s->%s" % e for e in v[:3])))
    print("rigid body words that changed (offset: in n bodies, examples):")
    for off in sorted(rb_changes):
        v = rb_changes[off]
        print("  +%03X: %d bodies; e.g. %s" % (off, len(v), ", ".join("%d %08X %s->%s" % e for e in v[:3])))


if __name__ == "__main__":
    main()
