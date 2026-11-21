#!/usr/bin/env python3
"""Deterministic save selection for scripted runs (tests/save_select.py; 0.2.0-beta.5): the same game save on every
system, whatever order the file system lists the checkpoint in.
    build/venv/Scripts/python.exe -m unittest tests.test_save_select"""
import json
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import save_select  # noqa: E402

A, B = "BLUS30490GAME-DATA66BE2553", "BLUS30490GAME-DATA66BE25F6"
SYSTEM, EDIT = "BLUS30490SYSTEM", "BLJM60180EDIT"


class SaveSelect(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="saves-", dir=os.environ.get("TMPDIR"))
        for n in (A, B, SYSTEM, EDIT, "not-a-save"):
            os.makedirs(os.path.join(self.d, n))
        with open(os.path.join(self.d, "README.txt"), "w") as f:
            f.write("a file, not a save")

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def checkpoint(self, **kw):
        with open(os.path.join(self.d, "checkpoint.json"), "w") as f:
            json.dump(kw, f)

    def test_one_game_save_by_default_the_first_by_name(self):
        names, loaded = save_select.choose(self.d)
        self.assertEqual(loaded, A)
        self.assertEqual(names, sorted([A, EDIT, SYSTEM]))

    def test_the_same_whatever_the_file_system_order(self):
        listed = sorted(os.listdir(self.d))
        for order in (listed, list(reversed(listed)), [B, SYSTEM, A, EDIT, "not-a-save", "README.txt"]):
            with mock.patch.object(save_select.os, "listdir", return_value=order):
                self.assertEqual(save_select.choose(self.d), (sorted([A, EDIT, SYSTEM]), A), order)

    def test_the_checkpoint_names_its_save(self):
        self.checkpoint(save=B)
        names, loaded = save_select.choose(self.d)
        self.assertEqual((loaded, names), (B, sorted([B, EDIT, SYSTEM])))

    def test_the_run_names_its_save(self):
        self.checkpoint(save=B)
        self.assertEqual(save_select.choose(self.d, named=A)[1], A)

    def test_a_named_save_that_isnt_there(self):
        with self.assertRaises(SystemExit):
            save_select.choose(self.d, named="BLUS30490GAME-NOPE")

    def test_the_dialog_on_gets_every_save(self):
        names, loaded = save_select.choose(self.d, picker=True)
        self.assertEqual((names, loaded), (sorted([A, B, EDIT, SYSTEM]), None))

    def test_a_single_save(self):
        shutil.rmtree(os.path.join(self.d, B))
        self.assertEqual(save_select.choose(self.d), (sorted([A, EDIT, SYSTEM]), A))


if __name__ == "__main__":
    unittest.main()
