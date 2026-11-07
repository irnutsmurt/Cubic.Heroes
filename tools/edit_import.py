#!/usr/bin/env python3
"""Import edited characters saved on a real PS3 (or downloaded as PS3 save data) into an
install's saves, where this port's save data reads them.

A real PS3 keeps an edit save's DATA.EDT encrypted (a protected file listed in its PARAM.PFD);
the port's save data stores files as they are, so the file is decrypted here, once, as the
PS3's save system would decrypt it for the game (the method of Apollo Save Tool's pfd_util.c,
bucanero/apollo-ps3, GPL-3.0). Two secrets are needed and neither is kept in the repository:
  - the PS3's fixed syscon manager key: taken from a local copy of Apollo's source
    (--keys-from .../pfd_util.c) into build/ext/pfd_keys.json;
  - the game's secure file ID (16 bytes): what the game passes the save system for the
    file. Captured from the game itself -- a run with PS3_SAVEDATA_SECURE_ID_LOG=1 that saves
    an edited character logs "[savedata] secure file 'DATA.EDT': id ..." (ps3recomp
    cellSaveData.c) -- into build/ext/secure_file_id.txt; or, without it, searched for in the
    decrypted EBOOT.elf. Either way it is checked against the HMAC-SHA1 the PARAM.PFD keeps
    for the file before anything is decrypted: an exact match, not a guess.

    build/venv/Scripts/python.exe tools/edit_import.py --keys-from build/ext/apollo-src/pfd_util.c
    build/venv/Scripts/python.exe tools/edit_import.py <PS3/SAVEDATA dir or save folder> [...] \\
        --into build/play-route/gamedata/dev_hdd0/home/00000001/savedata
Without --into it only checks and decrypts into build/ext/edit-import/. An existing save
folder of the same name is never overwritten."""
import hashlib
import hmac
import json
import os
import re
import shutil
import struct
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXT = os.path.join(ROOT, "build", "ext")
KEYS = os.path.join(EXT, "pfd_keys.json")
SFID = os.path.join(EXT, "secure_file_id.txt")
ELF = os.path.join(ROOT, "build", "EBOOT.elf")
# the four fixed bytes the PS3 mixes into a key made from a secure file ID (positions 1, 2, 5, 8)
FIXED = {1: 11, 2: 15, 5: 14, 8: 10}


def mixed(secure_id, size):
    """the secure file ID spread over `size` bytes around the fixed ones (pfd.c,
    pfd_generate_hash_key_for_secure_file_id; pfd_util.c, _get_aes_details_pfd)"""
    out, j = bytearray(size), 0
    for i in range(size):
        if i in FIXED:
            out[i] = FIXED[i]
        else:
            out[i] = secure_id[j]
            j += 1
    return bytes(out)


def keys_from_apollo(path):
    src = open(path, encoding="utf-8", errors="replace").read()

    def arr(name):
        m = re.search(name + r"\s*(?:\[\])?\s*=\s*\{([^}]*)\}", src)
        if not m:
            sys.exit(f"{name} not found in {path}")
        return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
    xor = arr("xor_key")
    key = arr(r"\.syscon_manager_key")
    key = bytes(b ^ xor[i % len(xor)] for i, b in enumerate(key))
    if len(key) != 16:
        sys.exit("the syscon manager key is not 16 bytes")
    os.makedirs(EXT, exist_ok=True)
    json.dump({"syscon_manager_key": key.hex(), "from": os.path.relpath(path, ROOT)}, open(KEYS, "w"))
    print(f"key saved to {os.path.relpath(KEYS, ROOT)} (local; never committed)")


def load_key():
    try:
        return bytes.fromhex(json.load(open(KEYS))["syscon_manager_key"])
    except (OSError, ValueError, KeyError):
        sys.exit(f"no key: run with --keys-from <apollo-ps3 source/pfd_util.c> first ({KEYS})")


def parse_pfd(path):
    """the entries of a PARAM.PFD: {name: (entry key, [file hashes], file size)}"""
    b = open(path, "rb").read()
    magic, version = struct.unpack(">QQ", b[:16])
    if magic != 0x50464442:
        raise ValueError(f"{path}: not a PS3 PARAM.PFD")
    capacity, reserved, used = struct.unpack(">QQQ", b[96:120])
    table = 120 + capacity * 8
    out = {}
    for i in range(used):
        e = b[table + i * 272: table + (i + 1) * 272]
        name = e[8:73].split(b"\0")[0].decode("ascii", "replace")
        out[name] = (e[80:144], [e[144 + 20 * k: 164 + 20 * k] for k in range(4)], struct.unpack(">Q", e[264:272])[0])
    return version, out


