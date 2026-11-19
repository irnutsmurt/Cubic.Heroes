#!/usr/bin/env python3
"""Signed releases and updates (M9.3: tools/release_trust.py, tools/updates.py, tools/release.py, the worker's
update commands):
    build/venv/Scripts/python.exe -m unittest tests.test_updates
Ed25519 against RFC 8032's vectors and OpenSSL's command line; a made-up release mirror signed with throwaway keys
made in a temporary folder (never the owner's): the channels, an altered, unsigned or foreign-signed release
refused, the next valid one taken; downloads checked, a damaged one refused, an interrupted one resumed (over a
local HTTP server that drops the connection), too little space refused, offline reported; a launcher package with
a path escaping it, or the install's own items, refused; the launcher's swap -- in its folder, which is also the
install's root (a path with spaces and any characters) -- replacing only the package's items, the install's left
byte for byte, kept when the new one comes up, put back when it does not or when the swap itself fails."""
import base64
import hashlib
import http.server
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
import zipfile
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import install_layout as layout  # noqa: E402
import release_trust as trust  # noqa: E402
import updates  # noqa: E402

OPENSSL = shutil.which("openssl")
PLAT = updates.platform_name()


def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb" if isinstance(data, bytes) else "w") as f:
        f.write(data)


def get(path):
    with open(path, "rb") as f:
        return f.read()


PKG_EXT = ".zip" if os.name == "nt" else ".tar.xz"   # each platform's own launcher package (M11.4)


def archive(path, members, folder=None):
    """a launcher package at `path` (zip or tar.xz by its name): `members` {name: text}, or the files under `folder`
    by their paths relative to its parent"""
    files = dict(members or {})
    if folder:
        for d, _, fs in os.walk(folder):
            for f in fs:
                full = os.path.join(d, f)
                files[os.path.relpath(full, os.path.dirname(folder)).replace(os.sep, "/")] = full
    if path.endswith(".zip"):
        with zipfile.ZipFile(path, "w") as zf:
            for n, v in files.items():
                zf.write(v, n) if folder and v.startswith(folder) else zf.writestr(n, v)
    else:
        import io
        import tarfile
        with tarfile.open(path, "w:xz") as t:
            for n, v in files.items():
                if folder and v.startswith(folder):
                    t.add(v, n)
                else:
                    data = v.encode() if isinstance(v, str) else v
                    ti = tarfile.TarInfo(n)
                    ti.size, ti.mode = len(data), 0o644
                    t.addfile(ti, io.BytesIO(data))
    return path


class Ed25519(unittest.TestCase):
    VECTORS = [   # RFC 8032 section 7.1, tests 1 and 2
        ("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f05"
         "95bbe24655141438e7a100b"),
        ("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb43"
         "02aeeb00d291612bb0c00")]

    def test_rfc8032(self):
        for pk, msg, sig in self.VECTORS:
            pk, msg, sig = bytes.fromhex(pk), bytes.fromhex(msg), bytes.fromhex(sig)
            self.assertTrue(trust.ed25519_verify(pk, msg, sig))
            bad = bytearray(sig)
            bad[7] ^= 0x10
            self.assertFalse(trust.ed25519_verify(pk, msg, bytes(bad)))
            self.assertFalse(trust.ed25519_verify(pk, msg + b"!", sig))
            self.assertFalse(trust.ed25519_verify(pk, msg, sig[:63]))


