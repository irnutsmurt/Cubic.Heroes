#!/usr/bin/env python3
"""tests/map_sweep.py check: the map sweep's regression verdict, on made-up evidence (no game run; map names
invented): a clean sweep passes; a crash, a map never changed to or never arrived, a stall, another area than the
baseline's and a translation in play each fail; pipelines met unprepared are reported, not failed."""
import json
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SWEEP = os.path.join(ROOT, "tests", "map_sweep.py")


def chunk(ev, n, maps, exit_code=0, ended="time (stop.trigger)", skip_arrival=(), frames_per=760, areas=None,
          extra=""):
    """a chunk directory as tests/map_sweep.py run leaves it"""
    d = os.path.join(ev, f"chunk{n}")
    os.makedirs(os.path.join(d, "run"))
    with open(os.path.join(d, "maps.txt"), "w", newline="\n") as f:
        f.write("".join(f"{m} -gDebug 15\n" for m in maps))
    lines = [f"[sweep] {len(maps)} maps, SetMap found, the first at 60 s"]
    t, fr = 100.0, 1000
    for i, m in enumerate(maps):
        lines.append(f"[sweep] t={t:.3f} map {i + 1}/{len(maps)} {m} -gDebug")
        t += 15
        fr += frames_per
        if m not in skip_arrival:
            area = (areas or {}).get(m, 30 + i)
            lines.append(f"[sweep] t={t:.3f} arrived {i + 1}/{len(maps)} {m} area {area} frames {fr}")
    lines.append("[sweep] done")
    with open(os.path.join(d, "run", "stderr.log"), "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n" + extra)
    with open(os.path.join(d, "run", "result.json"), "w") as f:
        json.dump({"ended_by": ended, "exit": exit_code}, f)
    open(os.path.join(d, "done"), "w").close()


def check(ev, *args):
    p = subprocess.run([sys.executable, SWEEP, "check", ev] + list(args), capture_output=True, text=True)
    return p.returncode, p.stdout


