#!/usr/bin/env python3
"""Hostile disc images against the installer's ISO 9660 extraction (0.2.0-beta.5, the owner's 2026-11-23 instruction:
"Audit and harden the ISO9660 extraction code").

    python -m unittest tests.test_iso_hostile

Every image here is synthetic, made by this file: a few made-up bytes in an ISO 9660 layout, nothing of the game. Each
case runs the installer's real extraction (tools/setup_pipeline.extract, kind "iso") in a child process with a time
limit (and, on POSIX, a memory limit), into a fresh installation folder inside a fresh work folder, and requires:
  - the extraction refuses the image (SetupError) -- or, for the valid control case, extracts it exactly;
  - it ends within the time limit (no endless loop) and within the memory limit;
  - nothing is written outside the installation's disc folder (the work folder's other contents are checked);
  - an existing disc copy is left as it was when the new extraction fails (no damage before verification).
These are the acceptance cases for the hardened extractor; written against the code as it was (2026-11-23), several
fail -- each failure is a defect listed in the Linux review (linux/reports on the coordination share)."""
import multiprocessing
import os
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

SECTOR = 2048
TIME_LIMIT = 30          # seconds per case; the real extraction of these tiny images takes well under one
MEMORY_LIMIT = 1 << 30   # bytes of address space for the child (POSIX); a 4 GB directory read must not fit


def both(fmt, v):
    return struct.pack("<" + fmt, v) + struct.pack(">" + fmt, v)


def record(name, lba, size, flags):
    n = len(name)
    ln = 33 + n + (1 - n % 2)
    return (bytes([ln, 0]) + both("I", lba) + both("I", size) + bytes(7) + bytes([flags, 0, 0]) + both("H", 1)
            + bytes([n]) + name + bytes(ln - 33 - n))


