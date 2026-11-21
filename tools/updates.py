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
import re
import shutil
import ssl
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


# the machine fields of a PE (IMAGE_FILE_HEADER.Machine) and an ELF (e_machine) header, as the manifest names them
PE_MACHINES = {0x8664: "x86_64", 0xAA64: "arm64", 0x14C: "x86"}
ELF_MACHINES = {62: "x86_64", 183: "arm64", 3: "x86"}


def executable_arch(path):
    """the processor a program was built for, from its own header (PE or ELF); None if it isn't one"""
    import struct
    try:
        with open(path, "rb") as f:
            head = f.read(4096)
    except OSError:
        return None
    if head[:4] == b"\x7fELF" and len(head) >= 20:
        return ELF_MACHINES.get(struct.unpack_from("<H" if head[5] == 1 else ">H", head, 18)[0], "unknown")
    if head[:2] == b"MZ" and len(head) >= 0x40:
        pe = struct.unpack_from("<I", head, 0x3C)[0]
        if pe + 6 <= len(head) and head[pe:pe + 4] == b"PE\0\0":
            return PE_MACHINES.get(struct.unpack_from("<H", head, pe + 4)[0], "unknown")
    return None


def platform_name(program=None):
    """this installation's system and processor, as the manifest names them: the processor the installed program was
    built for (the review's W-1) -- read from the running program's own header (the frozen worker, built with the
    package), not the machine's: an x86-64 package on a Windows-on-ARM PC runs emulated and is updated with the
    x86-64 package, never offered an ARM64 one that does not exist. Without a readable header (a development
    checkout's interpreter is the program then) the interpreter's own build decides (sysconfig, not the CPU)."""
    arch = executable_arch(program or sys.executable)
    if not arch:
        import sysconfig
        p = sysconfig.get_platform().lower()   # win-amd64, win-arm64, win32, linux-x86_64, linux-aarch64
        arch = "x86_64" if p.endswith(("amd64", "x86_64")) else "arm64" if p.endswith(("arm64", "aarch64")) \
            else "x86" if p.endswith(("win32", "i686")) else "unknown"
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


# Linux: the system's trusted certificate authorities (beta.4). The worker's OpenSSL is built in the pinned build
# environment (tools/linux_build_env), so its compiled-in defaults name that environment's folder (/opt/python/ssl),
# which no player's system has: with them every HTTPS request failed verification and read as offline (beta.3).
# The compiled-in defaults are therefore never used on Linux: SSL_CERT_FILE / SSL_CERT_DIR when the player set
# them (as OpenSSL itself honours them), otherwise the first of the distributions' own bundles that holds
# certificates, otherwise a hashed certificate folder. Verification and the host name check stay on whatever is
# found (ssl.create_default_context); nothing found is an error, never an unverified connection. Windows: urllib's
# default, unchanged (Python reads the Windows certificate store).
CA_FILES = (
    "/etc/ssl/certs/ca-certificates.crt",                  # Debian, Ubuntu, Arch, Manjaro, SteamOS, Gentoo, NixOS
    "/etc/pki/tls/certs/ca-bundle.crt",                    # Fedora, RHEL, CentOS
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",   # Fedora, RHEL 7+ (what the previous one links to)
    "/etc/ssl/ca-bundle.pem",                              # openSUSE
    "/etc/ssl/cert.pem",                                   # Alpine, Arch, Void
    "/etc/pki/tls/cacert.pem",                             # OpenELEC
)
CA_DIRS = ("/etc/ssl/certs", "/etc/pki/tls/certs")
_HASHED = re.compile(r"[0-9a-f]{8}\.\d+")   # OpenSSL's subject-hash names (c_rehash, openssl rehash)


def ca_store(environ=None, files=CA_FILES, dirs=CA_DIRS):
    """(context, where) for HTTPS on Linux: the certificate authorities the player's system trusts, verification and
    host name checks on. Raises ReleaseError "tls" when no store can be read: never a connection without one."""
    env = os.environ if environ is None else environ
    cafile, capath = env.get("SSL_CERT_FILE") or None, env.get("SSL_CERT_DIR") or None
    if cafile or capath:   # set by the player (or their distribution's environment): used as given, nothing else
        try:
            return ssl.create_default_context(cafile=cafile, capath=capath), "SSL_CERT_FILE/SSL_CERT_DIR"
        except (ssl.SSLError, OSError, ValueError) as e:
            raise trust.ReleaseError(f"The certificates SSL_CERT_FILE/SSL_CERT_DIR name could not be read ({e}).",
                                     "tls") from e
    for f in files:
        if not os.path.isfile(f):
            continue
        try:
            ctx = ssl.create_default_context(cafile=f)
        except (ssl.SSLError, OSError, ValueError):
            continue
        if ctx.cert_store_stats().get("x509_ca"):
            return ctx, f
    for d in dirs:
        try:
            if any(_HASHED.fullmatch(n) for n in os.listdir(d)):
                return ssl.create_default_context(capath=d), d
        except (ssl.SSLError, OSError, ValueError):
            continue
    raise trust.ReleaseError("No trusted certificate store was found on this system; set SSL_CERT_FILE to your "
                             "CA bundle.", "tls")


def tls_context():
    """None on Windows (urllib's default: the Windows certificate store); on Linux the system's store (ca_store)"""
    return None if os.name == "nt" else ca_store()[0]


def tls_store():
    """which store HTTPS is verified with, for the answer (the qualification records it)"""
    if os.name == "nt":
        return "the Windows certificate store"
    try:
        return ca_store()[1]
    except trust.ReleaseError:
        return None


def _tls_failure(e):
    """a URLError whose cause is the TLS handshake (a certificate that does not verify: untrusted, expired, another
    host's): told apart from no connection -- it is not offline, and must never read as offline (beta.3's Linux
    workers did)"""
    r = getattr(e, "reason", e)
    if isinstance(r, ssl.SSLCertVerificationError):
        return trust.ReleaseError("The update server's certificate could not be verified "
                                  f"({r.verify_message or r}); nothing was downloaded.", "tls")
    return None


def _open(url, headers=None, timeout=None):
    h = {"User-Agent": USER_AGENT, "Accept": "application/octet-stream"}
    h.update(headers or {})
    ctx = tls_context() if url.lower().startswith("https:") else None
    return urllib.request.urlopen(urllib.request.Request(url, headers=h), timeout=timeout or TIMEOUT_S, context=ctx)


