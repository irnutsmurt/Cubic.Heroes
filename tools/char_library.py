#!/usr/bin/env python3
"""The local character library: player-made characters imported, kept, previewed, exported and installed into the
game -- separate from the game's saves, which it only reads (to find installed characters) and adds new character
folders to, never changing or removing an existing one.

Layout (<install>/data/characters, kept with the installation across launcher and game updates):
  entries/<id>/character.edt      the character, plain (tools/edt_format.py); <id> = its SHA-256's first 16 hex
  entries/<id>/character.json     what the library knows: name, class, label, creator, source, hashes, previews
  entries/<id>/preview/thumb.png, views.png     generated (tools/char_preview.py), separate from the data
  originals/<id>/...              the files it was imported from, exactly as they were (local only; never exported)
  library.json                    the library's own state

The portable file (.chchar, a ZIP): character.json, character.edt, preview/thumb.png, preview/views.png -- the
character and what describes it, nothing else: no PARAM.SFO (it carries the creator's PS3 account and console
identifiers), no PARAM.PFD, no ICON0.PNG (mostly the game's own artwork), no key. Reading one never trusts its
previews (they are made again) or its metadata's hashes without checking them.

  python tools/char_library.py <library dir> import <path>... [--key-from-store] [--program EBOOT.elf]
  python tools/char_library.py <library dir> list
  python tools/char_library.py <library dir> export <out.chchar or dir> <id>...
  python tools/char_library.py <library dir> install <savedata dir> <id>...
"""
import datetime
import hashlib
import io
import json
import os
import posixpath
import shutil
import struct
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import char_preview  # noqa: E402
import edt_format  # noqa: E402
import ps3_save  # noqa: E402

FORMAT = "cubic.heroes.character"
FORMAT_VERSION = 1
IMPORTER = "char_library 1"
EDIT_PREFIX = "BLJM60180T"      # the edit-data folder prefix (the US release uses the Japanese release's)

# archive limits (a character is < 0.5 MB; a store archive of many is still small)
ZIP_MAX_MEMBERS = 4000
ZIP_MAX_MEMBER = 16 * 1024 * 1024
ZIP_MAX_TOTAL = 256 * 1024 * 1024
ZIP_MAX_RATIO = 200
FILE_MAX = 16 * 1024 * 1024


class LibraryError(Exception):
    pass


def _now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _sha(b):
    return hashlib.sha256(b).hexdigest()


# ---- reading sources safely ----------------------------------------------------------------------------------

def safe_member_name(name):
    """A ZIP member's name as a relative POSIX path, or None when it could write outside its folder."""
    n = name.replace("\\", "/")
    if n.startswith("/") or (len(n) > 1 and n[1] == ":") or "\0" in n:
        return None
    parts = [p for p in n.split("/") if p not in ("", ".")]
    if any(p == ".." for p in parts):
        return None
    return "/".join(parts)


def read_zip(data, label):
    """{relative path: bytes} of a ZIP, in memory, within the limits; LibraryError naming what is wrong."""
    try:
        z = zipfile.ZipFile(io.BytesIO(data))
    except zipfile.BadZipFile:
        raise LibraryError(f"{label}: not a readable ZIP archive")
    infos = z.infolist()
    if len(infos) > ZIP_MAX_MEMBERS:
        raise LibraryError(f"{label}: too many files in the archive ({len(infos)})")
    out, total = {}, 0
    for i in infos:
        if i.is_dir():
            continue
        name = safe_member_name(i.filename)
        if name is None:
            raise LibraryError(f"{label}: an unsafe path in the archive ({i.filename!r}); nothing imported from it")
        if i.file_size > ZIP_MAX_MEMBER:
            raise LibraryError(f"{label}: {name} is too large ({i.file_size} bytes)")
        if i.compress_size and i.file_size > 1024 * 1024 and i.file_size / i.compress_size > ZIP_MAX_RATIO:
            raise LibraryError(f"{label}: {name} expands too far (an archive bomb?)")
        total += i.file_size
        if total > ZIP_MAX_TOTAL:
            raise LibraryError(f"{label}: the archive expands to too much data")
        with z.open(i) as f:
            b = f.read(ZIP_MAX_MEMBER + 1)
        if len(b) != i.file_size:
            raise LibraryError(f"{label}: {name} is not the size the archive says")
        out[name] = b
    return out


