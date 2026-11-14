#!/usr/bin/env python3
"""The release key's public half (tools/release.py pubkey and keygen's recovery; docs/RELEASE_SIGNING.md):
    build/venv/Scripts/python.exe -m unittest tests.test_release_keys
With OpenSSL's command line (skipped without it), a throwaway encrypted Ed25519 key made in a temporary folder (its
passphrase random, made here, never printed): the public key extracted after a wrong passphrase, then the right one;
three wrong ones refused with the key unchanged; keygen whose extraction fails keeps the private key and names the
recovery command; the entry never written over the private key."""
import hashlib
import io
import json
import os
import secrets
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import release  # noqa: E402
import release_trust as trust  # noqa: E402


def _openssl():
    for c in (shutil.which("openssl"), r"C:\Program Files\Git\usr\bin\openssl.exe",
              r"C:\Program Files\Git\mingw64\bin\openssl.exe"):
        if c and os.path.exists(c):
            return c
    return None


OPENSSL = _openssl()


def sha(p):
    with open(p, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


@unittest.skipUnless(OPENSSL, "OpenSSL's command line isn't installed")
class PublicKeyTest(unittest.TestCase):
    def setUp(self):
        self.tmpd = tempfile.TemporaryDirectory()
        self.dir = self.tmpd.name
        self.key = os.path.join(self.dir, "release-signing-key.pem")
        self.pw = secrets.token_hex(12)
        subprocess.run([OPENSSL, "genpkey", "-algorithm", "ed25519", "-aes256", "-pass", "stdin", "-out", self.key],
                       input=(self.pw + "\n").encode(), check=True, capture_output=True)
        der = subprocess.run([OPENSSL, "pkey", "-in", self.key, "-passin", "stdin", "-pubout", "-outform", "DER"],
                             input=(self.pw + "\n").encode(), check=True, capture_output=True).stdout
        self.want = trust.public_key_from(der)
        self.before = sha(self.key)

    def tearDown(self):
        self.tmpd.cleanup()

    def test_wrong_then_right_passphrase(self):
        asked = iter(["not-it", self.pw])
        err = io.StringIO()
        with redirect_stderr(err):
            pub = release.extract_public(self.key, prompt=lambda _: next(asked))
        self.assertEqual(pub, self.want)
        self.assertIn("Try again", err.getvalue())
        self.assertNotIn(self.pw, err.getvalue())
        self.assertEqual(sha(self.key), self.before)

    def test_three_wrong_refused_key_unchanged(self):
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as e:
            release.extract_public(self.key, prompt=lambda _: "wrong")
        self.assertIn("unchanged", str(e.exception))
        self.assertEqual(sha(self.key), self.before)

    def test_pubkey_command_writes_the_entry(self):
        out = os.path.join(self.dir, "entry.json")

        class A:
            key, note = self.key, "test key"
        A.out = out
        real = release.getpass.getpass
        release.extract_public.__defaults__ = (3, lambda _: self.pw)
        try:
            with redirect_stdout(io.StringIO()):
                release.cmd_pubkey(A)
        finally:
            release.extract_public.__defaults__ = (3, real)
        with open(out) as f:
            entry = json.load(f)
        self.assertEqual(entry["id"], trust.key_id(self.want))
        self.assertEqual(trust.public_key_from(entry["ed25519"]), self.want)
        self.assertEqual(sha(self.key), self.before)
        A.out = self.key   # never over the private key
        with redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
            release.extract_public.__defaults__ = (3, lambda _: self.pw)
            try:
                release.cmd_pubkey(A)
            finally:
                release.extract_public.__defaults__ = (3, real)
        self.assertEqual(sha(self.key), self.before)

    def test_keygen_extraction_failing_keeps_the_key_and_names_the_recovery(self):
        out = os.path.join(self.dir, "new")

        class A:
            note = "x"
        A.out = out
        made = os.path.join(out, "release-signing-key.pem")
        real_gen, real_ex = release._genpkey, release.extract_public
        release._genpkey = lambda key: shutil.copyfile(self.key, key)

        def broken(key, *a, **k):
            raise SystemExit("OpenSSL couldn't read the passphrase")
        release.extract_public = broken
        try:
            with redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as e:
                release.cmd_keygen(A)
        finally:
            release._genpkey, release.extract_public = real_gen, real_ex
        msg = str(e.exception)
        self.assertIn("release.py pubkey --key", msg)
        self.assertIn("don't make a new key", msg)
        self.assertTrue(os.path.exists(made))
        self.assertEqual(sha(made), self.before)
        # and run again, keygen refuses to write over it, naming the same recovery
        with self.assertRaises(SystemExit) as e:
            release.cmd_keygen(A)
        self.assertIn("pubkey --key", str(e.exception))
        self.assertEqual(sha(made), self.before)


if __name__ == "__main__":
    unittest.main()
