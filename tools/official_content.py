#!/usr/bin/env python3
"""Official game content and the characters it brings (docs/CHARACTER_STORE.md, "Official content").

config/official_characters.json is an explicit, versioned mapping: for each official content package -- the game
itself, or an update such as the US v1.01 (BLUS30490-01.01) -- the characters it holds, by slot, and the Character
Store catalog entry each one is (or none). Which packages an installation has comes from the installation's own
record, never from reading or scanning the game's character files:

  <install>/data/install.json  "official_content": ["BLUS30490-01.00", "BLUS30490-01.01", ...]   (written by whatever
                               installs, removes or replaces an official package), or, when absent, derived from the
                               program the installation was built from ("elf_sha256" = config/game_version.json's
                               decrypted program: the game itself, its title id and version)

It is worked out each time it is asked (nothing cached), so an update installed, removed or replaced shows at the
store's next look. Japanese or European releases add their own packages to the same mapping.

    python tools/official_content.py check                  the mapping checked
    python tools/official_content.py installed INSTALL_DIR   the packages an installation has, and their characters
"""
import json
import os
import re
import sys

FORMAT = "cubic.heroes.official-characters"
VERSION = 1
ROOT = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAPPING = os.path.join(ROOT, "config", "official_characters.json")
GAME_VERSION = os.path.join(ROOT, "config", "game_version.json")
_ID = re.compile(r"^[A-Z]{4}[0-9]{5}-[0-9]{2}\.[0-9]{2}$")
_CID = re.compile(r"^[0-9a-f]{16}$")
_SLOT = re.compile(r"^[A-Za-z0-9_./-]{1,64}$")
KINDS = ("game", "update")
MATCHES = ("identical", "same-model", "none")


class MappingError(Exception):
    pass


def load(path=None):
    """{package id: {id, title_id, region, version, kind, label, requires, characters: [{slot, catalog_id, match}]}}
    -- the mapping, every field checked; MappingError when it is not one this version reads"""
    try:
        with open(path or MAPPING, encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, ValueError) as e:
        raise MappingError(f"the official characters mapping could not be read ({type(e).__name__})")
    if not isinstance(doc, dict) or doc.get("format") != FORMAT:
        raise MappingError("not an official characters mapping")
    if doc.get("version") != VERSION:
        raise MappingError(f"official characters mapping version {doc.get('version')!r} (this launcher reads {VERSION})")
    out = {}
    for p in doc.get("content") or []:
        pid = p.get("id") if isinstance(p, dict) else None
        if not isinstance(pid, str) or not _ID.match(pid) or pid in out:
            raise MappingError(f"a package with no valid id ({str(pid)[:30]!r})")
        if p.get("kind") not in KINDS:
            raise MappingError(f"{pid}: unknown kind {p.get('kind')!r}")
        label = p.get("label")
        if not isinstance(label, str) or not label.strip() or len(label) > 80:
            raise MappingError(f"{pid}: no label")
        req = p.get("requires")
        if req is not None and (not isinstance(req, str) or not _ID.match(req)):
            raise MappingError(f"{pid}: requires no valid package")
        chars, slots, cids = [], set(), set()
        for c in p.get("characters") or []:
            if not isinstance(c, dict):
                raise MappingError(f"{pid}: a character that is not an entry")
            slot, cid, match = c.get("slot"), c.get("catalog_id"), c.get("match")
            if not isinstance(slot, str) or not _SLOT.match(slot) or slot in slots:
                raise MappingError(f"{pid}: a character with no valid slot ({str(slot)[:30]!r})")
            if match not in MATCHES:
                raise MappingError(f"{pid} {slot}: unknown match {match!r}")
            if (cid is None) != (match == "none"):
                raise MappingError(f"{pid} {slot}: a catalog id goes with a match, and only with one")
            if cid is not None and (not isinstance(cid, str) or not _CID.match(cid) or cid in cids):
                raise MappingError(f"{pid} {slot}: catalog id {str(cid)[:20]!r} is not valid or is listed twice")
            slots.add(slot)
            if cid:
                cids.add(cid)
            chars.append({"slot": slot, "catalog_id": cid, "match": match,
                          "differs": [str(x)[:20] for x in (c.get("differs") or [])][:8]})
        out[pid] = {"id": pid, "title_id": str(p.get("title_id") or "")[:12], "region": str(p.get("region") or "")[:8],
                    "version": str(p.get("version") or "")[:8], "kind": p["kind"], "label": label.strip(),
                    "requires": req, "characters": chars}
    for pid, p in out.items():
        if p["requires"] and p["requires"] not in out:
            raise MappingError(f"{pid}: requires {p['requires']}, which the mapping doesn't list")
    return out


def _base_package(rec, game_version):
    """the game's own package, from the program the installation was built from: "<title id>-<app version>" when its
    recorded SHA-256 is the pinned decrypted program's"""
    try:
        with open(game_version or GAME_VERSION, encoding="utf-8") as f:
            gv = json.load(f)
    except (OSError, ValueError):
        return None
    sha = (gv.get("decrypted_elf") or {}).get("sha256")
    if sha and rec.get("elf_sha256") == sha and gv.get("title_id") and gv.get("app_ver"):
        return f"{gv['title_id']}-{gv['app_ver']}"
    return None


def installed(install, mapping=None, game_version=None):
    """the official content packages the installation at `install` has, [ids] in the mapping's order -- from its
    install.json (official_content, else its program); a package whose prerequisite is missing doesn't count"""
    mapping = load() if mapping is None else mapping
    rec = {}
    for p in (os.path.join(install, "data", "install.json"), os.path.join(install, "install.json")):
        try:
            with open(p, encoding="utf-8") as f:
                rec = json.load(f)
            break
        except (OSError, ValueError):
            continue
    if not isinstance(rec, dict):
        rec = {}
    listed = rec.get("official_content")
    if isinstance(listed, list):
        have = {x for x in listed if isinstance(x, str)}
    else:
        base = _base_package(rec, game_version)
        have = {base} if base else set()
    have = {x for x in have if x in mapping}
    return [pid for pid, p in mapping.items() if pid in have and (not p["requires"] or p["requires"] in have)]


def included(packages, mapping=None):
    """{catalog id: [{"id": package id, "label": its label}]} -- the catalog entries the given packages include"""
    mapping = load() if mapping is None else mapping
    out = {}
    for pid in packages:
        p = mapping.get(pid)
        for c in (p or {}).get("characters", []):
            if c["catalog_id"]:
                out.setdefault(c["catalog_id"], []).append({"id": pid, "label": p["label"]})
    return out


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if not argv or argv[0] not in ("check", "installed"):
        print(__doc__)
        return 2
    try:
        m = load()
    except MappingError as e:
        print(f"official characters mapping: {e}")
        return 1
    if argv[0] == "check":
        for pid, p in m.items():
            n = len(p["characters"])
            mapped = sum(1 for c in p["characters"] if c["catalog_id"])
            print(f"{pid} ({p['kind']}, {p['label']}): {n} characters, {mapped} in the store's catalog")
        return 0
    if len(argv) < 2:
        print("installed needs the installation folder")
        return 2
    pk = installed(argv[1], m)
    print(f"official content: {', '.join(pk) or 'none recognised'}")
    for cid, by in sorted(included(pk, m).items()):
        print(f"  {cid}: {by[0]['label']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
