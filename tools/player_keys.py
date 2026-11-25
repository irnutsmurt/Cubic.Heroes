#!/usr/bin/env python3
"""The player's keys, read in memory only (docs/INSTALLING.md). None of these keys is part of this repository or of
anything it distributes: each player supplies their own, and nothing here writes, logs, prints or puts a key in an
error's text -- errors name a file, a line number and what kind of line it is, never a value.

The disc's program (EBOOT.BIN) needs an APP key (erk and riv); the official v1.01 update needs the PS3's package key
for its .pkg, and an NPDRM key with the free licence's KLIC_FREE / KLIC_KEY pair for its program. A key source is any
of:

- a text keys file, with lines of this project's format (hexadecimal; '#' starts a comment):
      APP   <revision> <erk: 64 hex> <riv: 32 hex>   the disc's game program (EBOOT.BIN)
      NPDRM <revision> <erk: 64 hex> <riv: 32 hex>   a downloaded game program (the official update's EBOOT.BIN)
      KLIC_FREE <32 hex>  KLIC_KEY <32 hex>          the free licence's key and the key that unwraps it
      PKG   <32 hex>                                 the PS3's retail package key (the official update's .pkg)
  (NP_KLIC_FREE / NP_KLIC_KEY / PKG_KEY read the same), or of sections in the [name] / name=value form other PS3
  tools use (erk=, riv=, revision=, type= or self_type= APP / NPDRM; a single key as key= or erk=);
- a folder of binary key files, one key per file, as PS3 tools keep them (app-key-<x> with app-iv-<x>, npdrm-key-<x>
  with npdrm-iv-<x>, gpkg-key, klic-free, klic-key): the folder itself and its own subfolders, one level down;
- a ZIP archive holding either, read in memory (nothing of it is written anywhere).

Lines and files of other kinds are ignored, so a source made for another tool works as it is. A key need not be
labelled with its revision: the decryptor tries every one of its kind until the program's checksum matches.
"""
import os
import re
import zipfile

MAX_FILE = 64 << 10          # a keys file or a key file (keys are a few dozen bytes; a big file isn't one)
MAX_ZIP = 16 << 20           # a ZIP's total size read
MAX_MEMBERS = 4096
TEXT_NAMES = re.compile(r"(?i)(^|[._-])keys?([._-]|$)|\.keys$|\.txt$|\.ini$|\.cfg$|^keys$")


class KeysError(ValueError):
    """a keys file that can't be read as one: line is the 1-based line number"""

    def __init__(self, message, line=0):
        super().__init__(message)
        self.line = line


_HEX = re.compile(r"[0-9A-Fa-f]+\Z")
_SINGLE = {"PKG": "pkg", "PKG_KEY": "pkg", "KLIC_FREE": "klic_free", "NP_KLIC_FREE": "klic_free",
           "KLIC_KEY": "klic_key", "NP_KLIC_KEY": "klic_key"}
_SETS = {"APP": "app", "NPDRM": "npdrm"}


def _hex(field, size, what, n):
    if len(field) != 2 * size or not _HEX.match(field):
        raise KeysError(f"Line {n} of the keys file ({what}) isn't {2 * size} hexadecimal digits.", n)
    return bytes.fromhex(field)


class Keyring:
    """app / npdrm: {revision: (erk, riv)} for labelled keys, app_any / npdrm_any: [(erk, riv)] unlabelled ones;
    pkg, klic_free, klic_key: 16 bytes or None"""

    def __init__(self):
        self.app, self.npdrm = {}, {}
        self.app_any, self.npdrm_any = [], []
        self.pkg = self.klic_free = self.klic_key = None

    def __repr__(self):   # never the values
        return (f"Keyring(app={sorted(self.app)}+{len(self.app_any)}, npdrm={sorted(self.npdrm)}+{len(self.npdrm_any)}, "
                f"pkg={self.pkg is not None}, klic_free={self.klic_free is not None}, "
                f"klic_key={self.klic_key is not None})")

    def __bool__(self):
        return bool(self.app or self.npdrm or self.app_any or self.npdrm_any or self.pkg or self.klic_free or
                    self.klic_key)

    def candidates(self, kind, revision):
        """the (erk, riv) pairs to try for a program of kind 'app' or 'npdrm' and this revision: the one labelled
        with it first, then every other of that kind (a wrong one is refused by the metadata check), each once"""
        if kind not in ("app", "npdrm"):
            return []
        lab, any_ = (self.app, self.app_any) if kind == "app" else (self.npdrm, self.npdrm_any)
        seen, out = set(), []
        for pair in ([lab[revision]] if revision in lab else []) + list(lab.values()) + any_:
            if pair not in seen:
                seen.add(pair)
                out.append(pair)
        return out

    def has_update_keys(self):
        """whether this source can open the official update: the package key, and the free-licence pair"""
        return bool(self.pkg and self.klic_free and self.klic_key)

    def merge(self, other):
        for r, p in other.app.items():
            self.app.setdefault(r, p)
        for r, p in other.npdrm.items():
            self.npdrm.setdefault(r, p)
        self.app_any += [p for p in other.app_any if p not in self.app_any]
        self.npdrm_any += [p for p in other.npdrm_any if p not in self.npdrm_any]
        self.pkg = self.pkg or other.pkg
        self.klic_free = self.klic_free or other.klic_free
        self.klic_key = self.klic_key or other.klic_key
        return self