def _get(url, limit=1 << 20):
    """a small document (the release list, a manifest, a signature)"""
    try:
        with _open(url, {"Accept": "application/vnd.github+json"} if "api.github.com" in url else None) as r:
            data = r.read(limit + 1)
    except urllib.error.HTTPError as e:
        raise trust.ReleaseError(f"The update server answered {e.code} for {url.rsplit('/', 1)[-1]}.", "server")
    except (urllib.error.URLError, OSError, TimeoutError) as e:
        raise (_tls_failure(e) or trust.ReleaseError("No connection to the update server (offline?).", "offline")) from e
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
    # what the check went by, for its answer (beta.4: a check that finds nothing says why -- the channel asked, how
    # many releases the server listed, this installation's platform and kind)
    seen = {"channel": channel, "releases": len(rels), "prereleases": sum(1 for r in rels if r["prerelease"]),
            "with_manifest": sum(1 for r in rels if MANIFEST in r["assets"]), "platform": platform_name(),
            "kind": installed_kind()}
    refused = []
    nothing_here = None   # the newest verified release with no package this installation can take (W-1)
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
        mine = [a for a in m["assets"] if a["platform"] == platform_name()]
        if not pick_asset(mine):
            # the review's W-1: a release with nothing for this system and this kind of installation (another
            # processor's, another package kind's) is never offered -- an older one that has it may be
            nothing_here = nothing_here or {"version": m["version"], "tag": r["tag"]}
            continue
        return {"available": True, "tag": r["tag"], "version": m["version"], "channel": m["channel"],
                "notes": m.get("notes", ""), "min_launcher": m.get("min_launcher"), "newer": newer,
                "assets": mine, "manifest": m,
                "manifest_url": r["assets"][MANIFEST], "urls": r["assets"], "refused": refused,
                "manifest_bytes": mbytes, "signature_bytes": sig,
                "tls_store": tls_store(), "seen": seen}
    out = {"available": False, "refused": refused, "tls_store": tls_store(), "seen": seen}
    if nothing_here:
        out.update(no_package=True, version=nothing_here["version"], tag=nothing_here["tag"],
                   platform=platform_name(), kind=installed_kind())
    return out


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
            raise (_tls_failure(e) or
                   trust.ReleaseError("No connection to the download server (offline?).", "offline")) from e
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


# ---------------- the launcher's own update (0.2.0-beta.4: installed on restart) ----------------
#
# Before the portable root, the whole launcher folder was renamed (<dir>.new in its place, the old kept as <dir>.old).
# That can't be done when the install lives in the folder (it would move the saves, the builds and the very
# download being applied), so the launcher's package items are swapped one by one; anything else the folder holds
# stays. Until 0.2.0-beta.4 the launcher downloaded and verified an update but never installed it (nothing started
# the swap). The whole flow now, every file inside the installation:
#   download-update    the package for this system and this kind of installation into downloads/<version>/, with
#                      the release's manifest and signature exactly as fetched (release.json, release.json.sig)
#   apply-update       (the player said yes) the download verified again from those files -- the signature with the
#                      launcher's trusted keys, a version newer than this one, this system's and this installation
#                      kind's package, its size and SHA-256 -- then staged: a zip or tarball unpacked into
#                      downloads/launcher-new and its programs checked (this system's binary format and processor,
#                      the version inside); an AppImage copied beside the running one. Nothing of the working
#                      installation changes. The helper (a copy of this worker) is started; the launcher exits.
#   swap-launcher      (the helper) once the launcher and anything else running from the folder have ended: the
#                      swap, journalled step by step (downloads/launcher-update.json), the launcher's program
#                      replaced last in one rename; the new launcher checked (`--version --json`) and its window
#                      started; anything failing puts the previous launcher back. The replaced launcher is kept in
#                      downloads/launcher-rollback: the launcher's own rollback.
#   rollback-launcher  the same swap from downloads/launcher-rollback
#   recover-launcher   an update a crash or a power cut interrupted: the previous launcher put back, or a swap found
#                      complete and healthy kept (the next start runs it)

STAGED, PREVIOUS, FAILED, HELPER, ROLLBACK = ("launcher-new", "launcher-previous", "launcher-failed",
                                              "launcher-helper", "launcher-rollback")
JOURNAL, LOCK, LAST = "launcher-update.json", "launcher-update.lock", "launcher-update-last.json"
VERSION_FILE = ".cubic-version"        # in launcher-rollback: the version kept there
APPIMAGE_KEPT = "previous.AppImage"    # an AppImage installation's kept launcher, in launcher-previous/-rollback


def update_dirs(launcher_dir):
    """where the launcher's update stages and keeps things, all inside its folder (downloads/; an AppImage
    installation's: its root's)"""
    d = os.path.join(os.path.abspath(launcher_dir), "downloads")
    return {"downloads": d, "staged": os.path.join(d, STAGED), "previous": os.path.join(d, PREVIOUS),
            "failed": os.path.join(d, FAILED), "helper": os.path.join(d, HELPER),
            "rollback": os.path.join(d, ROLLBACK), "journal": os.path.join(d, JOURNAL),
            "lock": os.path.join(d, LOCK), "last": os.path.join(d, LAST)}


def _not_package(name):
    """a name a launcher package may never put at the folder's top: the install's own items, the root's marker,
    the folder the update itself uses"""
    low = name.lower()
    return low in {n.lower() for n in layout.INSTALL_ITEMS} or low == layout.ROOT_MARKER.lower() or \
        low == HEALTH_FILE or name.startswith(".")


def _room(folder, size):
    """enough free space for a package unpacked (its size, and 64 MB to spare); ReleaseError disk_space"""
    free = shutil.disk_usage(folder).free
    if free < size + (64 << 20):
        raise trust.ReleaseError(f"Not enough free space to install the update ({(size >> 20) + 64} MB needed, "
                                 f"{free >> 20} MB free).", "disk_space")


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
    kind = trust.name_kind(package)
    if kind == "appimage":
        raise trust.ReleaseError("An AppImage is updated by downloading the new .AppImage file, not unpacked.",
                                 "bad_download")
    # the review's W-6: only this system's own kind of launcher package is staged, by its name (the kind the manifest
    # and release_trust.KINDS tie to a system) and by its bytes -- Windows took any name but .zip as a tarball
    system = "windows" if os.name == "nt" else "linux"
    want = next(k for k, s in trust.KINDS.items() if s == system and k != "appimage")
    if kind != want:
        raise trust.ReleaseError(
            f"{os.path.basename(package)} is not this system's launcher package (a .{want}: "
            + ("a zip keeps no executable bits" if kind == "zip" else "another system's or an unknown kind") + ").",
            "bad_download")
    try:
        with open(package, "rb") as f:
            head = f.read(6)
    except OSError as e:
        raise trust.ReleaseError(f"The launcher's package can't be read ({e}).", "bad_download")
    if not head.startswith({"zip": (b"PK\x03\x04", b"PK\x05\x06"), "tar.xz": (b"\xfd7zXZ\x00",)}[want]):
        raise trust.ReleaseError(f"{os.path.basename(package)} is not a {want} archive, whatever its name says.",
                                 "bad_download")
    os.makedirs(part)
    try:
        if low.endswith((".tar.xz", ".txz")):
            import tarfile
            with tarfile.open(package, "r:xz") as t:
                members = t.getmembers()
                _room(part, sum(m.size for m in members if m.isfile()))
                for m in members:
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
                _room(part, sum(i.file_size for i in z.infolist()))
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


