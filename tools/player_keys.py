#!/usr/bin/env python3
"""The player's keys for the game's program, read in memory only (docs/INSTALLING.md). None of these keys is part of
this repository or of anything it distributes: each player supplies their own, and nothing here writes, logs, prints
or puts a key in an error's text -- errors name a file, a line number and what kind of line it is, never a value.

The disc's program (EBOOT.BIN) needs an APP key (erk and riv). A key source is any of:

- a text keys file, with lines of this project's format (hexadecimal; '#' starts a comment):
      APP <revision> <erk: 64 hex> <riv: 32 hex>
  or of sections in the [name] / name=value form other PS3 tools use (erk=, riv=, revision=, type= or self_type= APP);
- a folder of binary key files, one key per file, as PS3 tools keep them (app-key-<x> with app-iv-<x>): the folder
  itself and its own subfolders, one level down;
- a ZIP archive holding either, read in memory (nothing of it is written anywhere).

Lines and files of other kinds are ignored, so a source made for another tool works as it is. A key need not be
labelled with its revision: the decryptor tries every one until the program's checksum matches.
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


def _hex(field, size, what, n):
    if len(field) != 2 * size or not _HEX.match(field):
        raise KeysError(f"Line {n} of the keys file ({what}) isn't {2 * size} hexadecimal digits.", n)
    return bytes.fromhex(field)


class Keyring:
    """app: {revision: (erk, riv)} for labelled keys, app_any: [(erk, riv)] unlabelled ones"""

    def __init__(self):
        self.app, self.app_any = {}, []

    def __repr__(self):   # never the values
        return f"Keyring(app={sorted(self.app)}+{len(self.app_any)})"

    def __bool__(self):
        return bool(self.app or self.app_any)

    def candidates(self, kind, revision):
        """the (erk, riv) pairs to try for a program of kind 'app' and this revision: the one labelled with it
        first, then every other (a wrong one is refused by the metadata check), each once"""
        if kind != "app":
            return []
        seen, out = set(), []
        for pair in ([self.app[revision]] if revision in self.app else []) + list(self.app.values()) + self.app_any:
            if pair not in seen:
                seen.add(pair)
                out.append(pair)
        return out

    def merge(self, other):
        for r, p in other.app.items():
            self.app.setdefault(r, p)
        self.app_any += [p for p in other.app_any if p not in self.app_any]
        return self


# ---------------- text ----------------

def parse(text):
    """a text keys file -> Keyring: this project's APP lines and [name] / name=value sections, in any mix. Raises
    KeysError for an APP line that isn't well formed."""
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
        if f[0].upper() == "APP":
            if len(f) != 4:
                raise KeysError(f"Line {n} of the keys file (APP) needs a revision, an erk and a riv.", n)
            try:
                rev = int(f[1], 16)
            except ValueError:
                raise KeysError(f"Line {n} of the keys file (APP) has a revision that isn't hexadecimal.", n) \
                    from None
            k.app[rev] = (_hex(f[2], 32, "APP", n), _hex(f[3], 16, "APP", n))
    if section is not None:
        sections.append((section, fields))
    for name, fields in sections:
        _section(k, name, fields)
    return k


def _hexok(v, size):
    return v is not None and len(v) == 2 * size and _HEX.match(v)


def _section(k, name, f):
    """one [name] section of another tool's keys file: an APP key set is taken, anything else ignored"""
    label = re.sub(r"[^a-z0-9]", "_", name.lower())
    kind = (f.get("self_type") or f.get("type") or "").upper()
    erk, riv = f.get("erk"), f.get("riv")
    if not (_hexok(erk, 32) and _hexok(riv, 16)) or not (kind == "APP" or label.startswith("app")):
        return
    pair = (bytes.fromhex(erk), bytes.fromhex(riv))
    rev = f.get("revision")
    try:
        r = int(rev, 16) if rev is not None else None
    except ValueError:
        r = None
    if r is None:
        k.app_any.append(pair)
    else:
        k.app.setdefault(r, pair)


# ---------------- binary key files (one key per file) ----------------

def from_files(files):
    """{file name: bytes} of one folder -> Keyring: app-key-<x> with app-iv-<x> (32 + 16 bytes); others ignored"""
    k = Keyring()
    low = {name.lower(): data for name, data in files.items()}
    for name, data in sorted(low.items()):
        m = re.match(r"app[-_]key[-_](.+)\Z", name)
        if m and len(data) == 32:
            iv = low.get(f"app-iv-{m.group(1)}", low.get(f"app_iv_{m.group(1)}"))
            if iv is not None and len(iv) == 16:
                k.app_any.append((data, iv))
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

KEY_FILE = re.compile(r"(?i)^[a-z0-9]+[-_](key|iv|pub|priv|ctype)[-_].+$|^gpkg[-_]key$")


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
