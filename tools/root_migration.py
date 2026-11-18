#!/usr/bin/env python3
"""An existing installation copied into a portable root, and the old installations found (the portable launcher: the
install's root is the launcher's own folder; the worker's `migrate-root` and `find-legacy`,
docs/M9_WORKER_PROTOCOL.md).

    plan(old, new)                               what a migration would copy, and why it would be refused: nothing
                                                 written anywhere
    migrate(old, new, progress, log)             the old install's items COPIED into the new root, every file checked
                                                 against its source, then put in place; game/ made again
    find_legacy(report)                          the old launcher's (and the setup app's) pointers to an install and
                                                 the old default folders, each with what it holds: read only

The source is only ever read: nothing in it is moved, renamed, written or removed, and it stays a complete install
the old launcher can still use. What is copied (COPY_ITEMS): builds/, data/ (the disc's files, the lifts, the caches,
the saves in data/gamedata, developer mode's data/devmode, install.json), config/ (the settings), backups/ (the save
backups), active.json. Not copied: game/ (a link: made again for the active build), logs/ (they tell of the old
folder; the old one keeps them, and the new root starts its own), downloads/ (update downloads, fetched again when
needed), data/tmp/ (a worker's temporary files), a running preparation's lock and part file (a migration is refused
while one runs), and any link met inside (never followed). Everything else at the old root -- a launcher's package,
the player's own files -- stays where it is.

The copy goes into <new>/.migrate-root.part/ first; each file is checked there (its size, and its SHA-256 against the
source's for the saves, their backups, the settings and the records), and each build's game against its build.json
(SHA-256, as a rollback checks it); only then is it moved into place, active.json last, so a new root never looks
like an install before the copy is whole. A copy cancelled or failed leaves nothing in the new root but the part
folder, which is removed (and, if it stayed, removed by the next migration).
"""
import hashlib
import json
import os
import shutil
import sys

ROOT = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import install_layout as layout  # noqa: E402

COPY_ITEMS = ("builds", "data", "config", "backups", "active.json")   # active.json last: the install appears whole
SKIP = {"data/tmp", "data/cache/prepare.lock", "data/cache/prepare-part.bin"}
# Checked by SHA-256 against the source (the player's own, or small records); the rest by size, the builds' games by
# their build.json
HASHED = ("data/gamedata/", "data/devmode/", "backups/", "config/", "active.json", "data/install.json")
PART = ".migrate-root.part"
CHUNK = 8 << 20
MARGIN = 256 << 20   # free space kept beyond the copy (the cache files the game writes as it is first played)


class MigrationError(Exception):
    def __init__(self, message, code="failed", details=None):
        super().__init__(message)
        self.code = code
        self.details = details


def _norm(p):
    return os.path.normcase(os.path.abspath(p))


def _real(p):
    try:
        return os.path.normcase(os.path.realpath(p))
    except OSError:
        return _norm(p)


def _contains(outer, inner):
    try:
        return os.path.commonpath([outer, inner]) == outer
    except ValueError:   # other drives
        return False


def overlap(old, new):
    """the two roots are the same folder, or one lies inside the other (as given, and through links)"""
    for a, b in ((_norm(old), _norm(new)), (_real(old), _real(new))):
        if a == b or _contains(a, b) or _contains(b, a):
            return True
    return False


def _rel(*parts):
    return "/".join(p.replace(os.sep, "/") for p in parts if p)


def _scan(path, rel, out):
    """files (rel, size), folders and links under path, never through a link"""
    if layout.is_link(path):
        out["links"].append(rel)
        return
    if rel in SKIP or rel.startswith("builds/.staging-"):   # a build an operation did not finish: not the install's
        out["skipped"].append(rel)
        return
    if os.path.isdir(path):
        out["dirs"].append(rel)
        with os.scandir(path) as it:
            names = sorted(e.name for e in it)
        for n in names:
            _scan(os.path.join(path, n), _rel(rel, n), out)
        return
    if os.path.isfile(path):
        size = os.path.getsize(path)
        out["files"].append((rel, size))
        out["bytes"] += size


def inventory(old):
    """what a migration copies from old: {files: [(rel, size)], dirs, links, skipped, bytes, items: {item: {bytes,
    files}}}"""
    out = {"files": [], "dirs": [], "links": [], "skipped": [], "bytes": 0}
    items = {}
    for item in COPY_ITEMS:
        p = os.path.join(old, item)
        if not os.path.lexists(p):
            continue
        b0, f0 = out["bytes"], len(out["files"])
        _scan(p, item, out)
        items[item] = {"bytes": out["bytes"] - b0, "files": len(out["files"]) - f0}
    out["items"] = items
    return out


