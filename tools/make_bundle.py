#!/usr/bin/env python3
"""Developers: assemble the setup app's download, for Linux or Windows (x86_64).

    python3 tools/build_deps.py                 # SDL2, glslang (once per version)
    python3 tools/make_bundle.py --prebuilt     # + the runtime library (after runtime changes)
    python3 tools/make_bundle.py [--platform linux|windows|all]

Makes build/dist/DotHeroes-Setup-<platform>-x86_64/ and the download, build/dist/Cubic.Heroes-<version>-<platform>-x86_64
.zip (Windows) / .tar.xz (Linux), whose one folder, Cubic.Heroes, is the portable installation root:
    CubicHeroes[.exe]       the launcher (M9.4; tools/build_launcher.py), SDL2's library beside it and
                            licenses/ (Dear ImGui's, SDL2's); it runs the worker below
    LICENSE.txt, README.txt
    worker/                 the pipeline worker the launcher runs (M9.5), holding:
    DotHeroes-Setup[.exe]   the app (PyInstaller, with its Python inside; `--worker`)
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
prefix build/wine (C:\\Python314, with pycryptodome); Windows Zig is in build/toolchain/win/. On Windows itself:
the checkout's own build/venv (PyInstaller) and build/toolchain/zig-x86_64-windows-<version>. The launcher is built
on the platform it is for.
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
             "shortcuts.py", "self_update.py", "version.py", "gen_spu_native.py", "spu_native_task.cpp.in",
             "worker.py", "game_settings.py", "install_layout.py", "updates.py",
             "release_trust.py", "settings.py", "diagnostics.py",
             "pipeline_manifest.py", "uninstall.py", "ps3_disc.py",
             "root_migration.py"]   # the launcher's worker (M9.0-M9.5; root_migration: the portable root)


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
    # Tk is the source checkout's window only (setup_app.py _load_tk): the packaged worker never opens one (M11.3:
    # its runtime, its licence notices and 4 MB left out)
    args = ["--noconfirm", "--onedir", "--windowed", "--name", APP, "--icon", None,
            "--exclude-module", "tkinter", "--exclude-module", "_tkinter"]
    if platform == "windows" and os.name == "nt":   # on Windows itself: the checkout's own venv
        env = None
        cmd = [need(os.path.join(ROOT, "build", "venv", "Scripts", "pyinstaller.exe"),
                    "build/venv/Scripts/pip install pyinstaller pycryptodome pillow")]
    elif platform == "windows":
        paths = {k: winpath(v) for k, v in paths.items()}
        env = dict(os.environ, WINEPREFIX=os.path.join(ROOT, "build", "wine"), WINEDEBUG="-all")
        cmd = ["wine", "C:\\Python314\\python.exe", "-m", "PyInstaller"]
    else:
        env = None
        cmd = [need(os.path.join(ROOT, "build", "venv", "bin", "pyinstaller"),
                    "python3 -m venv build/venv && build/venv/bin/pip install pyinstaller pycryptodome pillow")]
    args[args.index(None)] = paths["icon"]
    hidden = ["setup_pipeline", "zig_build", "iso9660", "self_decrypt", "shortcuts", "self_update", "version",
              "worker", "game_settings", "install_layout", "updates", "release_trust", "settings", "diagnostics",
              "pipeline_manifest", "uninstall", "ps3_disc", "root_migration", "Crypto.Cipher.AES",
              "Crypto.Util.Counter"]
    hidden += tool_imports()
    subprocess.check_call(cmd + args + [
        "--distpath", paths["dist"], "--workpath", paths["work"], "--specpath", paths["spec"],
        "--paths", paths["tools"], *[a for m in hidden for a in ("--hidden-import", m)], paths["app"]], env=env)


def copy_zig(platform, out):
    src = (os.path.join(ROOT, "build", "toolchain", "win", f"zig-x86_64-windows-{ZIG_VERSION}") if platform == "windows"
           else os.path.join(ROOT, "build", "toolchain", f"zig-x86_64-linux-{ZIG_VERSION}"))
    if platform == "windows" and not os.path.isdir(src):   # a Windows checkout keeps it beside the Linux one
        src = os.path.join(ROOT, "build", "toolchain", f"zig-x86_64-windows-{ZIG_VERSION}")
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


def dist_files(sites, dist, name):
    """a package's licence file in its dist-info (licenses/ or the folder itself)"""
    return [f for d in sites for sub in (("licenses",), ()) for f in
            glob.glob(os.path.join(d, dist + ".dist-info", *sub, name))]


