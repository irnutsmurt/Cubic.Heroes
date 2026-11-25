"""The official update's readers, with made-up keys only: tools/player_keys.py (the player's keys file),
tools/pkg_extract.py (a PS3 retail package) and tools/self_decrypt.py (a plain and an NPDRM free-licence program).
Every package and program here is built by the test itself; no Sony key, package or program is used or needed."""
import hashlib
import hmac
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

from Crypto.Cipher import AES
from Crypto.Util import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import pkg_extract  # noqa: E402
import player_keys  # noqa: E402
import self_decrypt  # noqa: E402

TMP = os.path.join(ROOT, "build", "tmp")


def rnd(n):
    return os.urandom(n)


def ctr_crypt(key, iv, data):
    return AES.new(key, AES.MODE_CTR, counter=Counter.new(128, initial_value=int.from_bytes(iv, "big"))).encrypt(data)


def align(n, a=16):
    return (n + a - 1) // a * a


# ---------------- a made-up retail package ----------------

def make_pkg(key, files, dirs=(), revision=0x8000, pkg_type=1, riv=None, kinds=None):
    """files: [(name, bytes)]; dirs: [name] -> the package's bytes, laid out as a retail PS3 package is"""
    riv = riv or rnd(16)
    entries = [(d, b"", 4) for d in dirs] + [(n, b, (kinds or {}).get(n, 3)) for n, b in files]
    off = len(entries) * 0x20
    name_offs, data_offs = [], []
    for n, _, _ in entries:
        name_offs.append(off)
        off += align(len(n.encode()))
    for _, b, _ in entries:
        data_offs.append(off)
        off += align(len(b))
    plain = bytearray(off)
    for i, (n, b, kind) in enumerate(entries):
        nb = n.encode()
        struct.pack_into(">IIQQII", plain, i * 0x20, name_offs[i], len(nb), data_offs[i], len(b), kind, 0)
        plain[name_offs[i]:name_offs[i] + len(nb)] = nb
        plain[data_offs[i]:data_offs[i] + len(b)] = b
    meta = b"".join(struct.pack(">II", pid, len(body)) + body for pid, body in
                    [(1, struct.pack(">I", 3)), (2, struct.pack(">I", 4)), (3, struct.pack(">I", 0x85A)),
                     (4, struct.pack(">Q", len(plain))), (8, bytes.fromhex("0203000001000101"))])
    data_off = align(0xC0 + len(meta), 0x80)
    total = data_off + len(plain) + 0x40 + 0x20
    h = bytearray(0xC0)
    h[0:4] = b"\x7fPKG"
    struct.pack_into(">HHIIIIQQQ", h, 4, revision, pkg_type, 0xC0, 5, len(meta), len(entries), total, data_off,
                     len(plain))
    cid = b"UP0005-BLUS30490_00-TESTPATCH0000001"
    h[0x30:0x30 + len(cid)] = cid
    h[0x70:0x80] = riv
    h[0xB8:0xC0] = hashlib.sha1(bytes(h[:0x80])).digest()[-8:]
    body = bytes(h) + meta + bytes(data_off - 0xC0 - len(meta)) + ctr_crypt(key, riv, bytes(plain)) + bytes(0x40)
    return body + hashlib.sha1(body).digest() + bytes(12)


# ---------------- a made-up program (SELF) ----------------

def make_elf(segments):
    """segments: [bytes] -> a big-endian ELF64 with one PT_LOAD per segment, its bytes"""
    n = len(segments)
    offs, off = [], 0x1000
    for s in segments:
        offs.append(off)
        off = align(off + len(s), 0x1000)
    out = bytearray(offs[-1] + len(segments[-1]))
    out[0:4] = b"\x7fELF"
    out[4:8] = bytes([2, 2, 1, 0x66])
    struct.pack_into(">HHIQQQIHHHHHH", out, 0x10, 2, 21, 1, 0x10000, 0x40, 0, 0, 0x40, 56, n, 0x40, 0, 0)
    for i, (s, o) in enumerate(zip(segments, offs)):
        struct.pack_into(">IIQQQQQQ", out, 0x40 + 56 * i, 1, 5, o, 0x10000 + o, 0x10000 + o, len(s), len(s),
                         0x10000)
        out[o:o + len(s)] = s
    return bytes(out)


