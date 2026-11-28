"""Made-up PS3 programs for the tests: an ELF and a SELF of it encrypted with made-up keys, laid out as a disc program's
is (tools/self_decrypt.py reads them). No Sony key or program is used or needed."""
import hashlib
import hmac
import os
import struct
import zlib

from Crypto.Cipher import AES
from Crypto.Util import Counter


def rnd(n):
    return os.urandom(n)


def ctr_crypt(key, iv, data):
    return AES.new(key, AES.MODE_CTR, counter=Counter.new(128, initial_value=int.from_bytes(iv, "big"))).encrypt(data)


def align(n, a=16):
    return (n + a - 1) // a * a


def make_elf(segments):
    """segments: [bytes] -> a big-endian ELF64 with one PT_LOAD per segment, its bytes"""
    n = len(segments)
    offs, off = [], 0x1000
    for s in segments:
        offs.append(off)
        off = align(off + len(s), 0x1000)
    out = bytearray(offs[-1] + len(segments[-1]))
    out[0:4] = b"\x7fELF"
    out[4:8] = bytes([2, 2, 1, 0x66])
    struct.pack_into(">HHIQQQIHHHHHH", out, 0x10, 2, 21, 1, 0x10000, 0x40, 0, 0, 0x40, 56, n, 0x40, 0, 0)
    for i, (s, o) in enumerate(zip(segments, offs)):
        struct.pack_into(">IIQQQQQQ", out, 0x40 + 56 * i, 1, 5, o, 0x10000 + o, 0x10000 + o, len(s), len(s),
                         0x10000)
        out[o:o + len(s)] = s
    return bytes(out)


def make_self(elf, erk, riv, keyrev=0x1, compress=(True,), self_type=4):
    """elf -> a SELF of it (a plain application: self type 4), its metadata opened by (erk, riv). self_type: another
    type written into its application info (to test that only plain programs are taken)."""
    phnum = struct.unpack_from(">H", elf, 0x38)[0]
    phdrs = [struct.unpack_from(">IIQQQQQQ", elf, 0x40 + 56 * i) for i in range(phnum)]
    app_off, elf_off = 0x70, 0x90
    phdr_off = elf_off + 0x40
    ctrl_off = align(phdr_off + 56 * phnum)
    ctrl = struct.pack(">IIQ", 1, 0x30, 0) + bytes(0x20)                     # control flags
    mi_off = align(ctrl_off + len(ctrl))
    mkey, miv = rnd(16), rnd(16)
    secs, keys, blobs = [], [], []
    for i, p in enumerate(phdrs):
        seg = elf[p[2]:p[2] + p[5]]
        comp = compress[i % len(compress)]
        stored = zlib.compress(seg) if comp else seg
        skey, siv, hkey = rnd(16), rnd(16), rnd(0x40)
        k0 = len(keys)
        keys += [skey, siv]
        digest = hmac.new(hkey, stored, hashlib.sha1).digest()
        sha = (digest + bytes(12)) + hkey
        keys += [sha[j:j + 16] for j in range(0, 0x60, 16)]
        blobs.append(ctr_crypt(skey, siv, stored))
        secs.append([0, len(blobs[-1]), 2, i, 2, k0 + 2, 3, k0, k0 + 1, 2 if comp else 1])
    hsize = align(mi_off + 0x40 + 0x20 + 0x30 * len(secs) + 16 * len(keys))
    off = hsize
    for s, b in zip(secs, blobs):
        s[0] = off
        off = align(off + len(b))
    mh = struct.pack(">QIIIIII", 0, 1, len(secs), len(keys), 0, 0, 0)
    mh += b"".join(struct.pack(">QQIIIIIIII", *s) for s in secs) + b"".join(keys)
    mi = AES.new(erk, AES.MODE_CBC, riv).encrypt(mkey + bytes(16) + miv + bytes(16))
    d = bytearray(off)
    struct.pack_into(">4sIHHIQQ", d, 0, b"SCE\0", 2, keyrev, 1, mi_off - 0x20, hsize, off - hsize)
    struct.pack_into(">9Q", d, 0x20, 3, app_off, elf_off, phdr_off, 0, 0, 0, ctrl_off, len(ctrl))
    struct.pack_into(">QIIQ", d, app_off, 0x1010000001000003, 1, self_type, 0x0001000000000000)
    d[elf_off:elf_off + 0x40] = elf[:0x40]
    d[phdr_off:phdr_off + 56 * phnum] = elf[0x40:0x40 + 56 * phnum]
    d[ctrl_off:ctrl_off + len(ctrl)] = ctrl
    d[mi_off:mi_off + 0x40] = mi
    d[mi_off + 0x40:mi_off + 0x40 + len(mh)] = ctr_crypt(mkey, miv, mh)
    for s, b in zip(secs, blobs):
        d[s[0]:s[0] + len(b)] = b
    return bytes(d)
