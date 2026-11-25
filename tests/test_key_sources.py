"""The game program's key sources (tools/player_keys.py formats, tools/key_sources.py discovery and matching,
tools/self_decrypt.py's tries), with made-up keys and programs only: text keys files of both kinds, binary key folders,
ZIP archives, the places beside a disc (and only those), the order sources are tried in, an RPCS3 stand-in, and that no
key ever appears in a result or an error."""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import key_sources  # noqa: E402
import player_keys  # noqa: E402
import self_decrypt  # noqa: E402
from self_fixtures import make_elf, make_self, rnd  # noqa: E402

TMP = os.path.join(ROOT, "build", "tmp")


class Base(unittest.TestCase):
    def setUp(self):
        os.makedirs(TMP, exist_ok=True)
        self.dir = tempfile.mkdtemp(prefix="keysrc-", dir=TMP)
        self.elf = make_elf([rnd(6000), rnd(900)])
        self.want = hashlib.sha256(self.elf).hexdigest()
        self.erk, self.riv = rnd(32), rnd(16)
        self.app = make_self(self.elf, self.erk, self.riv, keyrev=1)

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def put(self, rel, data):
        p = os.path.join(self.dir, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data.encode() if isinstance(data, str) else data)
        return p

    def ps3tools_files(self, prefix=""):
        """a binary key folder as PS3 tools keep one: decoys first, the right set under a firmware's name"""
        return {f"{prefix}app-key-341": rnd(32), f"{prefix}app-iv-341": rnd(16),
                f"{prefix}app-key-315": self.erk, f"{prefix}app-iv-315": self.riv,
                f"{prefix}iso-key-315": rnd(32), f"{prefix}iso-iv-315": rnd(16), f"{prefix}gpkg-key": rnd(16),
                f"{prefix}README": b"These files contain the keys", f"{prefix}curves": rnd(7744)}

    def keys_file(self, rel, app):
        return self.put(rel, "".join(f"APP {r:x} {e.hex()} {v.hex()}\n" for r, (e, v) in app.items()))

    def assertNoKey(self, text):
        for k in (self.erk, self.riv):
            self.assertNotIn(k.hex(), text.lower())