class Candidate:
    """One character found in what the player chose: its DATA.EDT and PARAM.PFD bytes and the original files."""

    def __init__(self, label, files, source):
        self.label = label          # where it came from, for messages ("Link.zip: PS3/SAVEDATA/BLJM...")
        self.files = files          # {name: bytes}: the original files of the character, as found
        self.source = source        # {"kind": ..., "name": ...}
        self.meta = None            # a .chchar's character.json

    @property
    def edt(self):
        for k, v in self.files.items():
            if posixpath.basename(k).upper() in ("DATA.EDT", "CHARACTER.EDT") or k.lower().endswith(".edt"):
                return v
        return b""

    @property
    def pfd(self):
        for k, v in self.files.items():
            if posixpath.basename(k).upper() == "PARAM.PFD":
                return v
        return b""


def _group_tree(files, label, kind):
    """Characters in a set of paths (a folder's or an archive's): each folder holding a DATA.EDT, each .chchar, each
    loose .edt file."""
    out = []
    folders = {}
    for path, b in files.items():
        d, base = posixpath.split(path)
        folders.setdefault(d, {})[base] = b
    for d in sorted(folders):
        entries = folders[d]
        upper = {k.upper(): k for k in entries}
        if "DATA.EDT" in upper:
            group = {k: entries[k] for k in entries if k.upper() in ("DATA.EDT", "PARAM.PFD", "PARAM.SFO", "ICON0.PNG")}
            out.append(Candidate(f"{label}: {d or '.'}", group, {"kind": kind, "name": f"{label}:{d}"}))
        for k in sorted(entries):
            if k.lower().endswith(".chchar"):
                out += from_chchar(entries[k], f"{label}: {posixpath.join(d, k)}")
            elif k.lower().endswith(".edt") and k.upper() != "DATA.EDT":
                out.append(Candidate(f"{label}: {posixpath.join(d, k)}", {k: entries[k]}, {"kind": kind, "name": k}))
    return out


