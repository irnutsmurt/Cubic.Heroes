#!/usr/bin/env python3
"""Static checks of the player's build path (tools/setup_pipeline.py, tools/zig_build.py)
against the native release the developers build and test (tools/r8_lift.sh,
tools/r8_release_build.sh, port/CMakeLists.txt). Nothing is compiled or run:
    build/venv/Scripts/python.exe tests/test_player_pipeline.py
docs/NATIVE_R8.md section 7.2 has what these guard against (the player path
built the development configuration; its source list lacked cpu_place.cpp)."""
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import setup_pipeline  # noqa: E402
import zig_build  # noqa: E402

RELEASE_DEFINES = {"DOTHEROES_NATIVE_RELEASE", "PPU_BARE_LOADS", "PPU_STORE_MOV", "PS3RECOMP_RSX_COMMAND_PATH",
                   "PS3RECOMP_RSX_VULKAN", "PS3RECOMP_SPU_INTERPRETER"}


def read(*p):
    with open(os.path.join(ROOT, *p), encoding="utf-8") as f:
        return f.read()


def cmake_exe_sources():
    """port/CMakeLists.txt: the port's own sources of the game executable (add_executable
    and target_sources of ${PROJECT_NAME}), less variables and resources"""
    text = read("port", "CMakeLists.txt")
    out = []
    for m in re.finditer(r"(add_executable\(\$\{PROJECT_NAME\}|target_sources\(\$\{PROJECT_NAME\} PRIVATE)([^)]*)\)",
                         text):
        out += [w for w in m.group(2).split() if w.endswith((".cpp", ".c")) and not w.startswith(("${", '"'))]
    return out


class LiftOptions(unittest.TestCase):
    def test_matches_r8_lift(self):
        """every configuration file and flag of the developers' release lift is in the player's"""
        sh = read("tools", "r8_lift.sh")
        dev = re.findall(r"(--sjlj|--insn-patches|--entry-observers|--traps) config/(\S+)", sh)
        player = setup_pipeline.LIFT_CONFIG
        self.assertEqual(sorted(dev), sorted(player))
        self.assertIn("--chunks 48", sh)
        self.assertEqual(setup_pipeline.PPU_CHUNKS, "48")
        default = re.search(r"FLAGS=\$\{R8_LIFT_FLAGS-([^}]*)\}", sh).group(1).split()
        for f in default:
            self.assertIn(f, setup_pipeline.LIFT_FLAGS)

    def test_config_files_ship(self):
        """the lift's configuration files exist and are committed (make_bundle copies config/)"""
        tracked = subprocess.run(["git", "ls-files", "config"], cwd=ROOT, capture_output=True, text=True).stdout.split()
        for _, f in setup_pipeline.LIFT_CONFIG:
            self.assertIn("config/" + f, tracked)

    def test_lift_options_paths(self):
        opts = setup_pipeline.lift_options()
        self.assertIn("--promote-leaf", opts)
        i = opts.index("--entry-observers")
        self.assertTrue(opts[i + 1].endswith("r7_native_hooks.json"))
        self.assertEqual(sum(1 for o in opts if o == "--insn-patches"), 3)


class PortSources(unittest.TestCase):
    def test_port_sources_match_cmake(self):
        """zig_build.PORT_SOURCES is the executable's own source list (cpu_place.cpp was missing from
        2026-11-03 to 2026-11-07)"""
        self.assertEqual(sorted(cmake_exe_sources()), sorted(zig_build.PORT_SOURCES))


