#!/usr/bin/env python3
"""Read a PS3 package (.pkg): the official game update, as the player downloads it from Sony -- no other program.

The package's own layout and digests are read without any key: the header, its metadata, and the SHA-1 digest that
covers the whole file. Its contents (the item table, names and files) are encrypted with the PS3's retail package
key, which is NOT part of this repository: it comes from the player's keys file (a `PKG` line; tools/player_keys.py).
Nothing here writes, logs or puts a key in an error.

    pkg_extract.py info    PKG                          the header, its metadata, whether the file is intact
    pkg_extract.py list    PKG --keys KEYS              the items
    pkg_extract.py extract PKG --keys KEYS --out DIR    the files, into DIR (never outside it)
"""
import argparse
import hashlib
import os
import struct
import sys

from Crypto.Cipher import AES
from Crypto.Util import Counter

MAGIC = b"\x7fPKG"
HEADER = 0xC0
TRAILER = 0x20                 # SHA-1 of everything before it, then 12 zero bytes
ITEM = 0x20
RETAIL = 0x8000
TYPE_PS3 = 1
KIND_DIR = 4
CHUNK = 4 << 20
ITEMS_MAX = 100000
NAME_MAX = 1024
META_NAMES = {1: "drm_type", 2: "content_type", 3: "package_flags", 4: "package_size", 5: "package_version",
              6: "qa_digest", 7: "unknown_7", 8: "software_revision", 9: "unknown_9", 10: "install_directory",
              11: "unknown_11", 13: "unknown_13", 14: "file_count"}


class PkgError(Exception):
    """code: not_a_package, unsupported, damaged, wrong_key, unsafe_name, io"""

    def __init__(self, message, code):
        super().__init__(message)
        self.code = code


class Item:
    __slots__ = ("name", "offset", "size", "flags")

    def __init__(self, name, offset, size, flags):
        self.name, self.offset, self.size, self.flags = name, offset, size, flags

    @property
    def kind(self):
        return self.flags & 0xFF

    @property
    def is_dir(self):
        return self.kind == KIND_DIR

    def __repr__(self):
        return f"Item({self.name!r}, {self.size}, kind={self.kind:#x})"


def safe_name(name):
    """an item's name as a relative path inside the output folder, or None: no absolute path, drive, '..' or '.',
    no backslash or control character, no empty part"""
    if not name or len(name) > NAME_MAX or "\\" in name or ":" in name or name.startswith("/"):
        return None
    parts = name.split("/")
    if any(p in ("", ".", "..") or any(ord(c) < 0x20 for c in p) for p in parts):
        return None
    return parts


