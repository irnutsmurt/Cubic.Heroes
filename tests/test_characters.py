#!/usr/bin/env python3
"""The character system (M12): the edit-data format, PS3 save recognition and decryption with a player's key, the
previews, the local library, its portable files and installing into the game's saves.
    build/venv/Scripts/python.exe -m unittest tests.test_characters
Everything here is made up: a character built by the test (no player's or game's data), a made-up "console key" and
"secure file ID" (never a real console's), a minimal program file. The real files (a PS3 original and its decrypted
copy) are checked locally by tests/scripts/edit_load_*.drive, never committed."""
import io
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
import zipfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import char_library as L  # noqa: E402
import char_preview  # noqa: E402
import edt_format as E  # noqa: E402
import ps3_save as P  # noqa: E402
import secret_store  # noqa: E402

TEST_KEY = bytes(range(0xA0, 0xB0))         # made up
TEST_SID = bytes([0x5A, 0xA5] * 8)          # made up


def make_character(name="ＴＥＳＴ", klass="ＨＥＲＯ", colour=(0.25, 0.5, 0.75)):
    """A small made-up character: a 3 x 6 x 2 block figure in every pose."""
    c = E.Character()
    c.name, c.klass = name, klass
    c.settings = tuple(float(i) for i in range(14))
    for p in range(6):
        vox = []
        for x in range(6, 9):
            for y in range(0, 6 + (p % 2)):
                for z in range(7, 9):
                    rgb = (1.0, 0.8, 0.7) if y >= 5 else colour
                    vox.append(E.Voxel((0, x, y, z, rgb, float(y % 3))))
        c.poses.append(E.Pose(vox, (0.0, 0.0, 0.0, 8.0, 6.0, 8.0)))
    return c


def fake_program(sid):
    """A minimal big-endian ELF64 with one loadable segment holding the secure file ID among other bytes."""
    body = os.urandom(64) + sid * 3 + os.urandom(64)
    phoff = 64
    hdr = bytearray(64)
    hdr[:4] = b"\x7fELF"
    hdr[0x20:0x28] = struct.pack(">Q", phoff)
    hdr[0x38:0x3A] = struct.pack(">H", 1)
    seg_off = 64 + 56
    ph = struct.pack(">IIQQQQQQ", 1, 4, seg_off, 0x10000, 0x10000, len(body), len(body), 0x10000)
    return bytes(hdr) + ph + body


class Format(unittest.TestCase):
    def test_round_trip_exact(self):
        c = make_character()
        b = E.serialize(c)
        self.assertTrue(b.startswith(b"EDTFILE1.9"))
        self.assertEqual(E.serialize(E.parse(b)), b)
        self.assertEqual(E.display_name(E.parse(b).name), "TEST")

    def test_damage_named(self):
        b = E.serialize(make_character())
        for bad, words in ((b[:-3], "ends early"), (b + b"\0", "unexpected byte"), (b"XX" + b[2:], "not a decrypted"),
                           (b[:12] + b"\xff" * 4 + b[16:], "name")):
            with self.assertRaises(E.EdtError) as cm:
                E.parse(bad)
            self.assertIn(words, str(cm.exception))

    def test_grid_and_duplicates_refused(self):
        c = make_character()
        v = c.poses[0].voxels[0]
        c.poses[0].voxels.append(E.Voxel((0, v.x, v.y, v.z, v.rgb, 0.0)))
        with self.assertRaises(E.EdtError):
            E.parse(E.serialize(c))
        c = make_character()
        c.poses[1].voxels.append(E.Voxel((0, 16, 0, 0, (1.0, 1.0, 1.0), 0.0)))
        with self.assertRaises(E.EdtError):
            E.parse(E.serialize(c))

    def test_shape_hash_ignores_name(self):
        a, b = make_character("ＡＡ"), make_character("ＢＢ")
        self.assertEqual(E.shape_hash(a), E.shape_hash(b))
        self.assertNotEqual(E.shape_hash(a), E.shape_hash(make_character(colour=(0.1, 0.1, 0.1))))