HEALTH_FILE = ".started-ok"   # legacy name (0.2.0-beta.3's swap): now in downloads/, named by HEALTH_ENV
HEALTH_ENV = "CUBIC_UPDATE_HEALTH"   # the file the new launcher's window writes its version into once it is up
HEALTH_TIMEOUT_S = 60
CHECK_TIMEOUT_S = 180                # `<launcher> --version --json`: the launcher and its worker answering
IDLE_TIMEOUT_S = 120                 # the launcher, and anything else running from the folder, given time to end
DETACHED = 0x00000008 | 0x00000200   # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
BREAKAWAY = 0x01000000               # CREATE_BREAKAWAY_FROM_JOB: the worker's job ends its processes with it
NO_WINDOW = 0x08000000               # CREATE_NO_WINDOW: the checks' console programs show nothing


def _alive(pid):
    if not pid:
        return False
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
    except PermissionError:
        return True
    except OSError:
        return False


def _proc_info(pid):
    """(program path, start time in seconds since the epoch, a zombie) of a process; None if there is none"""
    if os.name == "nt":
        import ctypes
        import ctypes.wintypes as w
        k = ctypes.WinDLL("kernel32")
        k.OpenProcess.restype = w.HANDLE
        h = k.OpenProcess(0x1000 | 0x00100000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE
        if not h:
            return None
        try:
            if k.WaitForSingleObject(h, 0) != 0x102:   # not WAIT_TIMEOUT: it has ended
                return None
            buf = ctypes.create_unicode_buffer(32768)
            n = w.DWORD(len(buf))
            path = buf.value if k.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)) else ""
            ft = [w.FILETIME() for _ in range(4)]
            start = None
            if k.GetProcessTimes(h, *[ctypes.byref(f) for f in ft]):
                ticks = (ft[0].dwHighDateTime << 32) | ft[0].dwLowDateTime
                start = (ticks - 116444736000000000) / 1e7
            return path, start, False
        finally:
            k.CloseHandle(h)
    try:
        with open(f"/proc/{pid}/stat", encoding="utf-8", errors="replace") as f:
            rest = f.read().rsplit(")", 1)[1].split()
    except (OSError, IndexError):
        try:   # no /proc: whether it exists at all
            os.kill(pid, 0)
            return "", None, False
        except PermissionError:
            return "", None, False
        except OSError:
            return None
    start = None
    try:
        with open("/proc/stat", encoding="utf-8") as f:
            btime = next(int(ln.split()[1]) for ln in f if ln.startswith("btime "))
        start = btime + int(rest[19]) / os.sysconf("SC_CLK_TCK")   # field 22: the start, in ticks since boot
    except (OSError, StopIteration, ValueError, IndexError):
        pass
    try:
        path = os.readlink(f"/proc/{pid}/exe").replace(" (deleted)", "")
    except OSError:
        path = ""
    return path, start, rest[0] == "Z"


def is_process(pid, exe=None, since=None):
    """pid is still the process recorded with it: running (a zombie, killed but not yet reaped, is not), started no
    later than `since` (when it was recorded -- a pid given to another process after a reboot or a crash starts
    later), and running `exe` when that was recorded"""
    if not pid or pid == -1:
        return bool(pid)
    info = _proc_info(pid)
    if not info:
        return False
    path, start, zombie = info
    if zombie:
        return False
    if since is not None and start is not None and start > since + 2:
        return False
    if exe and path and os.path.normcase(os.path.realpath(path)) != os.path.normcase(os.path.realpath(exe)):
        return False
    return True


def _clean_env(env=None):
    """the environment a launcher is started in by the update: without the old AppImage's own variables (its
    mount is gone once it exits; a new AppImage's runtime sets its own)"""
    env = dict(os.environ if env is None else env)
    appdir = env.get("APPDIR") or ""
    for k in ("APPIMAGE", "APPDIR", "ARGV0", "OWD"):
        env.pop(k, None)
    for k in ("LD_LIBRARY_PATH", "PATH"):
        if appdir and k in env:
            env[k] = os.pathsep.join(p for p in env[k].split(os.pathsep) if not p.startswith(appdir))
    return env


