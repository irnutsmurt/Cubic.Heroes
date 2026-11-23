#!/usr/bin/env python3
"""The disc's files checked against the game's file list and installed by way of a staging folder
(tools/disc_source.py, tools/disc_manifest.py; 0.2.0-beta.5):
    build/venv/Scripts/python.exe -m unittest tests.test_disc_source
Made-up files and images (tests/iso_build.py), never the game's: every source kind gives the same installed files
(an ISO with and without Joliet, rebuilt in another order with gaps and padding, a SYNTHETIC encrypted image, the
folder, the folder with its names in another case); missing, resized, modified, duplicate and linked files refused;
an installed copy kept when a new one fails, an interrupted swap finished or undone; the manifest tool refuses
any image but the verified dump, and its output is the same bytes every time. The real config/disc_manifest.json is
checked against config/game_version.json."""
import hashlib
import json
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)
import disc_manifest  # noqa: E402
import disc_source as ds  # noqa: E402
import iso_build  # noqa: E402
import ps3_disc  # noqa: E402

FILES = {
    "PS3_GAME/PARAM.SFO": b"\0PSF made-up",
    "PS3_GAME/ICON0.PNG": b"made-up icon" * 50,
    "PS3_GAME/USRDIR/EBOOT.BIN": b"SCE\0" + b"made-up program " * 300,
    "PS3_GAME/USRDIR/data/all.lua.eu": b"-- made-up script\n" * 400,
    "PS3_GAME/USRDIR/data/Map/Field01.dat": bytes(range(256)) * 37,
    "PS3_GAME/USRDIR/data/Map/empty_ok.bin": b"x",
}


def sha(b):
    return hashlib.sha256(b).hexdigest()


def made_up_manifest(files=FILES):
    return {"format": 1, "files": [{"path": p, "size": len(b), "sha256": sha(b)} for p, b in sorted(files.items())]}


def tree(top):
    """{relative path: bytes} of everything under top (the staging marker files excluded)"""
    out = {}
    for d, _, fs in os.walk(top):
        for n in fs:
            p = os.path.join(d, n)
            with open(p, "rb") as f:
                out[os.path.relpath(p, top).replace(os.sep, "/")] = f.read()
    return out


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR") or None)
        self.addCleanup(self.tmp.cleanup)
        self.d = self.tmp.name
        self.m = made_up_manifest()
        self.disc = os.path.join(self.d, "install", "data", "disc")

    def folder(self, files=FILES, name="folder", case=lambda s: s):
        top = os.path.join(self.d, name)
        for p, b in files.items():
            q = os.path.join(top, *[case(c) for c in p.split("/")])
            os.makedirs(os.path.dirname(q), exist_ok=True)
            with open(q, "wb") as f:
                f.write(b)
        return top

    def iso(self, files=FILES, name="disc.iso", **kw):
        p = os.path.join(self.d, name)
        iso_build.build(p, files, **kw)
        return p

    def install(self, source, kind, key=None, **kw):
        if kind == "folder":
            return ds.install(source, "folder", self.disc, m=self.m, **kw)
        with open(source, "rb") as raw:
            f = ps3_disc.DiscImage(raw, key) if key else raw
            return ds.install(source, "iso", self.disc, f=f, m=self.m, **kw)


