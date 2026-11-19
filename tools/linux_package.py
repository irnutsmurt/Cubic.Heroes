#!/usr/bin/env python3
"""The Linux packages, made in the pinned build environment (M11.4; docs/LINUX_PACKAGING.md).

    python3 tools/linux_package.py [--steps zig,appimage-pieces,deps,mksquashfs,prebuilt,bundle,appimage]

On the developer's machine it runs itself inside tools/linux_build_env (Debian 11, glibc 2.31; built once with
`docker build -t cubic-heroes-linux-build tools/linux_build_env`), with the checkout mounted at /src: a neutral path,
so nothing built records the developer's own folders (M11.3: compiler caches had put the build machine's user name
into the shipped libraries), and as the developer's user, so build/ never holds files root owns. Inside it (or with
--here, already inside) the steps run in order:

  zig              the pinned Zig (tools/fetch_zig.py)
  appimage-pieces  the AppImage runtime and the sources it and mksquashfs come from, verified (tools/fetch_appimage.py;
                   run on the host before the container, where gpg is)
  deps             SDL2, OpenAL Soft, FFmpeg, glslang... against this baseline's headers (tools/build_deps.py)
  mksquashfs       squashfs-tools 4.7.5's mksquashfs, gzip and zstd only, fully static: the player's machine makes its
                   game AppImage with it, with nothing installed
  prebuilt         the runtime library in the release configuration (tools/zig_build.py build_prebuilt)
  bundle           the worker frozen with this environment's Python, the launcher, the tarball (tools/make_bundle.py)
  appimage         the launcher's AppImage from the same folder (tools/make_appimage.py)

The game itself is never in a package: players build it from their own disc.
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
IMAGE = "cubic-heroes-linux-build"
STEPS = ["zig", "appimage-pieces", "deps", "mksquashfs", "prebuilt", "bundle", "appimage"]
HOST_STEPS = {"appimage-pieces"}   # gpg verification on the developer's machine, before anything uses the runtime
APPIMAGE_OUT = os.path.join(ROOT, "build", "deps", "linux", "appimage")
SQUASHFS_TOOLS = "squashfs-tools-4.7.5"


def s_zig():
    import fetch_zig
    fetch_zig.fetch("linux", os.path.join(ROOT, "build", "toolchain"))


def s_appimage_pieces():
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "fetch_appimage.py")])


# the run-time backends each shipped library must have (M11.4: built in the container without them once, which no
# offscreen test can see -- the game had no window under X11): (library, names it must carry for dlopen)
BACKENDS = {
    "libSDL2-2.0.so.0": ("libX11.so.6", "libXext.so.6", "libXcursor.so.1", "libXi.so.6", "libXrandr.so.2",
                         "libXss.so.1", "libwayland-client.so.0", "libwayland-egl.so.1", "libxkbcommon.so.0",
                         "libudev.so.1"),
    "libopenal.so.1": ("libpipewire-0.3.so.0", "libpulse.so.0", "libasound.so.2"),
}


def check_backends(lib_dir):
    """every backend's library name in the built libraries (they dlopen them by name); exit on any missing"""
    missing = []
    for lib, names in BACKENDS.items():
        with open(os.path.join(lib_dir, lib), "rb") as f:
            blob = f.read()
        missing += [f"{lib}: {n}" for n in names if n.encode() + b"\0" not in blob]
    if missing:
        sys.exit("[deps] run-time backends missing (the game would lack a window system or an audio server):\n  "
                 + "\n  ".join(missing))
    print(f"[deps] run-time backends present: SDL2 (X11, Wayland, udev), OpenAL Soft (PipeWire, PulseAudio, ALSA)")


def s_deps():
    """from clean: a package's libraries are never built over the host's work folders (other paths, other headers)"""
    import build_deps
    shutil.rmtree(os.path.join(build_deps.DEPS, "work", "linux"), ignore_errors=True)
    out = os.path.join(build_deps.DEPS, "linux")
    for name in (os.listdir(out) if os.path.isdir(out) else []):
        if name != "appimage":   # the verified runtime and mksquashfs (appimage-pieces, mksquashfs) stay
            p = os.path.join(out, name)
            shutil.rmtree(p) if os.path.isdir(p) and not os.path.islink(p) else os.remove(p)
    build_deps.build("linux")
    check_backends(os.path.join(build_deps.DEPS, "linux", "lib"))


