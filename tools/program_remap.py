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
2. maps an address: inside a matched (or relocated) function, the same offset in its counterpart; in the data
   segment, the same address when the two programs' data agree around it (16 bytes); anything else unresolved;
3. writes config/programs/<id>/: each address-tied file of config/ with its addresses mapped -- observers keep their
   v1.00 address as their "id" (the lifter passes it to the port, so the port's dispatch stays as it is) -- and, for
   port/'s own code, port_aliases.h (the lifted functions port/ names, v1.00's name to this program's) and
   port_addrs.inc (each address port/ writes down, v1.00's to this program's, for PA() in port/program_addrs.h);
4. stops on anything unresolved, unless config/programs/<id>/remap_manual.json decides it (an address reviewed by
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
                "dev_capture_observers.json", "m8_movie_trace.json", "m8_dev_credits.json", "r8_promote.json"]
# observers: the lifter passes the entry's "id" (when given) to the port in place of its address
OBSERVER_FILES = {"r6_native_observers.json", "r7_native_hooks.json", "dev_capture_observers.json",
                  "m8_movie_trace.json"}
_HEX = re.compile(r"^0x[0-9A-Fa-f]{1,8}$")
PORT_FUNC = re.compile(r"\bfunc_([0-9A-F]{8})\b")
PORT_ADDR = re.compile(r"\bPA\((0x[0-9A-Fa-f]{1,8})u?\)")


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


def relocated_only(wa, wb, limit=16):
    """the same instruction stream but for address immediates (and branch displacements): at most `limit` such words"""
    if len(wa) != len(wb):
        return False
    n = 0
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
        n += 1
        if n > limit:
            return False
    return True


def match(A, B):
    """{v1.00 function start: (other start, how)} -- how: identical, moved, relocated"""
    hb = collections.defaultdict(list)
    wb = {}
    for s, e in B.funcs:
        w = B.words(s, e)
        if w is not None:
            wb[s] = w
            hb[norm_hash(w)].append(s)
    out = {}
    wa = {}
    for s, e in A.funcs:
        w = A.words(s, e)
        if w is None:
            continue
        wa[s] = w
        cands = hb.get(norm_hash(w), [])
        if cands:
            t = min(cands, key=lambda c: abs(c - s))
            out[s] = (t, "identical" if t == s and wb[t] == w else "moved")
    taken = {t for t, _ in out.values()}
    # relocated: where the nearest matched function before it says, the same length, address immediates only
    ms = sorted(out)
    for s, e in A.funcs:
        if s in out or s not in wa:
            continue
        k = bisect.bisect_left(ms, s) - 1
        shift = out[ms[k]][0] - ms[k] if k >= 0 else 0
        t = s + shift
        if t in wb and t not in taken and relocated_only(wa[s], wb[t]):
            out[s] = (t, "relocated")
            taken.add(t)
    return out


class Mapper:
    def __init__(self, A, B, manual=None):
        self.A, self.B, self.m = A, B, match(A, B)
        self.manual = {int(k, 16): v for k, v in (manual or {}).items() if not k.startswith("_")}
        self.unresolved = collections.defaultdict(set)   # address -> the places that use it

    def map(self, a, where):
        """the other program's address for v1.00's `a` (an int), or None (recorded as unresolved)"""
        if a in self.manual:
            v = self.manual[a]
            t = v.get("to") if isinstance(v, dict) else v
            return int(t, 16) if isinstance(t, str) else None
        if self.A.in_code(a):
            f = self.A.func_of(a)
            if f is not None and f in self.m:
                return self.m[f][0] + (a - f)
        elif self.A.in_data(a):
            ra, rb = self.A.read(a & ~15, 16), self.B.read(a & ~15, 16)
            if ra is not None and ra == rb:
                return a
            if ra is None and rb is None and self.B.in_data(a):   # .bss: no bytes in either, same layout assumed
                return a
        self.unresolved[a].add(where)
        return None

    def status(self, a):
        f = self.A.func_of(a)
        return self.m[f][1] if f is not None and f in self.m else ("data" if self.A.in_data(a) else "changed")


def walk(x, f, path=""):
    """x with every "0x..." string in the program's range passed through f(value, path) (dicts, lists)"""
    if isinstance(x, dict):
        return {k: (x[k] if k.startswith("_") else walk(x[k], f, f"{path}.{k}")) for k in x}
    if isinstance(x, list):
        return [walk(v, f, f"{path}[{i}]") for i, v in enumerate(x)]
    if isinstance(x, str) and _HEX.match(x):
        return f(x, path)
    return x


def remap_config(mp, name, doc):
    """the config file for the other program; observers get their v1.00 address as "id" """
    def one(s, path):
        a = int(s, 16)
        if not (mp.A.in_code(a) or mp.A.in_data(a)):
            return s   # not an address of the program (a value, a mask, an SPU local-store address)
        t = mp.map(a, f"{name}{path}")
        width = len(s) - 2
        return f"0x{t:0{max(width, 6)}X}" if t is not None else s
    out = walk(doc, one)
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


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("v100_elf")
    ap.add_argument("v100_functions")
    ap.add_argument("other_elf")
    ap.add_argument("other_functions")
    ap.add_argument("program_id")
    ap.add_argument("--out", help="the program's config folder (default config/programs/<id>)")
    ap.add_argument("--config", default=os.path.join(ROOT, "config"))
    ap.add_argument("--port", default=os.path.join(ROOT, "port"))
    ap.add_argument("--report", help="a JSON report of the match and every address's state")
    a = ap.parse_args(argv)
    out = a.out or os.path.join(a.config, "programs", a.program_id)
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
                              for x, w in sorted(mp.unresolved.items())}}
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
