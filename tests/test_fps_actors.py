#!/usr/bin/env python3
"""tests/fps_actors.py on made-up runs: a class that moves by time keeps its speed at a higher frame rate, one that
moves a step a frame is a suspect; an enemy segment counts only the characters new in it."""
import io
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fps_actors  # noqa: E402

REC = struct.Struct("<QIIfff")
TIMED, FRAMED = 0x00FE4690, 0x00FE4760


def make_run(root, name, fps, segments):
    """segments: [(name, t0, t1, [(ea, vt, speed or None (a step a frame of 0.05), start)])]"""
    d = os.path.join(root, name)
    os.makedirs(d)
    with open(os.path.join(d, "run.json"), "w") as f:
        json.dump({"switches": {"DOTHEROES_FPS": str(fps)}}, f)
    recs, frames = [], []
    with open(os.path.join(d, "segments.jsonl"), "w") as f:
        for sname, t0, t1, movers in segments:
            f.write(json.dumps({"name": sname, "t0": t0, "t1": t1, "ok": True}) + "\n")
            n = int((t1 - t0) * fps)
            frames += [t0 + i / fps for i in range(n)]
            for ea, vt, speed, start in movers:
                x = 0.0
                for i in range(n):
                    t = t0 + i / fps
                    if t < start:
                        continue
                    x += speed / fps if speed else 0.05
                    recs.append((int(t * 1e9), ea, vt, x))
    recs.sort()
    with open(os.path.join(d, "actors.bin"), "wb") as f:
        f.write(b"".join(REC.pack(t, ea, vt, x, 0.0, 0.0) for t, ea, vt, x in recs))
    with open(os.path.join(d, "frames.log"), "w") as f:
        f.write("# test\n" + "".join(f"f {int(t * 1e6)}\n" for t in frames))
    return d


class FpsActors(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def runs(self):
        segs = [("boss-test", 100.0, 120.0, [(0x10000000, TIMED, 3.0, 100.0), (0x10000100, FRAMED, None, 100.0)])]
        return [make_run(self.root, f"r{fps}", fps, segs) for fps in (30, 144)]

    def test_frame_counted_class_is_suspect(self):
        out = io.StringIO()
        with redirect_stdout(out):
            sys.argv = ["fps_actors.py"] + self.runs()
            rc = fps_actors.main()
        text = out.getvalue()
        self.assertEqual(rc, 1)
        self.assertIn(f"boss-test class 0x{FRAMED:08X} at cap 144", text)
        self.assertNotIn(f"boss-test class 0x{TIMED:08X}", text.split("suspect(s)")[1])

    def test_speeds(self):
        r30, r144 = (fps_actors.run(d) for d in self.runs())
        a, b = r30["segs"]["boss-test"]["cls"], r144["segs"]["boss-test"]["cls"]
        self.assertAlmostEqual(a[TIMED]["med"], 3.0, delta=0.2)
        self.assertAlmostEqual(b[TIMED]["med"], 3.0, delta=0.2)
        self.assertAlmostEqual(b[FRAMED]["med"] / a[FRAMED]["med"], 144 / 30, delta=0.3)
        self.assertAlmostEqual(r144["segs"]["boss-test"]["fps"], 144, delta=1)

    def test_enemy_segment_counts_new_only(self):
        # the map's own enemy moves from before the segment; the created one appears in it
        segs = [("warmup", 90.0, 100.0, [(0x10000000, fps_actors.ENEMY, 2.0, 90.0)]),
                ("enemy-Test", 100.0, 110.0, [(0x10000000, fps_actors.ENEMY, 2.0, 100.0),
                                              (0x10000200, fps_actors.ENEMY, 6.0, 101.0)])]
        r = fps_actors.run(make_run(self.root, "e", 60, segs))
        e = r["segs"]["enemy-Test"]["cls"][fps_actors.ENEMY]
        self.assertEqual(e["n"], 1)
        self.assertAlmostEqual(e["med"], 6.0, delta=0.3)


if __name__ == "__main__":
    unittest.main()
