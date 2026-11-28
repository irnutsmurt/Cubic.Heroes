"""M12, the Character Store's repository side (docs/CHARACTER_STORE.md): builds, signs and checks a copy of the
community characters repository -- the catalog, the character files and their thumbnails -- from approved
submissions. It writes only into the copy and never touches the network: publishing a copy (a commit to the public
repository) is a separate step the owner reviews.

    char_catalog.py build --repo DIR [--updated YYYY-MM-DD] SUBMISSION.json [...]
        adds each submitted character (or a new version of a listed one) to the copy: characters/<id>/<id>.chchar
        (the library's export: the character and its description only -- no PS3 save files, no account data),
        characters/<id>/thumb.png (the previews' renderer) and its catalog entry; the catalog's version goes up by
        one and its signatures are dropped (sign it again)
    char_catalog.py historical --repo DIR [--updated YYYY-MM-DD] RECORDS.json
        adds recovered community characters (category "historical"), each with its original name, its creator as
        recorded, its collection and its archival source -- and no licence: none is claimed, the creator keeps every
        right. RECORDS.json: [{"character": FILE, "original_name", "creator", "collection", "original_date",
        "original_url", "archived_url", "tags"?}]
    char_catalog.py remove --repo DIR --id ID [--updated YYYY-MM-DD]
        a removal request granted: the entry and its files go, the catalog's version goes up (sign it again)
    char_catalog.py sign --repo DIR --key PRIVATE.pem [--keys KEYS.json]
        signs the catalog's "catalog" member (canonical JSON) with an Ed25519 key through OpenSSL (an encrypted key:
        OpenSSL asks for its passphrase itself) and checks the result as the launcher will
    char_catalog.py check --repo DIR [--keys KEYS.json]
        checks the copy as the launcher will: the signature, then every entry, every file's size and SHA-256, the
        character files' members and data, the thumbnails' form
    char_catalog.py keygen --out FOLDER  |  pubkey --key PRIVATE.pem
        the catalog key (by the owner; the same steps as the release key, docs/RELEASE_SIGNING.md, but a key of its
        own: a release key is refused) -- its public key's entry goes into config/catalog_keys.json
    char_catalog.py keys --add ENTRY.json | --revoke KEY_ID  [--file config/catalog_keys.json]
        a rotation's new key beside the old, or a key withdrawn (kept, marked revoked: no launcher that has this
        file trusts it again); docs/CHARACTER_STORE.md, "The catalog key"

A submission record (JSON) holds the creator's explicit agreement; nothing is ever given a licence by default (the
historical Hall of Heroes characters are never added this way):

    {"character": "rook.chchar", "label": "Rook", "creator": "Their name", "creator_url": "https://...",
     "license": "CC-BY-4.0", "tags": ["knight"], "description": "...",
     "consent": {"agreed": true, "date": "2026-11-24", "record": "where the agreement is kept (private)"},
     "replaces": "<catalog id>"}            (only for a new version of a listed character)

The consent record stays with the maintainers: the catalog carries the creator, the licence and the attribution only.
"""
import argparse
import base64
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import char_library as L  # noqa: E402
import char_store as S  # noqa: E402
import release_trust as T  # noqa: E402


def _sha(b):
    return hashlib.sha256(b).hexdigest()


def _path(repo):
    return os.path.join(repo, *S.CATALOG_PATH.split("/"))


def _load(repo):
    p = _path(repo)
    if not os.path.isfile(p):
        return {"format": S.CATALOG_FORMAT, "schema_version": S.SCHEMA_VERSION,
                "catalog": {"catalog_version": 0, "updated": "", "characters": []}, "signatures": []}
    with open(p, encoding="utf-8") as f:
        return json.load(f)


def _save(repo, doc):
    p = _path(repo)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1, ensure_ascii=False)
        f.write("\n")


