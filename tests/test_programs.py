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

    def add_program(self, pid=UPDATE, files=sp.PROGRAM_FILES + sp.PORT_FILES, info=None, folder="us-01.01"):
        """a program's folder, named by region and version and found by its program.json's id"""
        d = os.path.join(self.cfg, "programs", folder)
        os.makedirs(d, exist_ok=True)
        for n in files:
            if n in sp.PORT_FILES:
                with open(os.path.join(d, n), "w") as f:
                    f.write(f"/* {n} of {pid} */\n")
            else:
                shutil.copy2(os.path.join(self.cfg, n), d)
        with open(os.path.join(d, "program.json"), "w") as f:
            json.dump(dict(info or {}, id=pid), f)
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

    def test_found_by_its_id_never_by_a_title_id_folder(self):
        """config/programs/<title id>/ is kept out of the repository (tools/boundary_check.py): the folder is the one
        whose program.json names the program, whatever it is called"""
        self.add_program(folder=UPDATE)
        os.remove(os.path.join(self.cfg, "programs", UPDATE, "program.json"))
        self.assertIsNone(sp.program_dir(UPDATE))
        d = self.add_program(folder="any-name")
        self.assertEqual(sp.program_dir(UPDATE), d)
        self.assertEqual(sp.program_info(UPDATE)["dir"], d)
        self.add_program(pid="BLUS30490-09.99", folder="us-09.99")
        self.assertEqual(sp.program_dir(UPDATE), d)
        self.assertIsNone(sp.program_dir(sp.BASE_PROGRAM))

    def test_without_the_ports_files(self):
        self.add_program(files=[n for n in sp.PROGRAM_FILES + sp.PORT_FILES if n != "port_aliases.h"])
        with self.assertRaises(sp.SetupError) as c:
            sp.program_info(UPDATE)
        self.assertIn("port_aliases.h", c.exception.details)

    def test_never_v100s_files(self):
        """a file missing from its folder: refused, even though config/ has v1.00's"""
        self.add_program(files=[n for n in sp.PROGRAM_FILES + sp.PORT_FILES if n != "r8_hooks.json"])
        with self.assertRaises(sp.SetupError) as c:
            sp.program_info(UPDATE)
        self.assertEqual(c.exception.code, "program_unsupported")
        self.assertIn("r8_hooks.json", c.exception.details)
        self.assertEqual(sp.program_file("r8_hooks.json", UPDATE),
                         os.path.join(self.cfg, "programs", "us-01.01", "r8_hooks.json"))

    def test_its_own_configuration_and_lift(self):
        d = self.add_program(info={"defines": ['-DDOTHEROES_PROGRAM_V=101'], "spu_native": {"water": "spu_x"}})
        info = sp.program_info(UPDATE)
        self.assertEqual(info["spu_native"], {"water": "spu_x"})
        self.assertEqual(info["dir"], d)
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
        for info in ({"defines": ["-O3"]}, {"defines": [3]}, {"spu_native": ["x"]}):
            self.add_program(info=info)
            with self.assertRaises(sp.SetupError) as c:
                sp.program_info(UPDATE)
            self.assertEqual(c.exception.code, "program_unsupported", info)
        with open(os.path.join(self.cfg, "programs", "us-01.01", "program.json"), "w") as f:
            f.write("[]")                         # not a record: names no program
        with self.assertRaises(sp.SetupError) as c:
            sp.program_info(UPDATE)
        self.assertEqual(c.exception.code, "program_unsupported")

    def test_native_architecture_kept_in_its_lift(self):
        """the owner's one-architecture rule: the native renderer's entry hooks, the FFmpeg movie hooks, the audio
        hooks and the FMOD / Bink traps are all in another program's lift, from its own folder"""
        self.add_program()
        opts = sp.lift_options(UPDATE)
        pairs = list(zip(opts, opts[1:]))
        d = os.path.join(self.cfg, "programs", "us-01.01")
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

    def test_moved_images_found_by_their_contents(self):
        """v1.01 holds the water and waver images byte for byte, 0xD00 further on: found by their configurations'
        IMAGE_SHA256 and generated under their own names (gen_spu_native.py --image-name); a different image never"""
        import re
        images = os.path.join(self.dir, "gen", "spu", "images")
        os.makedirs(images)
        want = {}
        for task, moved in (("water", "spu_0000_at_00F48080"), ("waver", "spu_0001_at_00F49C80")):
            with open(os.path.join(ROOT, "tools", "spu_native", task + ".py"), encoding="utf-8") as f:
                want[moved] = re.search(r"^IMAGE_SHA256\s*=\s*'([0-9a-f]{64})'", f.read(), re.M).group(1)
            open(os.path.join(images, moved + ".elf"), "wb").close()
        open(os.path.join(images, "spu_0002_at_00F4B000.elf"), "wb").close()
        sha = lambda p, progress=None: want.get(os.path.basename(p)[:-4], "0" * 64)   # noqa: E731
        calls = []

        def tool(script, args, log, code="lift_failed"):
            calls.append(args)
            open(args[4], "w").close()
        self.add_program()
        with mock.patch.object(sp, "sha256_file", sha), mock.patch.object(sp, "run_tool", tool):
            sp.spu_native(mock.Mock(gen=os.path.join(self.dir, "gen")), lambda m: None, UPDATE)
            self.assertEqual(sp.native_image(images, "water", "spu_0000_at_00F47380"), "spu_0000_at_00F48080")
        self.assertEqual([a[a.index("--image-name") + 1] for a in calls], ["spu_0000_at_00F48080", "spu_0001_at_00F49C80"])
        self.assertTrue(calls[0][2].endswith("spu_0000_at_00F48080.elf"))
        with mock.patch.object(sp, "sha256_file", lambda p, progress=None: "1" * 64):   # no image is the task's
            self.assertEqual(sp.native_image(images, "water", "spu_0000_at_00F47380"), "spu_0000_at_00F47380")

    def test_update_program_with_them(self):
        self.add_program()
        self.assertEqual(self._run(UPDATE, ok=True), ["water_gen.cpp", "waver_gen.cpp"])