class SameFilesFromEverySource(Base):
    def test_every_source_kind_installs_the_same_files(self):
        want = {p: b for p, b in FILES.items()}
        key = os.urandom(16)
        plain = self.iso(name="plain.iso")
        enc = os.path.join(self.d, "synthetic-encrypted.iso")
        iso_build.encrypt_synthetic(plain, enc, key)
        sources = [
            ("iso, Joliet", plain, "iso", None),
            ("iso, no Joliet (upper-case names)", self.iso(name="nojoliet.iso", joliet=False), "iso", None),
            ("iso rebuilt: reversed, gaps, padding", self.iso(name="rebuilt.iso", order="reverse", gap=3, pad=17),
             "iso", None),
            ("iso rebuilt: by size, no Joliet", self.iso(name="rebuilt2.iso", order="size", joliet=False), "iso",
             None),
            ("SYNTHETIC encrypted iso", enc, "iso", key),
            ("folder", self.folder(), "folder", None),
            ("folder, lower-case names", self.folder(name="lower", case=str.lower), "folder", None),
        ]
        for label, src, kind, k in sources:
            with self.subTest(source=label):
                if os.path.isdir(self.disc):
                    shutil.rmtree(self.disc)
                n, extras = self.install(src, kind, k)
                self.assertEqual(n, len(FILES))
                self.assertEqual(tree(self.disc), want)   # the manifest's names, whatever the source's case
                self.assertFalse(os.path.exists(self.disc + ".staging") or os.path.exists(self.disc + ".old"))

    def test_synthetic_encrypted_image_reads_as_encrypted(self):
        plain = self.iso(name="plain.iso")
        enc = os.path.join(self.d, "synthetic-encrypted.iso")
        key = os.urandom(16)
        iso_build.encrypt_synthetic(plain, enc, key)
        with open(enc, "rb") as f:
            self.assertEqual(ps3_disc.kind(f), "encrypted")
            self.assertTrue(ps3_disc.check_key(f, key))
            self.assertFalse(ps3_disc.check_key(f, os.urandom(16)))
        with self.assertRaises(ds.Refused):   # its bytes without the key: the files don't match
            self.install(enc, "iso")
        self.assertFalse(os.path.exists(self.disc))

    def test_extras_skipped_and_named(self):
        files = dict(FILES, **{"PS3_GAME/USRDIR/EBOOT.elf": b"\x7fELF beside the program", "PS3_GAME/Thumbs.db": b"t"})
        logged = []
        n, extras = ds.install(self.folder(files), "folder", self.disc, m=self.m, log=logged.append)
        self.assertEqual((n, extras), (len(FILES), ["PS3_GAME/Thumbs.db", "PS3_GAME/USRDIR/EBOOT.elf"]))
        self.assertEqual(tree(self.disc), FILES)
        self.assertTrue(any("Thumbs.db" in s for s in logged))

    def test_check_writes_nothing(self):
        src = self.folder()
        self.assertEqual(ds.check(src, "folder", m=self.m), (len(FILES), []))
        self.assertFalse(os.path.exists(os.path.join(self.d, "install")))