class Package:
    def __init__(self, path):
        self.path = path
        try:
            self.file_size = os.path.getsize(path)
            with open(path, "rb") as f:
                h = f.read(HEADER)
        except OSError as e:
            raise PkgError(f"Can't read the package ({e.strerror}).", "io") from None
        if len(h) < HEADER or h[:4] != MAGIC:
            raise PkgError("That file isn't a PS3 package.", "not_a_package")
        (self.revision, self.type, self.meta_offset, self.meta_count, self.meta_size, self.item_count,
         self.total_size, self.data_offset, self.data_size) = struct.unpack_from(">HHIIIIQQQ", h, 4)
        self.content_id = h[0x30:0x54].split(b"\0", 1)[0].decode("ascii", "replace")
        self.qa_digest, self.riv = h[0x60:0x70], h[0x70:0x80]
        self.header = h
        if self.type != TYPE_PS3:
            raise PkgError("That package isn't for the PS3.", "unsupported")
        if self.revision != RETAIL:
            raise PkgError("That package is a development package, not a retail one.", "unsupported")
        if (self.total_size != self.file_size or self.data_offset < HEADER or
                self.data_offset + self.data_size + TRAILER > self.total_size or
                not 0 < self.item_count <= ITEMS_MAX or self.item_count * ITEM > self.data_size):
            raise PkgError("The package is incomplete or damaged.", "damaged")
        self.metadata = self._metadata()

    def _metadata(self):
        out = {}
        try:
            with open(self.path, "rb") as f:
                f.seek(self.meta_offset)
                raw = f.read(min(self.meta_size or 0x400, 0x10000) + 0x100)
        except OSError:
            return out
        off = 0
        for _ in range(min(self.meta_count, 64)):
            if off + 8 > len(raw):
                break
            pid, size = struct.unpack_from(">II", raw, off)
            body = raw[off + 8:off + 8 + size]
            out[META_NAMES.get(pid, f"packet_{pid}")] = body.hex()
            off += 8 + size
        return out

    def info(self):
        return {"content_id": self.content_id, "revision": f"{self.revision:#06x}", "items": self.item_count,
                "size": self.total_size, "data": [self.data_offset, self.data_size], "metadata": self.metadata}

    def verify(self, progress=None):
        """whether the file is the one its maker sealed: the header's own digest, and the SHA-1 at its end over
        everything before it. No key is needed. Raises PkgError(damaged) otherwise."""
        if hashlib.sha1(self.header[:0x80]).digest()[-8:] != self.header[0xB8:0xC0]:
            raise PkgError("The package's header is damaged.", "damaged")
        h, done, end = hashlib.sha1(), 0, self.total_size - TRAILER
        with open(self.path, "rb") as f:
            while done < end:
                b = f.read(min(CHUNK, end - done))
                if not b:
                    break
                h.update(b)
                done += len(b)
                if progress:
                    progress(done, end)
            tail = f.read(TRAILER)
        if done != end or h.digest() != tail[:20]:
            raise PkgError("The package is incomplete or damaged: its digest doesn't match.", "damaged")

    # ---- the encrypted part ----

    def _decrypt(self, f, key, offset, size):
        """size bytes of the data area from offset (relative to the data area), decrypted"""
        start = offset - offset % 16
        f.seek(self.data_offset + start)
        raw = f.read(size + (offset - start))
        if len(raw) != size + (offset - start):
            raise PkgError("The package is incomplete or damaged.", "damaged")
        ctr = Counter.new(128, initial_value=(int.from_bytes(self.riv, "big") + start // 16) % (1 << 128))
        return AES.new(key, AES.MODE_CTR, counter=ctr).decrypt(raw)[offset - start:]

    def items(self, key):
        """the item table, decrypted with the package key; a wrong key reads as nonsense and is refused"""
        if not key or len(key) != 16:
            raise PkgError("The keys file has no package key (a PKG line).", "wrong_key")
        out = []
        with open(self.path, "rb") as f:
            table = self._decrypt(f, key, 0, self.item_count * ITEM)
            for i in range(self.item_count):
                noff, nsize, doff, dsize, flags, _ = struct.unpack_from(">IIQQII", table, i * ITEM)
                if (nsize == 0 or nsize > NAME_MAX or noff + nsize > self.data_size or
                        doff + dsize > self.data_size):
                    raise PkgError("The package key doesn't open this package.", "wrong_key")
                raw = self._decrypt(f, key, noff, nsize).rstrip(b"\0")
                try:
                    name = raw.decode("ascii")
                except UnicodeDecodeError:
                    raise PkgError("The package key doesn't open this package.", "wrong_key") from None
                if safe_name(name) is None:
                    raise PkgError(f"The package names a file outside its own folder (item {i}).", "unsafe_name")
                out.append(Item(name, doff, dsize, flags))
        return out

    def read(self, key, item, sink, progress=None):
        """an item's bytes, decrypted, to sink(bytes) in chunks"""
        done = 0
        with open(self.path, "rb") as f:
            while done < item.size:
                n = min(CHUNK, item.size - done)
                sink(self._decrypt(f, key, item.offset + done, n))
                done += n
                if progress:
                    progress(done, item.size)

    def extract(self, key, out_dir, only=None, progress=None):
        """the files into out_dir (made if missing), each written as .part then renamed: [(name, size, sha256)].
        only: a name prefix to limit it to. progress(done_bytes, total_bytes)."""
        items = self.items(key)
        files = [i for i in items if not i.is_dir and (only is None or i.name.startswith(only))]
        total, done, out = sum(i.size for i in files) or 1, 0, []
        root = os.path.realpath(out_dir)
        for i in items:
            if i.is_dir and (only is None or i.name.startswith(only)):
                os.makedirs(os.path.join(root, *safe_name(i.name)), exist_ok=True)
        for i in files:
            dest = os.path.join(root, *safe_name(i.name))
            if os.path.commonpath([root, os.path.realpath(dest)]) != root:
                raise PkgError(f"The package names a file outside its own folder ({i.name}).", "unsafe_name")
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            h, base = hashlib.sha256(), done
            with open(dest + ".part", "wb") as w:
                def sink(b, w=w, h=h):
                    w.write(b)
                    h.update(b)
                self.read(key, i, sink, (lambda d, t, base=base: progress(base + d, total)) if progress else None)
            os.replace(dest + ".part", dest)
            done += i.size
            out.append((i.name, i.size, h.hexdigest()))
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("command", choices=["info", "list", "extract"])
    ap.add_argument("pkg")
    ap.add_argument("--keys", help="the player's keys file (its PKG line)")
    ap.add_argument("--out")
    ap.add_argument("--only")
    a = ap.parse_args()
    try:
        p = Package(a.pkg)
        if a.command == "info":
            import json
            p.verify()
            print(json.dumps(dict(p.info(), intact=True), indent=1))
            return 0
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import player_keys
        if not a.keys:
            ap.error("--keys is needed")
        key = player_keys.load(a.keys).pkg
        if a.command == "list":
            for i in p.items(key):
                print(f"{'d' if i.is_dir else '-'} {i.kind:#04x} {i.size:>12} {i.name}")
        else:
            if not a.out:
                ap.error("--out is needed")
            p.verify()
            for name, size, sha in p.extract(key, a.out, a.only):
                print(f"{sha} {size:>12} {name}")
    except PkgError as e:
        print(f"pkg_extract: {e}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
