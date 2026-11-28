#!/usr/bin/env python3
"""The game programs' keys, downloaded when the player's computer has none (beta.6; the owner's decision of
2026-11-25: automatic, pinned and verified -- RPCS3 is never required).

config/key_downloads.json lists the public sources in order, each pinned to one commit and each of its files to its
size and SHA-256. A download is the launcher's own (tools/updates.py: resumed, checked before it is given its name,
removed when it fails the check) into the installation's data/keys/<source id>/ -- private to the installation:
never in a package, a log, a diagnostics export or a save backup. The files are data read in memory by
tools/player_keys.py, never run; whatever they open is accepted only as the exact pinned program
(tools/key_sources.py). Nothing here reads, prints or returns a key.

What a program needs ("kind"): app -- the disc's v1.00 program (an APP key); npdrm -- the official update's program
(an NPDRM key and the free licence's pair); pkg -- the official update's package (the package key).

    key_download.py list                   the sources and what each opens
    key_download.py fetch INSTALL KIND     the keys for KIND into INSTALL/data/keys (the first source that has them)
"""
import hashlib
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ROOT = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CONFIG = os.path.join(ROOT, "config", "key_downloads.json")
KINDS = ("app", "npdrm", "pkg")
_ID = re.compile(r"[a-z0-9][a-z0-9-]{0,63}\Z")
_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}\Z")


class KeyDownloadError(Exception):
    """code: offline, server, not_official (a file that isn't the pinned one), disk_space, cancelled, interrupted,
    tls, io, no_source (no source has that kind), config"""

    def __init__(self, message, code, details=""):
        super().__init__(message)
        self.code, self.details = code, details


def load(path=None):
    """the sources, checked: [{id, name, home, licence, commit, url, opens, files: [{path, size, sha256}]}]"""
    try:
        with open(path or CONFIG, encoding="utf-8") as f:
            cfg = json.load(f)
    except (OSError, ValueError) as e:
        raise KeyDownloadError("The list of key sources can't be read.", "config", repr(e)) from None
    out = []
    for s in cfg.get("sources", []) if cfg.get("format") == "cubic.heroes.key-downloads" else [None]:
        ok = (isinstance(s, dict) and _ID.match(str(s.get("id", ""))) and re.fullmatch(r"[0-9a-f]{40}", str(s.get(
            "commit", ""))) and str(s.get("url", "")).startswith("https://") and set(s.get("opens") or ()) <= set(KINDS)
              and s.get("files"))
        for f in (s or {}).get("files") or []:
            ok = ok and isinstance(f, dict) and all(_NAME.match(part) and part not in (".", "..")
                                                    for part in str(f.get("path", "")).split("/")) and \
                isinstance(f.get("size"), int) and 0 < f["size"] <= (1 << 20) and \
                re.fullmatch(r"[0-9a-f]{64}", str(f.get("sha256", "")))
        if not ok:
            raise KeyDownloadError("The list of key sources is damaged.", "config", json.dumps(s)[:300])
        out.append(s)
    return out


def folder(install):
    return os.path.join(os.path.abspath(install), "data", "keys")


def source_dir(install, src):
    return os.path.join(folder(install), src["id"])


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def complete(install, src):
    """whether every file of the source is in the installation, exactly as pinned"""
    d = source_dir(install, src)
    for f in src["files"]:
        p = os.path.join(d, os.path.basename(f["path"]))
        try:
            if os.path.getsize(p) != f["size"] or _sha256(p) != f["sha256"]:
                return False
        except OSError:
            return False
    return True


def _for(kind, sources):
    return [s for s in sources if kind is None or kind in s["opens"]]


def downloaded(install, kind=None, sources=None):
    """[(folder, how)] -- the sources this installation already downloaded whole, that open `kind`"""
    sources = load() if sources is None else sources
    return [(source_dir(install, s), f"downloaded before ({s['name']})") for s in _for(kind, sources)
            if complete(install, s)]


def fetch(install, kind, sources=None, progress=None, cancelled=None, base=None):
    """the first source that opens `kind` and isn't here yet, downloaded whole into data/keys/<id>/ (each file
    checked against its pin; one that isn't is removed). A source that can't be downloaded: the next one. Returns
    [(folder, how)] of the source downloaded -- or of one already here. Raises KeyDownloadError when none could be.
    base: a folder standing in for the sources' server (tests): <base>/<id>/<path>."""
    import release_trust as trust
    import updates
    sources = load() if sources is None else sources
    cands = _for(kind, sources)
    if not cands:
        raise KeyDownloadError(f"No source of keys for the {kind} program is known.", "no_source")
    for s in cands:
        if complete(install, s):
            return [(source_dir(install, s), f"downloaded before ({s['name']})")]
    last = None
    total = sum(f["size"] for f in cands[0]["files"]) or 1
    for s in cands:
        d = source_dir(install, s)
        existed = os.path.isdir(d)
        done = 0
        try:
            for f in s["files"]:
                name = os.path.basename(f["path"])
                url = (os.path.join(base, s["id"], *f["path"].split("/")) if base else
                       s["url"].format(commit=s["commit"], path=f["path"]))
                asset = {"name": name, "size": f["size"], "sha256": f["sha256"]}
                updates.download(asset, url, d, (lambda h, t, done=done: progress(done + h, total)) if progress
                                 else None, cancelled)
                done += f["size"]
            if complete(install, s):
                return [(d, f"downloaded ({s['name']})")]
            last = KeyDownloadError("The downloaded keys aren't the expected files.", "not_official", s["id"])
        except trust.ReleaseError as e:
            code = "not_official" if e.code == "bad_download" else e.code
            last = KeyDownloadError({"offline": "No connection to the internet to download the keys (offline?).",
                                     "not_official": "A downloaded key file isn't the expected one (it was removed).",
                                     "cancelled": "Cancelled."}.get(code, f"The keys couldn't be downloaded: {e}"),
                                    code, s["id"])
            if code == "cancelled":
                raise last from None
        except OSError as e:
            last = KeyDownloadError(f"The keys couldn't be saved ({e.strerror or e}).", "io", s["id"])
        if not existed:   # nothing left behind by a download that didn't finish (a .part is kept to resume)
            try:
                if os.path.isdir(d) and not os.listdir(d):
                    os.rmdir(d)
                if os.path.isdir(folder(install)) and not os.listdir(folder(install)):
                    os.rmdir(folder(install))
            except OSError:
                pass
    raise last


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    try:
        if argv[:1] == ["list"]:
            for s in load():
                print(f"{s['id']}: {s['name']} @ {s['commit'][:12]} ({s['licence']}); opens {', '.join(s['opens'])}; "
                      f"{len(s['files'])} file(s)")
            return 0
        if len(argv) == 3 and argv[0] == "fetch" and argv[2] in KINDS:
            for d, how in fetch(argv[1], argv[2]):
                print(f"{d}: {how}")
            return 0
    except KeyDownloadError as e:
        print(f"key_download: {e}", file=sys.stderr)
        return 2
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