class Defines(unittest.TestCase):
    def _jobs(self, defines=(), program_dir=None):
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
                        for j in zig_build.game_jobs(src, gen, os.path.join(d, "obj"), "zig", "windows", defines,
                                                     program_dir)}

    def program_folder(self, where, aliases="/* aliases */"):
        d = os.path.join(where, "us-01.01")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "port_addrs.inc"), "w") as f:
            f.write("/* addrs */\n")
        with open(os.path.join(d, "port_aliases.h"), "w") as f:
            f.write(aliases + "\n")
        return d

    def test_program_folder_as_port_cmakelists_does(self):
        """port/CMakeLists.txt's DOTHEROES_PROGRAM: DOTHEROES_PROGRAM_ADDRS for the game's own sources, `-include
        port_aliases.h` for the port's own only -- never the lift's; objects named by the files' contents"""
        with tempfile.TemporaryDirectory() as t:
            a = self.program_folder(os.path.join(t, "a"))
            b = self.program_folder(os.path.join(t, "b"))       # the same files elsewhere (a launcher moved)
            c = self.program_folder(os.path.join(t, "c"), "/* other aliases */")
            pa = os.path.abspath(a).replace("\\", "/")
            ja, jb, jc, base = self._jobs((), a), self._jobs((), b), self._jobs((), c), self._jobs()
        for f in ("main.cpp", "cpu_place.cpp", "phys_hooks.cpp"):
            self.assertIn(f'-DDOTHEROES_PROGRAM_ADDRS="{pa}/port_addrs.inc"', ja[f][1])
            i = ja[f][1].index("-include")
            self.assertEqual(ja[f][1][i + 1], f"{pa}/port_aliases.h")
        for f in ("ppu_hle_nids.cpp", "ppu_loader.cpp"):            # the scaffold, the NID table: the define only
            self.assertIn(f'-DDOTHEROES_PROGRAM_ADDRS="{pa}/port_addrs.inc"', ja[f][1])
            self.assertNotIn("-include", ja[f][1])
        for f in ("ppu_recomp_0.cpp", "spu_recomp.c"):              # the lift: neither
            self.assertFalse([x for x in ja[f][1] if "PROGRAM" in x or x == "-include"])
            self.assertEqual(ja[f][0], base[f][0])
        self.assertEqual({f: o for f, (o, _) in ja.items()}, {f: o for f, (o, _) in jb.items()})
        self.assertNotEqual(ja["main.cpp"][0], jc["main.cpp"][0])  # other contents: other objects
        self.assertNotEqual(ja["main.cpp"][0], base["main.cpp"][0])

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
