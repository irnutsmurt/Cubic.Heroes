"""M12, the Character Store's read-only client (tools/char_store.py) and the repository tool (tools/char_catalog.py):
no network -- a copy of the repository built from made-up characters and signed with a key made for the test, served
by a fake that counts requests, answers conditionally (ETag) and can be offline, rate limited or tampered with; one
test through a real local HTTP server.

    py -3 -m unittest tests.test_char_store
"""
import base64
import ctypes
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import char_catalog as C  # noqa: E402
import char_library as L  # noqa: E402
import char_store as S  # noqa: E402
import edt_format as E  # noqa: E402
import release_trust as T  # noqa: E402
from test_characters import make_character  # noqa: E402

BASE = "https://example.invalid/repo/"
QUIET = (lambda *a: None)


class TestKey:
    """an Ed25519 key made for the test (OpenSSL's, through ctypes): never a real key"""

    def __init__(self):
        lib = T.libcrypto()
        vp, sz = ctypes.c_void_p, ctypes.c_size_t
        lib.EVP_PKEY_new_raw_private_key.restype = vp
        lib.EVP_PKEY_new_raw_private_key.argtypes = (ctypes.c_int, vp, ctypes.c_char_p, sz)
        lib.EVP_PKEY_get_raw_public_key.restype = ctypes.c_int
        lib.EVP_PKEY_get_raw_public_key.argtypes = (vp, ctypes.c_char_p, ctypes.POINTER(sz))
        lib.EVP_DigestSignInit.restype = ctypes.c_int
        lib.EVP_DigestSignInit.argtypes = (vp, vp, vp, vp, vp)
        lib.EVP_DigestSign.restype = ctypes.c_int
        lib.EVP_DigestSign.argtypes = (vp, ctypes.c_char_p, ctypes.POINTER(sz), ctypes.c_char_p, sz)
        self.lib = lib
        self.pkey = lib.EVP_PKEY_new_raw_private_key(T.EVP_PKEY_ED25519, None, os.urandom(32), 32)
        buf, n = ctypes.create_string_buffer(32), sz(32)
        assert lib.EVP_PKEY_get_raw_public_key(self.pkey, buf, ctypes.byref(n)) == 1
        self.public = buf.raw
        self.id = T.key_id(self.public)

    def sign(self, msg):
        ctx = self.lib.EVP_MD_CTX_new()
        try:
            assert self.lib.EVP_DigestSignInit(ctx, None, None, None, self.pkey) == 1
            sig, n = ctypes.create_string_buffer(64), ctypes.c_size_t(64)
            assert self.lib.EVP_DigestSign(ctx, sig, ctypes.byref(n), msg, len(msg)) == 1
            return sig.raw
        finally:
            self.lib.EVP_MD_CTX_free(ctx)

    def keys(self):
        return [(self.id, self.public)]

    def sign_doc(self, doc):
        doc["signatures"] = [{"key_id": self.id, "ed25519": base64.b64encode(self.sign(S.canonical(doc["catalog"]))).decode()}]
        return doc


class FakeServer:
    """serves a repository folder like raw.githubusercontent.com: ETag + If-None-Match, counted requests"""

    def __init__(self, repo):
        self.repo, self.requests, self.offline, self.limited, self.override = repo, [], False, None, {}

    def __call__(self, url, headers=None, limit=S.CATALOG_MAX):
        self.requests.append((url, dict(headers or {})))
        if self.offline:
            raise OSError("no connection")
        if self.limited:
            return S.Response(429, {"Retry-After": str(self.limited)}, b"")
        assert url.startswith(BASE), url
        rel = url[len(BASE):]
        if rel in self.override:
            body = self.override[rel]
        else:
            p = os.path.join(self.repo, *rel.split("/"))
            if not os.path.isfile(p):
                return S.Response(404, {}, b"")
            body = open(p, "rb").read()
        etag = '"' + hashlib.sha1(body).hexdigest() + '"'
        if (headers or {}).get("If-None-Match") == etag:
            return S.Response(304, {"ETag": etag}, b"")
        if len(body) > limit:
            raise S.StoreError("too large")
        return S.Response(200, {"ETag": etag}, body)