def _start_detached(cmd, cwd, env=None):
    """a process that outlives this one: detached, and (Windows) out of the worker's job when the job allows it"""
    env = _clean_env(env)
    if os.name != "nt":
        return subprocess.Popen(cmd, cwd=cwd, env=env, start_new_session=True, stdin=subprocess.DEVNULL,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    kw = dict(cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        return subprocess.Popen(cmd, creationflags=DETACHED | BREAKAWAY, **kw)
    except OSError:   # a job that refuses the breakaway (not the worker's own): started inside it
        return subprocess.Popen(cmd, creationflags=DETACHED, **kw)


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


def _remove(path):
    if os.path.isdir(path) and not layout.is_link(path):
        layout.rmtree(path)
    elif os.path.lexists(path):
        os.remove(path)


def _write_json(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1)
        f.flush()
        os.fsync(f.fileno())
    _replace(tmp, path)


def _read_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def helper_running(j):
    """the journal's helper is still at work (not this process; the same process it recorded)"""
    pid = (j or {}).get("helper_pid")
    return bool(pid) and pid != os.getpid() and is_process(pid, j.get("helper_exe"), j.get("helper_since"))


def read_journal(launcher_dir):
    """the swap in progress, or left by one that never finished (a crash, a power cut); None"""
    return _read_json(update_dirs(launcher_dir)["journal"])


def _this_version():
    from version import VERSION
    return VERSION


# ---- one update at a time ----

class UpdateLock:
    """downloads/launcher-update.lock: one change of the launcher at a time (apply, swap, rollback, recovery). It
    names its process; a lock whose process is gone is taken over. apply-update hands it to the helper it starts."""

    def __init__(self, launcher_dir, op):
        self.path, self.op, self.held = update_dirs(launcher_dir)["lock"], op, False

    def owner(self):
        d = _read_json(self.path)
        if isinstance(d, dict):
            return d
        try:   # being written this moment, or damaged: someone's until it is old
            return -1 if time.time() - os.path.getmtime(self.path) < 10 else None
        except OSError:
            return None

    def acquire(self, wait=0.0):
        deadline = time.monotonic() + wait
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        while True:
            try:
                fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            except FileExistsError:
                d = self.owner()
                pid = d.get("pid") if isinstance(d, dict) else d
                if pid == os.getpid():   # handed over to this process
                    self.held = True
                    return self
                if pid != -1 and not (isinstance(d, dict) and is_process(pid, d.get("exe"), d.get("since"))):
                    # its process is gone (or the pid is another process's now)
                    try:
                        os.remove(self.path)
                    except OSError:
                        pass
                    continue
                if time.monotonic() >= deadline:
                    raise trust.ReleaseError("Another update of the launcher is in progress; try again when it has "
                                             "finished.", "busy")
                time.sleep(0.25)
                continue
            with os.fdopen(fd, "w") as f:
                json.dump({"pid": os.getpid(), "exe": sys.executable, "op": self.op, "since": time.time()}, f)
            self.held = True
            return self

    def hand_over(self, pid, exe=None):
        """the lock given to another process (the helper apply-update starts), released here"""
        _write_json(self.path, {"pid": pid, "exe": exe, "op": "swap", "since": time.time()})
        self.held = False

    def release(self):
        d = self.owner()
        if self.held and isinstance(d, dict) and d.get("pid") == os.getpid():
            try:
                os.remove(self.path)
            except OSError:
                pass
        self.held = False

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.release()


# ---- the checks before anything is staged ----

def binary_format(path):
    """("pe" or "elf", processor) of a program file; (None, None) when it is neither"""
    import struct
    try:
        with open(path, "rb") as f:
            head = f.read(4096)
    except OSError:
        return None, None
    if head[:4] == b"\x7fELF" and len(head) >= 20:
        return "elf", ELF_MACHINES.get(struct.unpack_from("<H" if head[5] == 1 else ">H", head, 18)[0], "unknown")
    if head[:2] == b"MZ" and len(head) >= 0x40:
        pe = struct.unpack_from("<I", head, 0x3C)[0]
        if pe + 6 <= len(head) and head[pe:pe + 4] == b"PE\0\0":
            return "pe", PE_MACHINES.get(struct.unpack_from("<H", head, pe + 4)[0], "unknown")
    return None, None


def check_program(path, what):
    """a program this system runs: Windows' format (PE) on Windows, ELF on Linux, this installation's processor --
    never another system's binaries, whatever their name says"""
    want = ("pe" if os.name == "nt" else "elf", platform_name().split("-", 1)[1])
    got = binary_format(path)
    if got != want:
        raise trust.ReleaseError(f"{what} is not a program for this system ({got[0] or 'not a program'}"
                                 f"{' ' + got[1] if got[1] else ''}; this system runs {want[0]} {want[1]}).",
                                 "platform")


def check_appimage(path):
    """an AppImage for this system: an ELF program of this processor carrying the AppImage type 2 mark"""
    check_program(path, os.path.basename(path))
    with open(path, "rb") as f:
        head = f.read(11)
    if head[8:11] != b"AI\x02":
        raise trust.ReleaseError(f"{os.path.basename(path)} is not an AppImage.", "platform")


def package_version(folder):
    """the version a launcher package says it is (its worker's tools/version.py); None if it says none"""
    try:
        with open(os.path.join(folder, "worker", "tools", "version.py"), encoding="utf-8") as f:
            m = re.search(r'^VERSION\s*=\s*["\']([^"\']+)["\']', f.read(), re.M)
    except OSError:
        return None
    return m.group(1) if m else None


def launcher_names():
    return ("CubicHeroes.exe" if os.name == "nt" else "CubicHeroes",
            "DotHeroes-Setup.exe" if os.name == "nt" else "DotHeroes-Setup")


def check_package(folder, version):
    """a staged launcher package: its launcher and its worker this system's programs, and the version it says the
    one the signed manifest says"""
    exe, worker = launcher_names()
    for path, what in ((os.path.join(folder, exe), "The new launcher"),
                       (os.path.join(folder, "worker", worker), "The new launcher's worker")):
        if not os.path.isfile(path):
            raise trust.ReleaseError(f"{what} is missing from the package.", "bad_download")
        check_program(path, what)
    got = package_version(folder)
    if got != version:
        raise trust.ReleaseError(f"The package holds version {got or 'unknown'}, not {version}.", "bad_download")


def launcher_check(cmd, version, timeout=CHECK_TIMEOUT_S, env=None):
    """(ok, why): the launcher (cmd: its program, or an AppImage) started with --version --json answers -- it starts,
    its worker starts -- and both are `version`. No window: the command line's own answer."""
    kw = dict(capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout,
              stdin=subprocess.DEVNULL, env=_clean_env(env))
    if os.name == "nt":
        kw["creationflags"] = NO_WINDOW
    try:
        r = subprocess.run(list(cmd) + ["--version", "--json"], **kw)
    except (OSError, subprocess.TimeoutExpired) as e:
        return False, f"it did not answer ({e})"
    line = next((ln for ln in reversed(r.stdout.splitlines()) if ln.startswith("{")), "")
    try:
        d = json.loads(line)
    except ValueError:
        return False, f"no answer (exit {r.returncode}): {(r.stderr or '').strip()[-300:]}"
    if not d.get("ok"):
        return False, f"it answered an error: {d.get('message') or d}"
    if d.get("cubicheroes") != version or d.get("worker") != version:
        return False, f"it is version {d.get('cubicheroes')} with worker {d.get('worker')}, not {version}"
    return True, ""


# ---- what is running from the installation ----

def _processes():
    """[(pid, program path)] of this system's processes"""
    out = []
    if os.name == "nt":
        import ctypes
        import ctypes.wintypes as w
        psapi, k32 = ctypes.WinDLL("psapi"), ctypes.WinDLL("kernel32")
        k32.OpenProcess.restype = w.HANDLE
        pids = (w.DWORD * 8192)()
        got = w.DWORD()
        if not psapi.EnumProcesses(pids, ctypes.sizeof(pids), ctypes.byref(got)):
            return out
        for pid in pids[:got.value // ctypes.sizeof(w.DWORD)]:
            h = k32.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
            if not h:
                continue
            buf = ctypes.create_unicode_buffer(32768)
            n = w.DWORD(len(buf))
            if k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)):
                out.append((int(pid), buf.value))
            k32.CloseHandle(h)
    elif os.path.isdir("/proc"):
        for d in os.listdir("/proc"):
            if d.isdigit():
                try:
                    out.append((int(d), os.readlink(f"/proc/{d}/exe")))
                except OSError:
                    continue
    return out


def running_from(folder, files=(), ignore=(), prefixes=()):
    """the programs running from folder (the launcher, its worker, the game: all inside a portable root) or that are
    one of `files` (an AppImage's runtime), but this process and anything in `ignore` (the helper's own folder)"""
    me = os.getpid()
    base = os.path.normcase(os.path.abspath(folder)) + os.sep
    exact = {os.path.normcase(os.path.abspath(f)) for f in files}
    more = [os.path.normcase(os.path.abspath(p)) + os.sep for p in prefixes if p]
    skip = [os.path.normcase(os.path.abspath(p)) + os.sep for p in ignore]
    found = []
    for pid, exe in _processes():
        p = os.path.normcase(exe.replace(" (deleted)", ""))
        if pid == me or any(p.startswith(s) for s in skip):
            continue
        if p.startswith(base) or p in exact or any(p.startswith(m) for m in more):
            found.append((pid, exe))
    return found


def wait_idle(folder, files=(), ignore=(), timeout=IDLE_TIMEOUT_S, log=print, prefixes=()):
    """[] once nothing runs from the installation any more (the launcher that just exited, a game still open), else
    what still does after `timeout` seconds"""
    deadline = time.monotonic() + timeout
    said = False
    while True:
        busy = running_from(folder, files, ignore, prefixes)
        if not busy or time.monotonic() >= deadline:
            return busy
        if not said:
            log("launcher update: waiting for " + ", ".join(os.path.basename(p) for _, p in busy) + " to end")
            said = True
        time.sleep(0.5)


# ---- applying a downloaded update (apply-update, before the launcher exits) ----

def downloaded(install, version):
    """the download of `version` kept by download-update: (folder, manifest bytes, signature bytes); ReleaseError
    no_download when it is not there"""
    d = os.path.join(os.path.abspath(install), "downloads", version)
    try:
        with open(os.path.join(d, MANIFEST), "rb") as f:
            mbytes = f.read(1 << 20)
        with open(os.path.join(d, SIGNATURE), "rb") as f:
            sig = f.read(1024)
    except OSError:
        raise trust.ReleaseError(f"Version {version} hasn't been downloaded (or its download is incomplete); "
                                 "download it again.", "no_download")
    return d, mbytes, sig


def verified_download(install, version, keys=None, installed=None, kind=None, allow_older=False, full=True):
    """the downloaded package of `version` verified again, from the files kept with it, before anything is staged:
    the manifest's signature (the launcher's trusted keys), its version, newer than this launcher's, this system's
    and this kind of installation's package, its size and SHA-256 (full=False: its size only -- the launcher's
    page; installing checks everything). Returns (manifest, asset, path)."""
    keys = trust.trusted_keys() if keys is None else keys
    if not keys:
        raise trust.ReleaseError("This launcher has no release key to check updates with.", "no_trusted_keys")
    d, mbytes, sig = downloaded(install, version)
    m = trust.verify_release(mbytes, sig, keys)
    if m["version"] != version:
        raise trust.ReleaseError(f"The downloaded manifest is version {m['version']}, not {version}.",
                                 "bad_download")
    installed = installed or _this_version()
    if not allow_older and (trust.parse_version(version) or ()) <= (trust.parse_version(installed) or ()):
        raise trust.ReleaseError(f"Version {version} is not newer than this launcher's ({installed}).", "not_newer")
    kind = kind or installed_kind()
    plat = platform_name()
    asset = pick_asset([a for a in m["assets"] if a["platform"] == plat], kind)
    if not asset or trust.name_kind(asset["name"]) != kind:
        raise trust.ReleaseError(f"Version {version} has no {kind} package for {plat}.", "platform")
    path = os.path.join(d, asset["name"])
    if not os.path.isfile(path):
        raise trust.ReleaseError(f"{asset['name']} hasn't been downloaded; download it again.", "no_download")
    if full:
        trust.verify_download(path, asset)
    elif os.path.getsize(path) != asset["size"]:
        raise trust.ReleaseError(f"{asset['name']} is not the size the release says.", "bad_download")
    return m, asset, path


def check_appimage_folder(appimage):
    """the AppImage's own folder can take the new file beside it (one rename on one file system): else refused --
    before anything is downloaded or staged, and never put anywhere else"""
    d = os.path.dirname(os.path.realpath(appimage))
    if not os.access(d, os.W_OK | os.X_OK):
        raise trust.ReleaseError(f"The AppImage's folder can't be written ({d}), so the update can't be put in place: "
                                 "download the new AppImage from the release page instead.", "root_not_writable")


def _fsync(path):
    """a file's (or, on Linux, a folder's) data on disk before a rename that relies on it (a power cut)"""
    try:
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) if os.path.isdir(path) else os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(fd)
    except OSError:
        pass
    finally:
        os.close(fd)