def python_runtime_licences(lic, platform):
    """the worker's own runtime, as PyInstaller packs it: Python's licence (with the notices of what its build
    carries: OpenSSL, libffi, zlib, bzip2, ...), pycryptodome's, and PyInstaller's (its bootloader's exception) --
    from the Python the app is made with (M11.3's licence audit)"""
    if platform == "windows" and os.name != "nt":   # made under Wine
        base = os.path.join(ROOT, "build", "wine", "drive_c", "Python314")
        sites = [os.path.join(base, "Lib", "site-packages")]
    else:   # this checkout's venv (make_bundle runs from it)
        base = sys.base_prefix
        import sysconfig
        sites = [sysconfig.get_paths()["purelib"]]
    wanted = {
        "Python.txt": [os.path.join(base, "LICENSE.txt"), os.path.join(base, "LICENSE"),
                       f"/usr/share/doc/python3.{sys.version_info.minor}/copyright"],
        "pycryptodome.txt": dist_files(sites, "pycryptodome*", "LICENSE*"),
        "PyInstaller.txt": dist_files(sites, "pyinstaller-*", "COPYING*"),
    }
    for name, paths in wanted.items():
        for path in paths:
            if os.path.isfile(path):
                shutil.copy2(path, os.path.join(lic, name))
                break
        else:
            raise SystemExit(f"no licence file for {name} (looked at {paths}): the package would ship without it")


def at3_source(lic):
    """at3_standalone (LGPL-2.1, ps3recomp's third_party/at3_standalone, unmodified) is compiled into the runtime
    library the package carries (lib/libps3recomp_runtime.a): its complete source goes with it, as OpenAL Soft's and
    FFmpeg's do (M11.3's licence audit)"""
    src = os.path.join(PS3RECOMP, "third_party", "at3_standalone")
    if not os.path.isdir(src):
        raise SystemExit(f"at3_standalone's source is missing ({src}): the package would ship its code without it")
    os.makedirs(os.path.join(lic, "source"), exist_ok=True)
    arc = os.path.join(lic, "source", "at3_standalone.tar.gz")

    def norm(ti):   # the same archive from the same files: no owner, no time
        ti.uid = ti.gid = 0
        ti.uname = ti.gname = ""
        ti.mtime = 0
        return ti
    with tarfile.open(arc, "w:gz") as t:
        for d, dirs, files in os.walk(src):
            dirs.sort()
            for f in sorted(files):
                full = os.path.join(d, f)
                t.add(full, arcname="at3_standalone/" + os.path.relpath(full, src).replace(os.sep, "/"),
                      recursive=False, filter=norm)
    import re
    rev = re.search(r'^PS3RECOMP_REV = "([0-9a-f]+)"', open(os.path.join(ROOT, "tools", "pipeline.py"),
                    encoding="utf-8").read(), re.M).group(1)
    with open(os.path.join(lic, "at3_standalone-NOTICE.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("at3_standalone -- the ATRAC3/ATRAC3plus decoder from PPSSPP (derived from FFmpeg), LGPL-2.1\n"
                "(at3_standalone-LGPL-2.1.txt). It is part of ps3recomp (github.com/sp00nznet/ps3recomp, revision\n"
                f"{rev}), third_party/at3_standalone, unmodified, and is compiled into the runtime\n"
                "library worker/lib/libps3recomp_runtime.a, which the setup links into the game on your PC.\n"
                "Its complete source: source/at3_standalone.tar.gz.\n")


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
    python_runtime_licences(lic, platform)
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
    at3_source(lic)


def launcher(out, platform):
    """the launcher at the download's root (M9.5): built for this platform, SDL2's library beside it, its licences"""
    if platform != zig_build.HOST:
        sys.exit(f"the launcher is built on the platform it is for: run this on {platform} (this is {zig_build.HOST})")
    built = os.path.join(ROOT, "build", "launcher")
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "build_launcher.py"), "--out", built])
    shutil.copy2(os.path.join(built, "CubicHeroes" + zig_build.exe_suffix(platform)), out)
    if platform == "windows":
        shutil.copy2(os.path.join(built, "SDL2.dll"), out)
    else:
        shutil.copytree(os.path.join(built, "lib"), os.path.join(out, "lib"))
    lic = os.path.join(out, "licenses")
    os.makedirs(lic, exist_ok=True)
    shutil.copy2(os.path.join(built, "licenses", "Dear-ImGui-LICENSE.txt"), os.path.join(lic, "Dear-ImGui.txt"))
    shutil.copy2(os.path.join(built, "licenses", "SDL2-LICENSE.txt"), os.path.join(lic, "SDL2.txt"))
    shutil.copy2(os.path.join(ROOT, "LICENSE"), os.path.join(out, "LICENSE.txt"))   # this project: GPL-3.0


