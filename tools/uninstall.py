#!/usr/bin/env python3
"""Uninstalling the game (M9: docs/M9_LAUNCHER_PLAN.md section 20), part by part, from an install the launcher
manages (tools/install_layout.py) -- never anything outside it.

    plan(install, parts)            what each part holds: {part: {"bytes", "files"}}, nothing removed
    remove(install, parts, exe)     the parts removed; {"removed": {part: {"bytes", "files"}}, "kept": [...]}

The parts (DEFAULT_PARTS keeps the saves, their backups and the settings):
    builds     the generated game builds (builds/), game/ (a link: the link only), active.json, the logs
    data       the disc's files and the lift made from them (data/disc, data/gen, data/dev_flash, install.json), and
               developer mode's test saves and lists (data/devmode: test data only, never the player's saves)
    shaders    the shader and pipeline caches (data/cache/shaders) and the recipes
    objects    the compilation caches (data/cache/objects; the compiler's own, data/cache/zig-global and zig-local)
    settings   the settings (config/)
    saves      the saves (data/gamedata) and their backups (backups/)
    shortcuts  the Start-menu and desktop shortcuts and the Steam entries made for the launcher (`exe`)

Safety: the install must be one the launcher made (active.json, data/install.json or builds/) and not itself a
link; every path removed is inside it with no link on the way; a link (junction, symlink) met inside is removed as a
link, never followed; disc images (.iso) and keys files are never removed, wherever they are; nothing at all while
the game runs from this install or a preparation holds its lock."""
import os
import stat
import sys

ROOT = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import install_layout as layout  # noqa: E402

PARTS = ("builds", "data", "shaders", "objects", "settings", "saves", "shortcuts")
DEFAULT_PARTS = ("builds", "data", "shaders", "objects", "shortcuts")
PROTECTED_EXT = (".iso", ".keys")


class UninstallError(Exception):
    def __init__(self, message, code="failed"):
        super().__init__(message)
        self.code = code


def part_paths(lay):
    """each part's paths (files or folders) in the install"""
    return {
        "builds": [lay.builds_dir, lay.game_link, lay.active_json, lay.logs],
        "data": [lay.disc, lay.gen_root, os.path.join(lay.data, "dev_flash"), lay.install_json,
                 os.path.join(lay.data, "devmode")],
        "shaders": [lay.shaders, os.path.join(lay.cache, "native-recipes.bin"), os.path.join(lay.cache, "prepare.lock"),
                    os.path.join(lay.cache, "prepare-part.bin")],
        "objects": [lay.objects, lay.zig_global, lay.zig_local],
        "settings": [lay.config],
        "saves": [lay.gamedata, os.path.dirname(lay.backups_dir)],
        "shortcuts": [],
    }


def protected(path):
    """a disc image or a keys file: never removed"""
    n = os.path.basename(path).lower()
    return n.endswith(PROTECTED_EXT) or ("key" in n and n.endswith(".txt"))


def _check_install(lay):
    root = lay.root
    if layout.is_link(root):
        raise UninstallError("The install folder is a link: nothing is removed through it.", "unsafe_path")
    if not os.path.isdir(root):
        raise UninstallError("There is no install in this folder.", "no_install")
    drive_root = os.path.splitdrive(root)[0] + os.sep
    home = os.path.expanduser("~")
    if os.path.normcase(root) in (os.path.normcase(drive_root), os.path.normcase(home), "/"):
        raise UninstallError("This folder is a drive or home folder, not an install.", "unsafe_path")
    if not (os.path.exists(lay.active_json) or os.path.exists(lay.install_json) or os.path.isdir(lay.builds_dir)):
        raise UninstallError("This folder doesn't hold an install the launcher made.", "no_install")


def _inside(root, path):
    """the path lies in root, and no folder between them is a link (so nothing is reached through one)"""
    r = os.path.normcase(os.path.abspath(root))
    p = os.path.normcase(os.path.abspath(path))
    if p == r or os.path.commonpath([r, p]) != r:
        return False
    cur = os.path.dirname(os.path.abspath(path))
    while os.path.normcase(cur) != r:
        if layout.is_link(cur):
            return False
        nxt = os.path.dirname(cur)
        if nxt == cur:
            return False
        cur = nxt
    return True


