#!/usr/bin/env python3
"""The install with build slots (tools/install_layout.py) and the worker's operations on it (M9.1):
    build/venv/Scripts/python.exe -m unittest tests.test_install_layout
On made-up installs in temporary folders (a "game" is a few bytes; the conversion and the build are stood in for):
slots, the active build and the kept ones, game/ (a junction or symlink -- never followed by a removal), rollback,
pruning only what no kept build uses, a build that fails its check or is cancelled leaving the active one as it
was, an update with nothing changed, save backups (restore keeps the saves it replaces; delete needs confirmation),
and the setup app's layout migrated. The real conversion: tests/player_install_test.py."""
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import install_layout as layout  # noqa: E402
import setup_pipeline as pipeline  # noqa: E402
import worker  # noqa: E402

EXE = layout.exe_name()


def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb" if isinstance(data, bytes) else "w") as f:
        f.write(data)


def get(path):
    with open(path, "rb") as f:
        return f.read()


class Fake:
    """pipeline.run stood in for: a game of the given bytes into the staged slot, its lift and objects named"""

    def __init__(self, game=b"game 1", lift="lift-a", objects=("a.o", "b.o"), shaders="sh-1", inputs=None,
                 fail=None):
        self.game, self.lift, self.objects, self.shaders, self.inputs, self.fail = game, lift, objects, shaders, \
            inputs, fail
        self.calls = []

    def __call__(self, install, out_dir=None, unchanged_inputs=None, progress=None, **kw):
        self.calls.append(dict(kw, out_dir=out_dir, unchanged_inputs=unchanged_inputs))
        inputs = self.inputs or self.game.hex()
        if unchanged_inputs == inputs:
            return {"unchanged": True, "inputs": inputs, "lift": self.lift}
        progress("build", 0.5, "x")
        if self.fail:
            raise self.fail
        put(os.path.join(out_dir, EXE), self.game)
        lay = layout.Layout(install)
        os.makedirs(os.path.join(lay.gen_root, self.lift), exist_ok=True)
        for o in self.objects:
            put(os.path.join(lay.objects, o), o)
        os.makedirs(os.path.join(lay.shaders, self.shaders), exist_ok=True)
        return {"exe": os.path.join(out_dir, EXE), "lift": self.lift, "objects": list(self.objects),
                "shader_cache": self.shaders, "version": "test", "inputs": inputs}


def checked(slot, log):
    return {"info": {"ffmpeg": {"ok": True}}, "memtest": "test"}


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="layout-test-")
        self.install = os.path.join(self.tmp, "install")
        self.lay = layout.Layout(self.install)
        self.real_run = pipeline.run
        self.cb = worker.Callbacks()

    def tearDown(self):
        pipeline.run = self.real_run
        if os.path.isdir(self.install) and layout.is_link(self.lay.game_link):
            layout.remove_link(self.lay.game_link)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def build(self, fake):
        pipeline.run = fake
        return worker.build_slot(self.lay, self.cb, validate=checked)