class Clock:
    def __init__(self):
        self.t = 1_800_000_000.0

    def __call__(self):
        return self.t


def submission(d, name, colour, **over):
    edt = os.path.join(d, name.lower() + ".edt")
    with open(edt, "wb") as f:
        f.write(E.serialize(make_character(name=name, colour=colour)))
    sub = {"character": os.path.basename(edt), "label": name.title(), "creator": "Test Creator",
           "license": "CC-BY-4.0", "tags": ["test"], "description": "made up for a test",
           "consent": {"agreed": True, "date": "2026-11-24", "record": "unit test (made-up character)"}}
    sub.update(over)
    p = os.path.join(d, name.lower() + ".json")
    with open(p, "w", encoding="utf-8") as f:
        json.dump(sub, f)
    return p


class StoreBase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="store-test-")
        self.repo = os.path.join(self.tmp, "repo")
        self.src = os.path.join(self.tmp, "src")
        os.makedirs(self.src)
        self.subs = [submission(self.src, n, c) for n, c in (("ALPHA", (0.9, 0.1, 0.1)), ("BETA", (0.1, 0.9, 0.1)),
                                                             ("GAMMA", (0.1, 0.1, 0.9)))]
        added, problems = C.build(self.repo, self.subs, "2026-11-24", log=QUIET)
        self.assertEqual((len(added), problems), (3, []))
        self.key = TestKey()
        self.resign()
        self.server = FakeServer(self.repo)
        self.clock = Clock()
        self.lib = L.Library(os.path.join(self.tmp, "lib"))
        self.savedata = os.path.join(self.tmp, "savedata")
        os.makedirs(self.savedata)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def doc(self):
        return json.load(open(C._path(self.repo), encoding="utf-8"))

    def resign(self):
        C._save(self.repo, self.key.sign_doc(self.doc()))

    def store(self, keys=None):
        return S.Store(self.lib, base=BASE, get=self.server, clock=self.clock,
                       keys=self.key.keys() if keys is None else keys)


