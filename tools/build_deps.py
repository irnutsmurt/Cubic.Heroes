#!/usr/bin/env python3
"""Developers: the third-party libraries the port links, built with the bundled Zig.

    python3 tools/build_deps.py [--platform linux|windows|all]

Downloads pinned sources into build/deps/src/ (checked against SHA-256) and
builds, per platform, into build/deps/<platform>/:
    SDL2      shared (libSDL2-2.0.so.0 | SDL2.dll + libSDL2.dll.a): the
              window and input, and on Linux OpenAL Soft's audio output (its
              SDL2 backend; the game's own cellAudio output is retired since
              M8.4). Installed next to the game. On Linux its X11 / Wayland /
              audio backends are loaded at run time, so the system's headers
              are needed only here.
    glslang   static (libglslang.a and friends): the game's shaders, translated
              to SPIR-V at run time.
    Vulkan    headers only: the backend loads the driver's loader at run time.
    Bullet    2.74 source only (zlib licence): port/CMakeLists.txt compiles
              the narrowphase files the native collision task
              (port/native/bullet_cd.cpp) uses into its own library.
    OpenAL Soft  1.25.2, shared (OpenAL32.dll | libopenal.so.1; LGPL-2.0-or-later):
              the game's audio (docs/OPENAL_MIGRATION_PLAN.md), loaded by the
              game at run time from its own folder. The release archive from
              openal-soft.org, checked on 2026-11-11 to hold the same tree as the
              project's tag 1.25.2 (commit b2c48f7) on GitHub. Built without its
              HRTF data set (the game never used HRTF, and the set's terms do not
              address redistribution): the file is removed from the unpacked
              tree, which is what make_bundle.py ships as the library's source.
              Linux: the SDL2 backend as well (SDL2 ships beside the game), and
              PipeWire, PulseAudio and ALSA when their headers are on the host.
    FFmpeg    9.0.2, shared (avutil-61.dll, avcodec-63.dll, avformat-63.dll |
              libavutil.so.61, libavcodec.so.63, libavformat.so.63;
              LGPL-2.1-or-later): the movies (docs/FFMPEG_MIGRATION_PLAN.md),
              loaded by the game at run time from its own folder. The Bink
              demuxer and decoders only; built by tools/build_ffmpeg.py.
Linux targets glibc 2.31 (Ubuntu 20.04, Debian 11 and newer); Windows the
MinGW ABI. Nothing here comes from the game.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import zig_build  # noqa: E402

DEPS = os.path.join(ROOT, "build", "deps")
SOURCES = {
    "SDL2": ("https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz",
             "SDL2-2.32.10", None),
    "glslang": ("https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz",
                "glslang-16.6.0", None),
    "Vulkan-Headers": ("https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz",
                       "Vulkan-Headers-vulkan-sdk-1.4.363.0", None),
    # the game's Bullet is 2.73 (docs/BULLET_NATIVE.md); 2.74 is the reference
    # compiled for the native collision task, checked against the game's
    "Bullet": ("https://storage.googleapis.com/google-code-archive-downloads/v2/code.google.com/bullet/bullet-2.74.tgz",
               "bullet-2.74", None),
    "OpenAL-Soft": ("https://openal-soft.org/openal-releases/openal-soft-1.25.2.tar.bz2", "openal-soft-1.25.2", None),
    # M8 (docs/FFMPEG_MIGRATION_PLAN.md): FFmpeg 9.0.2 from the project's official GitHub mirror (ffmpeg.org refused
    # connections from the build host on 2026-11-13); the tag n9.0.2 is commit 946fcce07b6dcd0331c8cc609192aeff5e1924f8
    "FFmpeg": ("https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n9.0.2.tar.gz", "FFmpeg-n9.0.2", None),
}
# OpenAL Soft's build: the library alone, as a DLL / shared object (LGPL: dynamically linked). No utilities,
# examples or tests; no configuration, HRTF or preset files installed and no HRTF data embedded; no legacy EAX
# API (the game's reverb is EFX); no git version lookup (a release archive).
OPENAL_OPTIONS = ["-DLIBTYPE=SHARED", "-DALSOFT_UTILS=OFF", "-DALSOFT_NO_CONFIG_UTIL=ON", "-DALSOFT_EXAMPLES=OFF",
                  "-DALSOFT_TESTS=OFF", "-DALSOFT_INSTALL_CONFIG=OFF", "-DALSOFT_INSTALL_HRTF_DATA=OFF",
                  "-DALSOFT_EMBED_HRTF_DATA=OFF", "-DALSOFT_INSTALL_AMBDEC_PRESETS=OFF",
                  "-DALSOFT_INSTALL_EXAMPLES=OFF", "-DALSOFT_INSTALL_UTILS=OFF", "-DALSOFT_UPDATE_BUILD_VERSION=OFF",
                  "-DALSOFT_EAX=OFF", "-DALSOFT_ENABLE_MODULES=OFF",
                  "-DCMAKE_CXX_SCAN_FOR_MODULES=OFF"]   # C++20, no modules: no dependency scanner needed
# data files of the release archive the build does not use and the project does not redistribute
OPENAL_DROP = ["hrtf/Default HRTF.mhr"]


def openal_options(platform):
    if platform == "windows":
        return OPENAL_OPTIONS + ["-DALSOFT_REQUIRE_WASAPI=ON"]
    return OPENAL_OPTIONS + ["-DALSOFT_BACKEND_SDL2=ON", "-DALSOFT_REQUIRE_SDL2=ON",
                             "-DSDL2_DIR=" + os.path.join(DEPS, platform, "lib", "cmake", "SDL2"),
                             "-DCMAKE_INSTALL_RPATH=$ORIGIN", "-DCMAKE_SHARED_LINKER_FLAGS=-s"]   # no debug info


# what the game loads at run time (port/audio/al_api.cpp), per platform: (where build_deps installs it, its name)
OPENAL_LIB = {"windows": ("bin", "OpenAL32.dll"), "linux": ("lib", "libopenal.so.1")}
OPENAL_NOTICE = "OpenAL-Soft-NOTICE.txt"
OPENAL_LICENCES = [("COPYING", "OpenAL-Soft-LGPL-2.0.txt"), ("BSD-3Clause", "OpenAL-Soft-BSD-3-Clause.txt"),
                   ("LICENSE-pffft", "OpenAL-Soft-pffft.txt"), ("fmt-11.2.0/LICENSE", "OpenAL-Soft-fmt.txt"),
                   ("gsl/LICENSE", "OpenAL-Soft-gsl.txt"), ("gsl/ThirdPartyNotices.txt", "OpenAL-Soft-gsl-notices.txt")]


def openal_archive_name():
    return SOURCES["OpenAL-Soft"][1] + ".tar.xz"


def openal_notice(source_sha256=None):
    url, top, _ = SOURCES["OpenAL-Soft"]
    upstream = pins().get("OpenAL-Soft", "?")
    return (f"OpenAL Soft {top.rsplit('-', 1)[1]} -- the game's audio library\n\n"
            "OpenAL32.dll (Windows) / lib/libopenal.so.1 (Linux) is OpenAL Soft, copyright (C) Chris Robinson and\n"
            "contributors, licensed under the GNU Library General Public License version 2 or (at your option) any\n"
            "later version (OpenAL-Soft-LGPL-2.0.txt); parts are under the BSD 3-Clause licence, PFFFT's licence,\n"
            "and the MIT licences of {fmt} and Microsoft GSL (the other OpenAL-Soft-*.txt files). It is not\n"
            "modified. The game loads it at run time from its own folder (dynamic linking): you may replace it with\n"
            "another build of OpenAL Soft.\n\n"
            f"Source: {url}\n  (sha256 {upstream}), built by this project's tools/build_deps.py with zig's clang,\n"
            "  CMake options:" + "".join(f"\n    {o}" for o in OPENAL_OPTIONS) + "\n"
            "  and on Windows -DALSOFT_REQUIRE_WASAPI=ON; on Linux -DALSOFT_BACKEND_SDL2=ON (the SDL2 shipped with\n"
            "  the game), rpath $ORIGIN, stripped.\n"
            f"The corresponding source is shipped with the setup as licenses/source/{openal_archive_name()}"
            + (f"\n  (sha256 {source_sha256})" if source_sha256 else "") + ":\n"
            f"  the release archive with {', '.join(OPENAL_DROP)} left out -- a data set the build does not use\n"
            "  (it embeds and installs no HRTF data) and this project does not redistribute.\n")


def openal_source_archive(out_dir):
    """licenses/source/: the release archive less OPENAL_DROP, as .tar.xz (members, names and times kept); its
    sha256. The tree build_deps built from is the same (openal_source)."""
    fetch("OpenAL-Soft")
    url, top, _ = SOURCES["OpenAL-Soft"]
    src = os.path.join(DEPS, "src", os.path.basename(url))
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, openal_archive_name())
    drop = {top + "/" + d for d in OPENAL_DROP}
    with tarfile.open(src) as t, tarfile.open(out, "w:xz", preset=9) as o:
        for m in t.getmembers():
            if m.name in drop:
                continue
            o.addfile(m, t.extractfile(m) if m.isfile() else None)
    return out, hashlib.sha256(open(out, "rb").read()).hexdigest()


def openal_licences(out_dir):
    """The licence files of the release tree, under the names OPENAL_LICENCES gives."""
    tree = fetch("OpenAL-Soft")
    os.makedirs(out_dir, exist_ok=True)
    for rel, name in OPENAL_LICENCES:
        shutil.copy2(os.path.join(tree, rel), os.path.join(out_dir, name))


def openal_source():
    """The unpacked release with OPENAL_DROP removed: what is built and what ships as its source."""
    tree = fetch("OpenAL-Soft")
    for rel in OPENAL_DROP:
        p = os.path.join(tree, rel)
        if os.path.exists(p):
            os.remove(p)
    return tree
PINS = os.path.join(ROOT, "config", "deps.sha256")


def pins():
    out = {}
    if os.path.exists(PINS):
        for line in open(PINS):
            if line.strip() and not line.startswith("#"):
                digest, name = line.split()
                out[name] = digest
    return out


def fetch(name):
    """The unpacked source tree of `name`, downloading it on first use. A new
    download's hash is recorded in config/deps.sha256; later ones must match."""
    url, top, _ = SOURCES[name]
    src = os.path.join(DEPS, "src")
    tree = os.path.join(src, top)
    if os.path.isdir(tree):
        return tree
    os.makedirs(src, exist_ok=True)
    archive = os.path.join(src, os.path.basename(url))
    if not os.path.exists(archive):
        print(f"[deps] downloading {url}", flush=True)
        urllib.request.urlretrieve(url, archive + ".part")
        os.replace(archive + ".part", archive)
    digest = hashlib.sha256(open(archive, "rb").read()).hexdigest()
    known = pins()
    if name in known and known[name] != digest:
        sys.exit(f"[deps] {archive}: sha256 {digest} does not match config/deps.sha256")
    if name not in known:
        with open(PINS, "a", newline="\n") as f:   # LF on every host (.gitattributes)
            if not known:
                f.write("# sha256 of the third-party source archives tools/build_deps.py builds\n")
            f.write(f"{digest} {name}\n")
    with tarfile.open(archive) as t:
        t.extractall(src, filter="data")
    return tree


