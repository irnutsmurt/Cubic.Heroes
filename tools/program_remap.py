#!/usr/bin/env python3
"""Another program of the same game (the official v1.01 update's): v1.00's address-tied configuration moved to it.

The game's code is lifted from its program, and much of this project is tied to the program's addresses: the
renderer's, physics', audio's and movies' entry hooks and observers, the instruction patches, the excluded code, setjmp
and longjmp, and port/'s own references to the game's functions and data. Another program of the game has most
functions unchanged but moved, a few relocated (the same code, other data addresses) and a few changed. This tool:

1. matches v1.00's functions to the other program's by content -- each function's words with the displacement of
   its relative branches masked (b/bl, bc), so a function that only moved, and calls functions that moved, matches;
   among equal candidates the nearest; then, for each one left, the function where the shift of the nearest matched
   function before it puts it, when it has the same length and differs only in address immediates (addis, addi, ori,
   and the loads and stores' 16-bit offsets): relocated;
2. maps an address: inside a matched (or relocated) function, the same offset in its counterpart; in the code
   segment between functions, the shift of the nearest matched function before it when the bytes agree; in the data
   segment, the same address when the two programs' data agree there (32 bytes, a code pointer counting as agreeing
   when it points at its counterpart); anything else unresolved;
3. writes config/programs/<region>-<version>/ (its program.json names the program id): each address-tied file of config/ with its addresses mapped -- observers keep their
   v1.00 address as their "id" (the lifter passes it to the port, so the port's dispatch stays as it is) -- and, for
   port/'s own code, port_aliases.h (the lifted functions port/ names, v1.00's name to this program's) and
   port_addrs.inc (each address port/ writes down, v1.00's to this program's, for PA() in port/program_addrs.h);
4. stops on anything unresolved, unless the program folder's remap_manual.json decides it (an address reviewed by
   hand: its counterpart, or null when it no longer applies, with a note).

Reads only the two decrypted programs and their function lists on this machine; writes addresses, never code bytes.

    python tools/program_remap.py V100.elf V100.functions.json OTHER.elf OTHER.functions.json PROGRAM_ID [--out DIR]
        [--report FILE]
"""
import argparse
import bisect
import collections
import glob
import hashlib
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# the configuration tied to the program's addresses (config/<name>); each is written for the other program
CONFIG_FILES = ["audio_hooks.json", "excluded_code.json", "insn_patches.json", "m8_native_movie.json",
                "r7_native_hooks.json", "r8_hooks.json", "sjlj.json", "r6_native_observers.json",
                "dev_capture_observers.json", "m8_movie_trace.json", "m8_dev_credits.json", "r8_promote.json",
                "spu_entry_hooks.json", "spu_relocatable.json"]
# observers: the lifter passes the entry's "id" (when given) to the port in place of its address
OBSERVER_FILES = {"r6_native_observers.json", "r7_native_hooks.json", "dev_capture_observers.json",
                  "m8_movie_trace.json"}
_HEX = re.compile(r"^0x[0-9A-Fa-f]{1,8}$")
# an SPU image embedded in the program, named by its index and where it lies (its local-store addresses do not move)
_SPU_IMAGE = re.compile(r"\bspu_([0-9]{4})_at_([0-9A-F]{8})\b")
SMALL = 6   # words: a function this short (a trampoline: a TOC adjustment and a branch) is matched by its callers
PORT_FUNC = re.compile(r"\bfunc_([0-9A-F]{8})\b")
PORT_ADDR = re.compile(r"\bPA(?:_RT)?\((0x[0-9A-Fa-f]{1,8})u?\)")