def stage_appimage(package, appimage, asset=None):
    """the new AppImage copied beside the running one (.<name>.new in its folder, so that putting it in place is one
    rename on one file system), executable, checked again; the running AppImage and its mount are never written to.
    Returns the staged file."""
    target = os.path.realpath(appimage)   # a symlinked AppImage: the real file is the one replaced
    check_appimage_folder(target)
    check_appimage(package)
    new = os.path.join(os.path.dirname(target), "." + os.path.basename(target) + ".new")
    need = os.path.getsize(package) + (64 << 20)
    free = shutil.disk_usage(os.path.dirname(target)).free
    if free < need:
        raise trust.ReleaseError(f"Not enough free space beside the AppImage ({need >> 20} MB needed, {free >> 20} MB "
                                 "free).", "disk_space")
    part = new + ".part"
    try:
        for p in (part, new):
            if os.path.lexists(p):
                os.remove(p)
        shutil.copyfile(package, part)
        os.chmod(part, 0o755)
        if asset:
            trust.verify_download(part, dict(asset, name=os.path.basename(part)))
        _fsync(part)
        os.replace(part, new)
        _fsync(os.path.dirname(new))
    except BaseException:
        if os.path.lexists(part):
            os.remove(part)
        raise
    return new


def prepare_update(install, launcher_dir, version, keys=None, appimage=None, installed=None):
    """apply-update's work before the launcher exits: the download verified again (verified_download), then staged
    -- a zip or tarball unpacked into downloads/launcher-new and its programs checked (check_package), an AppImage
    copied beside the running one (stage_appimage) -- and the swap's journal begun ("staged": nothing of the working
    installation is changed yet). Returns what the swap needs."""
    installed = installed or _this_version()
    kind = "appimage" if appimage else installed_kind()
    if kind == "appimage" and not appimage:
        raise trust.ReleaseError("An AppImage installation's update needs the AppImage's own path.", "usage")
    m, asset, path = verified_download(install, version, keys, installed, kind)
    if kind == "appimage":
        staged = stage_appimage(path, appimage, asset)
    else:
        staged = stage_launcher(path, launcher_dir)
        try:
            check_package(staged, version)
        except trust.ReleaseError:
            layout.rmtree(staged)
            raise
    j = {"state": "staged", "version": version, "from": installed, "kind": kind, "staged": staged,
         "appimage": os.path.realpath(appimage) if appimage else None, "since": time.time()}
    _write_json(update_dirs(launcher_dir)["journal"], j)
    return j