class Formats(Base):
    def test_binary_folder_one_level_down(self):
        for name, data in self.ps3tools_files("ps3keys-master/").items():
            self.put("keys-folder/" + name, data)
        ring = player_keys.load(os.path.join(self.dir, "keys-folder"))
        self.assertIn((self.erk, self.riv), ring.app_any)
        self.assertEqual(len(ring.app_any), 2)                                  # the iso set and the rest ignored
        self.assertEqual(self_decrypt.decrypt_self(self.app, ring), self.elf)   # found among the unlabelled sets
        self.assertNoKey(repr(ring))

    def test_zip_in_memory(self):
        z = os.path.join(self.dir, "ps3keys-master.zip")
        with zipfile.ZipFile(z, "w") as f:
            for name, data in self.ps3tools_files("ps3keys-master/").items():
                f.writestr(name, data)
            f.writestr("ps3keys-master/huge.bin", b"\0" * (player_keys.MAX_FILE + 1))   # skipped, not read
        before = set(os.listdir(self.dir))
        ring = player_keys.load(z)
        self.assertEqual(set(os.listdir(self.dir)), before)                     # nothing written
        self.assertEqual(self_decrypt.decrypt_self(self.app, ring), self.elf)
        with self.assertRaises(player_keys.KeysError):
            player_keys.load(self.put("broken.zip", b"PK\x03\x04not a zip"))

    def test_section_text_files(self):
        text = ("[metldr]\ntype=SELF\nself_type=LDR\nerk=" + rnd(32).hex() + "\nriv=" + rnd(16).hex() + "\n\n"
                "[appldr-0x0001]\ntype=SELF\nself_type=APP\nrevision=0001\nerk=" + self.erk.hex() + "\nriv=" +
                self.riv.hex() + "\n\n[NP_klic_free]\ntype=OTHER\nerk=" + rnd(16).hex() + "\n")
        ring = player_keys.parse(text)
        self.assertEqual(ring.app, {1: (self.erk, self.riv)})                   # only the APP set taken
        self.assertEqual(self_decrypt.decrypt_self(self.app, ring), self.elf)
        mixed = player_keys.parse(f"APP 1 {self.erk.hex()} {self.riv.hex()}\nNPDRM 1 00 00\n[x]\nfoo=bar\n")
        self.assertEqual(mixed.app[1], (self.erk, self.riv))

    def test_any_label_and_wrong_pairs(self):
        self.assertEqual(self_decrypt.decrypt_self(self.app, {2: (self.erk, self.riv)}), self.elf)   # mislabelled
        with self.assertRaises(self_decrypt.MissingKeys) as c:      # a right erk with a wrong riv is refused too
            self_decrypt.decrypt_self(self.app, {1: (rnd(32), self.riv), 2: (self.erk, rnd(16))})
        self.assertEqual((c.exception.kind, c.exception.revision, c.exception.need), ("app", 1, ["APP 0x0001"]))
        self.assertNoKey(str(c.exception))
        npdrm = make_self(self.elf, self.erk, self.riv, self_type=8)            # a downloaded program: not handled
        with self.assertRaises(ValueError) as c:
            self_decrypt.decrypt_self(npdrm, {1: (self.erk, self.riv)})
        self.assertIn("NPDRM unsupported", str(c.exception))
        with self.assertRaises(ValueError):
            self_decrypt.decrypt_self(b"SCE\0" + rnd(20), {1: (self.erk, self.riv)})   # too short to be one

    def test_kinds(self):
        self.assertEqual(player_keys.kind_of(self.put("EBOOT.elf", self.elf)), "elf")
        self.assertEqual(player_keys.kind_of(self.put("rpcs3.exe", b"MZ\x90\0")), "rpcs3")
        self.assertEqual(player_keys.kind_of(self.put("rpcs3-v0.0.43.AppImage", b"\x7fELF\2\1")), "rpcs3")
        self.assertEqual(player_keys.kind_of(self.put("other.exe", b"MZ")), None)
        self.assertEqual(player_keys.kind_of(self.put("k.zip", b"PK\x03\x04")), "zip")
        self.assertEqual(player_keys.kind_of(self.dir), "folder")
        self.assertEqual(player_keys.kind_of(self.put("keys.txt", "APP 1 00 00\n")), "text")
        self.assertIsNone(player_keys.kind_of(os.path.join(self.dir, "missing")))


class Beside(Base):
    def test_only_the_explicitly_relevant_places(self):
        iso = self.put("games/3D Dot Game Heroes.iso", b"iso")
        want = [self.put("games/keys.txt", "x"), self.put("games/ps3keys-master.zip", b"PK"),
                self.put("games/my.keys", "x"), self.put("games/EBOOT.elf", b"\x7fELF")]
        self.put("games/ps3keys/app-key-315", self.erk)
        want.append(os.path.join(self.dir, "games", "ps3keys"))
        for other in ("games/game.dkey", "games/game.key", "games/game.ird", "games/notes.txt", "games/photo.jpg",
                      "games/photos/keys.txt", "games/sub/deeper/keys.txt", "elsewhere/keys.txt"):
            self.put(other, "x")
        self.assertEqual(sorted(key_sources.beside(iso)), sorted(want))
        # a disc folder: the folder holding it, and its own top level beside PS3_GAME
        self.put("dumps/BLUS30490/PS3_GAME/PARAM.SFO", b"x")
        inner = self.put("dumps/BLUS30490/keys.txt", "x")
        outer = self.put("dumps/keys.zip", b"PK")
        self.assertEqual(sorted(key_sources.beside(os.path.join(self.dir, "dumps", "BLUS30490"))), sorted([outer, inner]))


