#!/usr/bin/env python3
"""Decrypt a retail PS3 application SELF (EBOOT.BIN) into a plain ELF.

The keys are NOT part of this repository. Pass a keys file whose lines are

    APP <revision> <erk-hex-64> <riv-hex-32>

(see docs/SETUP.md; tools/player_keys.py reads the other forms of keys). Only non-NPDRM disc applications are
handled, which is all BLUS-30490 needs.

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


SELF_APP = 4


class MissingKeys(ValueError):
    """the keys given have nothing that opens this program: kind is 'app', need what is missing"""

    def __init__(self, message, kind, revision, need):
        super().__init__(message)
        self.kind, self.revision, self.need = kind, revision, need


def _ring(keys):
    """keys: a player_keys.Keyring, or {revision: (erk, riv)} APP keys"""
    import player_keys
    if isinstance(keys, player_keys.Keyring):
        return keys
    ring = player_keys.Keyring()
    ring.app.update(keys or {})
    return ring


def decrypt_self(d, keys):
    """the program's ELF. keys: a player_keys.Keyring, or {revision: (erk, riv)} APP keys. Every APP key is tried,
    the one labelled with the program's revision first: a wrong one fails the metadata check at once. Raises
    MissingKeys when none opens it, ValueError for anything else (an NPDRM program among them: not handled)."""
    if len(d) < 0x100:
        raise ValueError("not a SELF")
    try:
        return _decrypt_self(d, keys)
    except struct.error as e:
        raise ValueError(f"not a valid SELF ({e})") from None


def _decrypt_self(d, keys):
    magic, hver, keyrev, se_type, meta_off, hsize, esize = struct.unpack_from(">4sIHHIQQ", d, 0)
    if magic != b"SCE\0" or se_type != 1:
        raise ValueError("not a SELF")
    (ehdr_type, appinfo_off, elf_off, phdr_off, shdr_off, sinfo_off,
     sceversion_off, ctrl_off, ctrl_size) = struct.unpack_from(">9Q", d, 0x20)
    _, _, self_type, _ = struct.unpack_from(">QIIQ", d, appinfo_off)
    ring = _ring(keys)
    mi_off = 0x20 + meta_off
    raw = d[mi_off:mi_off + 0x40]
    if self_type != SELF_APP:
        raise ValueError(f"SELF type {self_type} is not a plain application (NPDRM unsupported)")
    kind = "app"
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
                          f"(wrong key?)", kind, keyrev, [f"{kind.upper()} {keyrev:#06x}"])
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
