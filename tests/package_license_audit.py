#!/usr/bin/env python3
"""The licensing and redistribution audit of a Linux package (M11.4, the final qualification).

    python3 tests/package_license_audit.py PACKAGE_DIR|TAR.XZ|APPIMAGE

Every shared library and program in the package is matched to the component it comes from, and each component to
the licence texts (and, for the LGPL and GPL ones, the corresponding source) the package must carry. A file no rule
names fails the audit: nothing third-party may ship without its terms. Also checked: the project's own licence and
the notices that explain what was built and how (FFmpeg, OpenAL Soft, the AppImage pieces). Game data and generated
game code are the boundary check's (tools/boundary_check.py); this audit is about third-party terms. Exit 1 on any
finding.
"""
import fnmatch
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile

# component: (file patterns it ships as, licence files that must be in worker/licenses or licenses/, sources)
COMPONENTS = {
    "Cubic.Heroes (the launcher, the worker's code)": (["CubicHeroes"], ["LICENSE.txt"], []),
    "Dear ImGui (in the launcher)": ([], ["Dear-ImGui.txt"], []),
    "SDL2": (["libSDL2-2.0.so*"], ["SDL2.txt"], []),
    "OpenAL Soft": (["libopenal.so*"], ["OpenAL-Soft-LGPL-2.0.txt", "OpenAL-Soft-NOTICE.txt"],
                    ["openal-soft-*.tar.xz"]),
    "FFmpeg": (["libavcodec.so*", "libavformat.so*", "libavutil.so*"], ["FFmpeg-LGPL-2.1.txt", "FFmpeg-NOTICE.txt"],
               ["FFmpeg-n*.tar.gz", "FFmpeg-build"]),
    "OpenSSL": (["libssl.so*", "libcrypto.so*"], ["OpenSSL-Apache-2.0.txt"], []),
    "CPython": (["libpython3*.so*", "*.cpython-3*-x86_64-linux-gnu.so"], ["Python.txt", "Python-incorporated-software.txt"],
                []),
    "libffi": (["libffi.so*"], ["libffi-MIT.txt"], []),
    "bzip2": (["libbz2.so*"], ["bzip2.txt"], []),
    "xz (liblzma)": (["liblzma.so*"], ["xz-liblzma.txt"], []),
    "zlib": (["libz.so*", "zlib*.so"], ["zlib.txt"], []),
    "pycryptodome": (["*.abi3.so"], ["pycryptodome.txt"], []),
    "PyInstaller (the worker's bootloader)": (["DotHeroes-Setup"], ["PyInstaller.txt"], []),
    "ps3recomp, at3_standalone, Bullet (the runtime library)": (["libps3recomp_runtime.a", "libdotheroes_native.a"],
                                                                ["ps3recomp.txt", "at3_standalone-LGPL-2.1.txt",
                                                                 "at3_standalone-NOTICE.txt", "Bullet.txt"],
                                                                ["at3_standalone.tar.gz"]),
    "glslang (the worker's shader translator libraries), Vulkan-Headers": (
        ["libglslang.a", "libglslang-default-resource-limits.a", "libSPIRV.a", "libOSDependent.a",
         "libMachineIndependent.a", "libGenericCodeGen.a"], ["glslang.txt", "Vulkan-Headers.txt"], []),
    "AppImage type-2 runtime (libfuse, squashfuse, zstd, zlib, mimalloc, musl)": (
        ["runtime-x86_64"], ["AppImage-type2-runtime-MIT.txt", "AppImage-NOTICE.txt", "libfuse-LGPL-2.1.txt",
                             "squashfuse-BSD-2-Clause.txt", "zstd-BSD.txt", "mimalloc-MIT.txt", "musl-MIT.txt"],
        ["type2-runtime-*.tar.gz", "fuse-3.*.tar.xz", "squashfuse-*.tar.gz"]),
    "squashfs-tools (mksquashfs)": (["mksquashfs"], ["squashfs-tools-GPL-2.0.txt"], ["squashfs-tools-*.tar.gz"]),
}
ZIG = "worker/toolchain/zig-"   # the Zig toolchain ships whole with its own LICENSE files (checked below)


