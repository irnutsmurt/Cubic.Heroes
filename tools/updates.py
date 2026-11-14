#!/usr/bin/env python3
"""Updates from GitHub Releases (M9.3; docs/M9_LAUNCHER_PLAN.md section 12, docs/RELEASE_SIGNING.md).

    check(channel, installed, source)        the newest release of the channel (stable; prerelease: any), its
                                              manifest verified -- newer than the installed version or not
    download(manifest, platform, dest, ...)  its download for this platform, resumed where a previous attempt
                                              stopped, checked against the manifest (size, SHA-256)
    stage_launcher(zip, launcher_dir)        a downloaded launcher unpacked beside the running one
    swap_launcher(...)                       (a helper process, after the launcher exits) the new launcher in the
                                              old one's place, started; put back if it does not start

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
import release_trust as trust  # noqa: E402

REPO = "irnutsmurt/Cubic.Heroes"
GITHUB_API = f"https://api.github.com/repos/{REPO}/releases?per_page=30"
USER_AGENT = "CubicHeroes-Launcher"
MANIFEST, SIGNATURE = "release.json", "release.json.sig"
TIMEOUT_S = 20
CHUNK = 1 << 20


def platform_name():
    return "windows-x86_64" if os.name == "nt" else "linux-x86_64"


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

def stage_launcher(package, launcher_dir):
    """the launcher's package (a verified zip) unpacked beside launcher_dir, in <name>.new; every member's path
    inside it (no member may escape). Returns the staged folder."""
    parent = os.path.dirname(os.path.abspath(launcher_dir))
    new = os.path.abspath(launcher_dir) + ".new"
    if os.path.exists(new):
        shutil.rmtree(new)
    with zipfile.ZipFile(package) as z:
        for n in z.namelist():
            p = os.path.abspath(os.path.join(new, n))
            if not p.startswith(new + os.sep) and p != new:
                raise trust.ReleaseError(f"The launcher's package holds a path outside it ({n}).", "bad_download")
        z.extractall(new)
    entries = os.listdir(new)
    if len(entries) == 1 and os.path.isdir(os.path.join(new, entries[0])):   # one top folder: its contents
        top = os.path.join(new, entries[0])
        tmp = os.path.join(parent, os.path.basename(new) + ".tmp")
        os.replace(top, tmp)
        shutil.rmtree(new)
        os.replace(tmp, new)
    return new


HEALTH_FILE = ".started-ok"   # the new launcher writes it once it is up: the swap is then kept
HEALTH_TIMEOUT_S = 60


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


def swap_launcher(launcher_dir, start, wait_pid=None, log=print, health_timeout=HEALTH_TIMEOUT_S):
    """run by a helper after the launcher has exited: <launcher_dir>.new takes its place (the old kept as
    <launcher_dir>.old), the new launcher is started with --after-update and has health_timeout seconds to write
    HEALTH_FILE in its folder; if the swap fails, or the new one does not come up, the old one is put back and
    started with --update-failed. start: the launcher's executable name inside its folder. Returns "updated" or
    "restored"."""
    d = os.path.abspath(launcher_dir)
    new, old = d + ".new", d + ".old"
    deadline = time.monotonic() + 120
    while wait_pid and _alive(wait_pid) and time.monotonic() < deadline:
        time.sleep(0.2)
    kw = {"creationflags": 0x00000008 | 0x00000200} if os.name == "nt" else {"start_new_session": True}
    try:
        if os.path.exists(old):
            shutil.rmtree(old)
        for _ in range(50):   # the old launcher's files may stay locked a moment after it exits
            try:
                os.replace(d, old)
                break
            except OSError:
                time.sleep(0.2)
        else:
            raise OSError("the launcher's folder stayed in use")
        os.replace(new, d)
    except OSError as e:
        log(f"launcher update: the swap failed ({e}); the previous launcher kept")
        if not os.path.exists(d) and os.path.exists(old):
            os.replace(old, d)
        subprocess.Popen([os.path.join(d, start), "--update-failed"], cwd=d, **kw)
        return "restored"
    health = os.path.join(d, HEALTH_FILE)
    if os.path.exists(health):
        os.remove(health)
    p = subprocess.Popen([os.path.join(d, start), "--after-update"], cwd=d, **kw)
    t0 = time.monotonic()
    while time.monotonic() - t0 < health_timeout:
        if os.path.exists(health):
            log("launcher update: the new launcher is up")
            return "updated"
        if p.poll() is not None and not os.path.exists(health):
            break
        time.sleep(0.25)
    log("launcher update: the new launcher did not come up; the previous one put back")
    if p.poll() is None:
        p.kill()
        p.wait()
    failed = d + ".failed"
    if os.path.exists(failed):
        shutil.rmtree(failed)
    for _ in range(50):
        try:
            os.replace(d, failed)
            break
        except OSError:
            time.sleep(0.2)
    os.replace(old, d)
    subprocess.Popen([os.path.join(d, start), "--update-failed"], cwd=d, **kw)
    return "restored"
