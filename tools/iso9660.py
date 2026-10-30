#!/usr/bin/env python3
"""Minimal ISO9660 reader for decrypted PS3 disc images.

Usage:
  iso9660.py list <iso>
  iso9660.py extract <iso> <outdir> [path-prefix ...]
"""
import os
import struct
import sys

SECTOR = 2048


def read_dir(f, lba, size, prefix="", joliet=False):
    f.seek(lba * SECTOR)
    data = f.read(size)
    off = 0
    while off < len(data):
        ln = data[off]
        if ln == 0:  # records never cross sector boundaries
            off = (off // SECTOR + 1) * SECTOR
            continue
        rec = data[off:off + ln]
        ext_lba = struct.unpack_from("<I", rec, 2)[0]
        ext_len = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33 + nlen]
        off += ln
        if name in (b"\x00", b"\x01"):
            continue
        name = (name.decode("utf-16-be", "replace") if joliet
                else name.decode("ascii", "replace")).split(";")[0]
        path = prefix + name
        if flags & 2:
            yield from read_dir(f, ext_lba, ext_len, path + "/", joliet)
        else:
            # multi-extent files (flag 0x80) are emitted once per extent
            yield path, ext_lba, ext_len, bool(flags & 0x80)


def find_volume(f):
    """Prefer the Joliet SVD: it carries the mixed-case names the game opens."""
    sector = 16
    pvd = None
    while True:
        f.seek(sector * SECTOR)
        vd = f.read(SECTOR)
        if vd[1:6] != b"CD001" or vd[0] == 255:
            return pvd, False
        if vd[0] == 1 and pvd is None:
            pvd = vd
        if vd[0] == 2 and vd[88:91] in (b"%/@", b"%/C", b"%/E"):
            return vd, True
        sector += 1


def entries(f):
    vd, joliet = find_volume(f)
    root = vd[156:156 + 34]
    lba = struct.unpack_from("<I", root, 2)[0]
    size = struct.unpack_from("<I", root, 10)[0]
    files = {}
    order = []
    for path, l, n, multi in read_dir(f, lba, size, "", joliet):
        if path not in files:
            files[path] = []
            order.append(path)
        files[path].append((l, n))
    return [(p, files[p]) for p in order]


def main():
    cmd, iso = sys.argv[1], sys.argv[2]
    with open(iso, "rb") as f:
        ents = entries(f)
        if cmd == "list":
            for p, ext in ents:
                print(f"{sum(n for _, n in ext):12d} {ext[0][0]:8d} {p}")
        elif cmd == "extract":
            out = sys.argv[3]
            prefixes = sys.argv[4:]
            for p, ext in ents:
                if prefixes and not any(p.startswith(x) for x in prefixes):
                    continue
                dst = os.path.join(out, p)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, "wb") as o:
                    for l, n in ext:
                        f.seek(l * SECTOR)
                        while n:
                            chunk = f.read(min(n, 1 << 20))
                            o.write(chunk)
                            n -= len(chunk)


if __name__ == "__main__":
    main()