def cmake_build(name, src, platform, args, host_headers=False):
    """host_headers: on Linux, the system's headers after zig's own (SDL2's X11 and
    Wayland backends need theirs; nothing else should see them)."""
    work = os.path.join(DEPS, "work", platform, name)
    prefix = os.path.join(DEPS, platform)
    tc = zig_build.cmake_toolchain(platform, extra_include_after=("/usr/include",) if host_headers and platform == "linux" else (),
                                   cpu=zig_build.PORTABLE_CPU)
    subprocess.check_call(["cmake", "-S", src, "-B", work, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                           f"-DCMAKE_TOOLCHAIN_FILE={tc}", f"-DCMAKE_INSTALL_PREFIX={prefix}",
                           "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"] + args, stdout=subprocess.DEVNULL)
    subprocess.check_call(["cmake", "--build", work, "--target", "install", "--parallel", "4"], stdout=subprocess.DEVNULL)
    return prefix


def build(platform):
    print(f"[deps] {platform}: SDL2", flush=True)
    cmake_build("SDL2", fetch("SDL2"), platform,
                ["-DSDL_SHARED=ON", "-DSDL_STATIC=OFF", "-DSDL_TEST=OFF", "-DSDL2_DISABLE_SDL2MAIN=ON",
                 "-DSDL_TESTS=OFF"], host_headers=True)
    print(f"[deps] {platform}: glslang", flush=True)
    cmake_build("glslang", fetch("glslang"), platform,
                # SPIRV/doc.cpp at -O0: see tools/cmake/glslang_doc_O0.cmake
                ["-DCMAKE_PROJECT_INCLUDE=" + os.path.join(ROOT, "tools", "cmake", "glslang_doc_O0.cmake"),
                 "-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON", "-DBUILD_SHARED_LIBS=OFF", "-DENABLE_OPT=OFF", "-DGLSLANG_TESTS=OFF",
                 "-DENABLE_GLSLANG_BINARIES=OFF", "-DGLSLANG_ENABLE_INSTALL=ON"])
    inc = os.path.join(DEPS, platform, "include")
    dst = os.path.join(inc, "vulkan")
    if not os.path.isdir(dst):
        shutil.copytree(os.path.join(fetch("Vulkan-Headers"), "include", "vulkan"), dst)
        shutil.copytree(os.path.join(fetch("Vulkan-Headers"), "include", "vk_video"), os.path.join(inc, "vk_video"))
    fetch("Bullet")   # source only: built with the port (BULLET_DIR)
    print(f"[deps] {platform}: OpenAL Soft", flush=True)
    cmake_build("OpenAL-Soft", openal_source(), platform, openal_options(platform), host_headers=True)
    print(f"[deps] {platform}: FFmpeg", flush=True)
    import build_ffmpeg   # here: it imports this module
    build_ffmpeg.build(platform)
    print(f"[deps] {platform}: done -> {os.path.relpath(os.path.join(DEPS, platform), ROOT)}", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--platform", choices=("linux", "windows", "all"), default="all")
    a = ap.parse_args()
    for p in ("linux", "windows") if a.platform == "all" else (a.platform,):
        build(p)


if __name__ == "__main__":
    main()