class Slots(Base):
    def test_first_build_active(self):
        self.assertEqual(self.lay.kind(), "none")
        bid, info, changed = self.build(Fake())
        self.assertTrue(changed)
        self.assertEqual(self.lay.kind(), "slots")
        st = self.lay.read_active()
        self.assertEqual((st["active"], st["previous"], st["retention"]), (bid, [], 1))
        self.assertTrue(layout.is_link(self.lay.game_link))
        self.assertEqual(get(os.path.join(self.lay.game_link, EXE)), b"game 1")
        self.assertEqual(info["lift"], "lift-a")
        for f in ("build.json", "build.cfg"):
            self.assertTrue(os.path.exists(os.path.join(self.lay.build_dir(bid), f)))
        with open(os.path.join(self.lay.build_dir(bid), "build.cfg")) as f:
            self.assertIn("DOTHEROES_SHADER_CACHE_ID=sh-1", f.read())
        self.assertEqual([n for n in os.listdir(self.lay.builds_dir) if n.startswith(".")], [], "no staging left")

    def test_update_keeps_one_previous_and_prunes(self):
        a, _, _ = self.build(Fake(b"game 1", "lift-a", ("a.o", "b.o"), "sh-1"))
        b, _, _ = self.build(Fake(b"game 2", "lift-b", ("a.o", "c.o"), "sh-1"))
        self.assertEqual(self.lay.read_active()["previous"], [a])
        c, _, _ = self.build(Fake(b"game 3", "lift-c", ("a.o", "d.o"), "sh-2"))
        self.assertEqual(self.lay.read_active()["previous"], [b])
        gone = self.lay.prune()["removed"]
        self.assertIn(f"builds/{a}", gone)
        self.assertIn("data/gen/lift-a", gone)
        self.assertIn("data/cache/objects/b.o", gone)
        for kept in ("lift-b", "lift-c"):
            self.assertTrue(os.path.isdir(os.path.join(self.lay.gen_root, kept)), kept)
        for kept in ("a.o", "c.o", "d.o"):   # the kept build b still links c.o: never removed while it is kept
            self.assertTrue(os.path.exists(os.path.join(self.lay.objects, kept)), kept)
        for kept in ("sh-1", "sh-2"):
            self.assertTrue(os.path.isdir(os.path.join(self.lay.shaders, kept)), kept)
        self.assertEqual(self.lay.kept_objects(), {"a.o", "c.o", "d.o"})

    def test_rollback_and_forward(self):
        a, _, _ = self.build(Fake(b"game 1"))
        b, _, _ = self.build(Fake(b"game 2"))
        st = self.lay.rollback()
        self.assertEqual((st["active"], st["previous"]), (a, [b]))
        self.assertEqual(get(os.path.join(self.lay.game_link, EXE)), b"game 1")
        st = self.lay.rollback()
        self.assertEqual((st["active"], st["previous"]), (b, [a]))
        self.assertEqual(get(os.path.join(self.lay.game_link, EXE)), b"game 2")

    def test_rollback_refuses_a_damaged_build(self):
        a, _, _ = self.build(Fake(b"game 1"))
        self.build(Fake(b"game 2"))
        put(os.path.join(self.lay.build_dir(a), EXE), b"game X")   # the same size, other bytes
        with self.assertRaises(layout.LayoutError) as e:
            self.lay.rollback()
        self.assertEqual(e.exception.code, "build_damaged")
        self.assertEqual(get(os.path.join(self.lay.game_link, EXE)), b"game 2", "the active build untouched")

    def test_a_repair_keeps_the_whole_build_for_rollback(self):
        a, _, _ = self.build(Fake(b"game 1"))
        b, _, _ = self.build(Fake(b"game 2"))
        os.remove(os.path.join(self.lay.build_dir(b), EXE))   # the active build damaged, then repaired
        c, _, _ = self.build(Fake(b"game 3"))
        self.assertEqual(self.lay.read_active()["previous"], [a], "the whole build kept, not the damaged one")
        self.assertIn(f"builds/{b}", self.lay.prune()["removed"])
        self.assertEqual(self.lay.rollback()["active"], a)

    def test_rollback_and_retention_keep_only_whole_builds(self):   # the review's W-4
        a, _, _ = self.build(Fake(b"game 1"))
        self.lay.set_retention(2)
        b, _, _ = self.build(Fake(b"game 2"))
        c, _, _ = self.build(Fake(b"game 3"))
        self.assertEqual(self.lay.read_active()["previous"], [b, a])
        os.remove(os.path.join(self.lay.build_dir(c), EXE))   # the active build damaged
        self.assertEqual(self.lay.rollback()["previous"], [a], "the damaged build is not kept for rollback")
        os.remove(os.path.join(self.lay.build_dir(a), EXE))   # a kept build damaged
        self.assertEqual(self.lay.set_retention(1)["previous"], [], "a damaged build is not kept by the retention")

    def test_rollback_without_previous(self):
        self.build(Fake())
        with self.assertRaises(layout.LayoutError) as e:
            self.lay.rollback()
        self.assertEqual(e.exception.code, "no_previous")

    def test_retention(self):
        a, _, _ = self.build(Fake(b"game 1"))
        self.lay.set_retention(2)
        b, _, _ = self.build(Fake(b"game 2"))
        c, _, _ = self.build(Fake(b"game 3"))
        self.assertEqual(self.lay.read_active()["previous"], [b, a])
        self.lay.set_retention(0)
        self.assertEqual(self.lay.read_active()["previous"], [])
        self.lay.prune()
        self.assertEqual(sorted(self.lay.builds()), [c])

    def test_same_game_reuses_its_slot(self):
        a, _, _ = self.build(Fake(b"game 1", inputs="i1"))
        b, _, _ = self.build(Fake(b"game 1", inputs="i2"))   # inputs differ, the game is byte for byte the same
        self.assertEqual(a, b)
        self.assertEqual(len(self.lay.builds()), 1)

    def test_nothing_changed(self):
        a, _, _ = self.build(Fake(b"game 1", inputs="same"))
        fake = Fake(b"game 2", inputs="same")
        bid, _, changed = self.build(fake)
        self.assertEqual((bid, changed), (a, False))
        self.assertEqual(fake.calls[0]["unchanged_inputs"], "same")
        self.assertEqual(get(os.path.join(self.lay.game_link, EXE)), b"game 1")

    def test_failed_check_leaves_the_active_build(self):
        a, _, _ = self.build(Fake(b"game 1"))

        def bad(slot, log):
            raise pipeline.SetupError("The new build failed its self-test.", "build_invalid")
        pipeline.run = Fake(b"game 2")
        with self.assertRaises(pipeline.SetupError):
            worker.build_slot(self.lay, self.cb, validate=bad)
        self.assertEqual(self.lay.read_active()["active"], a)
        self.assertEqual(get(os.path.join(self.lay.game_link, EXE)), b"game 1")
        self.assertEqual(sorted(self.lay.builds()), [a])
        self.assertEqual([n for n in os.listdir(self.lay.builds_dir) if n.startswith(".")], [])

    def test_failed_or_cancelled_build_leaves_the_active_build(self):
        a, _, _ = self.build(Fake(b"game 1"))
        for err in (pipeline.zig_build.BuildError("compiling failed"), pipeline.Cancelled(), OSError(28, "full")):
            with self.assertRaises(BaseException):
                self.build(Fake(b"game 2", fail=err))
            self.assertEqual(self.lay.read_active()["active"], a)
            self.assertEqual([n for n in os.listdir(self.lay.builds_dir) if n.startswith(".")], [])

    def test_leftover_staging_cleaned(self):
        self.build(Fake())
        os.makedirs(os.path.join(self.lay.builds_dir, ".staging-1-x", "sub"))
        self.build(Fake(b"game 2"))
        self.assertEqual([n for n in os.listdir(self.lay.builds_dir) if n.startswith(".")], [])

    def test_link_removal_never_follows(self):
        a, _, _ = self.build(Fake(b"game 1"))
        layout.rmtree(self.lay.game_link)   # the link only
        self.assertFalse(os.path.exists(self.lay.game_link))
        self.assertEqual(get(os.path.join(self.lay.build_dir(a), EXE)), b"game 1")
        self.lay.point_game(a)   # repaired
        self.assertEqual(get(os.path.join(self.lay.game_link, EXE)), b"game 1")

    def test_prune_leaves_the_install_alone_when_nothing_is_built(self):
        self.lay.ensure_dirs()
        put(os.path.join(self.lay.objects, "x.o"), "x")
        self.lay.prune()
        self.assertTrue(os.path.exists(os.path.join(self.lay.objects, "x.o")))


