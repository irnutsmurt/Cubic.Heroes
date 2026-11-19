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
        with open(vdf, "rb") as f:
            before = f.read()
        # Steam running: its list left alone, quietly (no error); the entry is reported as left for later
        self.assertNotIn(vdf, uninstall.remove_shortcuts(exe, roots=[steam], steam_running=True))
        with open(vdf, "rb") as f:
            self.assertEqual(f.read(), before)
        self.assertTrue(uninstall.steam_entry_left(exe, roots=[steam]))
        # Steam closed: removed (only the Steam entry when links=False: the launcher's later clean-up)
        done = uninstall.remove_shortcuts(exe, roots=[steam], steam_running=False, links=False)
        self.assertEqual(done, [vdf])
        self.assertFalse(uninstall.steam_entry_left(exe, roots=[steam]))
        left = shortcuts.read_shortcuts(vdf)
        self.assertEqual([e["AppName"] for e in left.values()], ["Other"])
        # a list with nothing of ours is never written
        other = os.path.join(steam, "userdata", "456", "config")
        os.makedirs(other)
        ovdf = os.path.join(other, "shortcuts.vdf")
        shortcuts.write_shortcuts(ovdf, {"0": {"AppName": "Other", "Exe": '"C:\\\\other.exe"'}})
        t0 = os.path.getmtime(ovdf)
        os.utime(ovdf, (t0 - 100, t0 - 100))
        self.assertNotIn(ovdf, uninstall.remove_shortcuts(exe, roots=[steam], steam_running=False))
        self.assertEqual(os.path.getmtime(ovdf), t0 - 100)