def bundle(platform):
    name = f"{APP}-{platform}-x86_64"
    out = os.path.join(DIST, name)
    work = os.path.join(out, "worker")   # the launcher's pipeline worker: the frozen app and what it runs
    lib_dir = os.path.join(ROOT, "build", "lib", platform)
    need(os.path.join(lib_dir, zig_build.RUNTIME_LIB), "make_bundle.py --prebuilt")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    print(f"== {platform}: app (PyInstaller)", flush=True)
    tmp = os.path.join(DIST, "pyi-" + platform)
    shutil.rmtree(tmp, ignore_errors=True)
    pyinstaller(platform, tmp)
    os.rename(os.path.join(tmp, APP), work)
    shutil.rmtree(tmp)
    print("toolchain (Zig, trimmed)", flush=True)
    zsrc = copy_zig(platform, work)
    print("libraries, sources, tools, licences", flush=True)
    shutil.copytree(lib_dir, os.path.join(work, "lib"), ignore=shutil.ignore_patterns("src"))
    copy_sources(work, lib_dir)
    copy_tools(work)
    licenses(work, zsrc, platform)
    print("the launcher", flush=True)
    launcher(out, platform)
    needs = ("Windows 10 or 11 and a graphics driver with Vulkan (any recent AMD, NVIDIA or Intel driver)."
             if platform == "windows" else
             "a Vulkan-capable graphics driver and glibc 2.31 or newer\n"
             "(Ubuntu 20.04, Debian 11, Fedora 32 or anything more recent).")
    with open(os.path.join(out, "README.txt"), "w", newline="\r\n" if platform == "windows" else "\n") as f:
        f.write(f"Cubic.Heroes (3D Dot Game Heroes Recomp) - native PC port, version {VERSION}\n\n"
                f"Run CubicHeroes{zig_build.exe_suffix(platform)}, the launcher: it installs the game from your own\n"
                "disc (an ISO of the US release BLUS-30490 -- decrypted, or encrypted with its disc key -- or\n"
                "its folder) and its keys file (or an EBOOT.elf you decrypted), keeps it up to date, and plays\n"
                "it. A controller does everything.\n"
                "worker/ holds what the launcher runs to convert and build the game.\n\n"
                "Nothing from the game is included: the game is rebuilt from your disc on your machine.\n"
                f"The game needs {needs}\n")
    # the package's top items are exactly the ones the worker knows as the launcher's (install_layout.
    # LAUNCHER_PACKAGE): the portable root keeps the install beside them, and the launcher's update and removal
    # touch only these
    import install_layout
    top, want = sorted(os.listdir(out)), sorted(install_layout.launcher_package(platform))
    if top != want:
        sys.exit(f"{out}: the package's top items {top} are not install_layout.LAUNCHER_PACKAGE's {want}")
    print("boundary check (the folder)", flush=True)
    import boundary_check
    if boundary_check.check_artifact(out, bundle=True):
        sys.exit(f"{out}: the boundary check failed; nothing archived")
    # M11.3: nothing of the developer's own (a home path with the user name, the private literals of
    # .git/push-guard.local) in what is shipped -- the prebuilt libraries once carried the build machine's
    # user name through the compiler's cache path
    print("private-information check (the folder)", flush=True)
    guard = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "push_guard.py"), "package", out])
    if guard.returncode == 1:
        sys.exit(f"{out}: private information found; nothing archived")
    if guard.returncode != 0:
        print("  warning: no private literals on this machine (.git/push-guard.local): only home paths with them "
              "are looked for, so nothing was checked")
    print("archive", flush=True)
    # M11.3: what a player downloads is named for the game and its version, and unpacks to one Cubic.Heroes folder
    # -- the portable installation root (docs/CLI.md); the launcher's updates go by the signed manifest's names
    top = "Cubic.Heroes"
    stem = os.path.join(DIST, f"Cubic.Heroes-{VERSION}-{platform}-x86_64")
    if platform == "windows":
        arc = stem + ".zip"
        with zipfile.ZipFile(arc, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for d, _, fs in os.walk(out):
                for f in fs:
                    p = os.path.join(d, f)
                    z.write(p, os.path.join(top, os.path.relpath(p, out)))
    else:
        arc = stem + ".tar.xz"
        with tarfile.open(arc, "w:xz", preset=6) as t:
            t.add(out, arcname=top)
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
