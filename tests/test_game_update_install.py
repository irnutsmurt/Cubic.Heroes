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
    """the stand-in pipeline, aware of the program it builds: a different game for v1.01. disc_only (beta.6): the
    disc's steps alone -- the disc's program recorded as the install's own (its pin), nothing built"""
    v100_pin = None

    def __call__(self, install, out_dir=None, unchanged_inputs=None, progress=None, program=None, **kw):
        if kw.get("disc_only"):
            self.calls.append(dict(kw, out_dir=None, program=None))
            lay = layout.Layout(install)
            os.makedirs(lay.data, exist_ok=True)
            rec = pipeline.install_info(install) or {}
            rec["elf_sha256"] = self.v100_pin
            with open(lay.install_json, "w") as f:
                json.dump(rec, f)
            return {"disc_only": True, "elf_sha256": self.v100_pin}
        if program and getattr(self, "fail_program", None) == program:
            raise pipeline.SetupError("Recompiling the game failed.", "lift_failed")
        if program:
            self.game, self.lift, self.inputs = b"game v1.01", "lift-v101", "inputs-v101"
        else:
            self.game, self.lift, self.inputs = b"game v1.00", "lift-v100", "inputs-v100"
            lay = layout.Layout(install)
            if self.v100_pin and not os.path.exists(lay.install_json):   # the disc's program recorded, as the
                os.makedirs(lay.data, exist_ok=True)                      # pipeline records it
                with open(lay.install_json, "w") as f:
                    json.dump({"elf_sha256": self.v100_pin, "lift": "lift-v100"}, f)
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
        self.fake.v100_pin = self.v100_pin
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


