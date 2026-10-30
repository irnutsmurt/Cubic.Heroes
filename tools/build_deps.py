#!/usr/bin/env python3
"""Developers: the third-party libraries the port links, built with the bundled Zig.

    python3 tools/build_deps.py [--platform linux|windows|all]

Downloads pinned sources into build/deps/src/ (checked against SHA-256) and
builds, per platform, into build/deps/<platform>/:
    SDL2      shared (libSDL2-2.0.so.0 | SDL2.dll + libSDL2.dll.a): the
              window, input and audio. Installed next to the game. On Linux its
              X11 / Wayland / audio backends are loaded at run time, so the
              system's headers are needed only here.
    glslang   static (libglslang.a and friends): the game's shaders, translated
              to SPIR-V at run time.
    Vulkan    headers only: the backend loads the driver's loader at run time.
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
}
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
        with open(PINS, "a") as f:
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
    tc = zig_build.cmake_toolchain(platform, extra_include_after=("/usr/include",) if host_headers and platform == "linux" else ())
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
    print(f"[deps] {platform}: done -> {os.path.relpath(os.path.join(DEPS, platform), ROOT)}", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--platform", choices=("linux", "windows", "all"), default="all")
    a = ap.parse_args()
    for p in ("linux", "windows") if a.platform == "all" else (a.platform,):
        build(p)


if __name__ == "__main__":
    main()