@unittest.skipUnless(OPENSSL, "OpenSSL's command line")
class ManifestKinds(unittest.TestCase):
    """the review's W-5: a download's kind is a known string that its platform and its file name agree with"""

    def manifest(self, name, kind, platform):
        a = {"name": name, "platform": platform, "size": 1, "sha256": "0" * 64}
        if kind is not ...:
            a["kind"] = kind
        return {"format": 1, "version": "9.9.9", "channel": "prerelease", "min_launcher": None, "notes": "",
                "key_id": "x", "assets": [a]}

    def test_good_kinds(self):
        for name, kind, plat in (("C-1-windows-x86_64.zip", "zip", "windows-x86_64"),
                                 ("C-1-linux-x86_64.tar.xz", "tar.xz", "linux-x86_64"),
                                 ("C-1-x86_64.AppImage", "appimage", "linux-x86_64"),
                                 ("C-1-linux-x86_64.tar.xz", ..., "linux-x86_64")):
            trust.check_manifest(self.manifest(name, kind, plat))

    def test_bad_kinds_refused(self):
        for name, kind, plat in (("C-1-linux-x86_64.tar.xz", "zip", "linux-x86_64"),      # name and kind disagree
                                 ("C-1-x86_64.AppImage", "tar.xz", "linux-x86_64"),
                                 ("C-1-windows-x86_64.zip", ["zip"], "windows-x86_64"),   # not a string
                                 ("C-1-windows-x86_64.zip", 3, "windows-x86_64"),
                                 ("C-1-linux-x86_64.tar.xz", "tar.xz", "windows-x86_64")):  # another system's kind
            with self.assertRaises(trust.ReleaseError, msg=(name, kind, plat)) as e:
                trust.check_manifest(self.manifest(name, kind, plat))
            self.assertEqual(e.exception.code, "bad_manifest")