def read_submission(path):
    """the submission record, checked: an explicit agreement to a licence the store accepts"""
    with open(path, encoding="utf-8") as f:
        sub = json.load(f)
    c = sub.get("consent") or {}
    if c.get("agreed") is not True or not str(c.get("date") or "").strip() or not str(c.get("record") or "").strip():
        raise ValueError("no explicit agreement recorded (consent.agreed, date and record): not added")
    if sub.get("license") not in S.LICENSES:
        raise ValueError(f"licence {sub.get('license')!r} is not one the store accepts ({', '.join(S.LICENSES)})")
    if not str(sub.get("creator") or "").strip():
        raise ValueError("no creator: attribution is required")
    url = str(sub.get("creator_url") or "")
    if url and not url.startswith("https://"):
        raise ValueError("creator_url must be an https address")
    if not sub.get("character"):
        raise ValueError("no character file named")
    sub["character"] = os.path.join(os.path.dirname(os.path.abspath(path)), sub["character"])
    return sub


def build(repo, submissions, updated=None, log=print):
    doc = _load(repo)
    cat = doc["catalog"]
    by_id = {e["id"]: e for e in cat["characters"]}
    listed_sha = {e["edt_sha256"] for e in cat["characters"]}
    today = updated or datetime.date.today().isoformat()
    tmp = tempfile.mkdtemp(prefix="catalog-")
    added, problems = [], []
    try:
        lib = L.Library(os.path.join(tmp, "lib"))
        for sp in submissions:
            try:
                sub = read_submission(sp)
            except (OSError, ValueError) as e:
                problems.append(f"{os.path.basename(sp)}: {e}")
                continue
            cands, errs = L.find([sub["character"]])
            problems += [f"{os.path.basename(sp)}: {x}" for x in errs]
            rs = lib.import_candidates(cands)   # no key: a save still encrypted is reported, never decrypted here
            ok = [r for r in rs if r["status"] in ("imported", "similar", "duplicate")]
            if len(ok) != 1:
                problems += [f"{os.path.basename(sp)}: {r['status']}: {r.get('message', '')}" for r in rs] or \
                            [f"{os.path.basename(sp)}: no character in it"]
                if len(ok) > 1:
                    problems.append(f"{os.path.basename(sp)}: a submission is one character")
                continue
            lid = ok[0]["id"]
            m = lib.meta(lid)
            if m["sha256"] in listed_sha:
                problems.append(f"{os.path.basename(sp)}: this character is already in the catalog; not added again")
                continue
            old = by_id.get(sub.get("replaces") or "")
            if sub.get("replaces") and not old:
                problems.append(f"{os.path.basename(sp)}: replaces {sub['replaces']}, which isn't listed; not added")
                continue
            cid = old["id"] if old else m["sha256"][:16]
            if not old and cid in by_id:
                problems.append(f"{os.path.basename(sp)}: id {cid} is taken; not added")
                continue
            m["label"] = (sub.get("label") or m["label"])[:64]
            m["creator"], m["description"] = sub["creator"], sub.get("description") or ""
            m["tags"] = [t for t in sub.get("tags") or [] if isinstance(t, str)][:12]
            m["source"] = {"kind": "store", "license": sub["license"]}
            lib._write_meta(lid, m)
            chchar = lib.export(lid)
            with open(lib.preview_path(lid, "thumb.png"), "rb") as f:
                thumb = f.read()
            fpath, tpath = f"characters/{cid}/{cid}.chchar", f"characters/{cid}/thumb.png"
            os.makedirs(os.path.join(repo, "characters", cid), exist_ok=True)
            with open(os.path.join(repo, *fpath.split("/")), "wb") as f:
                f.write(chchar)
            with open(os.path.join(repo, *tpath.split("/")), "wb") as f:
                f.write(thumb)
            lic = sub["license"]
            entry = {
                "id": cid, "version": old["version"] + 1 if old else 1, "category": "community", "edt_sha256": m["sha256"],
                "previous_sha256": (old.get("previous_sha256", []) + [old["edt_sha256"]]) if old else [],
                "label": m["label"], "name": m.get("name") or "", "class": m.get("class") or "",
                "creator": sub["creator"], "creator_url": sub.get("creator_url") or "", "license": lic,
                "attribution": f"\"{m['label']}\" by {sub['creator']}, {S.LICENSES[lic][0]} ({S.LICENSES[lic][1]})",
                "tags": m["tags"], "description": m["description"],
                "added": old["added"] if old else today, "updated": today,
                "file": {"path": fpath, "sha256": _sha(chchar), "size": len(chchar)},
                "thumb": {"path": tpath, "sha256": _sha(thumb), "size": len(thumb)},
            }
            if old:
                cat["characters"] = [e for e in cat["characters"] if e["id"] != cid]
            cat["characters"].append(entry)
            by_id[cid] = entry
            listed_sha.add(m["sha256"])
            added.append(m["label"] + (f" (version {entry['version']})" if old else ""))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if added:
        cat["characters"].sort(key=lambda e: e["id"])
        cat["catalog_version"] = int(cat.get("catalog_version") or 0) + 1
        cat["updated"] = today
        doc["format"], doc["schema_version"] = S.CATALOG_FORMAT, S.SCHEMA_VERSION
        doc["signatures"] = []   # the signed part changed: sign it again
        _save(repo, doc)
    for x in problems:
        log("problem: " + x)
    log(f"{len(added)} added, {len(cat['characters'])} in the catalog (version {cat['catalog_version']})" +
        ("; not signed yet: run sign" if added else ""))
    return added, problems


