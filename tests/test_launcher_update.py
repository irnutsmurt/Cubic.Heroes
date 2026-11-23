#!/usr/bin/env python3
"""The launcher's update installed on restart (0.2.0-beta.4; tools/updates.py, the worker's apply-update,
swap-launcher, rollback-launcher, recover-launcher, launcher-state):
    build/venv/Scripts/python.exe -m unittest tests.test_launcher_update

A portable root (the launcher's package beside the install's items, the player's own file, the game link) and a
release downloaded into it as download-update leaves one (the package, release.json and release.json.sig as
fetched), signed with throwaway Ed25519 keys made in a temporary folder -- never the project's.

- Before anything is staged: the signature (altered, foreign, missing), the version (not newer), the platform (another
  system's package, another system's programs inside it, another installation kind's), the package's bytes (one
  changed), the version inside the package, the download missing, too little disk space -- each refused, nothing
  staged, the installation untouched.
- The swap: the package's items replaced, the install's untouched byte for byte, the replaced launcher kept for
  rollback; a new launcher that doesn't answer as the version expected, or whose window doesn't come up, puts the
  previous one back.
- A crash at every step of the swap: the folder always has a launcher to start, and recovery puts the previous
  launcher back byte for byte (or keeps a swap that had finished and answers as expected).
- One update at a time (the lock); a program still running from the installation (the game) stops the swap.
- The launcher's own rollback, and back again.
- An AppImage installation: the file replaced in one rename, kept for rollback, put back on failure, recovered.
- The worker's commands: download-update keeps the manifest and signature as fetched; apply-update refuses what
  fails a check, a running update, and starts nothing then."""
import base64
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import install_layout as layout  # noqa: E402
import release_trust as trust  # noqa: E402
import updates  # noqa: E402

OPENSSL = shutil.which("openssl")
WIN = os.name == "nt"
EXE, WORKER_EXE = updates.launcher_names()
PLAT = updates.platform_name()
KIND = "zip" if WIN else "tar.xz"
OLD, NEW = "0.2.0-beta.3", "0.2.0-beta.4"


def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb" if isinstance(data, bytes) else "w") as f:
        f.write(data)


def get(path):
    with open(path, "rb") as f:
        return f.read()


def program(fmt, arch="x86_64", tag=b""):
    """the first bytes of a program file: a PE or ELF header naming the processor (what check_program reads)"""
    if fmt == "pe":
        head = bytearray(0x200)
        head[:2] = b"MZ"
        struct.pack_into("<I", head, 0x3C, 0x80)
        head[0x80:0x84] = b"PE\0\0"
        struct.pack_into("<H", head, 0x84, {"x86_64": 0x8664, "arm64": 0xAA64}[arch])
    else:
        head = bytearray(0x200)
        head[:4] = b"\x7fELF"
        head[4], head[5] = 2, 1
        struct.pack_into("<H", head, 18, {"x86_64": 62, "arm64": 183}[arch])
    return bytes(head) + tag


NATIVE = "pe" if WIN else "elf"
FOREIGN = "elf" if WIN else "pe"


def package(folder, version, fmt=NATIVE):
    """a launcher package's items, each saying its version; its programs this system's (or `fmt`'s)"""
    for n in layout.launcher_package():
        if n == EXE:
            put(os.path.join(folder, n), program(fmt, tag=f"launcher {version}".encode()))
        elif n == "worker":
            put(os.path.join(folder, n, WORKER_EXE), program(fmt, tag=f"worker {version}".encode()))
            put(os.path.join(folder, n, "_internal", "python.bin"), f"python {version}")
            put(os.path.join(folder, n, "tools", "version.py"), f'"""the version"""\nVERSION = "{version}"\n')
        elif n in ("licenses", "lib"):
            put(os.path.join(folder, n, "SDL2.txt"), f"licence {version}")
        else:
            put(os.path.join(folder, n), f"{n} {version}")


def archive(path, folder):
    """the package folder as this system's download (a zip or a tar.xz with one top folder)"""
    top = os.path.basename(folder)
    if path.endswith(".zip"):
        import zipfile
        with zipfile.ZipFile(path, "w") as z:
            for d, _, fs in os.walk(folder):
                for f in fs:
                    full = os.path.join(d, f)
                    z.write(full, os.path.join(top, os.path.relpath(full, folder)).replace(os.sep, "/"))
    else:
        import tarfile
        with tarfile.open(path, "w:xz") as t:
            t.add(folder, top)
    return path