# ---------------- text ----------------

def parse(text):
    """a text keys file -> Keyring: this project's lines and [name] / name=value sections, in any mix. Raises
    KeysError for a line of this project's kinds that isn't well formed."""
    k = Keyring()
    section, fields, sections = None, {}, []
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line or line.startswith(";"):
            continue
        m = re.match(r"\[(.+)\]\Z", line)
        if m:
            if section is not None:
                sections.append((section, fields))
            section, fields = m.group(1).strip(), {}
            continue
        if section is not None and "=" in line:
            name, _, value = line.partition("=")
            fields[name.strip().lower()] = value.strip()
            continue
        f = line.split()
        kind = f[0].upper()
        if kind in _SETS:
            if len(f) != 4:
                raise KeysError(f"Line {n} of the keys file ({kind}) needs a revision, an erk and a riv.", n)
            try:
                rev = int(f[1], 16)
            except ValueError:
                raise KeysError(f"Line {n} of the keys file ({kind}) has a revision that isn't hexadecimal.", n) \
                    from None
            getattr(k, _SETS[kind])[rev] = (_hex(f[2], 32, kind, n), _hex(f[3], 16, kind, n))
        elif kind in _SINGLE:
            if len(f) != 2:
                raise KeysError(f"Line {n} of the keys file ({kind}) needs one key.", n)
            setattr(k, _SINGLE[kind], _hex(f[1], 16, kind, n))
    if section is not None:
        sections.append((section, fields))
    for name, fields in sections:
        _section(k, name, fields)
    return k


def _hexok(v, size):
    return v is not None and len(v) == 2 * size and _HEX.match(v)


def _section(k, name, f):
    """one [name] section of another tool's keys file: an APP or NPDRM key set, or a single package / free-licence
    key; anything else ignored"""
    label = re.sub(r"[^a-z0-9]", "_", name.lower())
    kind = (f.get("self_type") or f.get("type") or "").upper()
    erk, riv = f.get("erk"), f.get("riv")
    if _hexok(erk, 32) and _hexok(riv, 16):
        which = ("app" if (kind == "APP" or label.startswith("app")) else
                 "npdrm" if (kind == "NPDRM" or label.startswith("npdrm")) else None)
        if not which:
            return
        pair = (bytes.fromhex(erk), bytes.fromhex(riv))
        rev = f.get("revision")
        try:
            r = int(rev, 16) if rev is not None else None
        except ValueError:
            r = None
        if r is None:
            getattr(k, which + "_any").append(pair)
        else:
            getattr(k, which).setdefault(r, pair)
        return
    single = f.get("key") or (erk if _hexok(erk, 16) else None)
    if not _hexok(single, 16):
        return
    if "klic_free" in label or label == "np_klicensee_free":
        k.klic_free = k.klic_free or bytes.fromhex(single)
    elif "klic_key" in label:
        k.klic_key = k.klic_key or bytes.fromhex(single)
    elif "gpkg" in label or label in ("pkg", "pkg_key", "pkg_aes_key"):
        k.pkg = k.pkg or bytes.fromhex(single)


# ---------------- binary key files (one key per file) ----------------

def from_files(files):
    """{file name: bytes} of one folder -> Keyring: <type>-key-<x> with <type>-iv-<x> for types app and npdrm
    (32 + 16 bytes), gpkg-key (16), klic-free / klic-key (16); other files ignored"""
    k = Keyring()
    low = {name.lower(): data for name, data in files.items()}
    for name, data in sorted(low.items()):
        m = re.match(r"(app|npdrm)[-_]key[-_](.+)\Z", name)
        if m and len(data) == 32:
            iv = low.get(f"{m.group(1)}-iv-{m.group(2)}", low.get(f"{m.group(1)}_iv_{m.group(2)}"))
            if iv is not None and len(iv) == 16:
                getattr(k, m.group(1) + "_any").append((data, iv))
    for names, attr in ((("gpkg-key", "gpkg_key", "pkg-aes-key"), "pkg"),
                        (("klic-free", "klic_free", "np-klic-free", "np_klic_free"), "klic_free"),
                        (("klic-key", "klic_key", "np-klic-key", "np_klic_key"), "klic_key")):
        for n in names:
            if len(low.get(n, b"")) == 16:
                setattr(k, attr, low[n])
                break
    return k