def historical(repo, records_file, updated=None, log=print):
    """recovered characters added as category "historical": the original name and creator, the archival source, no
    licence (none is claimed); each through the library's import, exported as the library exports any character"""
    doc = _load(repo)
    cat = doc["catalog"]
    listed_sha = {e["edt_sha256"] for e in cat["characters"]}
    ids = {e["id"] for e in cat["characters"]}
    today = updated or datetime.date.today().isoformat()
    with open(records_file, encoding="utf-8") as f:
        records = json.load(f)
    tmp = tempfile.mkdtemp(prefix="catalog-hist-")
    added, problems = [], []
    try:
        lib = L.Library(os.path.join(tmp, "lib"))
        for n, rec in enumerate(records):
            tag = f"record {n + 1} ({rec.get('original_name') or rec.get('character')})"
            if rec.get("license"):
                problems.append(f"{tag}: a historical character is given no licence; not added")
                continue
            if not str(rec.get("collection") or "").strip():
                problems.append(f"{tag}: no collection named; not added")
                continue
            cands, errs = L.find([rec["character"]])
            problems += [f"{tag}: {x}" for x in errs]
            rs = [r for r in lib.import_candidates(cands) if r["status"] in ("imported", "similar", "duplicate")]
            if len(rs) != 1:
                problems.append(f"{tag}: not exactly one character in it; not added")
                continue
            lid = rs[0]["id"]
            m = lib.meta(lid)
            if m["sha256"] in listed_sha:
                problems.append(f"{tag}: already in the catalog; not added again")
                continue
            cid = m["sha256"][:16]
            if cid in ids:
                problems.append(f"{tag}: id {cid} is taken; not added")
                continue
            name = (rec.get("original_name") or m["label"])[:64]
            creator = (rec.get("creator") or "").strip()[:64] or "unknown"
            coll = rec["collection"][:80]
            m["label"], m["creator"] = name, creator
            m["description"] = (f"Recovered community creation, first shared on {coll}" +
                                (f" ({rec['original_date']})" if rec.get("original_date") else "") +
                                ". No licence is claimed: all rights remain with its creator.")
            m["tags"] = [t for t in rec.get("tags") or [] if isinstance(t, str)][:12]
            m["source"] = {"kind": "historical", "collection": coll, "original_name": name,
                           "original_date": rec.get("original_date") or "", "url": rec.get("archived_url") or "",
                           "rights": S.HISTORICAL_RIGHTS}
            lib._write_meta(lid, m)
            chchar = lib.export(lid)
            with open(lib.preview_path(lid, "thumb.png"), "rb") as f:
                thumb = f.read()
            fpath, tpath = f"characters/{cid}/{cid}.chchar", f"characters/{cid}/thumb.png"
            os.makedirs(os.path.join(repo, "characters", cid), exist_ok=True)
            with open(os.path.join(repo, *fpath.split("/")), "wb") as f:
                f.write(chchar)
            with open(os.path.join(repo, *tpath.split("/")), "wb") as f:
                f.write(thumb)
            cat["characters"].append({
                "id": cid, "version": 1, "category": "historical", "edt_sha256": m["sha256"], "previous_sha256": [],
                "label": name, "name": m.get("name") or "", "class": m.get("class") or "", "creator": creator,
                "attribution": f"\"{name}\" by {creator}, shared on {coll}",
                "source": {"collection": coll, "original_name": name, "original_date": rec.get("original_date") or "",
                           "original_url": rec.get("original_url") or "", "archived_url": rec.get("archived_url") or ""},
                "tags": m["tags"], "description": m["description"], "added": today, "updated": today,
                "file": {"path": fpath, "sha256": _sha(chchar), "size": len(chchar)},
                "thumb": {"path": tpath, "sha256": _sha(thumb), "size": len(thumb)},
            })
            listed_sha.add(m["sha256"])
            ids.add(cid)
            added.append(name)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if added:
        cat["characters"].sort(key=lambda e: e["id"])
        cat["catalog_version"] = int(cat.get("catalog_version") or 0) + 1
        cat["updated"] = today
        doc["format"], doc["schema_version"] = S.CATALOG_FORMAT, S.SCHEMA_VERSION
        doc["signatures"] = []
        _save(repo, doc)
    for x in problems:
        log("problem: " + x)
    log(f"{len(added)} historical characters added, {len(cat['characters'])} in the catalog (version "
        f"{cat['catalog_version']})" + ("; not signed yet: run sign" if added else ""))
    return added, problems


