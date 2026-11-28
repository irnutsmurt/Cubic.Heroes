#!/usr/bin/env python3
"""The official v1.01 update's files in an installation (beta.6; docs/OFFICIAL_UPDATE.md).

    <install>/data/official/BLUS30490-01.01/     the update as Sony's package lays it out (PARAM.SFO, USRDIR/...), and
                                                 its program decrypted beside its EBOOT.BIN (USRDIR/EBOOT.elf)
    <install>/data/official/.staging-<pid>-<t>/  an update being put together (removed by the next start if left)

Apart from the saves (data/gamedata) and every personal file, so a save backup never copies the update (the owner,
2026-11-25). The game sees it at the PS3's own place for an installed update (/dev_hdd0/game/BLUS30490/) through the
native runtime's overlay: the v1.01 build's build.cfg names the folder (DOTHEROES_UPDATE_DIR), the v1.00 build's
doesn't. Nothing here changes the active build: the worker's game-update-install stages and checks everything,
then builds and activates v1.01 (tools/worker.py op_game_update); any failure leaves v1.00 as it was.

Every file is checked against the update's file list (config/update_manifest.json) before the folder is put in
place; the program is accepted only as the pinned decrypted program (config/game_version.json). Nothing of the
update, its keys or its program is part of this repository or of anything it distributes.
"""
import hashlib
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import game_update  # noqa: E402

UPDATE_ID = "BLUS30490-01.01"
CHUNK = 8 << 20


class StageError(Exception):
    """code: not_official, keys, io, cancelled"""

    def __init__(self, message, code, details=""):
        super().__init__(message)
        self.code, self.details = code, details


def root(install):
    return os.path.join(os.path.abspath(install), "data", "official")


def folder(install, pid=UPDATE_ID):
    """where an official update's files are in the installation"""
    return os.path.join(root(install), pid)


def relative(pid=UPDATE_ID):
    """the folder relative to the installation's root, as the v1.01 build's build.cfg names it"""
    return f"data/official/{pid}"


def elf_path(install, pid=UPDATE_ID):
    return os.path.join(folder(install, pid), "USRDIR", "EBOOT.elf")


def _pin():
    return game_update.record()["decrypted_elf"]["sha256"]


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


def clean_staging(install):
    """what an attempt that didn't finish left: removed (a failed or interrupted install starts clean)"""
    removed = []
    r = root(install)
    if os.path.isdir(r):
        for n in os.listdir(r):
            if n.startswith((".staging-", ".old-")):
                shutil.rmtree(os.path.join(r, n), ignore_errors=True)
                removed.append(n)
    return removed


def prepared(install, pid=UPDATE_ID, full=True):
    """whether the installation holds the whole update and its pinned program (an earlier install, or one that
    stopped after this step): its files checked against the file list (full: their SHA-256 too), its program's"""
    d = folder(install, pid)
    try:
        if full:
            game_update.check_folder(d)
        else:
            m = game_update.load_manifest()
            for f in m["files"]:
                if os.path.getsize(os.path.join(d, *f["path"].split("/"))) != f["size"]:
                    return False
        return _sha256(elf_path(install, pid)) == _pin()
    except (game_update.UpdateError, OSError, KeyError, ValueError):
        return False


def new_staging(install):
    os.makedirs(root(install), exist_ok=True)
    d = os.path.join(root(install), f".staging-{os.getpid()}-{int(time.time() * 1000)}")
    os.makedirs(d)
    return d


def stage_package(pkg, key, staging, progress=None, check=None):
    """the official package's files into staging (the package checked first: exactly the official one), then
    checked against the file list. key: the package key (16 bytes), from the player's keys or the downloaded
    ones. progress(done, total); check(): raises to cancel."""
    import pkg_extract
    try:
        game_update.check_package(pkg)
        p = pkg_extract.Package(pkg)
        if not key:
            raise StageError("The update package can't be opened: no package key.", "keys")
        p.extract(key, staging, progress=(lambda d, t: (check and check(), progress and progress(d, t))))
    except pkg_extract.PkgError as e:
        raise StageError(str(e), "keys" if e.code == "wrong_key" else "not_official" if e.code in (
            "damaged", "not_a_package", "unsupported", "unsafe_name") else "io") from None
    except game_update.UpdateError as e:
        raise StageError(str(e), "not_official" if e.code == "not_official" else "io") from None
    _check_staged(staging)


def stage_folder(src, staging, progress=None, check=None):
    """an installed or extracted update's files (RPCS3's, or a folder the player chose) copied into staging, only
    the file list's files, each checked as it is copied"""
    m = game_update.load_manifest()
    total, done = m["total_size"] or 1, 0
    for f in m["files"]:
        s = os.path.join(src, *f["path"].split("/"))
        d = os.path.join(staging, *f["path"].split("/"))
        os.makedirs(os.path.dirname(d), exist_ok=True)
        h = hashlib.sha256()
        try:
            with open(s, "rb") as r, open(d + ".part", "wb") as w:
                for chunk in iter(lambda: r.read(CHUNK), b""):
                    if check:
                        check()
                    h.update(chunk)
                    w.write(chunk)
                    done += len(chunk)
                    if progress:
                        progress(done, total)
        except OSError as e:
            raise StageError(f"The update's files couldn't be copied ({e.strerror or e}).", "io", s) from None
        if h.hexdigest() != f["sha256"]:
            os.remove(d + ".part")
            raise StageError(f"That folder isn't the official v{m['app_ver']} update: {f['path']} differs.",
                             "not_official", s)
        os.replace(d + ".part", d)
    _check_staged(staging)


def _check_staged(staging):
    try:
        game_update.check_folder(staging)
    except game_update.UpdateError as e:
        raise StageError(str(e), "not_official") from None


def eboot(staging):
    with open(os.path.join(staging, "USRDIR", "EBOOT.BIN"), "rb") as f:
        return f.read()


def place_program(staging, elf):
    """the decrypted program beside the update's EBOOT.BIN -- only the pinned one"""
    if hashlib.sha256(elf).hexdigest() != _pin():
        raise StageError("The update's program isn't the one expected.", "not_official")
    p = os.path.join(staging, "USRDIR", "EBOOT.elf")
    with open(p + ".part", "wb") as f:
        f.write(elf)
    os.replace(p + ".part", p)


def promote(install, staging, pid=UPDATE_ID):
    """the staged update put in place in one rename (an earlier copy moved aside first, then removed)"""
    dst = folder(install, pid)
    old = None
    if os.path.exists(dst):
        old = os.path.join(root(install), f".old-{os.getpid()}-{int(time.time() * 1000)}")
        os.replace(dst, old)
    try:
        os.replace(staging, dst)
    except OSError:
        if old:
            os.replace(old, dst)   # put back what was there
        raise
    if old:
        shutil.rmtree(old, ignore_errors=True)
    return dst


def remove(install, pid=UPDATE_ID):
    """the update's files removed (only when no kept build uses them: the worker decides)"""
    d = folder(install, pid)
    if os.path.isdir(d):
        shutil.rmtree(d)
        return True
    return False
