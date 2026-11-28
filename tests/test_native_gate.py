"""tests/native_gate.py: the native-architecture gate on a run's exit census (no game run here: the cases are the
census values the runs of 2026-11-25 had).

    py -3 -m unittest tests.test_native_gate
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from native_gate import native_gate  # noqa: E402

CLEAN_FIGHT = {"spurs_task": 0, "native_decline": 0, "lifted_spu_start": 0, "spurs_instance": 3,
               "phys_native": 44574, "phys_lifted": 0}


class Gate(unittest.TestCase):
    def test_a_native_fight_passes(self):
        self.assertEqual(native_gate(CLEAN_FIGHT, fights=True), {"ok": True, "why": []})

    def test_v101_before_its_physics_fix_fails(self):
        """v1.01 before 61df8ac: the physics tasks declined, run through SPURS, two SPU programs lifted"""
        c = dict(CLEAN_FIGHT, spurs_task=44331, native_decline=44331, lifted_spu_start=2, phys_native=0,
                 phys_lifted=44331)
        r = native_gate(c, fights=True)
        self.assertFalse(r["ok"])
        self.assertEqual(len(r["why"]), 5)

    def test_a_fighting_run_left_at_a_menu_fails(self):
        """the save list dialog kept the run at the title: a clean census, no physics"""
        c = dict(CLEAN_FIGHT, phys_native=0)
        self.assertFalse(native_gate(c, fights=True)["ok"])
        self.assertTrue(native_gate(c, fights=False)["ok"])   # a menu drive is fine so

    def test_no_census(self):
        self.assertFalse(native_gate({}, fights=False)["ok"])
        self.assertFalse(native_gate(None, fights=True)["ok"])

    def test_spurs_instances_alone_are_fine(self):
        """the native path still makes the game's SPURS instances; no task runs on them"""
        self.assertTrue(native_gate(dict(CLEAN_FIGHT, spurs_instance=3))["ok"])


if __name__ == "__main__":
    unittest.main()
