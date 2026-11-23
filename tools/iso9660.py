#!/usr/bin/env python3
"""ISO 9660 reader for PS3 disc images (decrypted, or an encrypted one read through ps3_disc.DiscImage), hardened
against malformed and hostile images (0.2.0-beta.5): nothing in an image can make it read outside the image, loop,
recurse without end, read more than a bounded amount of directory data, or produce a name that is a path.

    entries(f)              [(path, [(lba, length), ...])] of every file, in directory order (the Joliet tree when the
                            image has one: it carries the mixed-case names the game opens)
    find_volume(f)          (descriptor, joliet)
    stream(f, extents)      a file's bytes in chunks; a short read raises ImageError
    read_file(f, extents)   a (small) file's bytes

Refused with ImageError (a ValueError; NotISO when there is no ISO 9660 volume at all):
  - no primary volume descriptor, or no terminator within MAX_DESCRIPTORS sectors;
  - a directory record shorter than its fixed part, crossing a sector, or with a name longer than the record;
  - an interleaved file; a multi-extent file not ending, interrupted or with a part not a whole number of sectors;
  - an extent (a directory's or a file's) that isn't inside the image;
  - a directory met twice or overlapping another (a loop); two files, a file and a directory, or either and the
    volume descriptors sharing a sector (the game's disc has none; a hard-linked file is refused too); a tree deeper
    than MAX_DEPTH, more than MAX_ENTRIES records, a directory over MAX_DIR_BYTES or more than MAX_TOTAL_DIR_BYTES of
    directories in all;
  - a name that isn't valid (Joliet: UTF-16BE; otherwise ASCII), is empty, "." or "..", or holds "/", "\\", ":",
    NUL or a control character, or a version that isn't a number;
  - two entries of one directory with the same name, or names differing only in case (one file on Windows, two on
    Linux: refused on both).

Usage (a developer's tool):
  iso9660.py list <iso>
  iso9660.py extract <iso> <outdir> [path-prefix ...]
"""
import os
import struct
import sys

SECTOR = 2048
MAX_DESCRIPTORS = 32              # volume descriptors read from sector 16 on, the terminator included
MAX_DEPTH = 32                    # directory levels below the root
MAX_ENTRIES = 100_000             # directory records in all (the game's disc: about 3,000)
MAX_DIR_BYTES = 16 << 20          # one directory's data
MAX_TOTAL_DIR_BYTES = 64 << 20    # every directory's data together
CHUNK = 8 << 20
JOLIET = (b"%/@", b"%/C", b"%/E")
BAD_CHARS = set("/\\:\0")


class ImageError(ValueError):
    """the image is malformed or hostile; the message says how (it may quote a name, never file contents)"""


class NotISO(ImageError):
    """no ISO 9660 volume at all"""


def image_size(f):
    f.seek(0, os.SEEK_END)
    return f.tell()


def _read_exact(f, pos, n, what):
    f.seek(pos)
    data = f.read(n)
    if len(data) != n:
        raise ImageError(f"{what} is cut short: the image ends before it")
    return data


def find_volume(f):
    """(the descriptor to read, joliet): the Joliet supplementary descriptor when there is one, else the primary"""
    return _volume(f)[:2]


def _volume(f):
    """find_volume's, and the sector after the terminator"""
    pvd = svd = None
    for i in range(MAX_DESCRIPTORS):
        f.seek((16 + i) * SECTOR)
        vd = f.read(SECTOR)
        if len(vd) < SECTOR or vd[1:6] != b"CD001":
            if i == 0:
                raise NotISO("no ISO 9660 volume descriptor")
            raise ImageError("the volume descriptors end without a terminator")
        if vd[0] == 255:
            break
        if vd[0] == 1 and pvd is None:
            pvd = vd
        elif vd[0] == 2 and vd[88:91] in JOLIET and svd is None:
            svd = vd
    else:
        raise ImageError(f"no volume descriptor terminator in the first {MAX_DESCRIPTORS} descriptors")
    if pvd is None:
        raise ImageError("no primary volume descriptor")
    return (svd, True, 17 + i) if svd is not None else (pvd, False, 17 + i)


def _name(raw, joliet, is_dir):
    try:
        name = raw.decode("utf-16-be") if joliet else raw.decode("ascii")
    except UnicodeDecodeError:
        raise ImageError("a name isn't valid " + ("UTF-16" if joliet else "ASCII")) from None
    if ";" in name:
        name, ver = name.rsplit(";", 1)
        if not ver.isdigit():
            raise ImageError(f"a name's version isn't a number: {name!r}")
    if not joliet and not is_dir and name.endswith(".") and len(name) > 1:
        name = name[:-1]   # "NAME.;1": a file without an extension
    if (not name or name in (".", "..") or any(c in BAD_CHARS or ord(c) < 32 or ord(c) == 127 for c in name)):
        raise ImageError(f"a name isn't a valid file name: {name!r}")
    return name