def remove(repo, cid, updated=None, log=print):
    """a removal request granted: the entry and its files removed, the catalog's version up, the signatures dropped
    (nothing of the request -- who asked, why -- is written into the repository)"""
    doc = _load(repo)
    cat = doc["catalog"]
    e = next((x for x in cat["characters"] if x["id"] == cid), None)
    if e is None:
        sys.exit(f"no character {cid} in the catalog")
    cat["characters"] = [x for x in cat["characters"] if x["id"] != cid]
    shutil.rmtree(os.path.join(repo, "characters", cid), ignore_errors=True)
    cat["catalog_version"] = int(cat.get("catalog_version") or 0) + 1
    cat["updated"] = updated or datetime.date.today().isoformat()
    doc["signatures"] = []
    _save(repo, doc)
    log(f"removed {cid} ({e['label']}); catalog version {cat['catalog_version']}: sign it again, then publish")
    return e


def sign(repo, key, keys_file=None, log=print):
    """the catalog signed with the private key (OpenSSL, as tools/release.py signs releases), then verified"""
    import release as R
    doc = _load(repo)
    msg = S.canonical(doc["catalog"])
    tmp = tempfile.mkdtemp(prefix="catalog-sign-")
    try:
        m, sig = os.path.join(tmp, "catalog.canonical"), os.path.join(tmp, "catalog.sig")
        with open(m, "wb") as f:
            f.write(msg)
        subprocess.check_call([R.openssl(), "pkeyutl", "-sign", "-inkey", key, "-rawin", "-in", m, "-out", sig])
        with open(sig, "rb") as f:
            s = f.read()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    # a release key (any in config/release_keys.json, revoked or not) never signs the catalog
    with open(T.KEYS_FILE, encoding="utf-8") as f:
        rel = [T.public_key_from(k["ed25519"]) for k in json.load(f).get("keys", []) if k.get("ed25519")]
    if any(T.ed25519_verify(pk, msg, s) for pk in rel):
        sys.exit("that is a release key: the catalog is signed with its own key, never the release key")
    keys = S.trusted_keys(S.DEFAULT_BASE, {}, keys_file=keys_file or S.KEYS_FILE)
    kid = next((k for k, pk in keys if T.ed25519_verify(pk, msg, s)), None)
    if kid is None:
        sys.exit("signed, but by no key in " + (keys_file or S.KEYS_FILE) + ": the catalog was not changed")
    doc["signatures"] = [x for x in doc.get("signatures", []) if x.get("key_id") != kid] + \
                        [{"key_id": kid, "ed25519": base64.b64encode(s).decode()}]
    _save(repo, doc)
    S.parse_catalog(open(_path(repo), "rb").read(), keys)
    log(f"signed by {kid}; verified as the launcher will (catalog version {doc['catalog']['catalog_version']})")
    return kid