class HeldBuildFile(Base):
    """GitHub issue #5 (beta.4, Windows 11): the new build compiled, linked and checked, then its folder's rename into
    place refused -- "Access is denied" -- because another program (an antivirus scanning the game the check had just
    run, an indexer, a network drive's server) still held a file in it. Windows can't rename a folder with a file
    open in it; the rename now waits a while for it, and a longer hold is a plain message, the active build kept."""

    def hold(self, release_after=None):
        """the check holds the new game open (as a scanner would), let go after release_after seconds (None: never,
        until the test ends)"""
        held = []

        def validate(slot, log):
            f = open(os.path.join(slot, EXE), "rb")
            held.append(f)
            if release_after is not None:
                threading.Timer(release_after, f.close).start()
            return checked(slot, log)

        self.addCleanup(lambda: [f.close() for f in held])
        return validate

    def test_held_a_moment_then_put_in_place(self):
        pipeline.run = Fake(b"game 1")
        bid, _, _ = worker.build_slot(self.lay, self.cb, validate=self.hold(release_after=0.6))
        self.assertEqual(self.lay.read_active()["active"], bid)
        self.assertEqual(get(os.path.join(self.lay.game_link, EXE)), b"game 1")
        self.assertEqual([n for n in os.listdir(self.lay.builds_dir) if n.startswith(".")], [], "no staging left")

    @unittest.skipUnless(os.name == "nt", "Windows: a held file stops a folder's rename")
    def test_held_too_long_a_plain_refusal_the_active_build_kept(self):
        a, _, _ = self.build(Fake(b"game 1"))
        pipeline.run = Fake(b"game 2", "lift-b")
        with mock.patch.object(layout, "REPLACE_WAIT_S", 0.5):
            with self.assertRaises(layout.LayoutError) as e:
                worker.build_slot(self.lay, self.cb, validate=self.hold())
        self.assertEqual(e.exception.code, "io_error")
        self.assertIn("another program", str(e.exception))
        self.assertEqual(self.lay.read_active()["active"], a)
        self.assertEqual(get(os.path.join(self.lay.game_link, EXE)), b"game 1")
        self.assertEqual(self.lay.builds(), {a: self.lay.build_info(a)})

    def test_waits_with_backoff_then_gives_up_windows_only(self):
        calls = []
        with mock.patch.object(layout.os, "replace", side_effect=[PermissionError(13, "held"), PermissionError(13, "held"),
                                                                   None]) as rep:
            if os.name == "nt":
                layout.replace_when_free("a", "b", wait=5, sleep=calls.append)
                self.assertEqual(rep.call_count, 3)
                self.assertEqual(calls, [0.1, 0.2])
            else:   # elsewhere a PermissionError is a real one: no waiting
                with self.assertRaises(PermissionError):
                    layout.replace_when_free("a", "b", wait=5, sleep=calls.append)
                self.assertEqual((rep.call_count, calls), (1, []))


