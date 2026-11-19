#!/usr/bin/env python3
"""tests/fs_case_check.py fails when it has nothing to check (M11.4, the review's L-5), and finds a case mismatch."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fs_case_check as fc  # noqa: E402


class FsCaseCheck(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.disc = os.path.join(self.tmp, "disc")
        os.makedirs(os.path.join(self.disc, "Data"))
        open(os.path.join(self.disc, "Data", "Title.TPK"), "w").close()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def log(self, text):
        p = os.path.join(self.tmp, "stderr.log")
        with open(p, "w", encoding="utf-8") as f:
            f.write(text)
        return p

    def test_nothing_to_check_fails(self):
        self.assertEqual(fc.main(["x", os.path.join(self.tmp, "no-disc"), self.log("x\n")]), 1)
        self.assertEqual(fc.main(["x", self.disc, os.path.join(self.tmp, "no-such-run")]), 1)
        self.assertEqual(fc.main(["x", self.disc, self.log("")]), 1)

    def test_a_case_mismatch_is_found(self):
        asked = os.path.join(self.disc, "data", "title.tpk")
        self.assertEqual(fc.main(["x", self.disc, self.log(f"[fs] open FAIL '/x' -> '{asked}'\n")]), 1)

    def test_a_clean_run_passes(self):
        missing = os.path.join(self.disc, "Data", "Nowhere.TPK")
        self.assertEqual(fc.main(["x", self.disc, self.log(f"[fs] open FAIL '/x' -> '{missing}'\n")]), 0)


if __name__ == "__main__":
    unittest.main()