def check_roots(old, new):
    """why a migration from old to new is refused (MigrationError), else the old install's Layout and its kind"""
    old, new = os.path.abspath(old), os.path.abspath(new)
    if overlap(old, new):
        raise MigrationError("The installation can't be copied into itself: the old and new folders are the same, "
                             "or one is inside the other.", "root_overlap")
    lay = layout.Layout(old)
    if layout.is_link(old) or not os.path.isdir(old):
        raise MigrationError("There is no installation in the old folder.", "no_install")
    kind = lay.kind()
    if kind == "setup-app":
        raise MigrationError("The old folder holds an install made by the setup app: migrate it in place first "
                             "(migrate), then copy it.", "needs_migration")
    if kind == "none":
        raise MigrationError("The old folder doesn't hold an installation the launcher made.", "no_install")
    target = layout.Layout(new)
    if os.path.lexists(target.active_json) or os.path.lexists(target.builds_dir):
        raise MigrationError("The new folder already holds an installation: nothing was copied over it.",
                             "install_exists")
    if os.path.exists(target.game_link) and not layout.is_link(target.game_link):
        raise MigrationError("The new folder has a game folder in the way (not the launcher's link).", "install_exists")
    return lay, kind


def plan(old, new):
    """what migrate(old, new) would copy, nothing written: {from, to, kind, items, bytes, files, needed, links,
    skipped, active}"""
    lay, kind = check_roots(old, new)
    inv = inventory(lay.root)
    st = lay.read_active() if kind == "slots" else None
    return {"from": lay.root, "to": os.path.abspath(new), "kind": kind, "items": inv["items"], "bytes": inv["bytes"],
            "files": len(inv["files"]), "needed": inv["bytes"] + MARGIN, "links": inv["links"],
            "skipped": inv["skipped"] + ["game", "logs", "downloads"], "active": st and st["active"]}


def _sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


def _copy_file(src, dst, on_bytes):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(src, "rb") as fi, open(dst, "wb") as fo:
        while True:
            chunk = fi.read(CHUNK)
            if not chunk:
                break
            fo.write(chunk)
            on_bytes(len(chunk))   # the cancel is taken here
    shutil.copystat(src, dst)


def _merge_move(src, dst, rel, replaced):
    """src into dst: a whole item renamed when dst is free, else its contents one by one; a file of the new root's
    already there is kept beside it (<name>.before-migrate-root) and reported"""
    if not os.path.lexists(dst):
        os.replace(src, dst)
        return
    if os.path.isdir(src) and os.path.isdir(dst) and not layout.is_link(dst):
        for n in sorted(os.listdir(src)):
            _merge_move(os.path.join(src, n), os.path.join(dst, n), _rel(rel, n), replaced)
        os.rmdir(src)
        return
    aside = dst + ".before-migrate-root"
    n = 1
    while os.path.lexists(aside):
        n += 1
        aside = f"{dst}.before-migrate-root-{n}"
    os.replace(dst, aside)
    os.replace(src, dst)
    replaced.append({"path": rel, "kept_as": os.path.basename(aside)})


def migrate(old, new, progress=None, log=None):
    """old's items copied into new (see the module's notes); progress(step, fraction, message) -- it may raise to
    cancel (the worker's), and the part folder is removed then. Returns what was copied and checked."""
    progress = progress or (lambda s, f, m: None)
    log = log or (lambda t: None)
    lay, kind = check_roots(old, new)
    new = os.path.abspath(new)
    part = os.path.join(new, PART)
    if os.path.lexists(part):   # a migration that did not finish
        layout.rmtree(part)
    inv = inventory(lay.root)
    total = max(1, inv["bytes"])
    os.makedirs(part)
    try:
        done = [0]

        def moved(n):
            done[0] += n
            progress("copy", min(1.0, done[0] / total), f"{done[0] >> 20} of {total >> 20} MB")
        progress("copy", 0.0, "Copying the installation")
        for rel in inv["dirs"]:
            os.makedirs(os.path.join(part, *rel.split("/")), exist_ok=True)
        for rel, _ in inv["files"]:
            _copy_file(os.path.join(lay.root, *rel.split("/")), os.path.join(part, *rel.split("/")), moved)
        progress("copy", 1.0, "Copying the installation")
        # every file checked against its source before anything is put in place
        progress("check-copy", 0.0, "Checking the copy")
        hashed, n = 0, len(inv["files"])
        for i, (rel, size) in enumerate(inv["files"]):
            src, dst = os.path.join(lay.root, *rel.split("/")), os.path.join(part, *rel.split("/"))
            if os.path.getsize(dst) != size or os.path.getsize(src) != size:
                raise MigrationError(f"The copy of {rel} is not the size of the original.", "copy_failed")
            if rel.startswith(HASHED) or rel in HASHED or (rel.startswith("builds/") and rel.count("/") == 2 and
                                                            rel.endswith(("/build.json", "/build.cfg"))):
                if _sha(src) != _sha(dst):
                    raise MigrationError(f"The copy of {rel} differs from the original.", "copy_failed")
                hashed += 1
            progress("check-copy", (i + 1) / max(1, n) * 0.5, "Checking the copy")
        copied = layout.Layout(part)
        builds = sorted(copied.builds())
        for i, bid in enumerate(builds):
            why = copied.check_build(bid, full=True)
            if why:
                raise MigrationError(f"The copy of build {bid} can't be used: {why}.", "copy_failed")
            progress("check-copy", 0.5 + 0.5 * (i + 1) / len(builds), "Checking the copy")
        progress("check-copy", 1.0, "Checking the copy")
        # put in place: the data first, active.json last
        progress("place", 0.0, "Putting it in place")
        replaced = []
        for item in COPY_ITEMS:
            src = os.path.join(part, item)
            if os.path.lexists(src):
                _merge_move(src, os.path.join(new, item), item, replaced)
        os.rmdir(part)
    except BaseException:
        try:
            if os.path.lexists(part):
                layout.rmtree(part)
        except OSError:
            pass
        raise
    target = layout.Layout(new)
    game_link = False
    st = target.read_active() if os.path.exists(target.active_json) else None
    if st and st["active"] and os.path.isdir(target.build_dir(st["active"])):
        try:
            target.point_game(st["active"])
            game_link = True
        except (OSError, layout.LayoutError) as e:
            log(f"game/ was not made again ({e}): the worker's repair-link makes it")
    progress("place", 1.0, "Putting it in place")
    return {"from": lay.root, "to": new, "kind": kind, "copied": inv["items"], "bytes": inv["bytes"],
            "files": len(inv["files"]), "skipped": inv["skipped"] + ["game", "logs", "downloads"],
            "links_skipped": inv["links"], "replaced": replaced,
            "verified": {"files": len(inv["files"]), "hashed": hashed, "builds": builds},
            "active": st and st["active"], "game_link": game_link}


