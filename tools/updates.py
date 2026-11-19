#!/usr/bin/env python3
"""Updates from GitHub Releases (M9.3; docs/M9_LAUNCHER_PLAN.md section 12, docs/RELEASE_SIGNING.md).

    check(channel, installed, source)        the newest release of the channel (stable; prerelease: any), its
                                              manifest verified -- newer than the installed version or not
    download(manifest, platform, dest, ...)  its download for this platform, resumed where a previous attempt
                                              stopped, checked against the manifest (size, SHA-256)
    stage_launcher(zip, launcher_dir)        a downloaded launcher unpacked into <launcher_dir>/downloads/launcher-new
    stage_helper(launcher_dir, app_dir)      the worker's copy the swap runs from (<launcher_dir>/downloads/
                                              launcher-helper): it must not run from the worker/ it replaces
    swap_launcher(...)                       (a helper process, after the launcher exits) the new package's items in
                                              place of the old ones', started; put back if it does not start

The launcher's folder is, portable by default, also the install's root: an update replaces only the launcher
package's own items in it (install_layout.LAUNCHER_PACKAGE, and the new package's top items) and never moves,
renames or removes the install's (install_layout.INSTALL_ITEMS: builds/, data/, config/, logs/, backups/,
downloads/, active.json, game/) nor anything else the folder holds. Everything it stages or keeps stays inside the
folder: downloads/launcher-new (the package unpacked), downloads/launcher-previous (the replaced items, until the
new launcher reports a good start), downloads/launcher-failed (a new launcher that did not start), and
downloads/launcher-helper (the swap's own copy of the worker).

Nothing is installed from a release whose manifest is unsigned, signed by a key the launcher doesn't trust, or
altered, nor from a download that is not the one signed for. A check never blocks play: offline is a result
("offline"), not an error the launcher must stop at. The network is used here only: the release list, a
release's manifest and signature, and the downloads the player asked for. No telemetry: requests carry a generic
user agent and nothing of the player's.
"""
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import install_layout as layout  # noqa: E402
import release_trust as trust  # noqa: E402

REPO = "irnutsmurt/Cubic.Heroes"
GITHUB_API = f"https://api.github.com/repos/{REPO}/releases?per_page=30"
USER_AGENT = "CubicHeroes-Launcher"
MANIFEST, SIGNATURE = "release.json", "release.json.sig"
TIMEOUT_S = 20
CHUNK = 1 << 20


def platform_name():
    """this system and its processor, as the manifest names them (an x86-64 package is never taken elsewhere)"""
    import platform
    m = platform.machine().lower()
    arch = {"amd64": "x86_64", "x64": "x86_64", "x86_64": "x86_64"}.get(m, m or "unknown")
    return ("windows-" if os.name == "nt" else "linux-") + arch


def installed_kind():
    """the package kind this launcher came in: the .zip on Windows; on Linux the AppImage (it runs from its image,
    $APPIMAGE naming the file) or the tarball's folder"""
    if os.name == "nt":
        return "zip"
    return "appimage" if os.environ.get("APPIMAGE") else "tar.xz"


def pick_asset(assets, kind=None):
    """the download for this installation among the platform's: the same kind as the one installed (an AppImage is
    replaced by an AppImage, a tarball's folder by a tarball); None if the release has none of that kind"""
    kind = kind or installed_kind()
    for a in assets:
        if trust.asset_kind(a) == kind:
            return a
    return None


def _open(url, headers=None, timeout=TIMEOUT_S):
    h = {"User-Agent": USER_AGENT, "Accept": "application/octet-stream"}
    h.update(headers or {})
    return urllib.request.urlopen(urllib.request.Request(url, headers=h), timeout=timeout)


def _get(url, limit=1 << 20):
    """a small document (the release list, a manifest, a signature)"""
    try:
        with _open(url, {"Accept": "application/vnd.github+json"} if "api.github.com" in url else None) as r:
            data = r.read(limit + 1)
    except urllib.error.HTTPError as e:
        raise trust.ReleaseError(f"The update server answered {e.code} for {url.rsplit('/', 1)[-1]}.", "server")
    except (urllib.error.URLError, OSError, TimeoutError) as e:
        raise trust.ReleaseError("No connection to the update server (offline?).", "offline") from e
    if len(data) > limit:
        raise trust.ReleaseError("An update document is larger than expected.", "server")
    return data


