"""beta.6: characters that come with official game content (config/official_characters.json, tools/official_content.py)
and the Character Store's state (tools/char_store.py, docs/CHARACTER_STORE.md "Official content"). Made-up characters
and a test mapping only; never official files. Two checks run only where the maintainers' local files are (the
store repository's working copy, the private verification of the mapping) and are skipped elsewhere.

    py -3 -m unittest tests.test_official_content
"""
import builtins
import copy
import json
import os
import re
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import char_catalog as C  # noqa: E402
import char_store as S  # noqa: E402
import edt_format as E  # noqa: E402
import official_content as O  # noqa: E402
from test_char_store import QUIET, StoreBase  # noqa: E402
from test_characters import make_character  # noqa: E402

GAME_VERSION = json.load(open(os.path.join(ROOT, "config", "game_version.json"), encoding="utf-8"))
V100_ELF = GAME_VERSION["decrypted_elf"]["sha256"]


def write_install(root, **rec):
    os.makedirs(os.path.join(root, "data"), exist_ok=True)
    with open(os.path.join(root, "data", "install.json"), "w", encoding="utf-8") as f:
        json.dump(rec, f)
    return root


class TheMapping(unittest.TestCase):
    """the shipped mapping: versioned, complete, and free of the official files' data or fingerprints"""

    def setUp(self):
        self.m = O.load()
        self.raw = open(O.MAPPING, encoding="utf-8").read()

    def test_us_v101(self):
        self.assertIn("BLUS30490-01.00", self.m)
        p = self.m["BLUS30490-01.01"]
        self.assertEqual((p["kind"], p["requires"], p["label"]),
                         ("update", "BLUS30490-01.00", "Included with the official v1.01 update"))
        chars = p["characters"]
        kinds = [c["match"] for c in chars]
        self.assertEqual((len(chars), kinds.count("identical"), kinds.count("same-model"), kinds.count("none")),
                         (51, 39, 6, 6))
        self.assertEqual(len({c["catalog_id"] for c in chars if c["catalog_id"]}), 45)
        for c in chars:   # the six non-identical ones differ only in the in-game name (verified privately)
            if c["match"] == "same-model":
                self.assertEqual(c["differs"], ["name"])

    def test_the_six_official_only_characters_have_no_catalog_entry(self):
        none = [c for c in self.m["BLUS30490-01.01"]["characters"] if c["match"] == "none"]
        self.assertEqual(len(none), 6)
        self.assertTrue(all(c["catalog_id"] is None for c in none))

    def test_no_official_data_or_fingerprints(self):
        self.assertIsNone(re.search(r"[0-9a-fA-F]{32,}", self.raw))   # no file hashes: catalog ids are 16 hex
        doc = json.loads(self.raw)
        keys = set()

        def walk(x):
            if isinstance(x, dict):
                keys.update(x)
                for v in x.values():
                    walk(v)
            elif isinstance(x, list):
                for v in x:
                    walk(v)
        walk(doc)
        self.assertFalse({k for k in keys if "sha" in k.lower() or "hash" in k.lower() or k in ("data", "edt")})

    def test_a_bad_mapping_is_refused(self):
        doc = json.loads(self.raw)
        bad = []
        for change in (lambda d: d.update(version=2), lambda d: d.update(format="x"),
                       lambda d: d["content"][1]["characters"].append(dict(d["content"][1]["characters"][0])),
                       lambda d: d["content"][1]["characters"][0].update(catalog_id=None),
                       lambda d: d["content"][1].update(requires="BLUS30490-09.99")):
            d = copy.deepcopy(doc)
            change(d)
            p = os.path.join(self.tmpdir(), "m.json")
            json.dump(d, open(p, "w"))
            with self.assertRaises(O.MappingError):
                O.load(p)
            bad.append(True)
        self.assertEqual(len(bad), 5)

    def tmpdir(self):
        import tempfile
        d = tempfile.mkdtemp(prefix="official-")
        self.addCleanup(lambda: __import__("shutil").rmtree(d, ignore_errors=True))
        return d