def _walk(path, root, delete, acc):
    """the bytes and files under path (removed when delete); links removed as links, never followed"""
    if not os.path.lexists(path):
        return
    if not _inside(root, path):
        raise UninstallError(f"A path outside the install was refused: {path}", "unsafe_path")
    if layout.is_link(path):
        if delete:
            if os.path.isdir(path) or os.name == "nt" and getattr(os.path, "isjunction", lambda _: False)(path):
                layout.remove_link(path)
            else:
                os.unlink(path)
        acc["links"] += 1
        return
    if os.path.isdir(path):
        with os.scandir(path) as it:
            entries = [e.path for e in it]
        for e in entries:
            _walk(e, root, delete, acc)
        if delete:
            try:
                os.rmdir(path)
            except OSError:
                acc["left"].append(path)   # something protected stayed in it
        return
    if protected(path):
        acc["protected"].append(path)
        return
    try:
        size = os.lstat(path).st_size
    except OSError:
        size = 0
    if delete:
        try:
            os.remove(path)
        except PermissionError:
            try:
                os.chmod(path, stat.S_IWRITE)
                os.remove(path)
            except PermissionError:   # held open (in a portable root: the running worker's own log in logs/)
                acc["left"].append(path)
                return
    acc["bytes"] += size
    acc["files"] += 1


def _measure(lay, part, delete):
    acc = {"bytes": 0, "files": 0, "links": 0, "protected": [], "left": []}
    for p in part_paths(lay)[part]:
        _walk(p, lay.root, delete, acc)
    return acc


def _launcher_program(root, exe):
    """a program of the launcher's package in root (the launcher, its worker, the update's helper copy): in a
    portable root these run from the install's folder, and they are not the game"""
    return any(_within(os.path.join(root, n), exe) for n in PACKAGE_ITEMS + UPDATE_LEFTOVERS)


def running_from(root):
    """the processes whose program lies in root (the game from this install), as their paths -- never the
    launcher's own programs (a portable root holds them too)"""
    found = _running_from(root)
    return [p for p in found if not _launcher_program(root, p)]


def _running_from(root):
    root_n = os.path.normcase(os.path.abspath(root)) + os.sep
    found = []
    if os.name == "nt":
        import ctypes
        import ctypes.wintypes as w
        psapi, k32 = ctypes.WinDLL("psapi"), ctypes.WinDLL("kernel32")
        k32.OpenProcess.restype = w.HANDLE
        pids = (w.DWORD * 4096)()
        got = w.DWORD()
        if not psapi.EnumProcesses(pids, ctypes.sizeof(pids), ctypes.byref(got)):
            return found
        for pid in pids[:got.value // ctypes.sizeof(w.DWORD)]:
            h = k32.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
            if not h:
                continue
            buf = ctypes.create_unicode_buffer(32768)
            n = w.DWORD(len(buf))
            if k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)) and \
                    os.path.normcase(buf.value).startswith(root_n):
                found.append(buf.value)
            k32.CloseHandle(h)
    else:
        for d in os.listdir("/proc") if os.path.isdir("/proc") else []:
            if d.isdigit():
                try:
                    exe = os.readlink(f"/proc/{d}/exe")
                except OSError:
                    continue
                if os.path.normcase(exe).startswith(root_n):
                    found.append(exe)
    return found


def _check_idle(lay):
    if running_from(lay.root):
        raise UninstallError("The game is running from this install: close it first.", "game_running")
    lock = os.path.join(lay.cache, "prepare.lock")
    if os.path.exists(lock):
        try:
            with open(lock) as f:
                pid = int(f.read().strip() or 0)
        except (OSError, ValueError):
            pid = 0
        import updates
        if pid and updates._alive(pid):
            raise UninstallError("Shaders are being prepared for this install: wait or stop it first.", "busy")