class Ps3Save(unittest.TestCase):
    def setUp(self):
        self.plain = E.serialize(make_character())
        self.enc, self.pfd = P._encrypt_for_tests(TEST_KEY, TEST_SID, self.plain)
        self.entry = P.parse_pfd(self.pfd)[1]["DATA.EDT"]

    def test_classify_by_the_data_not_the_pfd(self):
        self.assertEqual(P.classify(self.enc, self.pfd)[0], "encrypted")
        self.assertEqual(P.classify(self.plain, self.pfd)[0], "plain")      # decrypted beside its PARAM.PFD
        self.assertEqual(P.classify(self.plain, b"")[0], "plain")
        self.assertEqual(P.classify(self.enc, b"")[0], "damaged")           # no PFD: nothing to decrypt with
        self.assertEqual(P.classify(self.enc[:-16], self.pfd)[0], "damaged")  # sizes disagree

    def test_secure_id_from_the_program(self):
        self.assertEqual(P.find_secure_id(fake_program(TEST_SID), self.enc, self.entry), TEST_SID)
        self.assertIsNone(P.find_secure_id(fake_program(bytes([7] * 16)), self.enc, self.entry))

    def test_key_validated_before_use(self):
        self.assertTrue(P.check_console_key(TEST_KEY, TEST_SID, self.enc, self.entry))
        self.assertFalse(P.check_console_key(bytes(16), TEST_SID, self.enc, self.entry))
        self.assertEqual(P.decrypt_character(TEST_KEY, TEST_SID, self.enc, self.entry), self.plain)
        with self.assertRaises(P.SaveError) as cm:
            P.decrypt_character(bytes(16), TEST_SID, self.enc, self.entry)
        self.assertNotIn(TEST_KEY.hex(), str(cm.exception))

    def test_key_text(self):
        self.assertEqual(P.parse_key_text(" ".join(TEST_KEY.hex()[i:i + 4] for i in range(0, 32, 4))), TEST_KEY)
        for bad in ("12", "zz" * 16, TEST_KEY.hex() + "00"):
            with self.assertRaises(P.SaveError) as cm:
                P.parse_key_text(bad)
            self.assertNotIn(bad, str(cm.exception))


class Preview(unittest.TestCase):
    def test_deterministic_png(self):
        b = E.serialize(make_character())
        t1, v1, ok = char_preview.make(b)
        t2, v2, _ = char_preview.make(b)
        self.assertTrue(ok)
        self.assertEqual((t1, v1), (t2, v2))
        for png in (t1, v1):
            self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))
            w, h = struct.unpack(">II", png[16:24])
            idat = png[png.index(b"IDAT") + 4:png.index(b"IEND") - 8]
            self.assertEqual(len(zlib.decompress(idat)), h * (1 + 3 * w))

    def test_placeholder_when_not_a_character(self):
        t, v, ok = char_preview.make(b"not a character")
        self.assertFalse(ok)
        self.assertTrue(t.startswith(b"\x89PNG"))

    def test_the_model_is_drawn(self):
        c = char_preview.thumbnail(make_character(colour=(1.0, 0.0, 0.0)))
        reds = sum(1 for row in c.rows for i in range(0, len(row), 3) if row[i] > 150 and row[i + 1] < 60)
        self.assertGreater(reds, 1000)