class RemoveLauncherTest(unittest.TestCase):
    """the launcher itself removed: only a packaged download's own items, never an install inside, never through a
    link, the player's other files and the game's install kept"""

    def setUp(self):
        self.tmpd = tempfile.TemporaryDirectory()
        t = self.tmpd.name
        self.pkg = os.path.join(t, "Cubic.Heroes")
        exe = "CubicHeroes.exe" if os.name == "nt" else "CubicHeroes"
        put(os.path.join(self.pkg, exe), 100)
        put(os.path.join(self.pkg, "worker", "DotHeroes-Setup.exe" if os.name == "nt" else "DotHeroes-Setup"), 50)
        put(os.path.join(self.pkg, "worker", "_internal", "python.dll"), 30)
        put(os.path.join(self.pkg, "licenses", "SDL2.txt"), 5)
        put(os.path.join(self.pkg, "README.txt"), 5)
        put(os.path.join(self.pkg, "my notes.txt"), 7)               # the player's: kept
        self.outside = os.path.join(t, "elsewhere")
        put(os.path.join(self.outside, "precious.txt"), 11)
        layout.make_link(os.path.join(self.pkg, "worker", "outlink"), self.outside)
        self.data = os.path.join(t, "appdata", "CubicHeroes")
        put(os.path.join(self.data, "launcher.json"), 20)
        put(os.path.join(self.data, "logs", "worker-install.jsonl"), 20)
        put(os.path.join(self.data, "other app file.txt"), 3)        # not the launcher's: kept
        self.install = os.path.join(t, "install")
        put(os.path.join(self.install, "active.json"), 5)
        put(os.path.join(self.install, "data", "gamedata", "save.bin"), 9)
        put(os.path.join(self.install, "config", "dotheroes.cfg"), 4)

    def tearDown(self):
        self.tmpd.cleanup()

    def test_plan_and_remove(self):
        p = uninstall.launcher_plan(self.pkg, self.data, self.install)
        self.assertEqual(p["bytes"], 100 + 50 + 30 + 5 + 5 + 20 + 20)
        self.assertEqual(p["kept_items"], ["my notes.txt"])
        self.assertTrue(os.path.exists(os.path.join(self.pkg, "README.txt")))   # the plan removes nothing
        out = uninstall.remove_launcher(self.pkg, self.data, self.install)
        self.assertEqual(sorted(os.listdir(self.pkg)), ["my notes.txt"])
        self.assertFalse(out["folder_removed"])
        self.assertEqual(sorted(os.listdir(self.data)), ["other app file.txt"])
        self.assertTrue(os.path.exists(os.path.join(self.outside, "precious.txt")))   # through the link: untouched
        for kept in ("active.json", os.path.join("data", "gamedata", "save.bin"), os.path.join("config", "dotheroes.cfg")):
            self.assertTrue(os.path.exists(os.path.join(self.install, kept)), kept)

    def test_folder_gone_when_only_the_package(self):
        os.remove(os.path.join(self.pkg, "my notes.txt"))
        out = uninstall.remove_launcher(self.pkg, self.data)
        self.assertTrue(out["folder_removed"])
        self.assertFalse(os.path.exists(self.pkg))

    def test_install_in_a_players_folder_inside_is_kept(self):
        inner = os.path.join(self.pkg, "game install")
        put(os.path.join(inner, "active.json"), 5)
        put(os.path.join(inner, "data", "gamedata", "save.bin"), 9)
        p = uninstall.launcher_plan(self.pkg, self.data)
        self.assertIn("game install", p["kept_items"])
        uninstall.remove_launcher(self.pkg, self.data, install=inner, roots=[])
        self.assertEqual(sorted(os.listdir(self.pkg)), ["game install", "my notes.txt"])
        self.assertTrue(os.path.exists(os.path.join(inner, "data", "gamedata", "save.bin")))

    def test_refusals(self):
        inner = os.path.join(self.pkg, "worker", "game install")   # inside a package item: it would go with it
        put(os.path.join(inner, "active.json"), 5)
        with self.assertRaises(uninstall.UninstallError) as e:
            uninstall.launcher_plan(self.pkg, self.data)
        self.assertEqual(e.exception.code, "install_inside")
        with self.assertRaises(uninstall.UninstallError) as e:
            uninstall.remove_launcher(self.pkg, self.data, install=inner)
        self.assertEqual(e.exception.code, "install_inside")
        self.assertTrue(os.path.exists(os.path.join(self.pkg, "README.txt")))   # nothing removed
        import shutil
        shutil.rmtree(inner)
        os.makedirs(os.path.join(self.pkg, ".git"))   # a source checkout
        with self.assertRaises(uninstall.UninstallError) as e:
            uninstall.launcher_plan(self.pkg)
        self.assertEqual(e.exception.code, "not_packaged")
        os.rmdir(os.path.join(self.pkg, ".git"))
        link = os.path.join(self.tmpd.name, "pkg-link")
        layout.make_link(link, self.pkg)
        with self.assertRaises(uninstall.UninstallError) as e:
            uninstall.launcher_plan(link)
        self.assertEqual(e.exception.code, "unsafe_path")
        layout.remove_link(link)
        with self.assertRaises(uninstall.UninstallError) as e:
            uninstall.launcher_plan(self.outside)
        self.assertEqual(e.exception.code, "not_packaged")


def tree(root, names):
    """the files under root's named items, as bytes (links as their target, never followed)"""
    out = {}
    for n in names:
        p = os.path.join(root, n)
        if layout.is_link(p):
            out[n] = "link"
        elif os.path.isfile(p):
            with open(p, "rb") as f:
                out[n] = f.read()
        elif os.path.isdir(p):
            for d, dirs, fs in os.walk(p):
                dirs[:] = [x for x in dirs if not layout.is_link(os.path.join(d, x))]
                for fn in fs:
                    with open(os.path.join(d, fn), "rb") as f:
                        out[os.path.relpath(os.path.join(d, fn), root)] = f.read()
    return out


