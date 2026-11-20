#!/usr/bin/env python3
"""HTTPS for updates (beta.4: tools/updates.py ca_store, tls_context, the "tls" result):
    python -m unittest tests.test_update_tls
beta.3's Linux worker looked for certificate authorities only where its OpenSSL was built (the build environment's
/opt/python/ssl), found none on a player's system, and every check read as offline. Here: the store a Linux worker
uses (SSL_CERT_FILE / SSL_CERT_DIR as given; else the distributions' bundles, a hashed folder; never the compiled-in
defaults; nothing found an error), and, over a local HTTPS server with a throwaway CA made in a temporary folder by
OpenSSL's command line, what a request meets: a trusted certificate fetched; an untrusted, an expired and another
host's certificate refused as "tls" (never "offline", never fetched); a closed port and a stalled server "offline";
an interrupted download over TLS resumed. Nothing here reaches the network; the live check is tests/live_update_check.py."""
import hashlib
import http.server
import os
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import release_trust as trust  # noqa: E402
import updates  # noqa: E402

OPENSSL = shutil.which("openssl")
PLAT = updates.platform_name()
NO_ENV = {k: v for k, v in os.environ.items() if k not in ("SSL_CERT_FILE", "SSL_CERT_DIR")}


def run(*args, cwd):
    return subprocess.run([OPENSSL, *args], cwd=cwd, capture_output=True, text=True)


class Pki:
    """a throwaway CA and server certificates for 127.0.0.1, in a temporary folder"""

    def __init__(self, d):
        self.d = d
        # a configuration of its own: the system's (if any) would add its v3_ca extensions beside these -- OpenSSL
        # 1.1.1 then writes basicConstraints twice, and the CA is refused
        with open(os.path.join(d, "req.cnf"), "w") as f:
            f.write("[req]\ndistinguished_name = dn\nx509_extensions = v3\nprompt = no\n[dn]\n"
                    "CN = Cubic.Heroes test CA\n[v3]\nbasicConstraints = critical,CA:TRUE\n"
                    "keyUsage = critical,keyCertSign,cRLSign\nsubjectKeyIdentifier = hash\n")
        ok = run("req", "-x509", "-config", "req.cnf", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                 "-nodes", "-keyout", "ca.key", "-out", "ca.pem", "-days", "2", cwd=d)
        if ok.returncode:
            raise unittest.SkipTest("openssl could not make a test CA: " + ok.stderr[-200:])
        self.ca = os.path.join(d, "ca.pem")

    def server(self, name, san="IP:127.0.0.1,DNS:localhost", expired=False):
        """a server certificate signed by the CA (expired: valid only in January 2020, through `openssl ca`, which
        takes past dates from OpenSSL 1.1.1 on -- `x509 -req` only from 3.4); None if this OpenSSL can't make it"""
        with open(os.path.join(self.d, name + ".ext"), "w") as f:
            f.write(f"basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\n"
                    f"extendedKeyUsage=serverAuth\nsubjectAltName={san}\nsubjectKeyIdentifier=hash\n"
                    f"authorityKeyIdentifier=keyid\n")
        r = run("req", "-new", "-config", "req.cnf", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes",
                "-keyout", name + ".key", "-out", name + ".csr", "-subj", "/CN=localhost", cwd=self.d)
        if r.returncode == 0 and expired:
            with open(os.path.join(self.d, "ca.cnf"), "w") as f:
                f.write("[ca]\ndefault_ca = test\n[test]\ndir = .\ndatabase = index.txt\nnew_certs_dir = .\n"
                        "certificate = ca.pem\nprivate_key = ca.key\nserial = serial\ndefault_md = sha256\n"
                        "policy = any\nunique_subject = no\n[any]\ncommonName = supplied\n")
            open(os.path.join(self.d, "index.txt"), "w").close()
            with open(os.path.join(self.d, "serial"), "w") as f:
                f.write("1000\n")
            r = run("ca", "-batch", "-config", "ca.cnf", "-in", name + ".csr", "-out", name + ".pem", "-notext",
                    "-startdate", "20200101000000Z", "-enddate", "20200201000000Z", "-extfile", name + ".ext",
                    cwd=self.d)
        elif r.returncode == 0:
            r = run("x509", "-req", "-in", name + ".csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial",
                    "-out", name + ".pem", "-extfile", name + ".ext", "-days", "2", cwd=self.d)
        return (os.path.join(self.d, name + ".pem"), os.path.join(self.d, name + ".key")) if r.returncode == 0 else None


