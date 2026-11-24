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

INDEX_VERSION = 1
PREVIEW_FILES = ("thumb.png", "views.png", "thumb.rle", "views.rle")
INSTALL_FILES = {"DATA.EDT", "PARAM.SFO", "ICON0.PNG", "PARAM.PFD"}


def source_kind(src):
    """a character's source, for filtering: ps3 (decrypted from a PS3 save), chchar (a Cubic.Heroes character
    file), hall-of-heroes, community, or files (anything else the player imported)"""
    k = (src or {}).get("kind") or "files"
    if (src or {}).get("decrypted_from_ps3_save"):
        return "ps3"
    return k if k in ("chchar", "hall-of-heroes", "community") else "files"


def sfo_detail_name(sfo):
    """the character's name from an edit save's PARAM.SFO (its DETAIL "NAME:..."), in ordinary letters; "" if none"""
    try:
        if sfo[:4] != b"\0PSF":
            return ""
        kt, dt, n = struct.unpack("<III", sfo[8:20])
        for i in range(min(n, 64)):
            ko, _fmt, ln, _mx, do = struct.unpack("<HHIII", sfo[20 + 16 * i:36 + 16 * i])
            key = sfo[kt + ko:sfo.index(b"\0", kt + ko)]
            if key == b"DETAIL":
                v = sfo[dt + do:dt + do + ln].split(b"\0")[0].decode("utf-8", "replace")
                return edt_format.display_name(v[5:] if v.startswith("NAME:") else v)[:64]
    except (struct.error, ValueError, IndexError):
        pass
    return ""


def _write_atomic(path, data):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


