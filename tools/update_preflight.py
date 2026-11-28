#!/usr/bin/env python3
"""The official v1.01 update: what an installation has, and whether the update can be installed -- the launcher's
checks before it offers Install (docs/OFFICIAL_UPDATE.md). Nothing of the game is changed or kept; the only thing
written is, when the computer has none, the keys that open the update (a few small files, downloaded from the pinned
public sources into the installation's data/keys: tools/key_download.py).

v1.00 is complete and fully supported; the update is optional. preflight() answers, for one installation:
  state       "not_installed" | "v100" | "v101" | "unsupported" (another game or version)
  checks      in order, each {id, status, title, message, remedies, details, values}:
                status   ok | warning | missing | failed | skipped
                message  what the player reads (plain words, real values), remedies the actions the launcher offers
                         for it (retry, download, choose_pkg, choose_folder, choose_elf, choose_keys, locate_rpcs3,
                         free_space, continue_v100), details the technical part (shown under "Show details")
  ready       every mandatory check passed (the update can be installed now, or after its download, which is
              itself checked before anything changes)
  plan        how it would be installed: {source: pkg | folder | download, path,
                                         decrypt: keys | downloaded | rpcs3 | elf | after_download}
  blockers    the messages of the checks that stop it (what a disabled Install says)

The update's inputs are looked for where a player keeps them, never by searching their computer: the package or
folder they chose, the one chosen before, the official package's own name beside their disc and in their Downloads
folder, the installation's own download folder, and an RPCS3 they set up (its installed copy of the update). The keys:
as for the game itself (tools/key_sources.py) -- chosen, remembered, beside the disc; else downloaded automatically
(pinned, checked); an RPCS3 they set up is an optional fallback, never required.

    update_preflight.py check INSTALL_DIR [--pkg P] [--folder D] [--elf E] [--keys K] [--rpcs3 R] [--offline]
"""
import hashlib
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import game_update  # noqa: E402
import key_download  # noqa: E402
import key_sources  # noqa: E402
import official_content  # noqa: E402
import player_keys  # noqa: E402

ROOT = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CONFIG = os.path.join(ROOT, "config")
BASE_ID = "BLUS30490-01.00"
UPDATE_ID = "BLUS30490-01.01"
PROGRAMS = os.path.join(CONFIG, "programs")   # per-program configuration: the folder whose program.json names it
# what a program's folder must hold for its build (tools/setup_pipeline.py PROGRAM_FILES and PORT_FILES)
PROGRAM_NEEDS = ("sjlj.json", "insn_patches.json", "r8_hooks.json", "audio_hooks.json", "m8_native_movie.json",
                 "r7_native_hooks.json", "excluded_code.json", "spu_relocatable.json", "spu_entry_hooks.json",
                 "port_addrs.inc", "port_aliases.h")


def program_dir(pid):
    """the folder under config/programs whose program.json names the program (us-01.01 for BLUS30490-01.01: folders
    are named by region and version), or None"""
    try:
        names = sorted(os.listdir(PROGRAMS))
    except OSError:
        return None
    for n in names:
        rec = _read_json(os.path.join(PROGRAMS, n, "program.json"))
        if isinstance(rec, dict) and rec.get("id") == pid:
            return os.path.join(PROGRAMS, n)
    return None
MARGIN = 512 << 20                            # free space kept beyond the estimate
GB = 1 << 30


def _gb(n):
    return f"{n / GB:.1f} GB"


def _check(cid, status, title, message, remedies=(), details="", **values):
    return {"id": cid, "status": status, "title": title, "message": message, "remedies": list(remedies),
            "details": details, "values": values}