class Saves(Base):
    def saves(self, text):
        put(os.path.join(self.lay.gamedata, "dev_hdd0", "home", "00000001", "savedata", "SLOT", "DATA.BIN"), text)

    def current(self):
        return get(os.path.join(self.lay.gamedata, "dev_hdd0", "home", "00000001", "savedata", "SLOT", "DATA.BIN"))

    def test_backup_restore_delete(self):
        self.lay.ensure_dirs()
        self.saves("first")
        one = self.lay.backup_saves("manual", "before the boss")
        self.assertEqual((one["files"], one["automatic"], one["note"]), (1, False, "before the boss"))
        self.saves("second")
        out = self.lay.restore_saves(one["id"])
        self.assertEqual(self.current(), b"first")
        before = [b for b in self.lay.backups() if b["id"] == out["previous_saves"]][0]
        self.assertEqual(before["reason"], "before-restore")
        self.lay.restore_saves(before["id"])   # nothing lost: the replaced saves come back
        self.assertEqual(self.current(), b"second")
        self.lay.delete_backup(one["id"])
        self.assertNotIn(one["id"], [b["id"] for b in self.lay.backups()])

    def test_prune_keeps_manual_backups(self):
        self.lay.ensure_dirs()
        self.saves("x")
        manual = self.lay.backup_saves("manual")
        for _ in range(4):
            self.lay.backup_saves("before-update")
        gone = self.lay.prune_backups(keep_auto=2)
        self.assertEqual(len(gone), 2)
        ids = [b["id"] for b in self.lay.backups()]
        self.assertIn(manual["id"], ids)
        self.assertEqual(len(ids), 3)

    def test_update_backs_up_first(self):
        self.build(Fake(b"game 1"))
        self.saves("mine")
        pipeline.run = Fake(b"game 2")
        out = worker.op_update(self.install, self.cb, validate=checked)
        self.assertTrue(out["changed"])
        b = [x for x in self.lay.backups() if x["id"] == out["saves_backup"]][0]
        self.assertEqual(b["reason"], "before-update")
        self.assertEqual(self.current(), b"mine", "the saves untouched by the update")

    def test_saves_in_no_slot(self):
        self.saves("mine")
        a, _, _ = self.build(Fake(b"game 1"))
        for d, _, fs in os.walk(self.lay.build_dir(a)):
            self.assertNotIn("DATA.BIN", fs)


