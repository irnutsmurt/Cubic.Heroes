#!/usr/bin/env python3
"""The game's Steam library artwork from the player's own image files (tools/shortcuts.py import_steam_art, the
worker's steam-art; 0.2.0-beta.5). Nothing is downloaded: the player downloads the images and the launcher places
them. Made-up Steam folders and made-up images only.
    build/venv/Scripts/python.exe -m unittest tests.test_steam_art"""
import json
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import shortcuts  # noqa: E402

PNG = b"\x89PNG\r\n\x1a\n" + b"\0\0\0\rIHDR" + b"made-up poster" * 10
JPG = b"\xff\xd8\xff\xe0" + b"made-up hero" * 10
LOGO = PNG + b"made-up logo"


class Fixture(unittest.TestCase):
    """two Steam accounts with the game added (no artwork)"""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="steam-art-", dir=os.environ.get("TMPDIR"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.root = os.path.join(self.tmp, "Steam")
        self.exe = os.path.join(self.tmp, "Cubic.Heroes", "CubicHeroes.exe")
        self.accounts = [os.path.join(self.root, "userdata", a, "config") for a in ("111", "222")]
        for c in self.accounts:
            os.makedirs(c)
        shortcuts.add_to_steam(self.exe, "", roots=[self.root], artwork=False)
        self.appid = shortcuts.steam_app_id(f'"{self.exe}"', shortcuts.NAME)

    def image(self, name, data):
        p = os.path.join(self.tmp, name)
        with open(p, "wb") as f:
            f.write(data)
        return p

    def grid(self, account, name):
        return os.path.join(self.accounts[account], "grid", name)


class SteamArt(Fixture):
    """the player's own files, placed (import_steam_art)"""

    def test_placed_in_every_account_with_the_game(self):
        r = shortcuts.import_steam_art(self.exe, "poster", self.image("poster.png", PNG), roots=[self.root])
        self.assertEqual((r["accounts"], len(r["placed"]), r["kept"]), (2, 2, []))
        for a in (0, 1):
            with open(self.grid(a, f"{self.appid}p.png"), "rb") as f:
                self.assertEqual(f.read(), PNG)
        r = shortcuts.import_steam_art(self.exe, "hero", self.image("hero.jpg", JPG), roots=[self.root])
        self.assertTrue(os.path.isfile(self.grid(0, f"{self.appid}_hero.jpg")))

    def test_an_account_without_the_game_is_left_alone(self):
        other = os.path.join(self.root, "userdata", "333", "config")
        os.makedirs(other)
        r = shortcuts.import_steam_art(self.exe, "poster", self.image("p.png", PNG), roots=[self.root])
        self.assertEqual(r["accounts"], 2)
        self.assertFalse(os.path.exists(os.path.join(other, "grid")))

    def test_only_png_or_jpeg_by_its_bytes(self):
        fake = self.image("not-really.png", b"GIF89a" + b"x" * 100)
        with self.assertRaises(shortcuts.ArtError) as e:
            shortcuts.import_steam_art(self.exe, "poster", fake, roots=[self.root])
        self.assertEqual(e.exception.code, "not_an_image")
        named_wrong = self.image("poster.txt", PNG)   # the name doesn't matter, the bytes do; written as .png
        shortcuts.import_steam_art(self.exe, "poster", named_wrong, roots=[self.root])
        self.assertTrue(os.path.isfile(self.grid(0, f"{self.appid}p.png")))

    def test_size_limit(self):
        big = self.image("big.png", PNG + b"\0" * (shortcuts.ART_MAX_BYTES + 1))
        with self.assertRaises(shortcuts.ArtError) as e:
            shortcuts.import_steam_art(self.exe, "poster", big, roots=[self.root])
        self.assertEqual(e.exception.code, "image_too_large")
        self.assertFalse(os.path.exists(self.grid(0, f"{self.appid}p.png")))

    def test_the_players_own_artwork_is_kept(self):
        own = self.grid(0, f"{self.appid}p.jpg")
        os.makedirs(os.path.dirname(own))
        with open(own, "wb") as f:
            f.write(JPG)   # the player's own poster, put there by them
        r = shortcuts.import_steam_art(self.exe, "poster", self.image("p.png", PNG), roots=[self.root])
        self.assertEqual(r["kept"], [own])
        with open(own, "rb") as f:
            self.assertEqual(f.read(), JPG)
        self.assertFalse(os.path.exists(self.grid(0, f"{self.appid}p.png")))
        self.assertTrue(os.path.isfile(self.grid(1, f"{self.appid}p.png")))   # the other account had none: placed
        # replaced only when the player says so
        r = shortcuts.import_steam_art(self.exe, "poster", self.image("p.png", PNG), roots=[self.root], replace=True)
        self.assertEqual(r["kept"], [])
        self.assertFalse(os.path.exists(own))
        self.assertTrue(os.path.isfile(self.grid(0, f"{self.appid}p.png")))

    def test_our_own_is_replaced(self):
        shortcuts.import_steam_art(self.exe, "poster", self.image("a.png", PNG), roots=[self.root])
        r = shortcuts.import_steam_art(self.exe, "poster", self.image("b.jpg", JPG), roots=[self.root])
        self.assertEqual((len(r["placed"]), r["kept"]), (2, []))
        self.assertFalse(os.path.exists(self.grid(0, f"{self.appid}p.png")))   # one file per slot
        with open(self.grid(0, f"{self.appid}p.jpg"), "rb") as f:
            self.assertEqual(f.read(), JPG)
        with open(self.grid(0, shortcuts.ART_RECORD), encoding="utf-8") as f:
            self.assertEqual(list(json.load(f)), [f"{self.appid}p.jpg"])

    def test_ours_changed_by_the_player_counts_as_theirs(self):
        shortcuts.import_steam_art(self.exe, "poster", self.image("a.png", PNG), roots=[self.root])
        with open(self.grid(0, f"{self.appid}p.png"), "wb") as f:
            f.write(PNG + b"edited")
        r = shortcuts.import_steam_art(self.exe, "poster", self.image("b.png", PNG + b"2"), roots=[self.root])
        self.assertEqual(r["kept"], [self.grid(0, f"{self.appid}p.png")])

    def test_no_steam_and_game_not_added(self):
        with self.assertRaises(shortcuts.ArtError) as e:
            shortcuts.import_steam_art(self.exe, "poster", self.image("p.png", PNG), roots=[])
        self.assertEqual(e.exception.code, "no_steam")
        with self.assertRaises(shortcuts.ArtError) as e:
            shortcuts.import_steam_art(self.exe + "x", "poster", self.image("p.png", PNG), roots=[self.root])
        self.assertEqual(e.exception.code, "not_in_steam")

    def test_the_owners_two_images(self):
        self.assertEqual({k: v[:2] for k, v in shortcuts.STEAM_ART.items()}, {
            "poster": ("p", "https://cdn2.steamgriddb.com/grid/eaccbf3d792ad75cc1ad699862126724.png"),
            "hero": ("_hero", "https://cdn2.steamgriddb.com/hero/cbe68589759f566354b16b634b2ac6b8.png")})


class FakeResponse:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def read(self, n=-1):
        n = len(self.data) - self.pos if n is None or n < 0 else n
        b = self.data[self.pos:self.pos + n]
        self.pos += len(b)
        return b

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


def fake_net(answers):
    """urlopen stood in for: answers -- one for every URL, or {url: answer}; an answer is bytes or an exception"""
    calls = []

    def urlopen(req, timeout):
        calls.append((req.full_url, req.get_header("User-agent"), timeout))
        a = answers[req.full_url] if isinstance(answers, dict) else answers
        if isinstance(a, BaseException):
            raise a
        return FakeResponse(a)
    urlopen.calls = calls
    return urlopen


POSTER_URL = shortcuts.STEAM_ART["poster"][1]
HERO_URL = shortcuts.STEAM_ART["hero"][1]


class SteamArtDownload(Fixture):
    """Add to Steam downloads the two images once, on the player's computer; any failure leaves the game added
    (no real network: urlopen is stood in for)"""

    def add(self, net):
        report = {}
        written = shortcuts.add_to_steam(self.exe, "", roots=[self.root], report=report,
                                         fetch=lambda causes: shortcuts.fetch_steam_art(urlopen=net, causes=causes),
                                         logo=self.image("logo.png", LOGO))
        vdfs = [p for p in written if os.path.basename(p) == "shortcuts.vdf"]
        self.assertEqual(len(vdfs), 2, "the game is in both accounts' libraries whatever the artwork did")
        return report

    def test_png_downloaded_once_each_and_placed(self):
        net = fake_net({POSTER_URL: PNG, HERO_URL: PNG + b"hero"})
        r = self.add(net)
        self.assertEqual((len(r["placed"]), r["kept"], r["failed"]), (6, [], {}))   # poster, hero, logo x 2 accounts
        self.assertEqual(sorted(u for u, _, _ in net.calls), sorted([POSTER_URL, HERO_URL]))   # once each, not per account
        self.assertTrue(all(ua.startswith("Cubic.Heroes launcher ") and t == shortcuts.ART_TIMEOUT_S
                            for _, ua, t in net.calls))
        for a in (0, 1):
            with open(self.grid(a, f"{self.appid}p.png"), "rb") as f:
                self.assertEqual(f.read(), PNG)
            with open(self.grid(a, f"{self.appid}_hero.png"), "rb") as f:
                self.assertEqual(f.read(), PNG + b"hero")

    def test_an_error_page_or_a_jpeg_refused(self):
        r = self.add(fake_net({POSTER_URL: b"<!DOCTYPE html><html>Not found</html>", HERO_URL: JPG}))
        self.assertEqual([os.path.basename(p) for p in r["placed"]], [f"{self.appid}_logo.png"] * 2)   # only the logo
        self.assertEqual(sorted(r["failed"]), ["hero", "poster"])
        self.assertIn("not a PNG", r["causes"]["poster"])   # the cause kept; the player reads plain words
        self.assertEqual(r["failed"]["poster"], "the download wasn't a usable image")
        self.assertFalse(os.path.exists(os.path.join(self.accounts[0], "grid", f"{self.appid}p.png")))

    def test_oversize_refused_while_reading(self):
        r = self.add(fake_net(PNG + b"\0" * shortcuts.ART_MAX_BYTES))
        self.assertEqual(sorted(r["failed"]), ["hero", "poster"])
        self.assertIn("larger than", r["causes"]["poster"])
        self.assertEqual(r["failed"]["poster"], "the download wasn't a usable image")

    def test_a_timeout_handled(self):
        import socket
        r = self.add(fake_net(socket.timeout("timed out")))
        self.assertEqual(sorted(r["failed"]), ["hero", "poster"])
        self.assertEqual(r["failed"]["poster"], "it couldn't be downloaded -- check the internet connection")

    def test_failures_in_plain_words(self):
        """what the launcher shows: never a raw urlopen error (the owner's Steam test, beta.5)"""
        import urllib.error
        for err, words in ((ConnectionRefusedError(10061, "refused"), "check the internet connection"),
                           (urllib.error.URLError(ConnectionRefusedError(10061, "refused")), "check the internet connection"),
                           (urllib.error.HTTPError(POSTER_URL, 404, "Not Found", {}, None), "answered with an error (404)")):
            r = self.add(fake_net(err))
            self.assertIn(words, r["failed"]["poster"], err)
            self.assertNotIn("urlopen", r["failed"]["poster"])
            self.assertTrue(r["causes"]["poster"], err)

    def test_add_to_steam_succeeds_with_every_artwork_failure(self):
        import urllib.error
        for err in (urllib.error.URLError("no route"), urllib.error.HTTPError(POSTER_URL, 503, "busy", {}, None),
                    ConnectionResetError("reset"), ValueError("bad"), RuntimeError("anything else")):
            r = self.add(fake_net(err))
            self.assertEqual((len(r["placed"]), sorted(r["failed"])), (2, ["hero", "poster"]), err)   # the logos
        report = {}   # even a fetch that raises
        shortcuts.add_to_steam(self.exe, "", roots=[self.root], report=report, fetch=lambda **_: 1 / 0)
        self.assertEqual(list(report["failed"]), ["artwork"])

    def test_the_players_own_kept(self):
        own = self.grid(0, f"{self.appid}p.jpg")
        os.makedirs(os.path.dirname(own))
        with open(own, "wb") as f:
            f.write(JPG)
        r = self.add(fake_net(PNG))
        self.assertEqual(r["kept"], [own])
        with open(own, "rb") as f:
            self.assertEqual(f.read(), JPG)
        self.assertFalse(os.path.exists(self.grid(0, f"{self.appid}p.png")))
        self.assertTrue(os.path.isfile(self.grid(0, f"{self.appid}_hero.png")))   # the other slot was free

    def test_our_own_replaced(self):
        self.add(fake_net(PNG))
        r = self.add(fake_net(PNG + b"newer"))
        self.assertEqual((len(r["placed"]), r["kept"]), (6, []))
        with open(self.grid(0, f"{self.appid}p.png"), "rb") as f:
            self.assertEqual(f.read(), PNG + b"newer")

    def test_artwork_false_never_fetches(self):
        def no_fetch(**_):
            raise AssertionError("fetched")
        report = {}
        shortcuts.add_to_steam(self.exe, "", roots=[self.root], artwork=False, report=report, fetch=no_fetch)
        self.assertEqual(report, {"placed": [], "kept": [], "failed": {}, "causes": {}})

    def test_download_steam_art_never_over_the_players_own(self):
        g = os.path.join(self.tmp, "grid")
        os.makedirs(g)
        own = os.path.join(g, f"{self.appid}p.png")
        with open(own, "wb") as f:
            f.write(PNG + b"own")
        p, kept = shortcuts._place_art(g, self.appid, "p", JPG, ".jpg")
        self.assertEqual((p, kept), (None, [own]))
        with open(own, "rb") as f:
            self.assertEqual(f.read(), PNG + b"own")


class SteamLogo(Fixture):
    """the project's own logo (assets/steam-logo.png, shipped with Cubic.Heroes; nothing downloaded) placed by Add to
    Steam as <appid>_logo.png, by the same rules as the other artwork"""

    def add(self, **kw):
        report = {}
        shortcuts.add_to_steam(self.exe, "", roots=[self.root], report=report, fetch=lambda **_: ({}, {}), **kw)
        return report

    def test_the_shipped_logo_placed(self):
        r = self.add()
        self.assertEqual(r["failed"], {})
        with open(os.path.join(ROOT, "assets", "steam-logo.png"), "rb") as f:
            shipped = f.read()
        self.assertTrue(shipped.startswith(b"\x89PNG\r\n\x1a\n") and len(shipped) <= shortcuts.ART_MAX_BYTES)
        for a in (0, 1):
            with open(self.grid(a, f"{self.appid}_logo.png"), "rb") as f:
                self.assertEqual(f.read(), shipped)

    def test_the_players_own_logo_kept(self):
        own = self.grid(1, f"{self.appid}_logo.png")
        os.makedirs(os.path.dirname(own))
        with open(own, "wb") as f:
            f.write(PNG + b"mine")
        r = self.add()
        self.assertEqual(r["kept"], [own])
        with open(own, "rb") as f:
            self.assertEqual(f.read(), PNG + b"mine")

    def test_a_missing_or_bad_logo_never_stops_the_game(self):
        for logo in (os.path.join(self.tmp, "nowhere.png"), self.image("bad.png", JPG)):
            r = self.add(logo=logo)
            self.assertEqual(list(r["failed"]), ["logo"])
            for c in self.accounts:
                self.assertEqual(len(shortcuts.read_shortcuts(os.path.join(c, "shortcuts.vdf"))), 1)


@unittest.skipIf(os.name == "nt", "Linux's Steam folders")
class LinuxSteamRoots(unittest.TestCase):
    """every Linux Steam found by its userdata folder: native (the Steam Deck's too), Flatpak and Ubuntu's Snap"""

    def test_native_flatpak_and_snap(self):
        home = tempfile.mkdtemp(prefix="steam-home-", dir=os.environ.get("TMPDIR"))
        self.addCleanup(shutil.rmtree, home, True)
        kinds = [".local/share/Steam", ".var/app/com.valvesoftware.Steam/data/Steam",
                 "snap/steam/common/.local/share/Steam"]
        for k in kinds:
            os.makedirs(os.path.join(home, k, "userdata"))
        with mock.patch.dict(os.environ, {"HOME": home}):
            roots = shortcuts.steam_roots()
        self.assertEqual(sorted(roots), sorted(os.path.realpath(os.path.join(home, k)) for k in kinds))


if __name__ == "__main__":
    unittest.main()