class Releases(unittest.TestCase):
    """a mirror: releases.json (GitHub's API's shape) and each release's files in its own folder"""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="release-test-")
        self.mirror = os.path.join(self.tmp, "mirror")
        os.makedirs(self.mirror)
        self.key, self.kid, self.keys = self.make_key("k1")
        self.other, self.other_id, _ = self.make_key("k2")
        self.list = []

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def make_key(self, name):
        k = os.path.join(self.tmp, name + ".pem")
        subprocess.check_call([OPENSSL, "genpkey", "-algorithm", "ed25519", "-out", k], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
        der = subprocess.check_output([OPENSSL, "pkey", "-in", k, "-pubout", "-outform", "DER"],
                                      stderr=subprocess.DEVNULL)
        pk = trust.public_key_from(der)
        return k, trust.key_id(pk), [(trust.key_id(pk), pk)]

    def sign(self, key, path):
        subprocess.check_call([OPENSSL, "pkeyutl", "-sign", "-inkey", key, "-rawin", "-in", path, "-out",
                               path + ".sig"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def release(self, version, channel="stable", prerelease=False, payload=b"launcher bytes", key=None, kid=None,
                sign=True, tamper=False):
        d = os.path.join(self.mirror, version)
        os.makedirs(d)
        name = f"CubicHeroes-{PLAT}-{version}{PKG_EXT}"
        z = archive(os.path.join(d, name), {"CubicHeroes/launcher.txt": payload.decode() + version})
        data = get(z)
        m = {"format": 1, "version": version, "channel": channel, "min_launcher": None, "notes": f"notes {version}",
             "key_id": kid or self.kid, "assets": [{"name": name, "platform": PLAT, "size": len(data),
                                                     "sha256": hashlib.sha256(data).hexdigest()}]}
        mp = os.path.join(d, "release.json")
        put(mp, json.dumps(m, indent=1))
        assets = [{"name": name, "browser_download_url": z}, {"name": "release.json", "browser_download_url": mp}]
        if sign:
            self.sign(key or self.key, mp)
            assets.append({"name": "release.json.sig", "browser_download_url": mp + ".sig"})
        if tamper:
            put(mp, get(mp).replace(b"notes", b"NOTES"))
        self.list.insert(0, {"tag_name": "v" + version, "prerelease": prerelease, "draft": False, "assets": assets})
        put(os.path.join(self.mirror, "releases.json"), json.dumps(self.list))
        return z, m

    def test_channels(self):
        self.release("0.2.0")
        self.release("0.3.0-beta.1", channel="prerelease", prerelease=True)
        r = updates.check("stable", "0.1.0", self.mirror, self.keys)
        self.assertEqual((r["version"], r["newer"], r["notes"]), ("0.2.0", True, "notes 0.2.0"))
        r = updates.check("prerelease", "0.1.0", self.mirror, self.keys)
        self.assertEqual(r["version"], "0.3.0-beta.1")
        r = updates.check("stable", "0.2.0", self.mirror, self.keys)
        self.assertFalse(r["newer"], "the installed version is the newest")

    def test_refused_releases_skipped(self):
        self.release("0.2.0")
        self.release("0.2.1", tamper=True)
        self.release("0.2.2", sign=False)
        self.release("0.2.3", key=self.other, kid=self.other_id)
        r = updates.check("stable", "0.1.0", self.mirror, self.keys)
        self.assertEqual(r["version"], "0.2.0", "the newest release that verifies")
        why = {x["tag"]: x["code"] for x in r["refused"]}
        self.assertEqual(why, {"v0.2.1": "bad_signature", "v0.2.2": "unsigned", "v0.2.3": "untrusted_key"})

    def test_a_signed_key_id_must_match(self):
        self.release("0.2.0", kid=self.other_id)   # signed by k1, naming k2
        r = updates.check("stable", "0.1.0", self.mirror, self.keys + [(self.other_id, b"\x01" * 32)])
        self.assertFalse(r["available"])
        self.assertEqual(r["refused"][0]["code"], "bad_signature")

    def test_no_trusted_keys(self):
        self.release("0.2.0")
        with self.assertRaises(trust.ReleaseError) as e:
            updates.check("stable", "0.1.0", self.mirror, [])
        self.assertEqual(e.exception.code, "no_trusted_keys")

    def test_setup_app_offers_only_verified_releases(self):
        import self_update
        # a launcher trusting no key: nothing is offered, and nothing is asked of the network
        real, real_keys = updates.releases, updates.trust.KEYS_FILE
        empty = os.path.join(self.tmp, "no-keys.json")
        put(empty, json.dumps({"keys": []}))
        updates.releases = lambda source=None: self.fail("the release list was requested")
        updates.trust.KEYS_FILE = empty
        try:
            self.assertIsNone(self_update.latest())
        finally:
            updates.releases, updates.trust.KEYS_FILE = real, real_keys
        with self.assertRaises(OSError):   # a download not from a verified release: never unpacked
            self_update._checked(os.path.join(self.tmp, "x.zip"), "https://example.invalid/x.zip")

    def test_download_checked(self):
        z, m = self.release("0.2.0")
        r = updates.check("stable", "0.1.0", self.mirror, self.keys)
        dest = os.path.join(self.tmp, "dl")
        p = updates.download(r["assets"][0], r["urls"][r["assets"][0]["name"]], dest)
        self.assertEqual(get(p), get(z))
        # the mirror's file altered after signing: refused, nothing kept
        os.remove(p)
        put(z, get(z)[:-1] + b"X")
        with self.assertRaises(trust.ReleaseError) as e:
            updates.download(r["assets"][0], z, dest)
        self.assertEqual(e.exception.code, "bad_download")
        self.assertEqual(os.listdir(dest), [])

    def test_resume_from_part(self):
        z, m = self.release("0.2.0", payload=os.urandom(3 << 20).hex().encode())
        r = updates.check("stable", "0.1.0", self.mirror, self.keys)
        a = r["assets"][0]
        dest = os.path.join(self.tmp, "dl")
        put(os.path.join(dest, a["name"] + ".part"), get(z)[:12345])   # an earlier attempt stopped here
        p = updates.download(a, z, dest)
        self.assertEqual(hashlib.sha256(get(p)).hexdigest(), a["sha256"])

    def test_too_little_space(self):
        self.release("0.2.0")
        r = updates.check("stable", "0.1.0", self.mirror, self.keys)
        real = updates.shutil.disk_usage
        updates.shutil.disk_usage = lambda p: type("U", (), {"free": 1024})()
        try:
            with self.assertRaises(trust.ReleaseError) as e:
                updates.download(r["assets"][0], r["urls"][r["assets"][0]["name"]], os.path.join(self.tmp, "dl"))
            self.assertEqual(e.exception.code, "disk_space")
        finally:
            updates.shutil.disk_usage = real

    def test_offline(self):
        with self.assertRaises(trust.ReleaseError) as e:
            updates.check("stable", "0.1.0", "http://127.0.0.1:9/releases", self.keys)   # nothing listens there
        self.assertEqual(e.exception.code, "offline")

    def test_worker_check_and_download(self):
        z, m = self.release("0.2.0")
        kf = os.path.join(self.tmp, "keys.json")
        put(kf, json.dumps({"keys": [{"id": self.kid, "ed25519": base64.b64encode(self.keys[0][1]).decode()}]}))
        w = [sys.executable, os.path.join(ROOT, "tools", "worker.py")]
        p = subprocess.run(w + ["check-update", "--source", self.mirror, "--trusted-keys", kf, "--installed",
                                "0.1.0"], capture_output=True, text=True, timeout=60)
        res = json.loads(p.stdout.splitlines()[-1])
        self.assertEqual((res["data"]["version"], res["data"]["newer"]), ("0.2.0", True))
        inst = os.path.join(self.tmp, "install")
        p = subprocess.run(w + ["download-update", "--source", self.mirror, "--trusted-keys", kf, "--installed",
                                "0.1.0", "--install", inst], capture_output=True, text=True, timeout=60)
        res = json.loads(p.stdout.splitlines()[-1])
        self.assertTrue(res["ok"], res)
        self.assertEqual(get(res["data"]["path"]), get(z))
        empty = os.path.join(self.tmp, "no-keys.json")   # a launcher trusting no key: every update refused
        put(empty, json.dumps({"keys": []}))
        p = subprocess.run(w + ["check-update", "--source", self.mirror, "--trusted-keys", empty, "--installed", "0.1.0"],
                           capture_output=True, text=True, timeout=60)
        self.assertEqual(json.loads(p.stdout.splitlines()[-1])["code"], "no_trusted_keys")

    def test_worker_check_offline_is_not_an_error(self):
        kf = os.path.join(self.tmp, "keys.json")
        put(kf, json.dumps({"keys": [{"id": self.kid, "ed25519": base64.b64encode(self.keys[0][1]).decode()}]}))
        p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "worker.py"), "check-update", "--source",
                            "http://127.0.0.1:9/releases", "--trusted-keys", kf], capture_output=True, text=True,
                           timeout=60)
        res = json.loads(p.stdout.splitlines()[-1])
        self.assertEqual((p.returncode, res["ok"], res["data"]["offline"]), (0, True, True))

    def test_stage_refuses_an_escaping_member(self):
        bad = archive(os.path.join(self.tmp, "bad" + PKG_EXT), {"../escaped.txt": "x"})
        with self.assertRaises(trust.ReleaseError):
            updates.stage_launcher(bad, os.path.join(self.tmp, "launcher"))
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "escaped.txt")))

    @unittest.skipIf(os.name == "nt", "Linux's package")
    def test_linux_tarball_keeps_exec_bits_and_refuses_links_out(self):
        import io
        import tarfile
        pkg = os.path.join(self.tmp, "pkg.tar.xz")
        with tarfile.open(pkg, "w:xz") as t:
            for n, mode in (("Cubic.Heroes/CubicHeroes", 0o755), ("Cubic.Heroes/README.txt", 0o644)):
                ti = tarfile.TarInfo(n)
                ti.size, ti.mode = 2, mode
                t.addfile(ti, io.BytesIO(b"x\n"))
        new = updates.stage_launcher(pkg, os.path.join(self.tmp, "launcher"))
        self.assertTrue(os.access(os.path.join(new, "CubicHeroes"), os.X_OK))
        self.assertFalse(os.access(os.path.join(new, "README.txt"), os.X_OK))
        evil = os.path.join(self.tmp, "evil.tar.xz")
        with tarfile.open(evil, "w:xz") as t:
            ti = tarfile.TarInfo("Cubic.Heroes/lib")
            ti.type, ti.linkname = tarfile.SYMTYPE, "/etc"
            t.addfile(ti)
        with self.assertRaises(trust.ReleaseError):
            updates.stage_launcher(evil, os.path.join(self.tmp, "launcher2"))
        for wrong in ("x.zip", "x.AppImage"):   # not Linux's tarball: refused, nothing unpacked
            put(os.path.join(self.tmp, wrong), b"PK")
            with self.assertRaises(trust.ReleaseError):
                updates.stage_launcher(os.path.join(self.tmp, wrong), os.path.join(self.tmp, "launcher3"))

    def test_pick_asset_by_kind_and_platform(self):
        assets = [{"name": "Cubic.Heroes-1.0.0-x86_64.AppImage", "platform": "linux-x86_64"},
                  {"name": "Cubic.Heroes-1.0.0-linux-x86_64.tar.xz", "platform": "linux-x86_64"}]
        self.assertEqual(updates.pick_asset(assets, "tar.xz")["name"], assets[1]["name"])
        self.assertEqual(updates.pick_asset(assets, "appimage")["name"], assets[0]["name"])
        self.assertIsNone(updates.pick_asset(assets, "zip"))
        self.assertIn(updates.platform_name(), ("windows-x86_64", "linux-x86_64"))   # this test's machines
        self.assertEqual(updates.installed_kind(), "zip" if os.name == "nt" else
                         ("appimage" if os.environ.get("APPIMAGE") else "tar.xz"))

    def test_stage_one_top_folder(self):
        z, _ = self.release("0.2.0")
        new = updates.stage_launcher(z, os.path.join(self.tmp, "launcher"))
        self.assertEqual(get(os.path.join(new, "launcher.txt")), b"launcher bytes0.2.0")


