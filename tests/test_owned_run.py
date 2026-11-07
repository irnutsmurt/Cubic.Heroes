#!/usr/bin/env python3
"""tools/owned_run.py with dummy processes (no game, no build): a batch's whole
process tree ends when it is cancelled, when its launching shell is killed
(what stopping a background task does) or when the launcher itself is killed;
what it leaves running when it ends by itself is stopped; a process outside it
is never touched. Windows (job objects).
    build/venv/Scripts/python.exe tests/test_owned_run.py
docs/NATIVE_R8.md section 9.2."""
import ctypes
import ctypes.wintypes as w
import os
import subprocess
import sys
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import owned_run  # noqa: E402

PY = sys.executable
OWNED = os.path.join(ROOT, "tools", "owned_run.py")
DUMMY = os.path.join(ROOT, "tests", "owned_dummy.py")
GIT = next((d for d in (os.environ.get("GIT_USR_BIN", ""), r"C:\Program Files\Git\usr\bin")
            if d and os.path.exists(os.path.join(d, "bash.exe"))), None)
k32 = ctypes.WinDLL("kernel32", use_last_error=True) if os.name == "nt" else None
if k32:
    k32.OpenProcess.restype, k32.OpenProcess.argtypes = w.HANDLE, (w.DWORD, w.BOOL, w.DWORD)
    k32.WaitForSingleObject.restype, k32.WaitForSingleObject.argtypes = w.DWORD, (w.HANDLE, w.DWORD)
    k32.TerminateProcess.restype, k32.TerminateProcess.argtypes = w.BOOL, (w.HANDLE, w.UINT)
    k32.CloseHandle.argtypes = (w.HANDLE,)


def fwd(p):
    return p.replace("\\", "/")


def tree(root):
    """the pids below root (not root), from the process snapshot"""
    procs = owned_run.processes()
    kids = {}
    for pid, (pp, _) in procs.items():
        kids.setdefault(pp, []).append(pid)
    out, todo = [], list(kids.get(root, []))
    while todo:
        p = todo.pop()
        if p in out or p == root:
            continue
        out.append(p)
        todo += kids.get(p, [])
    return out


def wait_tree(root, at_least, secs=20):
    """the tree once it has at least `at_least` processes and has stopped growing; handles to each"""
    end, last, stable = time.time() + secs, None, 0
    while time.time() < end:
        t = sorted(tree(root))
        stable = stable + 1 if t == last else 0
        last = t
        if len(t) >= at_least and stable >= 3:
            break
        time.sleep(0.3)
    names = owned_run.processes()
    hs = []
    for p in last or []:
        h = k32.OpenProcess(0x00100000 | 0x1000, False, p)
        if h:
            hs.append((h, p, names.get(p, (0, "?"))[1]))
    return hs


def survivors(handles, secs=10):
    """the processes among `handles` still running after up to `secs`"""
    end = time.time() + secs
    while True:
        left = [(h, p, n) for h, p, n in handles if k32.WaitForSingleObject(h, 0) != 0]
        if not left or time.time() > end:
            return [f"{n} {p}" for _, p, n in left]
        time.sleep(0.2)


def owned(name, *cmd, **kw):
    return subprocess.Popen([PY, OWNED, "--name", name, "--", *cmd], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, **kw)


