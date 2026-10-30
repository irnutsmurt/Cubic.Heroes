#!/usr/bin/env python3
"""Correct ps3recomp's OPD-derived function list for this EBOOT.

ps3recomp sizes each function from its OPD start to the next OPD start. Two
properties of the 3D Dot Game Heroes EBOOT break that:

1. The executable PT_LOAD segment also holds the import stubs, ~5 MB of
   rodata (Lua source text, map names) and the 26 embedded SPU images. The
   last OPD function (a 0x34-byte CRT stub at 0xa77ba0) is therefore sized to
   the end of the segment and all of that data gets "lifted" as PPU code.
   Fix: clamp every function to the end of the SHF_EXECINSTR section that
   contains its start.

2. The game links with several TOCs. Calls that cross TOCs go through 16-byte
   linker trampolines:
       std r2,0x28(r1); addis r2,r2,hi; addi r2,r2,lo; b target
   They are packed back to back inside one OPD range, and the lifter emits
   each call target to the end of that range, duplicating every following
   trampoline as dead code (quadratic output). Fix: give each trampoline its
   own 16-byte function.

Usage: fix_functions.py EBOOT.elf in.functions.json out.functions.json
"""
import json
import struct
import sys

SHF_EXECINSTR = 0x4


def exec_sections(elf):
    shoff = struct.unpack_from(">Q", elf, 0x28)[0]
    shentsize, shnum = struct.unpack_from(">HH", elf, 0x3A)
    out = []
    for i in range(shnum):
        _, typ, flags, addr, off, size = struct.unpack_from(">IIQQQQ", elf, shoff + i * shentsize)
        if typ == 1 and flags & SHF_EXECINSTR and size:
            out.append((addr, addr + size, off))
    return sorted(out)


def is_trampoline(words):
    std, addis, addi, b = words
    return (std == 0xF8410028
            and addis & 0xFFFF0000 == 0x3C420000
            and addi & 0xFFFF0000 == 0x38420000
            and b >> 26 == 18 and b & 3 == 0)


def main():
    elf_path, in_path, out_path = sys.argv[1:4]
    elf = open(elf_path, "rb").read()
    secs = exec_sections(elf)
    funcs = json.load(open(in_path))

    def section_of(a):
        for s, e, off in secs:
            if s <= a < e:
                return s, e, off
        return None

    clamped = 0
    out = {}
    for f in funcs:
        start, end = int(f["start"], 16), int(f["end"], 16)
        sec = section_of(start)
        if sec and end > sec[1]:
            end = sec[1]
            clamped += 1
        out[start] = dict(f, start=hex(start), end=hex(end))

    # Scan code for trampolines and split them out.
    tramps = 0
    for s, e, off in secs:
        a = s
        while a + 16 <= e:
            w = struct.unpack_from(">4I", elf, off + (a - s))
            if is_trampoline(w):
                base = out.get(a)
                toc = base["toc"] if base else None
                if toc is None:
                    # inherit the TOC of the enclosing OPD function
                    enc = max((k for k in out if k <= a), default=None)
                    toc = out[enc]["toc"] if enc is not None else "0x0"
                entry = {"start": hex(a), "end": hex(a + 16), "toc": toc}
                if base and "opd" in base:
                    entry["opd"] = base["opd"]
                out[a] = entry
                tramps += 1
                a += 16
            else:
                a += 4

    # Code that follows a run of trampolines inside the same OPD range keeps
    # its own range instead of being cut off by the split.
    for a in sorted(out):
        f = out[a]
        if int(f["end"], 16) - a != 16 or not is_trampoline(
                struct.unpack_from(">4I", elf, section_of(a)[2] + (a - section_of(a)[0]))):
            continue
        rest = a + 16
        if rest in out:
            continue
        nxt = min((k for k in out if k > a), default=None)
        enc = max((k for k in out if k < a and int(out[k]["end"], 16) > rest), default=None)
        if enc is not None:
            end = int(out[enc]["end"], 16)
            if nxt is not None:
                end = min(end, nxt)
            if end > rest:
                out[rest] = {"start": hex(rest), "end": hex(end), "toc": out[enc]["toc"]}

    # Re-derive ends so no function runs over the next one's start.
    starts = sorted(out)
    for i, a in enumerate(starts):
        f = out[a]
        nxt = starts[i + 1] if i + 1 < len(starts) else None
        if nxt is not None and int(f["end"], 16) > nxt:
            f["end"] = hex(nxt)

    json.dump([out[a] for a in starts], open(out_path, "w"), indent=0)
    print(f"{len(funcs)} -> {len(starts)} functions; clamped {clamped} to code sections; "
          f"{tramps} TOC trampolines split out")


if __name__ == "__main__":
    main()
