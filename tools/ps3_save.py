#!/usr/bin/env python3
"""A character saved on a PS3: recognising it, and decrypting it with key material the PLAYER supplies.

On a real PS3 the game's save system keeps DATA.EDT (the character, tools/edt_format.py) encrypted as a "protected
file" listed in PARAM.PFD. Copied off the console -- to a USB stick, onto a website -- it stays encrypted; read as it
is, the game sees no name, no class and no voxels (Cubic.Heroes issue #8, RPCS3 issue #11336). Decrypted, it is the
same file byte for byte as an emulator's or Cubic.Heroes' own copy. Nothing else is encoded on top.

What decrypting takes (per protected file):
  1. the game's SECURE FILE ID (16 bytes) -- what the game hands the save system for the file. It is in the
     player's own decrypted program (every installation keeps data/disc/PS3_GAME/USRDIR/EBOOT.elf), found there and
     proved by the HMAC-SHA1 PARAM.PFD keeps for the file (keyed with the ID): an exact match, never a guess. No
     console key is involved.
  2. the PS3's SYSCON MANAGER KEY (16 bytes, AES-128) -- a fixed key of the console's system software. PARAM.PFD
     keeps each protected file's own AES key encrypted with it (AES-128-CBC, the IV made from the secure file ID).
     It is NOT in the game, the disc, the disc key or the keys file for the game's program. Cubic.Heroes never ships
     it: the player supplies it (tools/secret_store.py keeps it, protected by the operating system), and it is
     never printed, logged, written to diagnostics or put in any package.
The file is then decrypted 16 bytes at a time: AES-decrypt the block with the file's key, XOR with the block's
index (a 128-bit big-endian counter) AES-encrypted with the same key.

Validating a supplied key (before anything is written): the file key it gives must decrypt DATA.EDT's first block
to the character header "EDTFILE1.9" (80 known bits: a wrong key passes with probability 2^-80), and the whole
decrypted file must then parse as a character (tools/edt_format.py), its length the one PARAM.PFD records. Only then
is the character imported; the original files are only read, never changed.

Recognising (never by PARAM.PFD alone -- a decrypted DATA.EDT may still sit beside its original PARAM.PFD):
  plain      DATA.EDT parses as a character -- usable as it is, no key needed;
  encrypted  DATA.EDT is NOT a character, and PARAM.PFD is a PS3 PFD (magic "PFDB", version 3 or 4) listing
             DATA.EDT, whose recorded size rounds up to DATA.EDT's length, the data looking random (entropy > 7.5
             bits a byte);
  damaged    anything else (with the reason).
"""
import hashlib
import hmac
import math
import os
import struct
from collections import Counter

import edt_format

PFD_MAGIC = 0x50464442            # "PFDB", as a big-endian 64-bit number at offset 0
FIXED = {1: 11, 2: 15, 5: 14, 8: 10}   # the bytes PFD key derivation puts between a secure file ID's own
HEADER = edt_format.MAGIC


class SaveError(Exception):
    """A meaningful reason for the player; never contains key material."""


def _mixed(secure_id, size):
    """The secure file ID spread over `size` bytes around four fixed ones -- the PFD's key for a secure file."""
    out, j = bytearray(size), 0
    for i in range(size):
        if i in FIXED:
            out[i] = FIXED[i]
        else:
            out[i] = secure_id[j]
            j += 1
    return bytes(out)


def entropy(b):
    if not b:
        return 0.0
    c = Counter(b)
    n = len(b)
    return -sum(v / n * math.log2(v / n) for v in c.values())


def parse_pfd(data):
    """PARAM.PFD's protected files: (version, {name: (entry key 64 bytes, [4 hashes], size)}); None if not a PFD."""
    if len(data) < 120:
        return None
    magic, version = struct.unpack(">QQ", data[:16])
    if magic != PFD_MAGIC or version not in (3, 4):
        return None
    capacity, _reserved, used = struct.unpack(">QQQ", data[96:120])
    table = 120 + capacity * 8
    if capacity > 4096 or used > 4096 or table + used * 272 > len(data):
        return None
    out = {}
    for i in range(used):
        e = data[table + i * 272: table + (i + 1) * 272]
        name = e[8:73].split(b"\0")[0].decode("ascii", "replace")
        out[name] = (e[80:144], [e[144 + 20 * k: 164 + 20 * k] for k in range(4)], struct.unpack(">Q", e[264:272])[0])
    return version, out


def classify(edt, pfd_bytes=None):
    """('plain', Character) | ('encrypted', {pfd entry}) | ('damaged', reason) for a DATA.EDT and its PARAM.PFD."""
    if edt_format.is_plain(edt):
        try:
            return "plain", edt_format.parse(edt)
        except edt_format.EdtError as e:
            return "damaged", f"the character data is damaged: {e}"
    pfd = parse_pfd(pfd_bytes) if pfd_bytes else None
    if pfd and "DATA.EDT" in pfd[1]:
        _key, _hashes, size = pfd[1]["DATA.EDT"]
        if (size + 15) // 16 * 16 == len(edt) and (len(edt) < 4096 or entropy(edt) > 7.5):
            return "encrypted", pfd[1]["DATA.EDT"]
        return "damaged", "the character data does not match its PS3 save information (PARAM.PFD)"
    if pfd_bytes is None or not pfd_bytes:
        return "damaged", "not a character file, and there is no PS3 save information (PARAM.PFD) to decrypt it with"
    return "damaged", "not a character file, and its PARAM.PFD is not a PS3 save's"


