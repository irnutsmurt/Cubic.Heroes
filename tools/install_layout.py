#!/usr/bin/env python3
"""An install with build slots (M9.1: docs/M9_LAUNCHER_PLAN.md sections 3.3 and 10).

    <install>/
      game/                     the active build: a junction (Windows) or a symlink to builds/<id> -- the stable
                                path shortcuts start, re-pointed when another build becomes active
      active.json               {"layout": 2, "active": id, "previous": [ids, newest first], "retention": n}
      builds/<id>/              dotheroes[.exe], its libraries and notices, build.json (what it is), build.cfg
                                (what the game reads: its id, its shader cache's folder)
      config/dotheroes.cfg      the settings: independent of the builds, kept by every update and rollback
      data/disc/                the player's disc, extracted (the disc image is not needed after the install)
      data/gen/<lift-id>/       one lift's recompiled code, shared by the builds made from it
      data/cache/objects/       compiled objects by fingerprint, shared by every build
      data/cache/shaders/<id>/  translated shaders and pipelines, per translator (shared by the builds with it)
      data/gamedata/            the saves: never in a build, never written by an update
      data/install.json         the conversion's record (each step's fingerprint)
      backups/saves/<id>/       save backups, each with its backup.json
      logs/                     the game's (game.log and five before it) and the worker's

The build that is active, and the ones kept for rollback, own their slots, their lift, their objects and their
shader folder: nothing any of them uses is removed (prune). Rollback rewrites active.json and re-points game/:
offline, no rebuild. A build becomes active only after it has been checked (the worker's validate).
"""
import datetime
import hashlib
import json
import os
import shutil
import stat
import sys
import time

LAYOUT = 2
GAME = "dotheroes"
DEFAULT_RETENTION = 1      # previous builds kept for rollback (the owner's default; configurable)
AUTO_BACKUPS_KEPT = 10     # automatic save backups kept (manual ones are only removed by the player)
AUTO_REASONS = ("before-update", "before-restore", "before-migration", "before-rollback")


def exe_name(name=GAME):
    return name + (".exe" if os.name == "nt" else "")


def now_id():
    return datetime.datetime.now().strftime("%Y%m%d-%H%M%S")


def write_json(path, obj):
    """written whole or not at all"""
    tmp = path + ".part"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, indent=1)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def read_json(path, default=None):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def tree_size(path):
    n = 0
    for d, _, fs in os.walk(path):
        for f in fs:
            try:
                n += os.path.getsize(os.path.join(d, f))
            except OSError:
                pass
    return n


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def is_link(path):
    """a junction or a symlink (never followed by a removal here)"""
    if os.path.islink(path):
        return True
    isj = getattr(os.path, "isjunction", None)
    if isj and isj(path):
        return True
    try:
        st = os.lstat(path)
    except OSError:
        return False
    return bool(getattr(st, "st_file_attributes", 0) & 0x400)   # FILE_ATTRIBUTE_REPARSE_POINT


def remove_link(path):
    if not is_link(path):
        return
    if os.name == "nt":
        os.rmdir(path)   # a junction's removal: the link only, never what it points at
    else:
        os.unlink(path)


def make_link(link, target):
    """link -> target (a directory): a junction on Windows (no privilege needed), a symlink elsewhere"""
    if os.name == "nt":
        import _winapi
        _winapi.CreateJunction(os.path.abspath(target), os.path.abspath(link))
    else:
        os.symlink(os.path.relpath(target, os.path.dirname(link)), link)


def rmtree(path):
    """a folder removed with what is in it; a read-only file is no obstacle; a link inside is removed as a link"""
    def onerror(fn, p, exc):
        os.chmod(p, stat.S_IWRITE)
        fn(p)
    if is_link(path):
        return remove_link(path)
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=onerror)
    else:
        shutil.rmtree(path, onerror=lambda fn, p, e: onerror(fn, p, e))


class LayoutError(Exception):
    def __init__(self, message, code="layout"):
        super().__init__(message)
        self.code = code


