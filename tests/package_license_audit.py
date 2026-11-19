#!/usr/bin/env python3
"""The licensing and redistribution audit of a package (M11.4, the final qualification): Linux's, and since the
Windows review's W-7 Windows' (its folder or zip: audit_windows, WIN_COMPONENTS).

    python3 tests/package_license_audit.py PACKAGE_DIR|TAR.XZ|APPIMAGE|ZIP

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


# The Windows package (the Windows review's W-7): its binaries by their path in the package (first rule that matches),
# each component's licence files, sources, and the notices a shared licence file must hold for it. The worker's Python
# is the python.org Windows build: its LICENSE.txt (Python.txt) carries the notices of the libraries that build links
# (OpenSSL's Apache-2.0 text, libffi, bzip2) and Microsoft's redistributable terms; Python-incorporated-software.txt
# (Doc/license.rst) the software CPython builds into its own modules (expat in pyexpat.pyd, libmpdec in _decimal.pyd)
WIN_COMPONENTS = [
    ("Cubic.Heroes (the launcher, the worker's code)", ["CubicHeroes.exe"], ["LICENSE.txt"], [], {}),
    ("Dear ImGui (in the launcher)", [], ["Dear-ImGui.txt"], [], {}),
    ("SDL2", ["SDL2.dll", "worker/lib/SDL2.dll", "worker/lib/libSDL2.dll.a"], ["SDL2.txt"], [], {}),
    ("OpenAL Soft", ["worker/lib/OpenAL32.dll"], ["OpenAL-Soft-LGPL-2.0.txt", "OpenAL-Soft-NOTICE.txt"],
     ["openal-soft-*.tar.xz"], {}),
    ("FFmpeg", ["worker/lib/avcodec-*.dll", "worker/lib/avformat-*.dll", "worker/lib/avutil-*.dll",
                "worker/lib/swresample-*.dll"], ["FFmpeg-LGPL-2.1.txt", "FFmpeg-NOTICE.txt"],
     ["FFmpeg-n*.tar.gz", "FFmpeg-build"], {}),
    ("pycryptodome", ["worker/_internal/Crypto/**"], ["pycryptodome.txt"], [], {}),
    ("OpenSSL (the worker's Python)", ["worker/_internal/libssl-3*.dll", "worker/_internal/libcrypto-3*.dll"],
     ["Python.txt"], [], {"Python.txt": ["Apache License"]}),
    ("libffi (the worker's Python)", ["worker/_internal/libffi-*.dll"], ["Python.txt"], [], {"Python.txt": ["libffi"]}),
    ("Microsoft Visual C++ runtime and UCRT (the worker's Python)",
     ["worker/_internal/VCRUNTIME140*.dll", "worker/_internal/MSVCP140*.dll", "worker/_internal/ucrtbase.dll",
      "worker/_internal/api-ms-win-*.dll"], ["Python.txt"], [], {"Python.txt": ["Microsoft Distributable Code"]}),
    ("CPython (with the software its modules incorporate: expat, libmpdec, bzip2, ...)",
     ["worker/_internal/python3*.dll", "worker/_internal/*.pyd"], ["Python.txt", "Python-incorporated-software.txt"],
     [], {"Python.txt": ["PYTHON SOFTWARE FOUNDATION LICENSE", "bzip2"],
          "Python-incorporated-software.txt": ["expat", "libmpdec"]}),
    ("PyInstaller (the worker's bootloader)", ["worker/DotHeroes-Setup.exe"], ["PyInstaller.txt"], [], {}),
    ("ps3recomp, at3_standalone, Bullet (the runtime library)",
     ["worker/lib/libps3recomp_runtime.a", "worker/lib/libdotheroes_native.a"],
     ["ps3recomp.txt", "at3_standalone-LGPL-2.1.txt", "at3_standalone-NOTICE.txt", "Bullet.txt"], ["at3_standalone.tar.gz"],
     {}),
    ("glslang (the worker's shader translator libraries), Vulkan-Headers",
     ["worker/lib/libglslang*.a", "worker/lib/libSPIRV.a", "worker/lib/libOSDependent.a",
      "worker/lib/libMachineIndependent.a", "worker/lib/libGenericCodeGen.a"], ["glslang.txt", "Vulkan-Headers.txt"], [],
     {}),
]


def path_match(rel, pat):
    """a package path against a rule: `*` within one path segment only (fnmatch's own crosses '/': an extension
    module in a package's folder would pass as CPython's), a final `/**` the whole folder below"""
    if pat.endswith("/**"):
        return rel.startswith(pat[:-2])
    r, q = rel.split("/"), pat.split("/")
    return len(r) == len(q) and all(fnmatch.fnmatch(a, b) for a, b in zip(r, q))


def is_pe(path):
    try:
        with open(path, "rb") as f:
            return f.read(2) == b"MZ"
    except OSError:
        return False


def audit_windows(root, label):
    """the Windows package: every PE program, library and extension module (and every static library) matched to its
    component; each component's licence files and sources present, and the notices it relies on in a shared file"""
    lic_dirs = [os.path.join(root, "worker", "licenses"), os.path.join(root, "licenses"), root]
    have = {n: os.path.join(d, n) for d in reversed(lic_dirs) if os.path.isdir(d) for n in os.listdir(d)}
    src_dir = os.path.join(root, "worker", "licenses", "source")
    sources = set(os.listdir(src_dir)) if os.path.isdir(src_dir) else set()
    bad, seen = [], {}
    for dp, _, fns in os.walk(root):
        rel_dir = os.path.relpath(dp, root).replace(os.sep, "/")
        if (rel_dir + "/").startswith(ZIG) or rel_dir.startswith("worker/licenses"):
            continue
        for n in fns:
            p = os.path.join(dp, n)
            rel = os.path.relpath(p, root).replace(os.sep, "/")
            if not (is_pe(p) or n.endswith(".a")):
                continue
            comp = next((c for c, pats, _, _, _ in WIN_COMPONENTS if any(path_match(rel, q) for q in pats)), None)
            if not comp:
                bad.append(f"{rel}: no component known for it (its licence unaccounted for)")
                continue
            seen.setdefault(comp, []).append(rel)
    texts = {}
    for comp, pats, lics, srcs, needs in WIN_COMPONENTS:
        if pats and comp not in seen:
            continue   # not in this package
        for l in lics:
            if l not in have:
                bad.append(f"{comp}: its licence text {l} is missing")
        for sp in srcs:
            if not any(fnmatch.fnmatch(s, sp) for s in sources):
                bad.append(f"{comp}: its corresponding source {sp} is missing from worker/licenses/source")
        for lf, words in needs.items():
            if lf not in have:
                continue   # reported above
            if lf not in texts:
                with open(have[lf], encoding="utf-8", errors="replace") as f:
                    texts[lf] = f.read()
            for w in words:
                if w not in texts[lf]:
                    bad.append(f"{comp}: {lf} does not hold its notice ({w!r})")
    tc = os.path.join(root, "worker", "toolchain")
    zigs = [d for d in os.listdir(tc) if d.startswith("zig-")] if os.path.isdir(tc) else []
    for z in zigs:
        if not os.path.isfile(os.path.join(tc, z, "LICENSE")):
            bad.append(f"the Zig toolchain {z}: its LICENSE is missing")
    if "zig.txt" not in have and zigs:
        bad.append("Zig: worker/licenses/zig.txt is missing")
    for comp in sorted(seen):
        print(f"  {comp}: {len(seen[comp])} file(s)")
    print(f"  the Zig toolchain: {len(zigs)} (its own LICENSE files)")
    for b in bad:
        print("FINDING: " + b)
    print(("PASS" if not bad else "FAIL") + f": {label}: {sum(len(v) for v in seen.values())} binaries in "
          f"{len(seen)} components, {len(bad)} finding(s)")
    return 1 if bad else 0


def unpack(arg, tmp):
    if os.path.isdir(arg):
        return arg
    if arg.lower().endswith(".zip"):   # the Windows package: its one top folder
        import zipfile
        with zipfile.ZipFile(arg) as z:
            z.extractall(tmp)
        tops = [d for d in os.listdir(tmp) if os.path.isdir(os.path.join(tmp, d))]
        return os.path.join(tmp, tops[0]) if len(tops) == 1 else tmp
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
        if os.path.isfile(os.path.join(root, "CubicHeroes.exe")):   # the Windows package
            return audit_windows(root, sys.argv[1])
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
