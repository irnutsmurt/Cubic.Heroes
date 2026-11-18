#!/usr/bin/env python3
"""tests/desktop_guard.py: tests stay offscreen unless an on-screen run is approved, and a window seen anyway ends
the run. No window is opened: a window is simulated (visible_windows replaced), the process is a sleeping Python."""
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import desktop_guard as g  # noqa: E402


class DesktopGuardTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.saved = g.APPROVAL
        g.APPROVAL = os.path.join(self.tmp, "ONSCREEN_APPROVAL.json")

    def tearDown(self):
        g.APPROVAL = self.saved

    def approve(self, minutes):
        with open(g.APPROVAL, "w", encoding="utf-8") as f:
            json.dump({"until": time.time() + minutes * 60, "what": "test"}, f)

    def test_off_by_default(self):
        self.assertFalse(g.onscreen_allowed())
        env = {"WINDOW": "1", "VISIBLE": "1", "TOPMOST": "1", "GRAB_EVERY": "1", "PS3RECOMP_VK_WINDOW_HIDDEN": "1"}
        self.assertFalse(g.game_env(env, True, quiet=True))
        self.assertEqual(env["PS3RECOMP_VK_WINDOW"], "0")
        for k in ("VISIBLE", "TOPMOST", "GRAB_EVERY"):
            self.assertNotIn(k, env)
        self.assertEqual(g.launcher_env({"CUBIC_NAV_SHOW": "1"})["CUBIC_NAV_HIDDEN"], "1")
        self.assertEqual(g.launcher_env({})["CUBIC_NAV_HIDDEN"], "1")

    def test_expired_approval_is_none(self):
        self.approve(-1)
        self.assertFalse(g.onscreen_allowed())

    def test_approved(self):
        self.approve(5)
        env = {}
        self.assertTrue(g.game_env(env, True, quiet=True))
        self.assertEqual(env["PS3RECOMP_VK_WINDOW"], "1")
        self.assertFalse(g.game_env({}, False, quiet=True))   # approved, but not asked for: still offscreen
        self.assertEqual(g.launcher_env({"CUBIC_NAV_SHOW": "1"})["CUBIC_NAV_HIDDEN"], "0")

    def test_require_stops_without_approval(self):
        with self.assertRaises(SystemExit):
            g.require_onscreen("a test")
        self.approve(5)
        g.require_onscreen("a test")   # no exit

    def test_watch_ends_a_process_that_shows_a_window(self):
        proc = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
        real = g.visible_windows, g.display_connections
        g.visible_windows = lambda pid: [("SDL_app", "x", 640, 360)] if pid == proc.pid else []
        g.display_connections = lambda pid: [("wayland", "/run/x/wayland-0")] if pid == proc.pid else []
        try:
            w = g.Watch(proc, every=0.05)
            w._run()   # on this thread
            self.assertIsNotNone(w.violation)
            self.assertIsNotNone(proc.wait(timeout=10))
        finally:
            g.visible_windows, g.display_connections = real
            if proc.poll() is None:
                proc.kill()

    def test_watch_leaves_a_windowless_process(self):
        proc = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(0.6)"])
        w = g.Watch(proc, every=0.05)
        w._run()   # returns when the process ends by itself
        self.assertIsNone(w.violation)
        self.assertEqual(proc.wait(timeout=10), 0)


    @unittest.skipIf(g.WIN, "Linux: the environment has no display")
    def test_linux_no_display(self):
        env = {"DISPLAY": ":1", "WAYLAND_DISPLAY": "wayland-0", "WAYLAND_SOCKET": "5"}
        self.assertFalse(g.game_env(env, True, quiet=True))
        self.assertNotIn("DISPLAY", env)
        self.assertNotIn("WAYLAND_SOCKET", env)
        self.assertEqual(env["WAYLAND_DISPLAY"], g.NO_WAYLAND)   # unset, libwayland would try wayland-0
        env = g.launcher_env({"DISPLAY": ":1", "CUBIC_NAV_SHOW": "1"})
        self.assertNotIn("DISPLAY", env)
        self.assertEqual(env["SDL_VIDEODRIVER"], "offscreen")
        self.approve(5)
        env = {"DISPLAY": ":1", "WAYLAND_DISPLAY": "wayland-0"}
        self.assertTrue(g.game_env(env, True, quiet=True))
        self.assertEqual((env["DISPLAY"], env["WAYLAND_DISPLAY"]), (":1", "wayland-0"))   # approved: untouched

    def _listener(self, name):
        import socket
        srv = socket.socket(socket.AF_UNIX)
        srv.bind(os.path.join(self.tmp, name))
        srv.listen(4)
        held = []   # accepted, as a display server does (an unaccepted connection has no server end to see)

        def serve():
            try:
                while True:
                    held.append(srv.accept()[0])
            except OSError:
                pass
        __import__("threading").Thread(target=serve, daemon=True).start()
        self.addCleanup(lambda: [c.close() for c in held + [srv]])
        return os.path.join(self.tmp, name)

    def _client(self, path):
        """A child that connects to `path` from a grandchild (the watch covers the whole tree), then sleeps."""
        code = ("import socket,subprocess,sys,time;"
                "subprocess.Popen([sys.executable,'-c',\"import socket,time;s=socket.socket(socket.AF_UNIX);"
                f"s.connect({path!r});time.sleep(60)\"]);time.sleep(60)")
        proc = subprocess.Popen([sys.executable, "-c", code], start_new_session=True)
        self.addCleanup(lambda: (os.killpg(proc.pid, 9) if proc.poll() is None else None))
        return proc

    @unittest.skipIf(g.WIN or not __import__("shutil").which("ss"), "Linux with ss")
    def test_linux_display_connection_seen_and_ended(self):
        proc = self._client(self._listener("wayland-guardtest"))
        deadline = time.time() + 10
        while not g.display_connections(proc.pid) and time.time() < deadline:
            time.sleep(0.1)
        self.assertEqual(g.display_connections(proc.pid)[0][0], "wayland")
        w = g.Watch(proc, every=0.05)
        w._run()
        self.assertIn("connected to a display server", w.violation)
        self.assertIsNotNone(proc.wait(timeout=10))

    @unittest.skipIf(g.WIN or not __import__("shutil").which("ss"), "Linux with ss")
    def test_linux_other_socket_ignored(self):
        proc = self._client(self._listener("not-a-display"))
        time.sleep(1.0)
        self.assertEqual(g.display_connections(proc.pid), [])
        self.assertIsNone(proc.poll())


if __name__ == "__main__":
    unittest.main()