class Migration(Base):
    def legacy(self, same_tools=True):
        r = self.install
        put(os.path.join(r, EXE), b"old game")
        put(os.path.join(r, "SDL2.dll"), b"lib")
        put(os.path.join(r, "FFmpeg-NOTICE.txt"), "notice")
        put(os.path.join(r, "dotheroes.cfg"), "DOTHEROES_FPS=30\n")
        put(os.path.join(r, "data", "disc", "PS3_GAME", "USRDIR", "EBOOT.elf"), b"elf")
        for d in ("out", "recompiled", "spu", "spu_native"):
            put(os.path.join(r, "data", "gen", d, "f.txt"), d)
        put(os.path.join(r, "data", "cache", "a-1234.o"), "obj")
        put(os.path.join(r, "data", "cache", "timings.json"), "{}")
        put(os.path.join(r, "data", "cache", "shaders", "0001.spv"), b"spv")
        put(os.path.join(r, "data", "gamedata", "save.bin"), b"mine")
        put(os.path.join(r, "data", "dotheroes.log"), "log")
        elf_sha = "e" * 64
        stamps = {"recomp": pipeline.tool_fingerprint("recomp", elf_sha) if same_tools else "old",
                  "spu": pipeline.tool_fingerprint("spu", elf_sha) if same_tools else "old"}
        layout.write_json(os.path.join(r, "data", "install.json"),
                          {"version": "0.1", "stamps": stamps, "elf_sha256": elf_sha})
        return pipeline.lift_id(elf_sha)

    def test_migrate_keeps_the_lift_and_the_saves(self):
        lid = self.legacy()
        self.assertEqual(self.lay.kind(), "setup-app")
        pipeline.run = Fake(b"new game", lift=lid, objects=("a-1234.o",), shaders="sh-9")
        out = worker.op_migrate(self.install, self.cb, validate=checked)
        self.assertTrue(out["lift_kept"])
        self.assertEqual(self.lay.kind(), "slots")
        self.assertFalse(os.path.exists(os.path.join(self.install, EXE)), "the old game removed")
        self.assertFalse(os.path.exists(os.path.join(self.install, "SDL2.dll")))
        self.assertEqual(get(os.path.join(self.lay.game_link, EXE)), b"new game")
        self.assertTrue(os.path.exists(os.path.join(self.lay.gen_root, lid, "recompiled", "f.txt")))
        self.assertTrue(os.path.exists(os.path.join(self.lay.objects, "a-1234.o")))
        self.assertTrue(os.path.exists(os.path.join(self.lay.shaders, "sh-9", "0001.spv")),
                        "the translations kept, under the build's translator")
        with open(self.lay.cfg) as f:
            self.assertIn("DOTHEROES_FPS=30", f.read(), "the player's settings kept")
        self.assertEqual(get(os.path.join(self.lay.gamedata, "save.bin")), b"mine")
        self.assertTrue(os.path.exists(os.path.join(self.lay.logs, "game.setup-app.log")))
        b = [x for x in self.lay.backups() if x["id"] == out["saves_backup"]][0]
        self.assertEqual(b["reason"], "before-migration")

    def test_migrate_with_other_tools_relifts(self):
        self.legacy(same_tools=False)
        pipeline.run = Fake(b"new game", lift="lift-new")
        out = worker.op_migrate(self.install, self.cb, validate=checked)
        self.assertFalse(out["lift_kept"])
        self.assertEqual(os.listdir(self.lay.gen_root), ["lift-new"], "the old lift's folders pruned")

    def test_migrate_failed_build_can_be_taken_up_again(self):
        self.legacy()
        pipeline.run = Fake(fail=pipeline.zig_build.BuildError("linking failed"))
        with self.assertRaises(pipeline.zig_build.BuildError):
            worker.op_migrate(self.install, self.cb, validate=checked)
        self.assertEqual(self.lay.kind(), "setup-app", "still the setup app's: migrate again")
        pipeline.run = Fake(b"new game")
        worker.op_migrate(self.install, self.cb, validate=checked)
        self.assertEqual(self.lay.kind(), "slots")
        self.assertEqual(get(os.path.join(self.lay.gamedata, "save.bin")), b"mine")

    def test_install_refuses_the_setup_apps_layout(self):
        self.legacy()
        with self.assertRaises(pipeline.SetupError) as e:
            worker.op_install(self.install, "disc.iso", "keys", None, self.cb, validate=checked)
        self.assertEqual(e.exception.code, "needs_migration")

    def test_only_the_setup_apps_known_files(self):
        """an unknown .dll or .txt beside the game is never taken for the setup app's"""
        self.legacy()
        put(os.path.join(self.install, "my notes.txt"), "the player's")
        put(os.path.join(self.install, "other.dll"), b"someone else's")
        self.assertEqual(self.lay.legacy_parts(), sorted([EXE, "SDL2.dll", "FFmpeg-NOTICE.txt", "dotheroes.cfg"]))