def plan(install, parts=PARTS):
    """what each part would free: {part: {"bytes", "files"}} (shortcuts: none counted)"""
    lay = layout.Layout(install)
    _check_install(lay)
    out = {}
    for part in parts:
        if part not in PARTS:
            raise UninstallError(f"Unknown part: {part}", "bad_request")
        acc = _measure(lay, part, False)
        out[part] = {"bytes": acc["bytes"], "files": acc["files"]}
    return out


def remove(install, parts=DEFAULT_PARTS, exe=None, roots=None):
    """the parts removed (shortcuts: those made for the launcher `exe`); the rest of the folder kept"""
    lay = layout.Layout(install)
    _check_install(lay)
    _check_idle(lay)
    for part in parts:
        if part not in PARTS:
            raise UninstallError(f"Unknown part: {part}", "bad_request")
    removed, protected_seen, left = {}, [], []
    for part in parts:
        if part == "shortcuts":
            removed[part] = {"removed": remove_shortcuts(exe, roots) if exe else [],
                             # Steam running: its entry for the launcher waits; the launcher removes it once Steam is
                             # closed (the worker's `shortcuts --remove-steam`)
                             "steam_later": bool(exe) and steam_entry_left(exe, roots)}
            continue
        acc = _measure(lay, part, True)
        removed[part] = {"bytes": acc["bytes"], "files": acc["files"], "links": acc["links"]}
        protected_seen += acc["protected"]
        left += [p for p in acc["left"] if os.path.isfile(p)]   # a file held open: left, said
    for d in (lay.cache, os.path.join(lay.root, "backups"), lay.data):   # folders left empty go too
        try:
            if os.path.isdir(d) and not layout.is_link(d) and not os.listdir(d):
                os.rmdir(d)
        except OSError:
            pass
    return {"removed": removed, "kept": [p for p in PARTS if p not in parts], "protected": protected_seen,
            "left": left, "bytes": sum(v.get("bytes", 0) for v in removed.values())}


def _steam_lists(roots=None):
    """every Steam account's shortcuts list on this PC (or under `roots`)"""
    import shortcuts
    found = list(roots) if roots is not None else shortcuts.steam_roots()
    targets = []
    for root in found:
        try:
            targets += [os.path.join(cfg, "shortcuts.vdf") for cfg in shortcuts.steam_accounts(root)]
        except OSError:
            pass
    return [t for t in targets if os.path.exists(t)]


def _our_keys(entries, exe):
    want = os.path.normcase(os.path.abspath(exe))
    return [k for k, e in entries.items()
            if os.path.normcase(os.path.abspath(str(e.get("Exe", "")).strip().strip('"'))) == want]


def steam_entry_left(exe, roots=None):
    """a Steam list still holds the launcher's entry (read only)"""
    import shortcuts
    for t in _steam_lists(roots):
        try:
            if _our_keys(shortcuts.read_shortcuts(t), exe):
                return True
        except Exception:   # noqa: BLE001 -- a list this can't read is not ours to judge
            pass
    return False


