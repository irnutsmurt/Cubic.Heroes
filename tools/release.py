#!/usr/bin/env python3
"""The owner's release signing (M9.3; docs/RELEASE_SIGNING.md). The private key never leaves the owner's keeping:
it is made and used here, on the owner's own machine, with OpenSSL's command line (Git for Windows ships one) --
never in the repository, a build machine or CI.

    py -3 tools/release.py keygen --out DIR
        a new Ed25519 key: DIR/release-signing-key.pem, encrypted with a passphrase OpenSSL asks for, and the public
        key's entry for config/release_keys.json (printed: add it in a commit of its own)
    py -3 tools/release.py manifest --version 0.2.0 [--channel stable|prerelease] [--notes NOTES.md]
                                    [--min-launcher 0.2.0] --key-id ID --out release.json  DOWNLOAD...
        the release's manifest: each download's name, platform (from its name), size and SHA-256
    py -3 tools/release.py sign --key DIR/release-signing-key.pem release.json
        release.json.sig beside it (OpenSSL asks for the passphrase); then verified as the launcher will
    py -3 tools/release.py verify release.json [release.json.sig]
        the launcher's own check, with config/release_keys.json
Upload the downloads, release.json and release.json.sig to the GitHub release.
"""
import argparse
import base64
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release_trust as trust  # noqa: E402


def openssl():
    for c in (shutil.which("openssl"), r"C:\Program Files\Git\usr\bin\openssl.exe",
              r"C:\Program Files\Git\mingw64\bin\openssl.exe", r"C:\Program Files\Git\ucrt64\bin\openssl.exe"):
        if c and os.path.exists(c):
            return c
    sys.exit("OpenSSL's command line wasn't found (Git for Windows ships one; or install OpenSSL 3).")


def platform_of(name):
    n = name.lower()
    if "windows" in n or n.endswith(".zip"):
        return "windows-x86_64"
    if "linux" in n or n.endswith((".tar.xz", ".appimage")):
        return "linux-x86_64"
    sys.exit(f"{name}: say its platform in its name (windows / linux)")


def cmd_keygen(a):
    os.makedirs(a.out, exist_ok=True)
    key = os.path.join(a.out, "release-signing-key.pem")
    if os.path.exists(key):
        sys.exit(f"{key} exists: a key is never written over (rotate: docs/RELEASE_SIGNING.md)")
    subprocess.check_call([openssl(), "genpkey", "-algorithm", "ed25519", "-aes256", "-out", key])
    der = subprocess.check_output([openssl(), "pkey", "-in", key, "-pubout", "-outform", "DER"])
    pub = trust.public_key_from(der)
    entry = {"id": trust.key_id(pub), "ed25519": base64.b64encode(pub).decode(), "added": datetime.date.today()
             .isoformat(), "note": a.note}
    with open(os.path.join(a.out, "release-signing-key.pub.json"), "w") as f:
        json.dump(entry, f, indent=1)
    print("private key (keep it offline, with a backup):", key)
    print("add to config/release_keys.json's keys:")
    print(json.dumps(entry, indent=1))


def cmd_manifest(a):
    assets = []
    for p in a.downloads:
        h = hashlib.sha256()
        with open(p, "rb") as f:
            for c in iter(lambda: f.read(8 << 20), b""):
                h.update(c)
        assets.append({"name": os.path.basename(p), "platform": platform_of(os.path.basename(p)),
                       "size": os.path.getsize(p), "sha256": h.hexdigest()})
    notes = open(a.notes, encoding="utf-8").read() if a.notes else ""
    m = {"format": trust.MANIFEST_FORMAT, "version": a.version, "channel": a.channel, "min_launcher": a.min_launcher,
         "published": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"), "notes": notes,
         "key_id": a.key_id, "assets": assets}
    trust.check_manifest(m)
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(m, f, indent=1)
    print(f"{a.out}: {len(assets)} download(s), version {a.version} ({a.channel})")


def cmd_sign(a):
    sig = a.manifest + ".sig"
    subprocess.check_call([openssl(), "pkeyutl", "-sign", "-inkey", a.key, "-rawin", "-in", a.manifest, "-out", sig])
    a.signature = sig
    cmd_verify(a)


def cmd_verify(a):
    with open(a.manifest, "rb") as f:
        mb = f.read()
    with open(a.signature or a.manifest + ".sig", "rb") as f:
        sig = f.read()
    keys = trust.trusted_keys(a.keys) if a.keys else trust.trusted_keys()
    try:
        m = trust.verify_release(mb, sig, keys)
    except trust.ReleaseError as e:
        sys.exit(f"refused ({e.code}): {e}")
    print(f"verified: version {m['version']} ({m['channel']}), signed by {m['key_id']}, {len(m['assets'])} download(s)")


def main():
    p = argparse.ArgumentParser(description="release signing (docs/RELEASE_SIGNING.md)")
    sub = p.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("keygen")
    k.add_argument("--out", required=True)
    k.add_argument("--note", default="the release key")
    m = sub.add_parser("manifest")
    m.add_argument("--version", required=True)
    m.add_argument("--channel", default="stable", choices=("stable", "prerelease"))
    m.add_argument("--notes")
    m.add_argument("--min-launcher")
    m.add_argument("--key-id", required=True)
    m.add_argument("--out", default="release.json")
    m.add_argument("downloads", nargs="+")
    s = sub.add_parser("sign")
    s.add_argument("--key", required=True)
    s.add_argument("--keys", help="the trusted keys file to verify with (default config/release_keys.json)")
    s.add_argument("manifest")
    v = sub.add_parser("verify")
    v.add_argument("--keys")
    v.add_argument("manifest")
    v.add_argument("signature", nargs="?")
    a = p.parse_args()
    if a.cmd == "sign":
        a.signature = None
    {"keygen": cmd_keygen, "manifest": cmd_manifest, "sign": cmd_sign, "verify": cmd_verify}[a.cmd](a)


if __name__ == "__main__":
    main()
