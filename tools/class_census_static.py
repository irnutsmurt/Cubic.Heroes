#!/usr/bin/env python3
"""class_census_static.py: which C++ classes the game's executable can construct, read from its RTTI (M6's static
census of the physics' fallbacks, docs/OPENAL_MIGRATION_PLAN.md section 10.2). A scene that never makes a sphere or a
constraint shows what that scene does; this shows what the whole executable can do: for every class whose RTTI name
matches, its typeinfo and vtable, every instruction that materialises the vtable's address point (a lis/addi pair,
or a load from the TOC of a word holding it), the function each is in, and that function's callers up to a depth
(from the lift), each marked excluded (config/excluded_code.json) or lifted. A class whose vtable address is never
materialised outside its own constructors and destructors, and whose constructors nothing calls, cannot be
constructed by the game.

  class_census_static.py <EBOOT.elf> <lift dir> <regex> [--depth N] [--config DIR]

Reads the player's executable and the developer's lift (game-derived, local); prints class names (the RTTI's own)
and addresses only.
"""
import argparse
import bisect
import collections
import glob
import json
import os
import re
import struct

FUNC_RE = re.compile(r'^void (?:__attribute__\(\(weak\)\) )?func_([0-9A-F]{8})\(ppu_context\* ctx\) \{(.*?)^\}', re.S | re.M)
CALL_RE = re.compile(r'\bfunc_([0-9A-F]{8})\b')


class Elf:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        phoff = struct.unpack(">Q", d[0x20:0x28])[0]
        phentsize, phnum = struct.unpack(">HH", d[0x36:0x3A])
        self.segs = []   # (vaddr, file offset, file size, flags)
        for i in range(phnum):
            o = phoff + i * phentsize
            t, fl, off, va, _pa, fsz, _msz, _al = struct.unpack(">IIQQQQQQ", d[o:o + 56])
            if t == 1 and fsz:
                self.segs.append((va, off, fsz, fl))

    def off(self, va):
        for s_va, s_off, s_sz, _ in self.segs:
            if s_va <= va < s_va + s_sz:
                return s_off + va - s_va
        return None

    def va(self, off):
        for s_va, s_off, s_sz, _ in self.segs:
            if s_off <= off < s_off + s_sz:
                return s_va + off - s_off
        return None

    def w32(self, va):
        o = self.off(va)
        return None if o is None else struct.unpack(">I", self.d[o:o + 4])[0]

    def words_equal(self, value):
        """Every 4-aligned address in a loaded segment holding the word value."""
        pat = struct.pack(">I", value)
        out = []
        for s_va, s_off, s_sz, _ in self.segs:
            seg = self.d[s_off:s_off + s_sz]
            i = seg.find(pat)
            while i >= 0:
                if i % 4 == 0:
                    out.append(s_va + i)
                i = seg.find(pat, i + 1)
        return out

    def text(self):
        for s_va, s_off, s_sz, fl in self.segs:
            if fl & 1:   # executable
                yield s_va, self.d[s_off:s_off + s_sz]


