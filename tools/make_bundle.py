#!/usr/bin/env python3
"""Developers: assemble the setup app's download, for Linux or Windows (x86_64).

    python3 tools/build_deps.py                 # SDL2, glslang (once per version)
    python3 tools/make_bundle.py --prebuilt     # + the runtime library (after runtime changes)
    python3 tools/make_bundle.py [--platform linux|windows|all]

Makes build/dist/DotHeroes-Setup-<platform>-x86_64/ and a .tar.xz (Linux) / .zip (Windows) of it:
    DotHeroes-Setup[.exe]   the app (PyInstaller, with its Python inside)
    _internal/              the app's Python runtime and modules
    toolchain/zig-*/        the C/C++ compiler (Zig), trimmed to the target used
    lib/                    the runtime library (ps3recomp + this port's patches,
                            prebuilt), glslang, SDL2, OpenAL Soft and FFmpeg (installed
                            next to the game)
    src/                    what is compiled against the player's recompiled code:
                            the port's main program, the runtime's PPU scaffold, headers
    tools/, config/         the lifting tools (ps3recomp's and this project's) and the
                            per-game configuration they read
    licenses/               third-party licences; licenses/source/ OpenAL Soft's and
                            FFmpeg's source (LGPL: the corresponding source of the
                            libraries shipped; FFmpeg's with its build driver and
                            configuration, licenses/source/FFmpeg-build/)
Nothing from the game is in it: players convert their own disc. tools/boundary_check.py
checks the folder before it is archived, and the archive after.

The Windows app is made by Windows Python + PyInstaller running under Wine, in the
prefix build/wine (C:\\Python314, with pycryptodome); Windows Zig is in build/toolchain/win/.
"""
import argparse
import ast
import glob
import os
import shutil
import subprocess
import sys
import tarfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import zig_build  # noqa: E402
from version import VERSION  # noqa: E402

DIST = os.path.join(ROOT, "build", "dist")
ZIG_VERSION = "0.17.0"
PS3RECOMP = os.environ.get("PS3RECOMP_DIR", os.path.join(ROOT, "build", "ref", "ps3recomp"))
APP = "DotHeroes-Setup"

# per platform: Zig's C headers and libc sources to keep (the rest is other targets).
# Unlike a C-only port, libc++ / libc++abi / libunwind stay: the game is C++.
KEEP = {
    "linux": {"include": {"any-linux-any", "generic-glibc", "x86-linux-gnu", "x86-linux-any"},
              "libc": {"glibc", "include"}},
    "windows": {"include": {"any-windows-any"}, "libc": {"mingw", "include"}},
}
ZIG_DROP = ("doc", "docs", "lldb", "libtsan")
# this project's tools the setup runs (the rest are developer tools)
OUR_TOOLS = ["fix_functions.py", "iso9660.py", "self_decrypt.py", "setup_pipeline.py", "zig_build.py",
             "shortcuts.py", "self_update.py", "version.py", "gen_spu_native.py", "spu_native_task.cpp.in"]


def copy_tracked(rel, dst, skip=()):
    """The files git tracks under ROOT/rel copied to dst (keeping the layout), never untracked ones: a file made
    from the player's game (a generated task, a lift) lying in the folder cannot reach a bundle. skip: path parts
    or name patterns left out."""
    import fnmatch
    r = subprocess.run(["git", "-C", ROOT, "ls-files", "-z", "--", rel], capture_output=True)
    files = [f for f in r.stdout.decode("utf-8").split("\0") if f] if r.returncode == 0 else None
    if not files:
        sys.exit(f"{rel}: no tracked files (the bundle is made from a git checkout)")
    for f in files:
        sub = os.path.relpath(f, rel).replace(os.sep, "/")
        if any(fnmatch.fnmatch(part, pat) for part in sub.split("/") for pat in skip):
            continue
        os.makedirs(os.path.dirname(os.path.join(dst, sub)) or dst, exist_ok=True)
        shutil.copy2(os.path.join(ROOT, f), os.path.join(dst, sub))