class Files(http.server.BaseHTTPRequestHandler):
    """serves `data` with Range support; the first full request is cut off after `cut` bytes when cut is set"""
    data, cut, cuts = b"hello over TLS", 0, [0]

    def do_GET(self):
        rng = self.headers.get("Range")
        start = int(rng.split("=")[1].split("-")[0]) if rng else 0
        body = self.data[start:]
        self.send_response(206 if rng else 200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.cut and not rng and self.cuts[0] == 0:
            self.cuts[0] += 1
            self.wfile.write(body[:self.cut])
            self.wfile.flush()
            self.connection.shutdown(2)
            return
        self.wfile.write(body)

    def log_message(self, *a):
        pass


class Https:
    def __init__(self, cert):
        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Files)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(*cert)
        self.srv.socket = ctx.wrap_socket(self.srv.socket, server_side=True)
        self.srv.handle_error = lambda *a: None   # the refused handshakes: expected here
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.url = f"https://127.0.0.1:{self.srv.server_address[1]}/f.tar.xz"

    def close(self):
        self.srv.shutdown()
        self.srv.server_close()


def ca_of(path):
    return lambda: updates.ca_store({"SSL_CERT_FILE": path})[0]


class CaStore(unittest.TestCase):
    """which certificate authorities a Linux worker trusts (pure: runs on every system)"""

    @classmethod
    def setUpClass(cls):
        if not OPENSSL:
            raise unittest.SkipTest("no openssl")
        cls.tmp = tempfile.mkdtemp(prefix="tls-store-")
        cls.pki = Pki(cls.tmp)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def path(self, name, data=b""):
        p = os.path.join(self.tmp, name)
        with open(p, "wb") as f:
            f.write(data)
        return p

    def test_verification_and_host_names_stay_on(self):
        ctx, where = updates.ca_store({}, files=(self.pki.ca,), dirs=())
        self.assertEqual(where, self.pki.ca)
        self.assertEqual(ctx.verify_mode, ssl.CERT_REQUIRED)
        self.assertTrue(ctx.check_hostname)

    def test_the_environment_wins_and_is_used_as_given(self):
        with open(self.pki.ca, "rb") as f:
            system = self.path("system.pem", f.read())
        ctx, where = updates.ca_store({"SSL_CERT_FILE": self.pki.ca}, files=(system,), dirs=())
        self.assertEqual(where, "SSL_CERT_FILE/SSL_CERT_DIR")
        with self.assertRaises(trust.ReleaseError) as e:   # set to nothing readable: an error, no other store
            updates.ca_store({"SSL_CERT_FILE": os.path.join(self.tmp, "missing.pem")}, files=(system,), dirs=())
        self.assertEqual(e.exception.code, "tls")

    def test_cert_dir_from_the_environment(self):
        h = run("x509", "-hash", "-noout", "-in", self.pki.ca, cwd=self.tmp).stdout.strip()
        d = os.path.join(self.tmp, "certdir")
        os.makedirs(d, exist_ok=True)
        shutil.copy(self.pki.ca, os.path.join(d, h + ".0"))
        ctx, where = updates.ca_store({"SSL_CERT_DIR": d}, files=(), dirs=())
        self.assertEqual(where, "SSL_CERT_FILE/SSL_CERT_DIR")
        self.assertEqual(ctx.verify_mode, ssl.CERT_REQUIRED)

    def test_the_first_bundle_with_certificates(self):
        empty = self.path("empty.pem")
        junk = self.path("junk.pem", b"not a certificate\n")
        _, where = updates.ca_store({}, files=(os.path.join(self.tmp, "absent.pem"), empty, junk, self.pki.ca),
                                    dirs=())
        self.assertEqual(where, self.pki.ca)

    def test_a_hashed_folder_when_no_bundle(self):
        h = run("x509", "-hash", "-noout", "-in", self.pki.ca, cwd=self.tmp).stdout.strip()
        d = os.path.join(self.tmp, "hashed")
        plain = os.path.join(self.tmp, "plain")
        os.makedirs(d, exist_ok=True)
        os.makedirs(plain, exist_ok=True)
        shutil.copy(self.pki.ca, os.path.join(d, h + ".0"))
        shutil.copy(self.pki.ca, os.path.join(plain, "ca.pem"))   # no hashed names: OpenSSL can't look it up
        _, where = updates.ca_store({}, files=(), dirs=(plain, d))
        self.assertEqual(where, d)

    def test_nothing_found_is_an_error_never_the_compiled_in_defaults(self):
        with self.assertRaises(trust.ReleaseError) as e:
            updates.ca_store({}, files=(os.path.join(self.tmp, "absent.pem"),), dirs=(os.path.join(self.tmp, "no"),))
        self.assertEqual(e.exception.code, "tls")

    @unittest.skipUnless(os.name == "nt", "Windows")
    def test_windows_keeps_urllibs_default(self):
        self.assertIsNone(updates.tls_context())

    @unittest.skipIf(os.name == "nt", "Linux")
    def test_linux_uses_the_store(self):
        with mock.patch.dict(os.environ, {"SSL_CERT_FILE": self.pki.ca}):
            self.assertIsInstance(updates.tls_context(), ssl.SSLContext)


class NeverUnverified(unittest.TestCase):
    """updates.py never weakens verification: a later change that did fails here"""

    def test_no_switch_turns_verification_off(self):
        import re
        with open(os.path.join(ROOT, "tools", "updates.py"), encoding="utf-8") as f:
            src = f.read()
        bad = re.findall(r"CERT_NONE|check_hostname\s*=\s*False|_create_unverified_context|verify_mode\s*=[^=]|"
                         r"PYTHONHTTPSVERIFY", src)
        self.assertEqual(bad, [])