class PortableRoot(Base):
    """the install's root is the launcher's folder: the launcher's package is never the setup app's game"""

    def package(self):
        r = self.install
        for n in layout.LAUNCHER_PACKAGE["windows"] + layout.LAUNCHER_PACKAGE["linux"]:
            if n in ("licenses", "worker", "lib"):
                put(os.path.join(r, n, "x.txt"), n)
            else:
                put(os.path.join(r, n), n)
        put(os.path.join(r, "worker", "DotHeroes-Setup.exe"), b"worker")
        put(os.path.join(r, layout.ROOT_MARKER), "root")

    def test_nothing_of_the_launcher_selected(self):
        self.package()
        self.build(Fake(b"game 1"))
        put(os.path.join(self.install, "my notes.txt"), "the player's")
        self.assertTrue(layout.holds_launcher(self.install))
        self.assertEqual(self.lay.legacy_parts(), [])
        before = sorted(os.listdir(self.install))
        self.assertEqual(self.lay.remove_legacy_game(), [])
        self.assertEqual(sorted(os.listdir(self.install)), before)

    def test_a_setup_app_game_beside_the_launcher(self):
        """the setup app's game in the launcher's folder: its own files go, the launcher's SDL2.dll and lib/ stay"""
        self.package()
        put(os.path.join(self.install, EXE), b"old game")
        put(os.path.join(self.install, "OpenAL32.dll"), b"lib")
        put(os.path.join(self.install, "FFmpeg-LGPL-2.1.txt"), "licence")
        put(os.path.join(self.install, "dotheroes.cfg"), "DOTHEROES_FPS=30\n")
        self.assertEqual(self.lay.legacy_parts(), sorted([EXE, "OpenAL32.dll", "FFmpeg-LGPL-2.1.txt",
                                                          "dotheroes.cfg"]))
        self.lay.remove_legacy_game()
        for kept in layout.LAUNCHER_PACKAGE["windows"] + layout.LAUNCHER_PACKAGE["linux"] + (layout.ROOT_MARKER,
                                                                                             "dotheroes.cfg"):
            self.assertTrue(os.path.exists(os.path.join(self.install, kept)), kept)
        self.assertFalse(os.path.exists(os.path.join(self.install, "OpenAL32.dll")))


class Commands(Base):
    def run_worker(self, *args):
        p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "worker.py"), *args, "--install",
                            self.install], capture_output=True, text=True, timeout=60)
        return p.returncode, json.loads(p.stdout.splitlines()[-1])

    def test_builds_rollback_saves(self):
        a, _, _ = self.build(Fake(b"game 1"))
        b, _, _ = self.build(Fake(b"game 2"))
        rc, res = self.run_worker("builds")
        self.assertEqual(rc, 0)
        self.assertEqual([(x["id"], x["state"]) for x in res["data"]["builds"]], sorted([(a, "kept"), (b, "active")]))
        rc, res = self.run_worker("rollback")
        self.assertEqual((rc, res["data"]["active"]), (0, a))
        rc, res = self.run_worker("saves", "--action", "backup", "--note", "x")
        self.assertEqual(rc, 0)
        bid = res["data"]["id"]
        rc, res = self.run_worker("saves", "--action", "restore", "--backup", bid)
        self.assertEqual((rc, res["code"]), (1, "needs_confirmation"))
        rc, res = self.run_worker("saves", "--action", "restore", "--backup", bid, "--confirm")
        self.assertEqual(rc, 0)
        rc, res = self.run_worker("info")
        self.assertEqual((res["data"]["kind"], res["data"]["active"]), ("slots", a))
        rc, res = self.run_worker("retention", "--keep", "0")
        self.assertEqual((rc, res["data"]["previous"]), (0, []))
        rc, res = self.run_worker("rollback")
        self.assertEqual((rc, res["code"]), (1, "no_previous"))


if __name__ == "__main__":
    unittest.main()