class FlakyServer(http.server.BaseHTTPRequestHandler):
    """serves one file with Range support; the first full request is cut off after `cut` bytes"""
    data, cut, cuts = b"", 0, [0]

    def do_GET(self):
        rng = self.headers.get("Range")
        start = int(rng.split("=")[1].split("-")[0]) if rng else 0
        body = self.data[start:]
        self.send_response(206 if rng else 200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if not rng and self.cuts[0] == 0:
            self.cuts[0] += 1
            self.wfile.write(body[:self.cut])
            self.wfile.flush()
            self.connection.shutdown(2)
            return
        self.wfile.write(body)

    def log_message(self, *a):
        pass


class HttpResume(unittest.TestCase):
    def test_interrupted_then_resumed(self):
        data = os.urandom(5 << 20)
        FlakyServer.data, FlakyServer.cut, FlakyServer.cuts = data, 1 << 20, [0]
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FlakyServer)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        tmp = tempfile.mkdtemp(prefix="dl-test-")
        try:
            url = f"http://127.0.0.1:{srv.server_address[1]}/f.zip"
            asset = {"name": "f.zip", "platform": PLAT, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            with self.assertRaises(trust.ReleaseError) as e:
                updates.download(asset, url, tmp)
            self.assertIn(e.exception.code, ("interrupted", "bad_download"))
            if e.exception.code == "interrupted":
                self.assertGreater(os.path.getsize(os.path.join(tmp, "f.zip.part")), 0, "the part kept")
            p = updates.download(asset, url, tmp)   # resumed (or begun again) and checked
            self.assertEqual(get(p), data)
        finally:
            srv.shutdown()
            shutil.rmtree(tmp, ignore_errors=True)


EXE = "CubicHeroes.exe" if os.name == "nt" else "CubicHeroes"
WORKER_EXE = "DotHeroes-Setup.exe" if os.name == "nt" else "DotHeroes-Setup"


def package(folder, version, start=None):
    """a launcher package's items (install_layout.LAUNCHER_PACKAGE), each saying its version"""
    for n in layout.launcher_package():
        if n == EXE:
            put(os.path.join(folder, n), f"launcher {version}")
        elif n in ("worker",):
            put(os.path.join(folder, n, WORKER_EXE), f"worker {version}")
            put(os.path.join(folder, n, "_internal", "python.dll"), f"python {version}")
        elif n in ("licenses", "lib"):
            put(os.path.join(folder, n, "SDL2.txt"), f"licence {version}")
        else:
            put(os.path.join(folder, n), f"{n} {version}")
    if start:
        put(os.path.join(folder, "start.bat"), start)


def portable_root(root):
    """the launcher's folder as the install's root: the package, the install's items, the player's own file"""
    package(root, "old")
    put(os.path.join(root, "active.json"), '{"layout": 2, "active": "b1", "previous": [], "retention": 1}')
    put(os.path.join(root, "builds", "b1", "dotheroes.exe"), b"the game")
    put(os.path.join(root, "data", "gamedata", "dev_hdd0", "SAVE.BIN"), b"the player's save")
    put(os.path.join(root, "data", "cache", "objects", "a.o"), b"object")
    put(os.path.join(root, "config", "dotheroes.cfg"), "DOTHEROES_FPS=60\n")
    put(os.path.join(root, "logs", "game.log"), "a log")
    put(os.path.join(root, "backups", "saves", "x", "gamedata", "SAVE.BIN"), b"a backup")
    put(os.path.join(root, "downloads", "0.2.0", "CubicHeroes-0.2.0.zip"), b"the download")
    put(os.path.join(root, layout.ROOT_MARKER), "root")
    put(os.path.join(root, "my notes.txt"), "the player's")
    layout.make_link(os.path.join(root, "game"), os.path.join(root, "builds", "b1"))


def snapshot(root, names=layout.INSTALL_ITEMS + (layout.ROOT_MARKER, "my notes.txt")):
    """every file of the install's items (and the player's), as bytes, never through a link; links as their target
    (the launcher update's own folders in downloads/ left out: they are the launcher's)"""
    out = {}
    for n in names:
        p = os.path.join(root, n)
        if layout.is_link(p):
            out[n] = ("link", os.path.normcase(os.path.realpath(p)))
        elif os.path.isfile(p):
            out[n] = get(p)
        elif os.path.isdir(p):
            for d, dirs, fs in os.walk(p):
                dirs[:] = [x for x in dirs if not layout.is_link(os.path.join(d, x)) and not (
                    os.path.basename(d) == "downloads" and x.startswith("launcher-"))]
                for f in fs:
                    q = os.path.join(d, f)
                    out[os.path.relpath(q, root)] = (get(q), os.path.getmtime(q))
    return out


class FakeLauncher:
    """the new launcher, started: it writes the health file (or not) as a real one would"""

    def __init__(self, folder, healthy):
        self.healthy, self.folder, self.started = healthy, folder, []

    def __call__(self, cmd, cwd):
        self.started.append(cmd[1:])
        if cmd[1] == "--after-update" and self.healthy:
            put(os.path.join(self.folder, updates.HEALTH_FILE), "ok")
        return mock.Mock(poll=lambda: 0 if not self.healthy else None)


class PortableSwap(unittest.TestCase):
    """the launcher's update in its folder, which is also the install's root: only the package's items replaced"""

    def setUp(self):
        self.tmpd = tempfile.TemporaryDirectory(prefix="swap-test-")
        self.root = os.path.join(self.tmpd.name, "Cubic Heroes ünïcode 測試")   # spaces and any characters
        portable_root(self.root)
        self.before = snapshot(self.root)
        self.dirs = updates.update_dirs(self.root)
        package(self.dirs["staged"], "new")

    def tearDown(self):
        layout.remove_link(os.path.join(self.root, "game"))
        self.tmpd.cleanup()

    def swap(self, healthy):
        fake = FakeLauncher(self.root, healthy)
        with mock.patch.object(updates, "_start_detached", fake):
            return updates.swap_launcher(self.root, EXE, log=lambda t: None, health_timeout=2), fake

    def test_swap_replaces_only_the_package(self):
        res, fake = self.swap(True)
        self.assertEqual(res, "updated")
        self.assertEqual(fake.started, [["--after-update"]])
        self.assertEqual(get(os.path.join(self.root, EXE)), b"launcher new")
        self.assertEqual(get(os.path.join(self.root, "worker", WORKER_EXE)), b"worker new")
        self.assertEqual(snapshot(self.root), self.before, "the install's items untouched, byte for byte")
        self.assertTrue(layout.is_link(os.path.join(self.root, "game")))
        for gone in ("previous", "staged"):   # the previous items kept only until the new launcher is up
            self.assertFalse(os.path.exists(self.dirs[gone]), gone)

    def test_put_back_when_the_new_one_does_not_start(self):
        res, fake = self.swap(False)
        self.assertEqual(res, "restored")
        self.assertEqual(fake.started, [["--after-update"], ["--update-failed"]])
        self.assertEqual(get(os.path.join(self.root, EXE)), b"launcher old", "the old launcher back")
        self.assertEqual(get(os.path.join(self.root, "worker", WORKER_EXE)), b"worker old")
        self.assertEqual(get(os.path.join(self.dirs["failed"], EXE)), b"launcher new", "the new one kept aside")
        self.assertEqual(snapshot(self.root), self.before)

    def test_put_back_when_the_swap_itself_fails(self):
        real = updates._replace

        def locked(src, dst, tries=50):   # the new worker can't be moved in (a file held)
            if os.path.normcase(src) == os.path.normcase(os.path.join(self.dirs["staged"], "worker")):
                raise PermissionError(13, "in use")
            return real(src, dst, tries)
        with mock.patch.object(updates, "_replace", locked):
            res, fake = self.swap(True)
        self.assertEqual(res, "restored")
        self.assertEqual(fake.started, [["--update-failed"]])
        for n in layout.launcher_package():
            self.assertTrue(os.path.lexists(os.path.join(self.root, n)), n)
        self.assertEqual(get(os.path.join(self.root, EXE)), b"launcher old")
        self.assertEqual(get(os.path.join(self.root, "worker", WORKER_EXE)), b"worker old")
        self.assertEqual(snapshot(self.root), self.before)

    def test_nothing_staged(self):
        layout.rmtree(self.dirs["staged"])
        res, fake = self.swap(True)
        self.assertEqual((res, fake.started), ("restored", [["--update-failed"]]))
        self.assertEqual(get(os.path.join(self.root, EXE)), b"launcher old")

    def test_stage_into_the_root(self):
        layout.rmtree(self.dirs["staged"])
        src = os.path.join(self.tmpd.name, "pkgsrc", "Cubic.Heroes-0.3.0")
        package(src, "0.3.0")
        z = archive(os.path.join(self.tmpd.name, "pkg" + PKG_EXT), None, folder=src)
        staged = updates.stage_launcher(z, self.root)
        self.assertEqual(os.path.normcase(staged), os.path.normcase(self.dirs["staged"]))
        self.assertEqual(sorted(os.listdir(staged)), sorted(layout.launcher_package()))
        self.assertEqual(get(os.path.join(staged, EXE)), b"launcher 0.3.0")
        self.assertEqual(snapshot(self.root), self.before)

    def test_stage_refuses_the_installs_items(self):
        z = archive(os.path.join(self.tmpd.name, "bad" + PKG_EXT), {EXE: "x", "data/gamedata/SAVE.BIN": "overwritten"})
        with self.assertRaises(trust.ReleaseError) as e:
            updates.stage_launcher(z, self.root)
        self.assertEqual(e.exception.code, "bad_download")
        self.assertFalse(os.path.exists(self.dirs["staged"]))
        self.assertEqual(snapshot(self.root), self.before)

    def test_helper_copy_inside_the_root(self):
        app = os.path.join(self.root, "worker")
        put(os.path.join(app, "tools", "worker.py"), "tool")
        helper = updates.stage_helper(self.root, app)
        self.assertEqual(os.path.normcase(os.path.dirname(helper)), os.path.normcase(self.dirs["helper"]))
        self.assertEqual(get(helper), b"worker old")
        self.assertTrue(os.path.exists(os.path.join(self.dirs["helper"], "_internal", "python.dll")))


@unittest.skipUnless(os.name == "nt", "the stand-in launchers are batch files")
class LauncherSwap(unittest.TestCase):
    """the swap with real processes: a stand-in launcher (a batch file) writes the health file, or fails"""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="swap-test-")
        self.d = os.path.join(self.tmp, "launcher")
        self.staged = updates.update_dirs(self.d)["staged"]

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def launcher(self, folder, version, healthy=True):
        package(folder, version, "@echo off\r\n" + (f"echo %1 > {updates.HEALTH_FILE}\r\n" if healthy
                                                    else "exit /b 1\r\n"))

    def test_kept_when_it_comes_up(self):
        self.launcher(self.d, "old")
        put(os.path.join(self.d, "my notes.txt"), "kept")
        self.launcher(self.staged, "new")
        self.assertEqual(updates.swap_launcher(self.d, "start.bat", log=lambda t: None, health_timeout=20), "updated")
        self.assertEqual(get(os.path.join(self.d, EXE)), b"launcher new")
        self.assertEqual(get(os.path.join(self.d, "my notes.txt")), b"kept", "anything else in the folder stays")

    def test_put_back_when_it_does_not(self):
        self.launcher(self.d, "old")
        self.launcher(self.staged, "new", healthy=False)
        self.assertEqual(updates.swap_launcher(self.d, "start.bat", log=lambda t: None, health_timeout=5), "restored")
        self.assertEqual(get(os.path.join(self.d, EXE)), b"launcher old")
        self.assertEqual(get(os.path.join(updates.update_dirs(self.d)["failed"], EXE)), b"launcher new")