class Matching(Base):
    def test_order_and_the_pin(self):
        disc = self.put("games/game.iso", b"iso")
        good = self.keys_file("games/keys.txt", {1: (self.erk, self.riv)})
        wrong = self.keys_file("mine/old-keys.txt", {1: (rnd(32), rnd(16))})
        src = key_sources.Sources(chosen=wrong, remembered=[wrong], disc=disc)
        elf, how = key_sources.open_program(self.app, self.want, src)
        self.assertEqual((elf, how["source"], how["how"]), (self.elf, good, "beside the disc"))
        with self.assertRaises(key_sources.NoProgram) as c:     # the right keys, another pin: never accepted
            key_sources.open_program(self.app, hashlib.sha256(b"another program").hexdigest(), src)
        self.assertEqual(c.exception.code, "wrong_key")
        self.assertNoKey(str(c.exception) + repr(c.exception.tried))

    def test_nothing_anywhere(self):
        disc = self.put("empty/game.iso", b"iso")
        with self.assertRaises(key_sources.NoProgram) as c:
            key_sources.open_program(self.app, self.want, key_sources.Sources(disc=disc))
        self.assertEqual((c.exception.code, c.exception.need), ("keys_missing", ["APP 0x0001"]))

    def test_a_folder_without_the_key_names_what_is_needed(self):
        for name, data in self.ps3tools_files().items():
            if "315" not in name:
                self.put("arch/" + name, data)
        with self.assertRaises(key_sources.NoProgram) as c:
            key_sources.open_program(self.app, self.want, key_sources.Sources(chosen=os.path.join(self.dir, "arch")))
        self.assertEqual((c.exception.code, c.exception.need), ("wrong_key", ["APP 0x0001"]))
        self.assertNoKey(str(c.exception) + repr(c.exception.tried))

    def test_a_decrypted_program(self):
        e = self.put("x/EBOOT.elf", self.elf)
        self.assertEqual(key_sources.open_program(self.app, self.want, key_sources.Sources(chosen=e))[1]["kind"], "elf")
        other = self.put("y/EBOOT.elf", b"\x7fELF" + rnd(100))
        with self.assertRaises(key_sources.NoProgram):
            key_sources.open_program(self.app, self.want, key_sources.Sources(chosen=other))

    def test_rpcs3_is_optional_and_checked(self):
        rp = self.put("RPCS3/rpcs3.exe", b"MZ\x90\0")
        seen = []

        def fake_run(cmd, **kw):   # stands in for the player's RPCS3: writes EBOOT.elf beside its input
            seen.append((cmd[1], os.path.dirname(cmd[2]), kw.get("stdin"), kw.get("stdout")))
            with open(os.path.join(os.path.dirname(cmd[2]), "EBOOT.elf"), "wb") as f:
                f.write(self.result)
            return subprocess.CompletedProcess(cmd, 0)

        old = key_sources.subprocess.run
        key_sources.subprocess.run = fake_run
        try:
            self.result = self.elf
            elf, how = key_sources.open_program(self.app, self.want, key_sources.Sources(rpcs3=rp), work_dir=self.dir)
            self.assertEqual((elf, how["kind"]), (self.elf, "rpcs3"))
            self.assertEqual(seen[0][0], "--decrypt")
            self.assertEqual(seen[0][2:], (subprocess.DEVNULL, subprocess.DEVNULL))   # nothing of its output kept
            self.assertFalse(os.path.exists(seen[0][1]), "its temporary folder removed")
            # chosen as the key source itself, RPCS3 is tried last; a wrong result is refused
            self.result = rnd(5000)
            with self.assertRaises(key_sources.NoProgram) as c:
                key_sources.open_program(self.app, self.want, key_sources.Sources(chosen=rp), work_dir=self.dir)
            self.assertIn("RPCS3's result isn't this game's program", repr(c.exception.tried))
        finally:
            key_sources.subprocess.run = old
        missing = key_sources.Sources(rpcs3=os.path.join(self.dir, "no-rpcs3.exe"))
        with self.assertRaises(key_sources.NoProgram):
            key_sources.open_program(self.app, self.want, missing, work_dir=self.dir)


if __name__ == "__main__":
    unittest.main()