def releases(source=None):
    """[{tag, prerelease, assets: {name: url}}] newest first: from GitHub's API, or a mirror for tests (a URL of
    the same JSON, or a folder holding releases.json)"""
    source = source or GITHUB_API
    if os.path.isdir(source):
        with open(os.path.join(source, "releases.json"), encoding="utf-8") as f:
            data = json.load(f)
        base = source
    else:
        data = json.loads(_get(source).decode("utf-8"))
        base = None
    out = []
    for r in data if isinstance(data, list) else []:
        if r.get("draft"):
            continue
        assets = {}
        for a in r.get("assets", []):
            url = a.get("browser_download_url") or ""
            if base and not url.startswith(("http://", "https://")):
                url = os.path.join(base, url)
            assets[a.get("name")] = url
        out.append({"tag": r.get("tag_name", ""), "prerelease": bool(r.get("prerelease")), "assets": assets,
                    "published": r.get("published_at")})
    return out


def _fetch(url, limit=1 << 20):
    if os.path.exists(url):
        with open(url, "rb") as f:
            return f.read(limit + 1)[:limit]
    return _get(url, limit)


def check(channel="stable", installed=None, source=None, keys=None):
    """the newest release of the channel whose manifest verifies; {available, version, notes, min_launcher,
    assets, tag, newer}. A release without a manifest is skipped (an older style); one whose manifest does not
    verify is reported, never used."""
    keys = trust.trusted_keys() if keys is None else keys
    if not keys:   # nothing could be verified: no request is made at all
        raise trust.ReleaseError("This launcher has no release key to check updates with.", "no_trusted_keys")
    rels = releases(source)
    refused = []
    for r in rels:
        if channel == "stable" and r["prerelease"]:
            continue
        if MANIFEST not in r["assets"]:
            continue
        mbytes = _fetch(r["assets"][MANIFEST])
        sig = _fetch(r["assets"][SIGNATURE], 1024) if SIGNATURE in r["assets"] else b""
        try:
            m = trust.verify_release(mbytes, sig, keys)
        except trust.ReleaseError as e:
            refused.append({"tag": r["tag"], "code": e.code, "why": str(e)})
            if e.code in ("no_trusted_keys", "no_crypto"):
                raise
            continue
        if channel == "stable" and m["channel"] != "stable":
            refused.append({"tag": r["tag"], "code": "channel", "why": "a prerelease's manifest on a stable release"})
            continue
        newer = installed is None or (trust.parse_version(m["version"]) or ()) > (trust.parse_version(installed) or ())
        return {"available": True, "tag": r["tag"], "version": m["version"], "channel": m["channel"],
                "notes": m.get("notes", ""), "min_launcher": m.get("min_launcher"), "newer": newer,
                "assets": [a for a in m["assets"] if a["platform"] == platform_name()], "manifest": m,
                "manifest_url": r["assets"][MANIFEST], "urls": r["assets"], "refused": refused}
    return {"available": False, "refused": refused}