# ---- the swap (the helper, after the launcher has exited) ----

def _order(names, start, *folders):
    """folders first, then files, the launcher's own program last: until the very last step the folder's top keeps
    a launcher to start"""
    def is_dir(n):
        return any(os.path.isdir(os.path.join(f, n)) for f in folders)
    return sorted(set(names), key=lambda n: (n == start, not is_dir(n), n))


def _restore(d, prev, names, outgoing, aside, log):
    """the previous launcher back in d, whatever step the swap reached (from what is on disk, not from the journal
    alone: a crash between two renames leaves no record of the second). For each item: if the previous one was set
    aside in prev, whatever stands in its place is the new one -- moved aside (to `aside`), the previous one put
    back; an item the previous launcher didn't have (not in outgoing) is the new package's -- moved aside; any other
    item was never moved, and is left."""
    os.makedirs(aside, exist_ok=True)
    failed = []
    for n in reversed(names):
        here, kept, out = os.path.join(d, n), os.path.join(prev, n), os.path.join(aside, n)
        try:
            if os.path.lexists(kept):
                if os.path.lexists(here):
                    if os.path.lexists(out):
                        _remove(out)
                    _replace(here, out)
                _replace(kept, here)
            elif n not in outgoing and os.path.lexists(here) and not os.path.lexists(out):
                _replace(here, out)
        except OSError as e:
            failed.append(n)
            log(f"launcher update: {n} could not be put back ({e})")
    return failed


def _keep_previous(dirs, version):
    """the replaced launcher kept as the launcher's rollback (one version: the one before it is removed)"""
    rb, prev = dirs["rollback"], dirs["previous"]
    if os.path.lexists(rb):
        layout.rmtree(rb)
    _replace(prev, rb)
    with open(os.path.join(rb, VERSION_FILE), "w", encoding="utf-8") as f:
        f.write((version or "") + "\n")


def rollback_version(launcher_dir):
    """the version kept in downloads/launcher-rollback (the launcher before the last update), or None"""
    try:
        with open(os.path.join(update_dirs(launcher_dir)["rollback"], VERSION_FILE), encoding="utf-8") as f:
            return f.read().strip() or None
    except OSError:
        return None


def _health_wait(proc, path, version, timeout):
    """the new launcher's window up: it wrote its version into `path` within `timeout` seconds"""
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        try:
            with open(path, encoding="utf-8") as f:
                got = f.read().strip()
            if got:
                return got == version or not version, f"its window reported version {got}"
        except OSError:
            pass
        if proc is not None and proc.poll() is not None:
            return False, f"its window ended at once (exit {proc.returncode})"
        time.sleep(0.25)
    return False, f"its window didn't come up within {int(timeout)} s"


def _last(dirs, result, **kw):
    _write_json(dirs["last"], dict(kw, result=result, time=time.time()))


def swap_launcher(launcher_dir, start, wait_pid=None, log=print, health_timeout=HEALTH_TIMEOUT_S,
                  expect_version=None, source="staged", restart=True, check=None, idle_timeout=IDLE_TIMEOUT_S):
    """run by a helper (stage_helper's copy, never the worker/ it replaces) after the launcher has exited: the package
    in downloads/launcher-new (source "staged"; "rollback": the launcher kept by the last update) in place of the
    launcher's package items. Nothing else in the folder -- the install's items above all -- is moved, renamed or
    removed.

    Transactional: every step is recorded in downloads/launcher-update.json before the next; folders move first and
    the launcher's own program last, replaced in one rename (a copy of the old one kept), so the folder always has a
    launcher to start; a swap interrupted anywhere is undone by recover_launcher. Then the new launcher is checked --
    `--version --json`: it starts, its worker answers, both are expect_version -- and, with restart, its window is
    started (--after-update) and must say it is up within health_timeout. Anything that fails puts the previous
    launcher back (the new one kept aside in downloads/launcher-failed) and, with restart, starts it with
    --update-failed. A kept update keeps the replaced launcher in downloads/launcher-rollback (the launcher's own
    rollback; a rollback keeps the newer one there in turn). Returns "updated", "restored" or "busy"."""
    d = os.path.abspath(launcher_dir)
    dirs = update_dirs(d)
    if source not in ("staged", "rollback"):
        raise ValueError(f"not a source: {source}")
    if os.path.basename(start) != start or _not_package(start):
        raise ValueError(f"not a launcher's name in its folder: {start}")
    src, prev = dirs[source], dirs["previous"]
    check = check or launcher_check
    health = os.path.join(dirs["downloads"], "launcher-started-ok")

    def launch(flag, env=None):
        return _start_detached([os.path.join(d, start), flag], d, env) if restart else None

    with UpdateLock(d, "swap").acquire(wait=120):
        j = read_journal(d) or {}
        from_v = (j.get("from") if source == "staged" else None) or package_version(d) or _this_version()
        if source == "rollback":
            expect_version = expect_version or rollback_version(d)
        deadline = time.monotonic() + 120
        while wait_pid and _alive(wait_pid) and time.monotonic() < deadline:
            time.sleep(0.2)
        busy = wait_idle(d, ignore=(dirs["helper"],), timeout=idle_timeout, log=log)
        if busy:
            why = "still running: " + ", ".join(os.path.basename(p) for _, p in busy)
            log(f"launcher update: not applied, {why}")
            _last(dirs, "busy", version=expect_version, why=why)
            launch("--update-failed")
            return "busy"
        if not os.path.isfile(os.path.join(src, start)):
            log("launcher update: nothing to put in place; the launcher kept as it is")
            _last(dirs, "restored", version=expect_version, why="nothing staged")
            launch("--update-failed")
            return "restored"
        incoming = sorted(n for n in os.listdir(src) if not _not_package(n))
        outgoing = sorted({n for n in list(layout.launcher_package()) + incoming
                           if not _not_package(n) and os.path.lexists(os.path.join(d, n))})
        names = _order(outgoing + incoming, start, src, d)
        if os.path.lexists(prev):
            layout.rmtree(prev)
        os.makedirs(prev)
        j = {"state": "swapping", "kind": "folder", "source": source, "version": expect_version, "from": from_v,
             "start": start, "names": names, "outgoing": outgoing, "incoming": incoming, "done": [],
             "helper_pid": os.getpid(), "helper_exe": sys.executable, "helper_since": time.time(),
             "since": time.time()}
        _write_json(dirs["journal"], j)
        aside = dirs["failed"] if source == "staged" else src
        try:
            for n in names:
                here, there, kept = os.path.join(d, n), os.path.join(src, n), os.path.join(prev, n)
                if n == start:   # the program last, in one rename; its copy kept to put back
                    if os.path.lexists(here):
                        shutil.copy2(here, kept)
                    _replace(there, here)
                else:
                    if os.path.lexists(here):
                        _replace(here, kept)
                    if os.path.lexists(there):
                        _replace(there, here)
                j["done"].append(n)
                _write_json(dirs["journal"], j)
        except OSError as e:
            why = f"the swap failed at {n} ({e})"
            return _undo(d, dirs, j, why, aside, log, launch)
        j["state"] = "swapped"
        _write_json(dirs["journal"], j)
        ok, why = check([os.path.join(d, start)], expect_version) if expect_version else (True, "")
        if ok and restart:
            if os.path.lexists(health):
                os.remove(health)
            p = launch("--after-update", dict(os.environ, **{HEALTH_ENV: health}))
            ok, why = _health_wait(p, health, expect_version, health_timeout)
            _remove(health)
            if not ok and p is not None and p.poll() is None:
                p.kill()
                p.wait()
        if not ok:
            return _undo(d, dirs, j, "the new launcher " + why, aside, log, launch)
        _keep_previous(dirs, from_v)
        if source == "staged" and os.path.lexists(src):
            layout.rmtree(src)
        _remove(dirs["journal"])
        _last(dirs, "updated", version=expect_version, previous=from_v, source=source)
        log(f"launcher update: {from_v} -> {expect_version}, kept")
        return "updated"


