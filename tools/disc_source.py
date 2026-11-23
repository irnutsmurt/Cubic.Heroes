#!/usr/bin/env python3
"""The player's disc, checked file by file and installed safely (0.2.0-beta.5; docs/M9_WORKER_PROTOCOL.md section 4).

Every game file -- everything under PS3_GAME/ -- is checked against config/disc_manifest.json (each file's path, size
and SHA-256, made by tools/disc_manifest.py from the verified dump of BLUS-30490 v01.00 only). Whatever the source --
the whole-disc ISO, an ISO rebuilt from the disc's folder (another layout and size), an encrypted ISO read through its
disc key, or the disc's folder -- the same files are installed, byte for byte:

  - the source's names are only looked up, case-insensitively, among the manifest's paths: what is written is always
    the manifest's path, so no name in a source can reach the file system (no traversal, no absolute path, no
    reserved name); a source file the manifest doesn't list is skipped and named in the log;
  - a manifest file missing, of another size or another SHA-256, or matched twice (names differing only in case) is
    refused (damaged_disc), sizes before anything is read; each file is hashed as it is written;
  - an image is read through tools/iso9660.py (its bounds, loops, limits and names checked); a folder is walked
    without following links: a symbolic link, a junction or anything that isn't a regular file or a folder is refused;
  - the files go to <data>/disc.staging; only when every one has been written and checked does it replace <data>/disc
    (the old copy moved to <data>/disc.old, then removed). A failure before that leaves the installed copy as it was;
    recover() finishes or undoes an interrupted swap. Saves, settings and builds are elsewhere and never touched.

Errors are Refused(message, code, details); tools/setup_pipeline.py turns them into its SetupError."""
import hashlib
import json
import os
import shutil
import stat
import sys

ROOT = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import iso9660  # noqa: E402

MANIFEST = os.path.join(ROOT, "config", "disc_manifest.json")
GAME_DIR = "PS3_GAME"
CHUNK = 8 << 20
SPARE = 64 << 20          # free space kept beyond the game files
MAX_FOLDER_ENTRIES = 100_000
MAX_FOLDER_DEPTH = 32
SHOW_EXTRAS = 10


class Refused(Exception):
    def __init__(self, message, code="damaged_disc", details=None):
        super().__init__(message)
        self.code = code
        self.details = details


def _base(rel):
    return rel.rsplit("/", 1)[-1]


# ---------------- the manifest ----------------

def manifest(path=None):
    """config/disc_manifest.json, its form checked: {"files": [{"path", "size", "sha256"}], ...}"""
    path = path or MANIFEST
    try:
        with open(path, encoding="utf-8") as f:
            m = json.load(f)
    except (OSError, ValueError) as e:
        raise Refused("The game's file list is missing or damaged: reinstall Cubic.Heroes.", "failed",
                      f"{path}: {e}") from None
    files = m.get("files") if isinstance(m, dict) else None
    ok = isinstance(files, list) and files and m.get("format") == 1
    seen = set()
    for e in files if ok else ():
        p = e.get("path") if isinstance(e, dict) else None
        if (not isinstance(p, str) or not p.startswith(GAME_DIR + "/") or p.casefold() in seen
                or any(c in ("", ".", "..") for c in p.split("/")) or "\\" in p
                or not isinstance(e.get("size"), int) or e["size"] < 0
                or not isinstance(e.get("sha256"), str) or len(e["sha256"]) != 64):
            ok = False
            break
        seen.add(p.casefold())
    if not ok:
        raise Refused("The game's file list is damaged: reinstall Cubic.Heroes.", "failed", path)
    return m


def manifest_total(m):
    return sum(e["size"] for e in m["files"])


# ---------------- the sources ----------------

def iso_sources(f):
    """{folded path: (path, extents, size)} of an image's files (every file: the manifest picks its own)"""
    try:
        ents = iso9660.entries(f)
    except iso9660.NotISO:
        raise Refused("This file isn't a disc image (no ISO 9660 file system).", "not_a_disc") from None
    except iso9660.ImageError as e:
        raise Refused(f"The disc image is damaged or isn't a valid disc image: {e}.", "damaged_disc") from None
    return {p.casefold(): (p, ext, sum(n for _, n in ext)) for p, ext in ents}


def _is_link(path_or_entry):
    """a symbolic link or a junction (Windows), without following it"""
    if isinstance(path_or_entry, os.DirEntry):
        e = path_or_entry
        return e.is_symlink() or (hasattr(e, "is_junction") and e.is_junction())
    p = path_or_entry
    return os.path.islink(p) or (hasattr(os.path, "isjunction") and os.path.isjunction(p))


def folder_game_dir(top):
    """the folder's PS3_GAME (its name in any case), or None"""
    try:
        with os.scandir(top) as it:
            found = [e for e in it if e.name.casefold() == GAME_DIR.casefold()]
    except OSError:
        return None
    if len(found) != 1:
        return None
    if _is_link(found[0]):
        raise Refused(f"The disc's folder {found[0].name} is a link: choose the folder itself.", "not_a_disc")
    return found[0].path if found[0].is_dir(follow_symlinks=False) else None