class Layout:
    def __init__(self, root):
        self.root = os.path.abspath(root)
        self.data = os.path.join(self.root, "data")
        self.disc = os.path.join(self.data, "disc")
        self.gen_root = os.path.join(self.data, "gen")
        self.cache = os.path.join(self.data, "cache")
        self.objects = os.path.join(self.cache, "objects")
        self.shaders = os.path.join(self.cache, "shaders")
        self.gamedata = os.path.join(self.data, "gamedata")
        self.install_json = os.path.join(self.data, "install.json")
        self.builds_dir = os.path.join(self.root, "builds")
        self.active_json = os.path.join(self.root, "active.json")
        self.game_link = os.path.join(self.root, "game")
        self.config = os.path.join(self.root, "config")
        self.cfg = os.path.join(self.config, "dotheroes.cfg")
        self.logs = os.path.join(self.root, "logs")
        self.backups_dir = os.path.join(self.root, "backups", "saves")

    # ---- what the folder holds ----
    def kind(self):
        """slots: this layout; setup-app: the setup app's (the game beside its data); partial: a first install
        not finished; none: nothing of ours"""
        if os.path.exists(self.active_json):
            return "slots"
        if os.path.exists(os.path.join(self.root, exe_name())) and os.path.exists(self.install_json):
            return "setup-app"
        if os.path.exists(self.install_json) or os.path.isdir(self.builds_dir):
            return "partial"
        return "none"

    def ensure_dirs(self):
        for d in (self.data, self.gen_root, self.objects, self.shaders, self.gamedata,
                  os.path.join(self.data, "dev_flash"), self.builds_dir, self.config, self.logs, self.backups_dir):
            os.makedirs(d, exist_ok=True)

    # ---- the active build and the ones kept ----
    def read_active(self):
        st = read_json(self.active_json, None) or {}
        return {"layout": LAYOUT, "active": st.get("active"), "previous": list(st.get("previous") or []),
                "retention": int(st.get("retention", DEFAULT_RETENTION))}

    def write_active(self, st):
        write_json(self.active_json, {"layout": LAYOUT, "active": st["active"], "previous": st["previous"],
                                      "retention": st["retention"]})

    def build_dir(self, bid):
        return os.path.join(self.builds_dir, bid)

    def build_info(self, bid):
        return read_json(os.path.join(self.build_dir(bid), "build.json"))

    def builds(self):
        """{id: build.json} of every finished slot"""
        out = {}
        if os.path.isdir(self.builds_dir):
            for n in sorted(os.listdir(self.builds_dir)):
                if n.startswith("."):
                    continue
                info = self.build_info(n)
                if info:
                    out[n] = info
        return out

    def retained(self, st=None):
        st = st or self.read_active()
        return [b for b in [st["active"]] + st["previous"] if b]

    def check_build(self, bid, full=False):
        """None when the slot is whole (its game there, its size -- or with full its SHA-256 -- as recorded), or why
        not"""
        info = self.build_info(bid)
        if not info:
            return "no record of it (build.json)"
        exe = os.path.join(self.build_dir(bid), exe_name())
        if not os.path.exists(exe):
            return "its game is missing"
        if os.path.getsize(exe) != info.get("exe_size"):
            return "its game is not the size recorded"
        if full and sha256_file(exe) != info.get("exe_sha256"):
            return "its game is not the one recorded (SHA-256)"
        return None

    # ---- a new build: staged, recorded, promoted ----
    def clean_staging(self):
        """slots left by an operation that did not finish"""
        removed = []
        if os.path.isdir(self.builds_dir):
            for n in os.listdir(self.builds_dir):
                if n.startswith(".staging-"):
                    rmtree(os.path.join(self.builds_dir, n))
                    removed.append(n)
        return removed

    def stage(self):
        os.makedirs(self.builds_dir, exist_ok=True)
        d = os.path.join(self.builds_dir, f".staging-{os.getpid()}-{now_id()}")
        os.makedirs(d)
        return d

    def record(self, staging, info):
        """build.json and build.cfg into a staged slot; info has lift, objects, shader_cache, version"""
        exe = os.path.join(staging, exe_name())
        sha = sha256_file(exe)
        info = dict(info)
        info.update(exe_sha256=sha, exe_size=os.path.getsize(exe), made=datetime.datetime.now().isoformat(
            timespec="seconds"))
        info.setdefault("id", f"{now_id()}-{sha[:8]}")
        write_json(os.path.join(staging, "build.json"), info)
        with open(os.path.join(staging, "build.cfg"), "w", encoding="utf-8", newline="\n") as f:
            f.write("# this build's place in its install (the pipeline worker writes it)\n")
            f.write(f"DOTHEROES_BUILD_ID={info['id']}\n")
            f.write(f"DOTHEROES_SHADER_CACHE_ID={info.get('shader_cache') or 'default'}\n")
        return info

    def promote(self, staging, info):
        """the staged slot under its id; an identical build already kept: that one (the staging removed)"""
        for bid, other in self.builds().items():
            if other.get("exe_sha256") == info["exe_sha256"] and not self.check_build(bid):
                rmtree(staging)
                return bid
        dst = self.build_dir(info["id"])
        if os.path.exists(dst):
            raise LayoutError(f"a build slot named {info['id']} is already there")
        os.replace(staging, dst)
        return info["id"]

    def activate(self, bid):
        """bid becomes the active build; the one before it is kept for rollback, as many as the retention says"""
        if self.check_build(bid):
            raise LayoutError(f"build {bid} can't be made active: {self.check_build(bid)}", "build_damaged")
        st = self.read_active()
        prev = [b for b in [st["active"]] + st["previous"] if b and b != bid]
        st["active"], st["previous"] = bid, prev[:max(0, st["retention"])]
        self.write_active(st)
        self.point_game(bid)
        return st

    def rollback(self, to=None):
        """back to a kept build (the newest before the active one by default): active.json and game/, nothing
        rebuilt, nothing downloaded"""
        st = self.read_active()
        if not st["previous"]:
            raise LayoutError("there is no earlier build to go back to", "no_previous")
        target = to or st["previous"][0]
        if target not in st["previous"]:
            raise LayoutError(f"build {target} is not one of the kept builds", "no_previous")
        why = self.check_build(target, full=True)
        if why:
            raise LayoutError(f"build {target} can't be used: {why}", "build_damaged")
        st["previous"] = [b for b in [st["active"]] + st["previous"] if b and b != target][:max(1, st["retention"])]
        st["active"] = target
        self.write_active(st)
        self.point_game(target)
        return st

    def set_retention(self, n):
        st = self.read_active()
        st["retention"] = max(0, int(n))
        st["previous"] = st["previous"][:st["retention"]]
        self.write_active(st)
        return st

    def point_game(self, bid):
        """game/ -> builds/<bid>; repaired whenever it points elsewhere"""
        target = self.build_dir(bid)
        if is_link(self.game_link):
            try:
                if os.path.samefile(self.game_link, target):
                    return
            except OSError:
                pass
            remove_link(self.game_link)
        elif os.path.exists(self.game_link):
            raise LayoutError(f"{self.game_link} is in the way (not a link)")
        make_link(self.game_link, target)

    def prune(self):
        """everything no kept build uses: the other slots, lifts, objects and shader folders. Returns what was
        removed and how many bytes."""
        st = self.read_active()
        keep = set(self.retained(st))
        removed, freed = [], 0
        builds = self.builds()
        for bid in list(builds):
            if bid not in keep:
                d = self.build_dir(bid)
                freed += tree_size(d)
                rmtree(d)
                removed.append(f"builds/{bid}")
                del builds[bid]
        lifts = {b.get("lift") for b in builds.values()}
        objects = set().union(*[set(b.get("objects") or ()) for b in builds.values()]) if builds else set()
        shaders = {b.get("shader_cache") for b in builds.values()}
        if os.path.isdir(self.gen_root):
            for n in os.listdir(self.gen_root):
                if n not in lifts and not n.startswith("."):
                    p = os.path.join(self.gen_root, n)
                    freed += tree_size(p)
                    rmtree(p)
                    removed.append(f"data/gen/{n}")
        if os.path.isdir(self.objects) and builds:
            for n in os.listdir(self.objects):
                if n.endswith(".o") and n not in objects:
                    p = os.path.join(self.objects, n)
                    freed += os.path.getsize(p)
                    os.remove(p)
                    removed.append(f"data/cache/objects/{n}")
        if os.path.isdir(self.shaders) and builds:
            for n in os.listdir(self.shaders):
                p = os.path.join(self.shaders, n)
                if os.path.isdir(p) and n not in shaders:
                    freed += tree_size(p)
                    rmtree(p)
                    removed.append(f"data/cache/shaders/{n}")
        return {"removed": removed, "freed": freed}

    def kept_objects(self):
        """object names the kept builds link (never removed by a new build's own clean-up)"""
        out = set()
        for bid in self.retained():
            out |= set((self.build_info(bid) or {}).get("objects") or ())
        return out

    # ---- saves ----
    def backups(self):
        out = []
        if os.path.isdir(self.backups_dir):
            for n in sorted(os.listdir(self.backups_dir), reverse=True):
                info = read_json(os.path.join(self.backups_dir, n, "backup.json"))
                if info and os.path.isdir(os.path.join(self.backups_dir, n, "gamedata")):
                    out.append(info)
        return out

    def backup_saves(self, reason="manual", note=None):
        """a copy of data/gamedata (all of it: the saves and the system's save data), never written over"""
        os.makedirs(self.backups_dir, exist_ok=True)
        bid = f"{now_id()}-{reason}"
        n = 1
        while os.path.exists(os.path.join(self.backups_dir, bid)):
            n += 1
            bid = f"{now_id()}-{reason}-{n}"
        part = os.path.join(self.backups_dir, "." + bid + ".part")
        if os.path.exists(part):
            rmtree(part)
        os.makedirs(part)
        src = self.gamedata
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(part, "gamedata"))
        else:
            os.makedirs(os.path.join(part, "gamedata"))
        files = sum(len(fs) for _, _, fs in os.walk(os.path.join(part, "gamedata")))
        info = {"id": bid, "reason": reason, "note": note, "made": datetime.datetime.now().isoformat(
            timespec="seconds"), "bytes": tree_size(os.path.join(part, "gamedata")), "files": files,
            "automatic": reason in AUTO_REASONS}
        write_json(os.path.join(part, "backup.json"), info)
        os.replace(part, os.path.join(self.backups_dir, bid))
        return info

    def restore_saves(self, bid):
        """the saves replaced by a backup's -- the current ones backed up first (before-restore), so nothing is
        lost; the swap is a rename of whole folders"""
        src = os.path.join(self.backups_dir, bid, "gamedata")
        if not os.path.isdir(src):
            raise LayoutError(f"no backup named {bid}", "no_backup")
        before = self.backup_saves("before-restore", note=f"the saves as they were before {bid} was restored")
        incoming = self.gamedata + ".restoring"
        if os.path.exists(incoming):
            rmtree(incoming)
        shutil.copytree(src, incoming)
        old = self.gamedata + ".replaced"
        if os.path.exists(old):
            rmtree(old)
        if os.path.exists(self.gamedata):
            os.replace(self.gamedata, old)
        os.replace(incoming, self.gamedata)
        if os.path.exists(old):
            rmtree(old)
        return {"restored": bid, "previous_saves": before["id"]}

    def delete_backup(self, bid):
        p = os.path.join(self.backups_dir, bid)
        if not os.path.isdir(p) or not os.path.exists(os.path.join(p, "backup.json")):
            raise LayoutError(f"no backup named {bid}", "no_backup")
        rmtree(p)
        return {"deleted": bid}

    def prune_backups(self, keep_auto=AUTO_BACKUPS_KEPT):
        """automatic backups beyond the newest keep_auto; a manual one is never removed here"""
        auto = [b for b in self.backups() if b.get("automatic")]
        gone = []
        for b in auto[keep_auto:]:
            rmtree(os.path.join(self.backups_dir, b["id"]))
            gone.append(b["id"])
        return gone

    # ---- the setup app's layout, moved into this one ----
    def legacy_parts(self):
        """what the setup app put beside the game, by name: the game, its libraries, notices and icon, the
        settings file"""
        out = []
        for n in os.listdir(self.root):
            p = os.path.join(self.root, n)
            if os.path.isfile(p) and (n == exe_name() or n.endswith((".dll", ".txt")) or n in ("icon.png",
                                                                                                  "dotheroes.cfg")):
                out.append(n)
        lib = os.path.join(self.root, "lib")
        if os.path.isdir(lib):
            out.append("lib")
        return sorted(out)

    def move_legacy_data(self, lift_id, shader_id):
        """the setup app's data into this layout, each move done once (a migration interrupted is taken up again):
        the recompiled code into data/gen/<lift_id> (when given), the objects into data/cache/objects, the
        translated shaders into data/cache/shaders/<shader_id>, the settings into config/, the log into logs/"""
        moved = []
        self.ensure_dirs()
        legacy_gen = [n for n in ("out", "recompiled", "spu", "spu_native", "done.json")
                      if os.path.exists(os.path.join(self.gen_root, n))]
        if legacy_gen and lift_id:
            dst = os.path.join(self.gen_root, lift_id)
            os.makedirs(dst, exist_ok=True)
            for n in legacy_gen:
                if not os.path.exists(os.path.join(dst, n)):
                    os.replace(os.path.join(self.gen_root, n), os.path.join(dst, n))
                    moved.append(f"data/gen/{n} -> data/gen/{lift_id}/{n}")
        for n in os.listdir(self.cache):
            p = os.path.join(self.cache, n)
            if os.path.isfile(p) and (n.endswith(".o") or n == "timings.json"):
                os.replace(p, os.path.join(self.objects, n))
                moved.append(f"data/cache/{n} -> data/cache/objects/")
        rec = os.path.join(self.shaders, "native-recipes.bin")   # M9.2's recipes: shared by every build here
        if os.path.isfile(rec) and not os.path.exists(os.path.join(self.cache, "native-recipes.bin")):
            os.replace(rec, os.path.join(self.cache, "native-recipes.bin"))
            moved.append("data/cache/shaders/native-recipes.bin -> data/cache/native-recipes.bin")
        loose = [n for n in os.listdir(self.shaders) if os.path.isfile(os.path.join(self.shaders, n))]
        if loose:
            dst = os.path.join(self.shaders, shader_id)
            os.makedirs(dst, exist_ok=True)
            for n in loose:
                os.replace(os.path.join(self.shaders, n), os.path.join(dst, n))
            moved.append(f"data/cache/shaders/ ({len(loose)} files) -> data/cache/shaders/{shader_id}/")
        old_cfg = os.path.join(self.root, "dotheroes.cfg")
        if os.path.exists(old_cfg) and not os.path.exists(self.cfg):
            os.replace(old_cfg, self.cfg)
            moved.append("dotheroes.cfg -> config/dotheroes.cfg")
        old_log = os.path.join(self.data, "dotheroes.log")
        if os.path.exists(old_log):
            os.replace(old_log, os.path.join(self.logs, "game.setup-app.log"))
            moved.append("data/dotheroes.log -> logs/game.setup-app.log")
        return moved

    def remove_legacy_game(self):
        """the setup app's game and its libraries beside the data, once a build slot is active"""
        removed = []
        for n in self.legacy_parts():
            if n == "dotheroes.cfg":
                continue
            p = os.path.join(self.root, n)
            rmtree(p) if os.path.isdir(p) else os.remove(p)
            removed.append(n)
        return removed

    def summary(self):
        st = self.read_active() if self.kind() == "slots" else None
        return {"kind": self.kind(), "active": st and st["active"], "previous": st and st["previous"],
                "retention": st and st["retention"], "builds": sorted(self.builds()),
                "game_link": is_link(self.game_link), "backups": len(self.backups())}