def _undo(d, dirs, j, why, aside, log, launch):
    log(f"launcher update: {why}; the previous launcher put back")
    j["state"] = "restoring"
    _write_json(dirs["journal"], j)
    if aside == dirs["failed"] and os.path.lexists(aside):
        layout.rmtree(aside)
    failed = _restore(d, dirs["previous"], j["names"], j["outgoing"], aside, log)
    if not failed:
        _remove(dirs["journal"])
        if os.path.lexists(dirs["previous"]):
            layout.rmtree(dirs["previous"])
    _last(dirs, "restored", version=j.get("version"), previous=j.get("from"), why=why, unrestored=failed)
    launch("--update-failed")
    return "restored"


# ---- the AppImage: one file replaced ----

def appimage_extracted(appdir=None):
    """the running AppImage was started without FUSE (--appimage-extract-and-run, or APPIMAGE_EXTRACT_AND_RUN=1): its
    image extracted into $TMPDIR/appimage_extracted_<hash> instead of mounted"""
    appdir = os.environ.get("APPDIR", "") if appdir is None else appdir
    return "appimage_extracted_" in appdir or os.environ.get("APPIMAGE_EXTRACT_AND_RUN") == "1"


def swap_appimage(appimage, root, wait_pid=None, log=print, health_timeout=HEALTH_TIMEOUT_S, expect_version=None,
                  source="staged", restart=True, check=None, idle_timeout=IDLE_TIMEOUT_S, extracted=False,
                  old_appdir=None):
    """the AppImage installation's swap, by the helper (its copy in <root>/downloads/launcher-helper: never the
    image): after the launcher has exited, the new AppImage (staged as .<name>.new beside it; "rollback": the one
    kept in downloads/launcher-rollback) takes the AppImage's path in one rename -- its name, its shortcuts and its
    `Cubic.Heroes` folder stay -- the previous one kept (a hard link, or a copy) in downloads/launcher-previous.
    Checked as swap_launcher checks; anything that fails puts the previous file back. A kept update keeps the
    previous AppImage in downloads/launcher-rollback. Never writes into a mounted image. extracted: the old one ran
    without FUSE, so the new one is started the same way (APPIMAGE_EXTRACT_AND_RUN=1: it would fail otherwise, and the
    update be undone for a reason not the package's); the old one's extracted copy (old_appdir) is removed once the
    update is kept. Returns "updated", "restored" or "busy"."""
    target = os.path.realpath(appimage)
    run_env = dict(os.environ, APPIMAGE_EXTRACT_AND_RUN="1") if extracted else None
    dirs = update_dirs(root)
    check = check or launcher_check
    health = os.path.join(dirs["downloads"], "launcher-started-ok")
    staged = os.path.join(os.path.dirname(target), "." + os.path.basename(target) + ".new")
    new = staged if source == "staged" else os.path.join(dirs["rollback"], APPIMAGE_KEPT)
    prev = dirs["previous"]
    kept = os.path.join(prev, APPIMAGE_KEPT)

    def launch(flag, env=None):
        if not restart:
            return None
        env = dict(env or os.environ)
        if extracted:
            env["APPIMAGE_EXTRACT_AND_RUN"] = "1"
        return _start_detached([target, flag], os.path.dirname(target), env)

    with UpdateLock(root, "swap").acquire(wait=120):
        j = read_journal(root) or {}
        from_v = (j.get("from") if source == "staged" else None) or _this_version()
        if source == "rollback":
            expect_version = expect_version or rollback_version(root)
        deadline = time.monotonic() + 120
        while wait_pid and _alive(wait_pid) and time.monotonic() < deadline:
            time.sleep(0.2)
        busy = wait_idle(root, files=(target,), ignore=(dirs["helper"],), timeout=idle_timeout, log=log,
                         prefixes=(old_appdir,) if old_appdir else ())
        if busy:
            why = "still running: " + ", ".join(os.path.basename(p) for _, p in busy)
            _last(dirs, "busy", version=expect_version, why=why)
            launch("--update-failed")
            return "busy"
        if not os.path.isfile(new):
            _last(dirs, "restored", version=expect_version, why="nothing staged")
            launch("--update-failed")
            return "restored"
        if os.path.lexists(prev):
            layout.rmtree(prev)
        os.makedirs(prev)
        j = {"state": "swapping", "kind": "appimage", "source": source, "version": expect_version, "from": from_v,
             "appimage": target, "new": new, "extracted": bool(extracted), "helper_pid": os.getpid(),
             "helper_exe": sys.executable, "helper_since": time.time(),
             "since": time.time()}
        _write_json(dirs["journal"], j)
        try:
            try:
                os.link(target, kept)   # the same file kept under a second name: nothing copied
            except OSError:
                shutil.copy2(target, kept)
                _fsync(kept)
            _replace(new, target)
            _fsync(os.path.dirname(target))
        except OSError as e:
            return _undo_appimage(dirs, j, f"the swap failed ({e})", log, launch)
        j["state"] = "swapped"
        _write_json(dirs["journal"], j)
        ok, why = check([target], expect_version, env=run_env) if expect_version else (True, "")
        if ok and restart:
            if os.path.lexists(health):
                os.remove(health)
            p = launch("--after-update", dict(os.environ, **{HEALTH_ENV: health}))
            ok, why = _health_wait(p, health, expect_version, health_timeout)
            _remove(health)
            if not ok and p is not None and p.poll() is None:
                p.kill()
                p.wait()
        if not ok:
            return _undo_appimage(dirs, j, "the new launcher " + why, log, launch)
        _keep_previous(dirs, from_v)
        _remove(dirs["journal"])
        _last(dirs, "updated", version=expect_version, previous=from_v, source=source)
        if extracted and old_appdir and os.path.basename(os.path.normpath(old_appdir)).startswith("appimage_extracted_") \
                and not running_from(old_appdir):   # the old version's extracted copy (~450 MB): no longer used
            try:
                layout.rmtree(old_appdir)
            except OSError as e:
                log(f"launcher update: the old extracted copy stays ({e})")
        return "updated"


