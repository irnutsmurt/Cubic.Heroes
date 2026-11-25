#!/usr/bin/env python3
"""The game's official v1.01 update (optional): the package exactly as Sony serves it, checked before it is opened.

config/game_version.json's "update" records the official package: where Sony serves it, its size and its SHA-256.
Sony serves it over plain HTTP, so nothing about it is trusted but an exact match with that record: a download, or a
package the player chooses on their computer, that isn't byte for byte the recorded package is refused before any of
it is read. The record was made from the download and checked against two independent records: the package's own
SHA-1 over its whole file, and the Internet Archive's captures of the same address on Sony's server, from 2013 to 2024,
whose SHA-1 is the same (6a85376e...603c).

The package is Sony's: it is never part of this repository or of anything this project distributes. Opening it needs
the player's own keys (tools/player_keys.py, tools/pkg_extract.py).

    game_update.py download DIR       the official package into DIR (resumed if a previous attempt stopped)
    game_update.py check PKG          whether PKG is the official package
    game_update.py check-folder DIR   whether DIR holds the official update's files (an installed or extracted update)
    game_update.py manifest PKG --keys KEYS [--out FILE]
                                      config/update_manifest.json from the official package only: each file's path,
                                      size and SHA-256 -- nothing of the files themselves
"""
import argparse
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release_trust as trust  # noqa: E402
import updates  # noqa: E402

CONFIG = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "config", "game_version.json")
MANIFEST = os.path.join(os.path.dirname(CONFIG), "update_manifest.json")


class UpdateError(Exception):
    """code: not_official, offline, server, interrupted, disk_space, cancelled, tls, io"""

    def __init__(self, message, code):
        super().__init__(message)
        self.code = code


def record(path=CONFIG):
    """the official update's record: {app_ver, content_id, file, url, size, sha256, sha1}"""
    with open(path, encoding="utf-8") as f:
        return json.load(f)["update"]


def check_package(path, rec=None, progress=None):
    """whether the file at path is the official package, byte for byte: its size, then its SHA-256. Raises
    UpdateError(not_official) otherwise -- before anything of it is read as a package."""
    rec = rec or record()
    try:
        size = os.path.getsize(path)
    except OSError as e:
        raise UpdateError(f"Can't read that file ({e.strerror}).", "io") from None
    if size != rec["size"]:
        raise UpdateError(f"That isn't the official v{rec['app_ver']} update: it is {size:,} bytes, the update is "
                          f"{rec['size']:,}.", "not_official")
    h, done = hashlib.sha256(), 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8 << 20), b""):
            h.update(chunk)
            done += len(chunk)
            if progress:
                progress(done, size)
    if h.hexdigest() != rec["sha256"]:
        raise UpdateError(f"That isn't the official v{rec['app_ver']} update: its contents differ (SHA-256). It may "
                          "be damaged or changed; download it again.", "not_official")
    return True


def download(dest_dir, rec=None, url=None, progress=None, cancelled=None):
    """the official package into dest_dir, from Sony's server (or url, for tests), with the launcher's own
    downloader: resumed from its .part when a previous attempt stopped, checked against the record before it is
    given its name; a file that fails the check is removed. Returns the path."""
    rec = rec or record()
    asset = {"name": rec["file"], "size": rec["size"], "sha256": rec["sha256"]}
    try:
        return updates.download(asset, url or rec["url"], dest_dir, progress, cancelled)
    except trust.ReleaseError as e:
        if e.code == "bad_download":
            raise UpdateError(f"The download isn't the official v{rec['app_ver']} update (its size or SHA-256 "
                              "differs); it was removed. Try again later.", "not_official") from None
        msg = {"offline": "No connection to Sony's download server (offline?).",
               "server": str(e).replace("download server", "Sony's download server")}.get(e.code, str(e))
        raise UpdateError(msg, e.code) from None


def make_manifest(pkg_path, keys_path, rec=None):
    """the update's file list, made only from the official package (checked first): every file's path, size and
    SHA-256, read in memory (nothing is written)"""
    import pkg_extract
    import player_keys
    rec = rec or record()
    check_package(pkg_path, rec)
    key = player_keys.load(keys_path).pkg
    p = pkg_extract.Package(pkg_path)
    files = []
    for it in p.items(key):
        if it.is_dir:
            continue
        h = hashlib.sha256()
        p.read(key, it, h.update)
        files.append({"path": it.name, "size": it.size, "sha256": h.hexdigest()})
    files.sort(key=lambda f: f["path"])
    return {"format": 1, "title_id": rec["content_id"].split("-")[1].split("_")[0], "app_ver": rec["app_ver"],
            "made_from": {"sha256": rec["sha256"], "size": rec["size"]}, "count": len(files),
            "total_size": sum(f["size"] for f in files), "files": files}


def load_manifest(path=MANIFEST):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def check_folder(folder, manifest=None, progress=None):
    """whether folder holds the official update's files -- the game's update folder from a PS3 or an emulator, or an
    extracted package: each file of the manifest present with its size and SHA-256 (other files are left alone).
    Raises UpdateError(not_official) naming the first file missing or different."""
    m = manifest or load_manifest()
    total, done = m["total_size"] or 1, 0
    for f in m["files"]:
        p = os.path.join(folder, *f["path"].split("/"))
        if not os.path.isfile(p):
            raise UpdateError(f"That folder isn't the complete v{m['app_ver']} update: {f['path']} is missing.",
                              "not_official")
        if os.path.getsize(p) != f["size"]:
            raise UpdateError(f"That folder isn't the official v{m['app_ver']} update: {f['path']} differs.",
                              "not_official")
        h = hashlib.sha256()
        with open(p, "rb") as r:
            for chunk in iter(lambda: r.read(8 << 20), b""):
                h.update(chunk)
                done += len(chunk)
                if progress:
                    progress(done, total)
        if h.hexdigest() != f["sha256"]:
            raise UpdateError(f"That folder isn't the official v{m['app_ver']} update: {f['path']} differs.",
                              "not_official")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("download").add_argument("dir")
    sub.add_parser("check").add_argument("pkg")
    sub.add_parser("check-folder").add_argument("dir")
    mk = sub.add_parser("manifest")
    mk.add_argument("pkg")
    mk.add_argument("--keys", required=True)
    mk.add_argument("--out", default=MANIFEST)
    a = ap.parse_args()
    try:
        if a.cmd == "download":
            print(download(a.dir))
        elif a.cmd == "check-folder":
            check_folder(a.dir)
            print("the official update's files")
        elif a.cmd == "manifest":
            m = make_manifest(a.pkg, a.keys)
            head = "".join(f' "{k}": {json.dumps(m[k])},\n' for k in m if k != "files")
            body = ",\n".join("  " + json.dumps(f) for f in m["files"])
            with open(a.out, "w", encoding="utf-8", newline="\n") as f:
                f.write("{\n" + head + ' "files": [\n' + body + "\n ]\n}\n")
            print(f"{a.out}: {m['count']} files, {m['total_size']:,} bytes")
        else:
            check_package(a.pkg)
            print("the official package")
    except UpdateError as e:
        print(f"game_update: {e}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
