"""The official v1.01 update's checks before Install (tools/update_preflight.py): made-up packages, programs and keys
only (tests/test_pkg_update.py's), never Sony's files. The record of the official package, its file list and the
programs' pins are replaced by the made-up ones for each test.

    py -3 -m unittest tests.test_update_preflight
"""
import hashlib
import json
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import game_update  # noqa: E402
import update_preflight as U  # noqa: E402
from test_pkg_update import keys_text, make_elf, make_pkg, make_self, rnd  # noqa: E402

TMP = os.path.join(ROOT, "build", "tmp")
REMEDIES = {"retry", "download", "choose_pkg", "choose_folder", "choose_elf", "choose_keys", "locate_rpcs3",
            "free_space", "continue_v100"}


class Preflight(unittest.TestCase):
    def setUp(self):
        os.makedirs(TMP, exist_ok=True)
        self.d = tempfile.mkdtemp(prefix="preflight-", dir=TMP)
        self.addCleanup(shutil.rmtree, self.d, True)
        # the made-up game: v1.00's program pin, the update's package and program
        self.v100_elf = make_elf([rnd(4000)])
        self.v101_elf = make_elf([rnd(6000), b"\0" * 500 + rnd(300)])
        self.erk, self.riv, self.kf, self.kk, self.pk = rnd(32), rnd(16), rnd(16), rnd(16), rnd(16)
        eboot = make_self(self.v101_elf, self.erk, self.riv, keyrev=1, npdrm=(self.kf, self.kk))
        self.files = [("PARAM.SFO", rnd(300)), ("USRDIR/EBOOT.BIN", eboot), ("USRDIR/data/edt/addition/05_001_X.EDT",
                                                                              rnd(800))]
        self.pkg_bytes = make_pkg(self.pk, self.files, dirs=["USRDIR", "USRDIR/data", "USRDIR/data/edt",
                                                             "USRDIR/data/edt/addition"])
        self.rec = {"app_ver": "01.01", "content_id": "UP0005-BLUS30490_00-M3DDGHPATCH00001",
                    "file": "UPDATE-TEST.pkg", "url": "http://example.invalid/UPDATE-TEST.pkg",
                    "size": len(self.pkg_bytes), "sha256": hashlib.sha256(self.pkg_bytes).hexdigest(),
                    "sha1": hashlib.sha1(self.pkg_bytes).hexdigest()}
        self.manifest = {"format": 1, "title_id": "BLUS30490", "app_ver": "01.01", "count": len(self.files),
                         "total_size": sum(len(b) for _, b in self.files),
                         "files": [{"path": n, "size": len(b), "sha256": hashlib.sha256(b).hexdigest()}
                                   for n, b in self.files]}
        pins = ({"title_id": "BLUS30490", "region": "US"}, hashlib.sha256(self.v100_elf).hexdigest(),
                hashlib.sha256(self.v101_elf).hexdigest())
        self.programs = os.path.join(self.d, "programs")
        os.makedirs(os.path.join(self.programs, U.UPDATE_ID))
        for p in (mock.patch.object(game_update, "record", lambda *a, **k: self.rec),
                  mock.patch.object(game_update, "load_manifest", lambda *a, **k: self.manifest),
                  mock.patch.object(U, "_pins", lambda: pins), mock.patch.object(U, "PROGRAMS", self.programs),
                  mock.patch.object(U, "downloads_folder", lambda: None)):
            p.start()
            self.addCleanup(p.stop)
        self.install = self.make_install(pins[1])
        self.disc_dir = os.path.join(self.d, "games")
        os.makedirs(self.disc_dir)
        self.disc = os.path.join(self.disc_dir, "game.iso")
        open(self.disc, "wb").close()

    def make_install(self, elf_sha, **extra):
        inst = os.path.join(self.d, "install")
        os.makedirs(os.path.join(inst, "data"), exist_ok=True)
        os.makedirs(os.path.join(inst, "config"), exist_ok=True)
        with open(os.path.join(inst, "data", "install.json"), "w") as f:
            json.dump(dict({"elf_sha256": elf_sha}, **extra), f)
        return inst

    def keys(self, **ring):
        p = os.path.join(self.d, "mykeys.txt")
        with open(p, "w", newline="\n") as f:
            f.write(keys_text(ring))
        return p

    def all_keys(self):
        return self.keys(npdrm={1: (self.erk, self.riv)}, KLIC_FREE=self.kf, KLIC_KEY=self.kk, PKG=self.pk)

    def put_pkg(self, where, data=None):
        p = os.path.join(where, self.rec["file"])
        with open(p, "wb") as f:
            f.write(self.pkg_bytes if data is None else data)
        return p

    def check(self, r, cid):
        return next(c for c in r["checks"] if c["id"] == cid)

    def plain(self, r):
        """every check speaks plainly: no exception names or tracebacks in what the player reads; remedies known"""
        for c in r["checks"]:
            for bad in ("Traceback", "Error", "Exception", "errno"):
                self.assertNotIn(bad, c["message"], c)
            self.assertTrue(set(c["remedies"]) <= REMEDIES, c["remedies"])
            self.assertTrue(c["message"].strip())

    # ---- the installation ----

    def test_not_installed(self):
        os.remove(os.path.join(self.install, "data", "install.json"))
        r = U.preflight(self.install, online=False)
        self.assertEqual((r["state"], r["ready"], self.check(r, "game")["status"]), ("not_installed", False, "missing"))
        self.plain(r)

    def test_another_game_or_version(self):
        self.make_install("0" * 64)
        r = U.preflight(self.install, online=False)
        self.assertEqual((r["state"], r["ready"]), ("unsupported", False))
        self.assertIn("BLUS-30490", self.check(r, "game")["message"])
        self.plain(r)

    def test_v101_already_installed_offers_nothing(self):
        self.make_install(hashlib.sha256(self.v100_elf).hexdigest(),
                          official_content=["BLUS30490-01.00", "BLUS30490-01.01"])
        r = U.preflight(self.install, online=False)
        self.assertEqual((r["state"], r["ready"], r["already"]), ("v101", False, True))
        self.assertIn("already installed", self.check(r, "already")["message"])

    def test_this_version_without_update_support(self):
        shutil.rmtree(os.path.join(self.programs, U.UPDATE_ID))
        r = U.preflight(self.install, pkg=self.put_pkg(self.d), keys=self.all_keys(), online=False)
        c = self.check(r, "support")
        self.assertEqual((r["ready"], c["status"], c["remedies"]), (False, "failed", ["continue_v100"]))
        self.assertIn("v1.00 game is complete", c["message"])   # v1.00 is never called incomplete

    # ---- the package ----

    def test_package_beside_the_disc_with_keys_is_ready(self):
        self.put_pkg(self.disc_dir)
        r = U.preflight(self.install, disc=self.disc, keys=self.all_keys(), online=False)
        self.assertTrue(r["ready"], r["blockers"])
        self.assertEqual((r["plan"]["source"], r["plan"]["decrypt"], r["plan"]["how"]), ("pkg", "keys", "beside your disc"))
        self.assertEqual(r["blockers"], [])
        self.plain(r)

    def test_missing_package_offline(self):
        r = U.preflight(self.install, disc=self.disc, keys=self.all_keys(), online=False)
        c = self.check(r, "package")
        self.assertEqual((r["ready"], c["status"]), (False, "missing"))
        self.assertEqual(c["message"], "Your game is ready, but the v1.01 update package could not be found. You can "
                                       "download the official update or select a PKG file you already have.")
        self.assertEqual(c["remedies"], ["download", "choose_pkg", "choose_folder", "continue_v100"])
        self.assertIn(c["message"], r["blockers"])   # what a disabled Install says

    def test_missing_package_online_offers_the_download(self):
        r = U.preflight(self.install, disc=self.disc, keys=self.all_keys(), head=lambda url: self.rec["size"])
        self.assertTrue(r["ready"], r["blockers"])
        self.assertEqual((r["plan"]["source"], r["plan"]["decrypt"]), ("download", "after_download"))
        self.assertIn("downloaded from Sony", self.check(r, "package")["message"])

    def test_corrupted_package(self):
        bad = bytearray(self.pkg_bytes)
        bad[len(bad) // 2] ^= 0xFF
        self.put_pkg(self.disc_dir, bytes(bad))
        r = U.preflight(self.install, disc=self.disc, keys=self.all_keys(), online=False)
        c = self.check(r, "package")
        self.assertEqual((r["ready"], c["status"]), (False, "failed"))
        self.assertEqual(c["message"], "The update package was found, but its integrity check failed. Please select a "
                                       "valid official v1.01 package.")
        self.assertEqual(c["remedies"], ["choose_pkg", "download", "continue_v100"])
        # online, the official download is offered instead, saying why the local one isn't used
        r = U.preflight(self.install, disc=self.disc, keys=self.all_keys(), head=lambda url: self.rec["size"])
        self.assertTrue(r["ready"])
        self.assertIn("didn't pass its integrity check", self.check(r, "package")["message"])

    def test_wrong_file_chosen_as_the_package(self):
        other = os.path.join(self.d, "other.pkg")
        open(other, "wb").write(rnd(1000))
        r = U.preflight(self.install, pkg=other, keys=self.all_keys(), online=False)
        self.assertEqual(self.check(r, "package")["status"], "failed")

    def test_an_already_updated_folder(self):
        folder = os.path.join(self.d, "rpcs3", "dev_hdd0", "game", "BLUS30490")
        for n, b in self.files:
            os.makedirs(os.path.dirname(os.path.join(folder, n)), exist_ok=True)
            open(os.path.join(folder, n), "wb").write(b)
        rp = os.path.join(self.d, "rpcs3", "rpcs3.exe")
        open(rp, "wb").write(b"MZ")
        keys = self.keys(npdrm={1: (self.erk, self.riv)}, KLIC_FREE=self.kf, KLIC_KEY=self.kk)   # no package key needed
        r = U.preflight(self.install, keys=keys, rpcs3=rp, online=False)
        self.assertTrue(r["ready"], r["blockers"])
        self.assertEqual((r["plan"]["source"], r["plan"]["how"], r["plan"]["decrypt"]), ("folder", "in your RPCS3", "keys"))
        open(os.path.join(folder, "USRDIR", "data", "edt", "addition", "05_001_X.EDT"), "wb").write(rnd(800))
        r = U.preflight(self.install, keys=keys, rpcs3=rp, online=False)   # a changed file: not used, said so
        self.assertEqual(self.check(r, "files")["status"], "warning")
        self.assertEqual(self.check(r, "package")["status"], "missing")

    # ---- opening it ----

    def test_no_package_key(self):
        r = U.preflight(self.install, pkg=self.put_pkg(self.d), online=False,
                        keys=self.keys(npdrm={1: (self.erk, self.riv)}, KLIC_FREE=self.kf, KLIC_KEY=self.kk))
        c = self.check(r, "keys")
        self.assertEqual((r["ready"], c["status"]), (False, "missing"))
        self.assertIn("package key", c["message"])
        self.assertIn("choose_keys", c["remedies"])

    def test_keys_that_dont_open_the_program(self):
        r = U.preflight(self.install, pkg=self.put_pkg(self.d), online=False,
                        keys=self.keys(npdrm={1: (rnd(32), self.riv)}, KLIC_FREE=self.kf, KLIC_KEY=self.kk, PKG=self.pk))
        c = self.check(r, "keys")
        self.assertEqual((r["ready"], c["status"]), (False, "failed"))
        self.assertEqual(c["message"], "The update executable could not be decrypted using the available resources. "
                                       "Select an existing decrypted update executable, provide compatible keys, or "
                                       "configure an available supported RPCS3 installation.")
        self.assertEqual(c["remedies"], ["choose_elf", "choose_keys", "locate_rpcs3", "continue_v100"])
        for k in (self.erk, self.riv, self.kf, self.kk, self.pk):   # no key in anything shown
            self.assertNotIn(k.hex(), json.dumps(r).lower())

    def test_a_decrypted_program_the_player_has(self):
        elf = os.path.join(self.d, "EBOOT-v101.elf")
        open(elf, "wb").write(self.v101_elf)
        r = U.preflight(self.install, pkg=self.put_pkg(self.d), elf=elf, keys=self.keys(PKG=self.pk), online=False)
        self.assertTrue(r["ready"], r["blockers"])
        self.assertEqual(r["plan"]["decrypt"], "elf")
        open(elf, "wb").write(self.v100_elf)   # not the update's program: not used, said so
        r = U.preflight(self.install, pkg=self.put_pkg(self.d), elf=elf, keys=self.keys(PKG=self.pk), online=False)
        self.assertEqual(self.check(r, "program")["status"], "warning")
        self.assertFalse(r["ready"])

    def test_download_without_any_way_to_open_it(self):
        r = U.preflight(self.install, keys=self.keys(PKG=self.pk), head=lambda url: self.rec["size"])
        c = self.check(r, "keys")
        self.assertEqual((r["ready"], c["status"]), (False, "missing"))
        self.assertIn("locate_rpcs3", c["remedies"])

    # ---- space, permission ----

    def test_not_enough_space(self):
        Usage = __import__("collections").namedtuple("Usage", "total used free")
        with mock.patch.object(U.shutil, "disk_usage", lambda p: Usage(10 << 30, 9 << 30, 1 << 30)):
            r = U.preflight(self.install, pkg=self.put_pkg(self.d), keys=self.all_keys(), online=False)
        c = self.check(r, "space")
        self.assertEqual((r["ready"], c["status"]), (False, "failed"))
        self.assertRegex(c["message"], r"^Not enough free disk space\. The update requires \d+\.\d GB, but only 1\.0 GB "
                                       r"is available\.$")
        self.assertEqual(c["values"]["free"], 1 << 30)

    def test_reads_only(self):
        before = {os.path.join(dp, n) for dp, dn, fn in os.walk(self.install) for n in fn}
        U.preflight(self.install, pkg=self.put_pkg(self.d), keys=self.all_keys(), online=False)
        after = {os.path.join(dp, n) for dp, dn, fn in os.walk(self.install) for n in fn}
        self.assertEqual(before, after)


if __name__ == "__main__":
    unittest.main()