class Program:
    """a 64-bit big-endian PPU ELF: its loadable segments, its functions [(start, end)], their normalised hashes"""

    def __init__(self, elf, functions):
        with open(elf, "rb") as f:
            self.d = f.read()
        if self.d[:4] != b"\x7fELF" or self.d[4] != 2 or self.d[5] != 2:
            raise SystemExit(f"{elf}: not a 64-bit big-endian ELF (a decrypted PPU program)")
        phoff, = struct.unpack(">Q", self.d[0x20:0x28])
        es, num = struct.unpack(">HH", self.d[0x36:0x3A])
        self.segs = []
        for i in range(num):
            o = phoff + i * es
            typ, flags, off, vaddr, _paddr, filesz, memsz = struct.unpack(">IIQQQQQ", self.d[o:o + 0x30])
            if typ == 1 and memsz:
                self.segs.append((vaddr, off, filesz, memsz, flags))
        self.code = [(v, v + m) for v, o, f, m, fl in self.segs if fl & 1]
        self.data = [(v, v + m) for v, o, f, m, fl in self.segs if not fl & 1]
        with open(functions) as f:
            j = json.load(f)
        j = j if isinstance(j, list) else j.get("functions", j)
        fs = set()
        for f in j:
            s = int(f["start"], 16) if isinstance(f["start"], str) else int(f["start"])
            e = int(f["end"], 16) if isinstance(f["end"], str) else int(f["end"])
            if e > s:
                fs.add((s, e))
        self.funcs = sorted(fs)
        self.starts = [s for s, _ in self.funcs]
        self.end = dict(self.funcs)

    def read(self, va, n):
        for v, o, f, m, fl in self.segs:
            if v <= va and va + n <= v + f:
                return self.d[o + va - v: o + va - v + n]
        return None

    def words(self, s, e):
        raw = self.read(s, e - s)
        return struct.unpack(f">{len(raw) // 4}I", raw[:len(raw) // 4 * 4]) if raw is not None else None

    def func_of(self, va):
        k = bisect.bisect_right(self.starts, va) - 1
        return self.starts[k] if k >= 0 and va < self.end[self.starts[k]] else None

    def in_code(self, va):
        return any(a <= va < b for a, b in self.code)

    def in_data(self, va):
        return any(a <= va < b for a, b in self.data)


def norm_hash(ws):
    out = []
    for w in ws:
        op = w >> 26
        if op == 18:
            w &= 0xFC000003          # b, bl: the displacement masked
        elif op == 16:
            w &= 0xFFFF0003          # bc: the displacement masked
        out.append(w)
    return hashlib.sha1(struct.pack(f">{len(out)}I", *out)).hexdigest()


# D-form instructions whose low 16 bits are an address part: addi, addis, ori, oris, and the loads and stores
_IMM_OPS = {14, 15, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53,
            54, 55, 58, 62}


def relocated_only(wa, wb):
    """the same instruction stream but for address immediates (and branch displacements): the same opcodes, registers
    and forms, only the 16-bit address parts of addis, addi, ori and the loads and stores differing"""
    if len(wa) != len(wb):
        return False
    for a, b in zip(wa, wb):
        if a == b:
            continue
        op = a >> 26
        if op != b >> 26:
            return False
        if op in (16, 18):
            ma = 0xFFFF0003 if op == 16 else 0xFC000003
            if a & ma != b & ma:
                return False
            continue
        if op not in _IMM_OPS or (a ^ b) & 0xFFFF0000:
            return False
    return True


def _shift_at(m, ms, s, before=True):
    """the shift of the nearest matched function before (or after) s"""
    k = bisect.bisect_left(ms, s) - 1 if before else bisect.bisect_right(ms, s)
    if 0 <= k < len(ms):
        return m[ms[k]][0] - ms[k]
    return None


def match(A, B):
    """{v1.00 function start: (other start, how)} -- how: identical, moved, relocated. Functions whose content (the
    displacement of relative branches masked) occurs once in each program are matched first: they set each region's
    shift. A function whose content occurs more than once (short stubs differing only in their branch target) takes
    the candidate the shift of its matched neighbours points at, else the nearest. One left over: the function the
    shift before or after it points at, when the two differ only in address immediates (relocated)."""
    hb, wb = collections.defaultdict(list), {}
    for s, e in B.funcs:
        w = B.words(s, e)
        if w is not None:
            wb[s] = w
            hb[norm_hash(w)].append(s)
    ha, wa = collections.defaultdict(list), {}
    for s, e in A.funcs:
        w = A.words(s, e)
        if w is not None:
            wa[s] = w
            ha[norm_hash(w)].append(s)
    out = {}

    def how(s, t):
        return "identical" if t == s and wb[t] == wa[s] else "moved"
    small = {s for s in wa if len(wa[s]) <= SMALL}   # trampolines: decided by their callers only
    for h, ss in ha.items():   # unique on both sides
        if len(ss) == 1 and len(hb.get(h, ())) == 1 and ss[0] not in small:
            out[ss[0]] = (hb[h][0], how(ss[0], hb[h][0]))
    taken = {t for t, _ in out.values()}
    # repeated content (short stubs differing only in their branch target): the calls decide -- a matched function
    # calling X in v1.00 whose counterpart calls X' votes for X -> X'; votes from each newly matched function too,
    # until nothing changes; what is left, the neighbours' shift
    rep = {s: h for h, ss in ha.items() for s in ss if s not in out}
    by_h = collections.defaultdict(set)
    for h in set(rep.values()):
        by_h[h] = {c for c in hb.get(h, ()) if c not in taken}
    # a trampoline's counterpart may have other immediates (a module's TOC moved): any small function of its length
    small_b = collections.defaultdict(set)
    for t in wb:
        if len(wb[t]) <= SMALL and t not in taken:
            small_b[len(wb[t])].add(t)

    def cands_of(s):
        return small_b[len(wa[s])] if s in small else by_h[rep[s]]
    votes = collections.defaultdict(collections.Counter)

    def vote_from(s, t):
        a_, b_ = wa[s], wb[t]
        if len(a_) != len(b_):
            return
        for i, (x, y) in enumerate(zip(a_, b_)):
            if x >> 26 != 18 or x & 2 or y >> 26 != 18:
                continue
            dx, dy = x & 0x03FFFFFC, y & 0x03FFFFFC
            tx = (s + 4 * i + (dx - 0x04000000 if dx & 0x02000000 else dx)) & 0xFFFFFFFF
            ty = (t + 4 * i + (dy - 0x04000000 if dy & 0x02000000 else dy)) & 0xFFFFFFFF
            if tx in rep and tx not in out:
                votes[tx][ty] += 1
    # a trampoline first by where it goes: the small function of its shape whose branch goes to the counterpart of
    # where v1.00's goes (its other words differing in address immediates only: a TOC adjustment that moved)
    def branch_target(base, ws):
        for i, w in enumerate(ws):
            if w >> 26 == 18 and not w & 2:
                d = w & 0x03FFFFFC
                return (base + 4 * i + (d - 0x04000000 if d & 0x02000000 else d)) & 0xFFFFFFFF
        return None
    by_target = collections.defaultdict(list)
    for t in wb:
        if len(wb[t]) <= SMALL and t not in taken:
            bt = branch_target(t, wb[t])
            if bt is not None:
                by_target[(len(wb[t]), bt)].append(t)
    for _round in range(3):   # a trampoline to a trampoline: its target matched in the round before
        for s in sorted(small):
            if s in out:
                continue
            ta_ = branch_target(s, wa[s])
            if ta_ is None or ta_ not in out:
                continue
            cs = [c for c in by_target.get((len(wa[s]), out[ta_][0]), ()) if c not in taken
                  and relocated_only(wa[s], wb[c])]
            if len(cs) == 1 or (cs and _round == 2):
                c = min(cs, key=lambda c: abs(c - s))
                out[s] = (c, how(s, c) if norm_hash(wa[s]) == norm_hash(wb[c]) else "relocated")
                taken.add(c)
    for s, (t, _h) in list(out.items()):
        vote_from(s, t)
    changed = True
    while changed:
        changed = False
        for s in sorted(votes):
            if s in out:
                continue
            for t, _n in votes[s].most_common():
                if t in cands_of(s) and t not in taken and (s not in small or relocated_only(wa[s], wb[t])):
                    out[s] = (t, how(s, t) if norm_hash(wa[s]) == norm_hash(wb[t]) else "relocated")
                    taken.add(t)
                    cands_of(s).discard(t)
                    vote_from(s, t)
                    changed = True
                    break
    ms = sorted(out)
    for s in sorted(rep):
        if s in out:
            continue
        cands = [c for c in by_h[rep[s]] if c not in taken]   # no votes: the same content, the neighbours' shift
        if not cands:
            continue
        sh = _shift_at(out, ms, s)
        want = s + (sh or 0)
        t = want if want in cands else min(cands, key=lambda c: abs(c - want))
        out[s] = (t, how(s, t))
        taken.add(t)
        by_h[rep[s]].discard(t)
    ms = sorted(out)
    for s, e in A.funcs:   # relocated: the shift before, then the shift after
        if s in out or s not in wa:
            continue
        for before in (True, False):
            sh = _shift_at(out, ms, s, before)
            t = s + sh if sh is not None else None
            if t in wb and t not in taken and relocated_only(wa[s], wb[t]):
                out[s] = (t, "relocated")
                taken.add(t)
                break
    return out


def branch_check(A, B, m):
    """the matched functions' calls and branches to other functions' entries: those whose target maps to another
    function than the counterpart's target (a mismatch somewhere) -> [(v1.00 function, offset, target, its map, the
    counterpart's target)]"""
    starts = set(A.starts)
    bad = []
    for s, (t, _how) in m.items():
        wa, wb = A.words(s, A.end[s]), B.words(t, B.end[t])
        if not wa or not wb or len(wa) != len(wb):
            continue
        for i, (a, b) in enumerate(zip(wa, wb)):
            if a >> 26 != 18 or a & 2:   # b/bl, relative
                continue
            da = a & 0x03FFFFFC
            db = b & 0x03FFFFFC
            ta = (s + 4 * i + (da - 0x04000000 if da & 0x02000000 else da)) & 0xFFFFFFFF
            tb = (t + 4 * i + (db - 0x04000000 if db & 0x02000000 else db)) & 0xFFFFFFFF
            if ta in starts and ta in m and m[ta][0] != tb:
                bad.append((s, 4 * i, ta, m[ta][0], tb))
    return bad


def callers_agree(A, B, m, x, tx):
    """the calls of v1.00's function x from matched functions: (agree, disagree) -- whether each caller's counterpart
    calls tx at the same place"""
    ok = bad = 0
    for s, (t, _how) in m.items():
        wa, wb = A.words(s, A.end[s]), B.words(t, B.end[t])
        if not wa or not wb or len(wa) != len(wb):
            continue
        for i, (a, b) in enumerate(zip(wa, wb)):
            if a >> 26 != 18 or a & 2:
                continue
            da = a & 0x03FFFFFC
            if (s + 4 * i + (da - 0x04000000 if da & 0x02000000 else da)) & 0xFFFFFFFF != x:
                continue
            db = b & 0x03FFFFFC
            if b >> 26 == 18 and (t + 4 * i + (db - 0x04000000 if db & 0x02000000 else db)) & 0xFFFFFFFF == tx:
                ok += 1
            else:
                bad += 1
    return ok, bad


def descriptors(P):
    """{function start: its TOC} from the program's function descriptors (OPD entries: the code, then the TOC)"""
    starts, toc = set(P.starts), {}
    for v, o, f, m, fl in P.segs:
        if fl & 1 or not f:
            continue
        d = P.d[o:o + f]
        for i in range(0, len(d) - 8, 4):
            c, t = struct.unpack_from(">II", d, i)
            if c in starts and P.in_data(t):
                toc.setdefault(c, collections.Counter())[t] += 1
    return {c: n.most_common(1)[0][0] for c, n in toc.items()}


def _sx(x):
    x &= 0xFFFF
    return x - 0x10000 if x & 0x8000 else x


def data_refs(A, B, m):
    """the data addresses matched functions reference, v1.00's -> the other's: each module's TOC (from the function
    descriptors), each instruction's data reference (TOC-relative, r2 + d; absolute, addis + d-form; TOC-relative
    and large, addis on r2 + d-form), and the data each referenced TOC slot points at. {v1.00 address: Counter}"""
    ta, tb = descriptors(A), descriptors(B)
    refs = collections.defaultdict(collections.Counter)
    for s, (t, _how) in m.items():
        if s in ta and t in tb:
            refs[ta[s]][tb[t]] += 1
        wa, wb = A.words(s, A.end[s]), B.words(t, B.end[t])
        if not wa or not wb or len(wa) != len(wb):
            continue
        hi_a, hi_b = {}, {}
        for a, b in zip(wa, wb):
            op, rt, ra = a >> 26, (a >> 21) & 31, (a >> 16) & 31
            if op == 15 and (b >> 26) == 15:   # addis rt, ra, hi: absolute (ra 0) or TOC-relative (ra 2)
                base_a = 0 if ra == 0 else ta.get(s) if ra == 2 else None
                base_b = 0 if ra == 0 else tb.get(t) if ra == 2 else None
                if base_a is not None and base_b is not None:
                    hi_a[rt], hi_b[rt] = base_a + (_sx(a) << 16), base_b + (_sx(b) << 16)
                else:
                    hi_a.pop(rt, None)
                continue
            if op in _IMM_OPS and op != 15 and (b >> 26) == op:
                if ra == 2 and s in ta and t in tb:
                    xa, xb = ta[s] + _sx(a), tb[t] + _sx(b)
                elif ra in hi_a:
                    xa, xb = (hi_a[ra] + _sx(a)) & 0xFFFFFFFF, (hi_b[ra] + _sx(b)) & 0xFFFFFFFF
                else:
                    xa = None
                if xa is not None and A.in_data(xa) and B.in_data(xb):
                    refs[xa][xb] += 1
                    if ra == 2 and op in (32, 58):   # a load from a TOC slot: the slot holds a data address
                        pa, pb = A.read(xa, 4), B.read(xb, 4)
                        if pa and pb:
                            (va,), (vb,) = struct.unpack(">I", pa), struct.unpack(">I", pb)
                            if A.in_data(va) and B.in_data(vb):
                                refs[va][vb] += 1
            if rt in hi_a and op not in (36, 37, 38, 39, 44, 45, 52, 53, 54, 55, 62):   # rt written (not a store)
                hi_a.pop(rt, None)
    return {x: c for x, c in refs.items()}


class Mapper:
    def __init__(self, A, B, manual=None):
        self.A, self.B, self.m = A, B, match(A, B)
        self.manual = {int(k, 16): v for k, v in (manual or {}).items() if not k.startswith("_")}
        self.unresolved = collections.defaultdict(set)   # address -> the places that use it
        self.refs = data_refs(A, B, self.m)
        self.ref_keys = sorted(self.refs)
        self.bracketed = {}   # data addresses mapped by their neighbours: (to the one below, to the one above, shift)

    def data_map(self, a):
        """a data address's counterpart from what the matched functions reference: the address itself when it is
        referenced (its dominant counterpart: 90% of the references), else the shift of the referenced addresses
        on both sides of it (the nearest each way) when they agree -- an address no code names directly, as a field of
        an object or a variable of the zero-filled data, lies where its neighbours do (the report says how far they
        are)"""
        def one(x):
            c = self.refs.get(x)
            if not c:
                return None
            (t, n), total = c.most_common(1)[0], sum(c.values())
            return t if n * 10 >= total * 9 else None
        t = one(a)
        if t is not None:
            return t
        k = bisect.bisect_right(self.ref_keys, a)
        lo = self.ref_keys[k - 1] if k > 0 else None
        hi = self.ref_keys[k] if k < len(self.ref_keys) else None
        if lo is None or hi is None:
            return None
        tl, th = one(lo), one(hi)
        if tl is None or th is None or tl - lo != th - hi:
            return None
        self.bracketed[a] = (a - lo, hi - a, tl - lo)   # the evidence: how far the references are, their shift
        return a + (tl - lo)

    def code_map(self, a):
        """a code address's counterpart: in a matched function the same offset; between functions (constants or
        tables in the code segment), the shift of the nearest matched function before it, when the bytes agree"""
        f = self.A.func_of(a)
        if f is not None:
            return self.m[f][0] + (a - f) if f in self.m else None
        ms = self._ms if hasattr(self, "_ms") else setattr(self, "_ms", sorted(self.m)) or self._ms
        k = bisect.bisect_right(ms, a) - 1
        if k < 0:
            return None
        t = a + (self.m[ms[k]][0] - ms[k])
        ra, rb = self.A.read(a, 32), self.B.read(t, 32)
        if ra is not None and ra == rb:
            return t
        # else the same 64 bytes once within 64 KB of there (an embedded image or table that moved by another shift)
        key = self.A.read(a, 64)
        if key is None or len(set(key)) < 8:   # too plain to locate (padding)
            return None
        lo, hi = max(t - 0x10000, 0), t + 0x10000
        for v, o, f, m_, fl in self.B.segs:
            if not fl & 1 or hi < v or lo >= v + f:
                continue
            a0, a1 = max(lo, v), min(hi, v + f)
            blob = self.B.d[o + a0 - v: o + a1 - v + 64]
            i = blob.find(key)
            if i >= 0 and blob.find(key, i + 1) < 0:
                return a0 + i
        return None

    def words_agree(self, a, n=32):
        """v1.00's data at a and the other's agree: each word the same, or a code address mapped to its counterpart
        (a vtable's or a function descriptor's entry: the code moved, the data did not)"""
        ra, rb = self.A.read(a, n), self.B.read(a, n)
        if ra is None or rb is None:
            return False
        for (wa,), (wb,) in zip(struct.iter_unpack(">I", ra), struct.iter_unpack(">I", rb)):
            if wa != wb and not (self.A.in_code(wa) and self.code_map(wa) == wb):
                return False
        return True

    def map(self, a, where):
        """the other program's address for v1.00's `a` (an int), or None (recorded as unresolved)"""
        if a in self.manual:
            v = self.manual[a]
            t = v.get("to") if isinstance(v, dict) else v
            return int(t, 16) if isinstance(t, str) else None
        if self.A.in_code(a):
            t = self.code_map(a)
            if t is not None:
                return t
        elif self.A.in_data(a):
            t = self.data_map(a)
            if t is not None:
                return t
            if not self.refs_near(a) and self.words_agree(a & ~15):
                return a   # no code references it or anything near it, and the bytes agree
        self.unresolved[a].add(where)
        return None

    def refs_near(self, a, span=0x1000):
        k = bisect.bisect_left(self.ref_keys, a - span)
        return k < len(self.ref_keys) and self.ref_keys[k] <= a + span

    def status(self, a):
        f = self.A.func_of(a)
        if f is not None:
            return self.m[f][1] if f in self.m else "in a changed function"
        return "data" if self.A.in_data(a) else "code outside a function"


def walk(x, f, path="", spu=None):
    """x with every "0x..." string passed through f(value, path), and every SPU image's name (spu_NNNN_at_<its file
    offset>, tools extract_spu_images.py) through spu(offset, path) (dicts, lists)"""
    if isinstance(x, dict):
        return {k: (x[k] if k.startswith("_") else walk(x[k], f, f"{path}.{k}", spu)) for k in x}
    if isinstance(x, list):
        return [walk(v, f, f"{path}[{i}]", spu) for i, v in enumerate(x)]
    if isinstance(x, str) and _HEX.match(x):
        return f(x, path)
    if isinstance(x, str) and _SPU_IMAGE.search(x) and spu is not None:
        def one(m):
            t = spu(int(m.group(2), 16), path)
            return f"spu_{m.group(1)}_at_{t:08X}" if t is not None else m.group(0)
        return _SPU_IMAGE.sub(one, x)
    return x


def spu_images(P):
    """the SPU programs embedded in the program file, as ps3recomp's extract_spu_images.py finds them: [(file
    offset, sha256 of the image)] in order"""
    import hashlib
    out, i = [], P.d.find(b"\x7fELF\x01\x02", 1)
    while i >= 0:
        if P.d[i + 18:i + 20] == b"\x00\x17":   # SPU
            shoff, = struct.unpack(">I", P.d[i + 32:i + 36])
            shes, shnum = struct.unpack(">HH", P.d[i + 46:i + 50])
            out.append((i, hashlib.sha256(P.d[i:i + shoff + shnum * shes]).hexdigest()))
        i = P.d.find(b"\x7fELF\x01\x02", i + 1)
    return out


def remap_config(mp, name, doc):
    """the config file for the other program; observers get their v1.00 address as "id" """
    def one(s, path):
        a = int(s, 16)
        if not (mp.A.in_code(a) or mp.A.in_data(a)):
            return s   # not an address of the program (a value, a mask, an SPU local-store address)
        t = mp.map(a, f"{name}{path}")
        width = len(s) - 2
        return f"0x{t:0{max(width, 6)}X}" if t is not None else s
    def spu(off, path):   # an SPU image: the other program's identical image, in the same order
        if not hasattr(mp, "_spu"):
            a, b = spu_images(mp.A), spu_images(mp.B)
            mp._spu = {oa: ob for (oa, ha), (ob, hb) in zip(a, b) if ha == hb} if len(a) == len(b) else {}
        t = mp._spu.get(off)
        if t is None:
            mp.unresolved[off].add(f"{name}{path} (an SPU image's file offset)")
        return t
    out = walk(doc, one, spu=spu)
    if name in OBSERVER_FILES:
        for key in ("observers",):
            for e, o in zip(doc.get(key) or [], out.get(key) or []):
                if isinstance(e, dict) and "addr" in e and "id" not in e:
                    o["id"] = e["addr"]
    return out


def port_refs(port_dir):
    """the lifted functions port/ names (func_XXXXXXXX) and the addresses it writes as PA(0x...)"""
    funcs, addrs = collections.defaultdict(set), collections.defaultdict(set)
    for p in glob.glob(os.path.join(port_dir, "**", "*.*"), recursive=True):
        if not p.endswith((".c", ".cpp", ".h", ".inc")):
            continue
        with open(p, encoding="utf-8", errors="replace") as f:
            txt = f.read()
        rel = os.path.relpath(p, port_dir)
        for m in PORT_FUNC.finditer(txt):
            funcs[int(m.group(1), 16)].add(rel)
        for m in PORT_ADDR.finditer(txt):
            addrs[int(m.group(1), 16)].add(rel)
    return funcs, addrs


# a program's folder is named for its region and version (config/programs/us-01.01): a folder named for a PS3 title
# ID is what tools/boundary_check.py keeps out of the repository (a save, a game install or a patch); program.json in
# it names the program (its "id", the build's DOTHEROES_PROGRAM)
REGIONS = {"US": "us", "ES": "eu", "JM": "jp", "JS": "jp", "AS": "asia", "KS": "kr"}


def program_folder(program_id):
    """config/programs/<region>-<version> for a program id like BLUS30490-01.01"""
    m = re.match(r"^[A-Z]{2}([A-Z]{2})\d{5}-(\d{2}\.\d{2})$", program_id)
    if not m or m.group(1) not in REGIONS:
        raise SystemExit(f"{program_id}: not a program id this tool names a folder for (e.g. BLUS30490-01.01)")
    return f"{REGIONS[m.group(1)]}-{m.group(2)}"


def find_program_dir(config, program_id):
    """the folder under config/programs whose program.json names program_id, else None"""
    for p in sorted(glob.glob(os.path.join(config, "programs", "*", "program.json"))):
        try:
            with open(p, encoding="utf-8") as f:
                if json.load(f).get("id") == program_id:
                    return os.path.dirname(p)
        except (OSError, ValueError):
            continue
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("v100_elf")
    ap.add_argument("v100_functions")
    ap.add_argument("other_elf")
    ap.add_argument("other_functions")
    ap.add_argument("program_id")
    ap.add_argument("--out", help="the program's config folder (default: the config/programs folder whose program.json names it, else config/programs/<region>-<version>)")
    ap.add_argument("--config", default=os.path.join(ROOT, "config"))
    ap.add_argument("--port", default=os.path.join(ROOT, "port"))
    ap.add_argument("--report", help="a JSON report of the match and every address's state")
    a = ap.parse_args(argv)
    out = a.out or find_program_dir(a.config, a.program_id) or os.path.join(a.config, "programs",
                                                                              program_folder(a.program_id))
    A, B = Program(a.v100_elf, a.v100_functions), Program(a.other_elf, a.other_functions)
    man_path = os.path.join(out, "remap_manual.json")
    manual = {}
    if os.path.exists(man_path):
        with open(man_path) as f:
            manual = json.load(f)
    mp = Mapper(A, B, manual)
    counts = collections.Counter(how for _, how in mp.m.values())
    print(f"[remap] v1.00 functions {len(A.funcs)}, {a.program_id} functions {len(B.funcs)}: "
          + ", ".join(f"{k} {counts[k]}" for k in ("identical", "moved", "relocated"))
          + f", unmatched {len(A.funcs) - len(mp.m)}")
    os.makedirs(out, exist_ok=True)
    pj = os.path.join(out, "program.json")   # the program's id (kept, with whatever else is in it)
    meta = {}
    if os.path.exists(pj):
        with open(pj, encoding="utf-8") as f:
            meta = json.load(f)
    meta = {"id": a.program_id, **{k: v for k, v in meta.items() if k != "id"}}
    # the native SPU tasks' images (tools/setup_pipeline.py SPU_NATIVE): this program's names for them
    try:
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import setup_pipeline
        a_imgs, b_imgs = spu_images(A), spu_images(B)
        smap = {oa: ob for (oa, ha), (ob, hb) in zip(a_imgs, b_imgs) if ha == hb} if len(a_imgs) == len(b_imgs) else {}
        names = {}
        a_offs = {o for o, _h in a_imgs}
        for task, image in setup_pipeline.SPU_NATIVE:
            m = _SPU_IMAGE.fullmatch(image)
            if not m or int(m.group(2), 16) not in a_offs:
                continue   # not an image of this v1.00 program (a made-up one in a test)
            t = smap.get(int(m.group(2), 16))
            if t is None:
                mp.unresolved[int(m.group(2), 16)].add(f"program.json spu_native {task} ({image})")
            else:
                names[task] = f"spu_{m.group(1)}_at_{t:08X}"
        if names:
            meta["spu_native"] = names
    except ImportError:
        pass
    with open(pj, "w", encoding="utf-8", newline="\n") as f:
        json.dump(meta, f, indent=1)
        f.write("\n")
    written = []
    for name in CONFIG_FILES:
        src = os.path.join(a.config, name)
        if not os.path.exists(src):
            continue
        with open(src, encoding="utf-8") as f:
            doc = json.load(f)
        res = remap_config(mp, name, doc)
        if isinstance(res, dict):
            res = {"_program": a.program_id, "_from": f"config/{name}, by tools/program_remap.py", **res}
        with open(os.path.join(out, name), "w", encoding="utf-8", newline="\n") as f:
            json.dump(res, f, indent=1)
            f.write("\n")
        written.append(name)
    # port/: the lifted functions it names and the addresses it writes down
    funcs, addrs = port_refs(a.port)
    alias = [f"/* port/'s lifted functions in {a.program_id} (tools/program_remap.py): v1.00's name -> this program's */"]
    for f in sorted(funcs):
        t = mp.map(f, "port/ " + ", ".join(sorted(funcs[f])))
        if t is not None and t != f:
            alias.append(f"#define func_{f:08X} func_{t:08X}")
    with open(os.path.join(out, "port_aliases.h"), "w", newline="\n") as fh:
        fh.write("\n".join(alias) + "\n")
    pairs = [f"/* port/'s addresses in {a.program_id} (tools/program_remap.py): {{v1.00, this program}} for PA() */"]
    for v in sorted(addrs):
        t = mp.map(v, "port/ " + ", ".join(sorted(addrs[v])))
        if t is not None:
            pairs.append(f"{{0x{v:08X}u, 0x{t:08X}u}},")
    with open(os.path.join(out, "port_addrs.inc"), "w", newline="\n") as fh:
        fh.write("\n".join(pairs) + "\n")
    if a.report:
        rep = {"program": a.program_id, "functions": {"v100": len(A.funcs), "other": len(B.funcs), **counts},
               "unresolved": {f"0x{x:08X}": {"status": mp.status(x), "used_by": sorted(w)}
                              for x, w in sorted(mp.unresolved.items())},
               "data_by_neighbours": {f"0x{x:08X}": {"below": f"-0x{lo:X}", "above": f"+0x{hi:X}", "shift": d}
                                      for x, (lo, hi, d) in sorted(mp.bracketed.items())}}
        with open(a.report, "w") as f:
            json.dump(rep, f, indent=1)
    print(f"[remap] wrote {len(written)} config files, port_aliases.h ({len(alias) - 1} names), port_addrs.inc "
          f"({len(pairs) - 1} addresses) to {os.path.relpath(out, ROOT)}")
    if mp.unresolved:
        print(f"[remap] {len(mp.unresolved)} addresses unresolved -- review them by hand ({man_path}):")
        for x, w in sorted(mp.unresolved.items()):
            print(f"  0x{x:08X} ({mp.status(x)}): {', '.join(sorted(w))[:150]}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
