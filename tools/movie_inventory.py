#!/usr/bin/env python3
"""movie_inventory.py: M8.0's static inventory of the movies' code (docs/FFMPEG_MIGRATION_PLAN.md section 2): where
Bink's library lies in the game's executable and whether anything outside it reaches it except through its call
stubs -- the check M8.4's exclusion (config/excluded_code.json, traps) rests on, as M5's did for FMOD.

  movie_inventory.py <EBOOT.elf> <lift dir> [--lo 0x00A54DD0] [--hi 0x00A76208]

For the range [lo, hi) of lifted functions it reports:
  - the edges: the functions just outside it, and the C++ classes whose vtables hold them (RTTI names);
  - every direct call into the range from outside: it must come from a call stub (TOC-switching glue whose only
    callee is a member); the stubs and the game functions that call them (Bink's API as the game uses it);
  - every function descriptor of a member reached only through a pointer, and every reference to it: code
    materialising it (the function it is in) or a data table holding it -- and every reference to such a table;
  - the C runtime's constructor and destructor tables' entries pointing into the range;
  - the strings Bink's members materialise, and the functions outside that materialise Bink's strings;
  - a verdict: closed (nothing outside reaches the range but its stubs) or the references that break it.
Reads the player's executable and the developer's lift (game-derived, local); prints addresses, counts, class and
string names only.
"""
import argparse
import bisect
import collections
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import class_census_static as ccs  # noqa: E402

STUB_AREA = (0x00987000, 0x0098A000)   # the linker's TOC-switching call stubs (FMOD's: 0x00987478-0x00987658)
CTOR_TABLES = (0x00FE0000, 0x00FE0800)  # the C runtime's constructor table and the destructor table after it