def make_self(elf, erk, riv, keyrev=0x1, npdrm=None, licence=3, compress=(True,)):
    """elf -> a SELF of it. npdrm: (klic_free, klic_key) makes it an NPDRM program (self type 8) whose metadata
    info is wrapped once more with the unwrapped licence key, as a downloaded program's is"""
    phnum = struct.unpack_from(">H", elf, 0x38)[0]
    phdrs = [struct.unpack_from(">IIQQQQQQ", elf, 0x40 + 56 * i) for i in range(phnum)]
    app_off, elf_off = 0x70, 0x90
    phdr_off = elf_off + 0x40
    ctrl_off = align(phdr_off + 56 * phnum)
    ctrl = struct.pack(">IIQ", 1, 0x30, 1) + bytes(0x20)                     # control flags
    if npdrm:
        npd = bytearray(0x80)
        npd[0:4] = b"NPD\0"
        struct.pack_into(">III", npd, 4, 3, licence, 0)
        npd[0x10:0x10 + 36] = b"UP0005-BLUS30490_00-TESTPATCH0000001"
        ctrl += struct.pack(">IIQ", 3, 0x90, 0) + bytes(npd)
    mi_off = align(ctrl_off + len(ctrl))
    mkey, miv = rnd(16), rnd(16)
    secs, keys, blobs = [], [], []
    for i, p in enumerate(phdrs):
        seg = elf[p[2]:p[2] + p[5]]
        comp = compress[i % len(compress)]
        stored = zlib.compress(seg) if comp else seg
        skey, siv, hkey = rnd(16), rnd(16), rnd(0x40)
        k0 = len(keys)
        keys += [skey, siv]
        digest = hmac.new(hkey, stored, hashlib.sha1).digest()
        sha = (digest + bytes(12)) + hkey
        keys += [sha[j:j + 16] for j in range(0, 0x60, 16)]
        blobs.append(ctr_crypt(skey, siv, stored))
        secs.append([0, len(blobs[-1]), 2, i, 2, k0 + 2, 3, k0, k0 + 1, 2 if comp else 1])
    hsize = align(mi_off + 0x40 + 0x20 + 0x30 * len(secs) + 16 * len(keys))
    off = hsize
    for s, b in zip(secs, blobs):
        s[0] = off
        off = align(off + len(b))
    mh = struct.pack(">QIIIIII", 0, 1, len(secs), len(keys), 0, 0, 0)
    mh += b"".join(struct.pack(">QQIIIIIIII", *s) for s in secs) + b"".join(keys)
    mi = mkey + bytes(16) + miv + bytes(16)
    mi = AES.new(erk, AES.MODE_CBC, riv).encrypt(mi)
    self_type = 4
    if npdrm:
        klic = AES.new(npdrm[1], AES.MODE_ECB).decrypt(npdrm[0])
        mi = AES.new(klic, AES.MODE_CBC, bytes(16)).encrypt(mi)
        self_type = 8
    d = bytearray(off)
    struct.pack_into(">4sIHHIQQ", d, 0, b"SCE\0", 2, keyrev, 1, mi_off - 0x20, hsize, off - hsize)
    struct.pack_into(">9Q", d, 0x20, 3, app_off, elf_off, phdr_off, 0, 0, 0, ctrl_off, len(ctrl))
    struct.pack_into(">QIIQ", d, app_off, 0x1010000001000003, 1, self_type, 0x0001000000000000)
    d[elf_off:elf_off + 0x40] = elf[:0x40]
    d[phdr_off:phdr_off + 56 * phnum] = elf[0x40:0x40 + 56 * phnum]
    d[ctrl_off:ctrl_off + len(ctrl)] = ctrl
    d[mi_off:mi_off + 0x40] = mi
    d[mi_off + 0x40:mi_off + 0x40 + len(mh)] = ctr_crypt(mkey, miv, mh)
    for s, b in zip(secs, blobs):
        d[s[0]:s[0] + len(b)] = b
    return bytes(d)