def secure_id_ok(secure_id, edt, entry):
    """True when `secure_id` is the one the file was saved with (PARAM.PFD's HMAC-SHA1 of the encrypted file)."""
    return hmac.compare_digest(hmac.new(_mixed(secure_id, 20), edt, hashlib.sha1).digest(), entry[1][0])


def find_secure_id(program, edt, entry, cached=None):
    """The game's secure file ID for this file, from the player's own decrypted program (EBOOT.elf bytes): the
    16-byte windows of its loadable segments, most frequent first, until one reproduces PARAM.PFD's HMAC. `cached`
    (a previous result) is tried first. None when none does (not this game's file, or not its program)."""
    if cached and secure_id_ok(cached, edt, entry):
        return cached
    if program[:4] != b"\x7fELF":
        raise SaveError("the game's program (EBOOT.elf) is not readable")
    phoff = struct.unpack(">Q", program[0x20:0x28])[0]
    n = struct.unpack(">H", program[0x38:0x3A])[0]
    windows = Counter()
    for i in range(n):
        t, fl, off, va, pa, fsz, msz, al = struct.unpack(">IIQQQQQQ", program[phoff + i * 56: phoff + i * 56 + 56])
        if t == 1 and fsz:
            for at in range(off, off + fsz - 15, 4):
                w = program[at:at + 16]
                if any(w):
                    windows[w] += 1
    for w, _count in windows.most_common():
        if secure_id_ok(w, edt, entry):
            return w
    return None


def _file_key(console_key, secure_id, entry):
    from Crypto.Cipher import AES
    return AES.new(console_key, AES.MODE_CBC, iv=_mixed(secure_id, 16)).decrypt(entry[0])[:16]


def _decrypt_blocks(key, data, first=0, count=None):
    from Crypto.Cipher import AES
    n = (len(data) + 15) // 16 if count is None else count
    data = data[:n * 16].ljust(n * 16, b"\0")
    enc = AES.new(key, AES.MODE_ECB)
    counters = b"".join(struct.pack(">QQ", first + i, 0) for i in range(n))
    ks = enc.encrypt(counters)
    plain = enc.decrypt(data)
    return bytes(a ^ b for a, b in zip(plain, ks))


def check_console_key(console_key, secure_id, edt, entry):
    """True when the key decrypts the file's first block to the character header (no other bytes decrypted)."""
    if len(console_key) != 16:
        return False
    return _decrypt_blocks(_file_key(console_key, secure_id, entry), edt[:16], count=1)[:len(HEADER)] == HEADER


def decrypt_character(console_key, secure_id, edt, entry):
    """The plain DATA.EDT bytes; SaveError (no key material in it) when the key or the data is wrong."""
    if not check_console_key(console_key, secure_id, edt, entry):
        raise SaveError("the PS3 save key does not decrypt this character (a wrong key)")
    size = entry[2]
    plain = _decrypt_blocks(_file_key(console_key, secure_id, entry), edt)[:size]
    try:
        c = edt_format.parse(plain)
    except edt_format.EdtError as e:
        raise SaveError(f"decrypted, but the character data is damaged: {e}")
    if edt_format.serialize(c) != plain:
        raise SaveError("decrypted, but the character data does not read back exactly")
    return plain


def parse_key_text(text):
    """A key the player typed: 32 hexadecimal digits (spaces allowed); SaveError otherwise. Never echoes it."""
    s = "".join(text.split())
    if len(s) != 32:
        raise SaveError("the PS3 save key is 32 hexadecimal digits (16 bytes)")
    try:
        return bytes.fromhex(s)
    except ValueError:
        raise SaveError("the PS3 save key is 32 hexadecimal digits (0-9, a-f)")


# ---- tests only: a synthetic encrypted save, made with a made-up key (never a real console's) ----

def _encrypt_for_tests(console_key, secure_id, plain, file_key=b"\x11" * 16):
    """(encrypted DATA.EDT, PARAM.PFD bytes) as a PS3 would write them, under any 16-byte key: for unit tests."""
    from Crypto.Cipher import AES
    n = (len(plain) + 15) // 16
    p = plain.ljust(n * 16, b"\0")
    ecb = AES.new(file_key, AES.MODE_ECB)
    ks = ecb.encrypt(b"".join(struct.pack(">QQ", i, 0) for i in range(n)))
    enc = ecb.encrypt(bytes(a ^ b for a, b in zip(p, ks)))
    entry_key = AES.new(console_key, AES.MODE_CBC, iv=_mixed(secure_id, 16)).encrypt(file_key + b"\0" * 48)
    h0 = hmac.new(_mixed(secure_id, 20), enc, hashlib.sha1).digest()
    capacity, used = 57, 1
    pfd = bytearray(struct.pack(">QQ", PFD_MAGIC, 3) + b"\0" * 80 + struct.pack(">QQQ", capacity, 0, used))
    pfd += b"\0" * (capacity * 8)
    e = bytearray(272)
    e[8:8 + len(b"DATA.EDT")] = b"DATA.EDT"
    e[80:144] = entry_key
    e[144:164] = h0
    e[264:272] = struct.pack(">Q", len(plain))
    pfd += e
    pfd += b"\0" * (32768 - len(pfd))
    return enc, bytes(pfd)


def scan_folder(path):
    """A save folder's DATA.EDT and PARAM.PFD bytes (b"" when missing)."""
    def rd(name):
        p = os.path.join(path, name)
        return open(p, "rb").read() if os.path.isfile(p) else b""
    return rd("DATA.EDT"), rd("PARAM.PFD")