def check(repo, keys_file=None, log=print, unsigned=False):
    """[problems] -- empty when the launcher would accept the catalog and every listed character (unsigned: all of it
    but the signature, before the owner signs)"""
    with open(_path(repo), "rb") as f:
        data = f.read()
    if unsigned:
        doc = json.loads(data.decode("utf-8"))
        if doc.get("format") != S.CATALOG_FORMAT or doc.get("schema_version") != S.SCHEMA_VERSION:
            raise S.StoreError("not a Cubic.Heroes character catalog of this schema")
        cat, problems = S.parse_entries(doc["catalog"], doc["schema_version"], "(not signed yet)")
    else:
        cat, problems = S.parse_catalog(data, S.trusted_keys(S.DEFAULT_BASE, {}, keys_file=keys_file or S.KEYS_FILE))
    for e in cat["characters"]:
        try:
            for kind in ("file", "thumb"):
                with open(os.path.join(repo, *e[kind]["path"].split("/")), "rb") as f:
                    b = f.read()
                if len(b) != e[kind]["size"] or _sha(b) != e[kind]["sha256"]:
                    raise S.StoreError(f"its {kind} does not match the catalog")
                if kind == "thumb":
                    S.thumb_rle(b)
                else:
                    files = L.read_zip(b, e["label"])
                    extra = sorted(set(files) - S.CHCHAR_MEMBERS)
                    if extra:
                        raise S.StoreError(f"its file holds more than a character ({', '.join(extra)})")
                    c = L.from_chchar(b, e["label"])[0]
                    if _sha(c.files["character.edt"]) != e["edt_sha256"]:
                        raise S.StoreError("its character data is not the one the catalog describes")
                    L.edt_format.parse(c.files["character.edt"])
        except (OSError, S.StoreError, L.LibraryError, L.edt_format.EdtError) as err:
            problems.append(f"{e['id']} ({e['label']}): {err}")
    # the official content mapping (config/official_characters.json): which of its catalog ids are in this catalog
    # (a report: the catalog's own entries are never changed by it)
    try:
        import official_content as O
        ids = {e["id"] for e in cat["characters"]}
        for pid, p in O.load().items():
            mapped = [c for c in p["characters"] if c["catalog_id"]]
            gone = [c["slot"] for c in mapped if c["catalog_id"] not in ids]
            log(f"official content {pid}: {len(mapped)} of its {len(p['characters'])} characters are catalog entries"
                + (f"; {len(gone)} not in this catalog ({', '.join(gone[:5])})" if gone else ""))
    except Exception as err:   # noqa: BLE001 -- a report, never a reason to refuse the catalog
        log(f"official content mapping not checked ({err})")
    for x in problems:
        log("problem: " + x)
    log(f"catalog version {cat['catalog_version']}, signed by {cat['key_id']}: {len(cat['characters'])} characters "
        f"checked, {len(problems)} problem(s)")
    return problems


