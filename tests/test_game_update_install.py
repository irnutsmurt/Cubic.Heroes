"""The official v1.01 update installed, switched and rolled back (beta.6: tools/worker.py op_game_update and
op_game_version, tools/official_update.py, tools/install_layout.py). Made-up installs, packages, programs and keys
(tests/test_pkg_update.py's); the conversion and the build are stood in for (tests/test_install_layout.py's Fake);
never Sony's files, never the network.

The owner's requirement of 2026-11-25: verify every input before changing the installation; stage and check the
update before activating it; no partial installs; saves, characters, settings and caches kept; a failed update leaves
v1.00 playable; the update's files apart from the saves (a save backup never holds them); the Character Store
follows the ACTIVE version (switching back to v1.00 changes it at once).

    py -3 -m unittest tests.test_game_update_install
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
import install_layout as layout  # noqa: E402
import key_download  # noqa: E402
import key_sources  # noqa: E402
import official_update  # noqa: E402
import setup_pipeline as pipeline  # noqa: E402
import update_preflight as U  # noqa: E402
import worker  # noqa: E402
from test_install_layout import EXE, Fake, checked, get, put  # noqa: E402
from test_pkg_update import keys_text, make_elf, make_pkg, make_self, rnd  # noqa: E402
from test_update_preflight import make_program  # noqa: E402

TMP = os.path.join(ROOT, "build", "tmp")
UP = U.UPDATE_ID


class ProgramFake(Fake):
    """the stand-in pipeline, aware of the program it builds: a different game for v1.01"""

    def __call__(self, install, out_dir=None, unchanged_inputs=None, progress=None, program=None, **kw):
        if program:
            self.game, self.lift, self.inputs = b"game v1.01", "lift-v101", "inputs-v101"
        else:
            self.game, self.lift, self.inputs = b"game v1.00", "lift-v100", "inputs-v100"
        out = super().__call__(install, out_dir=out_dir, unchanged_inputs=unchanged_inputs, progress=progress,
                               program=program, **kw)
        out["program"] = program or pipeline.BASE_PROGRAM
        return out


class Base(unittest.TestCase):
    def setUp(self):
        os.makedirs(TMP, exist_ok=True)
        self.d = tempfile.mkdtemp(prefix="gui-", dir=TMP)
        self.install = os.path.join(self.d, "install")
        self.lay = layout.Layout(self.install)
        self.addCleanup(self.cleanup)
        # the made-up game and update
        self.v100_elf = make_elf([rnd(3000)])
        self.v101_elf = make_elf([rnd(5000), b"\0" * 300 + rnd(200)])
        self.erk, self.riv, self.kf, self.kk, self.pk = rnd(32), rnd(16), rnd(16), rnd(16), rnd(16)
        eboot = make_self(self.v101_elf, self.erk, self.riv, keyrev=1, npdrm=(self.kf, self.kk))
        self.files = [("PARAM.SFO", rnd(300)), ("USRDIR/EBOOT.BIN", eboot),
                      ("USRDIR/data/edt/addition/05_001_X.EDT", rnd(900)), ("USRDIR/data/movie/new.bik", rnd(2000))]
        self.pkg_bytes = make_pkg(self.pk, self.files, dirs=["USRDIR", "USRDIR/data", "USRDIR/data/edt",
                                                             "USRDIR/data/edt/addition", "USRDIR/data/movie"])
        self.rec = {"app_ver": "01.01", "content_id": "UP0005-BLUS30490_00-M3DDGHPATCH00001", "file": "UPD-TEST.pkg",
                    "url": "http://example.invalid/UPD-TEST.pkg", "size": len(self.pkg_bytes),
                    "sha256": hashlib.sha256(self.pkg_bytes).hexdigest(),
                    "decrypted_elf": {"sha256": hashlib.sha256(self.v101_elf).hexdigest()}}
        self.manifest = {"format": 1, "title_id": "BLUS30490", "app_ver": "01.01", "count": len(self.files),
                         "total_size": sum(len(b) for _, b in self.files),
                         "files": [{"path": n, "size": len(b), "sha256": hashlib.sha256(b).hexdigest()}
                                   for n, b in self.files]}
        self.v100_pin = hashlib.sha256(self.v100_elf).hexdigest()
        pins = ({"title_id": "BLUS30490", "region": "US"}, self.v100_pin, self.rec["decrypted_elf"]["sha256"])
        programs = os.path.join(self.d, "programs")
        make_program(programs, UP)
        self.fake = ProgramFake()
        for p in (mock.patch.object(game_update, "record", lambda *a, **k: self.rec),
                  mock.patch.object(game_update, "load_manifest", lambda *a, **k: self.manifest),
                  mock.patch.object(U, "_pins", lambda: pins), mock.patch.object(U, "PROGRAMS", programs),
                  mock.patch.object(U, "downloads_folder", lambda: None),
                  mock.patch.object(key_download, "load", lambda *a, **k: []),
                  mock.patch.object(pipeline, "run", self.fake)):
            p.start()
            self.addCleanup(p.stop)
        self.cb = worker.Callbacks()
        # an installed v1.00 game, with saves and settings
        worker.build_slot(self.lay, self.cb, validate=checked)
        self.v100_build = self.lay.read_active()["active"]
        with open(self.lay.install_json, "w") as f:
            json.dump({"elf_sha256": self.v100_pin, "lift": "lift-v100"}, f)
        put(os.path.join(self.lay.gamedata, "home", "00000001", "savedata", "BLUS30490-SAVE", "DATA.SAV"), b"my save")
        put(os.path.join(self.lay.config, "dotheroes.cfg"), b"DOTHEROES_FPS=120\n")

    def cleanup(self):
        if os.path.isdir(self.install) and layout.is_link(self.lay.game_link):
            layout.remove_link(self.lay.game_link)
        shutil.rmtree(self.d, ignore_errors=True)

    def keys(self, **ring):
        p = os.path.join(self.d, "mykeys.txt")
        with open(p, "w", newline="\n") as f:
            f.write(keys_text(ring or {"npdrm": {1: (self.erk, self.riv)}, "KLIC_FREE": self.kf, "KLIC_KEY": self.kk,
                                       "PKG": self.pk}))
        return p

    def put_pkg(self, data=None):
        p = os.path.join(self.d, self.rec["file"])
        with open(p, "wb") as f:
            f.write(self.pkg_bytes if data is None else data)
        return p

    def check_msg(self, r, cid="package"):
        return next(c for c in r["checks"] if c["id"] == cid)["message"]

    def content(self):
        with open(self.lay.install_json) as f:
            return json.load(f).get("official_content")

    def install_update(self, **kw):
        if "pkg" not in kw:   # (not setdefault: its default would be made -- and the files rewritten -- every time)
            kw["pkg"] = self.put_pkg()
        if "sources" not in kw:
            kw["sources"] = key_sources.Sources(chosen=self.keys())
        return worker.op_game_update(self.install, self.cb, online=False, validate=checked, **kw)


class Install(Base):
    def test_installed_from_a_package_with_the_players_keys(self):
        out = self.install_update()
        st = self.lay.read_active()
        self.assertEqual(self.lay.program_of(st["active"]), UP)
        self.assertEqual(get(os.path.join(self.lay.game_link, EXE)), b"game v1.01")
        self.assertEqual(self.fake.calls[-1]["program"], UP)
        # the update's files in their own folder, checked, with the pinned program beside EBOOT.BIN
        d = official_update.folder(self.install)
        self.assertEqual(d, os.path.join(self.install, "data", "official", UP))
        for n, b in self.files:
            self.assertEqual(get(os.path.join(d, *n.split("/"))), b)
        self.assertEqual(get(os.path.join(d, "USRDIR", "EBOOT.elf")), self.v101_elf)
        # the build's own facts for the runtime
        cfg = open(os.path.join(self.lay.build_dir(st["active"]), "build.cfg")).read()
        self.assertIn(f"DOTHEROES_PROGRAM={UP}\n", cfg)
        self.assertIn(f"DOTHEROES_UPDATE_DIR=data/official/{UP}\n", cfg)
        # the store follows the active version; v1.00 kept for the switch back
        self.assertEqual(self.content(), [pipeline.BASE_PROGRAM, UP])
        self.assertEqual(out["official_content"], [pipeline.BASE_PROGRAM, UP])
        self.assertIn(self.v100_build, self.lay.kept())
        self.assertNotIn("DOTHEROES_UPDATE_DIR", open(os.path.join(self.lay.build_dir(self.v100_build),
                                                                    "build.cfg")).read())
        # saves, settings kept; the backup made first holds the saves only, never the update
        self.assertEqual(get(os.path.join(self.lay.gamedata, "home", "00000001", "savedata", "BLUS30490-SAVE",
                                          "DATA.SAV")), b"my save")
        self.assertEqual(get(os.path.join(self.lay.config, "dotheroes.cfg")), b"DOTHEROES_FPS=120\n")
        bk = os.path.join(self.lay.backups_dir, out["saves_backup"])
        names = [os.path.relpath(os.path.join(p, f), bk) for p, _, fs in os.walk(bk) for f in fs]
        self.assertTrue(any(n.endswith("DATA.SAV") for n in names))
        self.assertFalse(any("official" in n or n.endswith((".EDT", ".bik", "EBOOT.BIN")) for n in names), names)
        self.assertFalse([n for n in os.listdir(official_update.root(self.install)) if n.startswith(".")])
        self.assertEqual(U.game_state(self.install)["state"], "v101")

    def test_from_an_installed_update_folder(self):
        src = os.path.join(self.d, "rpcs3", "dev_hdd0", "game", "BLUS30490")
        for n, b in self.files:
            put(os.path.join(src, *n.split("/")), b)
        self.install_update(pkg=None, folder=src)
        self.assertEqual(self.lay.program_of(self.lay.read_active()["active"]), UP)
        self.assertTrue(official_update.prepared(self.install))

    def test_already_v101(self):
        self.install_update()
        n = len(self.fake.calls)
        out = self.install_update()
        self.assertTrue(out["already"])
        self.assertEqual(len(self.fake.calls), n)


class Safety(Base):
    def assertV100(self):
        st = self.lay.read_active()
        self.assertEqual(st["active"], self.v100_build)
        self.assertEqual(get(os.path.join(self.lay.game_link, EXE)), b"game v1.00")
        self.assertIn(self.content(), (None, [pipeline.BASE_PROGRAM]))
        self.assertEqual(U.game_state(self.install)["state"], "v100")
        r = official_update.root(self.install)
        self.assertFalse(os.path.isdir(r) and [n for n in os.listdir(r) if n.startswith(".")])

    def test_a_wrong_package_changes_nothing(self):
        bad = bytearray(self.pkg_bytes)
        bad[len(bad) // 2] ^= 1
        with self.assertRaises(pipeline.SetupError) as c:
            self.install_update(pkg=self.put_pkg(bytes(bad)))
        self.assertEqual(c.exception.code, "update_not_ready")
        self.assertIn("integrity check failed", str(c.exception))
        self.assertTrue(json.loads(c.exception.details)["checks"])
        self.assertV100()
        self.assertFalse(os.path.exists(official_update.folder(self.install)))

    def test_missing_keys_change_nothing(self):
        with self.assertRaises(pipeline.SetupError) as c:
            self.install_update(sources=key_sources.Sources(chosen=self.keys(PKG=self.pk)))
        self.assertEqual(c.exception.code, "update_not_ready")
        self.assertV100()

    def test_a_failed_build_leaves_v100_playable_and_a_retry_reuses_the_checked_files(self):
        self.fake.fail = pipeline.SetupError("Recompiling the game failed.", "lift_failed")
        with self.assertRaises(pipeline.SetupError):
            self.install_update()
        self.assertV100()
        self.assertTrue(official_update.prepared(self.install))   # verified files kept for the next attempt
        self.fake.fail = None
        skipped = []
        cb = worker.Callbacks(skip=skipped.append)
        worker.op_game_update(self.install, cb, pkg=self.put_pkg(),
                              sources=key_sources.Sources(chosen=self.keys()), online=False, validate=checked)
        self.assertEqual(self.lay.program_of(self.lay.read_active()["active"]), UP)
        self.assertTrue(set(worker.UPDATE_STEPS) <= set(skipped))

    def test_checks_after_a_failed_build_need_no_package_again(self):
        """the update's files verified by the failed attempt: the checks say so, ready without the package or keys"""
        self.fake.fail = pipeline.SetupError("Recompiling the game failed.", "lift_failed")
        pkg = self.put_pkg()
        with self.assertRaises(pipeline.SetupError):
            self.install_update(pkg=pkg)
        os.remove(pkg)
        r = U.preflight(self.install, online=False)
        self.assertTrue(r["ready"], r["blockers"])
        self.assertEqual((r["plan"]["source"], r["plan"]["decrypt"]), ("prepared", "done"))
        self.fake.fail = None
        worker.op_game_update(self.install, self.cb, online=False, validate=checked)   # no package, no keys
        self.assertEqual(self.lay.program_of(self.lay.read_active()["active"]), UP)

    def test_interrupted_while_unpacking(self):
        hits = []

        def progress(step, frac, msg):
            if step == "update-extract":
                hits.append(frac)
                cb.cancel.set()
        cb = worker.Callbacks(progress=progress)
        with self.assertRaises(pipeline.Cancelled):
            worker.op_game_update(self.install, cb, pkg=self.put_pkg(),
                                  sources=key_sources.Sources(chosen=self.keys()), online=False, validate=checked)
        self.assertTrue(hits)
        self.assertV100()
        self.assertFalse(os.path.exists(official_update.folder(self.install)))   # no partial update in place
        self.install_update()                                                     # a retry works
        self.assertEqual(self.lay.program_of(self.lay.read_active()["active"]), UP)