def from_chchar(data, label):
    files = read_zip(data, label)
    if "character.json" not in files or "character.edt" not in files:
        raise LibraryError(f"{label}: not a Cubic.Heroes character file (no character.json / character.edt)")
    try:
        meta = json.loads(files["character.json"].decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        raise LibraryError(f"{label}: its character.json is not readable")
    if meta.get("format") != FORMAT or not isinstance(meta.get("format_version"), int):
        raise LibraryError(f"{label}: not a Cubic.Heroes character file")
    if meta["format_version"] > FORMAT_VERSION:
        raise LibraryError(f"{label}: made by a newer Cubic.Heroes (format {meta['format_version']}); update to import it")
    if meta.get("sha256") != _sha(files["character.edt"]):
        raise LibraryError(f"{label}: the character data does not match its recorded SHA-256 (damaged or altered)")
    c = Candidate(label, {"character.edt": files["character.edt"]}, {"kind": "chchar", "name": label})
    c.meta = meta
    return [c]


def find(paths):
    """The characters in the paths the player chose (folders, ZIPs, .chchar files, DATA.EDT or .edt files):
    ([Candidate], [error messages])."""
    found, errors = [], []
    for p in paths:
        label = os.path.basename(os.path.normpath(p))
        try:
            if os.path.isdir(p):
                files = {}
                for root, dirs, names in os.walk(p):
                    dirs.sort()
                    depth = os.path.relpath(root, p).count(os.sep)
                    if depth > 8:
                        dirs[:] = []
                        continue
                    for n in sorted(names):
                        fp = os.path.join(root, n)
                        up = n.upper()
                        if up in ("DATA.EDT", "PARAM.PFD", "PARAM.SFO", "ICON0.PNG") or n.lower().endswith(
                                (".edt", ".chchar", ".zip")):
                            if os.path.getsize(fp) > FILE_MAX:
                                errors.append(f"{label}: {n} is too large to be a character")
                                continue
                            rel = os.path.relpath(fp, p).replace(os.sep, "/")
                            if n.lower().endswith(".zip"):
                                found += _group_tree(read_zip(open(fp, "rb").read(), f"{label}/{rel}"), f"{label}/{rel}", "zip")
                            else:
                                files[rel] = open(fp, "rb").read()
                found += _group_tree(files, label, "folder")
            elif os.path.isfile(p):
                if os.path.getsize(p) > FILE_MAX * 16:
                    raise LibraryError(f"{label}: too large")
                data = open(p, "rb").read()
                low = p.lower()
                if low.endswith(".chchar"):
                    found += from_chchar(data, label)
                elif data[:2] == b"PK":
                    found += _group_tree(read_zip(data, label), label, "zip")
                else:
                    d = os.path.dirname(p)
                    files = {os.path.basename(p): data}
                    if os.path.basename(p).upper() == "DATA.EDT":
                        for sib in ("PARAM.PFD", "PARAM.SFO", "ICON0.PNG"):
                            sp = os.path.join(d, sib)
                            if os.path.isfile(sp) and os.path.getsize(sp) <= FILE_MAX:
                                files[sib] = open(sp, "rb").read()
                    found.append(Candidate(label, files, {"kind": "file", "name": label}))
            else:
                errors.append(f"{label}: not found")
        except (LibraryError, OSError) as e:
            errors.append(str(e))
    if not found and not errors:
        errors.append("no character found (a character is a DATA.EDT, a PS3 character save folder, a ZIP of them, "
                      "or a .chchar file)")
    return found, errors


# ---- the game's save metadata, generated (never the creator's) -------------------------------------------------

def make_sfo(directory, name):
    """A PARAM.SFO for an edit-data folder, as Cubic.Heroes' own save system writes one: no account or console
    identifiers (ACCOUNT_ID zeros, PARAMS empty)."""
    detail = ("NAME:" + name).encode("utf-8") + b"\0"
    params = bytearray(1024)
    params[0] = 1
    fields = [  # key, format, value, max length
        ("ACCOUNT_ID", 0x0004, b"0000000000000000", 16),
        ("ATTRIBUTE", 0x0404, struct.pack("<I", 0), 4),
        ("CATEGORY", 0x0204, b"SD\0", 4),
        ("DETAIL", 0x0204, detail[:1023] + (b"\0" if len(detail) > 1023 else b""), 1024),
        ("PARAMS", 0x0004, bytes(params), 1024),
        ("PARAMS2", 0x0004, bytes(12), 12),
        ("PARENTAL_LEVEL", 0x0404, struct.pack("<I", 0), 4),
        ("SAVEDATA_DIRECTORY", 0x0204, directory.encode("ascii") + b"\0", 64),
        ("SAVEDATA_LIST_PARAM", 0x0204, b"\0", 8),
        ("SUB_TITLE", 0x0204, b"EDIT DATA\0", 128),
        ("TITLE", 0x0204, b"3D DOT GAME HEROES\0", 128),
    ]
    keys = b""
    koff = []
    for k, *_ in fields:
        koff.append(len(keys))
        keys += k.encode("ascii") + b"\0"
    while len(keys) % 4:
        keys += b"\0"
    data = b""
    doff = []
    for _k, _f, v, mx in fields:
        doff.append(len(data))
        data += v + b"\0" * (mx - len(v))
    hdr_size = 20 + 16 * len(fields)
    out = b"\0PSF" + struct.pack("<IIII", 0x0101, hdr_size, hdr_size + len(keys), len(fields))
    for i, (_k, fmt, v, mx) in enumerate(fields):
        out += struct.pack("<HHIII", koff[i], fmt, len(v), mx, doff[i])
    return out + keys + data


# ---- the library -----------------------------------------------------------------------------------------------

class Library:
    def __init__(self, root):
        self.root = root
        self.entries = os.path.join(root, "entries")
        self.originals = os.path.join(root, "originals")
        os.makedirs(self.entries, exist_ok=True)
        os.makedirs(self.originals, exist_ok=True)
        self.state_path = os.path.join(root, "library.json")
        try:
            self.state = json.load(open(self.state_path, encoding="utf-8"))
        except (OSError, ValueError):
            self.state = {"version": 1}

    def _save_state(self):
        tmp = self.state_path + ".tmp"
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            json.dump(self.state, f, indent=1)
        os.replace(tmp, self.state_path)

    # -- reading
    def ids(self):
        return sorted(d for d in os.listdir(self.entries) if os.path.isfile(os.path.join(self.entries, d, "character.json")))

    def meta(self, cid):
        return json.load(open(os.path.join(self.entries, cid, "character.json"), encoding="utf-8"))

    def edt(self, cid):
        return open(os.path.join(self.entries, cid, "character.edt"), "rb").read()

    def search(self, text=""):
        t = text.casefold()
        out = []
        for cid in self.ids():
            m = self.meta(cid)
            hay = " ".join([m.get("label", ""), m.get("name", ""), m.get("class", ""), m.get("creator", "") or "",
                            " ".join(m.get("tags", []))]).casefold()
            if t in hay:
                out.append((cid, m))
        return out

    # -- importing
    def import_candidates(self, cands, key=None, program=None):
        """[{label, status, id?, message}] -- status: imported, duplicate, similar (imported; another entry has the
        same model), needs-key, wrong-key, damaged, unsupported. Nothing of the sources is ever changed."""
        results = []
        shapes = {}
        for cid in self.ids():
            shapes.setdefault(self.meta(cid).get("shape_sha256"), []).append(cid)
        for c in cands:
            edt = c.edt
            kind, info = ps3_save.classify(edt, c.pfd)
            if kind == "damaged":
                results.append({"label": c.label, "status": "damaged", "message": info})
                continue
            decrypted = False
            if kind == "encrypted":
                if key is None:
                    results.append({"label": c.label, "status": "needs-key",
                                    "message": "saved on a PS3 and still encrypted: importing it needs your PS3 save "
                                               "key (Cubic.Heroes does not include it)"})
                    continue
                if program is None:
                    results.append({"label": c.label, "status": "needs-key",
                                    "message": "decrypting needs the installed game's program (EBOOT.elf)"})
                    continue
                cached = bytes.fromhex(self.state["secure_file_id"]) if self.state.get("secure_file_id") else None
                sid = ps3_save.find_secure_id(program, edt, info, cached)
                if sid is None:
                    results.append({"label": c.label, "status": "unsupported",
                                    "message": "this encrypted save is not 3D Dot Game Heroes' (US) character data"})
                    continue
                if sid != cached:
                    self.state["secure_file_id"] = sid.hex()
                    self._save_state()
                try:
                    edt = ps3_save.decrypt_character(key, sid, edt, info)
                except ps3_save.SaveError as e:
                    results.append({"label": c.label, "status": "wrong-key", "message": str(e)})
                    continue
                decrypted = True
            r = self._add(c, edt, decrypted, shapes)
            results.append(r)
        return results

    def _add(self, cand, edt, decrypted, shapes):
        ch = edt_format.parse(edt)
        sha = _sha(edt)
        cid = sha[:16]
        final = os.path.join(self.entries, cid)
        if os.path.exists(os.path.join(final, "character.json")):
            return {"label": cand.label, "status": "duplicate", "id": cid,
                    "message": f"already in the library as \"{self.meta(cid).get('label')}\" (the same file)"}
        shape = edt_format.shape_hash(ch)
        info = edt_format.describe(ch)
        src_meta = cand.meta or {}
        m = {
            "format": FORMAT, "format_version": FORMAT_VERSION,
            "id": cid, "sha256": sha, "shape_sha256": shape, "size": len(edt),
            "name": info["name"], "name_stored": ch.name, "class": info["class"], "gender": ch.gender,
            "voxels": info["voxels"],
            "label": src_meta.get("label") or info["name"] or "Character",
            "creator": src_meta.get("creator"), "description": src_meta.get("description"),
            "tags": [t for t in src_meta.get("tags", []) if isinstance(t, str)][:20],
            "source": src_meta.get("source") or {"kind": cand.source["kind"], "imported_from": cand.label,
                                                  "decrypted_from_ps3_save": decrypted},
            "imported": _now(), "importer": IMPORTER,
            "preview": {"renderer": char_preview.RENDERER},
            "compatibility": {"game": "3D Dot Game Heroes (BLUS-30490)", "edit_data": "EDTFILE1.9"},
        }
        thumb, views, ok = char_preview.make(edt)
        m["preview"]["ok"] = ok
        tmp = tempfile.mkdtemp(prefix=".import-", dir=self.entries)
        try:
            os.makedirs(os.path.join(tmp, "preview"))
            for name, b in (("character.edt", edt), ("preview/thumb.png", thumb), ("preview/views.png", views)):
                with open(os.path.join(tmp, name), "wb") as f:
                    f.write(b)
            with open(os.path.join(tmp, "character.json"), "w", encoding="utf-8", newline="\n") as f:
                json.dump(m, f, indent=1, ensure_ascii=False)
            os.replace(tmp, final)
        except Exception:
            shutil.rmtree(tmp, ignore_errors=True)
            raise
        # the originals, exactly as they were (local only)
        od = os.path.join(self.originals, cid)
        if not os.path.exists(od):
            os.makedirs(od)
            for name, b in cand.files.items():
                p = os.path.join(od, *safe_member_name(name).split("/")) if safe_member_name(name) else None
                if p:
                    os.makedirs(os.path.dirname(p), exist_ok=True)
                    with open(p, "wb") as f:
                        f.write(b)
        others = [o for o in shapes.get(shape, []) if o != cid]
        shapes.setdefault(shape, []).append(cid)
        if others:
            return {"label": cand.label, "status": "similar", "id": cid,
                    "message": f"imported; the same model as \"{self.meta(others[0]).get('label')}\" (another name "
                               "or settings)"}
        return {"label": cand.label, "status": "imported", "id": cid, "message": f"imported \"{m['label']}\""}

    # -- managing
    def rename(self, cid, label):
        m = self.meta(cid)
        m["label"] = label.strip()[:64] or m["label"]
        p = os.path.join(self.entries, cid, "character.json")
        with open(p + ".tmp", "w", encoding="utf-8", newline="\n") as f:
            json.dump(m, f, indent=1, ensure_ascii=False)
        os.replace(p + ".tmp", p)

    def delete(self, cid):
        """Remove the entry (and its originals) from the library; characters installed in the game stay."""
        for d in (os.path.join(self.entries, cid), os.path.join(self.originals, cid)):
            if os.path.isdir(d):
                shutil.rmtree(d)

    # -- exporting
    def export(self, cid):
        """The .chchar bytes of an entry: the character and its description only."""
        m = self.meta(cid)
        edt = self.edt(cid)
        if _sha(edt) != m["sha256"]:
            raise LibraryError(f"{m.get('label')}: its character data changed on disk; not exported")
        keep = ("format", "format_version", "sha256", "shape_sha256", "size", "name", "name_stored", "class",
                "gender", "voxels", "label", "creator", "description", "tags", "compatibility")
        out = {k: m.get(k) for k in keep}
        src = m.get("source") or {}
        out["source"] = {k: v for k, v in src.items() if k in ("kind", "collection", "url", "captured", "creator_url",
                                                               "license", "permission")}
        out["exported"] = _now()
        out["preview"] = {"renderer": char_preview.RENDERER}
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
            for name, b in (("character.json", json.dumps(out, indent=1, ensure_ascii=False).encode("utf-8")),
                            ("character.edt", edt),
                            ("preview/thumb.png", open(os.path.join(self.entries, cid, "preview", "thumb.png"), "rb").read()),
                            ("preview/views.png", open(os.path.join(self.entries, cid, "preview", "views.png"), "rb").read())):
                zi = zipfile.ZipInfo(name, date_time=(2010, 1, 1, 0, 0, 0))   # fixed: the same entry exports the same bytes
                zi.compress_type = zipfile.ZIP_DEFLATED
                z.writestr(zi, b)
        return buf.getvalue()

    # -- the game
    @staticmethod
    def installed(savedata):
        """{sha256 of DATA.EDT: folder} of the edit-data folders in the game's save folder, and [(folder, reason)]
        of those the game cannot use (still encrypted, damaged)."""
        found, unusable = {}, []
        if not os.path.isdir(savedata):
            return found, unusable
        for d in sorted(os.listdir(savedata)):
            if not d.startswith(EDIT_PREFIX):
                continue
            edt, pfd = ps3_save.scan_folder(os.path.join(savedata, d))
            kind, info = ps3_save.classify(edt, pfd)
            if kind == "plain":
                found[_sha(edt)] = d
            else:
                unusable.append((d, "saved on a PS3 and still encrypted: import it through the character library"
                                 if kind == "encrypted" else info))
        return found, unusable

    def install(self, cid, savedata):
        """Put the entry into the game's saves as a new edit-data folder; never replaces one. Returns the folder."""
        m = self.meta(cid)
        edt = self.edt(cid)
        have, _ = self.installed(savedata)
        if m["sha256"] in have:
            return have[m["sha256"]], False
        folder = EDIT_PREFIX + m["sha256"][:20].upper()
        dst = os.path.join(savedata, folder)
        if os.path.exists(dst):
            raise LibraryError(f"{folder} already exists in the game's saves with other content; not replaced")
        os.makedirs(savedata, exist_ok=True)
        tmp = tempfile.mkdtemp(prefix=".install-", dir=savedata)
        try:
            for name, b in (("DATA.EDT", edt), ("PARAM.SFO", make_sfo(folder, m["name_stored"])),
                            ("ICON0.PNG", open(os.path.join(self.entries, cid, "preview", "thumb.png"), "rb").read()),
                            ("PARAM.PFD", b"DATA.EDT\n")):   # Cubic.Heroes' own list of the protected files
                with open(os.path.join(tmp, name), "wb") as f:
                    f.write(b)
            os.replace(tmp, dst)
        except Exception:
            shutil.rmtree(tmp, ignore_errors=True)
            raise
        return folder, True


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        sys.exit(__doc__)
    lib = Library(a[0])
    cmd, rest = a[1], a[2:]
    if cmd == "list":
        for cid, m in lib.search(""):
            print(f"{cid}  {m['label']}  ({m['class']}, {sum(m['voxels'])} voxels)  {m.get('creator') or ''}")
        return 0
    if cmd == "import":
        key = None
        program = None
        if "--program" in rest:
            i = rest.index("--program")
            program = open(rest[i + 1], "rb").read()
            rest = rest[:i] + rest[i + 2:]
        if "--key-from-store" in rest:
            rest.remove("--key-from-store")
            import secret_store
            key = secret_store.load()
        cands, errs = find(rest)
        for e in errs:
            print("error:", e)
        for r in lib.import_candidates(cands, key, program):
            print(f"{r['status']:>11}  {r['label']}: {r['message']}")
        return 0
    if cmd == "export":
        out, ids = rest[0], rest[1:]
        for cid in ids:
            b = lib.export(cid)
            p = out if out.lower().endswith(".chchar") and len(ids) == 1 else os.path.join(
                out, edt_format.display_name(lib.meta(cid)["label"]).replace("/", "_") + f"-{cid[:8]}.chchar")
            os.makedirs(os.path.dirname(os.path.abspath(p)), exist_ok=True)
            open(p, "wb").write(b)
            print(p)
        return 0
    if cmd == "install":
        for cid in rest[1:]:
            folder, new = lib.install(cid, rest[0])
            print(f"{cid}: {folder} ({'installed' if new else 'already installed'})")
        return 0
    sys.exit(__doc__)


if __name__ == "__main__":
    sys.exit(main())