def keys_cmd(path, add=None, revoke=None, log=print):
    """config/catalog_keys.json changed: a key added (a rotation: beside the others) or revoked (kept, marked)"""
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    keys = data.setdefault("keys", [])
    if add:
        with open(add, encoding="utf-8") as f:
            entry = json.load(f)
        pk = T.public_key_from(entry["ed25519"])
        kid = T.key_id(pk)
        if entry.get("id") and entry["id"] != kid:
            sys.exit("the entry's id does not match its key")
        if kid in S.release_key_ids():
            sys.exit("that is a release key: the catalog is signed with its own key, never the release key")
        if any(k.get("id") == kid for k in keys):
            sys.exit(f"{kid} is already in {path}")
        keys.append({"id": kid, "ed25519": entry["ed25519"], "added": entry.get("added") or datetime.date.today().isoformat(),
                     "note": entry.get("note") or "a character catalog key"})
        log(f"added {kid}")
    if revoke:
        k = next((k for k in keys if k.get("id") == revoke), None)
        if k is None:
            sys.exit(f"no key {revoke} in {path}")
        k["revoked"] = True
        k["revoked_on"] = datetime.date.today().isoformat()
        log(f"revoked {revoke}: launchers with this file no longer trust it, nor the catalogs it signed")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=1)
        f.write("\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--repo", required=True)
    b.add_argument("--updated", help="the date recorded (default: today)")
    b.add_argument("submissions", nargs="+")
    h = sub.add_parser("historical")
    h.add_argument("--repo", required=True)
    h.add_argument("--updated")
    h.add_argument("records")
    rm = sub.add_parser("remove")
    rm.add_argument("--repo", required=True)
    rm.add_argument("--id", required=True)
    rm.add_argument("--updated")
    s = sub.add_parser("sign")
    s.add_argument("--repo", required=True)
    s.add_argument("--key", required=True)
    s.add_argument("--keys", help="the trusted keys to verify with (default config/catalog_keys.json)")
    c = sub.add_parser("check")
    c.add_argument("--repo", required=True)
    c.add_argument("--keys")
    c.add_argument("--unsigned", action="store_true", help="everything but the signature (before signing)")
    k = sub.add_parser("keygen")
    k.add_argument("--out", required=True)
    k.add_argument("--note", default="the character catalog key")
    pk = sub.add_parser("pubkey")
    pk.add_argument("--key", required=True)
    pk.add_argument("--note", default="the character catalog key")
    ks = sub.add_parser("keys")
    ks.add_argument("--file", default=S.KEYS_FILE)
    g = ks.add_mutually_exclusive_group(required=True)
    g.add_argument("--add", help="a public key's entry (keygen/pubkey wrote it)")
    g.add_argument("--revoke", help="a key's id")
    a = ap.parse_args(argv)
    if a.cmd == "keys":
        keys_cmd(a.file, a.add, a.revoke)
        return 0
    if a.cmd == "build":
        _, problems = build(a.repo, a.submissions, a.updated)
        return 1 if problems else 0
    if a.cmd == "historical":
        _, problems = historical(a.repo, a.records, a.updated)
        return 1 if problems else 0
    if a.cmd == "remove":
        remove(a.repo, a.id, a.updated)
        return 0
    if a.cmd == "sign":
        sign(a.repo, a.key, a.keys)
        return 0
    if a.cmd == "check":
        return 1 if check(a.repo, a.keys, unsigned=a.unsigned) else 0
    import release as R
    if a.cmd == "keygen":
        os.makedirs(a.out, exist_ok=True)
        key = os.path.join(a.out, "catalog-signing-key.pem")
        if os.path.exists(key):
            sys.exit(f"{key} exists: a key is never written over")
        R._genpkey(key)
        print("private key (keep it offline, with a backup):", key)
        entry = R.key_entry(R.extract_public(key), a.note)
    else:
        entry = R.key_entry(R.extract_public(os.path.abspath(a.key)), a.note)
    if entry["id"] in S.release_key_ids():
        sys.exit("that is a release key: the catalog needs a key of its own (keygen makes one)")
    out = os.path.join(os.path.dirname(os.path.abspath(a.key if a.cmd == "pubkey" else key)), "catalog-signing-key.pub.json")
    with open(out, "w") as f:
        json.dump(entry, f, indent=1)
    print(f"the public key's entry ({out}); add it with: py -3 tools/char_catalog.py keys --add \"{out}\"")
    print(json.dumps(entry, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