def keys_text(ring):
    lines = [f"APP {r:x} {e.hex()} {v.hex()}" for r, (e, v) in ring.get("app", {}).items()]
    lines += [f"NPDRM {r:x} {e.hex()} {v.hex()}" for r, (e, v) in ring.get("npdrm", {}).items()]
    lines += [f"{k} {ring[k].hex()}" for k in ("PKG", "KLIC_FREE", "KLIC_KEY") if k in ring]
    return "\n".join(lines) + "\n"


class Base(unittest.TestCase):
    def setUp(self):
        os.makedirs(TMP, exist_ok=True)
        self.dir = tempfile.mkdtemp(prefix="pkgupd-", dir=TMP)

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def write(self, name, data):
        p = os.path.join(self.dir, name)
        with open(p, "wb" if isinstance(data, bytes) else "w", **({} if isinstance(data, bytes) else
                                                                    {"newline": "\n"})) as f:
            f.write(data)
        return p

    def assertNoKey(self, text, *keys):
        for k in keys:
            self.assertNotIn(k.hex(), text.lower())
            self.assertNotIn(k.hex().upper(), text)


class TestKeysFile(Base):
    def test_every_kind(self):
        a, n = (rnd(32), rnd(16)), (rnd(32), rnd(16))
        p, kf, kk = rnd(16), rnd(16), rnd(16)
        text = ("\ufeff# my keys\n" + f"APP 1 {a[0].hex()} {a[1].hex()}  # disc\n" +
                f"npdrm 0x3 {n[0].hex().upper()} {n[1].hex()}\n" + f"PKG_KEY {p.hex()}\n" +
                f"NP_KLIC_FREE {kf.hex()}\nKLIC_KEY {kk.hex()}\n" + "[metldr]\ntype=SELF\nNOTAKEY x y z w\n")
        k = player_keys.load(self.write("k.txt", text.encode("utf-8")))
        self.assertEqual(k.app, {1: a})
        self.assertEqual(k.npdrm, {3: n})
        self.assertEqual((k.pkg, k.klic_free, k.klic_key), (p, kf, kk))
        self.assertTrue(k.has_update_keys())
        self.assertNoKey(repr(k), a[0], a[1], n[0], p, kf, kk)

    def test_app_only_is_not_enough_for_the_update(self):
        k = player_keys.parse(f"APP 1 {rnd(32).hex()} {rnd(16).hex()}\n")
        self.assertFalse(k.has_update_keys())
        self.assertIsNone(k.pkg)

    def test_bad_lines_name_the_line_never_the_value(self):
        good = rnd(16).hex()
        for text, line in [(f"PKG {good[:-2]}\n", 1), (f"# x\nKLIC_FREE {good} extra\n", 2),
                           (f"APP 1 {rnd(32).hex()}\n", 1), (f"NPDRM zz {rnd(32).hex()} {rnd(16).hex()}\n", 1),
                           (f"\n\nPKG {good[:-1]}g\n", 3)]:
            with self.assertRaises(player_keys.KeysError) as c:
                player_keys.parse(text)
            self.assertEqual(c.exception.line, line)
            self.assertIn(f"Line {line}", str(c.exception))
            self.assertNotIn(good[:16], str(c.exception))