def _records(data):
    """(name bytes, extent lba, extent length, flags) of a directory's records, its own and its parent's left out"""
    off, n = 0, len(data)
    while off < n:
        ln = data[off]
        if ln == 0:   # records never cross a sector: the rest of this one is padding
            off = (off // SECTOR + 1) * SECTOR
            continue
        if ln < 34 or off + ln > n or off // SECTOR != (off + ln - 1) // SECTOR:
            raise ImageError("a directory record is malformed")
        rec = data[off:off + ln]
        nlen = rec[32]
        if nlen == 0 or 33 + nlen > ln:
            raise ImageError("a directory record's name is malformed")
        if rec[26] or rec[27]:
            raise ImageError("an interleaved file (not supported)")
        off += ln
        name = rec[33:33 + nlen]
        if name in (b"\x00", b"\x01"):
            continue
        yield name, struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0], rec[25]


def entries(f):
    vd, joliet, after = _volume(f)
    size = image_size(f)
    spans = [(16, after, "the volume descriptors")]   # every sector something occupies: none may hold two things
    root_lba, root_len = struct.unpack_from("<I", vd, 158)[0], struct.unpack_from("<I", vd, 166)[0]
    files = {}                 # path -> extents, in directory order
    names = {}                 # every path, folded -> as found
    dir_sectors = set()
    count = total_dir = 0
    stack = [(root_lba, root_len, "", 0)]
    while stack:
        lba, ln, prefix, depth = stack.pop()
        where = prefix.rstrip("/") or "the root"
        if depth > MAX_DEPTH:
            raise ImageError(f"the directory tree is deeper than {MAX_DEPTH} levels ({where})")
        if not 0 < ln <= MAX_DIR_BYTES:
            raise ImageError(f"a directory's size is out of bounds ({where})")
        total_dir += ln
        if total_dir > MAX_TOTAL_DIR_BYTES:
            raise ImageError("the directories are too large")
        if lba * SECTOR + ln > size:
            raise ImageError(f"a directory lies outside the image ({where})")
        span = range(lba, lba + -(-ln // SECTOR))
        if any(s in dir_sectors for s in span):
            raise ImageError(f"a directory is met twice: a loop or an overlap ({where})")
        dir_sectors.update(span)
        spans.append((span.start, span.stop, f"the directory {where}"))
        data = _read_exact(f, lba * SECTOR, ln, f"the directory {where}")
        subdirs = []
        multi = None   # the file whose next extent must follow
        for raw, ext_lba, ext_len, flags in _records(data):
            count += 1
            if count > MAX_ENTRIES:
                raise ImageError(f"more than {MAX_ENTRIES} entries")
            is_dir = bool(flags & 2)
            path = prefix + _name(raw, joliet, is_dir)
            key = path.casefold()
            if multi is not None:
                if is_dir or path != multi:
                    raise ImageError(f"a file in several extents is interrupted: {multi}")
                if ext_lba * SECTOR + ext_len > size:
                    raise ImageError(f"a file lies outside the image: {path}")
                files[path].append((ext_lba, ext_len))
                if ext_len:
                    spans.append((ext_lba, ext_lba + -(-ext_len // SECTOR), path))
            else:
                if key in names:
                    same = names[key] == path
                    raise ImageError(f"{'the same name twice' if same else 'names differing only in case'}: "
                                     f"{names[key]!r}" + ("" if same else f" and {path!r}"))
                names[key] = path
                if is_dir:
                    subdirs.append((ext_lba, ext_len, path + "/", depth + 1))
                    continue
                if ext_lba * SECTOR + ext_len > size:
                    raise ImageError(f"a file lies outside the image: {path}")
                files[path] = [(ext_lba, ext_len)]
                if ext_len:
                    spans.append((ext_lba, ext_lba + -(-ext_len // SECTOR), path))
            if flags & 0x80:
                if ext_len % SECTOR:
                    raise ImageError(f"a part of a file in several extents isn't whole sectors: {path}")
                multi = path
            else:
                multi = None
        if multi is not None:
            raise ImageError(f"a file in several extents has no last part: {multi}")
        stack.extend(reversed(subdirs))
    spans.sort()
    for (a0, a1, a), (b0, b1, b) in zip(spans, spans[1:]):
        if b0 < a1:   # a disc's files, directories and descriptors each have sectors of their own
            raise ImageError(f"two things share sectors: {a} and {b}")
    return list(files.items())


def stream(f, extents, chunk=CHUNK):
    """a file's bytes, chunk by chunk; ImageError when the image ends before them"""
    for lba, n in extents:
        f.seek(lba * SECTOR)
        while n:
            b = f.read(min(n, chunk))
            if not b:
                raise ImageError("a file is cut short: the image ends before it")
            n -= len(b)
            yield b


def read_file(f, extents, limit=64 << 20):
    if sum(n for _, n in extents) > limit:
        raise ImageError("a file is larger than expected")
    return b"".join(stream(f, extents))


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
            for p, ext in ents:   # names checked by entries(): no component is "." or "..", none holds a separator
                if prefixes and not any(p.startswith(x) for x in prefixes):
                    continue
                dst = os.path.join(out, *p.split("/"))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, "wb") as o:
                    for b in stream(f, ext):
                        o.write(b)


if __name__ == "__main__":
    main()