def _undo_appimage(dirs, j, why, log, launch):
    log(f"launcher update: {why}; the previous AppImage put back")
    target, kept = j["appimage"], os.path.join(dirs["previous"], APPIMAGE_KEPT)
    failed = []
    try:
        if os.path.lexists(kept):
            if j["source"] == "rollback" and not os.path.lexists(j["new"]) and os.path.lexists(target):
                os.makedirs(os.path.dirname(j["new"]), exist_ok=True)   # the rolled-back-to file stays available
                try:
                    os.link(target, j["new"])
                except OSError:
                    shutil.copy2(target, j["new"])
            _replace(kept, target)
    except OSError as e:
        failed.append(os.path.basename(target))
        log(f"launcher update: the AppImage could not be put back ({e})")
    if not failed:
        _remove(dirs["journal"])
        if os.path.lexists(dirs["previous"]):
            layout.rmtree(dirs["previous"])
    _last(dirs, "restored", version=j.get("version"), previous=j.get("from"), why=why, unrestored=failed)
    launch("--update-failed")
    return "restored"


# ---- after a crash: recovery ----

def recover_launcher(launcher_dir, start=None, log=print, wait_pid=None, restart=False, check=None,
                     idle_timeout=IDLE_TIMEOUT_S, prefixes=()):
    """a launcher update left unfinished (its journal, its helper gone: a crash, a power cut, the helper ended): a
    swap found complete and healthy (the new launcher answering as the version expected) is kept; anything else is
    undone -- the previous launcher put back from what is on disk. A journal that never got past "staged" changed
    nothing and is dropped. Returns "nothing", "in_progress", "kept" or "restored"."""
    d = os.path.abspath(launcher_dir)
    dirs = update_dirs(d)
    j = read_journal(d)
    if not j:
        return "nothing"
    if helper_running(j):
        return "in_progress"
    check = check or launcher_check

    def launch(flag):
        if not restart:
            return None
        if j.get("kind") == "appimage":
            env = dict(os.environ, APPIMAGE_EXTRACT_AND_RUN="1") if j.get("extracted") else None
            return _start_detached([j["appimage"], flag], os.path.dirname(j["appimage"]), env)
        return _start_detached([os.path.join(d, start or j.get("start") or launcher_names()[0]), flag], d)

    with UpdateLock(d, "recover").acquire(wait=30):
        deadline = time.monotonic() + 60
        while wait_pid and _alive(wait_pid) and time.monotonic() < deadline:
            time.sleep(0.2)
        state = j.get("state")
        if state == "staged":   # the launcher never got to the swap: nothing changed
            _remove(dirs["journal"])
            launch("--update-failed")
            return "nothing"
        files = (j["appimage"],) if j.get("kind") == "appimage" else ()
        busy = wait_idle(d, files=files, ignore=(dirs["helper"],), timeout=idle_timeout, log=log, prefixes=prefixes)
        if busy:   # it can't be put right while something runs from the installation: said, never silent
            why = "an interrupted update couldn't be put right while something was running from the installation (" + \
                ", ".join(os.path.basename(p) for _, p in busy) + "); close it and start the launcher again"
            log("launcher update: " + why)
            _last(dirs, "busy", version=j.get("version"), why=why)
            launch("--update-failed")
            return "busy"
        if state == "swapped" and j.get("version"):
            cmd = [j["appimage"]] if j.get("kind") == "appimage" else [os.path.join(d, j["start"])]
            env = dict(os.environ, APPIMAGE_EXTRACT_AND_RUN="1") if j.get("extracted") else None
            ok, why = check(cmd, j["version"], env=env)
            if ok:
                _keep_previous(dirs, j.get("from"))
                if j.get("source") == "staged" and j.get("kind") != "appimage" and os.path.lexists(dirs["staged"]):
                    layout.rmtree(dirs["staged"])
                _remove(dirs["journal"])
                _last(dirs, "updated", version=j["version"], previous=j.get("from"), recovered=True)
                launch("--after-update")
                return "kept"
        why = "the update was interrupted"
        if j.get("kind") == "appimage":
            _undo_appimage(dirs, j, why, log, lambda flag: None)
        else:
            src = dirs[j.get("source", "staged")]
            aside = dirs["failed"] if j.get("source", "staged") == "staged" else src
            _undo(d, dirs, j, why, aside, log, lambda flag: None)
        launch("--update-failed")
        return "restored"


# ---- what the launcher shows ----

def launcher_state(install, launcher_dir, appimage=None, installed=None, keys=None):
    """for the launcher's Updates page: this version; a downloaded newer version ready to install (verified again);
    the launcher kept for rollback; the last update's result; an unfinished update (interrupted: recovery needed)"""
    installed = installed or _this_version()
    dirs = update_dirs(launcher_dir)
    out = {"version": installed, "kind": "appimage" if appimage else installed_kind()}
    j = read_journal(launcher_dir)
    if j:
        alive = helper_running(j)
        out["journal"] = {"state": j.get("state"), "version": j.get("version"), "helper_running": alive}
        out["interrupted"] = not alive and j.get("state") in ("swapping", "swapped", "restoring")
    rb = rollback_version(launcher_dir)
    rb_item = os.path.join(dirs["rollback"], APPIMAGE_KEPT if appimage else launcher_names()[0])
    if rb and os.path.isfile(rb_item):
        out["rollback"] = {"version": rb}
    last = _read_json(dirs["last"])
    if last:
        out["last"] = last
    best = None
    base = os.path.join(os.path.abspath(install), "downloads")
    for v in (os.listdir(base) if os.path.isdir(base) else []):
        pv = trust.parse_version(v)
        if not pv or pv <= (trust.parse_version(installed) or ()) or (best and pv <= trust.parse_version(best)):
            continue
        try:
            verified_download(install, v, keys, installed, out["kind"], full=False)
        except trust.ReleaseError:
            continue
        best = v
    if best:
        out["ready"] = {"version": best}
    return out
