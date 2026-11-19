#!/usr/bin/env python3
"""The pinned Zig toolchain, downloaded and checked (M11.4: CI and a fresh checkout on either system).

    python3 tools/fetch_zig.py [--platform linux|windows] [--dest DIR]

Downloads Zig ZIG_VERSION for the platform (default: this one) from ziglang.org, checks it against the SHA-256 pinned
below (ziglang.org's own download index for that release) before unpacking anything, and unpacks it into
build/toolchain/zig-x86_64-<os>-<version>/ -- where tools/zig_build.py find_zig() and tools/make_bundle.py look.
A folder already there is kept. Nothing is installed system-wide.
"""
import argparse
import hashlib
import os
import shutil
import sys
import tarfile
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ZIG_VERSION = "0.17.0"   # tools/make_bundle.py ZIG_VERSION
PINS = {   # ziglang.org/download/index.json, release 0.17.0
    "linux": ("zig-x86_64-linux-0.17.0.tar.xz",
              "1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026"),
    "windows": ("zig-x86_64-windows-0.17.0.zip",
                "b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428"),
}


def fetch(platform, dest):
    name, want = PINS[platform]
    top = name.rsplit(".tar.xz", 1)[0].rsplit(".zip", 1)[0]
    final = os.path.join(dest, top)
    exe = os.path.join(final, "zig.exe" if platform == "windows" else "zig")
    if os.path.exists(exe):
        print(f"[zig] {final}: present")
        return final
    os.makedirs(dest, exist_ok=True)
    archive = os.path.join(dest, name)
    url = f"https://ziglang.org/download/{ZIG_VERSION}/{name}"
    print(f"[zig] downloading {url}", flush=True)
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "Cubic.Heroes-build"}),
                                timeout=120) as r, open(archive + ".part", "wb") as f:
        shutil.copyfileobj(r, f, 1 << 20)
    h = hashlib.sha256()
    with open(archive + ".part", "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    if h.hexdigest() != want:
        os.remove(archive + ".part")
        sys.exit(f"[zig] {name}: sha256 {h.hexdigest()} is not the pinned {want}")
    os.replace(archive + ".part", archive)
    if name.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            z.extractall(dest)
    else:
        with tarfile.open(archive) as t:
            t.extractall(dest, filter="data")
    os.remove(archive)
    if not os.path.exists(exe):
        sys.exit(f"[zig] {exe}: not in the archive")
    if platform != "windows":
        os.chmod(exe, 0o755)
    print(f"[zig] {final}: unpacked, sha256 checked")
    return final


def main():
    ap = argparse.ArgumentParser(description="the pinned Zig toolchain")
    ap.add_argument("--platform", choices=sorted(PINS), default="windows" if os.name == "nt" else "linux")
    ap.add_argument("--dest", default=os.path.join(ROOT, "build", "toolchain"))
    a = ap.parse_args()
    fetch(a.platform, a.dest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