def is_elf(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def glibc_static(path):
    """M11.4 (the review's L-1): a program with no dynamic section that carries glibc's own strings has glibc linked
    into it; the AppImage runtime (static with musl) has none of them"""
    info = subprocess.run(["readelf", "-d", path], capture_output=True, text=True).stdout
    if "(NEEDED)" in info or "Shared library" in info:
        return False
    with open(path, "rb") as f:
        data = f.read()
    return b"glibc detected" in data or b"GNU C Library" in data or b"GLIBC_PRIVATE" in data


def unpack(arg, tmp):
    if os.path.isdir(arg):
        return arg
    if arg.endswith(".tar.xz"):
        with tarfile.open(arg) as t:
            t.extractall(tmp, filter="data")
        return os.path.join(tmp, "Cubic.Heroes")
    subprocess.run([os.path.abspath(arg), "--appimage-extract"], cwd=tmp, check=True, capture_output=True,
                   env=dict(os.environ, APPIMAGE_EXTRACT_AND_RUN=""))
    return os.path.join(tmp, "squashfs-root")


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    tmp = tempfile.mkdtemp(prefix="licence-audit-")
    try:
        root = unpack(sys.argv[1], tmp)
        lic_dirs = [os.path.join(root, "worker", "licenses"), os.path.join(root, "licenses"), root]
        src_dir = os.path.join(root, "worker", "licenses", "source")
        have = {n for d in lic_dirs if os.path.isdir(d) for n in os.listdir(d)}
        sources = set(os.listdir(src_dir)) if os.path.isdir(src_dir) else set()
        bad, seen = [], {}
        for dp, dns, fns in os.walk(root):
            rel_dir = os.path.relpath(dp, root)
            if (rel_dir + "/").startswith(ZIG):
                continue
            for n in fns:
                p = os.path.join(dp, n)
                rel = os.path.relpath(p, root)
                if rel.startswith("worker/licenses/"):
                    continue
                if not (is_elf(p) or n.endswith(".a")):
                    continue
                if is_elf(p) and glibc_static(p):
                    bad.append(f"{rel}: statically linked with glibc (LGPL code inside the program; link glibc dynamically)")
                comp = next((c for c, (pats, _, _) in COMPONENTS.items() if any(fnmatch.fnmatch(n, q) for q in pats)),
                            None)
                if not comp:
                    bad.append(f"{rel}: no component known for it (its licence unaccounted for)")
                    continue
                seen.setdefault(comp, []).append(rel)
        for comp, (pats, lics, srcs) in COMPONENTS.items():
            if pats and comp not in seen:
                continue   # not in this package
            for l in lics:
                if l not in have:
                    bad.append(f"{comp}: its licence text {l} is missing")
            for sp in srcs:
                if not any(fnmatch.fnmatch(s, sp) for s in sources):
                    bad.append(f"{comp}: its corresponding source {sp} is missing from worker/licenses/source")
        zigs = [d for d in os.listdir(os.path.join(root, "worker", "toolchain")) if d.startswith("zig-")] \
            if os.path.isdir(os.path.join(root, "worker", "toolchain")) else []
        for z in zigs:
            if not os.path.isfile(os.path.join(root, "worker", "toolchain", z, "LICENSE")):
                bad.append(f"the Zig toolchain {z}: its LICENSE is missing")
        if "zig.txt" not in have and zigs:
            bad.append("Zig: worker/licenses/zig.txt is missing")
        for comp in sorted(seen):
            print(f"  {comp}: {len(seen[comp])} file(s)")
        print(f"  the Zig toolchain: {len(zigs)} (its own LICENSE files)")
        for b in bad:
            print("FINDING: " + b)
        print(("PASS" if not bad else "FAIL") + f": {sys.argv[1]}: {sum(len(v) for v in seen.values())} binaries in "
              f"{len(seen)} components, {len(bad)} finding(s)")
        return 1 if bad else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