class Library(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="chartest-")
        self.lib = L.Library(os.path.join(self.tmp, "lib"))
        self.plain = E.serialize(make_character())
        self.enc, self.pfd = P._encrypt_for_tests(TEST_KEY, TEST_SID, self.plain)
        self.ps3 = os.path.join(self.tmp, "PS3", "SAVEDATA", "BLJM60180T0000000000000000000A")
        os.makedirs(self.ps3)
        for n, b in (("DATA.EDT", self.enc), ("PARAM.PFD", self.pfd), ("PARAM.SFO", b"\0PSF account-bound"),
                     ("ICON0.PNG", b"\x89PNG not ours")):
            open(os.path.join(self.ps3, n), "wb").write(b)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_encrypted_needs_the_key_and_leaves_the_originals(self):
        before = {n: open(os.path.join(self.ps3, n), "rb").read() for n in os.listdir(self.ps3)}
        c, err = L.find([os.path.join(self.tmp, "PS3")])
        self.assertEqual((len(c), err), (1, []))
        self.assertEqual(self.lib.import_candidates(c)[0]["status"], "needs-key")
        self.assertEqual(self.lib.import_candidates(c, bytes(16), fake_program(TEST_SID))[0]["status"], "wrong-key")
        self.assertEqual(self.lib.ids(), [])
        r = self.lib.import_candidates(c, TEST_KEY, fake_program(TEST_SID))[0]
        self.assertEqual(r["status"], "imported")
        self.assertEqual(self.lib.edt(r["id"]), self.plain)          # usable without the key from now on
        after = {n: open(os.path.join(self.ps3, n), "rb").read() for n in os.listdir(self.ps3)}
        self.assertEqual(before, after)
        self.assertEqual(self.lib.import_candidates(L.find([os.path.join(self.tmp, "PS3")])[0], TEST_KEY,
                                                    fake_program(TEST_SID))[0]["status"], "duplicate")

    def test_plain_needs_no_key_and_similar_is_flagged(self):
        p = os.path.join(self.tmp, "a.edt")
        open(p, "wb").write(self.plain)
        self.assertEqual(self.lib.import_candidates(L.find([p])[0])[0]["status"], "imported")
        q = os.path.join(self.tmp, "b.edt")
        open(q, "wb").write(E.serialize(make_character(name="ＯＴＨＥＲ")))
        self.assertEqual(self.lib.import_candidates(L.find([q])[0])[0]["status"], "similar")

    def test_export_holds_only_the_character(self):
        p = os.path.join(self.tmp, "a.edt")
        open(p, "wb").write(self.plain)
        cid = self.lib.import_candidates(L.find([p])[0])[0]["id"]
        b = self.lib.export(cid)
        self.assertEqual(b, self.lib.export(cid))                      # the same bytes every time
        z = zipfile.ZipFile(io.BytesIO(b))
        self.assertEqual(sorted(z.namelist()), ["character.edt", "character.json", "preview/thumb.png",
                                                "preview/views.png"])
        meta = json.loads(z.read("character.json"))
        self.assertNotIn("account", json.dumps(meta).lower())
        self.assertEqual(meta["sha256"], L._sha(self.plain))
        # read back in another library; a tampered one refused
        out = os.path.join(self.tmp, "x.chchar")
        open(out, "wb").write(b)
        other = L.Library(os.path.join(self.tmp, "lib2"))
        self.assertEqual(other.import_candidates(L.find([out])[0])[0]["status"], "imported")
        bad = io.BytesIO()
        with zipfile.ZipFile(bad, "w") as zz:
            zz.writestr("character.json", z.read("character.json"))
            zz.writestr("character.edt", E.serialize(make_character(colour=(0.0, 0.0, 0.0))))
        open(out, "wb").write(bad.getvalue())
        c, err = L.find([out])
        self.assertEqual(c, [])
        self.assertIn("SHA-256", err[0])

    def test_unsafe_archives_refused(self):
        for name in ("../evil/DATA.EDT", "/abs/DATA.EDT", "C:/x/DATA.EDT", "a/../../DATA.EDT"):
            buf = io.BytesIO()
            with zipfile.ZipFile(buf, "w") as z:
                z.writestr(name, self.plain)
            p = os.path.join(self.tmp, "t.zip")
            open(p, "wb").write(buf.getvalue())
            c, err = L.find([p])
            self.assertEqual(c, [], name)
            self.assertIn("unsafe path", err[0])
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("bomb/DATA.EDT", b"\0" * (L.ZIP_MAX_MEMBER - 1))
        p = os.path.join(self.tmp, "bomb.zip")
        open(p, "wb").write(buf.getvalue())
        c, err = L.find([p])
        self.assertEqual(c, [])
        self.assertIn("expands too far", err[0])

    def test_install_never_replaces(self):
        p = os.path.join(self.tmp, "a.edt")
        open(p, "wb").write(self.plain)
        cid = self.lib.import_candidates(L.find([p])[0])[0]["id"]
        sav = os.path.join(self.tmp, "savedata")
        game_save = os.path.join(sav, "BLUS30490GAME-DATA00000000")
        os.makedirs(game_save)
        open(os.path.join(game_save, "DATA"), "wb").write(b"a game save")
        folder, new = self.lib.install(cid, sav)
        self.assertTrue(new and folder.startswith("BLJM60180T"))
        self.assertEqual(self.lib.install(cid, sav), (folder, False))
        self.assertEqual(open(os.path.join(game_save, "DATA"), "rb").read(), b"a game save")
        sfo = open(os.path.join(sav, folder, "PARAM.SFO"), "rb").read()
        self.assertIn(b"0000000000000000", sfo)                    # no account
        self.assertIn(b"NAME:\xef\xbc\xb4", sfo)                    # the character's own name
        found, unusable = L.Library.installed(sav)
        self.assertEqual(list(found.values()), [folder])
        enc_dir = os.path.join(sav, "BLJM60180T0000000000000000000B")
        os.makedirs(enc_dir)
        open(os.path.join(enc_dir, "DATA.EDT"), "wb").write(self.enc)
        open(os.path.join(enc_dir, "PARAM.PFD"), "wb").write(self.pfd)
        self.assertIn("still encrypted", L.Library.installed(sav)[1][0][1])


