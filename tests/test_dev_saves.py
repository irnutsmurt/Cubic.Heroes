#!/usr/bin/env python3
"""tools/dev_saves.py: developer mode's test saves (docs/M11_PLAN.md section 3.2), on a made-up install: the player's
saves copied into the test saves and never changed; test states kept, restored, deleted; everything refused while the
game runs; bad names refused."""
import hashlib
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import dev_saves  # noqa: E402
import install_layout as layout  # noqa: E402

NOT_RUNNING = lambda root: []          # noqa: E731
RUNNING = lambda root: [root + "/builds/b1/dotheroes.exe"]   # noqa: E731


def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def digest(tree):
    h = hashlib.sha256()
    for dp, dn, fn in sorted(os.walk(tree)):
        for f in sorted(fn):
            p = os.path.join(dp, f)
            h.update(os.path.relpath(p, tree).encode())
            with open(p, "rb") as x:
                h.update(x.read())
    return h.hexdigest()


class DevSaves(unittest.TestCase):
    def setUp(self):
        self.tmpd = tempfile.TemporaryDirectory()
        self.lay = layout.Layout(os.path.join(self.tmpd.name, "inst"))
        self.lay.ensure_dirs()
        sd = os.path.join(self.lay.gamedata, "dev_hdd0", "home", "00000001", "savedata")
        put(os.path.join(sd, "GAME-A", "DATA.SAV"), b"player save A" * 50)
        put(os.path.join(sd, "SYSTEM", "SYS.DAT"), b"system")
        put(os.path.join(self.lay.gamedata, "trophies", "t.json"), b"{}")
        self.p = dev_saves.paths(self.lay)

    def tearDown(self):
        self.tmpd.cleanup()

    def test_copy_player_reads_only(self):
        before = digest(self.lay.gamedata)
        r = dev_saves.copy_player(self.lay, NOT_RUNNING)
        self.assertEqual(r["files"], 3)
        self.assertEqual(digest(self.p["tests"]), before)
        # a test session writes its own saves; the player's stay as they were
        put(os.path.join(self.p["tests"], "dev_hdd0", "home", "00000001", "savedata", "GAME-A", "DATA.SAV"), b"test")
        self.assertEqual(digest(self.lay.gamedata), before)
        # copied again: the test tree replaced, the player's untouched
        dev_saves.copy_player(self.lay, NOT_RUNNING)
        self.assertEqual(digest(self.p["tests"]), before)
        self.assertEqual(digest(self.lay.gamedata), before)
        self.assertFalse(os.path.exists(self.p["tests"] + ".incoming"))
        self.assertFalse(os.path.exists(self.p["tests"] + ".replaced"))

    def test_states_keep_restore_delete(self):
        dev_saves.copy_player(self.lay, NOT_RUNNING)
        kept = digest(self.p["tests"])
        dev_saves.keep(self.lay, "boss-3", running=NOT_RUNNING)
        put(os.path.join(self.p["tests"], "later.bin"), b"progress")
        self.assertNotEqual(digest(self.p["tests"]), kept)
        with self.assertRaises(dev_saves.DevSavesError) as e:
            dev_saves.keep(self.lay, "boss-3", running=NOT_RUNNING)
        self.assertEqual(e.exception.code, "exists")
        dev_saves.restore(self.lay, "boss-3", running=NOT_RUNNING)
        self.assertEqual(digest(self.p["tests"]), kept)
        self.assertEqual([s["name"] for s in dev_saves.states(self.lay)["states"]], ["boss-3"])
        dev_saves.delete(self.lay, "boss-3")
        self.assertEqual(dev_saves.states(self.lay)["states"], [])

    def test_refused_while_the_game_runs(self):
        for f in (lambda: dev_saves.copy_player(self.lay, RUNNING),
                  lambda: dev_saves.keep(self.lay, "x", running=RUNNING)):
            with self.assertRaises(dev_saves.DevSavesError) as e:
                f()
            self.assertEqual(e.exception.code, "game_running")
        self.assertFalse(os.path.exists(self.p["tests"]))

    def test_bad_names_refused(self):
        for n in ("", "../x", "a/b", "x" * 41, "-lead"):
            with self.assertRaises(dev_saves.DevSavesError):
                dev_saves.keep(self.lay, n, running=NOT_RUNNING)
        with self.assertRaises(dev_saves.DevSavesError) as e:
            dev_saves.restore(self.lay, "none", running=NOT_RUNNING)
        self.assertEqual(e.exception.code, "no_state")

    def test_no_player_saves_yet(self):
        layout.rmtree(self.lay.gamedata)
        r = dev_saves.copy_player(self.lay, NOT_RUNNING)
        self.assertEqual(r["files"], 0)
        self.assertTrue(os.path.isdir(self.p["tests"]))


if __name__ == "__main__":
    unittest.main()
