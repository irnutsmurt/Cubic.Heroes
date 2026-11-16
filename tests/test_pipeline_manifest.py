#!/usr/bin/env python3
"""The first-session pipeline manifest (tools/pipeline_manifest.py; docs/M9_LAUNCHER_PLAN.md section 15):
    build/venv/Scripts/python.exe -m unittest tests.test_pipeline_manifest
On made-up discs and recipes in temporary folders (programs are random bytes): a manifest made from recipes names
each program's place, transform and hash and none of its bytes; resolving it against the same disc gives the
recorded recipes back (a fragment program as the disc holds it: its constants as stored); a disc whose bytes differ
adds nothing; a second resolve adds nothing; recipes already held are kept and a damaged tail is cut; the worker
adds the manifest's recipes to an install's before it prepares."""
import os
import random
import struct
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import install_layout as layout  # noqa: E402
import pipeline_manifest as pm  # noqa: E402
import worker  # noqa: E402

rng = random.Random(9)


def blob(n):
    return bytes(rng.randrange(256) for _ in range(n))


def head(key, vl, fl):
    body = blob(pm.HEAD - 24)   # PipeState and ProgFixed: numbers
    return struct.pack("<IIQ", pm.MAGIC, pm.FORMAT, key) + body + struct.pack("<II", vl, fl)


def record(h, vp, fp):
    return h + vp + fp + struct.pack("<I", pm.recipe_sum(h, vp, fp))


class Fixture:
    """a disc with a shader package (a vertex program word-reversed, a fragment program as stored) and an EBOOT
    (another pair), and the recipes the game would record (the first fragment program's constant block patched)"""

    def __init__(self, tmp):
        self.disc = os.path.join(tmp, "disc")
        usr = os.path.join(self.disc, "PS3_GAME", "USRDIR")
        os.makedirs(os.path.join(usr, "data", "shaders"))
        self.vp1, self.fp1, self.vp2, self.fp2 = blob(64), blob(96), blob(48), blob(32)
        self.pkg = blob(100) + pm.wswap(self.vp1) + blob(37) + self.fp1 + blob(50)
        self.elf = b"\x7fELF" + blob(200) + self.vp2 + blob(11) + self.fp2 + blob(20)
        with open(os.path.join(usr, "data", "shaders", "shaders.ps3.pkg"), "wb") as f:
            f.write(self.pkg)
        with open(os.path.join(usr, "EBOOT.elf"), "wb") as f:
            f.write(self.elf)
        self.fp1_patched = self.fp1[:32] + blob(16) + self.fp1[48:]   # one 16-byte constant changed at load
        self.h1, self.h2 = head(0x1111, len(self.vp1), len(self.fp1)), head(0x2222, len(self.vp2), len(self.fp2))
        self.recorded = os.path.join(tmp, "recorded.bin")
        with open(self.recorded, "wb") as f:
            f.write(record(self.h1, self.vp1, self.fp1_patched) + record(self.h2, self.vp2, self.fp2))
        self.manifest = os.path.join(tmp, "manifest.txt")