class WorkerCommand(unittest.TestCase):
    """tools/worker.py characters: the key over the control channel only, never in its output or log."""

    def test_import_with_a_key_over_control(self):
        import subprocess
        tmp = tempfile.mkdtemp(prefix="charworker-")
        try:
            inst = os.path.join(tmp, "inst")
            usr = os.path.join(inst, "data", "disc", "PS3_GAME", "USRDIR")
            os.makedirs(usr)
            os.makedirs(os.path.join(inst, "data", "gamedata"))
            open(os.path.join(usr, "EBOOT.elf"), "wb").write(fake_program(TEST_SID))
            plain = E.serialize(make_character())
            enc, pfd = P._encrypt_for_tests(TEST_KEY, TEST_SID, plain)
            save = os.path.join(tmp, "usb", "BLJM60180T0000000000000000000C")
            os.makedirs(save)
            open(os.path.join(save, "DATA.EDT"), "wb").write(enc)
            open(os.path.join(save, "PARAM.PFD"), "wb").write(pfd)
            log = os.path.join(tmp, "worker.jsonl")
            cmd = [sys.executable, os.path.join(ROOT, "tools", "worker.py"), "characters", "--install", inst,
                   "--log", log, "--char-action", "import", "--paths", os.path.join(tmp, "usb"), "--with-key",
                   "--control"]
            out = []
            for typed in ("00" * 16, TEST_KEY.hex()):
                p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, encoding="utf-8")
                res = None
                for line in p.stdout:
                    out.append(line)
                    e = json.loads(line)
                    if e["ev"] == "needs":
                        p.stdin.write("key " + typed + "\n")
                        p.stdin.flush()
                    if e["ev"] == "result":
                        res = e
                        break
                p.stdin.close()
                out.append(p.stderr.read())
                p.wait(60)
                self.assertTrue(res["ok"], res)
                want = "wrong-key" if typed.startswith("00") else "imported"
                self.assertEqual([r["status"] for r in res["data"]["results"]], [want])
            self.assertEqual(res["data"]["key_kept"], "this session")     # no --keep-key: not in the OS store
            text = "".join(out) + open(log, encoding="utf-8").read()
            self.assertNotIn(TEST_KEY.hex(), text.lower())
            self.assertEqual(open(os.path.join(save, "DATA.EDT"), "rb").read(), enc)    # the original untouched
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


class KeyStore(unittest.TestCase):
    def test_session_only(self):
        secret_store._session.clear()
        self.assertEqual(secret_store.save(TEST_KEY, keep=False), "this session")
        self.assertEqual(secret_store.load(), TEST_KEY)
        secret_store._session.clear()


if __name__ == "__main__":
    unittest.main()