class Catalog(StoreBase):
    def test_the_built_repository_checks(self):
        keys = os.path.join(self.tmp, "keys.json")
        with open(keys, "w") as f:
            json.dump({"keys": [{"id": self.key.id, "ed25519": base64.b64encode(self.key.public).decode()}]}, f)
        self.assertEqual(C.check(self.repo, keys, log=QUIET), [])
        doc = self.doc()
        self.assertEqual((doc["format"], doc["schema_version"], doc["catalog"]["catalog_version"]),
                         ("cubic.heroes.catalog", 1, 1))
        e = doc["catalog"]["characters"][0]
        for k in ("creator", "license", "attribution", "version", "edt_sha256"):
            self.assertTrue(e[k], k)
        self.assertEqual((e["license"], e["version"]), ("CC-BY-4.0", 1))
        self.assertIn("Test Creator", e["attribution"])
        self.assertTrue(e["file"]["sha256"] and e["file"]["size"] and e["thumb"]["sha256"] and e["thumb"]["size"])
        self.assertNotIn("consent", json.dumps(doc))   # the agreement's record stays with the maintainers
        z = zipfile.ZipFile(io.BytesIO(open(os.path.join(self.repo, *e["file"]["path"].split("/")), "rb").read()))
        self.assertEqual(set(z.namelist()), S.CHCHAR_MEMBERS)   # the character only: no PS3 save files

    def test_no_licence_without_an_explicit_agreement(self):
        repo2 = os.path.join(self.tmp, "repo2")
        bad = [submission(self.src, "NOCONSENT", (0.5, 0.5, 0.5), consent={}),
               submission(self.src, "HALFCONSENT", (0.5, 0.5, 0.4), consent={"agreed": True}),
               submission(self.src, "MITMODEL", (0.5, 0.4, 0.5), license="MIT"),
               submission(self.src, "NOLICENCE", (0.4, 0.5, 0.5), license=None),
               submission(self.src, "NOCREATOR", (0.4, 0.4, 0.5), creator="")]
        added, problems = C.build(repo2, bad, "2026-11-24", log=QUIET)
        self.assertEqual((added, len(problems)), ([], 5))
        self.assertFalse(os.path.exists(C._path(repo2)))

    def test_building_twice_adds_nothing(self):
        added, problems = C.build(self.repo, self.subs, "2026-11-24", log=QUIET)
        self.assertEqual((len(added), len(problems), self.doc()["catalog"]["catalog_version"]), (0, 3, 1))

    def test_a_new_version(self):
        sub = submission(self.src, "ALPHA", (0.2, 0.2, 0.9))   # the same character, changed
        old = next(e for e in self.doc()["catalog"]["characters"] if e["label"] == "Alpha")
        with open(sub, encoding="utf-8") as f:
            d = json.load(f)
        d["replaces"] = old["id"]
        with open(sub, "w", encoding="utf-8") as f:
            json.dump(d, f)
        added, problems = C.build(self.repo, [sub], "2026-11-25", log=QUIET)
        self.assertEqual((len(added), problems), (1, []))
        doc = self.doc()
        e = next(x for x in doc["catalog"]["characters"] if x["id"] == old["id"])
        self.assertEqual((e["version"], e["previous_sha256"], doc["catalog"]["catalog_version"], doc["signatures"]),
                         (2, [old["edt_sha256"]], 2, []))

    def test_fetched_cached_and_checked_conditionally(self):
        st = self.store()
        self.assertEqual(st.refresh()["status"], "updated")
        self.assertEqual(len(self.server.requests), 1)
        self.assertEqual(st.refresh()["status"], "recent")            # opened again soon: no request
        self.assertEqual(len(self.server.requests), 1)
        self.clock.t += S.AUTO_CHECK_S + 1
        self.assertEqual(st.refresh()["status"], "unchanged")         # later: one conditional request, a 304
        self.assertIn("If-None-Match", self.server.requests[-1][1])
        self.assertEqual(self.store().refresh(force=True)["status"], "unchanged")   # the refresh button: same
        cat, _ = st.entries(self.savedata)
        self.assertEqual((len(cat["characters"]), cat["catalog_version"], cat["key_id"]), (3, 1, self.key.id))

    def test_only_a_signed_catalog_is_accepted(self):
        good = open(C._path(self.repo), "rb").read()
        doc = json.loads(good)
        cases = {
            "unsigned": dict(doc, signatures=[]),
            "another key": TestKey().sign_doc(json.loads(good)),
            "changed after signing": json.loads(good),
        }
        cases["changed after signing"]["catalog"]["characters"][0]["label"] = "Changed"
        for name, d in cases.items():
            self.server.override[S.CATALOG_PATH] = json.dumps(d).encode()
            st = S.Store(L.Library(os.path.join(self.tmp, "lib-" + name.replace(" ", "-"))), base=BASE,
                         get=self.server, clock=self.clock, keys=self.key.keys())
            r = st.refresh()
            self.assertEqual(r["status"], "error", name)
            self.assertIn("signature" if name != "unsigned" else "signed", r["message"], name)
            self.assertIsNone(st.entries(self.savedata)[0], name)
        no_keys = S.Store(L.Library(os.path.join(self.tmp, "lib-nokeys")), base=BASE, get=self.server,
                          clock=self.clock, keys=[])
        self.server.override.pop(S.CATALOG_PATH)
        self.assertIn("no catalog key", no_keys.refresh()["message"])

    def test_no_going_back_to_an_older_catalog(self):
        st = self.store()
        st.refresh()
        older = self.doc()
        newer = self.doc()
        newer["catalog"]["catalog_version"] = 5
        newer["catalog"]["characters"] = newer["catalog"]["characters"][:2]
        self.server.override[S.CATALOG_PATH] = json.dumps(self.key.sign_doc(newer)).encode()
        self.assertEqual(st.refresh(force=True)["status"], "updated")
        self.server.override[S.CATALOG_PATH] = json.dumps(older).encode()   # validly signed, but version 1
        r = st.refresh(force=True)
        self.assertEqual(r["status"], "error")
        self.assertIn("older", r["message"])
        self.assertEqual(st.entries(self.savedata)[0]["catalog_version"], 5)
        same = self.doc()
        same["catalog"]["catalog_version"] = 5   # version 5 again, other content
        self.server.override[S.CATALOG_PATH] = json.dumps(self.key.sign_doc(same)).encode()
        self.assertIn("without a new catalog version", st.refresh(force=True)["message"])

    def test_offline_shows_the_saved_catalog(self):
        st = self.store()
        st.refresh()
        self.server.offline = True
        r = st.refresh(force=True)
        self.assertEqual(r["status"], "offline")
        self.assertIn("saved catalog", r["message"])
        self.assertEqual(len(st.entries(self.savedata)[0]["characters"]), 3)
        fresh = S.Store(L.Library(os.path.join(self.tmp, "lib2")), base=BASE, get=self.server, clock=self.clock,
                        keys=self.key.keys())
        self.assertEqual(fresh.refresh()["status"], "offline")
        self.assertIsNone(fresh.entries(self.savedata)[0])

    def test_a_rate_limit_is_honoured(self):
        st = self.store()
        st.refresh()
        self.server.limited = 120
        self.assertEqual(st.refresh(force=True)["status"], "limited")
        n = len(self.server.requests)
        self.server.limited = None
        self.clock.t += 60
        self.assertEqual(st.refresh(force=True)["status"], "limited")   # still inside the wait: no request
        self.assertEqual(len(self.server.requests), n)
        self.clock.t += 61
        self.assertEqual(st.refresh(force=True)["status"], "unchanged")

    def test_a_bad_entry_is_left_out_alone(self):
        doc = self.doc()
        cs = doc["catalog"]["characters"]
        cs[0]["file"]["path"] = "../../elsewhere.chchar"
        cs[1]["license"] = "MIT"   # not a licence for a character in the store
        self.server.override[S.CATALOG_PATH] = json.dumps(self.key.sign_doc(doc)).encode()
        st = self.store()
        self.assertEqual(st.refresh()["status"], "updated")
        got, problems = st.entries(self.savedata)
        self.assertEqual(len(got["characters"]), 1)
        self.assertEqual(len(problems), 2)

    def test_a_newer_or_foreign_catalog_keeps_the_saved_one(self):
        st = self.store()
        st.refresh()
        newer = self.key.sign_doc(dict(self.doc(), schema_version=2))
        for doc in (newer, {"format": "x"}, "[1, 2"):
            self.server.override[S.CATALOG_PATH] = (doc if isinstance(doc, str) else json.dumps(doc)).encode()
            self.assertEqual(st.refresh(force=True)["status"], "error")
            self.assertEqual(len(st.entries(self.savedata)[0]["characters"]), 3)

    def test_addresses_and_keys(self):
        self.assertEqual(S.base_url({}), S.DEFAULT_BASE)
        self.assertEqual(S.base_url({S.BASE_ENV: "http://127.0.0.1:8123/r"}), "http://127.0.0.1:8123/r/")
        for bad in ("http://example.com/r/", "ftp://127.0.0.1/", "https://u:p@example.com/"):
            with self.assertRaises(S.StoreError):
                S.base_url({S.BASE_ENV: bad})
        for bad in ("../x", "/etc/passwd", "https://evil.example/x", "a/../../b", "a//b", ""):
            with self.assertRaises(S.StoreError):
                S.resolve(BASE, bad)
        self.assertEqual(S.resolve(BASE, "characters/0123456789abcdef/x.chchar"),
                         BASE + "characters/0123456789abcdef/x.chchar")
        keys = os.path.join(self.tmp, "keys.json")
        with open(keys, "w") as f:
            json.dump({"keys": [{"id": self.key.id, "ed25519": base64.b64encode(self.key.public).decode()}]}, f)
        # another keys file: only for a copy on this computer, never for the official address
        self.assertEqual(S.trusted_keys("http://127.0.0.1:1/", {S.KEYS_ENV: keys}), self.key.keys())
        self.assertNotIn((self.key.id, self.key.public), S.trusted_keys(S.DEFAULT_BASE, {S.KEYS_ENV: keys}))


