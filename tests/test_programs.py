#!/usr/bin/env python3
"""The game's programs in the player's build path (beta.6: the disc's v1.00 and the official v1.01 update's;
tools/setup_pipeline.py, tools/zig_build.py). Nothing is compiled or run; no game file is read.

- the disc's program builds exactly as before programs existed: the same configuration paths, the same lift id;
- another program needs every address-tied file in its own folder (config/programs/<id>/), never v1.00's;
- its compile defines reach the port's objects only, and change their names;
- one native architecture (the owner, 2026-11-25): another program's build keeps every native replacement's
  configuration in its lift (the native renderer's entry hooks, FFmpeg movies, the FMOD / Bink traps) and is
  refused without its native SPU tasks -- never built with the stub and the lifted tasks.
    build/venv/Scripts/python.exe -m unittest tests.test_programs"""
import json
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import setup_pipeline as sp  # noqa: E402
import zig_build  # noqa: E402

UPDATE = "BLUS30490-01.01"
SHA = "ab" * 32


class Config(unittest.TestCase):
    """a copy of config/ (its JSON files) to add a program's folder to"""

    def setUp(self):
        os.makedirs(os.path.join(ROOT, "build", "tmp"), exist_ok=True)
        self.dir = tempfile.mkdtemp(prefix="programs-", dir=os.path.join(ROOT, "build", "tmp"))
        self.cfg = os.path.join(self.dir, "config")
        os.makedirs(self.cfg)
        for n in os.listdir(sp.config_dir()):
            if n.endswith(".json"):
                shutil.copy2(os.path.join(sp.config_dir(), n), self.cfg)
        p = mock.patch.object(sp, "config_dir", lambda: self.cfg)
        p.start()
        self.addCleanup(p.stop)

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def add_program(self, pid=UPDATE, files=sp.PROGRAM_FILES, info=None):
        d = os.path.join(self.cfg, "programs", pid)
        os.makedirs(d, exist_ok=True)
        for n in files:
            shutil.copy2(os.path.join(self.cfg, n), d)
        if info is not None:
            with open(os.path.join(d, "program.json"), "w") as f:
                json.dump(info, f)
        return d


class DiscProgram(Config):
    def test_unchanged(self):
        """None and the base id are the same program, configured from config/ itself, with the same fingerprints"""
        self.assertEqual(sp.lift_options(), sp.lift_options(sp.BASE_PROGRAM))
        for opt, f in sp.LIFT_CONFIG:
            self.assertIn(os.path.join(self.cfg, f), sp.lift_options())
        self.assertEqual(sp.tool_fingerprint("recomp", SHA), sp.tool_fingerprint("recomp", SHA, program=sp.BASE_PROGRAM))
        self.assertEqual(sp.lift_id(SHA), sp.lift_id(SHA, sp.BASE_PROGRAM))
        self.assertEqual(sp.program_info(), {})
        self.assertEqual(sp.program_file("spu_relocatable.json"), os.path.join(self.cfg, "spu_relocatable.json"))

    def test_another_program_folder_changes_nothing_for_it(self):
        before = sp.lift_id(SHA)
        self.add_program(info={"defines": ["-DX=1"]})
        self.assertEqual(sp.lift_id(SHA), before)
        self.assertEqual(sp.lift_options(), sp.lift_options(None))
        self.assertNotIn(os.sep + "programs" + os.sep, " ".join(sp.lift_options()))


class UpdateProgram(Config):
    def test_unsupported_without_its_folder(self):
        with self.assertRaises(sp.SetupError) as c:
            sp.program_info(UPDATE)
        self.assertEqual(c.exception.code, "program_unsupported")
        self.assertIn(UPDATE, str(c.exception))

    def test_never_v100s_files(self):
        """a file missing from its folder: refused, even though config/ has v1.00's"""
        self.add_program(files=[n for n in sp.PROGRAM_FILES if n != "r8_hooks.json"])
        with self.assertRaises(sp.SetupError) as c:
            sp.program_info(UPDATE)
        self.assertEqual(c.exception.code, "program_unsupported")
        self.assertIn("r8_hooks.json", c.exception.details)
        self.assertEqual(sp.program_file("r8_hooks.json", UPDATE),
                         os.path.join(self.cfg, "programs", UPDATE, "r8_hooks.json"))

    def test_its_own_configuration_and_lift(self):
        d = self.add_program(info={"defines": ['-DDOTHEROES_PROGRAM_V=101'], "spu_native": {"water": "spu_x"}})
        info = sp.program_info(UPDATE)
        self.assertEqual(info["spu_native"], {"water": "spu_x"})
        opts = sp.lift_options(UPDATE)
        for opt, f in sp.LIFT_CONFIG:
            self.assertIn(os.path.join(d, f), opts)
        self.assertNotIn(os.path.join(self.cfg, "r8_hooks.json"), opts)
        self.assertNotEqual(sp.lift_id(SHA, UPDATE), sp.lift_id(SHA))
        before = sp.lift_id(SHA, UPDATE)
        with open(os.path.join(d, "r8_hooks.json"), "a") as f:
            f.write("\n")
        self.assertNotEqual(sp.lift_id(SHA, UPDATE), before)   # its configuration is in its lift's fingerprint

    def test_bad_program_json(self):
        for info in ({"defines": ["-O3"]}, {"defines": [3]}, {"spu_native": ["x"]}, []):
            self.add_program(info=info)
            with self.assertRaises(sp.SetupError) as c:
                sp.program_info(UPDATE)
            self.assertEqual(c.exception.code, "program_unsupported", info)

    def test_native_architecture_kept_in_its_lift(self):
        """the owner's one-architecture rule: the native renderer's entry hooks, the FFmpeg movie hooks, the audio
        hooks and the FMOD / Bink traps are all in another program's lift, from its own folder"""
        self.add_program()
        opts = sp.lift_options(UPDATE)
        pairs = list(zip(opts, opts[1:]))
        d = os.path.join(self.cfg, "programs", UPDATE)
        for opt, f in [("--entry-observers", "r7_native_hooks.json"), ("--insn-patches", "m8_native_movie.json"),
                       ("--insn-patches", "audio_hooks.json"), ("--traps", "excluded_code.json"),
                       ("--insn-patches", "r8_hooks.json")]:
            self.assertIn((opt, os.path.join(d, f)), pairs)


