#!/usr/bin/env python3
"""What a release must be before anything of it is installed (M9.3; docs/RELEASE_SIGNING.md).

A release on GitHub carries, besides its downloads:
    release.json       the manifest: the version, the channel, the notes, the minimum launcher, each download's
                       name, platform, size and SHA-256, and the id of the key that signed it
    release.json.sig   the manifest's Ed25519 signature (64 bytes), made with the owner's private key -- which is
                       never in the repository, a build machine or CI

The launcher's pipeline worker checks the signature against the public keys it carries (config/release_keys.json)
and then every download's size and SHA-256 against the manifest: an unsigned, mis-signed or altered release, or a
download that is not the one signed for, is refused. Ed25519 is OpenSSL's (the libcrypto Python's own ssl module
loads): no cryptography of this project's own.
"""
import base64
import ctypes
import ctypes.util
import hashlib
import json
import os
import re
import sys

ROOT = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KEYS_FILE = os.path.join(ROOT, "config", "release_keys.json")
MANIFEST_FORMAT = 1
EVP_PKEY_ED25519 = 1087   # OpenSSL's NID_ED25519
# a DER SubjectPublicKeyInfo of an Ed25519 key: this prefix, then the 32 bytes of the key
ED25519_DER_PREFIX = bytes.fromhex("302a300506032b6570032100")
PLATFORMS = ("windows-x86_64", "linux-x86_64")


class ReleaseError(Exception):
    def __init__(self, message, code):
        super().__init__(message)
        self.code = code


# ---------------- Ed25519, through OpenSSL ----------------

_lib = None


def libcrypto():
    """the libcrypto Python's ssl module is built with (the frozen app carries it beside its _ssl)"""
    global _lib
    if _lib is not None:
        return _lib
    import _ssl
    cands = []
    d = os.path.dirname(getattr(_ssl, "__file__", "") or "")
    if d and os.path.isdir(d):
        cands += [os.path.join(d, n) for n in sorted(os.listdir(d))
                  if n.lower().startswith("libcrypto") and n.lower().endswith((".dll", ".so", ".dylib")) or
                  n.lower().startswith("libcrypto.so")]
    if getattr(sys, "frozen", False):
        base = getattr(sys, "_MEIPASS", os.path.dirname(sys.executable))
        cands += [os.path.join(base, n) for n in os.listdir(base) if n.lower().startswith("libcrypto")]
    cands += [n for n in (ctypes.util.find_library("crypto"), "libcrypto.so.3", "libcrypto-3-x64.dll",
                          "libcrypto-3.dll") if n]
    for c in cands:
        try:
            lib = ctypes.CDLL(c)
            lib.EVP_PKEY_new_raw_public_key   # noqa: B018 -- OpenSSL 1.1.1 or later
        except (OSError, AttributeError):
            continue
        vp, sz = ctypes.c_void_p, ctypes.c_size_t
        lib.EVP_PKEY_new_raw_public_key.restype = vp
        lib.EVP_PKEY_new_raw_public_key.argtypes = (ctypes.c_int, vp, ctypes.c_char_p, sz)
        lib.EVP_PKEY_free.argtypes = (vp,)
        lib.EVP_MD_CTX_new.restype = vp
        lib.EVP_MD_CTX_free.argtypes = (vp,)
        lib.EVP_DigestVerifyInit.restype = ctypes.c_int
        lib.EVP_DigestVerifyInit.argtypes = (vp, vp, vp, vp, vp)
        lib.EVP_DigestVerify.restype = ctypes.c_int
        lib.EVP_DigestVerify.argtypes = (vp, ctypes.c_char_p, sz, ctypes.c_char_p, sz)
        _lib = lib
        return lib
    raise ReleaseError("OpenSSL's library (libcrypto) wasn't found: releases can't be checked.", "no_crypto")


def ed25519_verify(public_key, message, signature):
    """True only for a valid Ed25519 signature of message by the 32-byte public key"""
    if len(public_key) != 32 or len(signature) != 64:
        return False
    lib = libcrypto()
    pkey = lib.EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, None, public_key, 32)
    if not pkey:
        return False
    ctx = lib.EVP_MD_CTX_new()
    try:
        if not ctx or lib.EVP_DigestVerifyInit(ctx, None, None, None, pkey) != 1:
            return False
        return lib.EVP_DigestVerify(ctx, signature, 64, message, len(message)) == 1
    finally:
        if ctx:
            lib.EVP_MD_CTX_free(ctx)
        lib.EVP_PKEY_free(pkey)


# ---------------- the keys trusted ----------------