class PortableRootTest(unittest.TestCase):
    """the portable root: the launcher's folder is the install's root. Removing the launcher removes its package
    only; the install (saves, settings, builds, logs, downloads) stays -- removed too only with everything"""

    def setUp(self):
        self.tmpd = tempfile.TemporaryDirectory()
        self.root = os.path.join(self.tmpd.name, "Cubic Heroes ünïcode 測試")
        r = self.root
        exe = "CubicHeroes.exe" if os.name == "nt" else "CubicHeroes"
        put(os.path.join(r, exe), 100)
        put(os.path.join(r, "worker", "DotHeroes-Setup.exe" if os.name == "nt" else "DotHeroes-Setup"), 50)
        put(os.path.join(r, "licenses", "SDL2.txt"), 5)
        put(os.path.join(r, "README.txt"), 5)
        put(os.path.join(r, "downloads", "launcher-previous", exe), 100)   # an update's leftover: the launcher's
        put(os.path.join(r, "downloads", "0.2.0", "pkg.zip"), 30)
        put(os.path.join(r, layout.ROOT_MARKER), 2)
        put(os.path.join(r, "active.json"), 5)
        put(os.path.join(r, "builds", "b1", "dotheroes.exe"), 70)
        layout.make_link(os.path.join(r, "game"), os.path.join(r, "builds", "b1"))
        put(os.path.join(r, "data", "gamedata", "save.bin"), 9)
        put(os.path.join(r, "data", "disc", "player-copy.iso"), 9)   # a disc image: never removed
        put(os.path.join(r, "data", "cache", "zig-global", "z"), 12)
        put(os.path.join(r, "config", "dotheroes.cfg"), 4)
        put(os.path.join(r, "logs", "worker-install.jsonl"), 6)
        put(os.path.join(r, "backups", "saves", "x", "save.bin"), 9)
        put(os.path.join(r, "launcher.json"), 8)                      # the launcher's preferences, in the root
        put(os.path.join(r, "my notes.txt"), 7)
        self.install_items = layout.INSTALL_ITEMS + (layout.ROOT_MARKER, "my notes.txt")

    def tearDown(self):
        if layout.is_link(os.path.join(self.root, "game")):
            layout.remove_link(os.path.join(self.root, "game"))
        self.tmpd.cleanup()

    def test_plan_keeps_the_install(self):
        p = uninstall.launcher_plan(self.root, self.root, self.root)
        self.assertTrue(p["portable"] and p["install_kept"])
        self.assertEqual(p["bytes"], 100 + 50 + 5 + 5 + 100)   # the package and the update's leftover: nothing else
        for n in ("active.json", "builds", "data", "config", "logs", "backups", "downloads", "my notes.txt"):
            self.assertIn(n, p["kept_items"])

    def test_remove_keeps_the_install_byte_for_byte(self):
        before = tree(self.root, self.install_items)
        out = uninstall.remove_launcher(self.root, self.root, self.root, roots=[])
        self.assertTrue(out["install_kept"])
        self.assertFalse(out["folder_removed"])
        after = tree(self.root, self.install_items)
        before.pop(os.path.join("downloads", "launcher-previous", "CubicHeroes.exe" if os.name == "nt" else
                                "CubicHeroes"))
        self.assertEqual(after, before)
        for n in layout.launcher_package():
            self.assertFalse(os.path.lexists(os.path.join(self.root, n)), n)
        self.assertTrue(os.path.exists(os.path.join(self.root, "launcher.json")), "the data folder in the install: kept")
        self.assertTrue(layout.is_link(os.path.join(self.root, "game")))

    def test_everything(self):
        p = uninstall.launcher_plan(self.root, self.root, self.root, everything=True)
        self.assertTrue(p["everything"] and not p["install_kept"])
        self.assertGreater(p["install_bytes"], 0)
        self.assertEqual(p["kept_items"], ["my notes.txt"])
        out = uninstall.remove_launcher(self.root, self.root, self.root, everything=True, roots=[])
        self.assertTrue(out["install_removed"])
        left = []
        for d, _, fs in os.walk(self.root):
            left += [os.path.relpath(os.path.join(d, f), self.root) for f in fs]
        self.assertEqual(sorted(left), sorted(["my notes.txt", os.path.join("data", "disc", "player-copy.iso")]))

    def test_uninstall_in_the_root_leaves_the_launcher(self):
        held = open(os.path.join(self.root, "logs", "worker-uninstall.jsonl"), "a")   # the worker's own --log, open
        try:
            out = uninstall.remove(self.root, roots=[])
        finally:
            held.close()
        if os.name == "nt":   # Windows refuses to remove a file held open: left and said, the rest removed
            self.assertEqual(out["left"], [os.path.join(self.root, "logs", "worker-uninstall.jsonl")])
        self.assertFalse(os.path.exists(os.path.join(self.root, "builds")))
        self.assertEqual(sorted(out["kept"]), ["saves", "settings"])
        self.assertTrue(os.path.exists(os.path.join(self.root, "worker")))
        self.assertTrue(os.path.exists(os.path.join(self.root, "README.txt")))
        self.assertFalse(os.path.exists(os.path.join(self.root, "data", "cache", "zig-global")),
                         "Zig's caches are the objects part")

    def test_the_launchers_programs_are_not_the_game(self):
        r = self.root
        found = [os.path.join(r, "CubicHeroes.exe"), os.path.join(r, "worker", "DotHeroes-Setup.exe"),
                 os.path.join(r, "downloads", "launcher-helper", "DotHeroes-Setup.exe"),
                 os.path.join(r, "game", "dotheroes.exe")]
        real = uninstall._running_from
        uninstall._running_from = lambda root: found
        try:
            self.assertEqual(uninstall.running_from(r), [found[-1]])
        finally:
            uninstall._running_from = real

    def test_worker_needs_a_separate_confirmation_for_everything(self):
        import subprocess
        import json
        w = [sys.executable, os.path.join(ROOT, "tools", "worker.py"), "remove-launcher", "--launcher", self.root,
             "--install", self.root]
        p = subprocess.run(w + ["--confirm", "--everything"], capture_output=True, text=True, encoding="utf-8",
                           timeout=60)
        res = json.loads(p.stdout.splitlines()[-1])
        self.assertEqual(res["code"], "needs_confirmation")
        self.assertTrue(os.path.exists(os.path.join(self.root, "data", "gamedata", "save.bin")))
        p = subprocess.run(w + ["--dry-run", "--everything"], capture_output=True, text=True, encoding="utf-8",
                           timeout=60)
        res = json.loads(p.stdout.splitlines()[-1])
        self.assertTrue(res["ok"], res)
        self.assertTrue(res["data"]["everything"])