class WhatAnInstallationHas(unittest.TestCase):
    """from the installation's own record only; recomputed every time: installed, removed, replaced"""

    def setUp(self):
        import tempfile
        self.tmp = tempfile.mkdtemp(prefix="official-inst-")
        self.addCleanup(lambda: __import__("shutil").rmtree(self.tmp, ignore_errors=True))
        self.m = O.load()

    def test_v100_from_its_program(self):
        d = write_install(os.path.join(self.tmp, "a"), elf_sha256=V100_ELF)
        self.assertEqual(O.installed(d, self.m), ["BLUS30490-01.00"])
        self.assertEqual(O.included(O.installed(d, self.m), self.m), {})   # v1.00 brings no store character

    def test_an_unknown_program_has_none(self):
        d = write_install(os.path.join(self.tmp, "b"), elf_sha256="0" * 64)
        self.assertEqual(O.installed(d, self.m), [])
        self.assertEqual(O.installed(os.path.join(self.tmp, "nothing"), self.m), [])

    def test_update_installed_removed_replaced(self):
        d = os.path.join(self.tmp, "c")
        write_install(d, elf_sha256=V100_ELF, official_content=["BLUS30490-01.00", "BLUS30490-01.01"])
        pk = O.installed(d, self.m)
        self.assertEqual(pk, ["BLUS30490-01.00", "BLUS30490-01.01"])
        inc = O.included(pk, self.m)
        self.assertEqual(len(inc), 45)
        self.assertTrue(all(v == [{"id": "BLUS30490-01.01", "label": "Included with the official v1.01 update"}]
                            for v in inc.values()))
        write_install(d, elf_sha256=V100_ELF, official_content=["BLUS30490-01.00"])   # removed
        self.assertEqual(O.included(O.installed(d, self.m), self.m), {})
        # an update without its game doesn't count; an unknown package is ignored
        write_install(d, official_content=["BLUS30490-01.01", "BLES00000-09.99"])
        self.assertEqual(O.installed(d, self.m), [])
        # replaced by another release's package (a mapping that has one: the same mechanism for JP / EU releases)
        m2 = copy.deepcopy(self.m)
        m2["BLJM60180-01.20"] = {"id": "BLJM60180-01.20", "title_id": "BLJM60180", "region": "JP", "version": "01.20",
                                 "kind": "update", "label": "Included with the official Japanese v1.20 update",
                                 "requires": None, "characters": [{"slot": "addition/x", "catalog_id": "abcdefabcdefabcd",
                                                                   "match": "identical", "differs": []}]}
        write_install(d, official_content=["BLJM60180-01.20"])
        self.assertEqual(O.included(O.installed(d, m2), m2),
                         {"abcdefabcdefabcd": [{"id": "BLJM60180-01.20",
                                                "label": "Included with the official Japanese v1.20 update"}]})

    def test_never_reads_the_games_character_files(self):
        d = write_install(os.path.join(self.tmp, "d"), elf_sha256=V100_ELF,
                          official_content=["BLUS30490-01.00", "BLUS30490-01.01"])
        game = os.path.join(d, "data", "disc", "PS3_GAME", "USRDIR", "data", "edt", "addition")
        os.makedirs(game)
        with open(os.path.join(game, "05_001_X.EDT"), "wb") as f:
            f.write(b"not to be read")
        opened, real = [], builtins.open

        def spy(p, *a, **k):
            opened.append(os.path.abspath(str(p)))
            return real(p, *a, **k)
        builtins.open = spy
        try:
            O.included(O.installed(d, self.m), self.m)
        finally:
            builtins.open = real
        self.assertFalse([p for p in opened if os.path.join("data", "disc") in p or p.lower().endswith(".edt")])


