#!/usr/bin/env python3
"""A disc image rebuilt from the disc's folder (tools/setup_pipeline.py: probe_disc, verify, extract):
    build/venv/Scripts/python.exe -m unittest tests.test_rebuilt_disc
PS3 ISO Tools, makeps3iso and similar write the disc's files into an ISO of another size than the whole-disc dump.
Such an image is accepted as the disc's folder is: the title, version and program are checked when it is chosen, the
files the version record lists as they are extracted. Made-up images and a made-up version record (nothing of the
game); also the disc key chosen in place of the keys file, refused with a message that says so."""
import hashlib
import os
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import setup_pipeline as pipeline  # noqa: E402

SECTOR = 2048
EBOOT = b"SCE\x00" + b"made-up program" * 300
LUA = b"-- made-up script\n" * 50


def sfo(entries):
    keys, data, index = b"", b"", b""
    for k, v in entries.items():
        v = v.encode() + b"\0"
        index += struct.pack("<HHIII", len(keys), 0x0204, len(v), len(v), len(data))
        keys += k.encode() + b"\0"
        data += v
    key_tab = 20 + len(index)
    return struct.pack("<4sIIII", b"\0PSF", 0x101, key_tab, key_tab + len(keys), len(entries)) + index + keys + data


SFO = sfo({"APP_VER": "01.00", "TITLE": "3D DOT GAME HEROES", "TITLE_ID": "BLUS30490"})


def both(fmt_le, v):
    return struct.pack("<" + fmt_le, v) + struct.pack(">" + fmt_le, v)


def dir_record(name, lba, size, is_dir):
    n = len(name)
    ln = 33 + n + (1 - n % 2)
    return (bytes([ln, 0]) + both("I", lba) + both("I", size) + bytes(7) + bytes([2 if is_dir else 0, 0, 0])
            + both("H", 1) + bytes([n]) + name + bytes(ln - 33 - n))


