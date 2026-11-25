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

    def test_manifest_from_the_official_package_only_and_the_folder_check(self):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from test_pkg_update import keys_text, make_pkg
        key = os.urandom(16)
        files = [("PARAM.SFO", os.urandom(432)), ("USRDIR/EBOOT.BIN", os.urandom(9000)),
                 ("USRDIR/data/edt/addition/a.EDT", os.urandom(5000))]
        pkg = self.put("u.pkg", make_pkg(key, files, ["USRDIR", "USRDIR/data"]))
        with open(pkg, "rb") as f:
            data = f.read()
        rec = {"app_ver": "01.01", "content_id": "UP0005-BLUS30490_00-TESTPATCH0000001", "size": len(data),
               "sha256": hashlib.sha256(data).hexdigest()}
        keys = os.path.join(self.dir, "keys.txt")
        with open(keys, "w", newline="\n") as f:
            f.write(keys_text({"PKG": key}))
        m = game_update.make_manifest(pkg, keys, rec)
        self.assertEqual((m["title_id"], m["count"]), ("BLUS30490", 3))
        self.assertEqual({f["path"]: f["sha256"] for f in m["files"]},
                         {n: hashlib.sha256(b).hexdigest() for n, b in files})
        with self.assertRaises(game_update.UpdateError):                       # another package: refused
            game_update.make_manifest(self.put("other.pkg", make_pkg(key, files)), keys, rec)
        folder = os.path.join(self.dir, "BLUS30490")
        for n, b in files + [("USRDIR/extra.bin", b"left alone")]:   # an installed update: other files are fine
            os.makedirs(os.path.dirname(os.path.join(folder, *n.split("/"))), exist_ok=True)
            self.put(os.path.join("BLUS30490", *n.split("/")), b)
        self.assertTrue(game_update.check_folder(folder, m))
        p = os.path.join(folder, "USRDIR", "data", "edt", "addition", "a.EDT")
        with open(p, "r+b") as f:
            f.write(b"\0")
        with self.assertRaises(game_update.UpdateError) as c:
            game_update.check_folder(folder, m)
        self.assertIn("a.EDT", str(c.exception))
        os.remove(p)
        with self.assertRaises(game_update.UpdateError) as c:
            game_update.check_folder(folder, m)
        self.assertIn("missing", str(c.exception))

    def test_the_committed_manifest_matches_the_record(self):
        m = game_update.load_manifest()
        r = game_update.record()
        self.assertEqual(m["made_from"], {"sha256": r["sha256"], "size": r["size"]})
        self.assertEqual((m["app_ver"], m["count"], len(m["files"])), (r["app_ver"], len(m["files"]), m["count"]))
        self.assertIn("USRDIR/EBOOT.BIN", {f["path"] for f in m["files"]})

    def test_offline(self):
        with self.assertRaises(game_update.UpdateError) as c:
            game_update.download(os.path.join(self.dir, "dl"), self.rec)
        self.assertEqual(c.exception.code, "offline")
        self.assertIn("Sony", str(c.exception))


if __name__ == "__main__":
    unittest.main()