class TheStore(StoreBase):
    """the store with official content: one entry per character, its sources apart, never downloaded twice"""

    def setUp(self):
        super().setUp()
        doc = self.doc()
        self.ids = [e["id"] for e in doc["catalog"]["characters"]]   # ALPHA, BETA, GAMMA (sorted by id)
        self.by_label = {e["label"]: e for e in doc["catalog"]["characters"]}
        # a test mapping: ALPHA identical, BETA the same model (another name), one official-only slot
        self.mapping = {
            "TEST00000-01.00": {"id": "TEST00000-01.00", "title_id": "TEST00000", "region": "US", "version": "01.00",
                                "kind": "game", "label": "Included with the game", "requires": None, "characters": []},
            "TEST00000-01.01": {"id": "TEST00000-01.01", "title_id": "TEST00000", "region": "US", "version": "01.01",
                                "kind": "update", "label": "Included with the official v1.01 update",
                                "requires": "TEST00000-01.00",
                                "characters": [
                                    {"slot": "addition/05_001", "catalog_id": self.by_label["Alpha"]["id"],
                                     "match": "identical", "differs": []},
                                    {"slot": "addition/05_002", "catalog_id": self.by_label["Beta"]["id"],
                                     "match": "same-model", "differs": ["name"]},
                                    {"slot": "addition/05_003", "catalog_id": None, "match": "none", "differs": []}]}}
        self.inst = write_install(os.path.join(self.tmp, "install"), official_content=["TEST00000-01.00"])

    def state(self, packages):
        write_install(self.inst, official_content=packages)
        inc = O.included(O.installed(self.inst, self.mapping), self.mapping)
        st = self.store()
        st.refresh()
        cat, problems = st.entries(self.savedata, included=inc)
        self.assertEqual(problems, [])
        return st, inc, {e["label"]: e for e in cat["characters"]}

    def snapshot(self):
        """the player's library and saves, file by file (the store's own cache, <library>/store, is the store's)"""
        out = {}
        store_dir = os.path.join(self.lib.root, "store")
        for root in (self.lib.root, self.savedata):
            for dp, dn, fn in os.walk(root):
                if os.path.abspath(dp).startswith(os.path.abspath(store_dir)):
                    continue
                for n in fn:
                    p = os.path.join(dp, n)
                    with open(p, "rb") as f:
                        out[p] = f.read()
        return out

    def test_v100_nothing_included_and_downloads_work(self):
        st, inc, got = self.state(["TEST00000-01.00"])
        self.assertEqual(inc, {})
        for e in got.values():
            self.assertEqual((e["included_with"], e["sources"]), ([], []))
        r = st.download(got["Alpha"]["id"], included=inc)
        self.assertEqual(r["status"], "imported")

    def test_v101_included_shown_installed_never_downloaded(self):
        st, inc, got = self.state(["TEST00000-01.00", "TEST00000-01.01"])
        for label in ("Alpha", "Beta"):   # the identical one and the same-model one: matched by the mapping
            self.assertEqual(got[label]["included_with"],
                             [{"id": "TEST00000-01.01", "label": "Included with the official v1.01 update"}])
            self.assertEqual(got[label]["sources"], [{"kind": "official", "package": "TEST00000-01.01",
                                                      "label": "Included with the official v1.01 update"}])
        self.assertEqual(got["Gamma"]["included_with"], [])
        self.assertEqual(len(got), 3)   # the official-only slot made no entry
        before, n = self.snapshot(), len(self.server.requests)
        for install in (False, True):
            r = st.download(got["Alpha"]["id"], self.savedata, install=install, included=inc)
            self.assertEqual((r["status"], r["id"]), ("official", None))
            self.assertIn("included with the official v1.01 update", r["message"])
        self.assertEqual((self.snapshot(), len(self.server.requests)), (before, n))   # nothing fetched or written

    def test_update_removed_then_the_store_offers_it_again(self):
        self.state(["TEST00000-01.00", "TEST00000-01.01"])
        st, inc, got = self.state(["TEST00000-01.00"])
        self.assertEqual(got["Alpha"]["included_with"], [])
        self.assertEqual(st.download(got["Alpha"]["id"], included=inc)["status"], "imported")

    def test_already_downloaded_or_imported_one_entry_sources_apart(self):
        st, inc, got = self.state(["TEST00000-01.00"])
        r = st.download(got["Alpha"]["id"], included=inc)           # the player downloaded ALPHA from the store
        gamma_file = os.path.join(self.tmp, "gamma-own.edt")         # and imported GAMMA themselves
        open(gamma_file, "wb").write(E.serialize(make_character(name="GAMMA", colour=(0.1, 0.1, 0.9))))
        imp = self.lib.import_candidates(__import__("char_library").find([gamma_file])[0])
        self.assertEqual(imp[0]["status"], "imported")
        lib_before = dict(self.lib.index())
        st, inc, got = self.state(["TEST00000-01.00", "TEST00000-01.01"])   # then installed the update
        a = got["Alpha"]
        self.assertEqual([s["kind"] for s in a["sources"]], ["official", "library"])
        self.assertEqual((a["sources"][1]["origin"], a["sources"][1]["library_id"]), ("store", r["id"]))
        self.assertEqual(a["included_with"][0]["label"], "Included with the official v1.01 update")   # not "downloaded"
        g = got["Gamma"]
        self.assertEqual(([s["kind"] for s in g["sources"]], g["sources"][0]["origin"]), (["library"], "import"))
        self.assertEqual(self.lib.index(), lib_before)   # the library untouched: nothing removed or added
        # personal imports of official characters stay allowed (the library has no official-content rule)
        alpha_copy = os.path.join(self.tmp, "alpha-copy.edt")
        open(alpha_copy, "wb").write(E.serialize(make_character(name="ALPHA", colour=(0.9, 0.1, 0.1))))
        rs = self.lib.import_candidates(__import__("char_library").find([alpha_copy])[0])
        self.assertIn(rs[0]["status"], ("imported", "duplicate", "similar"))

    def test_same_model_in_the_library_under_another_name(self):
        # the catalog gives the model's hash; the player's copy has another in-game name (the six same-model ones)
        doc = self.doc()
        beta = next(e for e in doc["catalog"]["characters"] if e["label"] == "Beta")
        mine = make_character(name="BETA RENAMED", colour=(0.1, 0.9, 0.1))
        beta["shape_sha256"] = E.shape_hash(mine)
        self.server.override[S.CATALOG_PATH] = json.dumps(self.key.sign_doc(doc)).encode()
        p = os.path.join(self.tmp, "beta-renamed.edt")
        open(p, "wb").write(E.serialize(mine))
        lid = self.lib.import_candidates(__import__("char_library").find([p])[0])[0]["id"]
        st, inc, got = self.state(["TEST00000-01.00"])
        b = got["Beta"]
        self.assertEqual((b["in_library"], b["same_model_in_library"]), ("", lid))
        self.assertEqual((b["sources"][0]["kind"], b["sources"][0]["same_model"]), ("library", True))

    def test_entries_never_write(self):
        before = self.snapshot()
        for pk in (["TEST00000-01.00"], ["TEST00000-01.00", "TEST00000-01.01"], []):
            self.state(pk)
        self.assertEqual(self.snapshot(), before)

    def test_nothing_publishes_or_uploads(self):
        for mod in (S, O, C):
            src = open(mod.__file__, encoding="utf-8").read()
            self.assertIsNone(re.search(r"method\s*=\s*[\"'](POST|PUT|PATCH|DELETE)", src), mod.__name__)
            self.assertNotIn("urlopen(Request(", src.replace(" ", ""))