class ManifestTest(unittest.TestCase):
    def setUp(self):
        self.tmpd = tempfile.TemporaryDirectory()
        self.tmp = self.tmpd.name
        self.f = Fixture(self.tmp)

    def tearDown(self):
        self.tmpd.cleanup()

    def test_make_names_places_not_bytes(self):
        res = pm.make(self.f.disc, [self.f.recorded], self.f.manifest)
        self.assertEqual(res, {"pipelines": 2, "left_out": 0})
        with open(self.f.manifest, encoding="ascii") as f:
            text = f.read()
        for prog in (self.f.vp1, self.f.fp1, self.f.vp2, self.f.fp2, pm.wswap(self.f.vp1)):
            self.assertNotIn(prog.hex(), text)
        entries = {k: (v, fp) for k, _, v, fp in pm.parse(self.f.manifest)}
        self.assertEqual(entries[0x1111][0][:3], ("data/shaders/shaders.ps3.pkg", 100, 1))
        self.assertEqual(entries[0x1111][1][:3], ("data/shaders/shaders.ps3.pkg", 100 + 64 + 37, 0))
        self.assertEqual(entries[0x2222][0][:3], ("EBOOT.elf", 204, 0))

    def test_resolve_gives_the_recipes_back(self):
        pm.make(self.f.disc, [self.f.recorded], self.f.manifest)
        out = os.path.join(self.tmp, "cache", "native-recipes.bin")
        self.assertEqual(pm.resolve(self.f.manifest, self.f.disc, out),
                         {"listed": 2, "held": 0, "added": 2, "not_on_disc": 0})
        got = {k: (h, v, fp) for k, h, v, fp in pm.read_recipes(out)}
        self.assertEqual(got[0x1111], (self.f.h1, self.f.vp1, self.f.fp1))   # the stored constants
        self.assertEqual(got[0x2222], (self.f.h2, self.f.vp2, self.f.fp2))
        self.assertEqual(pm.resolve(self.f.manifest, self.f.disc, out)["added"], 0)
        self.assertEqual(len(pm.read_recipes(out)), 2)

    def test_a_different_disc_adds_nothing(self):
        pm.make(self.f.disc, [self.f.recorded], self.f.manifest)
        pkg = os.path.join(self.f.disc, "PS3_GAME", "USRDIR", "data", "shaders", "shaders.ps3.pkg")
        b = bytearray(self.f.pkg)
        b[110] ^= 0xFF   # inside the vertex program
        with open(pkg, "wb") as f:
            f.write(b)
        out = os.path.join(self.tmp, "r.bin")
        self.assertEqual(pm.resolve(self.f.manifest, self.f.disc, out),
                         {"listed": 2, "held": 0, "added": 1, "not_on_disc": 1})
        self.assertEqual([k for k, _, _, _ in pm.read_recipes(out)], [0x2222])

    def test_held_recipes_kept_and_a_damaged_tail_cut(self):
        pm.make(self.f.disc, [self.f.recorded], self.f.manifest)
        out = os.path.join(self.tmp, "r.bin")
        own = record(head(0x3333, 16, 16), blob(16), blob(16))
        with open(out, "wb") as f:
            f.write(own + record(self.f.h2, self.f.vp2, self.f.fp2) + b"cut short")
        res = pm.resolve(self.f.manifest, self.f.disc, out)
        self.assertEqual(res, {"listed": 2, "held": 1, "added": 1, "not_on_disc": 0})
        self.assertEqual([k for k, _, _, _ in pm.read_recipes(out)], [0x3333, 0x2222, 0x1111])
        self.assertEqual(os.path.getsize(out), len(own) + sum(
            pm.HEAD + len(v) + len(fp) + 4 for v, fp in ((self.f.vp2, self.f.fp2), (self.f.vp1, self.f.fp1))))

    def test_a_program_not_on_the_disc_is_left_out(self):
        with open(self.f.recorded, "ab") as f:
            f.write(record(head(0x4444, 32, 32), blob(32), blob(32)))
        self.assertEqual(pm.make(self.f.disc, [self.f.recorded], self.f.manifest), {"pipelines": 2, "left_out": 1})

    def test_a_line_of_another_format_is_refused(self):
        with open(self.f.manifest, "w") as f:
            f.write("# x\n00 a 1 0 ff b 2 0 ee\n")
        with self.assertRaises(ValueError):
            pm.parse(self.f.manifest)

    def test_the_worker_adds_them_to_an_install(self):
        pm.make(self.f.disc, [self.f.recorded], self.f.manifest)
        lay = layout.Layout(os.path.join(self.tmp, "install"))
        os.makedirs(os.path.dirname(lay.disc))
        os.rename(self.f.disc, lay.disc)
        logs = []

        class Prog:
            def log(self, m):
                logs.append(m)
        old = worker.MANIFEST
        worker.MANIFEST = self.f.manifest
        try:
            self.assertEqual(worker.first_session_recipes(lay, Prog())["added"], 2)
            self.assertEqual(len(pm.read_recipes(os.path.join(lay.cache, "native-recipes.bin"))), 2)
            worker.MANIFEST = os.path.join(self.tmp, "none.txt")
            self.assertIsNone(worker.first_session_recipes(lay, Prog()))
        finally:
            worker.MANIFEST = old
        self.assertEqual(logs, [])


class Prog:
    def __init__(self):
        self.logs, self.fracs = [], []

    def log(self, m):
        self.logs.append(m)

    def progress(self, step, frac, message):
        self.fracs.append(frac)

    def skip(self, step):
        pass