class Refusals(Base):
    def refused(self, source, kind, code="damaged_disc", key=None):
        with self.assertRaises(ds.Refused) as e:
            self.install(source, kind, key)
        self.assertEqual(e.exception.code, code, str(e.exception))
        self.assertFalse(os.path.exists(self.disc + ".staging"), "the staging folder was left behind")
        return str(e.exception)

    def test_missing_file(self):
        files = dict(FILES)
        del files["PS3_GAME/USRDIR/data/all.lua.eu"]
        for src, kind in ((self.iso(files), "iso"), (self.folder(files), "folder")):
            self.assertIn("all.lua.eu is missing", self.refused(src, kind))

    def test_other_size_refused_before_anything_is_written(self):
        files = dict(FILES, **{"PS3_GAME/ICON0.PNG": b"another icon"})
        with mock.patch.object(ds, "copy_checked", side_effect=AssertionError("read")):
            self.assertIn("ICON0.PNG", self.refused(self.iso(files), "iso"))
        self.assertFalse(os.path.exists(self.disc))

    def test_modified_file(self):
        b = bytearray(FILES["PS3_GAME/USRDIR/data/Map/Field01.dat"])
        b[1000] ^= 1
        files = dict(FILES, **{"PS3_GAME/USRDIR/data/Map/Field01.dat": bytes(b)})
        for src, kind in ((self.iso(files), "iso"), (self.folder(files, name="f2"), "folder")):
            self.assertIn("Field01.dat doesn't match", self.refused(src, kind))
        self.assertFalse(os.path.exists(self.disc))

    @unittest.skipIf(os.name == "nt", "names differing only in case can't both exist on Windows' file systems")
    def test_folder_case_collision(self):
        top = self.folder()
        with open(os.path.join(top, "PS3_GAME", "usrdir.txt"), "wb"):
            pass
        os.makedirs(os.path.join(top, "PS3_GAME", "usrdir"))
        self.assertIn("only in case", self.refused(top, "folder"))

    def link(self, target, at):
        if os.name == "nt":
            import _winapi
            _winapi.CreateJunction(target, at)   # no administrator needed, unlike a symbolic link
        else:
            os.symlink(target, at)

    def test_folder_with_a_link_refused(self):
        outside = os.path.join(self.d, "outside")
        os.makedirs(outside)
        with open(os.path.join(outside, "secret.txt"), "w") as f:
            f.write("not the disc's")
        top = self.folder()
        self.link(outside, os.path.join(top, "PS3_GAME", "USRDIR", "data", "linked"))
        self.assertIn("link", self.refused(top, "folder"))

    def test_folder_ps3_game_itself_a_link(self):
        real = self.folder(name="real")
        top = os.path.join(self.d, "top")
        os.makedirs(top)
        self.link(os.path.join(real, "PS3_GAME"), os.path.join(top, "PS3_GAME"))
        self.refused(top, "folder", "not_a_disc")

    @unittest.skipUnless(hasattr(os, "mkfifo"), "a FIFO: POSIX")
    def test_folder_special_file(self):
        top = self.folder()
        os.mkfifo(os.path.join(top, "PS3_GAME", "pipe"))
        self.assertIn("isn't a file", self.refused(top, "folder"))

    def test_not_enough_space(self):
        free = shutil.disk_usage(self.d)._replace(free=1000)
        with mock.patch.object(ds.shutil, "disk_usage", return_value=free):
            self.refused(self.folder(), "folder", "disk_space")

    def test_install_disc_folder_a_link(self):
        os.makedirs(os.path.dirname(self.disc))
        elsewhere = os.path.join(self.d, "elsewhere")
        os.makedirs(elsewhere)
        self.link(elsewhere, self.disc)
        self.refused(self.folder(), "folder", "root_not_writable")
        self.assertEqual(os.listdir(elsewhere), [])
        ds.recover(self.disc)   # an update (no disc chosen) of such an install is not stopped by it
        for leftover in (".old", ".staging"):   # but what recovery would move or remove is never a link
            self.link(elsewhere, self.disc + leftover)
            with self.assertRaises(ds.Refused):
                ds.recover(self.disc)
            os.rmdir(self.disc + leftover) if os.name == "nt" else os.unlink(self.disc + leftover)
        self.assertEqual(os.listdir(elsewhere), [])

    def test_damaged_manifest_refused(self):
        bad = [
            {"format": 1, "files": [{"path": "PS3_GAME/../x", "size": 1, "sha256": "0" * 64}]},
            {"format": 1, "files": [{"path": "/etc/x", "size": 1, "sha256": "0" * 64}]},
            {"format": 1, "files": [{"path": "PS3_GAME/A", "size": 1, "sha256": "0" * 64},
                                    {"path": "PS3_GAME/a", "size": 1, "sha256": "0" * 64}]},
            {"format": 2, "files": [{"path": "PS3_GAME/A", "size": 1, "sha256": "0" * 64}]},
            {"format": 1, "files": []},
        ]
        for m in bad:
            p = os.path.join(self.d, "m.json")
            with open(p, "w") as f:
                json.dump(m, f)
            with self.subTest(m=m), self.assertRaises(ds.Refused):
                ds.manifest(p)