class TestPackage(Base):
    def setUp(self):
        super().setUp()
        self.key = rnd(16)
        self.files = [("USRDIR/EBOOT.BIN", rnd(5000)), ("PARAM.SFO", rnd(1092)), ("USRDIR/data/a.dat", b""),
                      ("USRDIR/data/sub/b.dat", rnd(70001)), ("ICON0.PNG", rnd(33))]
        self.path = self.write("u.pkg", make_pkg(self.key, self.files, ["USRDIR", "USRDIR/data", "USRDIR/data/sub"]))

    def test_header_and_digest_without_a_key(self):
        p = pkg_extract.Package(self.path)
        p.verify()
        i = p.info()
        self.assertEqual(i["content_id"], "UP0005-BLUS30490_00-TESTPATCH0000001")
        self.assertEqual(i["items"], 8)
        self.assertEqual(i["metadata"]["drm_type"], "00000003")
        self.assertEqual(i["metadata"]["software_revision"], "0203000001000101")

    def test_items_and_extract(self):
        p = pkg_extract.Package(self.path)
        items = p.items(self.key)
        self.assertEqual([i.name for i in items if i.is_dir], ["USRDIR", "USRDIR/data", "USRDIR/data/sub"])
        self.assertEqual({i.name: i.size for i in items if not i.is_dir}, {n: len(b) for n, b in self.files})
        seen = []
        got = p.extract(self.key, os.path.join(self.dir, "out"), progress=lambda d, t: seen.append((d, t)))
        self.assertEqual(sorted(got), sorted((n, len(b), hashlib.sha256(b).hexdigest()) for n, b in self.files))
        for n, b in self.files:
            with open(os.path.join(self.dir, "out", *n.split("/")), "rb") as f:
                self.assertEqual(f.read(), b)
        self.assertEqual(seen[-1][0], seen[-1][1])
        self.assertFalse([f for _, _, fs in os.walk(self.dir) for f in fs if f.endswith(".part")])

    def test_unaligned_chunks(self):
        old = pkg_extract.CHUNK
        try:
            pkg_extract.CHUNK = 37        # every chunk starts inside an AES block
            got = pkg_extract.Package(self.path).extract(self.key, os.path.join(self.dir, "o2"), only="USRDIR/data")
        finally:
            pkg_extract.CHUNK = old
        self.assertEqual({n for n, _, _ in got}, {"USRDIR/data/a.dat", "USRDIR/data/sub/b.dat"})
        with open(os.path.join(self.dir, "o2", "USRDIR", "data", "sub", "b.dat"), "rb") as f:
            self.assertEqual(f.read(), self.files[3][1])

    def test_wrong_or_missing_key(self):
        p = pkg_extract.Package(self.path)
        for k in (rnd(16), None, b"short"):
            with self.assertRaises(pkg_extract.PkgError) as c:
                p.items(k)
            self.assertEqual(c.exception.code, "wrong_key")
            self.assertNoKey(str(c.exception), self.key)

    def test_damage_is_found_without_a_key(self):
        data = bytearray(open(self.path, "rb").read())
        data[len(data) // 2] ^= 1
        with self.assertRaises(pkg_extract.PkgError) as c:
            pkg_extract.Package(self.write("bad.pkg", bytes(data))).verify()
        self.assertEqual(c.exception.code, "damaged")
        hdr = bytearray(open(self.path, "rb").read())
        hdr[0x40] ^= 1
        with self.assertRaises(pkg_extract.PkgError) as c:
            pkg_extract.Package(self.write("bad2.pkg", bytes(hdr))).verify()
        self.assertEqual(c.exception.code, "damaged")
        with self.assertRaises(pkg_extract.PkgError) as c:
            pkg_extract.Package(self.write("short.pkg", open(self.path, "rb").read()[:-100]))
        self.assertEqual(c.exception.code, "damaged")

    def test_refused_kinds(self):
        for data, code in [(b"not a package" * 20, "not_a_package"),
                           (make_pkg(self.key, self.files, revision=0), "unsupported"),
                           (make_pkg(self.key, self.files, pkg_type=2), "unsupported")]:
            with self.assertRaises(pkg_extract.PkgError) as c:
                pkg_extract.Package(self.write("x.pkg", data))
            self.assertEqual(c.exception.code, code)

    def test_names_outside_the_folder_are_refused(self):
        for bad in ("../evil.dat", "/abs.dat", "C:/x.dat", "a\\b.dat", "a//b.dat", "a/./b", "x\x01y"):
            p = pkg_extract.Package(self.write("n.pkg", make_pkg(self.key, [("ok.dat", b"1"), (bad, b"2")])))
            with self.assertRaises(pkg_extract.PkgError) as c:
                p.extract(self.key, os.path.join(self.dir, "o3"))
            self.assertEqual(c.exception.code, "unsafe_name", bad)
            self.assertFalse(os.path.exists(os.path.join(self.dir, "evil.dat")))

    def test_command_line_reads_the_key_from_the_file_only(self):
        kp = self.write("keys.txt", keys_text({"PKG": self.key}))
        out = os.path.join(self.dir, "cli")
        r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "pkg_extract.py"), "extract", self.path,
                            "--keys", kp, "--out", out], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len(r.stdout.splitlines()), len(self.files))
        self.assertNoKey(r.stdout + r.stderr, self.key)
        r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "pkg_extract.py"), "list", self.path,
                            "--keys", self.write("k2.txt", keys_text({"PKG": rnd(16)}))],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 2)
        self.assertIn("doesn't open this package", r.stderr)


