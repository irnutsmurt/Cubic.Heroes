#!/usr/bin/env python3
"""The character editor's data file (DATA.EDT, "edit data") of 3D Dot Game Heroes: read, validated, written back.

A player-made character is a save folder BLJM60180T<hex> (the US release uses the Japanese release's edit-data
prefix) holding PARAM.SFO, ICON0.PNG and DATA.EDT. This module reads only DATA.EDT, the character itself, as the game
writes it -- the layout below was read from players' own files (every byte of each accounted for), not from the game:

    "EDTFILE1.9"                     10 bytes, the magic and version
    u16 0                            (always 0 so far)
    u32 n, n bytes                   the character's name, UTF-8 (the game writes full-width letters)
    u32 gender                       0 so far (the editor's first choice)
    u32 n, n bytes                   the class, UTF-8 ("ＨＥＲＯ", "ＰＲＩＮＣＥ", "ＳＣＨＯＬＡＲ", ...)
    f32 x 14                         settings (kept as they are; their meaning is not needed here)
    u32 poses                        6: Stand, Walk 1, Walk 2, Hurray, Attack 1, Attack 2
    per pose:
      u32 n                          voxels
      n x { u8 flag, u8 x, u8 y, u8 z, f32 r, f32 g, f32 b, f32 slot }   20 bytes; x, y, z in 0..15 (y up, the
                                     character facing -z); r, g, b the colour in 0..1; slot the palette entry
      f32 x 6                        a box the editor records (kept as it is)

All numbers big-endian. Nothing is compressed or encrypted: a DATA.EDT copied off a PS3 is still encrypted by the
console's save system (see ps3_save.py), and is not this format until decrypted.

    python tools/edt_format.py <DATA.EDT>...      describe each file (a check that it parses and round-trips)
"""
import hashlib
import struct
import sys

MAGIC = b"EDTFILE1.9"
POSE_NAMES = ("Stand", "Walk 1", "Walk 2", "Hurray", "Attack 1", "Attack 2")
GRID = 16                  # the editor's grid: 16 x 16 x 16 per pose
MAX_SIZE = 2 * 1024 * 1024  # far above a full character (6 x 4096 voxels x 20 bytes = 492 KB)
MAX_STRING = 256


class EdtError(ValueError):
    """Not a character file, or a damaged one; the message says what and where."""


class Voxel(tuple):
    __slots__ = ()
    flag = property(lambda s: s[0])
    x = property(lambda s: s[1])
    y = property(lambda s: s[2])
    z = property(lambda s: s[3])
    rgb = property(lambda s: s[4])     # (r, g, b) floats 0..1, as stored
    slot = property(lambda s: s[5])    # float, as stored


class Pose:
    def __init__(self, voxels, box):
        self.voxels = voxels   # [Voxel]
        self.box = box         # 6 floats, as stored


class Character:
    def __init__(self):
        self.pad = 0
        self.name = ""
        self.gender = 0
        self.klass = ""
        self.settings = (0.0,) * 14
        self.poses = []

    def voxel_count(self):
        return sum(len(p.voxels) for p in self.poses)


def is_plain(data):
    """True when `data` starts as a decrypted character file does."""
    return data[:len(MAGIC)] == MAGIC