class Library:
    def __init__(self, root, log=None):
        self.root = root
        self.log = log or (lambda level, text: None)   # diagnostics: never a key, a payload or private save data
        self.entries = os.path.join(root, "entries")
        self.originals = os.path.join(root, "originals")
        os.makedirs(self.entries, exist_ok=True)
        os.makedirs(self.originals, exist_ok=True)
        self.state_path = os.path.join(root, "library.json")
        self.index_path = os.path.join(root, "index.json")
        try:
            self.state = json.load(open(self.state_path, encoding="utf-8"))
        except (OSError, ValueError):
            self.state = {"version": 1}
        self._index = None
        # an import or install interrupted before its final rename leaves only a hidden temporary folder: removed
        for d in os.listdir(self.entries):
            if d.startswith(".import-"):
                shutil.rmtree(os.path.join(self.entries, d), ignore_errors=True)
                self.log("warning", f"characters: removed an interrupted import's leftovers ({d})")

    def _save_state(self):
        _write_atomic(self.state_path, json.dumps(self.state, indent=1).encode("utf-8"))

    # -- reading
    def ids(self):
        return sorted(d for d in os.listdir(self.entries)
                      if not d.startswith(".") and os.path.isfile(os.path.join(self.entries, d, "character.json")))

    def meta(self, cid):
        with open(os.path.join(self.entries, cid, "character.json"), encoding="utf-8") as f:
            return json.load(f)

    def _write_meta(self, cid, m):
        _write_atomic(os.path.join(self.entries, cid, "character.json"),
                      json.dumps(m, indent=1, ensure_ascii=False).encode("utf-8"))
        self._index_put(cid, m)

    def edt(self, cid):
        with open(os.path.join(self.entries, cid, "character.edt"), "rb") as f:
            return f.read()

    # -- the index: one small file the list reads instead of every entry's description
    @staticmethod
    def _summary(cid, m):
        src = m.get("source") or {}
        vox = m.get("voxels") or []
        pv = m.get("preview") or {}
        return {"id": cid, "label": m.get("label") or m.get("name") or "Character", "name": m.get("name", ""),
                "class": m.get("class", ""), "gender": m.get("gender", 0), "creator": m.get("creator") or "",
                "tags": m.get("tags") or [], "source": source_kind(src), "imported": m.get("imported", ""),
                "voxels": sum(vox) if isinstance(vox, list) else 0, "poses": len(vox) if isinstance(vox, list) else 0,
                "sha256": m.get("sha256", ""), "size": m.get("size", 0), "preview_ok": bool(pv.get("ok")),
                "preview_current": pv.get("renderer") == char_preview.RENDERER and pv.get("sha256") == m.get("sha256"),
                "installed_folder": m.get("installed_folder"), "original_sha256": m.get("original_edt_sha256"),
                "description": m.get("description") or "", "shape": m.get("shape_sha256", "")}

    def index(self):
        if self._index is not None:
            return self._index
        ids = set(self.ids())
        try:
            with open(self.index_path, encoding="utf-8") as f:
                idx = json.load(f)
            if idx.get("version") == INDEX_VERSION and set(idx.get("entries", {})) == ids:
                self._index = idx["entries"]
                return self._index
        except (OSError, ValueError, AttributeError):
            pass
        entries = {}
        for cid in sorted(ids):   # rebuilt from the entries: one unreadable description never hides the rest
            try:
                entries[cid] = self._summary(cid, self.meta(cid))
            except (OSError, ValueError) as e:
                self.log("warning", f"characters: entry {cid}: its description is unreadable ({type(e).__name__}); "
                                    "left out of the list")
        self._index = entries
        self._save_index()
        return entries

    def _save_index(self):
        try:
            _write_atomic(self.index_path, json.dumps({"version": INDEX_VERSION, "entries": self._index},
                                                      ensure_ascii=False).encode("utf-8"))
        except OSError as e:
            self.log("warning", f"characters: the library's index could not be written ({e.strerror}); read again next time")

    def _index_put(self, cid, m):
        self.index()[cid] = self._summary(cid, m)
        self._save_index()

    def _index_drop(self, cid):
        if self.index().pop(cid, None) is not None:
            self._save_index()

    def search(self, text=""):
        t = text.casefold()
        out = []
        for cid, e in sorted(self.index().items()):
            hay = " ".join([e["label"], e["name"], e["class"], e["creator"], " ".join(e["tags"])]).casefold()
            if t in hay:
                out.append((cid, e))
        return out

    # -- previews: made again only when missing, from other data, or by another version of the renderer
    def preview_path(self, cid, which):
        return os.path.join(self.entries, cid, "preview", which)

    def ensure_previews(self, cid, force=False):
        """(ok, made): the entry's previews current; made again when needed (or forced: a corrupted cache)."""
        m = self.meta(cid)
        pv = m.get("preview") or {}
        missing = any(not os.path.isfile(self.preview_path(cid, f)) for f in PREVIEW_FILES)
        if not force and not missing and pv.get("renderer") == char_preview.RENDERER and pv.get("sha256") == m.get("sha256"):
            return bool(pv.get("ok")), False
        edt = self.edt(cid)
        if _sha(edt) != m.get("sha256"):
            self.log("warning", f"characters: entry {cid}: its character data changed on disk; previews show a placeholder")
            edt = b""
        files, ok = char_preview.make_all(edt)
        os.makedirs(os.path.join(self.entries, cid, "preview"), exist_ok=True)
        for name, b in files.items():
            _write_atomic(self.preview_path(cid, name), b)
        m["preview"] = {"renderer": char_preview.RENDERER, "sha256": m.get("sha256"), "ok": ok}
        self._write_meta(cid, m)
        return ok, True

    # -- importing
    def import_candidates(self, cands, key=None, program=None, progress=None, cancelled=None):
        """[{label, status, id?, message}] -- status: imported, duplicate, similar (imported; another entry has the
        same model), needs-key, wrong-key, damaged, unsupported, failed (a file error), cancelled. Each character is
        added whole or not at all; nothing of the sources is ever changed."""
        results = []
        shapes = {}
        for cid, e in self.index().items():
            shapes.setdefault(e.get("shape"), []).append(cid)
        sid_cache = bytes.fromhex(self.state["secure_file_id"]) if self.state.get("secure_file_id") else None
        key_ok = None   # the key checked once, on the first encrypted character, before any is decrypted
        n = len(cands)
        for i, c in enumerate(cands):
            if cancelled and cancelled():
                results += [{"label": x.label, "status": "cancelled", "message": "not imported: the import was stopped"}
                            for x in cands[i:]]
                break
            if progress:
                progress(i, n, c.label)
            try:
                edt = c.edt
                kind, info = ps3_save.classify(edt, c.pfd)
                if kind == "damaged":
                    results.append({"label": c.label, "status": "damaged", "message": info})
                    continue
                decrypted = False
                if kind == "encrypted":
                    if key is None:
                        results.append({"label": c.label, "status": "needs-key",
                                        "message": "saved on a PS3 and still encrypted: importing it needs your PS3 "
                                                   "syscon manager key"})
                        continue
                    if program is None:
                        results.append({"label": c.label, "status": "failed",
                                        "message": "decrypting needs the installed game's program (EBOOT.elf): "
                                                   "install the game first"})
                        continue
                    sid = ps3_save.find_secure_id(program, edt, info, sid_cache)
                    if sid is None:
                        results.append({"label": c.label, "status": "unsupported",
                                        "message": "this encrypted save is not 3D Dot Game Heroes' (US) character data"})
                        continue
                    if sid != sid_cache:
                        sid_cache = sid
                        self.state["secure_file_id"] = sid.hex()
                        self._save_state()
                    if key_ok is None:
                        key_ok = ps3_save.check_console_key(key, sid, edt, info)
                    if not key_ok:
                        results.append({"label": c.label, "status": "wrong-key",
                                        "message": "the PS3 key does not decrypt this character (a wrong key)"})
                        continue
                    try:
                        original = edt
                        edt = ps3_save.decrypt_character(key, sid, edt, info)
                    except ps3_save.SaveError as e:
                        results.append({"label": c.label, "status": "damaged", "message": str(e)})
                        continue
                    decrypted = True
                    c.original_sha = _sha(original)
                results.append(self._add(c, edt, decrypted, shapes))
            except OSError as e:
                full = getattr(e, "errno", None) == 28
                results.append({"label": c.label, "status": "failed",
                                "message": "the drive is full" if full else f"a file couldn't be written ({e.strerror or e})"})
                self.log("warning", f"characters: importing {c.label} failed: {type(e).__name__} {getattr(e, 'errno', '')}")
        if progress:
            progress(n, n, "")
        return results

    def _add(self, cand, edt, decrypted, shapes):
        ch = edt_format.parse(edt)
        sha = _sha(edt)
        cid = sha[:16]
        final = os.path.join(self.entries, cid)
        if os.path.exists(os.path.join(final, "character.json")):
            return {"label": cand.label, "status": "duplicate", "id": cid,
                    "message": f"already in the library as \"{self.index().get(cid, {}).get('label', cid)}\" (the same character)"}
        shape = edt_format.shape_hash(ch)
        info = edt_format.describe(ch)
        src_meta = cand.meta or {}
        m = {
            "format": FORMAT, "format_version": FORMAT_VERSION,
            "id": cid, "sha256": sha, "shape_sha256": shape, "size": len(edt),
            "name": info["name"], "name_stored": ch.name, "class": info["class"], "gender": ch.gender,
            "voxels": info["voxels"],
            "label": (src_meta.get("label") or info["name"] or "Character")[:64],
            "creator": src_meta.get("creator"), "description": src_meta.get("description"),
            "tags": [t for t in src_meta.get("tags", []) if isinstance(t, str)][:20],
            "source": src_meta.get("source") or {"kind": cand.source["kind"], "imported_from": cand.label,
                                                  "decrypted_from_ps3_save": decrypted},
            "imported": _now(), "importer": IMPORTER,
            "compatibility": {"game": "3D Dot Game Heroes (BLUS-30490)", "edit_data": "EDTFILE1.9"},
        }
        if decrypted and getattr(cand, "original_sha", None):
            m["original_edt_sha256"] = cand.original_sha
        files, ok = char_preview.make_all(edt)
        m["preview"] = {"renderer": char_preview.RENDERER, "sha256": sha, "ok": ok}
        tmp = tempfile.mkdtemp(prefix=".import-", dir=self.entries)
        try:
            os.makedirs(os.path.join(tmp, "preview"))
            with open(os.path.join(tmp, "character.edt"), "wb") as f:
                f.write(edt)
            for name, b in files.items():
                with open(os.path.join(tmp, "preview", name), "wb") as f:
                    f.write(b)
            with open(os.path.join(tmp, "character.json"), "w", encoding="utf-8", newline="\n") as f:
                json.dump(m, f, indent=1, ensure_ascii=False)
            os.replace(tmp, final)
        except Exception:
            shutil.rmtree(tmp, ignore_errors=True)
            raise
        self._index_put(cid, m)
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
                    "message": f"imported; the same model as \"{self.index().get(others[0], {}).get('label', others[0])}\" "
                               "(another name or settings)"}
        return {"label": cand.label, "status": "imported", "id": cid, "message": f"imported \"{m['label']}\""}

    # -- managing
    def rename(self, cid, label):
        m = self.meta(cid)
        m["label"] = " ".join(label.split())[:64] or m["label"]
        self._write_meta(cid, m)

    def delete(self, cid):
        """Remove the entry (and its originals) from the library. The game's saves are not touched: a copy installed
        in the game stays there."""
        for d in (os.path.join(self.entries, cid), os.path.join(self.originals, cid)):
            if os.path.isdir(d):
                shutil.rmtree(d)
        self._index_drop(cid)

    # -- exporting
    def export(self, cid):
        """The .chchar bytes of an entry: the character and its description only."""
        m = self.meta(cid)
        edt = self.edt(cid)
        if _sha(edt) != m["sha256"]:
            raise LibraryError(f"{m.get('label')}: its character data changed on disk; not exported")
        self.ensure_previews(cid)
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
            for name in ("character.json", "character.edt", "preview/thumb.png", "preview/views.png"):
                if name == "character.json":
                    b = json.dumps(out, indent=1, ensure_ascii=False).encode("utf-8")
                elif name == "character.edt":
                    b = edt
                else:
                    with open(self.preview_path(cid, name.split("/")[1]), "rb") as f:
                        b = f.read()
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
            try:
                edt, pfd = ps3_save.scan_folder(os.path.join(savedata, d))
            except OSError:
                unusable.append((d, "unreadable"))
                continue
            kind, info = ps3_save.classify(edt, pfd)
            if kind == "plain":
                found[_sha(edt)] = d
            else:
                unusable.append((d, "saved on a PS3 and still encrypted: import it through the character library"
                                 if kind == "encrypted" else info))
        return found, unusable

    def scan_saves(self, savedata):
        """The game's edit-data folders the library could take in: [{folder, name, kind}] -- kind "encrypted" (copied
        from a PS3, not imported yet) or "plain" (made in the game, not in the library yet)."""
        out = []
        if not os.path.isdir(savedata):
            return out
        have = {e["sha256"] for e in self.index().values()}
        imported_originals = {e.get("original_sha256") for e in self.index().values() if e.get("original_sha256")}
        for d in sorted(os.listdir(savedata)):
            if not d.startswith(EDIT_PREFIX):
                continue
            p = os.path.join(savedata, d)
            try:
                edt, pfd = ps3_save.scan_folder(p)
                sfo_p = os.path.join(p, "PARAM.SFO")
                name = sfo_detail_name(open(sfo_p, "rb").read()) if os.path.isfile(sfo_p) else ""
            except OSError:
                continue
            kind, _info = ps3_save.classify(edt, pfd)
            sha = _sha(edt)
            if kind == "encrypted" and sha not in imported_originals:
                out.append({"folder": d, "name": name, "kind": "encrypted"})
            elif kind == "plain" and sha not in have:
                out.append({"folder": d, "name": name, "kind": "plain"})
        return out

    def install(self, cid, savedata):
        """Put the entry into the game's saves as a new edit-data folder; never replaces one. (folder, new)."""
        m = self.meta(cid)
        edt = self.edt(cid)
        have, _ = self.installed(savedata)
        if m["sha256"] in have:
            return have[m["sha256"]], False
        self.ensure_previews(cid)
        folder = EDIT_PREFIX + m["sha256"][:20].upper()
        dst = os.path.join(savedata, folder)
        if os.path.exists(dst):
            raise LibraryError(f"{folder} already exists in the game's saves with other content; not replaced")
        os.makedirs(savedata, exist_ok=True)
        tmp = tempfile.mkdtemp(prefix=".install-", dir=savedata)
        try:
            with open(self.preview_path(cid, "thumb.png"), "rb") as f:
                icon = f.read()
            for name, b in (("DATA.EDT", edt), ("PARAM.SFO", make_sfo(folder, m["name_stored"])),
                            ("ICON0.PNG", icon),
                            ("PARAM.PFD", b"DATA.EDT\n")):   # Cubic.Heroes' own list of the protected files
                with open(os.path.join(tmp, name), "wb") as f:
                    f.write(b)
            os.replace(tmp, dst)
        except Exception:
            shutil.rmtree(tmp, ignore_errors=True)
            raise
        m["installed_folder"] = folder
        m["installed_at"] = _now()
        self._write_meta(cid, m)
        return folder, True

    def uninstall(self, cid, savedata):
        """Remove from the game's saves the folder the library installed for this entry -- only that folder, only
        when it still holds exactly what was installed. The library entry stays. Returns the folder removed."""
        m = self.meta(cid)
        folder = m.get("installed_folder")
        if not folder:
            raise LibraryError("this character was not installed by the library; its copy in the game's saves is the "
                               "game's own and is left as it is")
        p = os.path.join(savedata, folder)
        if not os.path.isdir(p):
            m.pop("installed_folder", None)
            self._write_meta(cid, m)
            return None
        names = set(os.listdir(p))
        with open(os.path.join(p, "DATA.EDT"), "rb") as f:
            same = _sha(f.read()) == m["sha256"]
        if not folder.startswith(EDIT_PREFIX) or not names <= INSTALL_FILES or not same:
            raise LibraryError("the copy in the game's saves changed since it was installed (edited in the game?): it is "
                               "left as it is -- import it first to keep the changes")
        gone = os.path.join(savedata, ".uninstall-" + folder)
        os.replace(p, gone)   # out of the game's sight at once, then removed
        shutil.rmtree(gone, ignore_errors=True)
        m.pop("installed_folder", None)
        m.pop("installed_at", None)
        self._write_meta(cid, m)
        return folder


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        sys.exit(__doc__)
    lib = Library(a[0])
    cmd, rest = a[1], a[2:]
    if cmd == "list":
        for cid, m in lib.search(""):
            print(f"{cid}  {m['label']}  ({m['class']}, {m['voxels']} voxels)  {m.get('creator') or ''}")
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
