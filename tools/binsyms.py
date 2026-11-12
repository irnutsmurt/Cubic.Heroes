#!/usr/bin/env python3
"""The defined symbols of an object file or a static library, read without any toolchain: ar archives (GNU and
COFF formats, their symbol index when there is one, else each member), COFF objects (regular and bigobj) and ELF
relocatable objects (64-bit and 32-bit, either byte order). tools/boundary_check.py uses it to tell what a
library defines from what it only refers to (a native patch calls lifted functions by name; it must not carry
them).

    py -3 tools/binsyms.py <file.a | file.o | file.obj> [<regex>]
"""
import re
import struct
import sys

AR_MAGIC = b"!<arch>\n"


def ar_members(data):
    """(name, bytes) of each member of an ar archive, its special members ("/", "//", "/SYM64/") included under
    their own names."""
    pos, longnames = len(AR_MAGIC), b""
    while pos + 60 <= len(data):
        h = data[pos:pos + 60]
        name = h[:16].decode("latin-1").rstrip()
        try:
            size = int(h[48:58].decode("latin-1").strip() or 0)
        except ValueError:
            break
        body = data[pos + 60:pos + 60 + size]
        if name == "//":
            longnames = body
        elif name.startswith("/") and name[1:].isdigit():
            o = int(name[1:])
            end = longnames.find(b"\n", o)
            name = longnames[o:end if end >= 0 else len(longnames)].decode("latin-1").rstrip("/")
        elif name.startswith("#1/"):          # BSD long name: the name leads the body
            n = int(name[3:])
            name, body = body[:n].rstrip(b"\0").decode("latin-1"), body[n:]
        elif name not in ("/", "/SYM64/", "__.SYMDEF", "__.SYMDEF SORTED"):
            name = name.rstrip("/")
        yield name, body
        pos += 60 + size + (size & 1)


def ar_index(data):
    """The archive's symbol index (every member's defined external symbols), or None without one."""
    for name, body in ar_members(data):
        if name in ("/", "/SYM64/"):
            w = 8 if name == "/SYM64/" else 4
            if len(body) < w:
                return []
            n = int.from_bytes(body[:w], "big")
            strings = body[w + w * n:]
            return [s.decode("latin-1") for s in strings.split(b"\0")[:n] if s]
        return None   # the index is the first member when there is one
    return None


def coff_defined(data):
    if len(data) < 20:
        return []
    machine, nsec = struct.unpack_from("<HH", data, 0)
    if machine == 0 and nsec == 0xFFFF:   # bigobj: ANON_OBJECT_HEADER_BIGOBJ
        if len(data) < 56:
            return []
        symptr, nsym = struct.unpack_from("<II", data, 48)
        recsize, secfmt = 20, "<i"
    else:
        symptr, nsym = struct.unpack_from("<II", data, 8)
        recsize, secfmt = 18, "<h"
    strtab = symptr + nsym * recsize
    out, i = [], 0
    while i < nsym and symptr + (i + 1) * recsize <= len(data):
        rec = data[symptr + i * recsize:symptr + (i + 1) * recsize]
        if rec[:4] == b"\0\0\0\0":
            off = struct.unpack_from("<I", rec, 4)[0]
            end = data.find(b"\0", strtab + off)
            name = data[strtab + off:end].decode("latin-1")
        else:
            name = rec[:8].rstrip(b"\0").decode("latin-1")
        section = struct.unpack_from(secfmt, rec, 12)[0]
        storage, naux = rec[recsize - 2], rec[recsize - 1]
        if section > 0 and storage in (2, 3):   # external or static, in a section of this object
            out.append(name)
        i += 1 + naux
    return out


def elf_defined(data):
    if len(data) < 52 or data[:4] != b"\x7fELF":
        return []
    is64, end = data[4] == 2, "<" if data[5] == 1 else ">"
    if is64:
        shoff, = struct.unpack_from(end + "Q", data, 0x28)
        shentsize, shnum = struct.unpack_from(end + "HH", data, 0x3A)
    else:
        shoff, = struct.unpack_from(end + "I", data, 0x20)
        shentsize, shnum = struct.unpack_from(end + "HH", data, 0x2E)
    secs = []
    for k in range(shnum):
        o = shoff + k * shentsize
        if o + shentsize > len(data):
            return []
        if is64:
            _, typ, _, _, off, size, link, _, _, entsize = struct.unpack_from(end + "IIQQQQIIQQ", data, o)
        else:
            _, typ, _, _, off, size, link, _, _, entsize = struct.unpack_from(end + "IIIIIIIIII", data, o)
        secs.append((typ, off, size, link, entsize))
    out = []
    for typ, off, size, link, entsize in secs:
        if typ not in (2, 11) or link >= len(secs):   # SHT_SYMTAB, SHT_DYNSYM
            continue
        stroff = secs[link][1]
        entsize = entsize or (24 if is64 else 16)
        for s in range(off + entsize, off + size, entsize):   # entry 0 is the null symbol
            if is64:
                nameoff, _, _, shndx = struct.unpack_from(end + "IBBH", data, s)
            else:
                nameoff, = struct.unpack_from(end + "I", data, s)
                shndx, = struct.unpack_from(end + "H", data, s + 14)
            if shndx == 0:
                continue   # undefined: a reference
            e = data.find(b"\0", stroff + nameoff)
            out.append(data[stroff + nameoff:e].decode("latin-1"))
    return out


def defined_symbols(data):
    """Every symbol `data` (an archive or an object) defines; [] for anything else."""
    if data.startswith(AR_MAGIC):
        idx = ar_index(data)
        if idx is not None:
            return idx
        out = []
        for name, body in ar_members(data):
            if name not in ("/", "//", "/SYM64/"):
                out += defined_symbols(body)
        return out
    if data[:4] == b"\x7fELF":
        return elf_defined(data)
    if len(data) >= 20 and (struct.unpack_from("<H", data, 0)[0] in (0x8664, 0x14C, 0xAA64)
                            or data[:4] == b"\0\0\xff\xff"):
        return coff_defined(data)
    return []


def main():
    data = open(sys.argv[1], "rb").read()
    pat = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None
    syms = defined_symbols(data)
    for s in syms:
        if not pat or pat.search(s):
            print(s)
    print(f"{len(syms)} defined", file=sys.stderr)


if __name__ == "__main__":
    main()
