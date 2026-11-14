#!/usr/bin/env python3
"""PS3 disc images: decrypted or encrypted, read the same way (M9: docs/M9_LAUNCHER_PLAN.md section 22).

An encrypted PS3 ISO keeps the file system readable and encrypts the rest, sector by sector: sector 0 lists the
plain regions (a count at 0, then each region's first and last sector, big-endian, at 8 + 8*i); every sector outside
them is AES-128-CBC encrypted with the disc's key, the IV the sector's number (big-endian, in the IV's last four
bytes). A decrypted ISO is the same image with those sectors decrypted in place.

    kind(f)                        "decrypted" or "encrypted" (or "unknown": not a PS3 disc image this can tell)
    DiscImage(f, key)              a file-like view that decrypts as it reads (key None: the image as it is)
    load_key(path_or_text, keys)   the disc key from a .dkey file (16 bytes, or 32 hex digits), the 32 digits typed,
                                   or an IRD file (its data1, turned into the key with the D1 constant from the player's
                                   keys file -- a "D1 <key> <iv>" line; never in this repository)
    check_key(f, key)              whether the key decrypts this disc (its program's header), before anything is written

No key is ever logged or written anywhere by this module; error messages never contain one."""
import gzip
import io
import os
import re
import struct

SECTOR = 2048
SCE_MAGIC = b"SCE\x00"


class DiscKeyError(Exception):
    def __init__(self, message, code="disc_key"):
        super().__init__(message)
        self.code = code


def plain_regions(f):
    """[(first, last)] sectors kept plain (sector 0's table), or [] when it isn't a PS3 disc's table"""
    f.seek(0)
    s0 = f.read(SECTOR)
    if len(s0) < SECTOR:
        return []
    n = struct.unpack_from(">I", s0, 0)[0]
    if not 0 < n <= 250:
        return []
    out = []
    for i in range(n):
        a, b = struct.unpack_from(">II", s0, 8 + 8 * i)
        if b < a:
            return []
        out.append((a, b))
    return out


def _aes(key, iv, data, decrypt=True):
    from Crypto.Cipher import AES
    c = AES.new(key, AES.MODE_CBC, iv)
    return c.decrypt(data) if decrypt else c.encrypt(data)


def sector_iv(n):
    return b"\x00" * 12 + struct.pack(">I", n)


class DiscImage(io.RawIOBase):
    """the image read through: sectors outside the plain regions decrypted with the key (None: as stored)"""

    def __init__(self, f, key=None):
        super().__init__()
        self.f = f
        self.key = key
        self.plain = plain_regions(f) if key else []
        f.seek(0, os.SEEK_END)
        self.size = f.tell()
        self.pos = 0

    def _is_plain(self, n):
        return any(a <= n <= b for a, b in self.plain)

    def readable(self):
        return True

    def close(self):
        try:
            self.f.close()
        finally:
            super().close()

    def seekable(self):
        return True

    def seek(self, off, whence=os.SEEK_SET):
        self.pos = off if whence == os.SEEK_SET else self.pos + off if whence == os.SEEK_CUR else self.size + off
        return self.pos

    def tell(self):
        return self.pos

    def read(self, n=-1):
        if n is None or n < 0:
            n = self.size - self.pos
        n = max(0, min(n, self.size - self.pos))
        if not self.key:
            self.f.seek(self.pos)
            data = self.f.read(n)
            self.pos += len(data)
            return data
        out = bytearray()
        while n > 0:
            sec, off = divmod(self.pos, SECTOR)
            self.f.seek(sec * SECTOR)
            raw = self.f.read(SECTOR)
            if len(raw) == SECTOR and not self._is_plain(sec):
                raw = _aes(self.key, sector_iv(sec), raw)
            take = raw[off:off + n]
            if not take:
                break
            out += take
            self.pos += len(take)
            n -= len(take)
        return bytes(out)

    def readinto(self, b):
        data = self.read(len(b))
        b[:len(data)] = data
        return len(data)


