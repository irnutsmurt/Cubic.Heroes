#!/usr/bin/env python3
"""The one authoritative settings file and the diagnostics report (M9: tools/settings.py, tools/diagnostics.py,
config/settings_schema.json):
    build/venv/Scripts/python.exe -m unittest tests.test_settings_diagnostics
Settings: every value checked against the schema, an invalid one reported and its default used, a line that is not
NAME=value left out, a damaged file reported and repaired (the damaged one kept), unknown keys kept, a change with
an invalid value refused whole, defaults restored. Diagnostics: the home and install folders, the user's and
computer's names and e-mail addresses replaced; saves, recipes and the disc's files never in a report; the zip is
what the reviewed folder holds."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import diagnostics  # noqa: E402
import install_layout as layout  # noqa: E402
import settings  # noqa: E402


AT = "@"   # e-mail-like test strings are put together here: no address in the repository's text
MAIL = "a.b" + AT + "example.com"


def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb" if isinstance(data, bytes) else "w", newline="\n" if isinstance(data, str) else None) as f:
        f.write(data)


class Settings(unittest.TestCase):
    def setUp(self):
        self.install = tempfile.mkdtemp(prefix="settings-test-")
        layout.write_json(os.path.join(self.install, "active.json"), {"layout": 2, "active": None, "previous": []})
        self.cfg = os.path.join(self.install, "config", "dotheroes.cfg")
        os.makedirs(os.path.dirname(self.cfg))

    def tearDown(self):
        shutil.rmtree(self.install, ignore_errors=True)

    def test_defaults_without_a_file(self):
        e = settings.effective(self.install)
        self.assertEqual(e["settings"]["DOTHEROES_FPS"]["value"], "60")
        self.assertEqual(e["settings"]["LAUNCHER_UPDATE_CHANNEL"]["value"], "stable")
        self.assertEqual(e["problems"], [])

    def test_invalid_values_and_lines(self):
        put(self.cfg, "# comment\nDOTHEROES_FPS=45\nPS3RECOMP_VK_FULLSCREEN=yes\nnot a setting\n"
                      "LAUNCHER_BUILDS_KEPT=9\nDOTHEROES_NATIVE_SOLVER=6,1,1,2\nMY_DEV_SWITCH=7\n")
        e = settings.effective(self.install)
        s = e["settings"]
        self.assertEqual((s["DOTHEROES_FPS"]["value"], s["DOTHEROES_FPS"]["source"][:7]), ("60", "default"))
        self.assertEqual(s["PS3RECOMP_VK_FULLSCREEN"]["value"], "1", "yes -> 1")
        self.assertEqual(s["LAUNCHER_BUILDS_KEPT"]["value"], "1", "9 is out of 0-5: the default")
        self.assertEqual(s["DOTHEROES_NATIVE_SOLVER"]["value"], "1,2,6", "the schema's order, no repeats")
        self.assertEqual(e["unknown"], {"MY_DEV_SWITCH": "7"})
        self.assertEqual(len(e["problems"]), 3)

    def test_set_checks_everything_first(self):
        put(self.cfg, "DOTHEROES_FPS=30\nMY_DEV_SWITCH=7\n")
        with self.assertRaises(settings.SettingsError):
            settings.set_values(self.install, {"PS3RECOMP_VK_FULLSCREEN": "1", "DOTHEROES_FPS": "45"})
        with open(self.cfg) as f:
            self.assertEqual(f.read(), "DOTHEROES_FPS=30\nMY_DEV_SWITCH=7\n", "nothing written")
        with self.assertRaises(settings.SettingsError) as e:
            settings.set_values(self.install, {"NO_SUCH": "1"})
        self.assertEqual(e.exception.code, "unknown_setting")
        e = settings.set_values(self.install, {"PS3RECOMP_VK_FULLSCREEN": "1", "LAUNCHER_UPDATE_CHANNEL": "prerelease"})
        self.assertEqual(e["settings"]["PS3RECOMP_VK_FULLSCREEN"]["value"], "1")
        self.assertEqual(e["settings"]["DOTHEROES_FPS"]["value"], "30", "the other values kept")
        self.assertEqual(e["unknown"], {"MY_DEV_SWITCH": "7"})

    def test_reset(self):
        put(self.cfg, "DOTHEROES_FPS=30\nPS3RECOMP_VK_FULLSCREEN=1\n")
        e = settings.reset(self.install, ["DOTHEROES_FPS"])
        self.assertEqual((e["settings"]["DOTHEROES_FPS"]["value"], e["settings"]["PS3RECOMP_VK_FULLSCREEN"]["value"]),
                         ("60", "1"))
        e = settings.reset(self.install)
        self.assertEqual(e["settings"]["PS3RECOMP_VK_FULLSCREEN"]["value"], "0")

    def test_damaged_file_repaired(self):
        put(self.cfg, b"DOTHEROES_FPS=30\n\x00\x01\x02garbage\nPS3RECOMP_VK_FULLSCREEN=1\n")
        e = settings.effective(self.install)
        self.assertFalse(e["readable"])
        with self.assertRaises(settings.SettingsError) as x:
            settings.set_values(self.install, {"DOTHEROES_FPS": "60"})
        self.assertEqual(x.exception.code, "damaged_settings")
        r = settings.repair(self.install)
        self.assertTrue(os.path.exists(r["repaired_from"]), "the damaged file kept")
        self.assertTrue(settings.effective(self.install)["readable"])
        self.assertEqual(r["settings"]["DOTHEROES_NATIVE_CD"]["value"], "1", "the performance switches back")

    def test_worker(self):
        w = [sys.executable, os.path.join(ROOT, "tools", "worker.py"), "settings", "--install", self.install]
        p = subprocess.run(w + ["--set", "DOTHEROES_FPS=30"], capture_output=True, text=True, timeout=60)
        res = json.loads(p.stdout.splitlines()[-1])
        self.assertEqual(res["data"]["settings"]["DOTHEROES_FPS"]["value"], "30")
        p = subprocess.run(w + ["--set", "DOTHEROES_FPS=31"], capture_output=True, text=True, timeout=60)
        self.assertEqual(json.loads(p.stdout.splitlines()[-1])["code"], "invalid_setting")


class Diagnostics(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="diag-test-")
        self.install = os.path.join(self.tmp, "install")
        self.lay = layout.Layout(self.install)
        self.lay.ensure_dirs()
        layout.write_json(self.lay.active_json, {"layout": 2, "active": None, "previous": []})
        home = os.path.expanduser("~")
        user = os.environ.get("USERNAME") or os.environ.get("USER") or "player"
        put(os.path.join(self.lay.logs, "game.log"),
            f"opened {home}\\Documents\\x.txt\nthe install {self.install}/data/disc\nuser {user} mail a.b@example.com\n")
        put(os.path.join(self.lay.gamedata, "dev_hdd0", "SAVE.BIN"), b"the hero's name")
        put(os.path.join(self.lay.cache, "native-recipes.bin"), b"program bytes")
        put(os.path.join(self.lay.disc, "PS3_GAME", "PARAM.SFO"), b"game")
        self.user, self.home = user, home

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_collect_sanitized_and_without_saves(self):
        out = os.path.join(self.tmp, "report")
        files = diagnostics.collect(self.install, out)
        names = [f["file"] for f in files]
        self.assertIn("about.json", names)
        self.assertIn("log-game.log", names)
        everything = ""
        for n in os.listdir(out):
            with open(os.path.join(out, n), encoding="utf-8") as f:
                everything += f.read()
        for secret in (self.home, self.install, MAIL, "the hero's name", "program bytes"):
            self.assertNotIn(secret.lower(), everything.lower(), secret)
        if len(self.user) >= 3:
            self.assertNotIn(f"user {self.user}", everything)
        self.assertIn("<home>", everything)
        self.assertIn("<install>", everything)
        self.assertIn("<email>", everything)
        with open(os.path.join(out, "about.json"), encoding="utf-8") as f:
            about = json.load(f)
        self.assertTrue(about["saves"]["present"])
        self.assertIsNotNone(about["sizes_mb"]["recipes_kb"])

    def test_pack_is_what_was_reviewed(self):
        out = os.path.join(self.tmp, "report")
        diagnostics.collect(self.install, out)
        os.remove(os.path.join(out, "log-game.log"))   # the player removed it
        z = diagnostics.pack(out, os.path.join(self.tmp, "report.zip"))
        with zipfile.ZipFile(z["zip"]) as zf:
            self.assertEqual(sorted(zf.namelist()), sorted(n for n in os.listdir(out)))
            self.assertNotIn("log-game.log", zf.namelist())

    def test_sanitizer(self):
        s = diagnostics.Sanitizer("C:\\Games\\Cubic", extra=("Alexandra",))
        t = s.text("C:\\Games\\Cubic\\logs and c:/games/cubic/data, by Alexandra (" + "alexandra" + AT + "x.org), not Alexandrite")
        self.assertEqual(t, "<install>\\logs and <install>/data, by <name> (<email>), not Alexandrite")


if __name__ == "__main__":
    unittest.main()