def parse(data):
    """A Character from DATA.EDT's bytes; EdtError when it is not one, or is damaged."""
    if len(data) > MAX_SIZE:
        raise EdtError(f"too large for a character ({len(data)} bytes)")
    if not is_plain(data):
        raise EdtError("not a decrypted character file (no EDTFILE1.9 header)")
    o = len(MAGIC)

    def take(fmt):
        nonlocal o
        size = struct.calcsize(fmt)
        if o + size > len(data):
            raise EdtError(f"ends early at byte {o} (needs {size} more)")
        v = struct.unpack_from(fmt, data, o)
        o += size
        return v

    def text(what):
        nonlocal o
        n, = take(">I")
        if n > MAX_STRING or o + n > len(data):
            raise EdtError(f"the {what} at byte {o - 4} is damaged (length {n})")
        try:
            s = data[o:o + n].decode("utf-8")
        except UnicodeDecodeError:
            raise EdtError(f"the {what} at byte {o} is not text")
        o += n
        return s

    c = Character()
    c.pad, = take(">H")
    c.name = text("name")
    c.gender, = take(">I")
    c.klass = text("class")
    c.settings = take(">14f")
    npose, = take(">I")
    if npose != len(POSE_NAMES):
        raise EdtError(f"{npose} poses (a character has {len(POSE_NAMES)})")
    for p in range(npose):
        n, = take(">I")
        if n > GRID ** 3:
            raise EdtError(f"pose {POSE_NAMES[p]}: {n} voxels (at most {GRID ** 3})")
        voxels, seen = [], set()
        for _ in range(n):
            flag, x, y, z, r, g, b, slot = take(">BBBBffff")
            if x >= GRID or y >= GRID or z >= GRID:
                raise EdtError(f"pose {POSE_NAMES[p]}: a voxel outside the grid ({x}, {y}, {z})")
            if (x, y, z) in seen:
                raise EdtError(f"pose {POSE_NAMES[p]}: two voxels at ({x}, {y}, {z})")
            seen.add((x, y, z))
            voxels.append(Voxel((flag, x, y, z, (r, g, b), slot)))
        c.poses.append(Pose(voxels, take(">6f")))
    if o != len(data):
        raise EdtError(f"{len(data) - o} unexpected byte(s) after the last pose")
    return c


def serialize(c):
    """The bytes of DATA.EDT for `c` -- for a parsed file, exactly the bytes it was parsed from."""
    out = [MAGIC, struct.pack(">H", c.pad)]
    for s, extra in ((c.name, None), (c.klass, c.gender)):
        if extra is not None:
            out.append(struct.pack(">I", extra))
        b = s.encode("utf-8")
        out += [struct.pack(">I", len(b)), b]
    out.append(struct.pack(">14f", *c.settings))
    out.append(struct.pack(">I", len(c.poses)))
    for p in c.poses:
        out.append(struct.pack(">I", len(p.voxels)))
        for v in p.voxels:
            out.append(struct.pack(">BBBBffff", v.flag, v.x, v.y, v.z, *v.rgb, v.slot))
        out.append(struct.pack(">6f", *p.box))
    return b"".join(out)


def display_name(s):
    """The game's full-width letters as ordinary ones, for searching and file names ("ＧｒｅｅｎＨｅｒｏ" ->
    "GreenHero"); the stored name is never changed."""
    out = []
    for ch in s:
        o = ord(ch)
        if 0xFF01 <= o <= 0xFF5E:
            out.append(chr(o - 0xFEE0))
        elif o == 0x3000:
            out.append(" ")
        else:
            out.append(ch)
    return "".join(out)


def shape_hash(c):
    """A hash of what the character looks like -- every pose's voxels (position and colour), not its name or
    settings: the same model under another name or file has the same shape hash."""
    h = hashlib.sha256()
    for p in c.poses:
        h.update(struct.pack(">I", len(p.voxels)))
        for v in sorted(p.voxels, key=lambda v: (v.x, v.y, v.z)):
            h.update(struct.pack(">BBBfff", v.x, v.y, v.z, *v.rgb))
    return h.hexdigest()


def describe(c):
    return {
        "name": display_name(c.name), "class": display_name(c.klass), "gender": c.gender,
        "voxels": [len(p.voxels) for p in c.poses],
        "colours": len({v.rgb for p in c.poses for v in p.voxels}),
        "shape_sha256": shape_hash(c),
    }


def main():
    rc = 0
    for path in sys.argv[1:]:
        data = open(path, "rb").read()
        try:
            c = parse(data)
        except EdtError as e:
            print(f"{path}: {e}")
            rc = 1
            continue
        same = serialize(c) == data
        print(f"{path}: {describe(c)}; round trip {'exact' if same else 'DIFFERENT'}")
        rc |= 0 if same else 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