class Thumbnails(StoreBase):
    def test_fetched_checked_and_cached(self):
        st = self.store()
        st.refresh()
        cat, _ = st.entries(self.savedata)
        self.assertEqual(st.fetch_thumbs(cat["characters"]), (3, 0))
        n = len(self.server.requests)
        self.assertEqual(st.fetch_thumbs(cat["characters"]), (0, 0))   # cached: no request
        self.assertEqual(len(self.server.requests), n)
        self.assertTrue(all(e["thumb_path"] for e in st.entries(self.savedata)[0]["characters"]))

    def test_a_tampered_thumbnail_is_refused(self):
        st = self.store()
        st.refresh()
        cat, _ = st.entries(self.savedata)
        e = cat["characters"][0]
        self.server.override[e["thumb"]["path"]] = b"\x89PNG\r\n\x1a\n" + b"x" * 40
        self.assertEqual(st.fetch_thumbs(cat["characters"]), (2, 1))
        self.assertFalse(os.path.exists(st.thumb_path(e)))

    def test_only_the_renderers_png_form_is_decoded(self):
        with self.assertRaises(S.StoreError):
            S.thumb_rle(b"GIF89a" + b"\0" * 40)
        e = self.doc()["catalog"]["characters"][0]
        huge = bytearray(open(os.path.join(self.repo, *e["thumb"]["path"].split("/")), "rb").read())
        huge[16:24] = (5000).to_bytes(4, "big") * 2   # a declared 5000 x 5000 picture: refused before decoding
        with self.assertRaises(S.StoreError):
            S.thumb_rle(bytes(huge))