def folder_path(top, rel):
    """a file of the disc's folder by its manifest path, matched case-insensitively component by component without
    following links; None when it isn't there"""
    d = top
    parts = rel.split("/")
    for i, part in enumerate(parts):
        try:
            with os.scandir(d) as it:
                found = [e for e in it if e.name.casefold() == part.casefold()]
        except OSError:
            return None
        if len(found) != 1 or _is_link(found[0]):
            return None
        last = i == len(parts) - 1
        if not (found[0].is_file(follow_symlinks=False) if last else found[0].is_dir(follow_symlinks=False)):
            return None
        d = found[0].path
    return d


def folder_sources(top):
    """{folded path: (path, file, size, identity)} of the files under the folder's PS3_GAME; a link, a junction, a
    special file or two names differing only in case are refused"""
    game = folder_game_dir(top)
    if game is None:
        raise Refused("That folder has no PS3_GAME folder: choose the disc's top folder.", "not_a_disc")
    out, names, count = {}, {}, 0
    stack = [(game, GAME_DIR, 0)]
    while stack:
        d, rel, depth = stack.pop()
        if depth > MAX_FOLDER_DEPTH:
            raise Refused(f"The disc's folder is nested too deeply ({rel}).", "damaged_disc")
        try:
            with os.scandir(d) as it:
                ents = list(it)
        except OSError as e:
            raise Refused(f"Can't read the disc's folder {rel} ({e.strerror}).", "damaged_disc") from None
        for e in sorted(ents, key=lambda e: e.name):
            count += 1
            if count > MAX_FOLDER_ENTRIES:
                raise Refused("The disc's folder holds far more files than the game's.", "damaged_disc")
            p = f"{rel}/{e.name}"
            key = p.casefold()
            if key in names:
                raise Refused(f"The disc's folder has two names differing only in case: {names[key]} and {p}.",
                              "damaged_disc")
            names[key] = p
            if _is_link(e):
                raise Refused(f"The disc's folder holds a link ({p}): copy the disc's files themselves.",
                              "damaged_disc")
            if e.is_dir(follow_symlinks=False):
                stack.append((e.path, p, depth + 1))
            elif e.is_file(follow_symlinks=False):
                st = os.lstat(e.path)
                out[key] = (p, e.path, st.st_size, (st.st_dev, st.st_ino))
            else:
                raise Refused(f"The disc's folder holds something that isn't a file ({p}).", "damaged_disc")
    return out


def _open_regular(path, ident):
    """a folder's file opened for reading, never through a link: it must still be the regular file that was listed"""
    try:
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0))
    except OSError as e:   # gone, unreadable, or (POSIX, O_NOFOLLOW) made a link since it was listed
        raise Refused(f"Can't read the disc's {os.path.basename(path)} ({e.strerror}).", "damaged_disc") from None
    try:
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode) or (st.st_dev, st.st_ino) != ident:
            raise Refused(f"The disc's folder changed while it was read ({os.path.basename(path)}).", "damaged_disc")
        return os.fdopen(fd, "rb")
    except BaseException:
        os.close(fd)
        raise


# ---------------- matching and copying ----------------

def match(m, sources):
    """[(manifest entry, source)] in the order to read them, and the source paths the manifest doesn't list.
    Refused when a manifest file is missing or of another size (nothing read yet)."""
    pairs, missing, sized = [], [], []
    for e in m["files"]:
        s = sources.get(e["path"].casefold())
        if s is None:
            missing.append(e["path"])
        elif s[2] != e["size"]:
            sized.append(e["path"])
        else:
            pairs.append((e, s))
    if missing:
        more = f" (and {len(missing) - 1} more)" if len(missing) > 1 else ""
        raise Refused(f"The disc's {_base(missing[0])} is missing{more}: the dump is incomplete.", "damaged_disc",
                      "missing: " + ", ".join(missing[:20]))
    if sized:
        more = f" (and {len(sized) - 1} more)" if len(sized) > 1 else ""
        raise Refused(f"The disc's {_base(sized[0])} doesn't match (its size){more}: the dump is damaged or "
                      "modified.", "damaged_disc", "size differs: " + ", ".join(sized[:20]))
    listed = {e["path"].casefold() for e in m["files"]}
    extras = sorted(s[0] for k, s in sources.items() if k not in listed)
    pairs.sort(key=lambda es: es[1][1][0][0] if isinstance(es[1][1], list) else 0)   # an image: by its first sector
    return pairs, extras


