"""The keys downloaded when the player's computer has none (beta.6: tools/key_download.py, tools/key_sources.py).
Made-up keys and programs only (tests/test_pkg_update.py's), served from a folder of the test -- never the internet,
never a real key. The shipped list of sources (config/key_downloads.json) is checked for its form and pins only.

    py -3 -m unittest tests.test_key_download
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
import key_download as KD  # noqa: E402
import key_sources  # noqa: E402
from test_pkg_update import make_elf, make_self, rnd  # noqa: E402

TMP = os.path.join(ROOT, "build", "tmp")


class Base(unittest.TestCase):
    def setUp(self):
        os.makedirs(TMP, exist_ok=True)
        self.d = tempfile.mkdtemp(prefix="keydl-", dir=TMP)
        self.addCleanup(shutil.rmtree, self.d, True)
        self.mirror = os.path.join(self.d, "mirror")
        self.install = os.path.join(self.d, "install")
        os.makedirs(self.install)
        self.erk, self.riv = rnd(32), rnd(16)

    def source(self, sid, files, opens=("app", "pkg")):
        """a made-up source: files {path: bytes} served from the mirror, pinned"""
        for p, b in files.items():
            dst = os.path.join(self.mirror, sid, *p.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(b)
        return {"id": sid, "name": f"source {sid}", "commit": "1" * 40, "url": "https://example.invalid/{commit}/{path}",
                "opens": list(opens), "files": [{"path": p, "size": len(b), "sha256": hashlib.sha256(b).hexdigest()}
                                                for p, b in files.items()]}

    def app_files(self):
        """codyps/ps3keys' form: one binary file per key"""
        return {"app-key-315": self.erk, "app-iv-315": self.riv, "gpkg-key": rnd(16)}


class Shipped(unittest.TestCase):
    def test_the_list_is_well_formed_and_pinned(self):
        srcs = KD.load()
        self.assertEqual([s["id"] for s in srcs], ["codyps-ps3keys", "webman-mod-keys"])   # the owner's source first
        for s in srcs:
            self.assertRegex(s["commit"], r"^[0-9a-f]{40}$")
            self.assertTrue(s["url"].startswith("https://raw.githubusercontent.com/"))
            self.assertIn("{commit}", s["url"])
            for f in s["files"]:
                self.assertRegex(f["sha256"], r"^[0-9a-f]{64}$")
                self.assertGreater(f["size"], 0)
        self.assertIn("app", srcs[0]["opens"])
        self.assertIn("npdrm", srcs[1]["opens"])
        self.assertNotIn("npdrm", srcs[0]["opens"])   # codyps/ps3keys has no NPDRM key (tested 2026-11-25)

    def test_no_key_in_the_list(self):
        """pins only: no value of the size of a key (32 or 64 hex digits) other than the commits and SHA-256s"""
        text = open(KD.CONFIG, encoding="utf-8").read()
        cfg = json.loads(text)
        allowed = {s["commit"] for s in cfg["sources"]} | {f["sha256"] for s in cfg["sources"] for f in s["files"]}
        import re
        for h in re.findall(r"[0-9A-Fa-f]{32,}", text):
            self.assertIn(h.lower(), allowed)

    def test_damaged_lists_are_refused(self):
        d = tempfile.mkdtemp(dir=TMP)
        self.addCleanup(shutil.rmtree, d, True)
        good = {"id": "a", "commit": "1" * 40, "url": "https://x/{commit}/{path}", "opens": ["app"],
                "files": [{"path": "k", "size": 16, "sha256": "2" * 64}]}
        for bad in ({"url": "http://x/{path}"}, {"commit": "abc"}, {"opens": ["other"]}, {"files": []},
                    {"files": [{"path": "../k", "size": 16, "sha256": "2" * 64}]}, {"id": "../x"},
                    {"files": [{"path": "k", "size": 16, "sha256": "zz"}]}):
            p = os.path.join(d, "k.json")
            with open(p, "w") as f:
                json.dump({"format": "cubic.heroes.key-downloads", "sources": [dict(good, **bad)]}, f)
            with self.assertRaises(KD.KeyDownloadError, msg=bad) as c:
                KD.load(p)
            self.assertEqual(c.exception.code, "config")