class PrepareTest(unittest.TestCase):
    """the worker's preparation (the launcher's background job): in parts, one at a time per install, resumable"""

    def setUp(self):
        self.tmpd = tempfile.TemporaryDirectory()
        self.install = os.path.join(self.tmpd.name, "install")
        self.lay = layout.Layout(self.install)
        os.makedirs(self.lay.cache)
        with open(self.lay.active_json, "w") as f:
            f.write('{"layout": 2, "active": "b1", "previous": [], "retention": 1}')
        self.recipes = os.path.join(self.lay.cache, "native-recipes.bin")
        with open(self.recipes, "wb") as f:
            for k in range(70):
                f.write(record(head(1000 + k, 16, 16), blob(16), blob(16)))
        self.calls = []
        self.old = worker._run_prepare, worker.MANIFEST
        worker.MANIFEST = os.path.join(self.tmpd.name, "no-manifest.txt")

        def fake(lay, prog, extra, recipes, at):   # the game's --prepare: the part's recipes made
            n = len(pm.read_recipes(recipes)) if recipes else 0
            self.calls.append((extra, n, at, os.path.exists(os.path.join(lay.cache, "prepare.lock"))))
            if self.cancel_at is not None and len(self.calls) == self.cancel_at:
                raise worker.pipeline.Cancelled()
            return {"recipes": n, "made": n, "failed": 0, "translated": n, "from_cache": 0, "ms": 1.0,
                    "cache": "none yet", "cache_mb": 0.1, "gpu": {}, "exit": 0}
        self.cancel_at = None
        worker._run_prepare = fake

    def tearDown(self):
        worker._run_prepare, worker.MANIFEST = self.old
        self.tmpd.cleanup()

    def test_in_parts_under_the_lock(self):
        out = worker.op_prepare(self.install, Prog())
        self.assertEqual([c[1] for c in self.calls], [32, 32, 6])
        self.assertEqual([c[2] for c in self.calls], [(0, 70), (32, 70), (64, 70)])
        self.assertTrue(all(c[3] for c in self.calls))   # each part under the lock
        self.assertEqual((out["recipes"], out["made"], out["parts"]), (70, 70, 3))
        self.assertFalse(os.path.exists(os.path.join(self.lay.cache, "prepare.lock")))
        self.assertFalse(os.path.exists(os.path.join(self.lay.cache, "prepare-part.bin")))
        self.assertIn("disk_mb", out)

    def test_stopped_part_way_leaves_nothing_behind(self):
        self.cancel_at = 2
        with self.assertRaises(worker.pipeline.Cancelled):
            worker.op_prepare(self.install, Prog())
        self.assertEqual(len(self.calls), 2)
        self.assertFalse(os.path.exists(os.path.join(self.lay.cache, "prepare.lock")))
        self.assertFalse(os.path.exists(os.path.join(self.lay.cache, "prepare-part.bin")))
        self.assertEqual(len(pm.read_recipes(self.recipes)), 70)   # the recipes untouched: the next run resumes

    def test_one_at_a_time(self):
        lock = os.path.join(self.lay.cache, "prepare.lock")
        with open(lock, "w") as f:
            f.write(str(os.getppid()))   # a live process holds it
        with self.assertRaises(worker.pipeline.SetupError) as e:
            worker.op_prepare(self.install, Prog())
        self.assertEqual(e.exception.code, "already_preparing")
        self.assertEqual(self.calls, [])
        with open(lock, "w") as f:
            f.write("999999")   # its process is gone: taken over
        worker.op_prepare(self.install, Prog())
        self.assertEqual(len(self.calls), 3)

    def test_comprehensive_in_one_run_and_its_figures_kept(self):
        """M9.2b/c: with the profiles shipped and the install's disc copy, one --prepare --static run (its own
        progress), the figures kept beside the cache, and the check saying it is done"""
        os.makedirs(os.path.join(self.lay.disc, "PS3_GAME", "USRDIR"))
        profiles = os.path.join(self.tmpd.name, "profiles.txt")
        with open(profiles, "w") as f:
            f.write("# none\n")
        old_profiles, worker.PROFILES = worker.PROFILES, profiles

        def fake(lay, prog, extra, recipes, at):
            self.calls.append((extra, recipes, at))
            if extra == ["--check"]:
                return {"check": True, "recipes": 70, "cache": "reused", "prepared": True, "gpu": {}, "exit": 0}
            return {"recipes": 70, "made": 70, "failed": 0, "translated": 300, "from_cache": 0, "ms": 9.0,
                    "package": {"records": 569, "pipelines": 1785, "made": 1785, "failed": 0},
                    "shaders": {"translations": 2726, "spirv_failed": 0, "text_failed": 0, "disc_failed": 0},
                    "cache": "none yet", "cache_mb": 20.0, "gpu": {}, "exit": 0}
        worker._run_prepare = fake
        try:
            out = worker.op_prepare(self.install, Prog())
            self.assertEqual(self.calls, [(["--static", profiles], None, (0, 0))])
            self.assertEqual((out["pipelines"], out["translations"], out["failed_all"]), (1855, 2726, 0))
            self.assertEqual((out["recorded"], out["from_package"], out["package_records"]), (70, 1785, 569))
            chk = worker.op_prepare(self.install, Prog(), check=True)
            self.assertTrue(chk["comprehensive"])
            self.assertTrue(chk["comprehensive_done"])
            self.assertEqual(chk["coverage"]["pipelines"], 1855)
            # a rebuild clears the folder: the check says it is not done until the preparation ends
            sf = worker.shader_folder(self.lay)
            os.remove(os.path.join(sf, worker.PREPARED))
            self.assertFalse(worker.op_prepare(self.install, Prog(), check=True)["comprehensive_done"])
        finally:
            worker.PROFILES = old_profiles

    def test_check_counts_the_manifest_without_writing(self):
        f = Fixture(self.tmpd.name)
        pm.make(f.disc, [f.recorded], f.manifest)
        worker.MANIFEST = f.manifest
        out = worker.op_prepare(self.install, Prog(), check=True)
        self.assertEqual(out["pending"], 2)
        self.assertEqual(len(pm.read_recipes(self.recipes)), 70)
        self.assertEqual(self.calls[0][0], ["--check"])


if __name__ == "__main__":
    unittest.main()