class OverTls(unittest.TestCase):
    """requests over a local HTTPS server: what is fetched, what is refused and how it is reported"""

    @classmethod
    def setUpClass(cls):
        if not OPENSSL:
            raise unittest.SkipTest("no openssl")
        cls.tmp = tempfile.mkdtemp(prefix="tls-net-")
        cls.pki = Pki(cls.tmp)
        cls.good = cls.pki.server("good")
        cls.other_host = cls.pki.server("other", san="DNS:example.invalid")
        cls.expired = cls.pki.server("expired", expired=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        Files.data, Files.cut, Files.cuts = b"hello over TLS", 0, [0]

    def serve(self, cert):
        s = Https(cert)
        self.addCleanup(s.close)
        return s

    def refused(self, url, ctx, match=None):
        with mock.patch.object(updates, "tls_context", ctx):
            with self.assertRaises(trust.ReleaseError) as e:
                updates._get(url)
        self.assertEqual(e.exception.code, "tls", str(e.exception))
        if match:
            self.assertIn(match, str(e.exception))

    def test_trusted_fetched(self):
        s = self.serve(self.good)
        with mock.patch.object(updates, "tls_context", ca_of(self.pki.ca)):
            self.assertEqual(updates._get(s.url), b"hello over TLS")

    @unittest.skipIf(os.name == "nt", "Linux: SSL_CERT_FILE through the worker's own lookup")
    def test_trusted_through_ssl_cert_file(self):
        s = self.serve(self.good)
        with mock.patch.dict(os.environ, {"SSL_CERT_FILE": self.pki.ca}):
            self.assertEqual(updates._get(s.url), b"hello over TLS")

    def test_untrusted_is_tls_not_offline(self):
        """the system's own store (Linux: its bundle; Windows: its certificate store) does not know the test CA"""
        s = self.serve(self.good)
        with mock.patch.dict(os.environ, NO_ENV, clear=True):
            self.refused(s.url, updates.tls_context)

    def test_expired_refused(self):
        if not self.expired:
            self.skipTest("this openssl could not make an expired certificate")
        self.refused(self.serve(self.expired).url, ca_of(self.pki.ca), "expired")

    def test_another_hosts_certificate_refused(self):
        self.refused(self.serve(self.other_host).url, ca_of(self.pki.ca))

    def test_download_refused_untrusted_nothing_kept(self):
        s = self.serve(self.good)
        dest = tempfile.mkdtemp(dir=self.tmp)
        asset = {"name": "f.tar.xz", "platform": PLAT, "size": len(Files.data),
                 "sha256": hashlib.sha256(Files.data).hexdigest()}
        with mock.patch.dict(os.environ, NO_ENV, clear=True), self.assertRaises(trust.ReleaseError) as e:
            updates.download(asset, s.url, dest)
        self.assertEqual(e.exception.code, "tls")
        self.assertFalse([n for n in os.listdir(dest) if n == "f.tar.xz"])

    def test_closed_port_is_offline(self):
        with socket.socket() as sk:
            sk.bind(("127.0.0.1", 0))
            port = sk.getsockname()[1]
        with mock.patch.object(updates, "tls_context", ca_of(self.pki.ca)), \
                self.assertRaises(trust.ReleaseError) as e:
            updates._get(f"https://127.0.0.1:{port}/releases")
        self.assertEqual(e.exception.code, "offline")

    def test_a_stalled_server_is_offline_after_the_timeout(self):
        """a server that accepts and never answers: the request ends at the timeout, as offline (never a hang)"""
        with socket.socket() as sk:
            sk.bind(("127.0.0.1", 0))
            sk.listen(1)
            port = sk.getsockname()[1]
            with mock.patch.object(updates, "tls_context", ca_of(self.pki.ca)), \
                    mock.patch.object(updates, "TIMEOUT_S", 1), self.assertRaises(trust.ReleaseError) as e:
                updates._get(f"https://127.0.0.1:{port}/releases")
        self.assertEqual(e.exception.code, "offline")

    def test_interrupted_download_resumed_over_tls(self):
        data = os.urandom(3 << 20)
        Files.data, Files.cut, Files.cuts = data, 1 << 20, [0]
        s = self.serve(self.good)
        dest = tempfile.mkdtemp(dir=self.tmp)
        asset = {"name": "f.tar.xz", "platform": PLAT, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        with mock.patch.object(updates, "tls_context", ca_of(self.pki.ca)):
            with self.assertRaises(trust.ReleaseError) as e:
                updates.download(asset, s.url, dest)
            self.assertIn(e.exception.code, ("interrupted", "bad_download"))
            p = updates.download(asset, s.url, dest)
        with open(p, "rb") as f:
            self.assertEqual(f.read(), data)


if __name__ == "__main__":
    unittest.main()