class FirstInstall(Base):
    """beta.6 (the owner, 2026-11-27): v1.01 chosen on the install page -- the disc's steps, the update's, then ONE
    build, v1.01's; v1.00 built later from the installation alone; both versions when the player asks for both"""

    def setUp(self):
        super().setUp()
        self.new = os.path.join(self.d, "fresh")
        self.nlay = layout.Layout(self.new)
        self.addCleanup(lambda: os.path.isdir(self.new) and layout.is_link(self.nlay.game_link) and
                        layout.remove_link(self.nlay.game_link))

    def first_install(self, **kw):
        update = {"sources": key_sources.Sources(chosen=self.keys()), "online": False}
        update.update(kw.pop("update", {}))
        if "pkg" not in update:
            update["pkg"] = self.put_pkg()
        return worker.op_install(self.new, os.path.join(self.d, "disc.iso"), update["sources"], None, self.cb,
                                 validate=checked, game_version="v101", update=update, **kw)

    def builds_of(self, lay):
        return sorted(lay.program_of(b) for b in lay.builds())

    def test_v101_chosen_builds_v101_only(self):
        n = len(self.fake.calls)
        out = self.first_install()
        calls = self.fake.calls[n:]
        self.assertTrue(calls[0].get("disc_only"))                      # the disc's steps alone
        self.assertEqual([c.get("program") for c in calls[1:]], [UP])   # one build: v1.01's
        self.assertEqual(self.builds_of(self.nlay), [UP])               # no v1.00 build made
        self.assertEqual(self.nlay.program_of(self.nlay.read_active()["active"]), UP)
        self.assertEqual(get(os.path.join(self.nlay.game_link, EXE)), b"game v1.01")
        self.assertEqual((out["game_version"], out["program"], out["v100_build"]), ("v101", UP, None))
        self.assertEqual(out["official_content"], [pipeline.BASE_PROGRAM, UP])
        self.assertEqual(U.game_state(self.new)["state"], "v101")
        # the disc's program is still the installation's own: v1.00 can be built without the disc
        self.assertEqual(pipeline.install_info(self.new)["elf_sha256"], self.v100_pin)
        self.assertEqual(pipeline.install_info(self.new)["game_update"]["program"], UP)

    def test_v100_built_later_from_the_installation(self):
        self.first_install()
        n = len(self.fake.calls)
        out = worker.op_game_version(self.new, self.cb, "v100", validate=checked)
        self.assertTrue(out["rebuilt"])
        self.assertIsNone(self.fake.calls[n].get("source"))             # no disc asked for: the installation's copy
        self.assertIsNone(self.fake.calls[n].get("program"))
        self.assertEqual(get(os.path.join(self.nlay.game_link, EXE)), b"game v1.00")
        self.assertEqual(self.builds_of(self.nlay), ["", UP])           # both kept now: switching is instant
        out = worker.op_game_version(self.new, self.cb, "v101", validate=checked)
        self.assertFalse(out["rebuilt"])

    def test_both_versions(self):
        n = len(self.fake.calls)
        out = self.first_install(also_v100=True)
        self.assertEqual([c.get("program") for c in self.fake.calls[n:]], [None, UP])   # v1.00 first, then v1.01
        self.assertEqual(self.builds_of(self.nlay), ["", UP])
        self.assertEqual(self.nlay.program_of(self.nlay.read_active()["active"]), UP)
        self.assertEqual(out["v100_build"], self.nlay.newest_of(""))
        n = len(self.fake.calls)
        self.assertFalse(worker.op_game_version(self.new, self.cb, "v100", validate=checked)["rebuilt"])
        self.assertEqual(len(self.fake.calls), n)

    def test_update_not_available_keeps_the_discs_steps(self):
        """no package and no connection: refused after the disc's steps, with an update_* code; nothing built; an
        install of v1.00 then needs no disc step again (resume) -- the launcher's "Install v1.00 instead" """
        n = len(self.fake.calls)
        beside = os.path.join(self.d, self.rec["file"])   # none beside the disc either
        if os.path.exists(beside):
            os.remove(beside)
        with self.assertRaises(pipeline.SetupError) as c:
            self.first_install(update={"pkg": None})
        self.assertEqual(c.exception.code, "update_not_ready")
        self.assertEqual(len(self.fake.calls), n + 1)                   # the disc's steps only
        self.assertEqual(self.nlay.builds(), {})
        self.assertEqual(pipeline.install_info(self.new)["elf_sha256"], self.v100_pin)
        worker.op_install(self.new, os.path.join(self.d, "disc.iso"), None, None, self.cb, validate=checked,
                          game_version="v100", resume=True)
        self.assertTrue(self.fake.calls[-1].get("resume"))
        self.assertEqual(self.builds_of(self.nlay), [""])

    def test_v101_build_failing_is_the_updates(self):
        """v1.01's build failing after the disc's steps: an update_* error (the launcher offers v1.00), nothing built"""
        self.fake.fail_program = UP
        with self.assertRaises(pipeline.SetupError) as c:
            self.first_install()
        self.assertEqual(c.exception.code, "update_lift_failed")
        self.assertEqual(self.nlay.builds(), {})
        self.assertEqual(pipeline.install_info(self.new)["elf_sha256"], self.v100_pin)

    def test_v100_chosen(self):
        out = worker.op_install(self.new, os.path.join(self.d, "disc.iso"), None, None, self.cb, validate=checked,
                                game_version="v100")
        self.assertEqual(out["game_version"], "v100")
        self.assertEqual(self.builds_of(self.nlay), [""])
        self.assertFalse(any(c.get("disc_only") for c in self.fake.calls))

    def test_the_planned_check_before_anything_is_installed(self):
        info = {"kind": "iso", "title_id": "BLUS30490", "app_ver": "01.00", "title": "3D DOT GAME HEROES"}
        with mock.patch.object(pipeline, "probe_disc", lambda *a: info):
            r = U.preflight(self.new, disc=os.path.join(self.d, "disc.iso"), pkg=self.put_pkg(), keys=self.keys(),
                            online=False, planned=True)
        self.assertTrue(r["ready"], r["blockers"])
        self.assertEqual(r["state"], "v100")
        self.assertTrue(r["plan"]["planned"])
        self.assertIn("can be installed with it", self.check_msg(r, "game"))
        self.assertFalse(os.path.exists(self.new))                      # nothing written for a check
        # another disc: refused, with the reason
        with mock.patch.object(pipeline, "probe_disc", side_effect=pipeline.SetupError("This is another game.",
                                                                                         "unsupported_version")):
            r = U.preflight(self.new, disc=os.path.join(self.d, "disc.iso"), online=False, planned=True)
        self.assertFalse(r["ready"])
        self.assertIn("another game", next(c for c in r["checks"] if c["id"] == "game")["details"])
        # without planned, nothing installed is still "install the game first"
        r = U.preflight(self.new, online=False)
        self.assertEqual(r["state"], "not_installed")

    def test_both_versions_the_update_failing_leaves_v100_installed(self):
        """the update refused once v1.00 is built: v1.00 active and playable, the failure the update's (update_*)"""
        with self.assertRaises(pipeline.SetupError) as c:
            self.first_install(also_v100=True, update={"pkg": self.put_pkg(b"not the package")})
        self.assertTrue(c.exception.code.startswith("update_"), c.exception.code)
        self.assertEqual(self.builds_of(self.nlay), [""])
        self.assertEqual(get(os.path.join(self.nlay.game_link, EXE)), b"game v1.00")
        self.assertEqual(U.game_state(self.new)["state"], "v100")
        # a build failure of v1.01 is the update's too
        self.fake.fail_program = UP
        with self.assertRaises(pipeline.SetupError) as c:
            self.first_install(also_v100=True)
        self.assertEqual(c.exception.code, "update_lift_failed")
        self.assertEqual(self.nlay.program_of(self.nlay.read_active()["active"]), "")

    def test_the_disc_steps_stay_done_in_the_progress(self):
        """Linux's review of 972077c: v1.01's build "skips" the disc's steps the install has done -- they stay done,
        and the bar never goes back"""
        events = []

        class Ev:
            def emit(self, ev, **kw):
                events.append(dict(kw, ev=ev))
        prog = worker.Progress(Ev(), worker.Callbacks(), worker.install_steps("v101"))
        for st in ("verify", "extract", "decrypt"):
            prog.progress(st, 0.0, "x")
            prog.progress(st, 1.0, "x")
        prog.progress("update-download", 0.5, "x")
        before = prog.overall()
        for st in ("verify", "extract", "decrypt"):
            prog.skip(st)
        self.assertGreaterEqual(prog.overall(), before)
        self.assertFalse([e for e in events if e["ev"] == "stage" and e.get("state") == "skipped"])
        prog.skip("update-extract")   # a step not done yet is still skipped
        self.assertEqual(prog.state["update-extract"], "skipped")

    def test_a_cancel_while_preparing_keeps_the_install(self):
        """stopped during the shaders at an install's end: the build stays made and active, the operation done"""
        os.makedirs(self.lay.cache, exist_ok=True)
        open(os.path.join(self.lay.cache, "native-recipes.bin"), "wb").close()

        def stopped(*a, **k):
            raise pipeline.Cancelled()
        with mock.patch.object(worker, "op_prepare", stopped), \
                mock.patch.object(worker, "first_session_recipes", lambda *a: None):
            out = worker.prepare_after(self.lay, self.cb, worker.validate_build, True)
        self.assertTrue(out["cancelled"])

    def test_the_space_for_both_versions(self):
        needs = []
        with mock.patch.object(worker, "check_space", lambda install, need: needs.append(need)):
            self.first_install(also_v100=True)
        self.assertEqual(needs, [worker.NEEDED["install-both"]])
        self.assertEqual(worker.NEEDED["install-both"], worker.NEEDED["install"] + worker.NEEDED["update"])

    def test_the_steps_of_each_install(self):
        self.assertEqual(worker.install_steps(), pipeline.STEPS + worker.BUILD_TAIL)
        v101 = worker.install_steps("v101")
        self.assertEqual(v101[:6], ["verify", "extract", "decrypt", "update-download", "update-extract",
                                    "update-decrypt"])
        self.assertEqual(v101.count("build"), 1)
        both = worker.install_steps("v101", True)
        self.assertIn("build:v100", both)
        self.assertLess(both.index("activate:v100"), both.index("update-download"))
        self.assertEqual(len(both), len(set(both)))                     # each step once
        self.assertEqual(worker.step_name("build:v100"), worker.step_name("build") + " (v1.00)")


if __name__ == "__main__":
    unittest.main()