class Downloads(StoreBase):
    def first(self, st):
        st.refresh()
        return st.entries(self.savedata)[0]["characters"][0]

    def test_downloaded_into_the_library_once(self):
        st = self.store()
        e = self.first(st)
        r = st.download(e["id"])
        self.assertEqual(r["status"], "imported")
        m = self.lib.meta(r["id"])
        self.assertEqual((m["source"]["kind"], m["label"], m["creator"], m["source"]["license"], m["source"]["version"]),
                         ("store", e["label"], "Test Creator", "CC-BY-4.0", 1))
        n = len(self.server.requests)
        self.assertEqual(st.download(e["id"])["status"], "duplicate")   # never twice, and no request for it
        self.assertEqual(len(self.server.requests), n)
        got = st.entries(self.savedata)[0]["characters"][0]
        self.assertEqual(got["in_library"], r["id"])

    def test_an_older_version_in_the_library_is_seen(self):
        st = self.store()
        e = self.first(st)
        lib_id = st.download(e["id"])["id"]
        sub = submission(self.src, "ALPHA2", (0.3, 0.3, 0.3), replaces=e["id"])
        C.build(self.repo, [sub], "2026-11-25", log=QUIET)
        self.resign()
        self.clock.t += S.AUTO_CHECK_S + 1
        self.assertEqual(st.refresh()["status"], "updated")
        got = next(x for x in st.entries(self.savedata)[0]["characters"] if x["id"] == e["id"])
        self.assertEqual((got["version"], got["in_library"], got["older_in_library"]), (2, "", lib_id))

    def test_download_and_install(self):
        st = self.store()
        e = self.first(st)
        r = st.download(e["id"], self.savedata, install=True)
        self.assertTrue(r["installed"])
        self.assertTrue(os.path.isfile(os.path.join(self.savedata, r["folder"], "DATA.EDT")))
        self.assertEqual(st.entries(self.savedata)[0]["characters"][0]["installed"], r["folder"])
        r2 = st.download(e["id"], self.savedata, install=True)   # already both: nothing added
        self.assertEqual((r2["status"], r2["folder"]), ("duplicate", r["folder"]))
        self.assertEqual(len(os.listdir(self.savedata)), 1)

    def test_a_file_that_does_not_match_the_catalog_is_refused(self):
        st = self.store()
        e = self.first(st)
        good = open(os.path.join(self.repo, *e["file"]["path"].split("/")), "rb").read()
        self.server.override[e["file"]["path"]] = good[:-1] + bytes([good[-1] ^ 1])
        with self.assertRaises(S.StoreError):
            st.download(e["id"])
        self.assertEqual(self.lib.ids(), [])

    def test_a_character_file_with_more_than_a_character_is_refused(self):
        # a catalog that lists, with its true checksum and a valid signature, a file carrying PS3 save files
        doc = self.doc()
        e = doc["catalog"]["characters"][0]
        good = open(os.path.join(self.repo, *e["file"]["path"].split("/")), "rb").read()
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as z:
            for n in zipfile.ZipFile(io.BytesIO(good)).namelist():
                z.writestr(n, zipfile.ZipFile(io.BytesIO(good)).read(n))
            z.writestr("PARAM.SFO", b"\0PSF" + b"\0" * 60)
        bad = buf.getvalue()
        e["file"].update(sha256=hashlib.sha256(bad).hexdigest(), size=len(bad))
        self.server.override[S.CATALOG_PATH] = json.dumps(self.key.sign_doc(doc)).encode()
        self.server.override[e["file"]["path"]] = bad
        st = self.store()
        st.refresh()
        with self.assertRaisesRegex(S.StoreError, "more than a character"):
            st.download(e["id"])
        self.assertEqual(self.lib.ids(), [])

    def test_offline_and_limited_downloads_say_so(self):
        st = self.store()
        e = self.first(st)
        self.server.offline = True
        with self.assertRaisesRegex(S.StoreError, "could not be reached"):
            st.download(e["id"])
        self.server.offline, self.server.limited = False, 300
        with self.assertRaisesRegex(S.StoreError, "wait"):
            st.download(e["id"])
        self.assertEqual(self.lib.ids(), [])


