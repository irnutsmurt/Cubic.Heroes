#!/usr/bin/env python3
"""Decrypt a retail PS3 application SELF (EBOOT.BIN) into a plain ELF.

The keys are NOT part of this repository. Pass a keys file whose lines are

    APP <revision> <erk-hex-64> <riv-hex-32>

(see docs/SETUP.md). Only non-NPDRM disc applications are handled, which is
all BLUS-30490 needs.

Usage: self_decrypt.py --keys <keys.txt> EBOOT.BIN EBOOT.elf
"""
import argparse
import hashlib
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


def decrypt_self(d, keys):
    magic, hver, keyrev, se_type, meta_off, hsize, esize = struct.unpack_from(">4sIHHIQQ", d, 0)
    if magic != b"SCE\0" or se_type != 1:
        raise ValueError("not a SELF")
    (ehdr_type, appinfo_off, elf_off, phdr_off, shdr_off, sinfo_off,
     sceversion_off, ctrl_off, ctrl_size) = struct.unpack_from(">9Q", d, 0x20)
    _, _, self_type, _ = struct.unpack_from(">QIIQ", d, appinfo_off)
    if self_type != 4:
        raise ValueError(f"SELF type {self_type} is not a plain application (NPDRM unsupported)")
    if keyrev not in keys:
        raise ValueError(f"no APP key for revision {keyrev:#x} in keys file")
    erk, riv = keys[keyrev]

    # Metadata info: AES-256-CBC with the application loader key.
    mi_off = 0x20 + meta_off
    mi = AES.new(erk, AES.MODE_CBC, riv).decrypt(d[mi_off:mi_off + 0x40])
    if mi[0x10:0x20] != bytes(16) or mi[0x30:0x40] != bytes(16):
        raise ValueError("metadata info did not decrypt (wrong key?)")
    mkey, miv = mi[0:16], mi[0x20:0x30]

    # Metadata header + section headers + section keys: AES-128-CTR.
    mh_off = mi_off + 0x40
    rest = ctr(mkey, miv, d[mh_off:hsize])
    _, _, sec_count, key_count, _ = struct.unpack_from(">QIIII", rest, 0)
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
    elf = decrypt_self(open(a.input, "rb").read(), load_keys(a.keys))
    open(a.output, "wb").write(elf)
    print(f"{a.output}: {len(elf)} bytes sha256={hashlib.sha256(elf).hexdigest()}")


if __name__ == "__main__":
    sys.exit(main())