class StagingAndRecovery(Base):
    def installed(self, content=b"the copy already installed"):
        p = os.path.join(self.disc, "PS3_GAME", "USRDIR", "EBOOT.BIN")
        os.makedirs(os.path.dirname(p))
        with open(p, "wb") as f:
            f.write(content)
        saves = os.path.join(os.path.dirname(self.disc), "gamedata", "save.dat")
        os.makedirs(os.path.dirname(saves), exist_ok=True)
        with open(saves, "wb") as f:
            f.write(b"a save")
        return p

    def test_failed_install_keeps_the_installed_copy_and_saves(self):
        p = self.installed()
        files = dict(FILES)
        del files["PS3_GAME/ICON0.PNG"]
        with self.assertRaises(ds.Refused):
            self.install(self.folder(files), "folder")
        with open(p, "rb") as f:
            self.assertEqual(f.read(), b"the copy already installed")
        self.assertEqual(tree(os.path.join(os.path.dirname(self.disc), "gamedata")), {"save.dat": b"a save"})

    def test_interrupted_extraction_keeps_the_installed_copy(self):
        p = self.installed()

        class Stop(BaseException):
            pass

        def progress(x):
            if x > 0.3:
                raise Stop()
        with self.assertRaises(Stop):
            self.install(self.iso(), "iso", progress=progress)
        with open(p, "rb") as f:
            self.assertEqual(f.read(), b"the copy already installed")
        self.assertFalse(os.path.exists(self.disc + ".staging"))

    def test_successful_install_replaces_the_copy_saves_untouched(self):
        self.installed()
        self.install(self.iso(), "iso")
        self.assertEqual(tree(self.disc), FILES)
        self.assertEqual(tree(os.path.join(os.path.dirname(self.disc), "gamedata")), {"save.dat": b"a save"})
        self.assertFalse(os.path.exists(self.disc + ".old"))

    def test_valid_decrypted_program_carried_across(self):
        elf = b"\x7fELF the supported decrypted program"
        self.installed()
        with open(os.path.join(self.disc, "PS3_GAME", "USRDIR", "EBOOT.elf"), "wb") as f:
            f.write(elf)
        self.install(self.iso(), "iso", keep_elf_sha256=sha(elf))
        self.assertEqual(tree(self.disc), dict(FILES, **{"PS3_GAME/USRDIR/EBOOT.elf": elf}))
        shutil.rmtree(self.disc)
        self.installed()
        with open(os.path.join(self.disc, "PS3_GAME", "USRDIR", "EBOOT.elf"), "wb") as f:
            f.write(b"\x7fELF something else")
        self.install(self.iso(name="2.iso"), "iso", keep_elf_sha256=sha(elf))
        self.assertEqual(tree(self.disc), FILES)   # not the supported program: not carried

    def test_recover_an_interrupted_swap(self):
        old, staging = self.disc + ".old", self.disc + ".staging"
        os.makedirs(os.path.join(old, "x"))
        os.makedirs(staging)
        ds.recover(self.disc)   # the copy moved aside, the new one not yet in place: the old one comes back
        self.assertTrue(os.path.isdir(os.path.join(self.disc, "x")))
        self.assertFalse(os.path.exists(old) or os.path.exists(staging))
        os.makedirs(os.path.join(old, "y"))
        ds.recover(self.disc)   # both there: the swap finished, the old one is removed
        self.assertTrue(os.path.isdir(os.path.join(self.disc, "x")))
        self.assertFalse(os.path.exists(old))

    def test_a_failed_swap_puts_the_copy_back(self):
        self.installed()
        real = os.replace

        def replace(a, b):
            if a.endswith(".staging"):
                raise OSError(13, "in use")
            return real(a, b)
        with mock.patch.object(ds.os, "replace", side_effect=replace), self.assertRaises(OSError):
            self.install(self.iso(), "iso")
        self.assertEqual(tree(self.disc), {"PS3_GAME/USRDIR/EBOOT.BIN": b"the copy already installed"})

    def test_pipeline_extract_and_run_recover(self):
        """setup_pipeline.extract goes through all this; its refusal is a SetupError with the code"""
        import setup_pipeline as sp
        paths = type("P", (), {"disc": self.disc, "eboot_bin": os.path.join(self.disc, "x")})()
        with mock.patch.object(ds, "manifest", lambda path=None: self.m):
            sp.extract(self.folder(), "folder", paths, lambda f: None, lambda s: None)
            self.assertEqual(tree(self.disc), FILES)
            files = dict(FILES)
            del files["PS3_GAME/PARAM.SFO"]
            with self.assertRaises(sp.SetupError) as e:
                sp.extract(self.iso(files), "iso", paths, lambda f: None, lambda s: None)
        self.assertEqual(e.exception.code, "damaged_disc")
        self.assertEqual(tree(self.disc), FILES)

    def test_folder_names_in_another_case_probe(self):
        import setup_pipeline as sp
        top = self.folder(case=str.lower)
        self.assertEqual(ds.folder_path(top, "PS3_GAME/USRDIR/EBOOT.BIN"),
                         os.path.join(top, "ps3_game", "usrdir", "eboot.bin"))
        self.assertIsNone(ds.folder_path(top, "PS3_GAME/USRDIR/NOPE.BIN"))
        self.assertEqual(sp._disc_file(top, "PS3_GAME/PARAM.SFO"), FILES["PS3_GAME/PARAM.SFO"])


