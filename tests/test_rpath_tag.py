#!/usr/bin/env python3
"""tools/zig_build.py rpath_tag: a linked program's DT_RUNPATH turned into DT_RPATH in place (M11.4: Zig writes
DT_RUNPATH for an executable whatever the flags; LD_LIBRARY_PATH outranks it, DT_RPATH it does not)."""
import os
import shutil
import struct
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import zig_build  # noqa: E402

DT_NULL, DT_NEEDED, DT_RPATH, DT_RUNPATH = 0, 1, 15, 29


def elf(entries, little=True, cls=2):
    """a minimal ELF64: the header, one PT_DYNAMIC program header, the dynamic entries"""
    dyn_off = 64 + 56
    head = bytearray(64)
    head[:4], head[4], head[5] = b"\x7fELF", cls, 1 if little else 2
    struct.pack_into("<Q", head, 32, 64)                 # e_phoff
    struct.pack_into("<HH", head, 54, 56, 1)              # e_phentsize, e_phnum
    ph = bytearray(56)
    struct.pack_into("<I", ph, 0, 2)                      # PT_DYNAMIC
    struct.pack_into("<QQ", ph, 8, dyn_off, dyn_off)      # p_offset, p_vaddr
    struct.pack_into("<Q", ph, 32, 16 * len(entries))     # p_filesz
    body = b"".join(struct.pack("<qQ", t, v) for t, v in entries)
    return bytes(head) + bytes(ph) + body


def tags(path):
    data = open(path, "rb").read()
    off = 64 + 56
    out = []
    while off + 16 <= len(data):
        t, _ = struct.unpack_from("<qQ", data, off)
        out.append(t)
        if t == DT_NULL:
            break
        off += 16
    return out


class RpathTag(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write(self, data):
        p = os.path.join(self.tmp, "prog")
        with open(p, "wb") as f:
            f.write(data)
        return p

    def test_runpath_becomes_rpath(self):
        p = self.write(elf([(DT_NEEDED, 1), (DT_RUNPATH, 7), (DT_NULL, 0)]))
        self.assertEqual(zig_build.rpath_tag(p), 1)
        self.assertEqual(tags(p), [DT_NEEDED, DT_RPATH, DT_NULL])

    def test_already_rpath_or_none_is_left(self):
        for entries in ([(DT_RPATH, 7), (DT_NULL, 0)], [(DT_NEEDED, 1), (DT_NULL, 0)]):
            p = self.write(elf(entries))
            before = open(p, "rb").read()
            self.assertEqual(zig_build.rpath_tag(p), 0)
            self.assertEqual(open(p, "rb").read(), before)

    def test_not_elf64_le_is_left(self):
        for data in (b"MZ" + bytes(200), elf([(DT_RUNPATH, 7), (DT_NULL, 0)], little=False)):
            p = self.write(data)
            self.assertEqual(zig_build.rpath_tag(p), 0)
            self.assertEqual(open(p, "rb").read(), data)

    def test_both_tags_refused(self):
        p = self.write(elf([(DT_RPATH, 7), (DT_RUNPATH, 7), (DT_NULL, 0)]))
        with self.assertRaises(SystemExit):
            zig_build.rpath_tag(p)


if __name__ == "__main__":
    unittest.main()