def key_id(public_key):
    return hashlib.sha256(public_key).hexdigest()[:16]


def public_key_from(text_or_bytes):
    """a public key from its base64 (32 bytes, or the 44-byte DER), a PEM, or DER bytes"""
    data = text_or_bytes
    if isinstance(data, str):
        s = data.strip()
        if "BEGIN PUBLIC KEY" in s:
            s = "".join(ln for ln in s.splitlines() if ln and "-----" not in ln)
        data = base64.b64decode(s)
    if len(data) == 44 and data.startswith(ED25519_DER_PREFIX):
        data = data[12:]
    if len(data) != 32:
        raise ReleaseError("Not an Ed25519 public key.", "bad_key")
    return data


def trusted_keys(path=None):
    """[(id, 32 bytes)] from config/release_keys.json: {"keys": [{"id", "ed25519" (base64), "added", "note"}]}"""
    try:
        with open(path or KEYS_FILE, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return []
    out = []
    for k in data.get("keys", []):
        try:
            pk = public_key_from(k["ed25519"])
        except (KeyError, ValueError, ReleaseError):
            continue
        if k.get("id") and k["id"] != key_id(pk):
            continue   # a key whose id does not match its bytes: not trusted
        if not k.get("revoked"):
            out.append((key_id(pk), pk))
    return out


# ---------------- the manifest ----------------

VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?$")


def parse_version(v):
    """'1.2.3' / '1.2.3-beta.1' / 'v1.2.3' -> a tuple that orders them (a prerelease before its release)"""
    m = VERSION_RE.match((v or "").lstrip("v"))
    if not m:
        return None
    pre = m.group(4)
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)), 0 if pre else 1,
            tuple(int(x) if x.isdigit() else x for x in (pre or "").split(".")) if pre else ())


def check_manifest(m):
    """the manifest's own rules; returns it, or raises bad_manifest"""
    def bad(why):
        raise ReleaseError(f"The release's manifest is not valid: {why}.", "bad_manifest")
    if not isinstance(m, dict) or m.get("format") != MANIFEST_FORMAT:
        bad("not this launcher's format")
    if not parse_version(m.get("version")):
        bad("its version")
    if m.get("channel") not in ("stable", "prerelease"):
        bad("its channel")
    if m.get("min_launcher") is not None and not parse_version(m.get("min_launcher")):
        bad("its minimum launcher version")
    assets = m.get("assets")
    if not isinstance(assets, list) or not assets:
        bad("no downloads")
    for a in assets:
        if not isinstance(a, dict) or not isinstance(a.get("name"), str) or "/" in a["name"] or "\\" in a["name"] \
                or a.get("platform") not in PLATFORMS or not isinstance(a.get("size"), int) or a["size"] <= 0 \
                or not re.fullmatch(r"[0-9a-f]{64}", str(a.get("sha256", ""))):
            bad(f"a download's record ({a.get('name') if isinstance(a, dict) else a!r})")
    if not isinstance(m.get("notes", ""), str):
        bad("its notes")
    return m


def verify_release(manifest_bytes, signature, keys=None):
    """the manifest, if it is signed by a trusted key and valid; else ReleaseError (unsigned, bad_signature,
    untrusted_key, bad_manifest, no_trusted_keys)"""
    keys = trusted_keys() if keys is None else keys
    if not keys:
        raise ReleaseError("This launcher has no release key to check updates with.", "no_trusted_keys")
    if not signature:
        raise ReleaseError("The release isn't signed.", "unsigned")
    try:
        m = json.loads(manifest_bytes.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        raise ReleaseError("The release's manifest is not readable.", "bad_manifest")
    kid = m.get("key_id") if isinstance(m, dict) else None
    signers = [pk for i, pk in keys if i == kid]
    if not signers:
        raise ReleaseError(f"The release is signed with a key this launcher doesn't trust ({kid}).", "untrusted_key")
    if not any(ed25519_verify(pk, manifest_bytes, signature) for pk in signers):
        raise ReleaseError("The release's signature is not valid: it was altered, or not signed by its key.",
                           "bad_signature")
    return check_manifest(m)


def verify_download(path, asset):
    """a downloaded file against its manifest record (size, SHA-256); ReleaseError bad_download when not"""
    size = os.path.getsize(path)
    if size != asset["size"]:
        raise ReleaseError(f"{asset['name']} is not the size the release says ({size} of {asset['size']} bytes).",
                           "bad_download")
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8 << 20), b""):
            h.update(chunk)
    if h.hexdigest() != asset["sha256"]:
        raise ReleaseError(f"{asset['name']} is not the file the release was signed for (SHA-256).", "bad_download")
    return True
