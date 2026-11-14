#!/usr/bin/env python3
"""The owner's release signing (M9.3; docs/RELEASE_SIGNING.md). The private key never leaves the owner's keeping:
it is made and used here, on the owner's own machine, with OpenSSL's command line (Git for Windows ships one) --
never in the repository, a build machine or CI.

    py -3 tools/release.py keygen --out DIR
        a new Ed25519 key: DIR/release-signing-key.pem, encrypted with a passphrase OpenSSL asks for, and the public
        key's entry for config/release_keys.json (printed: add it in a commit of its own)
    py -3 tools/release.py pubkey --key DIR/release-signing-key.pem [--out FILE]
        the public key's entry of an existing encrypted private key (its passphrase asked for, never echoed; a wrong one
        may be tried again); the private key is only read -- the recovery when keygen made the key but couldn't
        extract its public key
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
import getpass
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


def _genpkey(key):
    """a new encrypted Ed25519 private key (OpenSSL asks for its passphrase itself)"""
    subprocess.check_call([openssl(), "genpkey", "-algorithm", "ed25519", "-aes256", "-out", key])


def extract_public(key, attempts=3, prompt=getpass.getpass):
    """the 32-byte public key of an encrypted private key file. Its passphrase is asked for (never echoed) and given to
    OpenSSL on its standard input -- never on a command line, in a file or a log; a wrong one may be tried again. The
    private key file is only read."""
    if not os.path.isfile(key):
        sys.exit(f"{key}: no such file")
    for i in range(attempts):
        pw = prompt(f"Passphrase of {os.path.basename(key)}: ")
        r = subprocess.run([openssl(), "pkey", "-in", key, "-passin", "stdin", "-pubout", "-outform", "DER"],
                           input=(pw + "\n").encode("utf-8"), capture_output=True)
        pw = None
        if r.returncode == 0:
            return trust.public_key_from(r.stdout)
        print("That passphrase didn't open the key (or the file isn't an encrypted private key)." +
              (" Try again." if i + 1 < attempts else ""), file=sys.stderr)
    sys.exit("The public key wasn't extracted. The private key is unchanged; run this command again when ready.")


def key_entry(pub, note):
    return {"id": trust.key_id(pub), "ed25519": base64.b64encode(pub).decode(),
            "added": datetime.date.today().isoformat(), "note": note}


def _write_entry(entry, path, key):
    if os.path.abspath(path) == os.path.abspath(key):
        sys.exit("The entry would be written over the private key: choose another --out.")
    with open(path, "w") as f:
        json.dump(entry, f, indent=1)
    print("add to config/release_keys.json's keys:")
    print(json.dumps(entry, indent=1))


def cmd_keygen(a):
    os.makedirs(a.out, exist_ok=True)
    key = os.path.join(a.out, "release-signing-key.pem")
    if os.path.exists(key):
        sys.exit(f"{key} exists: a key is never written over (rotate: docs/RELEASE_SIGNING.md). If its public key was "
                 f"never extracted: py -3 tools/release.py pubkey --key \"{key}\"")
    _genpkey(key)
    print("private key (keep it offline, with a backup):", key)
    try:
        pub = extract_public(key)
    except (SystemExit, OSError, subprocess.SubprocessError, trust.ReleaseError) as e:
        sys.exit(f"The private key was made and is kept: {key}\nIts public key wasn't extracted ({e}). Nothing is "
                 f"lost -- don't make a new key. Run:\n    py -3 tools/release.py pubkey --key \"{key}\"\nIt asks for "
                 f"the passphrase and prints the entry for config/release_keys.json.")
    _write_entry(key_entry(pub, a.note), os.path.join(a.out, "release-signing-key.pub.json"), key)


def cmd_pubkey(a):
    """the public key's entry of an existing encrypted private key; the key only read"""
    pub = extract_public(os.path.abspath(a.key))
    out = a.out or os.path.join(os.path.dirname(os.path.abspath(a.key)), "release-signing-key.pub.json")
    _write_entry(key_entry(pub, a.note), out, a.key)


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
    pk = sub.add_parser("pubkey")
    pk.add_argument("--key", required=True, help="the encrypted private key (only read)")
    pk.add_argument("--out", help="where the entry is written (default: beside the key, release-signing-key.pub.json)")
    pk.add_argument("--note", default="release key")
    v = sub.add_parser("verify")
    v.add_argument("--keys")
    v.add_argument("manifest")
    v.add_argument("signature", nargs="?")
    a = p.parse_args()
    if a.cmd == "sign":
        a.signature = None
    {"keygen": cmd_keygen, "pubkey": cmd_pubkey, "manifest": cmd_manifest, "sign": cmd_sign,
     "verify": cmd_verify}[a.cmd](a)


if __name__ == "__main__":
    main()