def need(path, hint="see this script's docstring"):
    if not os.path.exists(path):
        sys.exit(f"missing {path} ({hint})")
    return path


def winpath(p):
    """a Linux path as Wine's Z: drive sees it"""
    return "Z:" + os.path.abspath(p).replace("/", "\\")


def tool_imports():
    """The standard-library modules the lifting tools import. They run as scripts
    inside the frozen app (setup_app.py run_script), which only carries modules
    something imports at build time: these are added explicitly."""
    mods = set()
    files = glob.glob(os.path.join(PS3RECOMP, "tools", "*.py")) + [os.path.join(ROOT, "tools", t) for t in OUR_TOOLS]
    local = {os.path.splitext(os.path.basename(f))[0] for f in files}
    for f in files:
        try:
            tree = ast.parse(open(f, encoding="utf-8").read())
        except SyntaxError:
            continue
        for n in ast.walk(tree):
            if isinstance(n, ast.Import):
                mods.update(a.name for a in n.names)
            elif isinstance(n, ast.ImportFrom) and n.module and not n.level:
                mods.add(n.module)
    stdlib = getattr(sys, "stdlib_module_names", set())
    return sorted(m for m in mods if m.split(".")[0] in stdlib and m.split(".")[0] not in local
                  and not m.startswith(("tkinter", "idlelib", "test", "turtle", "curses", "msvcrt", "winreg",
                                        "_winapi", "nt", "posix", "pwd", "grp", "fcntl", "termios", "resource")))


def pyinstaller(platform, dist):
    work = os.path.join(ROOT, "build", "pyinstaller", platform)
    os.makedirs(work, exist_ok=True)
    paths = {"dist": dist, "work": work, "spec": work, "tools": os.path.join(ROOT, "tools"),
             "app": os.path.join(ROOT, "setup_app.py"), "icon": os.path.join(work, "app.ico")}
    from PIL import Image
    Image.open(os.path.join(ROOT, "assets", "icon.png")).save(paths["icon"], sizes=[(256, 256), (48, 48), (32, 32),
                                                                                   (16, 16)])
    args = ["--noconfirm", "--onedir", "--windowed", "--name", APP, "--icon", None]
    if platform == "windows":
        paths = {k: winpath(v) for k, v in paths.items()}
        env = dict(os.environ, WINEPREFIX=os.path.join(ROOT, "build", "wine"), WINEDEBUG="-all")
        cmd = ["wine", "C:\\Python314\\python.exe", "-m", "PyInstaller"]
    else:
        env = None
        cmd = [need(os.path.join(ROOT, "build", "venv", "bin", "pyinstaller"),
                    "python3 -m venv build/venv && build/venv/bin/pip install pyinstaller pycryptodome pillow")]
    args[args.index(None)] = paths["icon"]
    hidden = ["setup_pipeline", "zig_build", "iso9660", "self_decrypt", "shortcuts", "self_update", "version",
              "Crypto.Cipher.AES", "Crypto.Util.Counter"]
    hidden += tool_imports()
    subprocess.check_call(cmd + args + [
        "--distpath", paths["dist"], "--workpath", paths["work"], "--specpath", paths["spec"],
        "--paths", paths["tools"], *[a for m in hidden for a in ("--hidden-import", m)], paths["app"]], env=env)


def copy_zig(platform, out):
    src = (os.path.join(ROOT, "build", "toolchain", "win", f"zig-x86_64-windows-{ZIG_VERSION}") if platform == "windows"
           else os.path.join(ROOT, "build", "toolchain", f"zig-x86_64-linux-{ZIG_VERSION}"))
    need(src, f"unpack Zig {ZIG_VERSION} for {platform} there")
    dst = os.path.join(out, "toolchain", os.path.basename(src))
    shutil.copytree(src, dst, symlinks=True, ignore=shutil.ignore_patterns(*ZIG_DROP))
    libc = os.path.join(dst, "lib", "libc")
    for d in os.listdir(libc):
        if d not in KEEP[platform]["libc"] and os.path.isdir(os.path.join(libc, d)):
            shutil.rmtree(os.path.join(libc, d))
    inc = os.path.join(libc, "include")
    for d in os.listdir(inc):
        if d not in KEEP[platform]["include"]:
            shutil.rmtree(os.path.join(inc, d))
    return src