def portable_root(root, version=OLD):
    package(root, version)
    put(os.path.join(root, "active.json"), '{"layout": 2, "active": "b1", "previous": [], "retention": 1}')
    put(os.path.join(root, "builds", "b1", layout.exe_name()), b"the game")
    put(os.path.join(root, "data", "gamedata", "dev_hdd0", "SAVE.BIN"), b"the player's save")
    put(os.path.join(root, "data", "cache", "pipelines", "cache.bin"), b"the shader cache")
    put(os.path.join(root, "config", "dotheroes.cfg"), "DOTHEROES_FPS=60\n")
    put(os.path.join(root, "logs", "game.log"), "a log")
    put(os.path.join(root, "backups", "saves", "x", "gamedata", "SAVE.BIN"), b"a backup")
    put(os.path.join(root, layout.ROOT_MARKER), "root")
    put(os.path.join(root, "my notes.txt"), "the player's")
    layout.make_link(os.path.join(root, "game"), os.path.join(root, "builds", "b1"))


def snapshot(root):
    """the install's items and the player's file, byte for byte (links as their target; downloads/, the updates'
    own folder, left out)"""
    out = {}
    for n in layout.INSTALL_ITEMS + (layout.ROOT_MARKER, "my notes.txt"):
        if n == "downloads":   # the updates' own folder: the downloads and the launcher update's records
            continue
        p = os.path.join(root, n)
        if layout.is_link(p):
            out[n] = ("link", os.path.normcase(os.path.realpath(p)))
        elif os.path.isfile(p):
            out[n] = get(p)
        elif os.path.isdir(p):
            for d, dirs, fs in os.walk(p):
                dirs[:] = [x for x in dirs if not layout.is_link(os.path.join(d, x))]
                for f in fs:
                    q = os.path.join(d, f)
                    out[os.path.relpath(q, root)] = get(q)
    return out


def launcher_items(root):
    """the launcher's package items at the root, byte for byte"""
    out = {}
    for n in layout.launcher_package():
        p = os.path.join(root, n)
        if os.path.isfile(p):
            out[n] = get(p)
        elif os.path.isdir(p):
            for d, _, fs in os.walk(p):
                for f in fs:
                    out[os.path.relpath(os.path.join(d, f), root)] = get(os.path.join(d, f))
        else:
            out[n] = None
    return out


def answers(version):
    """a stand-in for launcher_check: the program at the root answers as `version` when its bytes say so"""
    def check(cmd, expected, **kw):
        got = get(cmd[0])
        ok = f"launcher {expected}".encode() in got
        return ok, "" if ok else f"it is not {expected}"
    return check


class Crash(BaseException):
    """a power cut: nothing after it runs, nothing is cleaned up"""