def remove_shortcuts(exe, roots=None, steam_running=None, links=True):
    """the shortcuts Cubic.Heroes made for the launcher `exe`, removed: its Start-menu and desktop shortcuts (`links`)
    and its own Steam entry (the one whose program is `exe`; every other entry is left exactly as it is). Steam
    rewrites its list as it exits, so while it runs its entry is left alone -- quietly: uninstalling never fails over
    it, and the launcher removes it once Steam is closed."""
    import shortcuts
    done = []
    if links and os.name == "nt":
        for folder in ("Programs", "Desktop"):
            try:
                shortcuts._powershell(
                    "$d = [Environment]::GetFolderPath($env:DDGH_FOLDER); $w = New-Object -ComObject WScript.Shell;"
                    "$p = Join-Path $d ($env:DDGH_NAME + '.lnk');"
                    "if ((Test-Path -LiteralPath $p) -and ($w.CreateShortcut($p).TargetPath -eq $env:DDGH_EXE)) "
                    "{ Remove-Item -LiteralPath $p }", DDGH_FOLDER=folder, DDGH_NAME=shortcuts.NAME, DDGH_EXE=exe)
                done.append(folder)
            except Exception:   # noqa: BLE001 -- a shortcut that can't be read is left as it is
                pass
    targets = _steam_lists(roots)
    running = shortcuts.steam_running() if steam_running is None else steam_running
    if not targets or running:
        return done
    for t in targets:
        try:
            entries = shortcuts.read_shortcuts(t)   # Steam's numbered map
        except Exception:   # noqa: BLE001 -- a list this can't read is left as it is
            continue
        ours = _our_keys(entries, exe)
        if not ours:
            continue   # nothing of ours: the file isn't written at all
        keep = [e for k, e in sorted(entries.items(), key=lambda kv: int(kv[0]) if str(kv[0]).isdigit() else 0)
                if k not in ours]
        shortcuts.write_shortcuts(t, {str(i): e for i, e in enumerate(keep)})
        done.append(t)
    return done


# ---- removing the launcher itself (M9: docs/M9_LAUNCHER_PLAN.md section 20; the portable root) ----
#
# Only a packaged download's folder (the launcher beside worker/, as tools/make_bundle.py lays it out; never a source
# checkout) and only the package's own items in it (install_layout.LAUNCHER_PACKAGE) and its update's leftovers
# (downloads/launcher-*): anything else there stays, and the folder goes only when nothing is left. Portable by
# default, the install lives in this very folder (its root): its builds, data, settings, saves, backups, logs and
# downloads stay, as an install in any folder of the player's inside it or anywhere else does. Only when the player
# chose everything (`everything`: a separate option, separately confirmed) does the install whose root is this folder
# go too -- every part of it, the saves and settings included (never a disc image or keys file, never through a
# link) -- and the root's marker with it; an install anywhere else still stays. The launcher's own data folder (its
# preferences, its job logs, a report being reviewed) goes too, only those items -- unless it lies in the install
# being kept. The Start-menu and desktop shortcuts and the Steam entry made for the launcher go (Steam open: its
# entry is left -- the dialog says to close Steam first). The launcher can't delete itself while it runs: it starts a
# copy of the worker (`remove-launcher --wait-pid`), which waits for it to exit. That copy runs from the operating
# system's temporary folder -- the one place outside the install root the worker ever writes, since the helper has
# to run from outside the folder it removes (tools/worker.py cmd_remove_launcher).

PACKAGE_ITEMS = layout.launcher_package() + (("libSDL2-2.0.so.0", "CubicHeroes.exe") if os.name != "nt" else
                                             ("CubicHeroes",))   # and the other platform's program name (as before)
UPDATE_LEFTOVERS = tuple(os.path.join("downloads", n) for n in ("launcher-new", "launcher-new.part",
                                                                 "launcher-previous", "launcher-failed",
                                                                 "launcher-helper")) + (".started-ok",)   # its health
DATA_ITEMS = ("launcher.json", "logs", "report", "imgui.ini")


def _worker_exe(folder):
    return os.path.join(folder, "worker", "DotHeroes-Setup.exe" if os.name == "nt" else "DotHeroes-Setup")


def _within(outer, inner):
    """inner is outer or lies inside it (by its path: links are another check)"""
    o, i = os.path.normcase(os.path.abspath(outer)), os.path.normcase(os.path.abspath(inner))
    try:
        return os.path.commonpath([o, i]) == o
    except ValueError:
        return False


def _holds_install(path):
    return any(os.path.exists(os.path.join(path, n)) for n in ("active.json", os.path.join("data", "install.json"))) \
        or os.path.isdir(os.path.join(path, "builds"))