class Versions(Base):
    def test_switch_back_and_forth_without_rebuilding(self):
        self.install_update()
        v101 = self.lay.read_active()["active"]
        n = len(self.fake.calls)
        out = worker.op_game_version(self.install, self.cb, "v100", validate=checked)
        self.assertEqual((out["build"], out["rebuilt"]), (self.v100_build, False))
        self.assertEqual(self.content(), [pipeline.BASE_PROGRAM])   # the store's 45 no longer "installed"
        self.assertEqual(get(os.path.join(self.lay.game_link, EXE)), b"game v1.00")
        out = worker.op_game_version(self.install, self.cb, "v101", validate=checked)
        self.assertEqual((out["build"], out["rebuilt"]), (v101, False))
        self.assertEqual(self.content(), [pipeline.BASE_PROGRAM, UP])
        self.assertEqual(len(self.fake.calls), n)
        out = worker.op_game_version(self.install, self.cb, "v101", validate=checked)
        self.assertFalse(out["changed"])
        # installing again while v1.01 is kept: the switch, nothing rebuilt -- the checks say so
        worker.op_game_version(self.install, self.cb, "v100", validate=checked)
        r = U.preflight(self.install, online=False)
        self.assertTrue(r["ready"], r["blockers"])
        self.assertEqual(r["plan"]["source"], "kept")
        self.assertIn("already built", self.check_msg(r))
        out = self.install_update()
        self.assertTrue(out["switched"])
        self.assertEqual(len(self.fake.calls), n)

    def test_rollback_follows_too(self):
        self.install_update()
        a = worker.argparse.Namespace(install=self.install, to=None)
        with mock.patch.object(worker, "_layout", lambda a: self.lay):
            out = worker.cmd_rollback(a, None, None)
        self.assertEqual(out["active"], self.v100_build)
        self.assertEqual(out["official_content"], [pipeline.BASE_PROGRAM])

    def test_rebuilds_keep_the_active_version_and_the_other_one_kept(self):
        self.install_update()
        for _ in range(2):   # launcher updates: the active version is rebuilt (here: a new game each time)
            self.fake.game = b"x"
            worker.op_update(self.install, self.cb, validate=checked, full=True)
            self.assertEqual(self.fake.calls[-1]["program"], UP)
        self.assertEqual(self.lay.program_of(self.lay.read_active()["active"]), UP)
        self.assertIn(self.v100_build, self.lay.kept())            # v1.00 still there for the switch back
        self.assertTrue(os.path.isdir(self.lay.build_dir(self.v100_build)))
        self.assertTrue(os.path.isdir(os.path.join(self.lay.gen_root, "lift-v100")))

    def test_v101_without_its_files_is_rebuilt_as_v100(self):
        self.install_update()
        shutil.rmtree(official_update.folder(self.install))
        worker.op_update(self.install, self.cb, validate=checked, full=True)
        self.assertIsNone(self.fake.calls[-1]["program"])
        self.assertEqual(self.content(), [pipeline.BASE_PROGRAM])
        with self.assertRaises(pipeline.SetupError) as c:
            for b in [b for b in self.lay.builds() if self.lay.program_of(b) == UP]:
                layout.rmtree(self.lay.build_dir(b))
            worker.op_game_version(self.install, self.cb, "v101", validate=checked)
        self.assertEqual(c.exception.code, "update_missing")


if __name__ == "__main__":
    unittest.main()
