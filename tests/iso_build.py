#!/usr/bin/env python3
"""A developer's ISO 9660 writer for tests (0.2.0-beta.5): images "rebuilt" from a disc's files in other layouts --
another file order, gaps between files, padding at the end, with or without a Joliet tree -- the way PS3 ISO tools
rebuild a disc from its folder. Never shipped; it makes test images only (synthetic files, or a developer's own disc
folder under build/tmp).

    build(out, files, joliet=True, order="name", gap=0, pad=0)
        files: {"PS3_GAME/...": bytes or a file's path}; order: "name", "reverse" or "size" (the data's order)
    iso_build.py <disc folder> <out.iso> [--no-joliet] [--order name|reverse|size] [--gap N] [--pad N]
"""
import argparse
import os
import struct
import sys

SECTOR = 2048


def both(fmt, v):
    return struct.pack("<" + fmt, v) + struct.pack(">" + fmt, v)


def record(name, lba, size, is_dir):
    n = len(name)
    ln = 33 + n + (1 - n % 2)
    return (bytes([ln, 0]) + both("I", lba) + both("I", size) + bytes(7) + bytes([2 if is_dir else 0, 0, 0])
            + both("H", 1) + bytes([n]) + name + bytes(ln - 33 - n))


def _tree(files):
    root = {}
    for p in files:
        d = root
        parts = p.split("/")
        for part in parts[:-1]:
            d = d.setdefault(part, {})
        d[parts[-1]] = p
    return root


def _encode(name, joliet, is_dir):
    if joliet:
        return (name if is_dir else name + ";1").encode("utf-16-be")
    return (name.upper() if is_dir else name.upper() + ";1").encode("ascii")


def _dir_size(node, joliet):
    """bytes of a directory's records (whole sectors; a record never crosses one)"""
    used, size = 34 * 2, SECTOR
    for name, child in sorted(node.items()):
        ln = len(record(_encode(name, joliet, isinstance(child, dict)), 0, 0, False))
        if used + ln > SECTOR:
            size += SECTOR
            used = 0
        used += ln
    return size


def _dirs(node, joliet, prefix=""):
    """[(path, node)] every directory, the root first"""
    out = [(prefix, node)]
    for name, child in sorted(node.items()):
        if isinstance(child, dict):
            out += _dirs(child, joliet, prefix + name + "/")
    return out


def _size_of(src):
    return len(src) if isinstance(src, (bytes, bytearray)) else os.path.getsize(src)


def build(out, files, joliet=True, order="name", gap=0, pad=0):
    tree = _tree(files)
    trees = [False, True] if joliet else [False]
    nxt = 16 + len(trees) + 1   # descriptors, then the terminator
    dir_lba = {}   # (joliet, path) -> (lba, size)
    for j in trees:
        for path, node in _dirs(tree, j):
            size = _dir_size(node, j)
            dir_lba[(j, path)] = (nxt, size)
            nxt += size // SECTOR
    keys = {"name": lambda p: p, "reverse": lambda p: p, "size": lambda p: (_size_of(files[p]), p)}
    names = sorted(files, key=keys[order], reverse=(order == "reverse"))
    file_lba = {}
    for p in names:
        nxt += gap
        file_lba[p] = nxt
        nxt += max(1, -(-_size_of(files[p]) // SECTOR))
    total = nxt + pad
    with open(out, "wb") as f:
        f.truncate(total * SECTOR)
        for i, j in enumerate(trees):
            vd = bytearray(SECTOR)
            vd[0:7] = (b"\x02" if j else b"\x01") + b"CD001\x01"
            vd[80:88] = both("I", total)
            if j:
                vd[88:91] = b"%/E"
            lba, size = dir_lba[(j, "")]
            vd[156:190] = record(b"\0", lba, size, True)
            f.seek((16 + i) * SECTOR)
            f.write(vd)
        f.seek((16 + len(trees)) * SECTOR)
        f.write(b"\xffCD001\x01")
        for j in trees:
            for path, node in _dirs(tree, j):
                lba, size = dir_lba[(j, path)]
                parent = dir_lba[(j, path.rsplit("/", 2)[0] + "/" if path.count("/") > 1 else "")]
                body = bytearray(record(b"\0", lba, size, True) + record(b"\1", parent[0], parent[1], True))
                for name, child in sorted(node.items()):
                    is_dir = isinstance(child, dict)
                    if is_dir:
                        clba, csize = dir_lba[(j, path + name + "/")]
                    else:
                        clba, csize = file_lba[child], _size_of(files[child])
                    r = record(_encode(name, j, is_dir), clba, csize, is_dir)
                    if len(body) % SECTOR + len(r) > SECTOR:
                        body += bytes(SECTOR - len(body) % SECTOR)
                    body += r
                f.seek(lba * SECTOR)
                f.write(bytes(body))
        for p in names:
            f.seek(file_lba[p] * SECTOR)
            src = files[p]
            if isinstance(src, (bytes, bytearray)):
                f.write(src)
            else:
                with open(src, "rb") as s:
                    for b in iter(lambda: s.read(8 << 20), b""):
                        f.write(b)
    return total * SECTOR


def encrypt_synthetic(src, out, key, plain_end=None):
    """a SYNTHETIC encrypted image made from a decrypted one (never an authentic encrypted disc): sector 0's region
    table rewritten to one plain region (0..plain_end: by default up to the first file's sector, the file system
    kept readable as on a disc), every later sector AES-128-CBC encrypted with key, the IV its sector number --
    tools/ps3_disc.py's format, read back through ps3_disc.DiscImage"""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
    import iso9660
    from Crypto.Cipher import AES
    with open(src, "rb") as f:
        if plain_end is None:
            plain_end = min(ext[0][0] for _, ext in iso9660.entries(f)) - 1
        f.seek(0)
        with open(out, "wb") as o:
            n = 0
            for chunk in iter(lambda: f.read(SECTOR * 4096), b""):
                buf = bytearray(chunk)
                for i in range(0, len(buf) - SECTOR + 1, SECTOR):
                    sec = n + i // SECTOR
                    if sec == 0:
                        buf[0:16] = struct.pack(">II", 1, 0) + struct.pack(">II", 0, plain_end)
                    elif sec > plain_end:
                        iv = bytes(12) + struct.pack(">I", sec)
                        buf[i:i + SECTOR] = AES.new(key, AES.MODE_CBC, iv).encrypt(bytes(buf[i:i + SECTOR]))
                o.write(buf)
                n += len(chunk) // SECTOR
    return plain_end


def folder_files(top):
    """{"PS3_GAME/...": path} of a disc folder's PS3_GAME"""
    out = {}
    for d, _, fs in os.walk(os.path.join(top, "PS3_GAME")):
        for n in fs:
            p = os.path.join(d, n)
            out[os.path.relpath(p, top).replace(os.sep, "/")] = p
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("out")
    ap.add_argument("--no-joliet", action="store_true")
    ap.add_argument("--order", default="name", choices=("name", "reverse", "size"))
    ap.add_argument("--gap", type=int, default=0)
    ap.add_argument("--pad", type=int, default=0)
    a = ap.parse_args()
    n = build(a.out, folder_files(a.folder), joliet=not a.no_joliet, order=a.order, gap=a.gap, pad=a.pad)
    print(f"{a.out}: {n} bytes")


if __name__ == "__main__":
    sys.exit(main())