def copy_sources(out, lib_dir):
    """src/: the port's main program and the runtime's headers and PPU scaffold"""
    src = os.path.join(out, "src")
    copy_tracked("port", os.path.join(src, "port"), skip=("CMakeLists.txt", "LICENSE*", "win"))
    ps3 = os.path.join(src, "ps3recomp")
    for top in ("include", "runtime", "libs"):
        for d, _, fs in os.walk(os.path.join(PS3RECOMP, top)):
            if "/tests" in d.replace(os.sep, "/"):
                continue
            for f in fs:
                rel = os.path.relpath(os.path.join(d, f), PS3RECOMP)
                keep = f.endswith((".h", ".hpp", ".inc")) or rel.replace(os.sep, "/") in (
                    zig_build.SCAFFOLD + ["runtime/spu/spu_dispatch_mt.c"])
                if keep:
                    os.makedirs(os.path.dirname(os.path.join(ps3, rel)), exist_ok=True)
                    shutil.copy2(os.path.join(d, f), os.path.join(ps3, rel))
    os.makedirs(os.path.join(src, "generated"))
    shutil.copy2(need(os.path.join(lib_dir, "src", "ppu_hle_nids.cpp"), "make_bundle.py --prebuilt"),
                 os.path.join(src, "generated"))


def copy_tools(out):
    t = os.path.join(out, "tools")
    os.makedirs(t)
    for f in OUR_TOOLS:
        shutil.copy2(os.path.join(ROOT, "tools", f), t)
    shutil.copytree(os.path.join(PS3RECOMP, "tools"), os.path.join(t, "ps3recomp"),
                    ignore=shutil.ignore_patterns("test_*", "tests", "__pycache__", "*.pyc", "host_window"))
    # gen_spu_native.py's per-task configurations (setup_pipeline.spu_native)
    copy_tracked("tools/spu_native", os.path.join(t, "spu_native"))
    copy_tracked("config", os.path.join(out, "config"), skip=("deps.sha256",))
    os.makedirs(os.path.join(out, "assets"))
    shutil.copy2(os.path.join(ROOT, "assets", "icon.png"), os.path.join(out, "assets"))


def licenses(out, zsrc, platform):
    lic = os.path.join(out, "licenses")
    os.makedirs(lic)
    shutil.copy2(os.path.join(ROOT, "LICENSE"), os.path.join(out, "LICENSE.txt"))  # this project: GPL-3.0
    deps = os.path.join(ROOT, "build", "deps", "src")
    for name, path in (("ps3recomp.txt", os.path.join(PS3RECOMP, "LICENSE")),
                       ("at3_standalone-LGPL-2.1.txt", os.path.join(PS3RECOMP, "third_party", "at3_standalone",
                                                                   "COPYING.LGPLv2.1")),
                       ("zig.txt", os.path.join(zsrc, "LICENSE")),
                       ("SDL2.txt", os.path.join(deps, "SDL2-2.32.10", "LICENSE.txt")),
                       ("glslang.txt", os.path.join(deps, "glslang-16.6.0", "LICENSE.txt")),
                       ("Vulkan-Headers.txt", os.path.join(deps, "Vulkan-Headers-vulkan-sdk-1.4.363.0",
                                                           "LICENSE.md")),
                       ("Bullet.txt", os.path.join(deps, "bullet-2.74", "LICENSE"))):
        for p in [path] + glob.glob(os.path.splitext(path)[0] + "*"):
            if os.path.isfile(p):
                shutil.copy2(p, os.path.join(lic, name))
                break
        else:
            print(f"  warning: no licence file for {name} ({path})")
    import build_deps
    build_deps.openal_licences(lic)
    arc, digest = build_deps.openal_source_archive(os.path.join(lic, "source"))
    with open(os.path.join(lic, build_deps.OPENAL_NOTICE), "w", encoding="utf-8", newline="\n") as f:
        f.write(build_deps.openal_notice(digest))
    # FFmpeg: its licence and notice, and its source -- the release archive, the build driver and the
    # configuration as built for this platform
    import build_ffmpeg
    deps_lib = os.path.join(build_deps.DEPS, platform, build_ffmpeg.LIB_FILES[platform][0])
    for name in (build_ffmpeg.LICENCE[1], build_ffmpeg.NOTICE):
        shutil.copy2(os.path.join(deps_lib, name), lic)
    build_ffmpeg.source_files(os.path.join(lic, "source"), platform)