class NativeSpuTasks(Config):
    def _run(self, program, ok):
        gen = os.path.join(self.dir, "gen")
        os.makedirs(os.path.join(gen, "spu"), exist_ok=True)
        paths = mock.Mock(gen=gen)

        def tool(script, args, log, code="lift_failed"):
            if not ok:
                raise sp.SetupError("not this image", "lift_failed")
            open(args[-1], "w").close()
        with mock.patch.object(sp, "run_tool", tool):
            sp.spu_native(paths, lambda m: None, program)
        return sorted(os.listdir(os.path.join(gen, zig_build.SPU_NATIVE_DIR))) if \
            os.path.isdir(os.path.join(gen, zig_build.SPU_NATIVE_DIR)) else []

    def test_disc_program_falls_back_as_before(self):
        self.assertEqual(self._run(None, ok=False), [])

    def test_update_program_refused_without_them(self):
        self.add_program()
        with self.assertRaises(sp.SetupError) as c:
            self._run(UPDATE, ok=False)
        self.assertEqual(c.exception.code, "program_unsupported")

    def test_update_program_with_them(self):
        self.add_program()
        self.assertEqual(self._run(UPDATE, ok=True), ["water_gen.cpp", "waver_gen.cpp"])


class Defines(unittest.TestCase):
    def _jobs(self, defines=()):
        with tempfile.TemporaryDirectory() as d:
            gen = os.path.join(d, "gen")
            os.makedirs(os.path.join(gen, "recompiled"))
            os.makedirs(os.path.join(gen, "spu", "spu_0000"))
            for p in (("recompiled", "ppu_recomp.h"), ("recompiled", "ppu_recomp_0.cpp"),
                      ("spu", "spu_0000", "spu_recomp.c")):
                with open(os.path.join(gen, *p), "w") as f:
                    f.write("/* test */\n")
            lib = os.path.join(d, "lib")
            os.makedirs(os.path.join(lib, "src"))
            with open(os.path.join(lib, "src", "ppu_hle_nids.cpp"), "w") as f:
                f.write("/* test */\n")
            src = zig_build.Sources("windows")
            src.lib_dir = lib
            src.nids = os.path.join(lib, "src", "ppu_hle_nids.cpp")
            with mock.patch.object(zig_build, "_zig_version", lambda z: "zig test"), \
                    mock.patch.object(zig_build, "host_cpu", lambda z=None: "x86_64_v3"), \
                    mock.patch.object(zig_build, "headers_fingerprint", lambda s, g: b"h"):
                return {os.path.basename(j[0]): (os.path.basename(j[1]), j[3])
                        for j in zig_build.game_jobs(src, gen, os.path.join(d, "obj"), "zig", "windows", defines)}

    def test_port_only(self):
        base, prog = self._jobs(), self._jobs(('-DDOTHEROES_PROGRAM_ID="BLUS30490-01.01"',))
        self.assertEqual(base.keys(), prog.keys())
        for f in ("main.cpp", "cpu_place.cpp", "ppu_hle_nids.cpp"):
            self.assertIn('-DDOTHEROES_PROGRAM_ID="BLUS30490-01.01"', prog[f][1])
            self.assertNotEqual(base[f][0], prog[f][0], f)          # another object: never v1.00's reused
        for f in ("ppu_recomp_0.cpp", "spu_recomp.c"):
            self.assertNotIn('-DDOTHEROES_PROGRAM_ID="BLUS30490-01.01"', prog[f][1])
            self.assertEqual(base[f][0], prog[f][0], f)             # the lift's own objects: per lift already
        self.assertEqual({f: o for f, (o, _) in base.items()},     # no defines: exactly today's objects
                         {f: o for f, (o, _) in self._jobs(()).items()})


if __name__ == "__main__":
    unittest.main()