class Image:
    """an ISO 9660 image built by hand: directories and files placed at chosen sectors, any record allowed (a name
    with '/' or '..', an extent past the end, a directory pointing back at an ancestor, a duplicate name)"""

    def __init__(self):
        self.sectors = {}   # lba -> bytes
        self.next = 20
        self.root = None

    def alloc(self, n=1):
        lba = self.next
        self.next += max(1, n)
        return lba

    def file(self, data):
        lba = self.alloc(-(-len(data) // SECTOR))
        for i in range(0, max(1, len(data)), SECTOR):
            self.sectors[lba + i // SECTOR] = data[i:i + SECTOR]
        return lba, len(data)

    def directory(self, lba, parent, entries, size=SECTOR):
        """entries: [(name bytes, lba, size, flags)]"""
        body = record(b"\0", lba, size, 2) + record(b"\1", parent, SECTOR, 2)
        for e in entries:
            body += record(*e)
        assert len(body) <= SECTOR, "one sector per directory in these fixtures"
        self.sectors[lba] = body

    def write(self, path, total=None):
        total = total or self.next
        img = bytearray(total * SECTOR)
        pvd = bytearray(SECTOR)
        pvd[0:7] = b"\x01CD001\x01"
        pvd[80:88] = both("I", total)
        pvd[156:156 + 34] = record(b"\0", self.root, SECTOR, 2)
        img[16 * SECTOR:17 * SECTOR] = pvd
        img[17 * SECTOR:17 * SECTOR + 7] = b"\xffCD001\x01"
        for lba, b in self.sectors.items():
            if lba < total:
                img[lba * SECTOR:lba * SECTOR + len(b)] = b
        with open(path, "wb") as f:
            f.write(img)


EBOOT = b"SCE\x00" + b"made-up program " * 64


def game(extra_usrdir=(), eboot=EBOOT, usrdir_override=None, game_extra=()):
    """the usual PS3_GAME/{PARAM.SFO, USRDIR/EBOOT.BIN} tree with extra USRDIR records; returns the Image"""
    im = Image()
    root, g, usr = im.alloc(), im.alloc(), im.alloc()
    im.root = root
    sfo_lba, sfo_n = im.file(b"\0PSF made-up")
    eb_lba, eb_n = im.file(eboot)
    im.directory(root, root, [(b"PS3_GAME", g, SECTOR, 2)])
    im.directory(g, root, [(b"PARAM.SFO;1", sfo_lba, sfo_n, 0), (b"USRDIR", usr, SECTOR, 2), *game_extra])
    im.directory(usr, g, usrdir_override if usrdir_override is not None else
                 [(b"EBOOT.BIN;1", eb_lba, eb_n, 0), *extra_usrdir])
    return im


def _child(iso, install, q):
    """the real extraction, in its own process (a hang or a memory blow-up stays here)"""
    try:
        if os.name == "posix":
            import resource
            resource.setrlimit(resource.RLIMIT_AS, (MEMORY_LIMIT, MEMORY_LIMIT))
        import hashlib
        import setup_pipeline as pipeline
        want = {"files": {"PS3_GAME/USRDIR/EBOOT.BIN": hashlib.sha256(EBOOT).hexdigest()},
                "disc_image": {"size": 0, "sha256": "0" * 64}, "title_id": "BLUS30490", "app_ver": "01.00",
                "title": "made-up"}
        disc = os.path.join(install, "data", "disc")
        paths = types.SimpleNamespace(disc=disc, eboot_bin=os.path.join(disc, "PS3_GAME", "USRDIR", "EBOOT.BIN"))
        with mock.patch.object(pipeline, "version_info", lambda: want):
            pipeline.extract(iso, "iso", paths, lambda f: None, lambda m: None)
        q.put(("ok", None))
    except MemoryError:
        q.put(("memory", None))
    except RecursionError:
        q.put(("recursion", None))
    except Exception as e:   # noqa: BLE001 -- the kind of refusal is the result
        q.put((type(e).__name__, getattr(e, "code", None)))


def run_extract(iso, install, timeout=TIME_LIMIT):
    q = multiprocessing.Queue()
    p = multiprocessing.Process(target=_child, args=(iso, install, q))
    p.start()
    p.join(timeout)
    if p.is_alive():
        p.kill()
        p.join()
        return ("timeout", None)
    try:
        return q.get(timeout=2)
    except Exception:   # noqa: BLE001
        return ("crashed", p.exitcode)


def listing(top):
    out = set()
    for d, dirs, files in os.walk(top, followlinks=False):
        for n in dirs + files:
            out.add(os.path.relpath(os.path.join(d, n), top))
    return out


class Hostile(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR") or None)
        self.addCleanup(self.tmp.cleanup)
        self.work = self.tmp.name
        self.install = os.path.join(self.work, "install")
        os.makedirs(os.path.join(self.install, "data"))
        with open(os.path.join(self.work, "outside.txt"), "w") as f:
            f.write("must stay as it is")
        self.iso = os.path.join(self.work, "hostile.iso")

    def check_refused(self, im, total=None):
        im.write(self.iso, total)
        before = listing(self.work) - {"hostile.iso"}
        kind, code = run_extract(self.iso, self.install)
        after = listing(self.work) - {"hostile.iso"}
        escaped = sorted(p for p in after - before if not p.startswith(os.path.join("install", "data", "disc")))
        self.assertEqual(escaped, [], "files written outside the installation's disc folder")
        self.assertNotIn(kind, ("timeout", "memory", "recursion", "crashed"), f"the extraction ended by {kind}")
        self.assertEqual(kind, "SetupError", f"not refused: {kind} {code}")
        with open(os.path.join(self.work, "outside.txt")) as f:
            self.assertEqual(f.read(), "must stay as it is")
        return code

    # ---- the control: a valid image extracts exactly
    def test_valid_image_extracts(self):
        game().write(self.iso)
        kind, code = run_extract(self.iso, self.install)
        self.assertEqual((kind, code), ("ok", None))
        with open(os.path.join(self.install, "data", "disc", "PS3_GAME", "USRDIR", "EBOOT.BIN"), "rb") as f:
            self.assertEqual(f.read(), EBOOT)

    # ---- path traversal
    def usrdir(self, im, *records):
        """USRDIR rewritten: EBOOT.BIN and the given extra records"""
        usr = [k for k, v in im.sectors.items() if b"EBOOT.BIN" in v][0]
        eb_lba, eb_n = im.file(EBOOT)
        im.directory(usr, usr - 1, [(b"EBOOT.BIN;1", eb_lba, eb_n, 0), *records])
        return im

    def with_file(self, name, data=b"escaped"):
        im = game()
        lba, n = im.file(data)
        return self.usrdir(im, (name, lba, n, 0))

    def test_dotdot_directory_name(self):
        im = game()
        lba, n = im.file(b"escaped")
        evil = im.alloc()
        im.directory(evil, evil, [(b"EVIL.TXT;1", lba, n, 0)])
        self.check_refused(self.usrdir(im, (b"..", evil, SECTOR, 2)))

    def test_slash_in_name(self):
        self.check_refused(self.with_file(b"../../../../ESCAPED.TXT;1"))

    def test_absolute_name(self):
        self.check_refused(self.with_file(b"/tmp/ESCAPED.TXT;1"))

    def test_backslash_name(self):
        self.check_refused(self.with_file(b"..\\..\\ESCAPED.TXT;1"))

    def test_dot_name(self):
        self.check_refused(self.with_file(b".;1"))

    # ---- endless loops and resource exhaustion
    def test_extent_past_the_end(self):
        im = game()
        lba = im.alloc()
        self.usrdir(im, (b"BIG.DAT;1", lba, 50 * SECTOR, 0))
        self.check_refused(im, total=im.next)   # the image ends right after: 49 sectors are missing

    def test_truncated_image(self):
        im = game()
        im.write(self.iso)
        with open(self.iso, "r+b") as f:
            f.truncate(os.path.getsize(self.iso) - SECTOR // 2)   # the last file's data cut short
        kind, code = run_extract(self.iso, self.install)
        self.assertNotIn(kind, ("timeout", "ok"), f"a truncated image: {kind}")

    def test_directory_loop(self):
        im = game()
        usr = [k for k, v in im.sectors.items() if b"EBOOT.BIN" in v][0]
        self.check_refused(self.usrdir(im, (b"LOOP", usr, SECTOR, 2)))

    def test_huge_directory_length(self):
        im = game(game_extra=[(b"HUGE", 30, 0xFFFFF000, 2)])
        self.check_refused(im)

    # ---- duplicates and collisions
    def test_duplicate_name(self):
        im = game()
        a_lba, a_n = im.file(b"first")
        b_lba, b_n = im.file(b"second")
        self.check_refused(self.usrdir(im, (b"X.DAT;1", a_lba, a_n, 0), (b"X.DAT;1", b_lba, b_n, 0)))

    def test_case_only_collision(self):
        im = game()
        a_lba, a_n = im.file(b"upper")
        b_lba, b_n = im.file(b"lower")
        self.check_refused(self.usrdir(im, (b"DATA.BIN;1", a_lba, a_n, 0), (b"data.bin;1", b_lba, b_n, 0)))

    # ---- links and an existing copy
    @unittest.skipUnless(os.name == "posix", "symbolic links as a non-administrator: POSIX")
    def test_existing_symlink_at_a_destination(self):
        disc_usr = os.path.join(self.install, "data", "disc", "PS3_GAME", "USRDIR")
        os.makedirs(disc_usr)
        target = os.path.join(self.work, "outside.txt")
        os.symlink(target, os.path.join(disc_usr, "EBOOT.BIN"))
        game().write(self.iso)
        run_extract(self.iso, self.install)
        with open(target) as f:
            self.assertEqual(f.read(), "must stay as it is", "the extraction wrote through a symbolic link")

    def test_failed_extraction_keeps_the_existing_copy(self):
        disc_usr = os.path.join(self.install, "data", "disc", "PS3_GAME", "USRDIR")
        os.makedirs(disc_usr)
        with open(os.path.join(disc_usr, "EBOOT.BIN"), "wb") as f:
            f.write(b"the copy already installed")
        im = game(eboot=b"SCE\x00 another program")   # not the expected program: the extraction must fail
        im.write(self.iso)
        kind, _ = run_extract(self.iso, self.install)
        self.assertNotEqual(kind, "ok")
        with open(os.path.join(disc_usr, "EBOOT.BIN"), "rb") as f:
            self.assertEqual(f.read(), b"the copy already installed", "a failed extraction damaged the existing copy")


if __name__ == "__main__":
    unittest.main()