def bundle(platform):
    name = f"{APP}-{platform}-x86_64"
    out = os.path.join(DIST, name)
    lib_dir = os.path.join(ROOT, "build", "lib", platform)
    need(os.path.join(lib_dir, zig_build.RUNTIME_LIB), "make_bundle.py --prebuilt")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(DIST, exist_ok=True)
    print(f"== {platform}: app (PyInstaller)", flush=True)
    tmp = os.path.join(DIST, "pyi-" + platform)
    shutil.rmtree(tmp, ignore_errors=True)
    pyinstaller(platform, tmp)
    os.rename(os.path.join(tmp, APP), out)
    shutil.rmtree(tmp)
    print("toolchain (Zig, trimmed)", flush=True)
    zsrc = copy_zig(platform, out)
    print("libraries, sources, tools, licences", flush=True)
    shutil.copytree(lib_dir, os.path.join(out, "lib"), ignore=shutil.ignore_patterns("src"))
    copy_sources(out, lib_dir)
    copy_tools(out)
    licenses(out, zsrc, platform)
    needs = ("Windows 10 or 11 and a graphics driver with Vulkan (any recent AMD, NVIDIA or Intel driver)."
             if platform == "windows" else
             "a Vulkan-capable graphics driver and glibc 2.31 or newer\n"
             "(Ubuntu 20.04, Debian 11, Fedora 32 or anything more recent).")
    with open(os.path.join(out, "README.txt"), "w", newline="\r\n" if platform == "windows" else "\n") as f:
        f.write(f"Cubic.Heroes (3D Dot Game Heroes Recomp) - native PC port setup, version {VERSION}\n\n"
                f"Run {APP}{zig_build.exe_suffix(platform)}, choose your own disc (a decrypted ISO of the US\n"
                "release BLUS-30490, or its folder), your keys file (or an EBOOT.elf you decrypted) and an\n"
                "install folder, press Convert, then Play.\n\n"
                "Nothing from the game is included: the game is rebuilt from your disc on your machine.\n"
                f"The game needs {needs}\n")
    print("boundary check (the folder)", flush=True)
    import boundary_check
    if boundary_check.check_artifact(out, bundle=True):
        sys.exit(f"{out}: the boundary check failed; nothing archived")
    print("archive", flush=True)
    if platform == "windows":
        arc = out + ".zip"
        with zipfile.ZipFile(arc, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for d, _, fs in os.walk(out):
                for f in fs:
                    p = os.path.join(d, f)
                    z.write(p, os.path.join(name, os.path.relpath(p, out)))
    else:
        arc = out + ".tar.xz"
        with tarfile.open(arc, "w:xz", preset=6) as t:
            t.add(out, arcname=name)
    if boundary_check.check_artifact(arc, bundle=True):
        os.remove(arc)
        sys.exit(f"{arc}: the boundary check failed; the archive was removed")
    size = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(out) for f in fs)
    print(f"{out} ({size / 2**20:.0f} MB)\n{arc} ({os.path.getsize(arc) / 2**20:.0f} MB)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--platform", choices=["linux", "windows", "all"], default="all")
    ap.add_argument("--prebuilt", action="store_true", help="(re)build build/lib/<platform> first")
    a = ap.parse_args()
    for p in (["linux", "windows"] if a.platform == "all" else [a.platform]):
        if a.prebuilt:
            zig_build.build_prebuilt(p, PS3RECOMP, os.path.join(ROOT, "build", "gen", "recompiled"))
        bundle(p)


if __name__ == "__main__":
    main()