def download(asset, url, dest_dir, progress=None, cancelled=None):
    """the asset into dest_dir: resumed from its .part (a Range request) when a previous attempt stopped, checked
    against the manifest's record before it is given its name. A file that fails the check is removed (the next
    attempt starts again). Returns the path."""
    os.makedirs(dest_dir, exist_ok=True)
    final = os.path.join(dest_dir, asset["name"])
    if os.path.exists(final):
        try:
            trust.verify_download(final, asset)
            return final
        except trust.ReleaseError:
            os.remove(final)
    part = final + ".part"
    have = os.path.getsize(part) if os.path.exists(part) else 0
    if have > asset["size"]:
        os.remove(part)
        have = 0
    free = shutil.disk_usage(dest_dir).free
    if free < asset["size"] - have + (64 << 20):
        raise trust.ReleaseError(f"Not enough free space for the download ({(asset['size'] - have) >> 20} MB "
                                 f"needed, {free >> 20} MB free).", "disk_space")
    if os.path.exists(url):   # a mirror on disk (tests)
        with open(url, "rb") as src, open(part, "ab") as out:
            src.seek(have)
            for chunk in iter(lambda: src.read(CHUNK), b""):
                if cancelled and cancelled():
                    raise trust.ReleaseError("Cancelled.", "cancelled")
                out.write(chunk)
                have += len(chunk)
                if progress:
                    progress(have, asset["size"])
    else:
        try:
            r = _open(url, {"Range": f"bytes={have}-"} if have else None, timeout=60)
        except urllib.error.HTTPError as e:
            if e.code == 416 and have == asset["size"]:
                r = None
            else:
                raise trust.ReleaseError(f"The download server answered {e.code}.", "server")
        except (urllib.error.URLError, OSError, TimeoutError) as e:
            raise trust.ReleaseError("No connection to the download server (offline?).", "offline") from e
        if r is not None:
            with r:
                if have and r.status != 206:   # the server ignored the range: from the start
                    have = 0
                    mode = "wb"
                else:
                    mode = "ab"
                with open(part, mode) as out:
                    while True:
                        if cancelled and cancelled():
                            raise trust.ReleaseError("Cancelled.", "cancelled")
                        try:
                            chunk = r.read(CHUNK)
                        except (OSError, TimeoutError) as e:
                            raise trust.ReleaseError("The download stopped (the connection was lost); it resumes "
                                                     "where it stopped.", "interrupted") from e
                        if not chunk:
                            break
                        out.write(chunk)
                        have += len(chunk)
                        if progress:
                            progress(have, asset["size"])
                        if have > asset["size"]:
                            break
    try:
        trust.verify_download(part, asset)
    except trust.ReleaseError:
        os.remove(part)
        raise
    os.replace(part, final)
    return final


# ---------------- the launcher's own update ----------------
#
# Before the portable root, the whole launcher folder was renamed (<dir>.new in its place, the old kept as <dir>.old).
# That can't be done when the install lives in the folder (it would move the saves, the builds and the very
# download being applied), and the item-wise swap below is as right for a launcher folder that holds no install --
# anything the player keeps there now simply stays -- so it is the only one. No launcher ever started the old swap
# (the commands were in place before the launcher's side), so nothing depends on <dir>.new.

STAGED, PREVIOUS, FAILED, HELPER = "launcher-new", "launcher-previous", "launcher-failed", "launcher-helper"


def update_dirs(launcher_dir):
    """where the launcher's update stages and keeps things, all inside its folder (downloads/)"""
    d = os.path.join(os.path.abspath(launcher_dir), "downloads")
    return {"staged": os.path.join(d, STAGED), "previous": os.path.join(d, PREVIOUS),
            "failed": os.path.join(d, FAILED), "helper": os.path.join(d, HELPER)}


def _not_package(name):
    """a name a launcher package may never put at the folder's top: the install's own items, the root's marker,
    the folder the update itself uses"""
    low = name.lower()
    return low in {n.lower() for n in layout.INSTALL_ITEMS} or low == layout.ROOT_MARKER.lower() or \
        low == HEALTH_FILE or name.startswith(".")


