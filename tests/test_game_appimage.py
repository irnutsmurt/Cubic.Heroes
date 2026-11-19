#!/usr/bin/env python3
"""tools/game_appimage.py's start script (M11.4, the owner's D2): how the player's game AppImage finds its installation
-- relatively, so the root and its AppImage move together; through the pointer file or CUBIC_INSTALL_DIR; the root it
was made in when copied away alone -- and how it starts the game (direct play, the root as its folder). A stand-in
program prints what it was given; no AppImage is made and no game runs."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import game_appimage  # noqa: E402


@unittest.skipIf(os.name == "nt", "Linux's AppImage")
class StartScript(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="game appimage 日本 ")
        self.a = os.path.join(self.tmp, "root A")
        self.b = os.path.join(self.tmp, "root B")
        self.elsewhere = os.path.join(self.tmp, "elsewhere")
        for d in (os.path.join(self.a, "exports"), self.b, self.elsewhere):
            os.makedirs(d)
        for r in (self.a, self.b):
            open(os.path.join(r, "active.json"), "w").close()
        self.appdir = os.path.join(self.a, "exports")
        prog = os.path.join(self.appdir, "dotheroes")
        with open(prog, "w") as f:
            f.write('#!/bin/sh\necho "$DOTHEROES_INSTALL_ROOT|$CUBIC_DIRECT_PLAY|$(pwd)|$*"\n')
        os.chmod(prog, 0o755)
        with open(os.path.join(self.appdir, "AppRun"), "w") as f:
            f.write(game_appimage.apprun(self.a))
        os.chmod(os.path.join(self.appdir, "AppRun"), 0o755)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_as(self, appimage, **env):
        e = {k: v for k, v in os.environ.items() if k != "CUBIC_INSTALL_DIR"}
        e.update(env, APPIMAGE=appimage)
        r = subprocess.run([os.path.join(self.appdir, "AppRun"), "a b"], env=e, capture_output=True, text=True)
        return r.returncode, r.stdout.strip().split("|") if r.returncode == 0 else r.stderr

    def test_relative_to_its_root(self):
        rc, (root, direct, cwd, args) = self.run_as(os.path.join(self.appdir, "x.AppImage"))
        self.assertEqual((rc, root, direct, cwd, args), (0, self.a, "1", self.a, "a b"))

    def test_root_moved_with_it(self):
        moved = self.a + " moved"
        os.rename(self.a, moved)
        self.appdir = os.path.join(moved, "exports")
        rc, out = self.run_as(os.path.join(self.appdir, "x.AppImage"))
        self.assertEqual((rc, out[0]), (0, moved))

    def test_pointer_file_and_environment(self):
        with open(os.path.join(self.elsewhere, "cubicheroes-root.txt"), "w") as f:
            f.write("../root B\n")
        self.assertEqual(self.run_as(os.path.join(self.elsewhere, "x.AppImage"))[1][0], self.b)
        rc, out = self.run_as(os.path.join(self.tmp, "x.AppImage"), CUBIC_INSTALL_DIR=self.b)
        self.assertEqual(out[0], self.b)

    def test_copied_away_alone(self):
        self.assertEqual(self.run_as(os.path.join(self.elsewhere, "x.AppImage"))[1][0], self.a)
        shutil.rmtree(self.b)
        os.remove(os.path.join(self.a, "active.json"))   # its installation gone: refused, with how to point it
        rc, err = self.run_as(os.path.join(self.elsewhere, "x.AppImage"))
        self.assertEqual(rc, 3)
        self.assertIn("cubicheroes-root.txt", err)


if __name__ == "__main__":
    unittest.main()