class SweepCheck(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.ev = os.path.join(self.tmp.name, "ev")
        os.makedirs(self.ev)

    def tearDown(self):
        self.tmp.cleanup()

    def test_clean_sweep_passes_and_writes_a_baseline(self):
        chunk(self.ev, 0, ["M_A", "M_B"])
        chunk(self.ev, 1, ["M_C"], extra="[vk-compile] t=130.0 pipeline gfx key 00AB total 1.5 ms (x) unforeseen\n")
        base = os.path.join(self.tmp.name, "base.tsv")
        rc, out = check(self.ev, "--write-baseline", base)
        self.assertEqual(rc, 0, out)
        self.assertIn("PASS", out)
        self.assertIn("3 maps arrived of 3 planned; 1 pipelines met unprepared", out)
        with open(base) as f:
            rows = [ln.split() for ln in f if not ln.startswith("#")]
        self.assertEqual(rows, [["M_A", "30"], ["M_B", "31"], ["M_C", "30"]])
        rc, out = check(self.ev, "--baseline", base)
        self.assertEqual(rc, 0, out)

    def test_a_crash_fails(self):
        chunk(self.ev, 0, ["M_A"], exit_code=3, ended="the game exited")
        rc, out = check(self.ev)
        self.assertEqual(rc, 1)
        self.assertIn("did not run to its planned end", out)

    def test_a_map_never_arrived_fails(self):
        chunk(self.ev, 0, ["M_A", "M_B"], skip_arrival=("M_B",))
        rc, out = check(self.ev)
        self.assertEqual(rc, 1)
        self.assertIn("M_B changed to, no arrival logged", out)

    def test_a_stall_fails(self):
        chunk(self.ev, 0, ["M_A", "M_B"], frames_per=20)
        rc, out = check(self.ev)
        self.assertEqual(rc, 1)
        self.assertIn("M_B: the game hardly ran there", out)

    def test_another_area_than_the_baseline_fails(self):
        chunk(self.ev, 0, ["M_A", "M_B"], areas={"M_B": 30})   # M_B still in M_A's area: the change did not happen
        base = os.path.join(self.tmp.name, "base.tsv")
        with open(base, "w") as f:
            f.write("M_A 30\nM_B 31\n")
        rc, out = check(self.ev, "--baseline", base)
        self.assertEqual(rc, 1)
        self.assertIn("M_B arrived in area 30, the baseline's is 31", out)

    def test_a_translation_in_play_fails_only_with_a_preparation(self):
        chunk(self.ev, 0, ["M_A"], extra="[vk-compile] t=110.0 translate fp 120 words 3.1 ms glslang\n")
        rc, out = check(self.ev)
        self.assertEqual(rc, 1)
        self.assertIn("1 shader translation(s) compiled in play", out)
        rc, out = check(self.ev, "--no-prepared")
        self.assertEqual(rc, 0, out)

    def test_the_area_of_the_map_before_fails_without_a_baseline(self):
        # a map change the game did not take (a name it does not know reloads the current map): same area as before
        chunk(self.ev, 0, ["M_A", "M_B", "M_C"], areas={"M_A": 80, "M_B": 80, "M_C": 9})
        rc, out = check(self.ev)
        self.assertEqual(rc, 1)
        self.assertIn("M_B arrived in the area of the map before it (80): not reached", out)
        self.assertNotIn("M_C arrived in the area", out)

    def test_arrival_records(self):
        import struct
        sys.path.insert(0, os.path.join(ROOT, "tests"))
        import map_sweep

        def rec(t, a, b):
            return struct.pack("<IIII", t, 0, len(a), len(b)) + a + b
        # an alignment file's records (made up): a warp out (0x1004), arrival points (0x1005) of several kinds,
        # an object (0x13b8); the developers' -gDebug comes first whatever its place
        data = (b"\x10\x00\x00\x00junk" + rec(0x1004, b"M_X;-gfrom_y;-right", b"Warp") + rec(0x1005, b"-gfrom_a", b"Exit")
                + rec(0x13B8, b"", b"npc_010") + rec(0x1005, b"-g212;-o212;", b"WarpDoor")
                + rec(0x1005, b"-gDebug", b"Exit") + rec(0x1005, b"-gfrom_a", b"Exit"))
        p = os.path.join(self.tmp.name, "M_A.dat")
        with open(p, "wb") as f:
            f.write(data)
        self.assertEqual(map_sweep.arrivals(p), ["-gDebug", "-gfrom_a", "-g212;-o212;"])
        self.assertEqual(map_sweep.arrivals(os.path.join(self.tmp.name, "none.dat")), [])
        self.assertEqual(map_sweep.engine_name("Field_01"), "Field_1")
        self.assertEqual(map_sweep.engine_name("Field_10"), "Field_10")

    def test_left_out_draws_known_listed_new_failed(self):
        """M11.1b: the renderer's report -- a known left-out draw is listed with its bug, any other reason fails"""
        chunk(self.ev, 0, ["M_A"])
        with open(os.path.join(self.ev, "chunk0", "run", "ppfx_report.txt"), "w", newline="\n") as f:
            f.write("not rendered: 7 asp: no clip\n"
                    "not rendered: 1 unit 0 at 0:03190000 (format A5 1280x720 pitch 5120) has no native source\n")
        rc, out = check(self.ev)
        self.assertEqual(rc, 0, out)
        self.assertIn("known (B-007)", out)
        with open(os.path.join(self.ev, "chunk0", "run", "ppfx_report.txt"), "a", newline="\n") as f:
            f.write("not rendered: 3 asp: unit 3 (format A5 32x32 mips 6 dim 2 cube 1) has no source\n"
                    "not rendered: 1 asp: the batch is full\n")
        rc, out = check(self.ev)
        self.assertEqual(rc, 1, out)
        self.assertIn("3 draws not rendered: asp: unit 3", out)
        self.assertIn("1 draws not rendered: asp: the batch is full", out)   # B-006 is fixed: a failure again

    def test_texture_kinds_reported_by_map(self):
        """M11.1b: the cube-map and linear mipmapped textures each map first draws ([vk-texture])"""
        chunk(self.ev, 0, ["M_A", "M_B"],
              extra="[vk-texture] t=130.0 cube 32x32 levels 6 format A5 at 1:00207F00\n")
        rc, out = check(self.ev)
        self.assertEqual(rc, 0, out)
        self.assertIn("M_B: cube 32x32/6", out)

    def test_audio_per_map(self):
        """M11.1b: the music each map started and its mix's level, from the audio trace and the wave writer's mix"""
        import array
        import math
        import struct
        sys.path.insert(0, os.path.join(ROOT, "tests"))
        import map_sweep
        run = os.path.join(self.tmp.name, "run")
        os.makedirs(run)
        with open(os.path.join(run, "audio_trace.txt"), "w", newline="\n") as f:
            f.write("# audio trace v1\n")
            for k in range(0, 40):   # a line every half second, 30 guest frames apart
                f.write(f"{k * 500000} {k * 30} 1 SM.update 0.016 -> 0\n")
            f.write("10000000 600 1 SM.playBGM 12 0 0 -> 1\n")
        n = 48000 * 20
        a = array.array("f", [0.0] * (2 * n))
        for i in range(48000 * 10, n):   # a tone from 10 s on: the second map's
            a[2 * i] = a[2 * i + 1] = 0.25 * math.sin(i * 0.05)
        data = a.tobytes()
        with open(os.path.join(run, "mix.wav"), "wb") as f:
            f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 3, 2, 48000,
                    48000 * 8, 8, 32) + b"data" + struct.pack("<I", len(data)) + data)
        got = map_sweep.audio_per_map(run, [("M_A", 0), ("M_B", 600)])
        self.assertEqual(got["M_A"][0], [])
        self.assertLess(got["M_A"][1], map_sweep.SILENT_DB)
        self.assertEqual(got["M_B"][0], ["12"])
        self.assertGreater(got["M_B"][1], -30.0)

    def test_no_evidence_fails(self):
        rc, out = check(self.ev)
        self.assertEqual(rc, 1)
        self.assertIn("no chunk", out)


if __name__ == "__main__":
    unittest.main()