def _chunks(src, f):
    """a source file's bytes: an image's extents, or a folder's file (exactly its listed size)"""
    if isinstance(src[1], list):
        try:
            yield from iso9660.stream(f, src[1], CHUNK)
        except iso9660.ImageError:
            raise Refused(f"The disc image is cut short ({_base(src[0])}): the dump is incomplete.",
                          "damaged_disc") from None
        return
    left = src[2]
    with _open_regular(src[1], src[3]) as fh:
        while left:
            b = fh.read(min(left, CHUNK))
            if not b:
                break
            left -= len(b)
            yield b
        if left or fh.read(1):
            raise Refused(f"The disc's {_base(src[0])} changed while it was read.", "damaged_disc")


def copy_checked(pairs, f, dest, progress=None):
    """each file's bytes hashed (and written under dest, by its manifest path, when dest is given); refused at the
    first file whose SHA-256 isn't the manifest's"""
    total = sum(e["size"] for e, _ in pairs) or 1
    done = 0
    for e, src in pairs:
        h = hashlib.sha256()
        out = None
        if dest is not None:
            p = os.path.join(dest, *e["path"].split("/"))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            out = open(p, "xb")   # never over something already there (a link included)
        try:
            for b in _chunks(src, f):
                h.update(b)
                if out:
                    out.write(b)
                done += len(b)
                if progress:
                    progress(done / total)
        finally:
            if out:
                out.close()
        if h.hexdigest() != e["sha256"]:
            raise Refused(f"The disc's {_base(e['path'])} doesn't match: the dump is damaged or modified.",
                          "damaged_disc", f"SHA-256 differs: {e['path']}")
    if progress:
        progress(1.0)


def sources_of(source, kind, f=None):
    return folder_sources(source) if kind == "folder" else iso_sources(f)


def check(source, kind, f=None, m=None, progress=None):
    """the source against the manifest, nothing written: (files checked, extras)"""
    m = m or manifest()
    pairs, extras = match(m, sources_of(source, kind, f))
    copy_checked(pairs, f, None, progress)
    return len(pairs), extras


# ---------------- the install's copy: staging, promotion, recovery ----------------

def _refuse_link(p):
    if _is_link(p):
        raise Refused(f"The install's {os.path.basename(p)} folder is a link: Cubic.Heroes only writes into folders "
                      "of its own.", "root_not_writable", p)


def recover(disc):
    """finish or undo a swap that was interrupted: <disc>.old put back when <disc> is gone, removed when both are
    there; a leftover <disc>.staging removed"""
    old, staging = disc + ".old", disc + ".staging"
    for p in (old, staging):   # what this removes or moves is never a link (<disc> itself: refused by install())
        _refuse_link(p)
    if os.path.isdir(old):
        if not os.path.exists(disc):
            os.replace(old, disc)
        else:
            shutil.rmtree(old)
    if os.path.isdir(staging):
        shutil.rmtree(staging)


def install(source, kind, disc, progress=None, log=None, f=None, keep_elf_sha256=None, m=None):
    """the source's game files into <disc>, by way of <disc>.staging (module docstring). keep_elf_sha256: an
    EBOOT.elf already in <disc> with this SHA-256 (the supported decrypted program) is carried across."""
    m = m or manifest()
    log = log or (lambda s: None)
    _refuse_link(disc)   # the copy is swapped by renaming: never a link to elsewhere
    recover(disc)
    staging = disc + ".staging"
    pairs, extras = match(m, sources_of(source, kind, f))
    need = manifest_total(m) + SPARE
    parent = os.path.dirname(disc)
    os.makedirs(parent, exist_ok=True)
    free = shutil.disk_usage(parent).free
    if free < need:
        raise Refused(f"Not enough free space for the game files: {need / 1e9:.1f} GB needed, "
                      f"{free / 1e9:.1f} GB free.", "disk_space")
    os.makedirs(staging)
    try:
        copy_checked(pairs, f, staging, progress)
        elf = os.path.join(disc, GAME_DIR, "USRDIR", "EBOOT.elf")
        if keep_elf_sha256 and os.path.isfile(elf) and not _is_link(elf):
            h = hashlib.sha256()
            with open(elf, "rb") as fh:
                for b in iter(lambda: fh.read(CHUNK), b""):
                    h.update(b)
            if h.hexdigest() == keep_elf_sha256:
                shutil.copyfile(elf, os.path.join(staging, GAME_DIR, "USRDIR", "EBOOT.elf"))
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    old = disc + ".old"
    if os.path.exists(disc):
        os.replace(disc, old)
    try:
        os.replace(staging, disc)
    except BaseException:
        if os.path.exists(old) and not os.path.exists(disc):
            os.replace(old, disc)
        raise
    shutil.rmtree(old, ignore_errors=True)
    log(f"extracted {len(pairs)} game files, each checked against the game's file list")
    if extras:
        log(f"{len(extras)} file(s) not part of the game skipped: " + ", ".join(extras[:SHOW_EXTRAS])
            + (" ..." if len(extras) > SHOW_EXTRAS else ""))
    return len(pairs), extras