@unittest.skipUnless(os.name == "nt" and GIT, "Windows with Git's bash")
class OwnedRun(unittest.TestCase):
    def setUp(self):
        self.name = f"t{os.getpid()}-{self._testMethodName[5:]}"[:40]
        self.sh, self.bash = os.path.join(GIT, "sh.exe"), os.path.join(GIT, "bash.exe")
        self.batch_cmd = [self.sh, "-c", f"sleep 600 & '{fwd(PY)}' '{fwd(DUMMY)}' tree & wait"]
        self.procs = []

    def tearDown(self):
        subprocess.run([PY, OWNED, "--cancel", self.name], capture_output=True)
        for p in self.procs:   # reaped, each stopped by the test or the cancel above
            try:
                p.communicate(timeout=30)
            except subprocess.TimeoutExpired:
                p.kill()
                p.communicate()

    def track(self, p):
        self.procs.append(p)
        return p

    def test_exit_code(self):
        p = self.track(owned(self.name, PY, "-c", "import sys; sys.exit(3)"))
        out = p.communicate(timeout=60)[0]
        self.assertEqual(p.returncode, 3, out)
        self.assertFalse(os.path.exists(owned_run.record_path(self.name)))

    def test_leftovers_stopped(self):
        """the command ends by itself; what it started and left running is stopped"""
        p = self.track(owned(self.name, PY, DUMMY, "spawn-exit"))
        hs = wait_tree(p.pid, 3)
        out = p.communicate(timeout=60)[0]
        self.assertEqual(p.returncode, 0, out)
        self.assertIn("stopping what it left running", out)
        self.assertEqual(survivors(hs), [], out)

    def test_cancel(self):
        """--cancel ends the whole tree (shell, sleep, Python, its child), and nothing outside it"""
        outside = self.track(subprocess.Popen([PY, "-c", "import time; time.sleep(600)"]))
        try:
            p = self.track(owned(self.name, *self.batch_cmd))
            hs = wait_tree(p.pid, 5)
            self.assertGreaterEqual(len(hs), 5, [n for _, _, n in hs])
            listed = subprocess.run([PY, OWNED, "--list"], capture_output=True, text=True).stdout
            self.assertIn(self.name, listed)
            r = subprocess.run([PY, OWNED, "--cancel", self.name], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertEqual(survivors(hs), [])
            p.communicate(timeout=30)
            self.assertNotEqual(p.returncode, 0)
            self.assertIsNone(outside.poll(), "a process outside the batch was stopped")
        finally:
            outside.kill()

    def test_shell_killed(self):
        """the launching shell is killed (what stopping a background task does): the tree ends"""
        cmd = f"'{fwd(PY)}' '{fwd(OWNED)}' --name {self.name} -- '{fwd(self.sh)}' -c " \
              f"\"sleep 600 & '{fwd(PY)}' '{fwd(DUMMY)}' tree & wait\"; echo after"
        shell = self.track(subprocess.Popen([self.bash, "-c", cmd], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        hs = wait_tree(shell.pid, 6)
        self.assertGreaterEqual(len(hs), 6, [n for _, _, n in hs])
        shell.kill()   # TerminateProcess on the shell alone, as the background task's stop does
        self.assertEqual(survivors(hs), [])

    def test_shell_killed_exec(self):
        """the same with the launcher as the shell's only command (bash runs it in place)"""
        cmd = f"'{fwd(PY)}' '{fwd(OWNED)}' --name {self.name} -- '{fwd(self.sh)}' -c " \
              f"\"sleep 600 & '{fwd(PY)}' '{fwd(DUMMY)}' tree & wait\""
        shell = self.track(subprocess.Popen([self.bash, "-c", cmd], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        hs = wait_tree(shell.pid, 5)
        self.assertGreaterEqual(len(hs), 5, [n for _, _, n in hs])
        shell.kill()
        self.assertEqual(survivors(hs), [])

    def test_launcher_killed(self):
        """the launcher itself is killed: the job's kill-on-close ends the rest"""
        p = self.track(owned(self.name, *self.batch_cmd))
        hs = wait_tree(p.pid, 5)
        procs = owned_run.processes()
        # the launcher: the process in the tree whose child is the batch's sh
        launcher = next(pid for _, pid, n in hs if any(procs.get(c, (0, ""))[1].lower() == "sh.exe"
                                                       and procs[c][0] == pid for c in procs))
        h = k32.OpenProcess(0x0001, False, launcher)   # PROCESS_TERMINATE
        self.assertTrue(k32.TerminateProcess(h, 9))
        k32.CloseHandle(h)
        self.assertEqual(survivors(hs), [])
        p.communicate(timeout=30)

    def test_name_in_use(self):
        p = self.track(owned(self.name, *self.batch_cmd))
        wait_tree(p.pid, 5)
        r = subprocess.run([PY, OWNED, "--name", self.name, "--", PY, "-c", "pass"], capture_output=True, text=True,
                           timeout=60)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("is running", r.stdout + r.stderr)
        self.assertIsNone(p.poll(), "the first batch was disturbed")


if __name__ == "__main__":
    unittest.main(verbosity=2)