def find_secure_id(data, want):
    """the 16 bytes of the game's program whose key reproduces the file's HMAC"""
    try:
        sid = bytes.fromhex(open(SFID).read().strip())
        if hmac.new(mixed(sid, 20), data, hashlib.sha1).digest() == want:
            return sid
    except (OSError, ValueError):
        pass
    e = open(ELF, "rb").read()
    phoff, n = struct.unpack(">Q", e[0x20:0x28])[0], struct.unpack(">H", e[0x38:0x3A])[0]
    segs = []
    for i in range(n):
        t, fl, off, va, pa, fsz, msz, al = struct.unpack(">IIQQQQQQ", e[phoff + i * 56: phoff + i * 56 + 56])
        if t == 1 and fsz:
            segs.append((fl & 1, off, fsz))
    segs.sort()   # data before code
    for step in (4, 1):
        for x, off, fsz in segs:
            t0 = time.time()
            print(f"  trying the program's {'code' if x else 'data'} segment at 0x{off:X} "
                  f"({fsz >> 20} MB, every {step} bytes)", flush=True)
            for at in range(off, off + fsz - 15, step):
                if step == 1 and at % 4 == 0:
                    continue   # tried
                sid = e[at:at + 16]
                if not any(sid):
                    continue   # all zero: not a key
                if hmac.new(mixed(sid, 20), data, hashlib.sha1).digest() == want:
                    print(f"  found at 0x{at:X} after {time.time() - t0:.0f} s")
                    os.makedirs(EXT, exist_ok=True)
                    open(SFID, "w").write(sid.hex() + "\n")
                    return sid
    return None


def decrypt(data, entry_key, size, syscon, sid):
    """pfd_util.c decrypt_save_file: the entry's key under the syscon manager key (CBC, the
    IV made from the secure file ID), then each 16-byte block: AES-decrypt, XOR with the
    block counter AES-encrypted"""
    from Crypto.Cipher import AES
    ek = AES.new(syscon, AES.MODE_CBC, iv=mixed(sid, 16)).decrypt(entry_key)
    key = ek[:16]
    enc, dec = AES.new(key, AES.MODE_ECB), AES.new(key, AES.MODE_ECB)
    n = (len(data) + 15) // 16
    data = data.ljust(n * 16, b"\0")
    counters = b"".join(struct.pack(">QQ", i, 0) for i in range(n))
    ks = enc.encrypt(counters)
    plain = dec.decrypt(data)
    return bytes(a ^ b for a, b in zip(plain, ks))[:size]


def entropy(b):
    import math
    from collections import Counter
    c = Counter(b)
    return -sum(v / len(b) * math.log2(v / len(b)) for v in c.values())


def folders(args):
    for a in args:
        if os.path.exists(os.path.join(a, "PARAM.SFO")):
            yield a
        else:
            for d in sorted(os.listdir(a)):
                if os.path.exists(os.path.join(a, d, "PARAM.SFO")):
                    yield os.path.join(a, d)


def main():
    a = sys.argv[1:]
    if a[:1] == ["--keys-from"] and len(a) == 2:
        return keys_from_apollo(a[1])
    into = None
    if "--into" in a:
        i = a.index("--into")
        into, a = a[i + 1], a[:i] + a[i + 2:]
    if not a:
        sys.exit(__doc__)
    syscon = load_key()
    out_root = into or os.path.join(EXT, "edit-import")
    for src in folders(a):
        name = os.path.basename(os.path.normpath(src))
        version, entries = parse_pfd(os.path.join(src, "PARAM.PFD"))
        protected = [f for f in entries if f != "PARAM.SFO"]
        print(f"{name}: PFD v{version}, protected {protected}")
        dst = os.path.join(out_root, name)
        if os.path.exists(dst):
            print(f"  {dst} exists: left as it is")
            continue
        files = {}
        for f in protected:
            ekey, hashes, size = entries[f]
            data = open(os.path.join(src, f), "rb").read()
            sid = find_secure_id(data, hashes[0])
            if not sid:
                sys.exit(f"  no secure file ID in {ELF} reproduces {f}'s hash")
            plain = decrypt(data, ekey, size, syscon, sid)
            print(f"  {f}: {size} bytes, entropy {entropy(data):.2f} -> {entropy(plain):.2f} bits/byte, "
                  f"starts {plain[:16].hex()}")
            files[f] = plain
        os.makedirs(dst)
        for f in os.listdir(src):
            if f == "PARAM.PFD":
                continue
            if f in files:
                open(os.path.join(dst, f), "wb").write(files[f])
            else:
                shutil.copy2(os.path.join(src, f), dst)
        # the port's PARAM.PFD: the names of the files the game wrote as secure files
        open(os.path.join(dst, "PARAM.PFD"), "w", newline="\n").write("".join(f + "\n" for f in protected))
        print(f"  -> {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