class Manifest(unittest.TestCase):
    def test_rules(self):
        good = {"format": 1, "version": "0.2.0", "channel": "stable", "notes": "", "key_id": "x",
                "assets": [{"name": "a.zip", "platform": PLAT, "size": 1, "sha256": "0" * 64}]}
        trust.check_manifest(dict(good))
        for bad in ({"format": 2}, {"version": "two"}, {"channel": "nightly"}, {"assets": []},
                    {"assets": [{"name": "../a.zip", "platform": PLAT, "size": 1, "sha256": "0" * 64}]},
                    {"assets": [{"name": "a.zip", "platform": "mac", "size": 1, "sha256": "0" * 64}]},
                    {"assets": [{"name": "a.zip", "platform": PLAT, "size": 1, "sha256": "z"}]},
                    # M11.4: a kind that is no kind, or of the other system
                    {"assets": [{"name": "a.tar.xz", "platform": "linux-x86_64", "kind": "deb", "size": 1,
                                 "sha256": "0" * 64}]},
                    {"assets": [{"name": "a.zip", "platform": "linux-x86_64", "kind": "zip", "size": 1,
                                 "sha256": "0" * 64}]},
                    {"assets": [{"name": "a.AppImage", "platform": "windows-x86_64", "kind": "appimage", "size": 1,
                                 "sha256": "0" * 64}]}):
            with self.assertRaises(trust.ReleaseError):
                trust.check_manifest(dict(good, **bad))
        both = dict(good, assets=[   # one version, both systems, each its own kinds
            {"name": "w.zip", "platform": "windows-x86_64", "kind": "zip", "size": 1, "sha256": "0" * 64},
            {"name": "l.tar.xz", "platform": "linux-x86_64", "kind": "tar.xz", "size": 1, "sha256": "0" * 64},
            {"name": "l.AppImage", "platform": "linux-x86_64", "kind": "appimage", "size": 1, "sha256": "0" * 64}])
        trust.check_manifest(both)

    def test_versions(self):
        v = trust.parse_version
        self.assertLess(v("0.2.0-beta.1"), v("0.2.0"))
        self.assertLess(v("0.2.0"), v("0.10.0"))
        self.assertLess(v("0.2.0-beta.2"), v("0.2.0-beta.10"))
        self.assertIsNone(v("latest"))

    def test_keys_file(self):
        tmp = tempfile.mkdtemp()
        try:
            pk = bytes(range(32))
            good = {"id": trust.key_id(pk), "ed25519": base64.b64encode(pk).decode()}
            put(os.path.join(tmp, "k.json"), json.dumps({"keys": [good, dict(good, id="wrong"),
                                                                  dict(good, revoked=True)]}))
            self.assertEqual(trust.trusted_keys(os.path.join(tmp, "k.json")), [(trust.key_id(pk), pk)])
            # the repository's file: the owner's release key (2026-11-14), and only it
            self.assertEqual([k for k, _ in trust.trusted_keys()], ["6b5e12ec2cc5791f"])
        finally:
            shutil.rmtree(tmp)


if __name__ == "__main__":
    unittest.main()