# ---- the old installations: where the launcher before the portable one, and the setup app, kept them ----

def legacy_pref_files(env=None, home=None):
    """[(source, file)]: the old launcher's preferences (launcher/main.cpp data_dir: %APPDATA%\\CubicHeroes on
    Windows, ~/.config/cubic-heroes elsewhere) and the setup app's settings (setup_app.py settings_path)"""
    env = os.environ if env is None else env
    home = home or os.path.expanduser("~")
    if os.name == "nt":
        appdata = env.get("APPDATA") or os.path.join(home, "AppData", "Roaming")
        return [("launcher", os.path.join(appdata, "CubicHeroes", "launcher.json")),
                ("setup-app", os.path.join(appdata, "DotHeroes-Setup", "settings.json"))]
    xdg = env.get("XDG_CONFIG_HOME") or os.path.join(home, ".config")
    return [("launcher", os.path.join(home, ".config", "cubic-heroes", "launcher.json")),
            ("setup-app", os.path.join(xdg, "dotheroes-setup", "settings.json"))]


def legacy_default_roots(env=None, home=None):
    """[(source, folder)]: the old launcher's default install folder and the setup app's"""
    env = os.environ if env is None else env
    home = home or os.path.expanduser("~")
    if os.name == "nt":
        local = env.get("LOCALAPPDATA") or os.path.join(home, "AppData", "Local")
        return [("launcher-default", os.path.join(local, "Cubic.Heroes")),
                ("setup-app-default", os.path.join(local, "3D Dot Game Heroes"))]
    return [("launcher-default", os.path.join(home, "Games", "Cubic.Heroes")),
            ("setup-app-default", os.path.join(home, "Games", "3D Dot Game Heroes"))]


def _read_prefs(path):
    """the install a preferences file names, or None; (present, readable, install)"""
    if not os.path.isfile(path):
        return False, False, None
    try:
        with open(path, encoding="utf-8") as f:
            v = json.load(f)
    except (OSError, ValueError):
        return True, False, None
    inst = v.get("install") if isinstance(v, dict) else None
    return True, True, inst if isinstance(inst, str) and inst.strip() else None


def find_legacy(report, current=None, env=None, home=None):
    """read only: every pointer to an old installation and what each folder holds. report(folder) -> what it holds
    (the worker's info). current: the root in use now (marked, never offered for a migration into itself)."""
    cur = _norm(current) if current else None

    def look(folder):
        if not os.path.isdir(folder):
            return {"exists": False}
        try:
            out = dict(report(folder), exists=True)
        except OSError as e:
            return {"exists": True, "error": e.strerror or str(e)}
        out["migratable"] = out.get("kind") in ("slots", "partial")
        out["current"] = bool(cur) and _norm(folder) == cur
        return out
    prefs, defaults, found = [], [], {}
    for source, path in legacy_pref_files(env, home):
        present, readable, inst = _read_prefs(path)
        row = {"source": source, "file": path, "present": present, "readable": readable, "install": inst}
        if inst:
            row["holds"] = look(os.path.abspath(inst))
            found.setdefault(_norm(inst), {"path": os.path.abspath(inst), "from": [], **row["holds"]})["from"].append(
                source)
        prefs.append(row)
    for source, path in legacy_default_roots(env, home):
        row = {"source": source, "path": path, "holds": look(path)}
        defaults.append(row)
        if row["holds"].get("exists"):
            found.setdefault(_norm(path), {"path": path, "from": [], **row["holds"]})["from"].append(source)
    installs = [v for v in found.values() if v.get("kind") not in (None, "none")]
    return {"prefs": prefs, "defaults": defaults, "installs": installs,
            "migratable": [v["path"] for v in installs if v.get("migratable") and not v.get("current")]}