def stage_launcher(package, launcher_dir):
    """the launcher's package (a verified zip on Windows, tar.xz on Linux) unpacked into
    <launcher_dir>/downloads/launcher-new (one top folder in the archive: its contents); every member's path inside it
    (no member may escape), and no top item that is the install's (builds, data, config, ...: the package would
    replace them). A tarball keeps its programs' executable bits; tarfile's `data` filter refuses absolute paths,
    members or links leading outside and device files. An AppImage is never staged: it is replaced as a file (the
    image it runs from is read-only). Returns the staged folder."""
    new = update_dirs(launcher_dir)["staged"]
    part = new + ".part"
    for p in (part, new):
        if os.path.lexists(p):
            layout.rmtree(p)
    low = package.lower()
    if low.endswith(".appimage"):
        raise trust.ReleaseError("An AppImage is updated by downloading the new .AppImage file, not unpacked.",
                                 "bad_download")
    if os.name != "nt" and low.endswith(".zip"):
        raise trust.ReleaseError("A zip is Windows' package (it keeps no executable bits).", "bad_download")
    os.makedirs(part)
    try:
        if low.endswith((".tar.xz", ".txz")):
            import tarfile
            with tarfile.open(package, "r:xz") as t:
                for m in t.getmembers():
                    p = os.path.abspath(os.path.join(part, m.name))
                    if not p.startswith(part + os.sep) and p != part:
                        raise trust.ReleaseError(f"The launcher's package holds a path outside it ({m.name}).",
                                                 "bad_download")
                try:
                    t.extractall(part, filter="data")
                except tarfile.FilterError as e:
                    raise trust.ReleaseError(f"The launcher's package holds a member it may not ({e}).",
                                             "bad_download")
        else:
            with zipfile.ZipFile(package) as z:
                for n in z.namelist():
                    p = os.path.abspath(os.path.join(part, n))
                    if not p.startswith(part + os.sep) and p != part:
                        raise trust.ReleaseError(f"The launcher's package holds a path outside it ({n}).", "bad_download")
                z.extractall(part)
        entries = os.listdir(part)
        if len(entries) == 1 and os.path.isdir(os.path.join(part, entries[0])):   # one top folder: its contents
            os.replace(os.path.join(part, entries[0]), new)
            os.rmdir(part)
        else:
            os.replace(part, new)
        bad = sorted(n for n in os.listdir(new) if _not_package(n))
        if bad:
            raise trust.ReleaseError(f"The launcher's package holds the install's own items ({', '.join(bad)}).",
                                     "bad_download")
    except BaseException:
        for p in (part, new):
            if os.path.lexists(p):
                layout.rmtree(p)
        raise
    return new


def stage_helper(launcher_dir, app_dir):
    """the worker the swap runs as, copied from app_dir (the packaged worker: its program, its Python runtime,
    tools/ and config/ -- not its compiler or libraries) into <launcher_dir>/downloads/launcher-helper; the swap
    replaces worker/, which it therefore must not run from. Returns the copy's program."""
    dst = update_dirs(launcher_dir)["helper"]
    if os.path.lexists(dst):
        layout.rmtree(dst)
    os.makedirs(dst)
    exe = "DotHeroes-Setup.exe" if os.name == "nt" else "DotHeroes-Setup"
    shutil.copy2(os.path.join(app_dir, exe), dst)
    for d in ("_internal", "tools", "config"):
        if os.path.isdir(os.path.join(app_dir, d)):
            shutil.copytree(os.path.join(app_dir, d), os.path.join(dst, d), symlinks=True)
    return os.path.join(dst, exe)


HEALTH_FILE = ".started-ok"   # the new launcher writes it once it is up: the swap is then kept
HEALTH_TIMEOUT_S = 60
DETACHED = 0x00000008 | 0x00000200   # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
BREAKAWAY = 0x01000000               # CREATE_BREAKAWAY_FROM_JOB: the worker's job ends its processes with it


def _alive(pid):
    if os.name == "nt":
        import ctypes
        k = ctypes.WinDLL("kernel32")
        h = k.OpenProcess(0x00100000, False, pid)   # SYNCHRONIZE
        if not h:
            return False
        r = k.WaitForSingleObject(h, 0)
        k.CloseHandle(h)
        return r == 0x102   # WAIT_TIMEOUT: still running
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def _start_detached(cmd, cwd):
    """a process that outlives this one: detached, and (Windows) out of the worker's job when the job allows it"""
    if os.name != "nt":
        return subprocess.Popen(cmd, cwd=cwd, start_new_session=True)
    try:
        return subprocess.Popen(cmd, cwd=cwd, creationflags=DETACHED | BREAKAWAY)
    except OSError:   # a job that refuses the breakaway (not the worker's own): started inside it
        return subprocess.Popen(cmd, cwd=cwd, creationflags=DETACHED)


def _replace(src, dst, tries=50):
    """a rename that waits a moment for a file still held (the old launcher's, just after it exits)"""
    for i in range(tries):
        try:
            os.replace(src, dst)
            return
        except OSError:
            if i == tries - 1:
                raise
            time.sleep(0.2)


