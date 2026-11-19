#!/usr/bin/env python3
"""tools/hostenv.py (M11.4): the system's programs started from the frozen worker get the player's LD_LIBRARY_PATH,
not the worker's own _internal/; outside a frozen Linux worker the environment is left as it is."""
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import hostenv  # noqa: E402

WORKER = {"LD_LIBRARY_PATH": "/x/worker/_internal", "PATH": "/usr/bin"}


class HostEnv(unittest.TestCase):
    def test_not_frozen_unchanged(self):
        with mock.patch.object(sys, "frozen", False, create=True):
            self.assertEqual(hostenv.host_env(WORKER), WORKER)

    def test_frozen_linux(self):
        with mock.patch.object(sys, "frozen", True, create=True), mock.patch.object(sys, "platform", "linux"):
            self.assertNotIn("LD_LIBRARY_PATH", hostenv.host_env(WORKER))   # the player had none
            e = hostenv.host_env(dict(WORKER, LD_LIBRARY_PATH_ORIG="/opt/mine"))
            self.assertEqual(e["LD_LIBRARY_PATH"], "/opt/mine")
            self.assertNotIn("LD_LIBRARY_PATH_ORIG", e)
            self.assertEqual(e["PATH"], "/usr/bin")
        self.assertEqual(WORKER["LD_LIBRARY_PATH"], "/x/worker/_internal")   # the caller's mapping untouched

    def test_frozen_windows_unchanged(self):
        with mock.patch.object(sys, "frozen", True, create=True), mock.patch.object(sys, "platform", "win32"):
            self.assertEqual(hostenv.host_env(WORKER), WORKER)


if __name__ == "__main__":
    unittest.main()
