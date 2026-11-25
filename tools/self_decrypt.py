#!/usr/bin/env python3
"""Decrypt a retail PS3 application SELF (EBOOT.BIN) into a plain ELF.

The keys are NOT part of this repository. Pass a keys file whose lines are

    APP <revision> <erk-hex-64> <riv-hex-32>

(see docs/SETUP.md). The disc's program is a plain application. A downloaded one -- the official update's
EBOOT.BIN -- is an NPDRM application under a free licence: it needs the file's NPDRM key of its revision and the
KLIC_FREE / KLIC_KEY pair instead (tools/player_keys.py). Programs under a paid licence (a RAP) aren't handled.

Usage: self_decrypt.py --keys <keys.txt> EBOOT.BIN EBOOT.elf
"""
import argparse
import hashlib
import os
import struct
import sys
import zlib

from Crypto.Cipher import AES
from Crypto.Util import Counter


def load_keys(path):
    keys = {}
    for line in open(path):
        line = line.split("#", 1)[0].split()
        if len(line) == 4 and line[0] == "APP":
            keys[int(line[1], 16)] = (bytes.fromhex(line[2]), bytes.fromhex(line[3]))
    return keys


def ctr(key, iv, data):
    c = Counter.new(128, initial_value=int.from_bytes(iv, "big"))
    return AES.new(key, AES.MODE_CTR, counter=c).decrypt(data)


SELF_APP, SELF_NPDRM = 4, 8
LICENSE_FREE = 3


def self_info(d):
    """(self_type, key revision, NPD licence or None) of a SELF, read without any key"""
    if len(d) < 0x100:
        raise ValueError("not a SELF")
    magic, _, keyrev, se_type = struct.unpack_from(">4sIHH", d, 0)
    if magic != b"SCE\0" or se_type != 1:
        raise ValueError("not a SELF")
    appinfo_off = struct.unpack_from(">Q", d, 0x28)[0]
    ctrl_off, ctrl_size = struct.unpack_from(">QQ", d, 0x58)
    self_type = struct.unpack_from(">I", d, appinfo_off + 0x0C)[0]
    return self_type, keyrev, _npd(d, ctrl_off, ctrl_size)


def _npd(d, ctrl_off, ctrl_size):
    """the NPDRM control block's licence type (1 network, 2 local, 3 free), or None without one"""
    off, end = ctrl_off, min(ctrl_off + ctrl_size, len(d))
    while off + 0x10 <= end:
        typ, size = struct.unpack_from(">II", d, off)
        if typ == 3 and d[off + 0x10:off + 0x14] == b"NPD\0":
            return struct.unpack_from(">I", d, off + 0x18)[0]
        if size < 0x10:
            break
        off += size
    return None


class MissingKeys(ValueError):
    """the keys given have nothing that opens this program: kind is 'app' or 'npdrm', need what is missing"""

    def __init__(self, message, kind, revision, need):
        super().__init__(message)
        self.kind, self.revision, self.need = kind, revision, need


def _ring(keys, npdrm):
    """keys: a player_keys.Keyring, or {revision: (erk, riv)} APP keys (with npdrm a Keyring, or None)"""
    import player_keys
    if isinstance(keys, player_keys.Keyring):
        return keys
    ring = player_keys.Keyring()
    ring.app.update(keys or {})
    if npdrm is not None:
        ring.merge(npdrm)
    return ring


def decrypt_self(d, keys, npdrm=None):
    """the program's ELF. keys: a player_keys.Keyring (or {revision: (erk, riv)} APP keys; npdrm then a Keyring for
    an NPDRM program). Every key of the program's kind is tried, the one labelled with its revision first: a wrong
    one fails the metadata check at once. Raises MissingKeys when none opens it, ValueError for anything else."""
    if len(d) < 0x100:
        raise ValueError("not a SELF")
    try:
        return _decrypt_self(d, keys, npdrm)
    except struct.error as e:
        raise ValueError(f"not a valid SELF ({e})") from None