def _check_launcher_folder(folder, install=None):
    folder = os.path.abspath(folder)
    if layout.is_link(folder):
        raise UninstallError("The launcher's folder is a link: nothing is removed through it.", "unsafe_path")
    if not os.path.isdir(folder):
        raise UninstallError("The launcher's folder isn't there.", "not_packaged")
    drive_root = os.path.splitdrive(folder)[0] + os.sep
    if os.path.normcase(folder) in (os.path.normcase(drive_root), os.path.normcase(os.path.expanduser("~")), "/"):
        raise UninstallError("This folder is a drive or home folder.", "unsafe_path")
    exe = os.path.join(folder, "CubicHeroes.exe" if os.name == "nt" else "CubicHeroes")
    if not (os.path.isfile(exe) and os.path.isfile(_worker_exe(folder))):
        raise UninstallError("This launcher isn't a downloaded copy: there is nothing to remove here.", "not_packaged")
    if os.path.exists(os.path.join(folder, ".git")) or os.path.exists(os.path.join(folder, "tools", "worker.py")):
        raise UninstallError("This launcher runs from a source folder: it isn't removed this way.", "not_packaged")
    # The install in the launcher's folder (portable: the folder is its root; or a folder of the player's in it) is
    # kept with everything that isn't the package's. One inside a package item would go with that item: refused.
    homes = [install] if install else []
    for n in PACKAGE_ITEMS:
        p = os.path.join(folder, n)
        if os.path.isdir(p) and not layout.is_link(p):
            with os.scandir(p) as it:
                homes += [p] + [e.path for e in it if e.is_dir(follow_symlinks=False) and _holds_install(e.path)]
    for h in homes:
        if h == install or _holds_install(h):
            for n in PACKAGE_ITEMS:
                if _within(os.path.join(folder, n), h):
                    raise UninstallError("The game is installed inside the launcher's own files: uninstall the game "
                                         "first, or keep the launcher.", "install_inside")
    return folder, exe


def _install_here(folder, install=None):
    """the folder itself when it is the install's root (portable): given as --install, or holding one; else None"""
    if install and os.path.normcase(os.path.abspath(install)) == os.path.normcase(folder):
        return folder
    return folder if _holds_install(folder) or os.path.lexists(os.path.join(folder, layout.ROOT_MARKER)) else None


def _root_items(folder):
    """the install's items at the root (and the root's marker): what `everything` removes besides the package"""
    return [n for n in layout.INSTALL_ITEMS + (layout.ROOT_MARKER,) if os.path.lexists(os.path.join(folder, n))]


def _data_items(data_dir, here, everything):
    """the launcher's data folder's items that go: none when the folder lies in the install being kept (left with
    it); in the root itself with everything, those that are not the install's (counted and removed with it)"""
    if not data_dir or not os.path.isdir(data_dir) or layout.is_link(data_dir):
        return []
    if here and not everything and _within(here, data_dir):
        return []
    same = bool(here) and os.path.normcase(os.path.abspath(data_dir)) == os.path.normcase(here)
    return [n for n in DATA_ITEMS if not (same and n in layout.INSTALL_ITEMS)]


def launcher_plan(folder, data_dir=None, install=None, everything=False):
    """what removing the launcher removes, nothing removed: {folder, bytes, files, items, kept_items, data_bytes,
    portable, install_kept, everything, install_bytes}"""
    folder, exe = _check_launcher_folder(folder, install)
    here = _install_here(folder, install)
    acc = {"bytes": 0, "files": 0, "links": 0, "protected": [], "left": []}
    items = [n for n in PACKAGE_ITEMS if os.path.lexists(os.path.join(folder, n))]
    for n in items + [x for x in UPDATE_LEFTOVERS if os.path.lexists(os.path.join(folder, x))]:
        _walk(os.path.join(folder, n), folder, False, acc)
    iacc = {"bytes": 0, "files": 0, "links": 0, "protected": [], "left": []}
    gone = set(PACKAGE_ITEMS)
    if everything and here:
        for n in _root_items(here):
            _walk(os.path.join(here, n), here, False, iacc)
        gone |= set(_root_items(here))
    dacc = {"bytes": 0, "files": 0, "links": 0, "protected": [], "left": []}
    for n in _data_items(data_dir, here, everything):
        _walk(os.path.join(data_dir, n), data_dir, False, dacc)
        if os.path.normcase(os.path.abspath(data_dir)) == os.path.normcase(folder):
            gone.add(n)
    others = sorted(n for n in os.listdir(folder) if n not in gone)
    import shortcuts
    return {"folder": folder, "exe": exe, "bytes": acc["bytes"] + dacc["bytes"] + iacc["bytes"],
            "files": acc["files"] + dacc["files"] + iacc["files"], "items": items, "kept_items": others,
            "data_bytes": dacc["bytes"], "portable": bool(here), "install_kept": bool(here) and not everything,
            "everything": bool(everything), "install_bytes": iacc["bytes"],
            "steam_open": bool(steam_entry_left(exe)) and bool(shortcuts.steam_running())}