def _text_or_none(data):
    try:
        return data.decode("utf-16") if data[:2] in (b"\xff\xfe", b"\xfe\xff") else data.decode("utf-8-sig")
    except UnicodeDecodeError:
        return None


def _merge_text(k, files):
    for n, data in files.items():
        text = _text_or_none(data) if TEXT_NAMES.search(n) else None
        if text is not None:
            try:
                k.merge(parse(text))
            except KeysError:
                pass


def load_folder(path):
    """a folder of key files (and its own subfolders, one level down): binary key files, and any text keys file"""
    k = Keyring()
    dirs = [path] + sorted(os.path.join(path, d) for d in os.listdir(path) if os.path.isdir(os.path.join(path, d)))
    for d in dirs:
        files = {}
        for n in os.listdir(d):
            p = os.path.join(d, n)
            if os.path.isfile(p) and os.path.getsize(p) <= MAX_FILE:
                with open(p, "rb") as f:
                    files[n] = f.read()
        k.merge(from_files(files))
        _merge_text(k, files)
    return k


def load_key_set(path):
    """one file of a folder of binary key files, chosen: the key files beside it (only files named as key files, in
    that folder alone -- nothing else of the folder is read)"""
    d = os.path.dirname(os.path.abspath(path))
    files = {}
    for n in os.listdir(d):
        p = os.path.join(d, n)
        if KEY_FILE.match(n) and os.path.isfile(p) and os.path.getsize(p) <= MAX_FILE:
            with open(p, "rb") as f:
                files[n] = f.read()
    return from_files(files)


def load_zip(path):
    """a ZIP archive of key files or a keys file, read in memory; members over the size limits are skipped"""
    k = Keyring()
    try:
        z = zipfile.ZipFile(path)
    except (zipfile.BadZipFile, OSError) as e:
        raise KeysError(f"That ZIP archive can't be read ({type(e).__name__}).") from None
    with z:
        groups, total = {}, 0
        for i in z.infolist()[:MAX_MEMBERS]:
            if i.is_dir() or i.file_size > MAX_FILE or total + i.file_size > MAX_ZIP:
                continue
            total += i.file_size
            folder, _, name = i.filename.replace("\\", "/").rpartition("/")
            try:
                groups.setdefault(folder, {})[name] = z.read(i)
            except (zipfile.BadZipFile, OSError, RuntimeError, NotImplementedError):
                continue   # an encrypted or damaged member
    for files in groups.values():
        k.merge(from_files(files))
        _merge_text(k, files)
    return k


# ---------------- any source ----------------

KEY_FILE = re.compile(r"(?i)^[a-z0-9]+[-_](key|iv|pub|priv|ctype)[-_].+$|^gpkg[-_]key$|^(np[-_])?klic[-_](free|key)$")


def is_key_file(path):
    """a single file of a folder of binary key files (app-key-315, app-iv-315, ...): its folder is the source"""
    return KEY_FILE.match(os.path.basename(path)) is not None


def kind_of(path):
    """what a path the player chose is: 'elf' (a decrypted program), 'zip', 'folder', 'rpcs3' (an RPCS3 program),
    'text' (text or a small file that may be one), 'keyfile' (one file of a folder of binary key files), 'self' (a
    program still encrypted -- the disc's EBOOT.BIN), 'pkg' (a PS3 package), 'binary' (anything else), or None"""
    if os.path.isdir(path):
        return "folder"
    if not os.path.isfile(path):
        return None
    with open(path, "rb") as f:
        head = f.read(4)
    base = os.path.basename(path).lower()
    if head == b"\x7fELF" and "rpcs3" in base:
        return "rpcs3"
    if head == b"\x7fELF":
        return "elf"
    if head[:2] == b"MZ":
        return "rpcs3" if "rpcs3" in base else "binary"
    if head == b"PK\x03\x04":
        return "zip"
    if head == b"SCE\0":
        return "self"
    if head == b"\x7fPKG":
        return "pkg"
    if is_key_file(path) and os.path.getsize(path) <= MAX_FILE:
        return "keyfile"
    if os.path.getsize(path) <= MAX_FILE:
        return "text"
    return "binary"


def read_text(path):
    """a text keys file's text: UTF-8 (with or without a BOM), or UTF-16 with its BOM (as some editors save "Unicode"
    text). UnicodeDecodeError for anything else."""
    with open(path, "rb") as f:
        data = f.read(MAX_FILE + 1)
    if data[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return data.decode("utf-16")
    return data.decode("utf-8-sig")


def load(path):
    """the keys of a text keys file, a key folder (or one of its files) or a ZIP -> Keyring (OSError,
    UnicodeDecodeError or KeysError)"""
    kind = kind_of(path)
    if kind == "folder":
        return load_folder(path)
    if kind == "keyfile":
        return load_key_set(path)
    if kind == "zip":
        return load_zip(path)
    return parse(read_text(path))