class VerifyNeverBySize(Base):
    """setup_pipeline.verify: the whole image's SHA-256 is the canonical dump's fast path; any other decrypted image
    -- of the dump's very size or not (makeps3iso rebuilds the folder into an image of exactly its size) -- is
    checked by its title, version and program, then file by file as it is extracted"""

    def setUp(self):
        super().setUp()
        import setup_pipeline as sp
        import test_rebuilt_disc as rd
        self.sp, self.files = sp, rd.FILES
        self.m = made_up_manifest(self.files)
        self.rebuilt = self.iso(self.files, name="rebuilt.iso", order="reverse", gap=5)
        self.want = dict(rd.WANT, disc_image={"size": os.path.getsize(self.rebuilt), "sha256": "0" * 64})
        for target, attr, value in ((sp, "version_info", lambda: self.want),
                                    (ds, "manifest", lambda path=None: self.m)):
            p = mock.patch.object(target, attr, value)
            p.start()
            self.addCleanup(p.stop)

    def paths(self):
        return type("P", (), {"disc": self.disc, "eboot_bin": os.path.join(self.disc, "x")})()

    def test_canonical_size_other_layout_accepted_by_its_files(self):
        self.assertEqual(os.path.getsize(self.rebuilt), self.want["disc_image"]["size"])   # the dump's size exactly
        self.assertEqual(self.sp.verify(self.rebuilt, lambda f: None), "iso")
        self.sp.extract(self.rebuilt, "iso", self.paths(), lambda f: None, lambda s: None)
        self.assertEqual(tree(self.disc), self.files)

    def test_canonical_size_a_changed_file_refused_at_extraction(self):
        files = dict(self.files, **{"PS3_GAME/USRDIR/data/all.lua.eu": b"-- changed!\n" * 75})   # the same size
        self.assertEqual(len(files["PS3_GAME/USRDIR/data/all.lua.eu"]),
                         len(self.files["PS3_GAME/USRDIR/data/all.lua.eu"]))
        other = self.iso(files, name="changed.iso", order="reverse", gap=5)
        self.assertEqual(os.path.getsize(other), self.want["disc_image"]["size"])
        self.assertEqual(self.sp.verify(other, lambda f: None), "iso")   # title, version and program are right
        with self.assertRaises(self.sp.SetupError) as e:
            self.sp.extract(other, "iso", self.paths(), lambda f: None, lambda s: None)
        self.assertEqual(e.exception.code, "damaged_disc")
        self.assertIn("all.lua.eu", str(e.exception))
        self.assertFalse(os.path.exists(self.disc))

    def test_whole_sha256_is_the_fast_path(self):
        with open(self.rebuilt, "rb") as f:
            self.want["disc_image"]["sha256"] = hashlib.sha256(f.read()).hexdigest()
        with mock.patch.object(self.sp, "probe_disc", side_effect=AssertionError("probed")):
            self.assertEqual(self.sp.verify(self.rebuilt, lambda f: None), "iso")

    def test_another_size_the_same(self):
        padded = self.iso(self.files, name="padded.iso", pad=33)
        self.assertEqual(self.sp.verify(padded, lambda f: None), "iso")
        self.sp.extract(padded, "iso", self.paths(), lambda f: None, lambda s: None)
        self.assertEqual(tree(self.disc), self.files)