@unittest.skipIf(os.name == "nt", "Linux's desktop entries")
class LinuxShortcutsTest(unittest.TestCase):
    """M11.4: the menu and desktop entries, their icon, and their removal -- in temporary folders, never the user's"""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = self.tmp.name
        self.dirs = {"apps_dir": os.path.join(t, "applications"), "desktop_dir": os.path.join(t, "Desktop"),
                     "icons_dir": os.path.join(t, "icons")}

    def tearDown(self):
        self.tmp.cleanup()

    def test_made_and_removed_for_this_launcher_only(self):
        exe = os.path.join(self.tmp.name, "Games dir", "Cubic 日本", "Cubic.Heroes-1.0.0-x86_64.AppImage")
        made = shortcuts.create_shortcuts(exe, "", desktop=True, **self.dirs)
        self.assertEqual(len(made), 2)
        text = open(made[0], encoding="utf-8").read()
        self.assertIn("Icon=cubicheroes", text)
        self.assertIn('Exec="' + exe + '"', text)
        icon = os.path.join(self.dirs["icons_dir"], "hicolor", "256x256", "apps", "cubicheroes.png")
        self.assertTrue(os.path.isfile(icon))
        # another copy's entry is not this launcher's: left as it is
        self.assertEqual(shortcuts.remove_linux_shortcuts(exe + "-other", **self.dirs), [])
        removed = shortcuts.remove_linux_shortcuts(exe, **self.dirs)
        self.assertEqual(sorted(removed), sorted(made + [icon]))
        self.assertFalse(any(os.path.exists(p) for p in made + [icon]))


if __name__ == "__main__":
    unittest.main()
