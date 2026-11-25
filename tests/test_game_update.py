"""The official update's download and check (tools/game_update.py), against a local server and made-up packages: no
Sony server is contacted and no Sony file is used."""
import hashlib
import http.server
import os
import shutil
import sys
import tempfile
import threading
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import game_update  # noqa: E402

TMP = os.path.join(ROOT, "build", "tmp")


class Served(http.server.BaseHTTPRequestHandler):
    data = b""

    def do_GET(self):
        rng = self.headers.get("Range")
        start = int(rng.split("=")[1].split("-")[0]) if rng else 0
        body = self.data[start:]
        self.send_response(206 if rng else 200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


class OfficialUpdate(unittest.TestCase):
    def setUp(self):
        os.makedirs(TMP, exist_ok=True)
        self.dir = tempfile.mkdtemp(prefix="gameupd-", dir=TMP)
        self.data = os.urandom(300001)
        self.rec = {"app_ver": "01.01", "file": "UP-TEST.pkg", "url": "http://127.0.0.1:9/none.pkg",
                    "size": len(self.data), "sha256": hashlib.sha256(self.data).hexdigest()}

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def serve(self, data):
        Served.data = data
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Served)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.shutdown)
        return f"http://127.0.0.1:{srv.server_address[1]}/UP-TEST.pkg"

    def put(self, name, data):
        p = os.path.join(self.dir, name)
        with open(p, "wb") as f:
            f.write(data)
        return p

    def test_the_record(self):
        r = game_update.record()
        self.assertEqual((r["app_ver"], r["size"]), ("01.01", 185807824))
        self.assertTrue(r["url"].startswith("http://b0.ww.np.dl.playstation.net/") and r["url"].endswith(r["file"]))
        self.assertEqual(len(r["sha256"]), 64)
        self.assertTrue(r["file"].startswith(r["content_id"]))

    def test_check_accepts_only_the_exact_package(self):
        self.assertTrue(game_update.check_package(self.put("ok.pkg", self.data), self.rec))
        changed = bytearray(self.data)
        changed[100] ^= 1
        for name, data in (("short.pkg", self.data[:-1]), ("changed.pkg", bytes(changed))):
            with self.assertRaises(game_update.UpdateError) as c:
                game_update.check_package(self.put(name, data), self.rec)
            self.assertEqual(c.exception.code, "not_official")
        with self.assertRaises(game_update.UpdateError) as c:
            game_update.check_package(os.path.join(self.dir, "missing.pkg"), self.rec)
        self.assertEqual(c.exception.code, "io")

    def test_download_checked_then_named(self):
        url = self.serve(self.data)
        seen = []
        p = game_update.download(os.path.join(self.dir, "dl"), self.rec, url, progress=lambda d, t: seen.append(d))
        self.assertEqual(os.path.basename(p), "UP-TEST.pkg")
        with open(p, "rb") as f:
            self.assertEqual(f.read(), self.data)
        self.assertEqual(seen[-1], len(self.data))
        self.assertEqual(game_update.download(os.path.join(self.dir, "dl"), self.rec, url), p)   # kept, checked again

    def test_a_different_file_is_refused_and_removed(self):
        url = self.serve(os.urandom(len(self.data)))
        with self.assertRaises(game_update.UpdateError) as c:
            game_update.download(os.path.join(self.dir, "dl"), self.rec, url)
        self.assertEqual(c.exception.code, "not_official")
        self.assertEqual(os.listdir(os.path.join(self.dir, "dl")), [])

    def test_offline(self):
        with self.assertRaises(game_update.UpdateError) as c:
            game_update.download(os.path.join(self.dir, "dl"), self.rec)
        self.assertEqual(c.exception.code, "offline")
        self.assertIn("Sony", str(c.exception))


if __name__ == "__main__":
    unittest.main()