class Defines(unittest.TestCase):
    NINJA = ("build CMakeFiles/dotheroes.dir/stubs.cpp.obj: CXX_COMPILER__dotheroes_unscanned_ x\n"
             "  DEFINES = -DWRONG=1\n"
             "build CMakeFiles/dotheroes.dir/main.cpp.obj: CXX_COMPILER__dotheroes_unscanned_ C$:/x/port/main.cpp\n"
             "  DEFINES = -DDOTHEROES_NATIVE_RELEASE=1 -DPPU_BARE_LOADS=1 -DPS3RECOMP_RSX_VULKAN "
             "-DDOTHEROES_DEV_DIAGNOSTICS=1\n"
             "  FLAGS = -O1\n")

    def test_ninja_parse(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "build.ninja")
            with open(p, "w") as f:
                f.write(self.NINJA)
            self.assertEqual(zig_build.ninja_game_defines(p),
                             ["-DDOTHEROES_NATIVE_RELEASE=1", "-DPPU_BARE_LOADS=1", "-DPS3RECOMP_RSX_VULKAN"])

    def test_ninja_parse_real_release(self):
        """the measured R8 release's own build (build/r8/rel7/bld, local): the six release defines"""
        p = os.path.join(ROOT, "build", "r8", "rel7", "bld", "build.ninja")
        if not os.path.exists(p):
            self.skipTest("no build/r8/rel7/bld here")
        names = {d[2:].split("=", 1)[0] for d in zig_build.ninja_game_defines(p)}
        self.assertEqual(names, RELEASE_DEFINES)

    def _jobs(self, game_build):
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
            if game_build is not None:
                with open(os.path.join(lib, zig_build.GAME_BUILD), "w") as f:
                    json.dump(game_build, f)
            src = zig_build.Sources("windows")
            src.lib_dir = lib
            src.nids = os.path.join(lib, "src", "ppu_hle_nids.cpp")
            with mock.patch.object(zig_build, "_zig_version", lambda z: "zig test"), \
                    mock.patch.object(zig_build, "host_cpu", lambda z=None: "x86_64_v3"), \
                    mock.patch.object(zig_build, "headers_fingerprint", lambda s, g: b"h"):
                return zig_build.game_jobs(src, gen, os.path.join(d, "obj"), "zig", "windows")

    def test_jobs_get_prebuilt_defines(self):
        defs = ["-DDOTHEROES_NATIVE_RELEASE=1", "-DPPU_BARE_LOADS=1", "-DPPU_STORE_MOV=1",
                "-DPS3RECOMP_RSX_COMMAND_PATH=0", "-DPS3RECOMP_RSX_VULKAN", "-DPS3RECOMP_SPU_INTERPRETER=0"]
        jobs = self._jobs({"config": "native-release", "defines": defs})
        kinds = set()
        for s, obj, tool, flags in jobs:
            for d in defs:
                self.assertEqual(flags.count(d), 1, f"{os.path.basename(s)}: {d}")
            kinds.add(os.path.basename(s))
        # the port, the scaffold, the NID table, a lifted PPU chunk and an SPU image all get them
        for f in ("main.cpp", "cpu_place.cpp", "ppu_loader.cpp", "ppu_hle_nids.cpp", "ppu_recomp_0.cpp",
                  "spu_recomp.c", "spu_dispatch_mt.c"):
            self.assertIn(f, kinds)

    def test_jobs_without_record(self):
        """an older prebuilt (no record): the development configuration, nothing added"""
        for s, obj, tool, flags in self._jobs(None):
            self.assertFalse(any(f.startswith("-DDOTHEROES_NATIVE_RELEASE") or f.startswith("-DPPU_")
                                 for f in flags), os.path.basename(s))

    def test_configuration_change_recompiles(self):
        a = {os.path.basename(j[1]) for j in self._jobs(None)}
        b = {os.path.basename(j[1]) for j in self._jobs({"defines": ["-DDOTHEROES_NATIVE_RELEASE=1"]})}
        self.assertFalse(a & b, "objects of another configuration were reused")


class Prebuilt(unittest.TestCase):
    def _cmake_args(self, **kw):
        calls = []

        class Stop(Exception):
            pass

        def check_call(cmd, **k):
            calls.append(cmd)
            raise Stop()

        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "build", "deps", "windows", "lib"))
            with mock.patch.object(zig_build, "ROOT", d), \
                    mock.patch.object(zig_build, "cmake_toolchain", lambda *a, **k: "tc.cmake"), \
                    mock.patch.object(zig_build.subprocess, "check_call", check_call):
                with self.assertRaises(Stop):
                    zig_build.build_prebuilt("windows", "ps3recomp", "gen", **kw)
        return calls[0]

    def test_release_by_default(self):
        args = self._cmake_args()
        self.assertIn("-DDOTHEROES_NATIVE_RELEASE=ON", args)
        self.assertIn("-DDOTHEROES_DEV_DIAGNOSTICS=OFF", args)
        self.assertFalse(any(a.startswith("-DDOTHEROES_SPU_INTERPRETER=ON") for a in args))

    def test_development_on_request(self):
        self.assertIn("-DDOTHEROES_NATIVE_RELEASE=OFF", self._cmake_args(release=False))


if __name__ == "__main__":
    unittest.main(verbosity=2)