def _program_sector(f):
    """the first sector of the disc's program (PS3_GAME/USRDIR/EBOOT.BIN): where a decrypted image shows SCE's header"""
    import iso9660
    try:
        for path, ext in iso9660.entries(f):
            if path.upper() == "PS3_GAME/USRDIR/EBOOT.BIN":
                return ext[0][0]   # the first extent's sector
    except (OSError, ValueError, IndexError, TypeError, AttributeError, struct.error, UnicodeError):
        return None   # not an ISO 9660 image: the caller says what it is
    return None


def kind(f):
    """decrypted (the program's header readable as stored), encrypted (it is in an encrypted region and isn't), or
    unknown"""
    lba = _program_sector(f)
    if lba is None:
        return "unknown"
    f.seek(lba * SECTOR)
    head = f.read(16)
    if head.startswith(SCE_MAGIC):
        return "decrypted"
    regions = plain_regions(f)
    if regions and not any(a <= lba <= b for a, b in regions):
        return "encrypted"
    return "unknown"


def check_key(f, key):
    """whether the key opens this disc: its program's first sector decrypts to SCE's header"""
    lba = _program_sector(f)
    if lba is None:
        return False
    f.seek(lba * SECTOR)
    raw = f.read(SECTOR)
    return len(raw) == SECTOR and _aes(key, sector_iv(lba), raw).startswith(SCE_MAGIC)


HEX32 = re.compile(r"^[0-9a-fA-F]{32}$")


def _hex_key(text):
    t = re.sub(r"\s+", "", text)
    if HEX32.match(t):
        return bytes.fromhex(t)
    return None


def d1_constant(keys_path):
    """the D1 key and IV from the player's keys file (a "D1 <key> <iv>" line), or None"""
    if not keys_path or not os.path.isfile(keys_path):
        return None
    with open(keys_path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            p = line.split("#", 1)[0].split()
            if len(p) == 3 and p[0].upper() == "D1" and HEX32.match(p[1]) and HEX32.match(p[2]):
                return bytes.fromhex(p[1]), bytes.fromhex(p[2])
    return None


def ird_data1(path):
    """an IRD file's data1 (16 bytes); the file is gzip-compressed and starts with "3IRD" """
    with open(path, "rb") as fh:
        raw = fh.read()
    try:
        b = gzip.decompress(raw)
    except OSError:
        b = raw
    if not b.startswith(b"3IRD"):
        raise DiscKeyError("This file isn't an IRD file.", "not_ird")
    ver = b[4]
    o = 5 + 9   # the version, the game's id
    o += 1 + b[o]   # its name
    o += 4 + 5 + 5   # the update, game and app versions
    if ver == 7:
        o += 4   # an id
    for _ in range(2):   # the header and the footer, each a length and its bytes
        n = struct.unpack_from("<I", b, o)[0]
        o += 4 + n
    regions = b[o]
    o += 1 + 16 * regions
    files = struct.unpack_from("<I", b, o)[0]
    o += 4 + files * 24
    o += 2 + 2   # extra config, attachments
    if ver >= 9:
        o += 115   # the PIC
    if o + 32 > len(b):
        raise DiscKeyError("This IRD file is damaged or of a version not supported.", "bad_ird")
    return b[o:o + 16]


def load_key(source, keys_path=None):
    """the disc key from what the player gave: a .dkey file, an IRD file, or the 32 digits typed; DiscKeyError with a
    plain message when it can't be used (never quoting the key)"""
    if source and os.path.isfile(source):
        with open(source, "rb") as fh:
            head = fh.read(4)
        if head[:2] == b"\x1f\x8b" or head == b"3IRD":
            d1 = d1_constant(keys_path)
            if not d1:
                raise DiscKeyError("An IRD file needs one more key in your keys file (a D1 line). Choose a .dkey file "
                                   "or type the disc key instead, or add that line to your keys file.", "needs_d1")
            data1 = ird_data1(source)
            return _aes(d1[0], d1[1], data1, decrypt=False)
        size = os.path.getsize(source)
        with open(source, "rb") as fh:
            data = fh.read(4096)
        if size == 16:
            return data
        k = _hex_key(data.decode("ascii", errors="replace"))
        if k:
            return k
        raise DiscKeyError("This file isn't a disc key (.dkey: 32 hexadecimal digits) or an IRD file.", "bad_key_file")
    k = _hex_key(source or "")
    if k:
        return k
    raise DiscKeyError("A disc key is 32 characters, 0-9 and A-F.", "bad_key")