@unittest.skipUnless(OPENSSL, "the openssl command line makes the throwaway keys")
class Base(unittest.TestCase):
    def setUp(self):
        self.tmpd = tempfile.TemporaryDirectory(prefix="launcher-update-")
        self.tmp = self.tmpd.name
        self.root = os.path.join(self.tmp, "Cubic Heroes ünïcode")
        portable_root(self.root)
        self.before = snapshot(self.root)
        self.old_items = launcher_items(self.root)
        self.dirs = updates.update_dirs(self.root)
        self.key, self.kid, self.keys = self.make_key("k1")
        self.other, self.other_id, _ = self.make_key("k2")

    def tearDown(self):
        layout.remove_link(os.path.join(self.root, "game"))
        self.tmpd.cleanup()

    def make_key(self, name):
        k = os.path.join(self.tmp, name + ".pem")
        subprocess.run([OPENSSL, "genpkey", "-algorithm", "ed25519", "-out", k], check=True, capture_output=True)
        der = subprocess.run([OPENSSL, "pkey", "-in", k, "-pubout", "-outform", "DER"], check=True,
                             capture_output=True).stdout
        pk = trust.public_key_from(der)
        return k, trust.key_id(pk), [(trust.key_id(pk), pk)]

    def download(self, version=NEW, inside=None, fmt=NATIVE, platform=PLAT, kind=KIND, key=None, tamper=None,
                 kid=None):
        """what download-update leaves: downloads/<version>/ with the package, release.json and its signature"""
        src = os.path.join(self.tmp, "src-" + version, "Cubic.Heroes")
        if os.path.exists(os.path.dirname(src)):
            shutil.rmtree(os.path.dirname(src))
        package(src, inside or version, fmt)
        d = os.path.join(self.root, "downloads", version)
        os.makedirs(d, exist_ok=True)
        ext = {"zip": ".zip", "tar.xz": ".tar.xz"}[kind]
        name = f"Cubic.Heroes-{version}-{platform}{ext}"
        pkg = archive(os.path.join(d, name), src)
        data = get(pkg)
        m = {"format": 1, "version": version, "channel": "prerelease", "min_launcher": None, "notes": "n",
             "key_id": kid or self.kid, "assets": [{"name": name, "platform": platform, "kind": kind, "size": len(data),
                                             "sha256": hashlib.sha256(data).hexdigest()}]}
        mp = os.path.join(d, "release.json")
        put(mp, json.dumps(m, indent=1))
        subprocess.run([OPENSSL, "pkeyutl", "-sign", "-inkey", key or self.key, "-rawin", "-in", mp, "-out",
                        mp + ".sig"], check=True, capture_output=True)
        if tamper == "manifest":
            put(mp, get(mp).replace(b'"notes": "n"', b'"notes": "N"'))
        elif tamper == "signature":
            sig = bytearray(get(mp + ".sig"))
            sig[0] ^= 1
            put(mp + ".sig", bytes(sig))
        elif tamper == "package":
            b = bytearray(data)
            b[len(b) // 2] ^= 0xFF
            put(pkg, bytes(b))
        elif tamper == "no-signature":
            os.remove(mp + ".sig")
        return pkg

    def prepare(self, version=NEW, installed=OLD):
        return updates.prepare_update(self.root, self.root, version, self.keys, installed=installed)

    def assertUntouched(self):
        self.assertEqual(snapshot(self.root), self.before, "the install's items untouched, byte for byte")
        self.assertTrue(layout.is_link(os.path.join(self.root, "game")))

    def assertOld(self):
        self.assertEqual(launcher_items(self.root), self.old_items, "the previous launcher, byte for byte")
        self.assertUntouched()


class BeforeStaging(Base):
    """every check before anything is staged: refused, nothing staged, nothing changed"""

    def refused(self, code, **kw):
        with self.assertRaises(trust.ReleaseError) as e:
            self.prepare(**kw)
        self.assertEqual(e.exception.code, code, str(e.exception))
        self.assertFalse(os.path.exists(self.dirs["staged"]), "nothing staged")
        self.assertFalse(os.path.exists(self.dirs["journal"]))
        self.assertOld()
        return e.exception

    def test_a_verified_download_is_staged(self):
        self.download()
        j = self.prepare()
        self.assertEqual((j["state"], j["version"], j["from"], j["kind"]), ("staged", NEW, OLD, KIND))
        self.assertEqual(updates.package_version(self.dirs["staged"]), NEW)
        self.assertEqual(updates.read_journal(self.root)["state"], "staged")
        self.assertOld()   # staging changes nothing of the working installation

    def test_altered_manifest(self):
        self.download(tamper="manifest")
        self.refused("bad_signature")

    def test_invalid_signature(self):
        self.download(tamper="signature")
        self.refused("bad_signature")

    def test_missing_signature(self):
        self.download(tamper="no-signature")
        self.refused("no_download")

    def test_signed_by_another_key(self):
        self.download(key=self.other)   # naming the trusted key, signed by another: the signature fails
        self.refused("bad_signature")

    def test_foreign_key(self):
        self.download(key=self.other, kid=self.other_id)   # a key this launcher doesn't trust
        e = self.refused("untrusted_key")
        self.assertIn("key", str(e).lower())

    def test_altered_package(self):
        self.download(tamper="package")
        self.refused("bad_download")

    def test_not_newer(self):
        self.download(version=OLD)
        self.refused("not_newer", version=OLD)
        self.download(version="0.2.0-beta.2")
        self.refused("not_newer", version="0.2.0-beta.2")

    def test_another_systems_package(self):
        other = "linux-x86_64" if WIN else "windows-x86_64"
        self.download(platform=other, kind="tar.xz" if WIN else "zip")
        self.refused("platform")

    def test_another_systems_programs_inside(self):
        self.download(fmt=FOREIGN)   # signed and named for this system, but Windows programs on Linux (or the reverse)
        self.refused("platform")

    def test_another_processor_inside(self):
        src = os.path.join(self.tmp, "arm")
        package(src, NEW)
        put(os.path.join(src, EXE), program(NATIVE, "arm64"))
        with self.assertRaises(trust.ReleaseError) as e:
            updates.check_package(src, NEW)
        self.assertEqual(e.exception.code, "platform")

    def test_another_kind_of_installation(self):
        self.download()
        with self.assertRaises(trust.ReleaseError) as e:   # this installation is an AppImage: no AppImage released
            updates.verified_download(self.root, NEW, self.keys, OLD, "appimage")
        self.assertEqual(e.exception.code, "platform")

    def test_version_inside_differs(self):
        self.download(inside="0.2.0-beta.9")
        self.refused("bad_download")

    def test_not_downloaded(self):
        self.refused("no_download")

    def test_too_little_space(self):
        self.download()
        real = shutil.disk_usage
        with mock.patch.object(updates.shutil, "disk_usage",
                               lambda p: real(p)._replace(free=10 << 20)):
            self.refused("disk_space")

    def test_launcher_state(self):
        self.download()
        st = updates.launcher_state(self.root, self.root, installed=OLD, keys=self.keys)
        self.assertEqual(st["ready"], {"version": NEW})
        self.assertNotIn("rollback", st)
        self.download(version="0.2.0-beta.5", tamper="signature")   # a newer download that doesn't verify: not offered
        st = updates.launcher_state(self.root, self.root, installed=OLD, keys=self.keys)
        self.assertEqual(st["ready"], {"version": NEW})


class Swap(Base):
    def update(self, check=None, restart=False, **kw):
        self.download()
        self.prepare()
        return updates.swap_launcher(self.root, EXE, log=lambda t: None, expect_version=NEW,
                                     check=check or answers(NEW), restart=restart, idle_timeout=1, **kw)

    def assertNew(self):
        self.assertIn(b"launcher " + NEW.encode(), get(os.path.join(self.root, EXE)))
        self.assertIn(b"worker " + NEW.encode(), get(os.path.join(self.root, "worker", WORKER_EXE)))
        self.assertEqual(updates.package_version(self.root), NEW)
        self.assertUntouched()

    def test_updated(self):
        self.assertEqual(self.update(), "updated")
        self.assertNew()
        self.assertEqual(updates.rollback_version(self.root), OLD)
        self.assertEqual(launcher_items(self.dirs["rollback"]), self.old_items, "the previous launcher kept whole")
        for gone in ("staged", "previous", "journal", "lock"):
            self.assertFalse(os.path.exists(self.dirs[gone]), gone)
        last = json.load(open(self.dirs["last"]))
        self.assertEqual((last["result"], last["version"], last["previous"]), ("updated", NEW, OLD))

    def test_no_stale_files(self):
        put(os.path.join(self.root, "worker", "_internal", "gone.pyd"), "a file the new version no longer has")
        self.old_items = launcher_items(self.root)
        self.assertEqual(self.update(), "updated")
        self.assertFalse(os.path.exists(os.path.join(self.root, "worker", "_internal", "gone.pyd")),
                         "the worker replaced whole: nothing of the old one left in it")

    def test_new_launcher_not_answering_puts_the_previous_back(self):
        self.assertEqual(self.update(check=lambda cmd, v, **kw: (False, "it did not answer")), "restored")
        self.assertOld()
        self.assertIn(b"launcher " + NEW.encode(), get(os.path.join(self.dirs["failed"], EXE)), "kept aside")
        self.assertFalse(os.path.exists(self.dirs["journal"]))
        self.assertEqual(json.load(open(self.dirs["last"]))["result"], "restored")

    def test_window_not_coming_up_puts_the_previous_back(self):
        started = []

        def start(cmd, cwd, env=None):
            started.append(cmd[1])
            return mock.Mock(poll=lambda: 1, returncode=1)
        with mock.patch.object(updates, "_start_detached", start):
            self.assertEqual(self.update(restart=True, health_timeout=2), "restored")
        self.assertEqual(started, ["--after-update", "--update-failed"])
        self.assertOld()

    def test_window_up_with_the_right_version(self):
        def start(cmd, cwd, env=None):
            if cmd[1] == "--after-update":
                put(env[updates.HEALTH_ENV], NEW)
            return mock.Mock(poll=lambda: None)
        with mock.patch.object(updates, "_start_detached", start):
            self.assertEqual(self.update(restart=True, health_timeout=5), "updated")
        self.assertNew()

    def test_a_crash_at_every_step_is_recovered(self):
        self.download()
        n = 0
        while True:
            self.prepare()
            calls = [0]
            real = updates._replace

            def crashing(src, dst, tries=50, k=n):
                if calls[0] == k:
                    raise Crash()
                calls[0] += 1
                return real(src, dst, tries)
            with mock.patch.object(updates, "_replace", crashing):
                try:
                    res = updates.swap_launcher(self.root, EXE, log=lambda t: None, expect_version=NEW,
                                                check=answers(NEW), restart=False, idle_timeout=1)
                except Crash:
                    res = None
            if res is not None:   # the crash point lies past the last rename: the update went through
                self.assertEqual(res, "updated")
                break
            with self.subTest(step=n):
                self.assertTrue(os.path.isfile(os.path.join(self.root, EXE)), "a launcher to start, always")
                state = updates.read_journal(self.root)["state"]
                got = updates.recover_launcher(self.root, log=lambda t: None, check=answers(NEW), idle_timeout=1)
                self.assertEqual(got, {"swapped": "kept", "staged": "nothing"}.get(state, "restored"), state)
                if got != "kept":
                    self.assertOld()
                else:
                    self.assertEqual(updates.package_version(self.root), NEW)
                    break
                self.assertFalse(os.path.exists(self.dirs["journal"]))
            n += 1
        self.assertGreater(n, 4, "the crash was tried at several steps")

    def test_crash_after_the_swap_kept_or_undone_by_its_check(self):
        for ok, expect in ((True, "kept"), (False, "restored")):
            with self.subTest(ok=ok):
                self.download()
                self.prepare()
                with self.assertRaises(Crash):
                    updates.swap_launcher(self.root, EXE, log=lambda t: None, expect_version=NEW, restart=False,
                                          check=mock.Mock(side_effect=Crash()), idle_timeout=1)
                self.assertEqual(updates.read_journal(self.root)["state"], "swapped")
                got = updates.recover_launcher(self.root, log=lambda t: None, idle_timeout=1,
                                               check=lambda cmd, v, **kw: (ok, ""))
                self.assertEqual(got, expect)
                if ok:
                    self.assertEqual(updates.package_version(self.root), NEW)
                    self.assertEqual(updates.rollback_version(self.root), OLD)
                    # back to the old one for the next round
                    self.assertEqual(updates.swap_launcher(self.root, EXE, log=lambda t: None, source="rollback",
                                                           check=answers(OLD), restart=False, idle_timeout=1),
                                     "updated")
                self.assertOld()

    def test_a_staged_journal_alone_changed_nothing(self):
        self.download()
        self.prepare()   # the launcher never got to start the swap
        self.assertEqual(updates.recover_launcher(self.root, log=lambda t: None, idle_timeout=1), "nothing")
        self.assertFalse(os.path.exists(self.dirs["journal"]))
        self.assertOld()

    def test_rollback_and_forward_again(self):
        self.assertEqual(self.update(), "updated")
        self.assertEqual(updates.swap_launcher(self.root, EXE, log=lambda t: None, source="rollback",
                                               check=answers(OLD), restart=False, idle_timeout=1), "updated")
        self.assertOld()
        self.assertEqual(updates.rollback_version(self.root), NEW, "the newer one kept in turn")
        with mock.patch.object(updates, "_this_version", lambda: OLD):
            self.assertEqual(updates.swap_launcher(self.root, EXE, log=lambda t: None, source="rollback",
                                                   check=answers(NEW), restart=False, idle_timeout=1), "updated")
        self.assertEqual(updates.package_version(self.root), NEW)
        self.assertEqual(updates.rollback_version(self.root), OLD)

    def test_failed_rollback_keeps_the_rollback(self):
        self.assertEqual(self.update(), "updated")
        self.assertEqual(updates.swap_launcher(self.root, EXE, log=lambda t: None, source="rollback", restart=False,
                                               check=lambda cmd, v, **kw: (False, "no"), idle_timeout=1), "restored")
        self.assertEqual(updates.package_version(self.root), NEW)
        self.assertEqual(launcher_items(self.dirs["rollback"]), self.old_items, "still there to go back to")

    def test_one_update_at_a_time(self):
        p = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
        try:
            put(self.dirs["lock"], json.dumps({"pid": p.pid, "op": "swap"}))
            with self.assertRaises(trust.ReleaseError) as e:
                updates.UpdateLock(self.root, "apply").acquire()
            self.assertEqual(e.exception.code, "busy")
        finally:
            p.kill()
            p.wait()
        updates.UpdateLock(self.root, "apply").acquire().release()   # its process gone: taken over
        self.assertFalse(os.path.exists(self.dirs["lock"]))

    def test_a_recorded_process_is_only_the_process_recorded(self):
        p = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
        try:
            time.sleep(0.5)
            now = time.time()
            self.assertTrue(updates.is_process(p.pid, sys.executable, now), "itself, recorded after it started")
            self.assertFalse(updates.is_process(p.pid, sys.executable, now - 3600),
                             "a pid recorded before this process started is another's (reused after a reboot)")
            self.assertFalse(updates.is_process(p.pid, os.path.join(self.tmp, "other.exe"), now),
                             "another program under that pid")
        finally:
            p.kill()
            p.wait()
        self.assertFalse(updates.is_process(p.pid, sys.executable, now), "ended")

    @unittest.skipIf(WIN, "zombies are a POSIX thing")
    def test_a_zombie_is_not_running(self):
        p = subprocess.Popen(["sleep", "30"])
        os.kill(p.pid, 9)
        time.sleep(0.5)   # killed, not reaped
        self.assertFalse(updates.is_process(p.pid))
        p.wait()

    def test_a_stale_helper_record_doesnt_block_recovery(self):
        self.download()
        self.prepare()
        with self.assertRaises(Crash):
            updates.swap_launcher(self.root, EXE, log=lambda t: None, expect_version=NEW, restart=False,
                                  check=mock.Mock(side_effect=Crash()), idle_timeout=1)
        p = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
        try:   # after a reboot the recorded pid belongs to some other process, started later
            j = updates.read_journal(self.root)
            j.update(helper_pid=p.pid, helper_exe=sys.executable, helper_since=time.time() - 3600)
            updates._write_json(self.dirs["journal"], j)
            self.assertEqual(updates.recover_launcher(self.root, log=lambda t: None, idle_timeout=1,
                                                      check=lambda cmd, v, **kw: (False, "")), "restored")
        finally:
            p.kill()
            p.wait()
        self.assertOld()

    def test_a_recovery_that_cant_finish_says_so(self):
        self.download()
        self.prepare()
        with self.assertRaises(Crash):
            updates.swap_launcher(self.root, EXE, log=lambda t: None, expect_version=NEW, restart=False,
                                  check=mock.Mock(side_effect=Crash()), idle_timeout=1)
        game = os.path.join(self.root, "builds", "b1", layout.exe_name())
        started = []
        with mock.patch.object(updates, "_processes", lambda: [(4242, game)]),                 mock.patch.object(updates, "_start_detached", lambda cmd, cwd, env=None: started.append(cmd[1])):
            self.assertEqual(updates.recover_launcher(self.root, log=lambda t: None, idle_timeout=1, restart=True),
                             "busy")
        last = json.load(open(self.dirs["last"]))
        self.assertEqual(last["result"], "busy")
        self.assertIn(layout.exe_name(), last["why"])
        self.assertEqual(started, ["--update-failed"], "the window opens and says why")
        self.assertTrue(os.path.exists(self.dirs["journal"]), "still to be put right next time")

    def test_a_lock_of_a_reused_pid_is_taken_over(self):
        p = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
        try:
            put(self.dirs["lock"], json.dumps({"pid": p.pid, "exe": sys.executable, "since": time.time() - 3600}))
            updates.UpdateLock(self.root, "apply").acquire().release()
        finally:
            p.kill()
            p.wait()
        self.assertFalse(os.path.exists(self.dirs["lock"]))

    def test_the_game_still_running_stops_the_swap(self):
        game = os.path.join(self.root, "builds", "b1", layout.exe_name())
        with mock.patch.object(updates, "_processes", lambda: [(4242, game)]):
            self.assertEqual(self.update(), "busy")
        self.assertOld()
        self.assertEqual(json.load(open(self.dirs["last"]))["result"], "busy")

    def test_running_from_ignores_the_helper_and_other_folders(self):
        helper = os.path.join(self.dirs["helper"], WORKER_EXE)
        elsewhere = os.path.join(self.tmp, "Other Game", "game.exe")
        game = os.path.join(self.root, "builds", "b1", layout.exe_name())
        with mock.patch.object(updates, "_processes", lambda: [(1, helper), (2, elsewhere), (3, game)]):
            self.assertEqual(updates.running_from(self.root, ignore=(self.dirs["helper"],)), [(3, game)])


class AppImage(Base):
    """an AppImage installation: the file beside its Cubic.Heroes folder replaced in one rename"""

    def setUp(self):
        super().setUp()
        # the helper is a copy of the running (old) launcher's worker: its own version is OLD -- whatever version this
        # checkout says (an AppImage swap with no journal records the helper's own version as the one replaced)
        helper_version = mock.patch.object(updates, "_this_version", lambda: OLD)
        helper_version.start()
        self.addCleanup(helper_version.stop)
        self.app = os.path.join(self.tmp, "Cubic.Heroes-0.2.0-beta.3-x86_64.AppImage")
        put(self.app, self.image(OLD))
        self.staged = os.path.join(self.tmp, ".Cubic.Heroes-0.2.0-beta.3-x86_64.AppImage.new")

    def image(self, version):
        head = bytearray(program("elf", tag=f"launcher {version}".encode()))
        head[8:11] = b"AI\x02"
        return bytes(head)

    def swap(self, check, source="staged", **kw):
        return updates.swap_appimage(self.app, self.root, log=lambda t: None, expect_version=None if source ==
                                     "rollback" else NEW, check=check, source=source, restart=False,
                                     idle_timeout=1, **kw)

    def test_replaced_kept_and_rolled_back(self):
        put(self.staged, self.image(NEW))
        self.assertEqual(self.swap(answers(NEW)), "updated")
        self.assertIn(b"launcher " + NEW.encode(), get(self.app))
        self.assertFalse(os.path.exists(self.staged))
        self.assertIn(b"launcher " + OLD.encode(), get(os.path.join(self.dirs["rollback"], updates.APPIMAGE_KEPT)))
        self.assertEqual(updates.rollback_version(self.root), OLD)
        self.assertUntouched()
        self.assertEqual(self.swap(answers(OLD), source="rollback"), "updated")
        self.assertIn(b"launcher " + OLD.encode(), get(self.app))
        self.assertIn(b"launcher " + NEW.encode(), get(os.path.join(self.dirs["rollback"], updates.APPIMAGE_KEPT)))

    def test_put_back_when_it_does_not_answer(self):
        put(self.staged, self.image(NEW))
        self.assertEqual(self.swap(lambda cmd, v, **kw: (False, "no")), "restored")
        self.assertIn(b"launcher " + OLD.encode(), get(self.app))
        self.assertFalse(os.path.exists(self.dirs["journal"]))
        self.assertUntouched()

    def test_crash_recovered(self):
        put(self.staged, self.image(NEW))
        with self.assertRaises(Crash):
            self.swap(mock.Mock(side_effect=Crash()))
        self.assertEqual(updates.recover_launcher(self.root, log=lambda t: None, idle_timeout=1,
                                                  check=lambda cmd, v, **kw: (False, "")), "restored")
        self.assertIn(b"launcher " + OLD.encode(), get(self.app))
        self.assertUntouched()

    def test_a_folder_that_cant_be_written_is_refused_first(self):
        pkg = os.path.join(self.tmp, "new.AppImage")
        put(pkg, self.image(NEW))
        with mock.patch.object(updates.os, "access", lambda p, m: False):
            with self.assertRaises(trust.ReleaseError) as e:
                updates.stage_appimage(pkg, self.app)
        self.assertEqual(e.exception.code, "root_not_writable")
        self.assertFalse(os.path.exists(self.staged), "nothing put anywhere else")

    def test_started_without_fuse_the_new_one_is_too(self):
        put(self.staged, self.image(NEW))
        old = os.path.join(self.tmp, "tmp", "appimage_extracted_0123abcd")
        put(os.path.join(old, "CubicHeroes"), b"the old extracted copy")
        envs, started = [], []

        def check(cmd, v, env=None, **kw):
            envs.append((env or {}).get("APPIMAGE_EXTRACT_AND_RUN"))
            return True, ""

        def start(cmd, cwd, env=None):
            started.append((cmd[1], (env or {}).get("APPIMAGE_EXTRACT_AND_RUN")))
            if cmd[1] == "--after-update":
                put(env[updates.HEALTH_ENV], NEW)
            return mock.Mock(poll=lambda: None)
        with mock.patch.object(updates, "_start_detached", start):
            self.assertEqual(updates.swap_appimage(self.app, self.root, log=lambda t: None, expect_version=NEW,
                                                   check=check, idle_timeout=1, extracted=True, old_appdir=old,
                                                   health_timeout=5), "updated")
        self.assertEqual(envs, ["1"], "the check started it without FUSE")
        self.assertEqual(started, [("--after-update", "1")], "and its window")
        self.assertFalse(os.path.exists(old), "the old version's extracted copy removed once the update is kept")

    @unittest.skipIf(WIN, "symbolic links need privileges on Windows")
    def test_a_symlinked_appimage_is_replaced_where_it_really_is(self):
        link = os.path.join(self.tmp, "link.AppImage")
        os.symlink(self.app, link)
        put(self.staged, self.image(NEW))
        self.assertEqual(updates.swap_appimage(link, self.root, log=lambda t: None, expect_version=NEW,
                                               check=answers(NEW), restart=False, idle_timeout=1), "updated")
        self.assertTrue(os.path.islink(link), "the link stays a link")
        self.assertIn(b"launcher " + NEW.encode(), get(self.app), "the real file replaced")

    @unittest.skipIf(WIN, "an AppImage is a Linux program: this system's check refuses it")
    def test_staged_beside_it_after_the_checks(self):
        pkg = os.path.join(self.tmp, "new.AppImage")
        put(pkg, self.image(NEW))
        asset = {"name": "x.AppImage", "size": os.path.getsize(pkg), "sha256": hashlib.sha256(get(pkg)).hexdigest()}
        self.assertEqual(updates.stage_appimage(pkg, self.app, asset), self.staged)
        self.assertTrue(os.access(self.staged, os.X_OK))
        put(pkg, program("elf", tag=b"no mark"))
        with self.assertRaises(trust.ReleaseError) as e:
            updates.stage_appimage(pkg, self.app)
        self.assertEqual(e.exception.code, "platform")


class FrozenChildEnv(unittest.TestCase):
    """what the frozen worker starts outside itself (a new launcher, the game) gets the library path the worker was
    started with, not the one PyInstaller gave the worker (0.2.0-beta.5: an AppImage's `#!/bin/sh` AppRun under the
    system's bash loaded the worker's bundled readline and failed)"""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="childenv-", dir=os.environ.get("TMPDIR"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.internal = os.path.join(self.tmp, "worker", "_internal")
        self.system = os.path.join(self.tmp, "usr", "lib")
        os.makedirs(self.internal)
        os.makedirs(self.system)
        # the frozen Linux worker (tools/hostenv.py): PyInstaller's _internal first, the player's value in _ORIG
        for name, value in (("frozen", True), ("_MEIPASS", self.internal), ("platform", "linux")):
            p = mock.patch.object(updates.hostenv.sys, name, value, create=True)
            p.start()
            self.addCleanup(p.stop)

    def test_the_original_put_back(self):
        env = updates._clean_env({"LD_LIBRARY_PATH": self.internal + os.pathsep + self.system,
                                  "LD_LIBRARY_PATH_ORIG": self.system, "HOME": self.tmp})
        self.assertEqual(env["LD_LIBRARY_PATH"], self.system)
        self.assertNotIn("LD_LIBRARY_PATH_ORIG", env)
        self.assertEqual(env["HOME"], self.tmp)

    def test_no_original_none(self):
        env = updates._clean_env({"LD_LIBRARY_PATH": self.internal})
        self.assertNotIn("LD_LIBRARY_PATH", env)

    def test_not_frozen_left_as_it_is(self):
        with mock.patch.object(updates.hostenv.sys, "frozen", False):
            env = updates._clean_env({"LD_LIBRARY_PATH": self.internal, "LD_LIBRARY_PATH_ORIG": self.system})
        self.assertEqual((env["LD_LIBRARY_PATH"], env["LD_LIBRARY_PATH_ORIG"]), (self.internal, self.system))

    def test_with_an_appimage_too(self):
        appdir = os.path.join(self.tmp, "mnt")
        env = updates._clean_env({"APPDIR": appdir, "APPIMAGE": appdir + ".AppImage",
                                  "LD_LIBRARY_PATH": os.pathsep.join([self.internal, os.path.join(appdir, "lib")]),
                                  "PATH": os.pathsep.join([os.path.join(appdir, "bin"), self.system])})
        self.assertNotIn("LD_LIBRARY_PATH", env)
        self.assertEqual(env["PATH"], self.system)
        self.assertNotIn("APPDIR", env)

    @unittest.skipIf(os.name == "nt", "a #!/bin/sh AppRun: POSIX")
    def test_a_shell_apprun_doesnt_see_the_workers_libraries(self):
        run = os.path.join(self.tmp, "AppRun")
        with open(run, "w") as f:
            f.write('#!/bin/sh\nprintf "%s" "${LD_LIBRARY_PATH-unset}"\n')
        os.chmod(run, 0o755)
        base = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH_ORIG"}
        env = updates._clean_env(dict(base, LD_LIBRARY_PATH=self.internal))
        out = subprocess.run([run], env=env, capture_output=True, text=True, timeout=30).stdout
        self.assertNotIn(self.internal, out)


class WorkerCommands(Base):
    """the worker's commands, from this checkout"""

    def test_started_without_standard_handles(self):
        # a windowed worker started detached with no stdout or stderr (the launcher's recovery start, before beta.4's
        # fix): it must run -- never fail on its first output (PyInstaller shows that as an error dialog)
        import worker
        saved = sys.stdout, sys.stderr
        try:
            sys.stdout = sys.stderr = None
            code = worker.main(["version"])
        finally:
            sys.stdout, sys.stderr = saved
        self.assertEqual(code, 0)

    def worker(self, *args):
        kf = os.path.join(self.tmp, "keys.json")
        put(kf, json.dumps({"keys": [{"id": self.kid, "ed25519": base64.b64encode(self.keys[0][1]).decode()}]}))
        p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "worker.py"), *args, "--trusted-keys", kf],
                           capture_output=True, text=True, timeout=120)
        return json.loads(p.stdout.splitlines()[-1])

    def test_apply_refuses_and_starts_nothing(self):
        self.download(tamper="package")
        r = self.worker("apply-update", "--install", self.root, "--launcher", self.root, "--version", NEW,
                        "--installed", OLD, "--no-restart")
        self.assertEqual((r["ok"], r["code"]), (False, "bad_download"))
        self.assertFalse(os.path.exists(self.dirs["helper"]))
        self.assertFalse(os.path.exists(self.dirs["lock"]))
        self.assertOld()

    def test_apply_refused_while_an_update_is_unfinished(self):
        self.download()
        put(self.dirs["journal"], json.dumps({"state": "swapping", "helper_pid": None}))
        r = self.worker("apply-update", "--install", self.root, "--launcher", self.root, "--version", NEW,
                        "--installed", OLD, "--no-restart")
        self.assertEqual((r["ok"], r["code"]), (False, "busy"))

    def test_state(self):
        self.download()
        r = self.worker("launcher-state", "--install", self.root, "--launcher", self.root, "--installed", OLD)
        self.assertEqual(r["data"]["ready"], {"version": NEW})

    def test_rollback_needs_one_kept(self):
        r = self.worker("rollback-launcher", "--install", self.root, "--launcher", self.root, "--no-restart")
        self.assertEqual((r["ok"], r["code"]), (False, "no_rollback"))


if __name__ == "__main__":
    unittest.main()