def load_lift(lift):
    funcs, callers, callees = {}, collections.defaultdict(set), collections.defaultdict(set)
    for path in sorted(glob.glob(os.path.join(lift, "ppu_recomp_*.cpp"))):
        for m in ccs.FUNC_RE.finditer(open(path, encoding="utf-8", errors="replace").read()):
            f = int(m.group(1), 16)
            funcs[f] = os.path.basename(path)
            for c in set(ccs.CALL_RE.findall(m.group(2))):
                c = int(c, 16)
                if c != f:
                    callers[c].add(f)
                    callees[f].add(c)
    return funcs, callers, callees


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("lift")
    ap.add_argument("--lo", type=lambda s: int(s, 0), default=0x00A54DD0)
    ap.add_argument("--hi", type=lambda s: int(s, 0), default=0x00A76208)
    a = ap.parse_args()
    elf = ccs.Elf(a.elf)
    funcs, callers, callees = load_lift(a.lift)
    starts = sorted(funcs)
    lo, hi = a.lo, a.hi
    members = [f for f in starts if lo <= f < hi]
    member_set = set(members)
    inside = lambda x: lo <= x < hi  # noqa: E731
    data_segs = [(va, sz) for va, _o, sz, fl in elf.segs if not fl & 1]
    in_data = lambda x: any(va <= x < va + sz for va, sz in data_segs)  # noqa: E731
    tocs = set()
    for _f, s in ccs.toc_map(elf):
        tocs |= s

    def owner(x):
        i = bisect.bisect_right(starts, x) - 1
        return starts[i] if i >= 0 else None

    def cstr(va):
        o = elf.off(va)
        if o is None:
            return None
        e = elf.d.find(b"\0", o, o + 200)
        s = elf.d[o:e]
        return s.decode("latin-1") if e > o and all(32 <= c < 127 for c in s) else None

    def vtable_class(word):
        """The class whose vtable holds the descriptor pointer at `word` (walking back to the typeinfo slot)."""
        x = word
        for _ in range(80):
            x -= 4
            w = elf.w32(x)
            if w is None:
                return None
            if w == 0:
                continue
            if not in_data(w):
                return None
            n = cstr(elf.w32(w + 4) or 0)
            if n and re.match(r"^N?\d+[A-Za-z_]", n):
                return n
            if elf.w32(w + 4) not in tocs:
                return None
        return None

    def descriptors(f):
        return [d for d in elf.words_equal(f) if in_data(d) and elf.w32(d + 4) in tocs]

    problems = []
    print("range [0x%08X, 0x%08X): %d lifted functions, 0x%08X .. 0x%08X" % (lo, hi, len(members), members[0],
                                                                           members[-1]))

    # 1. the edges
    i0, i1 = starts.index(members[0]), starts.index(members[-1])
    for label, k in (("before", i0 - 1), ("after", i1 + 1)):
        f = starts[k]
        held = sorted({c for d in descriptors(f) for h in elf.words_equal(d) for c in [vtable_class(h)] if c})
        print("  just %s: func_%08X (callers %s)%s" % (label, f, " ".join("%08X" % c for c in sorted(callers.get(f, ())))
                                                       or "none", "; held by the vtable(s) of " + ", ".join(held)
                                                       if held else ""))

    # 2. direct calls into the range from outside: stubs only
    stubs = collections.defaultdict(set)   # stub -> its callers
    for f in members:
        for c in callers.get(f, ()):
            if inside(c):
                continue
            if STUB_AREA[0] <= c < STUB_AREA[1] and callees.get(c) == {f}:
                stubs[c] |= callers.get(c, set())
            else:
                problems.append("func_%08X calls member func_%08X directly" % (c, f))
    print("call stubs into the range: %d" % len(stubs))
    users = collections.defaultdict(list)
    for s in sorted(stubs):
        (t,) = callees[s]
        print("  stub 0x%08X -> func_%08X, called by %s" % (s, t, " ".join("func_%08X" % c for c in sorted(stubs[s]))))
        for c in stubs[s]:
            users[c].append(t)
    print("the game's functions calling Bink through them: %s" % " ".join("func_%08X" % u for u in sorted(users)))

    # 3. pointer-reached members: descriptors and every reference to them
    ptr = [f for f in members if not callers.get(f)]
    opds = {}
    for f in ptr:
        for d in descriptors(f):
            opds[d] = f
    code_refs = collections.defaultdict(list)
    for ia, v, _how in ccs.materialisations(elf, set(opds)):
        code_refs[v].append(owner(ia))
    tables = set()
    no_desc = 0
    for f in ptr:
        ds = descriptors(f)
        if not ds:
            no_desc += 1   # a pad before the next function, or code reached by falling through: nothing refers
            continue
        for d in ds:
            for o in code_refs.get(d, []):
                if not (o and inside(o)):
                    problems.append("func_%08X (outside) takes the address of member func_%08X" % (o or 0, f))
            for h in elf.words_equal(d):
                c = vtable_class(h)
                if c:
                    problems.append("member func_%08X is in the vtable of %s (at 0x%08X)" % (f, c, h))
                else:
                    tables.add(h)
    print("members reached only through a pointer: %d (%d with no descriptor: pads and fall-through entries)"
          % (len(ptr), no_desc))
    # the tables holding them: their extent (consecutive words that are descriptors, zero or small) and references
    spans = []
    for h in sorted(tables):
        s, e = h, h + 4
        ok = lambda w: w is not None and (w < 0x10000 or (in_data(w) and elf.w32(w + 4) in tocs))  # noqa: E731
        while ok(elf.w32(s - 4)) and h - s < 0x400:
            s -= 4
        while ok(elf.w32(e)) and e - h < 0x400:
            e += 4
        if not spans or s > spans[-1][1]:
            spans.append([s, e])
        else:
            spans[-1][1] = max(spans[-1][1], e)
    targets = {x for s, e in spans for x in range(s, e, 4)}
    trefs = collections.defaultdict(list)
    for ia, v, _how in ccs.materialisations(elf, targets):
        trefs[v].append(owner(ia))
    dref = {x: [w for w in elf.words_equal(x)] for x in targets}
    for s, e in spans:
        held = sorted({opds.get(elf.w32(x)) or (elf.w32(elf.w32(x) or 0) if elf.w32(x) and in_data(elf.w32(x)) else 0)
                       for x in range(s, e, 4)} - {0, None})
        code = sorted({o for x in range(s, e, 4) for o in trefs.get(x, [])})
        data = sorted({w for x in range(s, e, 4) for w in dref.get(x, [])})
        print("  table 0x%08X-0x%08X: %d words, its functions %s; referred to by code in %s; by data at %s" % (
            s, e, (e - s) // 4, " ".join("%08X" % f for f in held[:12]),
            " ".join("func_%08X%s" % (o or 0, "" if o and inside(o) else " (OUTSIDE)") for o in code) or "nothing",
            " ".join("0x%08X" % w for w in data) or "nothing"))
        for o in code:
            if not (o and inside(o)):
                problems.append("func_%08X (outside) refers to the table at 0x%08X" % (o or 0, s))
        for f in held:
            if f not in member_set and f in funcs:
                problems.append("the table at 0x%08X also holds func_%08X, outside the range" % (s, f))

    # 4. constructor and destructor tables
    ctors = []
    for x in range(CTOR_TABLES[0], CTOR_TABLES[1], 4):
        w = elf.w32(x)
        if w and in_data(w) and inside(elf.w32(w) or 0):
            ctors.append((x, elf.w32(w)))
    print("constructor/destructor table entries into the range: %d%s" % (
        len(ctors), "".join("\n  0x%08X -> func_%08X" % c for c in ctors)))

    # 5. strings
    strs = {}
    for m in re.finditer(rb"[\x20-\x7e]{6,}\x00", elf.d):
        va = elf.va(m.start())
        if va is not None:
            strs[va] = m.group()[:-1].decode("latin-1")
    bink_strings = {va for va, s in strs.items() if "Bink" in s or "BINK" in s}
    users_s = collections.defaultdict(set)
    for ia, v, _how in ccs.materialisations(elf, bink_strings):
        users_s[owner(ia)].add(strs[v])
    for o, ss in sorted(users_s.items(), key=lambda x: x[0] or 0):
        print("  func_%08X%s materialises %s" % (o or 0, "" if o and inside(o) else " (OUTSIDE)",
                                                 "; ".join(repr(s) for s in sorted(ss))))
        if not (o and inside(o)):
            problems.append("func_%08X (outside) uses Bink's strings" % (o or 0))

    print("verdict: %s" % ("closed -- nothing outside the range reaches it but its %d call stubs" % len(stubs)
                           if not problems else "NOT closed (%d):\n  " % len(problems) + "\n  ".join(problems[:40])))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