def toc_map(elf):
    """The TOC each function runs with, from the function descriptors (OPDs: 8 bytes, the entry and its r2) in the
    data segments: a large executable has several TOCs, one per group of object files. Returns a sorted list of
    (function address, frozenset of TOCs): an address's TOC is its nearest described function's at or before it
    (a static function shares its group's), more than one where the linker gave a shared routine several."""
    text = [(va, va + sz) for va, _o, sz, fl in elf.segs if fl & 1]
    pairs = []
    count = collections.Counter()
    for va, off, sz, fl in elf.segs:
        if fl & 1:
            continue
        seg = elf.d[off:off + sz]
        for i in range(len(seg) // 8):
            a, t = struct.unpack(">II", seg[8 * i:8 * i + 8])
            if a % 4 == 0 and t % 4 == 0 and any(lo <= a < hi for lo, hi in text):
                pairs.append((a, t))
                count[t] += 1
    tocs = {t for t, n in count.items() if n >= 100}   # a real TOC describes many functions
    by_func = collections.defaultdict(set)
    for a, t in pairs:
        if t in tocs:
            by_func[a].add(t)
    return sorted((a, frozenset(s)) for a, s in by_func.items())


def materialisations(elf, targets):
    """Instructions that put one of targets (address points) in a register: lis rX,hi then addi/ori rY,rX,lo within
    16 instructions (rX not rewritten between, as a compiler schedules it), or lwz/ld rY,d(r2) of a word holding it
    in the TOC the instruction's function runs with (toc_map). Yields (instruction address, target, how)."""
    tmap = toc_map(elf)
    tstarts = [a for a, _ in tmap]
    holders = {}   # address of a word holding a target -> the target
    for t in targets:
        for a in elf.words_equal(t):
            holders[a] = t
    for base, code in elf.text():
        n = len(code) // 4
        ins = struct.unpack(">%dI" % n, code[:n * 4])
        lis = {}   # register -> (index, value << 16)
        for i, x in enumerate(ins):
            op = x >> 26
            rd, ra, imm = (x >> 21) & 31, (x >> 16) & 31, x & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            if op == 15 and ra == 0:                       # lis rd, imm
                lis[rd] = (i, (simm << 16) & 0xFFFFFFFF)
                continue
            if op in (14, 24) and ra in lis and i - lis[ra][0] <= 16:   # addi / ori rd, ra, imm
                v = (lis[ra][1] + simm) & 0xFFFFFFFF if op == 14 else lis[ra][1] | imm
                if v in targets:
                    yield base + 4 * i, v, "lis/%s" % ("addi" if op == 14 else "ori")
            if op in (32, 58) and ra == 2:                  # lwz / ld rd, d(r2)
                d = simm if op == 32 else (simm & ~3)
                k = bisect.bisect_right(tstarts, base + 4 * i) - 1
                for toc in (tmap[k][1] if k >= 0 else ()):
                    if toc + d in holders:
                        yield base + 4 * i, holders[toc + d], "toc %#x%+#x" % (toc, d)
            if op in (14, 15, 24, 25, 31, 32, 33, 34, 40, 42, 58) and rd in lis and rd != ra:
                lis.pop(rd, None)   # rd rewritten (approximately: the common integer forms)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("lift")
    ap.add_argument("regex", help="matched against the RTTI's mangled class names (for example 'bt.*Shape$')")
    ap.add_argument("--depth", type=int, default=2)
    ap.add_argument("--config", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "config"))
    a = ap.parse_args()
    elf = Elf(a.elf)
    rx = re.compile(a.regex)

    # RTTI names: "<len><name>\0" strings (Itanium ABI); the typeinfo's second word points to the name.
    classes = {}   # name -> {"name_va", "typeinfo", "vtables": [address point]}
    for m in re.finditer(rb"(?<![0-9A-Za-z_])([1-9][0-9]?)([A-Za-z_][A-Za-z0-9_]*)\x00", elf.d):
        ln, nm = int(m.group(1)), m.group(2)
        if len(nm) != ln:
            continue
        name = nm.decode()
        if not rx.search(name):
            continue
        name_va = elf.va(m.start())
        if name_va is None:
            continue
        for ref in elf.words_equal(name_va):
            ti = ref - 4
            vts = [v + 4 for v in elf.words_equal(ti)]   # the vtable's typeinfo slot; the address point follows it
            classes.setdefault(name, {"name_va": name_va, "typeinfo": [], "vtables": []})
            classes[name]["typeinfo"].append(ti)
            for v in vts:
                if elf.w32(v - 8) == 0:   # offset-to-top 0: a primary vtable
                    classes[name]["vtables"].append(v)

    # The lift: functions, their bodies' extent, the call graph.
    funcs = {}
    callers = collections.defaultdict(set)
    for path in sorted(glob.glob(os.path.join(a.lift, "ppu_recomp_*.cpp"))):
        for m in FUNC_RE.finditer(open(path, encoding="utf-8", errors="replace").read()):
            f = int(m.group(1), 16)
            funcs[f] = path
            for c in set(CALL_RE.findall(m.group(2))):
                c = int(c, 16)
                if c != f:
                    callers[c].add(f)
    starts = sorted(funcs)
    excl = json.load(open(os.path.join(a.config, "excluded_code.json")))
    ranges = [(int(t["lo"], 0), int(t["hi"], 0), t["what"]) for t in excl.get("traps", [])]

    def owner(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return starts[i] if i >= 0 else None

    def mark(f):
        for lo, hi, what in ranges:
            if lo <= f < hi:
                return " [excluded: %s]" % what
        return ""

    points = {}
    for name, c in classes.items():
        for v in c["vtables"]:
            points[v] = name
    sites = collections.defaultdict(list)
    for ia, v, how in materialisations(elf, set(points)):
        sites[points[v]].append((ia, v, how))

    for name in sorted(classes):
        c = classes[name]
        print("%s: typeinfo %s, vtable address point(s) %s" % (
            name, ", ".join("0x%08X" % t for t in c["typeinfo"]) or "-",
            ", ".join("0x%08X" % v for v in c["vtables"]) or "none (no vtable: never constructed)"))
        if not c["vtables"]:
            continue
        if not sites[name]:
            print("  no instruction materialises it: never constructed")
            continue
        by_func = collections.OrderedDict()
        for ia, v, how in sorted(sites[name]):
            by_func.setdefault(owner(ia), []).append((ia, how))
        for f, refs in by_func.items():
            print("  in func_%08X%s: %s" % (f or 0, mark(f or 0), ", ".join("0x%08X (%s)" % r for r in refs)))
            frontier, seen = {f}, {f}
            for depth in range(1, a.depth + 1):
                nxt = set()
                for g in frontier:
                    nxt |= callers.get(g, set())
                nxt -= seen
                if not nxt:
                    if depth == 1:
                        print("    called by nothing in the lift (reached only through a pointer, or never)")
                    break
                seen |= nxt
                print("    callers at depth %d: %s" % (depth, ", ".join("func_%08X%s" % (g, mark(g)) for g in sorted(nxt)[:24])
                                                    + (" ... (%d)" % len(nxt) if len(nxt) > 24 else "")))
                frontier = nxt


if __name__ == "__main__":
    main()