def _wait_exit(pid, timeout):
    import time
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        if not _pid_alive(pid):
            return True
        time.sleep(0.25)
    return False


def _pid_alive(pid):
    if os.name == "nt":
        import ctypes
        k = ctypes.windll.kernel32
        h = k.OpenProcess(0x1000 | 0x00100000, False, int(pid))   # QUERY_LIMITED_INFORMATION | SYNCHRONIZE
        if not h:
            return False
        try:
            return k.WaitForSingleObject(h, 0) == 0x102   # WAIT_TIMEOUT: still running
        finally:
            k.CloseHandle(h)
    try:
        os.kill(int(pid), 0)
        return True
    except OSError:
        return False


def remove_launcher(folder, data_dir=None, install=None, wait_pid=None, timeout=120, everything=False, roots=None):
    """the launcher's package items (and its update's leftovers), its data folder's items and its shortcuts removed
    (after `wait_pid` exits); the install whose root is the folder kept -- removed too, every part of it, only with
    `everything`"""
    if wait_pid and not _wait_exit(wait_pid, timeout):
        raise UninstallError("The launcher is still running: nothing was removed.", "busy")
    folder, exe = _check_launcher_folder(folder, install)
    here = _install_here(folder, install)
    if everything and here:   # nothing at all while the game runs from it or a preparation holds its lock
        _check_idle(layout.Layout(here))
    out = {"folder": folder, "shortcuts": [], "bytes": 0, "files": 0, "left": [], "portable": bool(here),
           "install_kept": bool(here) and not everything, "everything": bool(everything)}
    try:
        out["shortcuts"] = remove_shortcuts(exe, roots)
    except Exception:   # noqa: BLE001 -- a shortcut that can't be removed never stops the rest
        pass
    out["steam_left"] = bool(steam_entry_left(exe, roots))
    acc = {"bytes": 0, "files": 0, "links": 0, "protected": [], "left": []}
    for n in PACKAGE_ITEMS + UPDATE_LEFTOVERS:
        p = os.path.join(folder, n)
        if os.path.lexists(p):
            _walk(p, folder, True, acc)
    dl = os.path.join(folder, "downloads")
    try:
        if os.path.isdir(dl) and not layout.is_link(dl) and not os.listdir(dl):
            os.rmdir(dl)   # the update's folder, when it held only the launcher's update
    except OSError:
        pass
    if everything and here:
        for n in _root_items(here):
            _walk(os.path.join(here, n), here, True, acc)
        out["install_removed"] = True
    gone_data = _data_items(data_dir, here, everything)
    for n in gone_data:
        _walk(os.path.join(data_dir, n), data_dir, True, acc)
    if gone_data and os.path.normcase(os.path.abspath(data_dir)) != os.path.normcase(folder):
        try:
            os.rmdir(data_dir)   # only when nothing else is in it
        except OSError:
            pass
    out["data_kept"] = bool(data_dir) and not gone_data
    try:
        os.rmdir(folder)   # only when the player kept nothing else there
        out["folder_removed"] = True
    except OSError:
        out["folder_removed"] = False
    out.update(bytes=acc["bytes"], files=acc["files"], left=acc["left"] + acc["protected"])
    return out
