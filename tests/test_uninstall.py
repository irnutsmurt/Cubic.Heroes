#!/usr/bin/env python3
"""Uninstalling (tools/uninstall.py; docs/M9_LAUNCHER_PLAN.md section 20):
    build/venv/Scripts/python.exe -m unittest tests.test_uninstall
On made-up installs in temporary folders: what each part holds; the default removal keeps the saves, their backups
and the settings and leaves the folder a first install's; a link inside is removed as a link, its target untouched;
disc images and keys files are never removed; a link as the install, a folder that isn't an install, and a
preparation running are refused; the Steam entries made for the launcher removed, the others kept."""
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import install_layout as layout  # noqa: E402
import shortcuts  # noqa: E402
import uninstall  # noqa: E402


def put(path, n=10):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"x" * n)


class UninstallTest(unittest.TestCase):
    def setUp(self):
        self.tmpd = tempfile.TemporaryDirectory()
        self.tmp = self.tmpd.name
        self.root = os.path.join(self.tmp, "install")
        self.lay = layout.Layout(self.root)
        L = self.lay
        put(L.active_json, 5)
        put(os.path.join(L.builds_dir, "b1", "dotheroes.exe"), 100)
        layout.make_link(L.game_link, os.path.join(L.builds_dir, "b1"))
        put(os.path.join(L.logs, "game.log"), 7)
        put(L.install_json, 3)
        put(os.path.join(L.disc, "PS3_GAME", "USRDIR", "EBOOT.BIN"), 50)
        put(os.path.join(L.disc, "player-copy.iso"), 9)          # a disc image: never removed
        put(os.path.join(L.gen_root, "lift", "a.cpp"), 30)
        put(os.path.join(L.shaders, "abc", "x.spv"), 20)
        put(os.path.join(L.cache, "native-recipes.bin"), 15)
        put(os.path.join(L.objects, "o1.o"), 40)
        put(L.cfg, 4)
        put(os.path.join(L.gamedata, "save.bin"), 60)
        put(os.path.join(L.backups_dir, "20261114", "save.bin"), 60)
        self.outside = os.path.join(self.tmp, "elsewhere")
        put(os.path.join(self.outside, "precious.txt"), 11)
        layout.make_link(os.path.join(L.gen_root, "outlink"), self.outside)   # a junction out of the install

    def tearDown(self):
        self.tmpd.cleanup()

    def test_plan(self):
        p = uninstall.plan(self.root)
        self.assertEqual(p["builds"]["bytes"], 100 + 5 + 7)   # game/ a link: not counted twice
        self.assertEqual(p["data"]["bytes"], 50 + 30 + 3)      # the .iso and the link's target not counted
        self.assertEqual(p["shaders"]["bytes"], 35)
        self.assertEqual(p["objects"]["bytes"], 40)
        self.assertEqual(p["saves"]["bytes"], 120)
        self.assertEqual(p["settings"]["bytes"], 4)

    def test_default_keeps_saves_and_settings(self):
        out = uninstall.remove(self.root, roots=[])
        L = self.lay
        for gone in (L.builds_dir, L.game_link, L.active_json, L.install_json, L.gen_root, L.shaders, L.objects,
                     L.logs):
            self.assertFalse(os.path.lexists(gone), gone)
        for kept in (L.cfg, os.path.join(L.gamedata, "save.bin"), os.path.join(L.backups_dir, "20261114", "save.bin"),
                     os.path.join(L.disc, "player-copy.iso")):
            self.assertTrue(os.path.exists(kept), kept)
        self.assertTrue(os.path.exists(os.path.join(self.outside, "precious.txt")))   # through the link: untouched
        self.assertEqual(L.kind(), "none")   # the launcher's first-run screen
        self.assertEqual(sorted(out["kept"]), ["saves", "settings"])
        self.assertIn(os.path.join(L.disc, "player-copy.iso"), out["protected"])

    def test_everything_but_protected(self):
        uninstall.remove(self.root, uninstall.PARTS[:-1])
        self.assertFalse(os.path.exists(self.lay.gamedata))
        self.assertFalse(os.path.exists(self.lay.config))
        self.assertTrue(os.path.exists(os.path.join(self.lay.disc, "player-copy.iso")))
        self.assertTrue(os.path.exists(os.path.join(self.outside, "precious.txt")))

    def test_refusals(self):
        link = os.path.join(self.tmp, "install-link")
        layout.make_link(link, self.root)
        with self.assertRaises(uninstall.UninstallError) as e:
            uninstall.remove(link)
        self.assertEqual(e.exception.code, "unsafe_path")
        os.makedirs(os.path.join(self.tmp, "not-an-install"))
        with self.assertRaises(uninstall.UninstallError) as e:
            uninstall.remove(os.path.join(self.tmp, "not-an-install"))
        self.assertEqual(e.exception.code, "no_install")
        put(os.path.join(self.lay.cache, "prepare.lock"))
        with open(os.path.join(self.lay.cache, "prepare.lock"), "w") as f:
            f.write(str(os.getppid()))   # a live process prepares
        with self.assertRaises(uninstall.UninstallError) as e:
            uninstall.remove(self.root)
        self.assertEqual(e.exception.code, "busy")
        self.assertTrue(os.path.exists(self.lay.builds_dir))   # nothing removed
        layout.remove_link(link)

    def test_inside(self):
        self.assertFalse(uninstall._inside(self.root, self.root))
        self.assertFalse(uninstall._inside(self.root, self.outside))
        self.assertFalse(uninstall._inside(self.root, os.path.join(self.lay.gen_root, "outlink", "precious.txt")))
        self.assertTrue(uninstall._inside(self.root, self.lay.cfg))

    def test_steam_entries(self):
        steam = os.path.join(self.tmp, "steam")
        cfg = os.path.join(steam, "userdata", "123", "config")
        os.makedirs(cfg)
        exe = os.path.join(self.tmp, "launcher", "CubicHeroes.exe")
        vdf = os.path.join(cfg, "shortcuts.vdf")
        shortcuts.write_shortcuts(vdf, {"0": {"AppName": "Other", "Exe": '"C:\\\\other.exe"'},
                                        "1": {"AppName": shortcuts.NAME, "Exe": f'"{exe}"'}})
        done = uninstall.remove_shortcuts(exe, roots=[steam])
        self.assertIn(vdf, done)
        left = shortcuts.read_shortcuts(vdf)
        self.assertEqual([e["AppName"] for e in left.values()], ["Other"])


if __name__ == "__main__":
    unittest.main()
