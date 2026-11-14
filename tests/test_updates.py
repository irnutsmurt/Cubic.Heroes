#!/usr/bin/env python3
"""Signed releases and updates (M9.3: tools/release_trust.py, tools/updates.py, tools/release.py, the worker's
update commands):
    build/venv/Scripts/python.exe -m unittest tests.test_updates
Ed25519 against RFC 8032's vectors and OpenSSL's command line; a made-up release mirror signed with throwaway keys
made in a temporary folder (never the owner's): the channels, an altered, unsigned or foreign-signed release
refused, the next valid one taken; downloads checked, a damaged one refused, an interrupted one resumed (over a
local HTTP server that drops the connection), too little space refused, offline reported; a launcher package with
a path escaping it refused; the launcher's swap kept when the new one comes up, put back when it does not."""
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

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
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
        name = f"CubicHeroes-{PLAT}-{version}.zip"
        z = os.path.join(d, name)
        with zipfile.ZipFile(z, "w") as zf:
            zf.writestr("CubicHeroes/launcher.txt", payload.decode() + version)
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
        # the repository's keys file has no key yet: nothing is offered, and nothing is asked of the network
        real = updates.releases
        updates.releases = lambda source=None: self.fail("the release list was requested")
        try:
            self.assertIsNone(self_update.latest())
        finally:
            updates.releases = real
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
        p = subprocess.run(w + ["check-update", "--source", self.mirror, "--installed", "0.1.0"],
                           capture_output=True, text=True, timeout=60)   # the repository's keys: none yet
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
        bad = os.path.join(self.tmp, "bad.zip")
        with zipfile.ZipFile(bad, "w") as zf:
            zf.writestr("../escaped.txt", "x")
        with self.assertRaises(trust.ReleaseError):
            updates.stage_launcher(bad, os.path.join(self.tmp, "launcher"))
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "escaped.txt")))

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


@unittest.skipUnless(os.name == "nt", "the stand-in launchers are batch files")
class LauncherSwap(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="swap-test-")
        self.d = os.path.join(self.tmp, "launcher")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def launcher(self, folder, version, healthy=True):
        put(os.path.join(folder, "version.txt"), version)
        put(os.path.join(folder, "start.bat"), "@echo off\r\n" + (f"echo %1 > {updates.HEALTH_FILE}\r\n" if healthy
                                                                   else "exit /b 1\r\n"))

    def test_kept_when_it_comes_up(self):
        self.launcher(self.d, "old")
        self.launcher(self.d + ".new", "new")
        self.assertEqual(updates.swap_launcher(self.d, "start.bat", log=lambda t: None, health_timeout=20), "updated")
        self.assertEqual(get(os.path.join(self.d, "version.txt")), b"new")
        self.assertEqual(get(os.path.join(self.d + ".old", "version.txt")), b"old", "the previous one kept")

    def test_put_back_when_it_does_not(self):
        self.launcher(self.d, "old")
        self.launcher(self.d + ".new", "new", healthy=False)
        self.assertEqual(updates.swap_launcher(self.d, "start.bat", log=lambda t: None, health_timeout=5), "restored")
        self.assertEqual(get(os.path.join(self.d, "version.txt")), b"old")
        self.assertEqual(get(os.path.join(self.d + ".failed", "version.txt")), b"new")


class Manifest(unittest.TestCase):
    def test_rules(self):
        good = {"format": 1, "version": "0.2.0", "channel": "stable", "notes": "", "key_id": "x",
                "assets": [{"name": "a.zip", "platform": PLAT, "size": 1, "sha256": "0" * 64}]}
        trust.check_manifest(dict(good))
        for bad in ({"format": 2}, {"version": "two"}, {"channel": "nightly"}, {"assets": []},
                    {"assets": [{"name": "../a.zip", "platform": PLAT, "size": 1, "sha256": "0" * 64}]},
                    {"assets": [{"name": "a.zip", "platform": "mac", "size": 1, "sha256": "0" * 64}]},
                    {"assets": [{"name": "a.zip", "platform": PLAT, "size": 1, "sha256": "z"}]}):
            with self.assertRaises(trust.ReleaseError):
                trust.check_manifest(dict(good, **bad))

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
            self.assertEqual(trust.trusted_keys(), [], "the repository's file: no key until the owner adds one")
        finally:
            shutil.rmtree(tmp)


if __name__ == "__main__":
    unittest.main()