def _decrypt_self(d, keys, npdrm):
    magic, hver, keyrev, se_type, meta_off, hsize, esize = struct.unpack_from(">4sIHHIQQ", d, 0)
    if magic != b"SCE\0" or se_type != 1:
        raise ValueError("not a SELF")
    (ehdr_type, appinfo_off, elf_off, phdr_off, shdr_off, sinfo_off,
     sceversion_off, ctrl_off, ctrl_size) = struct.unpack_from(">9Q", d, 0x20)
    _, _, self_type, _ = struct.unpack_from(">QIIQ", d, appinfo_off)
    ring = _ring(keys, npdrm)
    mi_off = 0x20 + meta_off
    raw = d[mi_off:mi_off + 0x40]
    if self_type == SELF_NPDRM:
        if _npd(d, ctrl_off, ctrl_size) != LICENSE_FREE:
            raise ValueError("an NPDRM program under a paid licence (unsupported)")
        need = [n for n, v in (("KLIC_FREE", ring.klic_free), ("KLIC_KEY", ring.klic_key)) if not v]
        if not ring.candidates("npdrm", keyrev):
            need.append(f"NPDRM {keyrev:#06x}")
        if need:
            raise MissingKeys(f"an NPDRM program: the keys have no {', '.join(need)}", "npdrm", keyrev, need)
        # The free licence's key, unwrapped, first opens the metadata info (AES-128-CBC, a zero IV).
        klic = AES.new(ring.klic_key, AES.MODE_ECB).decrypt(ring.klic_free)
        raw = AES.new(klic, AES.MODE_CBC, bytes(16)).decrypt(raw)
        kind = "npdrm"
    elif self_type == SELF_APP:
        kind = "app"
    else:
        raise ValueError(f"SELF type {self_type} is neither a plain nor an NPDRM application")
    cands = ring.candidates(kind, keyrev)
    if not cands:
        raise MissingKeys(f"no {kind.upper()} key in the keys given", kind, keyrev, [f"{kind.upper()} {keyrev:#06x}"])

    # Metadata info: AES-256-CBC with the application loader key; the right key leaves its padding zero. (A right
    # erk with a wrong riv does too -- CBC's IV only touches the first block -- so the metadata header it opens must
    # also make sense: its section and key counts fit the header.) Then the metadata header + section headers +
    # section keys: AES-128-CTR with the key and IV the metadata info holds.
    mh_off = mi_off + 0x40
    for erk, riv in cands:
        mi = AES.new(erk, AES.MODE_CBC, riv).decrypt(raw)
        if mi[0x10:0x20] != bytes(16) or mi[0x30:0x40] != bytes(16):
            continue
        rest = ctr(mi[0:16], mi[0x20:0x30], d[mh_off:hsize])
        _, _, sec_count, key_count, _ = struct.unpack_from(">QIIII", rest, 0)
        if 0 < sec_count <= 256 and key_count <= 4096 and 0x20 + 0x30 * sec_count + 16 * key_count <= len(rest):
            break
    else:
        raise MissingKeys(f"metadata info did not decrypt with any of the {len(cands)} {kind.upper()} key(s) given "
                          f"(wrong key?)", kind, keyrev, [f"{kind.upper()} {keyrev:#06x}"] +
                          (["KLIC_FREE", "KLIC_KEY"] if kind == "npdrm" else []))
    secs = [struct.unpack_from(">QQIIIIIIII", rest, 0x20 + 0x30 * i) for i in range(sec_count)]
    kbase = 0x20 + 0x30 * sec_count
    skeys = [rest[kbase + 16 * i:kbase + 16 * i + 16] for i in range(key_count)]

    # Rebuild the ELF image from the plaintext headers kept in the SELF.
    e_phoff, e_shoff = struct.unpack_from(">QQ", d, elf_off + 0x20)
    e_phentsize, e_phnum, e_shentsize, e_shnum = struct.unpack_from(">HHHH", d, elf_off + 0x36)
    phdrs = [struct.unpack_from(">IIQQQQQQ", d, phdr_off + 56 * i) for i in range(e_phnum)]
    size = max([e_phoff + 56 * e_phnum] + [p[2] + p[5] for p in phdrs])
    if e_shnum:
        size = max(size, e_shoff + e_shentsize * e_shnum)
    out = bytearray(size)
    out[0:0x40] = d[elf_off:elf_off + 0x40]
    out[e_phoff:e_phoff + 56 * e_phnum] = d[phdr_off:phdr_off + 56 * e_phnum]
    if e_shnum:
        out[e_shoff:e_shoff + e_shentsize * e_shnum] = d[shdr_off:shdr_off + e_shentsize * e_shnum]

    for (off, sz, typ, pidx, hashed, sha_idx, enc, key_idx, iv_idx, comp) in secs:
        if typ != 2:  # 2 = program segment; 1 = shdr, 3 = sceversion
            continue
        data = d[off:off + sz]
        if enc == 3:
            data = ctr(skeys[key_idx], skeys[iv_idx], data)
        if hashed == 2:
            # sha1 HMAC: key slots sha_idx..sha_idx+5 = digest(20) + hmac key(64)
            want = b"".join(skeys[sha_idx:sha_idx + 6])
            import hmac
            got = hmac.new(want[0x20:0x60], data, hashlib.sha1).digest()
            if got != want[:20]:
                raise ValueError(f"segment {pidx}: HMAC mismatch")
        if comp == 2:
            data = zlib.decompress(data)
        p = phdrs[pidx]
        if len(data) != p[5]:
            raise ValueError(f"segment {pidx}: size {len(data)} != p_filesz {p[5]}")
        out[p[2]:p[2] + len(data)] = data
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keys", required=True)
    ap.add_argument("input")
    ap.add_argument("output")
    a = ap.parse_args()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import player_keys
    ring = player_keys.load(a.keys)
    elf = decrypt_self(open(a.input, "rb").read(), ring)
    open(a.output, "wb").write(elf)
    print(f"{a.output}: {len(elf)} bytes sha256={hashlib.sha256(elf).hexdigest()}")


if __name__ == "__main__":
    sys.exit(main())