class Fetch(Base):
    def test_fetched_once_then_kept(self):
        src = self.source("s1", self.app_files())
        got = KD.fetch(self.install, "app", [src], base=self.mirror)
        d = os.path.join(self.install, "data", "keys", "s1")
        self.assertEqual(got, [(d, "downloaded (source s1)")])
        self.assertTrue(KD.complete(self.install, src))
        shutil.rmtree(self.mirror)   # offline now: what was downloaded is used
        self.assertEqual(KD.fetch(self.install, "app", [src], base=self.mirror), [(d, "downloaded before (source s1)")])
        self.assertEqual(KD.downloaded(self.install, "app", [src]), [(d, "downloaded before (source s1)")])
        self.assertEqual(KD.downloaded(self.install, "npdrm", [src]), [])

    def test_a_file_that_isnt_pinned_is_removed_and_the_next_source_used(self):
        bad = self.source("bad", self.app_files())
        bad["files"][0]["sha256"] = "0" * 64      # the server's file isn't the pinned one
        good = self.source("good", self.app_files())
        got = KD.fetch(self.install, "app", [bad, good], base=self.mirror)
        self.assertEqual(got[0][0], os.path.join(self.install, "data", "keys", "good"))
        self.assertFalse(os.path.exists(os.path.join(self.install, "data", "keys", "bad", "app-key-315")))

    def test_nothing_reachable(self):
        src = self.source("s1", self.app_files())
        shutil.rmtree(self.mirror)
        with mock.patch.object(KD, "_for", lambda kind, s: s):
            with self.assertRaises(KD.KeyDownloadError) as c:
                KD.fetch(self.install, "app", [dict(src, url="https://127.0.0.1:9/{commit}/{path}")])
        self.assertIn(c.exception.code, ("offline", "server", "tls"))
        with self.assertRaises(KD.KeyDownloadError) as c:
            KD.fetch(self.install, "npdrm", [src], base=self.mirror)
        self.assertEqual(c.exception.code, "no_source")

    def test_cancel(self):
        src = self.source("s1", {"k": rnd(1000)})
        with self.assertRaises(KD.KeyDownloadError) as c:
            KD.fetch(self.install, "app", [src], base=self.mirror, cancelled=lambda: True)
        self.assertEqual(c.exception.code, "cancelled")


class Discovery(Base):
    """tools/key_sources.py: the player's keys first; then the keys downloaded before; then a download; RPCS3 last"""

    def setUp(self):
        super().setUp()
        self.elf = make_elf([rnd(5000)])
        self.eboot = make_self(self.elf, self.erk, self.riv, keyrev=1)
        self.want = hashlib.sha256(self.elf).hexdigest()
        self.src = self.source("s1", self.app_files())
        real = KD.fetch
        for p in (mock.patch.object(KD, "load", lambda *a, **k: [self.src]),
                  mock.patch.object(KD, "fetch", lambda install, kind, sources=None, progress=None, cancelled=None,
                                    base=None: real(install, kind, [self.src], progress, cancelled, base=self.mirror))):
            p.start()
            self.addCleanup(p.stop)

    def test_downloaded_when_none_on_the_computer(self):
        elf, how = key_sources.open_program(self.eboot, self.want, key_sources.Sources(install=self.install,
                                                                                       download=True))
        self.assertEqual(elf, self.elf)
        self.assertEqual(how["kind"], "downloaded")
        self.assertIn("downloaded", how["how"])

    def test_not_downloaded_when_not_allowed(self):
        with self.assertRaises(key_sources.NoProgram):
            key_sources.open_program(self.eboot, self.want, key_sources.Sources(install=self.install))
        self.assertFalse(os.path.exists(os.path.join(self.install, "data", "keys")))

    def test_the_players_own_first(self):
        mine = os.path.join(self.d, "mykeys.txt")
        with open(mine, "w") as f:
            f.write(f"APP 1 {self.erk.hex()} {self.riv.hex()}\n")
        elf, how = key_sources.open_program(self.eboot, self.want, key_sources.Sources(chosen=mine,
                                                                                       install=self.install,
                                                                                       download=True))
        self.assertEqual((how["kind"], how["how"]), ("text", "chosen"))
        self.assertFalse(os.path.exists(os.path.join(self.install, "data", "keys")))

    def test_download_failure_is_said_plainly(self):
        shutil.rmtree(self.mirror)
        import setup_pipeline
        with self.assertRaises(setup_pipeline.SetupError) as c:
            setup_pipeline.open_eboot(self.eboot, key_sources.Sources(install=self.install, download=True),
                                      want=self.want)
        self.assertEqual(c.exception.code, "keys_download")
        self.assertIn("Connect to the internet", str(c.exception))
        self.assertNotIn(self.erk.hex(), str(c.exception).lower() + (c.exception.details or "").lower())

    def test_kind_of_program(self):
        self.assertEqual(key_sources.program_kind(self.eboot), "app")
        np = make_self(self.elf, self.erk, self.riv, keyrev=1, npdrm=(rnd(16), rnd(16)))
        self.assertEqual(key_sources.program_kind(np), "npdrm")
        self.assertEqual(key_sources.program_kind(b"junk" * 100), "app")


if __name__ == "__main__":
    unittest.main()