@unittest.skipUnless(os.path.isfile(os.path.join(ROOT, "build", "store-repo", "catalog", "v1", "catalog.json")),
                     "the store repository's working copy is the maintainers' (local)")
class TheHistoricalCatalog(unittest.TestCase):
    """the maintainers' working copy of the published repository: all 352 recovered characters kept, downloadable, with
    their attribution and provenance and the removal-request address; the mapping's 45 among them"""

    def test_all_352_kept_downloadable_and_the_45_mapped(self):
        repo = os.path.join(ROOT, "build", "store-repo")
        doc = json.load(open(os.path.join(repo, "catalog", "v1", "catalog.json"), encoding="utf-8"))
        hist = [e for e in doc["catalog"]["characters"] if e.get("category") == "historical"]
        self.assertEqual(len(hist), 352)
        ids = {e["id"] for e in hist}
        mapped = [c["catalog_id"] for c in O.load()["BLUS30490-01.01"]["characters"] if c["catalog_id"]]
        self.assertEqual((len(mapped), len(set(mapped) - ids)), (45, 0))
        self.assertTrue(doc["catalog"].get("removal_url", "").startswith("https://"))
        for e in hist:   # provenance, attribution and the files: as published
            self.assertTrue(e["source"]["collection"] and e["creator"] and e["attribution"])
            for kind in ("file", "thumb"):
                self.assertTrue(os.path.isfile(os.path.join(repo, *e[kind]["path"].split("/"))), (e["id"], kind))
        official_only = [c for c in O.load()["BLUS30490-01.01"]["characters"] if not c["catalog_id"]]
        self.assertEqual(len(official_only), 6)   # outside the catalog (unless added through the normal process)


@unittest.skipUnless(os.path.isfile(os.path.join(ROOT, "build", "v101", "official_map.json")),
                     "the mapping's private verification is the maintainers' (local)")
class ThePrivateVerification(unittest.TestCase):
    """the mapping agrees with its verification against the player's own files (kept private, never committed)"""

    def test_agrees(self):
        ver = {r["slot"]: r for r in json.load(open(os.path.join(ROOT, "build", "v101", "official_map.json"),
                                                    encoding="utf-8"))}
        kinds = {"identical": "identical", "same-shape": "same-model", "none": "none"}
        for c in O.load()["BLUS30490-01.01"]["characters"]:
            r = ver[c["slot"].split("/")[-1]]
            self.assertEqual((c["catalog_id"], c["match"]), (r.get("catalog_id"), kinds[r["match"]]), c["slot"])


if __name__ == "__main__":
    unittest.main()