class RealHttp(StoreBase):
    """the real client (urllib) against a local server: conditional requests, and no redirect to another host"""

    def test_through_http(self):
        import functools
        import http.server
        import threading

        class H(http.server.SimpleHTTPRequestHandler):
            def do_GET(self):
                if self.path.startswith("/away"):
                    self.send_response(302)
                    self.send_header("Location", "http://localhost:%d/catalog/v1/catalog.json" % self.server.server_port)
                    self.end_headers()
                    return
                super().do_GET()

            def log_message(self, *a):
                pass

        srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(H, directory=self.repo))
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        try:
            base = S.base_url({S.BASE_ENV: "http://127.0.0.1:%d/" % srv.server_port})
            st = S.Store(self.lib, base=base, clock=self.clock, keys=self.key.keys())
            self.assertEqual(st.refresh()["status"], "updated")
            self.assertEqual(st.refresh(force=True)["status"], "unchanged")   # If-Modified-Since: a 304
            e = st.entries(self.savedata)[0]["characters"][0]
            self.assertEqual(st.fetch_thumbs([e]), (1, 0))
            self.assertEqual(st.download(e["id"])["status"], "imported")
            with self.assertRaises(S.StoreError):   # 127.0.0.1 -> localhost: another host
                S.http_get(base + "away")
        finally:
            srv.shutdown()
            srv.server_close()


if __name__ == "__main__":
    unittest.main()
