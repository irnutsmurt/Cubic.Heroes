#!/usr/bin/env python3
"""Encrypted and decrypted PS3 disc images (tools/ps3_disc.py; docs/M9_LAUNCHER_PLAN.md section 22):
    build/venv/Scripts/python.exe -m unittest tests.test_ps3_disc
Made-up images and keys (random bytes, never a real key): the decrypting view, the region table, the key from a .dkey
file (raw or hex), typed, or from an IRD file through a D1 line of a keys file (a made-up constant), the plain errors
that never quote a key. With the player's own decrypted ISO beside the checkout (DISC_ISO, optional): it reads as
"decrypted"; an encrypted original simulated from it in memory (the file system plain, the rest encrypted with a
random key -- nothing written) reads as "encrypted", the key is checked before use, and through the view the game's
program and PARAM.SFO read as the decrypted image's."""
import gzip
import io
import os
import struct
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import ps3_disc as pd  # noqa: E402

ISO = os.environ.get("DISC_ISO", os.path.join(ROOT, "3D DOT GAME HEROES [BLUS-30490].iso"))


def enc(key, n, data):
    return pd._aes(key, pd.sector_iv(n), data, decrypt=False)


class EncryptedView(io.RawIOBase):
    """a decrypted image seen as its encrypted original: sector 0's table says (0, plain_end) is plain, every later
    sector is encrypted with key"""

    def __init__(self, f, key, plain_end):
        super().__init__()
        self.f, self.key, self.plain_end, self.pos = f, key, plain_end, 0
        f.seek(0, os.SEEK_END)
        self.size = f.tell()

    def seekable(self):
        return True

    def readable(self):
        return True

    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else self.pos + off if whence == 1 else self.size + off
        return self.pos

    def tell(self):
        return self.pos

    def read(self, n=-1):
        n = self.size - self.pos if n is None or n < 0 else min(n, self.size - self.pos)
        out = bytearray()
        while n > 0:
            sec, off = divmod(self.pos, pd.SECTOR)
            self.f.seek(sec * pd.SECTOR)
            raw = self.f.read(pd.SECTOR)
            if sec == 0:
                raw = struct.pack(">II", 1, 0) + struct.pack(">II", 0, self.plain_end) + raw[16:]
            elif sec > self.plain_end and len(raw) == pd.SECTOR:
                raw = enc(self.key, sec, raw)
            take = raw[off:off + n]
            if not take:
                break
            out += take
            self.pos += len(take)
            n -= len(take)
        return bytes(out)


class DiscTest(unittest.TestCase):
    def test_view_decrypts_only_outside_plain_regions(self):
        key = os.urandom(16)
        s0 = bytearray(pd.SECTOR)
        struct.pack_into(">II", s0, 0, 1, 0)
        struct.pack_into(">II", s0, 8, 0, 1)   # sectors 0-1 plain
        plain = [bytes(s0), os.urandom(pd.SECTOR)] + [os.urandom(pd.SECTOR) for _ in range(4)]
        stored = plain[:2] + [enc(key, i + 2, s) for i, s in enumerate(plain[2:])]
        f = io.BytesIO(b"".join(stored))
        self.assertEqual(pd.plain_regions(f), [(0, 1)])
        v = pd.DiscImage(f, key)
        self.assertEqual(v.read(), b"".join(plain))
        v.seek(3 * pd.SECTOR + 100)
        self.assertEqual(v.read(50), plain[3][100:150])
        self.assertEqual(pd.DiscImage(f, None).read(), b"".join(stored))

    def test_keys(self):
        with tempfile.TemporaryDirectory() as d:
            k = os.urandom(16)
            raw, hx = os.path.join(d, "a.dkey"), os.path.join(d, "b.dkey")
            with open(raw, "wb") as f:
                f.write(k)
            with open(hx, "w") as f:
                f.write(k.hex().upper() + "\n")
            self.assertEqual(pd.load_key(raw), k)
            self.assertEqual(pd.load_key(hx), k)
            self.assertEqual(pd.load_key(" " + k.hex() + " "), k)
            for bad in ("1234", "zz" * 16):
                with self.assertRaises(pd.DiscKeyError) as e:
                    pd.load_key(bad)
                self.assertNotIn(bad, str(e.exception))
            # an IRD file (version 9), its data1 turned into the key by a D1 line of the keys file (made-up values)
            data1, d1k, d1iv = os.urandom(16), os.urandom(16), os.urandom(16)
            body = (b"3IRD" + bytes([9]) + b"BLUS30490" + bytes([4]) + b"Test" + b"0000" + b"01.00" + b"01.00" +
                    struct.pack("<I", 3) + b"hdr" + struct.pack("<I", 2) + b"ft" + bytes([1]) + os.urandom(16) +
                    struct.pack("<I", 1) + os.urandom(24) + b"\x00\x00\x00\x00" + os.urandom(115) + data1 +
                    os.urandom(16) + b"\x00" * 8)
            ird = os.path.join(d, "x.ird")
            with open(ird, "wb") as f:
                f.write(gzip.compress(body))
            with self.assertRaises(pd.DiscKeyError) as e:
                pd.load_key(ird)
            self.assertEqual(e.exception.code, "needs_d1")
            keys = os.path.join(d, "keys.txt")
            with open(keys, "w") as f:
                f.write(f"APP 0000 {'00' * 32} {'00' * 16}\nD1 {d1k.hex()} {d1iv.hex()}\n")
            self.assertEqual(pd.load_key(ird, keys), pd._aes(d1k, d1iv, data1, decrypt=False))

    @unittest.skipUnless(os.path.exists(ISO), "the player's decrypted ISO isn't beside the checkout")
    def test_real_disc_decrypted_and_simulated_encrypted(self):
        import iso9660
        with open(ISO, "rb") as f:
            self.assertEqual(pd.kind(f), "decrypted")
            files = dict(iso9660.entries(f))
            eboot, sfo = files["PS3_GAME/USRDIR/EBOOT.BIN"][0][0], files["PS3_GAME/PARAM.SFO"][0][0]
            key = os.urandom(16)
            view = EncryptedView(f, key, plain_end=2000)
            self.assertGreater(eboot, 2000)
            self.assertEqual(pd.kind(view), "encrypted")
            self.assertTrue(pd.check_key(view, key))
            self.assertFalse(pd.check_key(view, os.urandom(16)))
            img = pd.DiscImage(view, key)
            img.seek(eboot * pd.SECTOR)
            self.assertTrue(img.read(4).startswith(pd.SCE_MAGIC))
            f.seek(sfo * pd.SECTOR)
            want = f.read(4096)   # read first: the view moves the same file as it reads
            img.seek(sfo * pd.SECTOR)
            self.assertEqual(img.read(4096), want)
            self.assertTrue(want.startswith(b"\x00PSF"))


if __name__ == "__main__":
    unittest.main()