def _move_back(names, frm, to, log):
    """names moved from frm back into to (an undo: each one tried, whatever the others do)"""
    for n in reversed(names):
        try:
            if os.path.lexists(os.path.join(to, n)):
                layout.rmtree(os.path.join(to, n)) if os.path.isdir(os.path.join(to, n)) else \
                    os.remove(os.path.join(to, n))
            _replace(os.path.join(frm, n), os.path.join(to, n))
        except OSError as e:
            log(f"launcher update: {n} could not be put back ({e})")


def swap_launcher(launcher_dir, start, wait_pid=None, log=print, health_timeout=HEALTH_TIMEOUT_S):
    """run by a helper (stage_helper's copy, never the worker/ it replaces) after the launcher has exited: the
    package staged in <launcher_dir>/downloads/launcher-new replaces the launcher's package items -- the old ones
    (LAUNCHER_PACKAGE's, and any the new package also has) moved into downloads/launcher-previous, the new ones into
    the folder; nothing else in the folder (the install's items above all) is moved, renamed or removed. The new
    launcher is started with --after-update and has health_timeout seconds to write HEALTH_FILE in its folder: then
    the previous items are removed. If the swap fails, or the new one does not come up, its items go to
    downloads/launcher-failed, the previous ones are put back and started with --update-failed. start: the
    launcher's executable name inside its folder. Returns "updated" or "restored"."""
    d = os.path.abspath(launcher_dir)
    dirs = update_dirs(d)
    new, prev, failed = dirs["staged"], dirs["previous"], dirs["failed"]
    if os.path.basename(start) != start or _not_package(start):
        raise ValueError(f"not a launcher's name in its folder: {start}")
    deadline = time.monotonic() + 120
    while wait_pid and _alive(wait_pid) and time.monotonic() < deadline:
        time.sleep(0.2)

    def launch(flag):
        return _start_detached([os.path.join(d, start), flag], d)
    if not os.path.isfile(os.path.join(new, start)):
        log("launcher update: no new launcher is staged; the previous launcher kept")
        launch("--update-failed")
        return "restored"
    incoming = sorted(n for n in os.listdir(new) if not _not_package(n))
    outgoing = sorted({n for n in list(layout.launcher_package()) + incoming
                       if not _not_package(n) and os.path.lexists(os.path.join(d, n))})
    moved_out, moved_in = [], []
    try:
        if os.path.lexists(prev):
            layout.rmtree(prev)
        os.makedirs(prev)
        for n in outgoing:
            _replace(os.path.join(d, n), os.path.join(prev, n))
            moved_out.append(n)
        for n in incoming:
            _replace(os.path.join(new, n), os.path.join(d, n))
            moved_in.append(n)
    except OSError as e:
        log(f"launcher update: the swap failed ({e}); the previous launcher kept")
        _move_back(moved_in, d, new, log)
        _move_back(moved_out, prev, d, log)
        launch("--update-failed")
        return "restored"
    health = os.path.join(d, HEALTH_FILE)
    if os.path.exists(health):
        os.remove(health)
    p = launch("--after-update")
    t0 = time.monotonic()
    while time.monotonic() - t0 < health_timeout:
        if os.path.exists(health):
            log("launcher update: the new launcher is up")
            for gone in (prev, new):   # the previous items, and the staging folder the new ones left empty
                try:
                    layout.rmtree(gone)
                except OSError as e:
                    log(f"launcher update: {os.path.basename(gone)} stays ({e}); the next update removes it")
            return "updated"
        if p.poll() is not None and not os.path.exists(health):
            break
        time.sleep(0.25)
    log("launcher update: the new launcher did not come up; the previous one put back")
    if p.poll() is None:
        p.kill()
        p.wait()
    if os.path.lexists(failed):
        layout.rmtree(failed)
    os.makedirs(failed)
    _move_back(moved_in, d, failed, log)   # the new items kept aside for a report, out of the folder's top
    _move_back(moved_out, prev, d, log)
    launch("--update-failed")
    return "restored"