def s_mksquashfs():
    """mksquashfs from the pinned source (fetch_appimage.py checked it): gzip and zstd only -- what the AppImage
    runtime reads -- linked fully static against this baseline, so it needs nothing on the player's machine"""
    import tarfile
    src_tar = os.path.join(ROOT, "build", "deps", "src", "appimage", SQUASHFS_TOOLS + ".tar.gz")
    work = os.path.join(ROOT, "build", "deps", "work", "linux", SQUASHFS_TOOLS)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    with tarfile.open(src_tar) as t:
        t.extractall(work, filter="data")
    d = os.path.join(work, SQUASHFS_TOOLS, "squashfs-tools")
    subprocess.check_call(["make", "-C", d, f"-j{os.cpu_count() or 4}", "mksquashfs", "GZIP_SUPPORT=1",
                           "ZSTD_SUPPORT=1", "XZ_SUPPORT=0", "LZO_SUPPORT=0", "LZ4_SUPPORT=0", "LZMA_XZ_SUPPORT=0",
                           "XATTR_SUPPORT=0", "EXTRA_LDFLAGS=-static"])
    os.makedirs(APPIMAGE_OUT, exist_ok=True)
    out = os.path.join(APPIMAGE_OUT, "mksquashfs")
    shutil.copy2(os.path.join(d, "mksquashfs"), out)
    subprocess.check_call(["strip", out])
    info = subprocess.run(["file", out], capture_output=True, text=True).stdout.strip()
    if "statically linked" not in info:
        sys.exit(f"[mksquashfs] not static: {info}")
    print(f"[mksquashfs] {out}: {info.split(': ', 1)[1][:90]}")


def s_prebuilt():
    import zig_build
    shutil.rmtree(os.path.join(ROOT, "build", "rel-linux"), ignore_errors=True)   # from clean, as the libraries
    zig_build.build_prebuilt("linux", os.path.join(ROOT, "build", "ref", "ps3recomp"),
                             os.path.join(ROOT, "build", "gen", "recompiled"))


def s_bundle():
    import make_bundle
    make_bundle.bundle("linux")


def s_appimage():
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "make_appimage.py")])


FNS = {"zig": s_zig, "appimage-pieces": s_appimage_pieces, "deps": s_deps, "mksquashfs": s_mksquashfs,
       "prebuilt": s_prebuilt, "bundle": s_bundle, "appimage": s_appimage}


def in_container(steps):
    uid, gid = os.getuid(), os.getgid()
    cmd = ["docker", "run", "--rm", "--user", f"{uid}:{gid}", "-e", "HOME=/tmp", "-v", f"{ROOT}:/src", "-w", "/src",
           IMAGE, "python3", "tools/linux_package.py", "--here", "--steps", ",".join(steps)]
    print("[linux-package] in " + IMAGE + ": " + ",".join(steps), flush=True)
    subprocess.check_call(cmd)


def main():
    ap = argparse.ArgumentParser(description="the Linux packages, in the pinned build environment")
    ap.add_argument("--steps", default=",".join(STEPS))
    ap.add_argument("--here", action="store_true", help="already inside the build environment")
    a = ap.parse_args()
    steps = a.steps.split(",")
    for s in steps:
        if s not in FNS:
            sys.exit(f"unknown step {s} (the steps: {', '.join(STEPS)})")
    if a.here:
        for s in steps:
            print(f"== {s}", flush=True)
            FNS[s]()
        return 0
    host = [s for s in steps if s in HOST_STEPS]
    for s in host:
        print(f"== {s} (this machine)", flush=True)
        FNS[s]()
    rest = [s for s in steps if s not in HOST_STEPS]
    if rest:
        in_container(rest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