class TestProgram(Base):
    def setUp(self):
        super().setUp()
        self.elf = make_elf([rnd(9000), b"\0" * 3000 + rnd(500)])
        self.erk, self.riv = rnd(32), rnd(16)

    def test_plain_application_as_before(self):
        s = make_self(self.elf, self.erk, self.riv, keyrev=1)
        self.assertEqual(self_decrypt.self_info(s), (4, 1, None))
        self.assertEqual(self_decrypt.decrypt_self(s, {1: (self.erk, self.riv)}), self.elf)
        # a key need not be labelled with its revision (beta.6): every one of the kind is tried
        self.assertEqual(self_decrypt.decrypt_self(s, {2: (self.erk, self.riv)}), self.elf)
        with self.assertRaises(ValueError):
            self_decrypt.decrypt_self(s, {1: (rnd(32), self.riv)})

    def test_npdrm_free_licence(self):
        kf, kk = rnd(16), rnd(16)
        s = make_self(self.elf, self.erk, self.riv, keyrev=0x0A, npdrm=(kf, kk), compress=(True, False))
        self.assertEqual(self_decrypt.self_info(s), (8, 0x0A, 3))
        ring = player_keys.parse(keys_text({"npdrm": {0x0A: (self.erk, self.riv)}, "KLIC_FREE": kf, "KLIC_KEY": kk}))
        self.assertEqual(self_decrypt.decrypt_self(s, {}, ring), self.elf)

    def test_npdrm_refusals_never_show_a_key(self):
        kf, kk = rnd(16), rnd(16)
        s = make_self(self.elf, self.erk, self.riv, keyrev=0x0A, npdrm=(kf, kk))
        cases = [
            ({"npdrm": {0x0A: (self.erk, self.riv)}}, "KLIC_FREE"),                        # no licence keys
            ({"KLIC_FREE": kf, "KLIC_KEY": kk}, "NPDRM 0x000a"),                          # no NPDRM key
            ({"npdrm": {0x0A: (self.erk, self.riv)}, "KLIC_FREE": rnd(16), "KLIC_KEY": kk}, "did not decrypt"),
            ({"npdrm": {0x0A: (rnd(32), self.riv)}, "KLIC_FREE": kf, "KLIC_KEY": kk}, "did not decrypt"),
        ]
        for ring, words in cases:
            with self.assertRaises(ValueError) as c:
                self_decrypt.decrypt_self(s, {0x0A: (self.erk, self.riv)}, player_keys.parse(keys_text(ring)))
            self.assertIn(words, str(c.exception))
            self.assertNoKey(str(c.exception), self.erk, self.riv, kf, kk)
        paid = make_self(self.elf, self.erk, self.riv, keyrev=0x0A, npdrm=(kf, kk), licence=2)
        with self.assertRaises(ValueError) as c:
            self_decrypt.decrypt_self(paid, {}, player_keys.parse(keys_text(
                {"npdrm": {0x0A: (self.erk, self.riv)}, "KLIC_FREE": kf, "KLIC_KEY": kk})))
        self.assertIn("paid licence", str(c.exception))

    def test_command_line(self):
        kf, kk = rnd(16), rnd(16)
        s = self.write("EBOOT.BIN", make_self(self.elf, self.erk, self.riv, keyrev=3, npdrm=(kf, kk)))
        kp = self.write("keys.txt", keys_text({"npdrm": {3: (self.erk, self.riv)}, "KLIC_FREE": kf, "KLIC_KEY": kk}))
        out = os.path.join(self.dir, "EBOOT.elf")
        r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "self_decrypt.py"), "--keys", kp, s, out],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(out, "rb") as f:
            self.assertEqual(f.read(), self.elf)
        self.assertNoKey(r.stdout + r.stderr, self.erk, kf, kk)


if __name__ == "__main__":
    unittest.main()
