#!/usr/bin/env python3
"""config/disc_manifest.json: every game file of BLUS-30490 v01.00 -- its path, size and SHA-256 -- what the install
checks each file of the player's disc against (tools/disc_source.py; 0.2.0-beta.5). A developer's tool, not shipped.

    disc_manifest.py make <the verified whole-disc ISO> [--out config/disc_manifest.json]
    disc_manifest.py check <ISO or disc folder> [--disc-key <.dkey | 32 digits>]

`make` only reads the independently verified dump: it refuses any image whose size and whole SHA-256 aren't
config/game_version.json's "disc_image" -- so the manifest can never be made from a player's or a contributor's files.
It lists every file under PS3_GAME/ (the files the install extracts; PS3_DISC.SFB and anything else on the disc aren't
installed), by the image's Joliet names, sorted, in a fixed form: the same dump always gives the same bytes. The
manifest holds names, sizes and hashes only: nothing of the files' contents, no key.

`check` runs the install's own checks on a source without writing anything: every manifest file found (names
matched in any case), of its size and SHA-256; the files not in the manifest are listed."""
import argparse
import hashlib
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import disc_source  # noqa: E402
import iso9660  # noqa: E402

GAME_VERSION = os.path.join(ROOT, "config", "game_version.json")


def _sha256(f, extents=None):
    h = hashlib.sha256()
    if extents is None:
        f.seek(0)
        for b in iter(lambda: f.read(8 << 20), b""):
            h.update(b)
    else:
        for b in iso9660.stream(f, extents):
            h.update(b)
    return h.hexdigest()


def render(m):
    """the manifest's bytes: one file per line, sorted by path, LF, ASCII"""
    head = {k: v for k, v in m.items() if k != "files"}
    lines = ["{"]
    for k in sorted(head):
        lines.append(f" {json.dumps(k)}: {json.dumps(head[k], sort_keys=True)},")
    lines.append(' "files": [')
    files = sorted(m["files"], key=lambda e: e["path"])
    for i, e in enumerate(files):
        lines.append("  " + json.dumps({"path": e["path"], "size": e["size"], "sha256": e["sha256"]})
                     + ("," if i < len(files) - 1 else ""))
    lines += [" ]", "}", ""]
    return "\n".join(lines).encode("ascii")


def make(iso, want=None):
    want = want or json.load(open(GAME_VERSION))
    ref = want["disc_image"]
    if os.path.getsize(iso) != ref["size"]:
        raise SystemExit(f"refused: {iso} is not the verified dump (size {os.path.getsize(iso)}, "
                         f"expected {ref['size']})")
    with open(iso, "rb") as f:
        if _sha256(f) != ref["sha256"]:
            raise SystemExit(f"refused: {iso} is not the verified dump (its SHA-256 isn't game_version.json's)")
        files = [{"path": p, "size": sum(n for _, n in ext), "sha256": _sha256(f, ext)}
                 for p, ext in iso9660.entries(f) if p.startswith(disc_source.GAME_DIR + "/")]
    m = {"format": 1, "title_id": want["title_id"], "app_ver": want["app_ver"], "made_from": dict(ref),
         "count": len(files), "total_size": sum(e["size"] for e in files), "files": files}
    m["made_from"].pop("note", None)
    return m


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    mk = sub.add_parser("make")
    mk.add_argument("iso")
    mk.add_argument("--out", default=disc_source.MANIFEST)
    ck = sub.add_parser("check")
    ck.add_argument("source")
    ck.add_argument("--disc-key")
    ck.add_argument("--manifest", default=disc_source.MANIFEST)
    a = ap.parse_args()
    if a.cmd == "make":
        m = make(a.iso)
        with open(a.out, "wb") as f:
            f.write(render(m))
        print(f"{a.out}: {m['count']} files, {m['total_size']} bytes")
        return
    m = disc_source.manifest(a.manifest)
    try:
        if os.path.isdir(a.source):
            n, extras = disc_source.check(a.source, "folder", m=m)
        else:
            import ps3_disc
            key = ps3_disc.load_key(a.disc_key) if a.disc_key else None
            with open(a.source, "rb") as raw:
                enc = ps3_disc.kind(raw) == "encrypted"
                if enc and not key:
                    raise SystemExit("refused: an encrypted image: give its --disc-key")
                f = ps3_disc.DiscImage(raw, key) if enc else raw
                n, extras = disc_source.check(a.source, "iso", f=f, m=m)
    except disc_source.Refused as e:
        raise SystemExit(f"refused ({e.code}): {e}" + (f"\n  {e.details}" if e.details else ""))
    print(f"OK: {n} files match the manifest" + (f"; not in it (skipped by the install): {', '.join(extras)}"
                                                    if extras else ""))


if __name__ == "__main__":
    main()