class ManifestTool(Base):
    def test_make_refuses_anything_but_the_verified_dump(self):
        iso = self.iso()
        size = os.path.getsize(iso)
        with self.assertRaises(SystemExit) as e:   # another size
            disc_manifest.make(iso, {"disc_image": {"size": size + 2048, "sha256": "0" * 64},
                                     "title_id": "BLUS30490", "app_ver": "01.00"})
        self.assertIn("not the verified dump", str(e.exception))
        with self.assertRaises(SystemExit) as e:   # the size, another SHA-256 (a modified file in place)
            disc_manifest.make(iso, {"disc_image": {"size": size, "sha256": "0" * 64},
                                     "title_id": "BLUS30490", "app_ver": "01.00"})
        self.assertIn("not the verified dump", str(e.exception))

    def test_make_is_deterministic_and_matches_the_files(self):
        iso = self.iso()
        with open(iso, "rb") as f:
            whole = hashlib.sha256(f.read()).hexdigest()
        want = {"disc_image": {"size": os.path.getsize(iso), "sha256": whole}, "title_id": "BLUS30490",
                "app_ver": "01.00"}
        a = disc_manifest.render(disc_manifest.make(iso, want))
        b = disc_manifest.render(disc_manifest.make(iso, want))
        self.assertEqual(a, b)
        self.assertNotIn(b"\r", a)
        m = json.loads(a)
        self.assertEqual(m["files"], made_up_manifest()["files"])   # sorted by path, the Joliet names
        self.assertEqual((m["count"], m["total_size"]), (len(FILES), sum(map(len, FILES.values()))))
        self.assertEqual(ds.manifest.__name__, "manifest")
        p = os.path.join(self.d, "m.json")
        with open(p, "wb") as f:
            f.write(a)
        ds.manifest(p)   # its own form accepted

    def test_the_real_manifest_and_the_version_record_agree(self):
        m = ds.manifest()
        with open(os.path.join(ROOT, "config", "game_version.json")) as f:
            v = json.load(f)
        files = {e["path"]: e["sha256"] for e in m["files"]}
        for p, h in v["files"].items():
            self.assertEqual(files[p], h, p)
        self.assertEqual(m["made_from"], {"size": v["disc_image"]["size"], "sha256": v["disc_image"]["sha256"]})
        self.assertEqual((m["title_id"], m["app_ver"]), (v["title_id"], v["app_ver"]))
        self.assertEqual((m["count"], m["total_size"]), (len(m["files"]), ds.manifest_total(m)))
        self.assertEqual([e["path"] for e in m["files"]], sorted(e["path"] for e in m["files"]))
        with open(ds.MANIFEST, "rb") as f:
            self.assertEqual(f.read(), disc_manifest.render(m))   # as the tool writes it: no hand edits

    def test_manifest_not_in_the_lift_fingerprint(self):
        import setup_pipeline as sp
        self.assertIn("disc_manifest.json", sp.NOT_LIFT_CONFIG)
        self.assertNotIn("disc_source.py", sp.LIFT_TOOLS)


class Keys(Base):
    def test_keys_file_chosen_as_the_disc_key(self):
        p = os.path.join(self.d, "keys.txt")
        with open(p, "w") as f:
            f.write("# my keys\nAPP 0001 " + "00" * 32 + " " + "00" * 16 + "\n")
        with self.assertRaises(ps3_disc.DiscKeyError) as e:
            ps3_disc.load_key(p)
        self.assertIn("keys file", str(e.exception))
        self.assertNotIn("00" * 16, str(e.exception))


if __name__ == "__main__":
    unittest.main()