def _read_json(p):
    try:
        with open(p, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def _dir_size(p):
    total = 0
    for dp, dn, fn in os.walk(p):
        for n in fn:
            try:
                total += os.path.getsize(os.path.join(dp, n))
            except OSError:
                pass
    return total


def _pins():
    gv = _read_json(os.path.join(CONFIG, "game_version.json")) or {}
    return gv, (gv.get("decrypted_elf") or {}).get("sha256"), ((gv.get("update") or {}).get("decrypted_elf") or {}).get("sha256")


# ---------------- the installation ----------------

def game_state(install):
    """{state, packages, title_id, version, record} of the installation: its own record (install.json) and its active
    build -- the game's files are not read"""
    rec = None
    for p in (os.path.join(install, "data", "install.json"), os.path.join(install, "install.json")):
        rec = _read_json(p)
        if rec is not None:
            break
    if not rec or not rec.get("elf_sha256"):
        return {"state": "not_installed", "packages": [], "record": rec or {}}
    gv, v100, v101 = _pins()
    try:
        packages = official_content.installed(install)
    except official_content.MappingError:
        packages = []
    if UPDATE_ID in packages or (v101 and rec.get("elf_sha256") == v101):
        state = "v101"
    elif rec.get("elf_sha256") == v100:
        state = "v100"
    else:
        state = "unsupported"
    return {"state": state, "packages": packages, "title_id": gv.get("title_id", ""), "region": gv.get("region", ""),
            "version": {"v100": "01.00", "v101": "01.01"}.get(state, ""), "record": rec}


def launcher_prefs(install):
    return _read_json(os.path.join(install, "config", "launcher.json")) or {}


def downloads_folder():
    """the player's Downloads folder (only the official package's own name is looked for there)"""
    if os.name == "nt":
        try:
            import ctypes
            from ctypes import wintypes
            guid = ctypes.c_byte * 16
            g = guid.from_buffer_copy(bytes.fromhex("90e24d373f126545916439c4925e467b"))   # FOLDERID_Downloads
            p = ctypes.c_wchar_p()
            if ctypes.windll.shell32.SHGetKnownFolderPath(ctypes.byref(g), 0, None, ctypes.byref(p)) == 0:
                out = p.value
                ctypes.windll.ole32.CoTaskMemFree(p)
                return out
        except Exception:   # noqa: BLE001 -- a convenience: no Downloads folder is no error
            pass
    d = os.path.join(os.path.expanduser("~"), "Downloads")
    return d if os.path.isdir(d) else None


def update_folder(install):
    """the installation's own folder for the update's package (a download goes here)"""
    return os.path.join(install, "data", "updates")


# ---------------- inputs ----------------

def package_candidates(install, disc=None, chosen=None, remembered=None):
    """[(path, how)] -- the update package's places, each once: the one chosen, the one chosen before, then the
    official package's own file name beside the disc, in the installation's own folder and in Downloads"""
    rec = game_update.record()
    out, seen = [], set()

    def add(p, how):
        if p and os.path.isfile(p):
            k = os.path.normcase(os.path.abspath(p))
            if k not in seen:
                seen.add(k)
                out.append((os.path.abspath(p), how))
    add(chosen, "chosen")
    add(remembered, "chosen before")
    for d, how in ((os.path.dirname(os.path.abspath(disc)) if disc else None, "beside your disc"),
                   (update_folder(install), "downloaded before"), (downloads_folder(), "in your Downloads folder")):
        if d:
            add(os.path.join(d, rec["file"]), how)
    return out


def folder_candidates(chosen=None, rpcs3=None):
    """[(path, how)] -- an already-extracted or installed update: the folder chosen, and the update an RPCS3 the
    player set up has installed (its dev_hdd0/game/BLUS30490)"""
    out = []
    if chosen and os.path.isdir(chosen):
        out.append((os.path.abspath(chosen), "chosen"))
    if rpcs3:
        base = os.path.dirname(os.path.abspath(rpcs3))
        for cand in (os.path.join(base, "dev_hdd0", "game", "BLUS30490"),
                     os.path.join(os.path.expanduser("~"), ".config", "rpcs3", "dev_hdd0", "game", "BLUS30490")):
            if os.path.isfile(os.path.join(cand, "USRDIR", "EBOOT.BIN")):
                out.append((cand, "in your RPCS3"))
                break
    return out


def _key_ring(sources):
    """every key the player's key sources hold, merged (none is shown or kept)"""
    ring = player_keys.Keyring()
    for path, how in sources.ordered():
        try:
            if player_keys.kind_of(path) in ("text", "folder", "zip", "keyfile"):
                ring.merge(player_keys.load(path))
        except (OSError, ValueError, UnicodeDecodeError):
            pass
    return ring


def _eboot_from_package(pkg, key):
    """the update's program (still encrypted) read from the package in memory"""
    import pkg_extract
    p = pkg_extract.Package(pkg)
    for it in p.items(key):
        if not it.is_dir and it.name == "USRDIR/EBOOT.BIN":
            buf = bytearray()
            p.read(key, it, buf.extend)
            return bytes(buf)
    raise ValueError("the package has no USRDIR/EBOOT.BIN")


# ---------------- the checks ----------------

def planned_state(disc, disc_key=None):
    """beta.6: the game state of an installation not made yet, from the disc it will be installed from (the launcher's
    install page offers v1.01 with the first install): "v100" when the disc is the supported US release v1.00 (its
    title, version and program, as the install's check reads them), else "unsupported" with the reason"""
    import setup_pipeline
    gv, _, _ = _pins()
    try:
        info = setup_pipeline.probe_disc(os.path.abspath(disc), disc_key) if disc else None
    except setup_pipeline.SetupError as e:
        return {"state": "unsupported", "packages": [], "record": {}, "planned": True, "reason": str(e)}
    if not info:
        return {"state": "unsupported", "packages": [], "record": {}, "planned": True, "reason": "No disc chosen."}
    return {"state": "v100", "packages": [], "title_id": gv.get("title_id", ""), "region": gv.get("region", ""),
            "version": "01.00", "record": {}, "planned": True}


def preflight(install, disc=None, pkg=None, folder=None, elf=None, keys=None, rpcs3=None, online=True, head=None,
              work_dir=None, key_download_allowed=True, planned=False, disc_key=None):
    """the update's checks for the installation at `install` (see the module's description). pkg / folder / elf /
    keys / rpcs3: what the player chose now (else what the launcher remembers); online False: the official download
    is not looked at and no key is downloaded; key_download_allowed False: no key is downloaded (tests, or the
    player's choice); head: a function url -> size or None (tests). planned (beta.6): before the game is installed,
    with the disc it will be installed from (and an encrypted image's disc_key) -- the install page's offer of v1.01;
    an installation that has the game is checked as it is."""
    install = os.path.abspath(install)
    prefs = launcher_prefs(install)
    up = prefs.get("update") or {}
    disc = disc or prefs.get("disc")
    rpcs3 = rpcs3 or prefs.get("rpcs3") or None
    rec = game_update.record()
    _, v100, v101 = _pins()
    checks, plan = [], {}
    gs = game_state(install)
    if planned and gs["state"] == "not_installed":
        gs = planned_state(disc, disc_key)

    # 1. the game
    if gs.get("planned"):
        if gs["state"] == "v100":
            checks.append(_check("game", "ok", "The game", "Your disc is 3D Dot Game Heroes (US, BLUS-30490) v1.00: the "
                                 "official v1.01 update can be installed with it.", version="01.00"))
        else:
            checks.append(_check("game", "failed", "The game", "The official update can be installed only with the "
                                 "US release of 3D Dot Game Heroes (BLUS-30490) v1.00.", ["continue_v100"],
                                 details=gs.get("reason", "")))
    elif gs["state"] == "not_installed":
        checks.append(_check("game", "missing", "The game", "Install the game first: the update is added to an "
                             "installed v1.00 game.", ["continue_v100"]))
    elif gs["state"] == "unsupported":
        checks.append(_check("game", "failed", "The game", "This installation isn't the US release of 3D Dot Game "
                             "Heroes (BLUS-30490) v1.00, which the official update is for.", ["continue_v100"],
                             details=f"installed program SHA-256 {gs['record'].get('elf_sha256', '')[:16]}..."))
    else:
        checks.append(_check("game", "ok", "The game", f"3D Dot Game Heroes (US, BLUS-30490) v"
                             f"{'1.01' if gs['state'] == 'v101' else '1.00'} is installed.", version=gs["version"]))
    if gs["state"] == "v101":
        checks.append(_check("already", "ok", "The update", "The v1.01 update is already installed: nothing to do."))
        return _result(gs, checks, plan, ready=False, already=True)
    if gs["state"] != "v100":
        return _result(gs, checks, plan, ready=False)

    # 2. this version of Cubic.Heroes can make the v1.01 program
    pdir = program_dir(UPDATE_ID)
    missing = [n for n in PROGRAM_NEEDS if not pdir or not os.path.isfile(os.path.join(pdir, n))]
    if not pdir or missing:
        checks.append(_check("support", "failed", "Update support", "This version of Cubic.Heroes can't install the "
                             "v1.01 update yet. Your v1.00 game is complete and keeps working.", ["continue_v100"],
                             details=f"no config/programs/*/program.json names {UPDATE_ID}" if not pdir else
                             "missing in " + os.path.basename(pdir) + ": " + ", ".join(missing)))
        return _result(gs, checks, plan, ready=False)

    # 2b. already in this installation: a v1.01 build kept (the player switched back to v1.00) -- made active again
    #     at once; or the update's files and program verified by an earlier attempt -- only the build is left
    import install_layout
    import official_update
    lay = install_layout.Layout(install)
    if os.path.exists(lay.active_json) and lay.newest_of(UPDATE_ID) and official_update.prepared(install, full=False):
        checks.append(_check("package", "ok", "The update", "v1.01 is already built in this installation: switching "
                             "to it takes a moment.", details=lay.newest_of(UPDATE_ID)))
        plan.update(source="kept", build=lay.newest_of(UPDATE_ID), decrypt="done")
        return _result(gs, checks, plan, ready=True)
    if official_update.prepared(install):
        checks.append(_check("package", "ok", "The update package", "The official v1.01 update's files are already in "
                             "this installation and verified.", details=official_update.folder(install)))
        checks.append(_check("keys", "ok", "Opening the update", "The update's program was decrypted before and "
                             "matches the official one."))
        plan.update(source="prepared", path=official_update.folder(install), decrypt="done")
        need = _need(install, plan)
        try:
            free = shutil.disk_usage(install).free
        except OSError:
            free = None
        ok = free is None or free >= need
        checks.append(_check("space", "ok" if ok else "failed", "Disk space", f"{_gb(free or 0)} free; the update "
                             f"needs about {_gb(need)}." if ok else f"Not enough free disk space. The update requires "
                             f"{_gb(need)}, but only {_gb(free)} is available.", [] if ok else
                             ["free_space", "retry", "continue_v100"], need=need, free=free))
        return _result(gs, checks, plan, ready=ok)

    # 3. the update's files: an already-updated folder, a package on this computer, or the official download
    src = None
    for path, how in folder_candidates(folder or up.get("folder"), rpcs3):
        try:
            game_update.check_folder(path)
            src = {"source": "folder", "path": path, "how": how}
            break
        except game_update.UpdateError as e:
            checks.append(_check("files", "warning", "The update's files", f"The update {how} isn't complete or "
                                 "isn't the official one, so it isn't used.", details=f"{path}: {e}"))
    bad_pkg = None
    if not src:
        for path, how in package_candidates(install, disc, pkg, up.get("pkg")):
            try:
                game_update.check_package(path, rec)
                src = {"source": "pkg", "path": path, "how": how}
                break
            except game_update.UpdateError as e:
                bad_pkg = bad_pkg or (path, how, str(e))
    if src:
        checks.append(_check("package", "ok", "The update package", {
            "pkg": f"The official v1.01 update package was found ({src['how']}) and verified.",
            "folder": f"The official v1.01 update's files were found ({src['how']}) and verified."}[src["source"]],
            details=src["path"], path=src["path"], how=src["how"]))
    else:
        size = None
        if online:
            try:
                size = (head or _head)(rec["url"])
            except Exception:   # noqa: BLE001
                size = None
        if bad_pkg and size is None:
            checks.append(_check("package", "failed", "The update package", "The update package was found, but its "
                                 "integrity check failed. Please select a valid official v1.01 package.",
                                 ["choose_pkg", "download", "continue_v100"], details=f"{bad_pkg[0]}: {bad_pkg[2]}"))
        elif size == rec["size"]:
            msg = (f"The official v1.01 update will be downloaded from Sony ({rec['size'] / (1 << 20):.0f} MB) and "
                   "verified before anything is changed.")
            if bad_pkg:
                msg = ("The update package found on this computer didn't pass its integrity check, so it isn't used. "
                       + msg)
            checks.append(_check("package", "ok", "The update package", msg, ["choose_pkg"],
                                 details=rec["url"] + (f"; refused: {bad_pkg[0]}" if bad_pkg else ""), size=rec["size"]))
            src = {"source": "download", "path": None, "how": "official download"}
        else:
            checks.append(_check("package", "missing", "The update package", "Your game is ready, but the v1.01 "
                                 "update package could not be found. You can download the official update or select "
                                 "a PKG file you already have.", ["download", "choose_pkg", "choose_folder",
                                                                   "continue_v100"],
                                 details=("Sony's server didn't answer (offline?)" if online else "not looked online")
                                 + (f"; refused: {bad_pkg[0]} ({bad_pkg[2]})" if bad_pkg else "")))
    plan.update(src or {})

    # 4. opening the update: the package key (for a package), and the program's NPDRM key with the free licence's
    #    pair -- the player's own keys, else (beta.6, the owner's decision of 2026-11-25) keys downloaded
    #    automatically from the pinned public sources (tools/key_download.py: a few small files into data/keys,
    #    fetched here, during the checks, so that "ready" is said only once the update's program has been opened);
    #    or a decrypted program the player has; or their RPCS3 (optional, never required)
    sources = key_sources.Sources(keys or prefs.get("keys"), [x for x in (prefs.get("keys_remembered") or []) if x],
                                  disc, rpcs3, install=install)
    ring = _key_ring(sources)
    elf_path = elf or up.get("elf")
    if elf_path and os.path.isfile(elf_path):
        try:
            ok = hashlib.sha256(open(elf_path, "rb").read()).hexdigest() == v101
        except OSError:
            ok = False
        if ok:
            plan["decrypt"], plan["elf"] = "elf", elf_path
        else:
            checks.append(_check("program", "warning", "The update's program", "The decrypted program you chose isn't "
                                 "the v1.01 update's, so it isn't used.", ["choose_elf"], details=elf_path))
    dl_error, auto = None, False
    if src and plan.get("decrypt") != "elf" and not (ring.has_update_keys() and (ring.npdrm or ring.npdrm_any)):
        try:
            got = key_download.downloaded(install, "npdrm") or (
                key_download.fetch(install, "npdrm") if online and key_download_allowed else [])
        except key_download.KeyDownloadError as e:
            got, dl_error = [], e
        for path, _ in got:
            try:
                ring.merge(player_keys.load(path))
                auto = True
            except (OSError, ValueError):
                pass
    dl_note = f"; the keys couldn't be downloaded: {dl_error}" if dl_error else (
        "" if online and key_download_allowed else "; keys not downloaded (offline)")
    no_keys = ["choose_elf", "choose_keys", "locate_rpcs3", "continue_v100"]
    if dl_error and dl_error.code in ("offline", "server", "interrupted", "tls"):
        no_keys = ["retry"] + no_keys
    if src and src["source"] == "pkg" and not ring.pkg and plan.get("decrypt") != "elf":
        checks.append(_check("keys", "missing", "Opening the update", "The update package can't be opened: the "
                             "package key isn't among your keys, and it couldn't be downloaded. Connect to the "
                             "internet and try again, or select keys that include it.",
                             ["retry", "choose_keys", "choose_folder", "continue_v100"],
                             details="needed: the PS3 package key (PKG / gpkg-key)" + dl_note))
    elif src and plan.get("decrypt") != "elf":
        eboot = None
        try:
            if src["source"] == "pkg":
                eboot = _eboot_from_package(src["path"], ring.pkg)
            elif src["source"] == "folder":
                with open(os.path.join(src["path"], "USRDIR", "EBOOT.BIN"), "rb") as f:
                    eboot = f.read()
        except (OSError, ValueError) as e:
            checks.append(_check("keys", "failed", "Opening the update", "The update package couldn't be opened with "
                                 "the keys available. Select keys that include the PS3 package key.",
                                 ["choose_keys", "continue_v100"], details=f"{type(e).__name__}: {e}"))
            eboot = False
        if eboot:
            try:
                _, how = key_sources.open_program(eboot, v101, sources, work_dir)
                plan["decrypt"] = {"rpcs3": "rpcs3", "downloaded": "downloaded"}.get(how.get("kind"), "keys")
                checks.append(_check("keys", "ok", "Opening the update", "The update's program opens with " + {
                    "rpcs3": "your RPCS3.", "downloaded": "the keys downloaded automatically."}.get(
                    plan["decrypt"], "your keys."), details=f"{how.get('how')}: {how.get('source')}"))
            except key_sources.NoProgram as e:
                checks.append(_check("keys", "failed", "Opening the update", "The update executable could not be "
                                     "decrypted using the available resources. Select an existing decrypted update "
                                     "executable, provide compatible keys, or configure an available supported RPCS3 "
                                     "installation." + (" The keys could also be downloaded automatically when you "
                                                        "are online." if dl_error else ""), no_keys,
                                     details="needed: " + ", ".join(e.need or ["NPDRM key, KLIC_FREE, KLIC_KEY"]) +
                                     "; tried: " + "; ".join(f"{t.get('how')}: {t.get('result')}" for t in e.tried)
                                     + dl_note))
        elif eboot is None and src["source"] == "download":
            if ring.has_update_keys() or sources.rpcs3:
                plan["decrypt"] = "after_download"
                checks.append(_check("keys", "ok", "Opening the update", ("The keys downloaded automatically" if auto
                                                                          else "Your keys") +
                                     (" or your RPCS3" if sources.rpcs3 and not ring.has_update_keys() else "") +
                                     " will open the update; they're checked right after the download, before "
                                     "anything is changed.",
                                     details=f"package key {'yes' if ring.pkg else 'no'}, free licence "
                                     f"{'yes' if ring.klic_free and ring.klic_key else 'no'}, NPDRM keys "
                                     f"{len(ring.npdrm) + len(ring.npdrm_any)}, RPCS3 {'yes' if sources.rpcs3 else 'no'}"
                                     + (", downloaded automatically" if auto else "")))
            else:
                checks.append(_check("keys", "missing", "Opening the update", "The keys that open the update couldn't "
                                     "be downloaded automatically. Connect to the internet and try again, or select "
                                     "your keys, a decrypted update program or an RPCS3 installation.", no_keys,
                                     details="needed: PKG, NPDRM key, KLIC_FREE, KLIC_KEY (or RPCS3)" + dl_note))
    elif plan.get("decrypt") == "elf":
        checks.append(_check("keys", "ok", "Opening the update", "The decrypted v1.01 program you have is used.",
                             details=plan["elf"]))

    # 5. disk space and permission, in the installation's folder (planned: the nearest folder that exists)
    if gs.get("planned"):
        plan["planned"] = True
    at = install
    while not os.path.isdir(at) and os.path.dirname(at) != at:
        at = os.path.dirname(at)
    need = _need(install, plan)
    try:
        free = shutil.disk_usage(at).free
    except OSError:
        free = None
    if free is not None and free < need:
        checks.append(_check("space", "failed", "Disk space", f"Not enough free disk space. The update requires "
                             f"{_gb(need)}, but only {_gb(free)} is available.", ["free_space", "retry",
                                                                                   "continue_v100"],
                             details=f"needed {need} bytes, free {free} bytes on {install}", need=need, free=free))
    else:
        checks.append(_check("space", "ok", "Disk space", f"{_gb(free or 0)} free; the update needs about {_gb(need)}.",
                             need=need, free=free))
    try:
        d = os.path.join(install, "data")
        fd, t = tempfile.mkstemp(prefix=".update-check-", dir=d if os.path.isdir(d) else at)
        os.close(fd)
        os.remove(t)
        checks.append(_check("write", "ok", "Permission", "The installation's folder can be written."))
    except OSError as e:
        checks.append(_check("write", "failed", "Permission", "The installation's folder can't be written, so the "
                             "update can't be added. Check the folder's permissions, then try again.",
                             ["retry", "continue_v100"], details=f"{install}: {e.strerror}"))

    # 6. this system
    import platform
    if os.name == "nt" or sys.platform.startswith("linux"):
        checks.append(_check("system", "ok", "This system", f"{platform.system()} {platform.machine()}: supported."))
    else:
        checks.append(_check("system", "failed", "This system", "This system isn't supported for the update.",
                             ["continue_v100"], details=platform.platform()))
    blocking = [c for c in checks if c["status"] in ("missing", "failed")]
    return _result(gs, checks, plan, ready=not blocking)


BUILD_WORK = 3 << 30    # the recompilation's working space at least (the program's lift, its objects, the build)


def _need(install, plan):
    """the free space the update needs: its download (if any), its files, a second build like the active one with
    its recompiled program and the compilation's working space (at least BUILD_WORK), and a margin"""
    rec = game_update.record()
    man = game_update.load_manifest()
    if plan.get("planned"):   # before the install: the update's download and files (the build is the install's own)
        return (rec["size"] if plan.get("source") == "download" else 0) + man["total_size"] + MARGIN
    lay = _read_json(os.path.join(install, "active.json")) or {}
    build = _dir_size(os.path.join(install, "builds", lay["active"])) if lay.get("active") else 0
    lift = (_read_json(os.path.join(install, "data", "install.json")) or {}).get("lift")
    gen = _dir_size(os.path.join(install, "data", "gen", lift)) if lift else 0
    return ((rec["size"] if plan.get("source") == "download" else 0) +
            (0 if plan.get("source") == "prepared" else man["total_size"]) +   # already in place: nothing more
            max(build + gen, BUILD_WORK) + MARGIN)


def _head(url):
    """the official package's size on Sony's server (a HEAD request), or None"""
    import urllib.request
    req = urllib.request.Request(url, method="HEAD", headers={"User-Agent": "Cubic.Heroes"})
    with urllib.request.urlopen(req, timeout=15) as r:
        n = r.headers.get("Content-Length")
        return int(n) if n and n.isdigit() else None


def _result(gs, checks, plan, ready, already=False):
    blockers = [c["message"] for c in checks if c["status"] in ("missing", "failed")]
    return {"state": gs["state"], "packages": gs.get("packages", []), "ready": bool(ready), "already": already,
            "checks": checks, "plan": plan, "blockers": blockers}


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["check"])
    ap.add_argument("install")
    ap.add_argument("--pkg")
    ap.add_argument("--folder")
    ap.add_argument("--elf")
    ap.add_argument("--keys")
    ap.add_argument("--rpcs3")
    ap.add_argument("--offline", action="store_true")
    a = ap.parse_args(argv)
    r = preflight(a.install, pkg=a.pkg, folder=a.folder, elf=a.elf, keys=a.keys, rpcs3=a.rpcs3, online=not a.offline)
    print(f"state {r['state']}; ready {r['ready']}; plan {r['plan']}")
    for c in r["checks"]:
        print(f"  [{c['status']:7s}] {c['title']}: {c['message']}" + (f"  (then: {', '.join(c['remedies'])})"
                                                                         if c["remedies"] else ""))
    return 0 if r["ready"] else 1


if __name__ == "__main__":
    sys.exit(main())