def make_iso(path, files, pad_sectors=0):
    """a minimal ISO 9660 image: PS3_GAME/{PARAM.SFO, USRDIR/{EBOOT.BIN, data/all.lua.eu}} (files: {path: bytes})"""
    root, game, usr, data = 18, 19, 20, 21
    at = 22
    place = {}
    for p, b in files.items():
        place[p] = at
        at += max(1, -(-len(b) // SECTOR))
    total = at + pad_sectors

    def directory(me, parent, entries):
        body = dir_record(b"\0", me, SECTOR, True) + dir_record(b"\1", parent, SECTOR, True)
        for name, lba, size, is_dir in entries:
            body += dir_record(name, lba, size, is_dir)
        return body.ljust(SECTOR, b"\0")

    img = bytearray(total * SECTOR)
    pvd = bytearray(SECTOR)
    pvd[0:7] = b"\x01CD001\x01"
    pvd[80:88] = both("I", total)
    pvd[156:156 + 34] = dir_record(b"\0", root, SECTOR, True)
    img[16 * SECTOR:17 * SECTOR] = pvd
    img[17 * SECTOR:17 * SECTOR + 7] = b"\xffCD001\x01"
    img[root * SECTOR:(root + 1) * SECTOR] = directory(root, root, [(b"PS3_GAME", game, SECTOR, True)])
    img[game * SECTOR:(game + 1) * SECTOR] = directory(game, root, [
        (b"PARAM.SFO;1", place["PS3_GAME/PARAM.SFO"], len(files["PS3_GAME/PARAM.SFO"]), False),
        (b"USRDIR", usr, SECTOR, True)])
    img[usr * SECTOR:(usr + 1) * SECTOR] = directory(usr, game, [
        (b"EBOOT.BIN;1", place["PS3_GAME/USRDIR/EBOOT.BIN"], len(files["PS3_GAME/USRDIR/EBOOT.BIN"]), False),
        (b"data", data, SECTOR, True)])
    img[data * SECTOR:(data + 1) * SECTOR] = directory(data, usr, [
        (b"all.lua.eu;1", place["PS3_GAME/USRDIR/data/all.lua.eu"], len(files["PS3_GAME/USRDIR/data/all.lua.eu"]),
         False)])
    for p, b in files.items():
        img[place[p] * SECTOR:place[p] * SECTOR + len(b)] = b
    with open(path, "wb") as f:
        f.write(img)


def sha(b):
    return hashlib.sha256(b).hexdigest()


FILES = {"PS3_GAME/PARAM.SFO": SFO, "PS3_GAME/USRDIR/EBOOT.BIN": EBOOT, "PS3_GAME/USRDIR/data/all.lua.eu": LUA}
WANT = {"title": "3D DOT GAME HEROES", "title_id": "BLUS30490", "app_ver": "01.00",
        "disc_image": {"size": 1 << 30, "sha256": "0" * 64},
        "files": {p: sha(b) for p, b in FILES.items()}, "decrypted_elf": {"sha256": "0" * 64}}


class RebuiltDisc(unittest.TestCase):
    def setUp(self):
        p = mock.patch.object(pipeline, "version_info", lambda: WANT)
        p.start()
        self.addCleanup(p.stop)
        # the game's file list (config/disc_manifest.json), made up the same way: the made-up files' sizes and SHA-256
        m = {"format": 1, "files": [{"path": k, "size": len(b), "sha256": sha(b)} for k, b in sorted(FILES.items())]}
        p = mock.patch.object(pipeline.disc_source, "manifest", lambda path=None: m)
        p.start()
        self.addCleanup(p.stop)
        self.tmp = tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR") or None)
        self.addCleanup(self.tmp.cleanup)
        self.d = self.tmp.name

    def paths(self):
        disc = os.path.join(self.d, "install", "disc")
        return types.SimpleNamespace(disc=disc, eboot_bin=os.path.join(disc, "PS3_GAME", "USRDIR", "EBOOT.BIN"))

    def test_rebuilt_image_accepted_and_extracted(self):
        iso = os.path.join(self.d, "rebuilt.iso")
        make_iso(iso, FILES, pad_sectors=7)
        self.assertNotEqual(os.path.getsize(iso), WANT["disc_image"]["size"])
        info = pipeline.probe_disc(iso)
        self.assertEqual((info["title_id"], info["app_ver"], info["kind"]), ("BLUS30490", "01.00", "iso"))
        self.assertEqual(pipeline.verify(iso, lambda f: None), "iso")
        paths = self.paths()
        pipeline.extract(iso, "iso", paths, lambda f: None, lambda m: None)
        for p, b in FILES.items():
            with open(os.path.join(paths.disc, *p.split("/")), "rb") as f:
                self.assertEqual(f.read(), b, p)

    def test_rebuilt_image_with_a_changed_file_refused_at_extraction(self):
        iso = os.path.join(self.d, "modified.iso")
        make_iso(iso, dict(FILES, **{"PS3_GAME/USRDIR/data/all.lua.eu": b"-- changed\n"}))
        self.assertEqual(pipeline.verify(iso, lambda f: None), "iso")   # title, version and program are right
        with self.assertRaises(pipeline.SetupError) as e:
            pipeline.extract(iso, "iso", self.paths(), lambda f: None, lambda m: None)
        self.assertEqual(e.exception.code, "damaged_disc")
        self.assertIn("all.lua.eu", str(e.exception))

    def test_rebuilt_image_with_another_program_refused_when_chosen(self):
        iso = os.path.join(self.d, "other.iso")
        make_iso(iso, dict(FILES, **{"PS3_GAME/USRDIR/EBOOT.BIN": b"SCE\x00another program"}))
        for check in (lambda: pipeline.probe_disc(iso), lambda: pipeline.verify(iso, lambda f: None)):
            with self.assertRaises(pipeline.SetupError) as e:
                check()
            self.assertEqual(e.exception.code, "damaged_disc")

    def test_folder_with_a_changed_file_refused_at_extraction(self):
        src = os.path.join(self.d, "folder")
        for p, b in dict(FILES, **{"PS3_GAME/PARAM.SFO": SFO + b"x"}).items():
            os.makedirs(os.path.dirname(os.path.join(src, p)), exist_ok=True)
            with open(os.path.join(src, p), "wb") as f:
                f.write(b)
        with self.assertRaises(pipeline.SetupError) as e:
            pipeline.extract(src, "folder", self.paths(), lambda f: None, lambda m: None)
        self.assertEqual(e.exception.code, "damaged_disc")

    def test_disc_key_chosen_as_the_keys_file(self):
        raw = os.path.join(self.d, "game.key")
        with open(raw, "wb") as f:
            f.write(os.urandom(16))
        dkey = os.path.join(self.d, "game.dkey")
        with open(dkey, "w") as f:
            f.write(os.urandom(16).hex().upper() + "\r\n")
        for k in (raw, dkey):
            with self.assertRaises(pipeline.SetupError) as e:
                pipeline.decrypt_eboot(EBOOT, keys=k)
            self.assertEqual(e.exception.code, "keys_unreadable")
            self.assertIn("disc key", str(e.exception))


if __name__ == "__main__":
    unittest.main()
