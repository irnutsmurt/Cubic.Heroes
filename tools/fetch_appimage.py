#!/usr/bin/env python3
"""The AppImage pieces (M11.4, the owner's approval of 2026-11-18): the official AppImage type-2 runtime, verified,
and the corresponding sources the packages carry with it.

    python3 tools/fetch_appimage.py

Downloads, only from the official repositories, into build/deps/src/appimage/:
- the runtime (AppImage/type2-runtime, release 20251108 -- never the rolling "continuous"), its detached signature
  and the release's signing key;
- the runtime's source at the release's commit, and what its build links statically: libfuse 3.15.0 (LGPL-2.1, with
  the runtime's own patch inside its source) and squashfuse 0.5.2 (the hashes the runtime's own build script pins);
- squashfs-tools 4.7.5 (GPL-2.0): mksquashfs, built by tools/linux_package.py, for the player's game AppImage.
Every file must match its SHA-256 in config/deps.sha256 (required: nothing is pinned on first use here), and the
runtime's signature must verify with the release's key, whose fingerprint is pinned below, in a keyring of its own
(the developer's is never touched). Only then is the runtime copied to build/deps/linux/appimage/runtime-x86_64, the
one place tools/make_appimage.py takes it from. Nothing downloaded here is executed here.
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "build", "deps", "src", "appimage")
OUT = os.path.join(ROOT, "build", "deps", "linux", "appimage")
RUNTIME_TAG = "20251108"
RUNTIME_COMMIT = "dd6cebedcbddde9c82f89b011e8e1d40b6e43868"
# the "AppImage type 2 runtime" signing key (EdDSA): the key in the release and in the repository
SIGNING_FPR = "570C77ACEA40C0F1B758902CBF96CCA56490F695"
REL = f"https://github.com/AppImage/type2-runtime/releases/download/{RUNTIME_TAG}"
FILES = [   # (manifest name, file, url)
    (f"AppImage-runtime-x86_64-{RUNTIME_TAG}", "runtime-x86_64", f"{REL}/runtime-x86_64"),
    (f"AppImage-runtime-x86_64-{RUNTIME_TAG}.sig", "runtime-x86_64.sig", f"{REL}/runtime-x86_64.sig"),
    ("AppImage-runtime-signing-pubkey", "signing-pubkey.asc", f"{REL}/signing-pubkey.asc"),
    ("AppImage-type2-runtime-source-dd6cebed", f"type2-runtime-{RUNTIME_COMMIT[:8]}.tar.gz",
     f"https://codeload.github.com/AppImage/type2-runtime/tar.gz/{RUNTIME_COMMIT}"),
    ("libfuse-3.15.0", "fuse-3.15.0.tar.xz",
     "https://github.com/libfuse/libfuse/releases/download/fuse-3.15.0/fuse-3.15.0.tar.xz"),
    ("squashfuse-0.5.2", "squashfuse-0.5.2.tar.gz", "https://github.com/vasi/squashfuse/archive/0.5.2.tar.gz"),
    ("squashfs-tools-4.7.5", "squashfs-tools-4.7.5.tar.gz",
     "https://github.com/plougher/squashfs-tools/releases/download/4.7.5/squashfs-tools-4.7.5.tar.gz"),
    # the licences of what else the runtime links statically, at the versions its upstream build (Alpine 3.21) had
    ("musl-COPYRIGHT-1.2.5", "musl-COPYRIGHT-1.2.5.txt", "https://git.musl-libc.org/cgit/musl/plain/COPYRIGHT?h=v1.2.5"),
    ("zstd-LICENSE-1.5.6", "zstd-LICENSE-1.5.6.txt", "https://raw.githubusercontent.com/facebook/zstd/v1.5.6/LICENSE"),
    ("zlib-LICENSE-1.3.1", "zlib-LICENSE-1.3.1.txt", "https://raw.githubusercontent.com/madler/zlib/v1.3.1/LICENSE"),
    ("mimalloc-LICENSE-2.1.7", "mimalloc-LICENSE-2.1.7.txt",
     "https://raw.githubusercontent.com/microsoft/mimalloc/v2.1.7/LICENSE"),
]


def pins():
    out = {}
    with open(os.path.join(ROOT, "config", "deps.sha256"), encoding="utf-8") as f:
        for line in f:
            if line.strip() and not line.startswith("#"):
                digest, name = line.split()
                out[name] = digest
    return out


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def fetch_all():
    known = pins()
    os.makedirs(SRC, exist_ok=True)
    for name, fname, url in FILES:
        want = known.get(name)
        if not want:
            sys.exit(f"[appimage] {name}: no SHA-256 in config/deps.sha256 (required before it is downloaded)")
        path = os.path.join(SRC, fname)
        if not os.path.exists(path) or sha256(path) != want:
            print(f"[appimage] downloading {url}", flush=True)
            req = urllib.request.Request(url, headers={"User-Agent": "Cubic.Heroes-build"})
            with urllib.request.urlopen(req, timeout=120) as r, open(path + ".part", "wb") as f:
                shutil.copyfileobj(r, f, 1 << 20)
            os.replace(path + ".part", path)
        got = sha256(path)
        if got != want:
            os.remove(path)
            sys.exit(f"[appimage] {fname}: sha256 {got} is not the pinned {want}; removed")
    return {fname: os.path.join(SRC, fname) for _, fname, _ in FILES}


def verify_signature(files):
    """the runtime's detached signature, by the pinned key only, in a throwaway keyring"""
    gpg = shutil.which("gpg")
    if not gpg:
        sys.exit("[appimage] gpg is needed to verify the runtime's signature")
    with tempfile.TemporaryDirectory(prefix="appimage-gnupg-") as home:
        os.chmod(home, 0o700)
        env = dict(os.environ, GNUPGHOME=home)
        subprocess.run([gpg, "--batch", "--quiet", "--import", files["signing-pubkey.asc"]], env=env, check=True,
                       capture_output=True)
        listed = subprocess.run([gpg, "--batch", "--with-colons", "--fingerprint"], env=env, capture_output=True,
                                text=True, check=True).stdout
        fprs = [ln.split(":")[9] for ln in listed.splitlines() if ln.startswith("fpr:")]
        if not fprs or fprs[0] != SIGNING_FPR:
            sys.exit(f"[appimage] the release's key is {fprs[:1]}, not the pinned {SIGNING_FPR}")
        r = subprocess.run([gpg, "--batch", "--status-fd", "1", "--verify", files["runtime-x86_64.sig"],
                            files["runtime-x86_64"]], env=env, capture_output=True, text=True)
        valid = [ln.split()[2] for ln in r.stdout.splitlines() if ln.startswith("[GNUPG:] VALIDSIG")]
        if r.returncode != 0 or not valid or valid[0] != SIGNING_FPR:
            sys.exit(f"[appimage] the runtime's signature did not verify with {SIGNING_FPR}:\n{r.stdout}{r.stderr}")
    print(f"[appimage] runtime {RUNTIME_TAG}: sha256 pinned, signature good (key {SIGNING_FPR})")


def main():
    files = fetch_all()
    verify_signature(files)
    os.makedirs(OUT, exist_ok=True)
    dst = os.path.join(OUT, "runtime-x86_64")
    shutil.copy2(files["runtime-x86_64"], dst)
    os.chmod(dst, 0o755)
    print(f"[appimage] {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
