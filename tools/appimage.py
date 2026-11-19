#!/usr/bin/env python3
"""An AppImage from a folder (M11.4): the AppImage type-2 runtime followed by a squashfs image of the folder.

Used for both of this project's AppImages, with the same two programs:
- the launcher's, made by the developer (tools/make_appimage.py) from the Linux package's folder;
- the game's, made on the player's own machine by the worker from the active build (the owner's D2: a thin
  AppImage, never distributed), with the runtime and mksquashfs the worker ships (worker/toolchain/appimage/).
The runtime is the official AppImage type2-runtime release tools/fetch_appimage.py pinned and verified (its GPG
signature, the pinned key); mksquashfs is squashfs-tools 4.7.5, built static (tools/linux_package.py). Nothing is
downloaded and no other packaging tool is needed: an AppImage is made offline.

The folder is the AppDir: AppRun (what the runtime starts), a .desktop file and an icon at its top.
"""
import os
import shutil
import stat
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def tools_dir():
    """where the runtime and mksquashfs are: the worker's toolchain/appimage/ in a package (this file in its tools/),
    the checkout's build/deps/linux/appimage/ otherwise"""
    for d in (os.path.join(ROOT, "toolchain", "appimage"), os.path.join(ROOT, "build", "deps", "linux", "appimage")):
        if os.path.isfile(os.path.join(d, "runtime-x86_64")) and os.path.isfile(os.path.join(d, "mksquashfs")):
            return d
    raise FileNotFoundError("the AppImage runtime and mksquashfs are not here (tools/linux_package.py makes them)")


def write_apprun(appdir, program, env=None):
    """AppRun: starts `program` (relative to the AppDir) with the arguments given to the AppImage. POSIX sh only.
    env: extra variables, their values relative to the AppDir when they start with "$HERE/"."""
    lines = ["#!/bin/sh", "# Cubic.Heroes: what the AppImage's runtime starts (tools/appimage.py)",
             'HERE="$(dirname "$(readlink -f "$0")")"']
    for k, v in (env or {}).items():
        lines.append(f'export {k}="{v}"')
    lines.append(f'exec "$HERE/{program}" "$@"')
    p = os.path.join(appdir, "AppRun")
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    os.chmod(p, 0o755)


def write_desktop(appdir, name, exec_name, icon_name, comment, categories="Game;"):
    p = os.path.join(appdir, icon_name + ".desktop")
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write("[Desktop Entry]\nType=Application\n"
                f"Name={name}\nComment={comment}\nExec={exec_name}\nIcon={icon_name}\n"
                f"Terminal=false\nCategories={categories}\n")
    return p


def build(appdir, out, tools=None, level=19, progress=None):
    """the AppImage `out` from the AppDir `appdir` (written beside as `out`.part, then renamed: an interrupted build
    leaves no AppImage). zstd at `level` (19: small; the runtime reads zstd and gzip). Returns `out`."""
    tools = tools or tools_dir()
    runtime = os.path.join(tools, "runtime-x86_64")
    mksquashfs = os.path.join(tools, "mksquashfs")
    for need in ("AppRun",):
        if not os.path.exists(os.path.join(appdir, need)):
            raise FileNotFoundError(f"{appdir}: no {need}")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    part = out + ".part"
    with tempfile.TemporaryDirectory(dir=os.path.dirname(os.path.abspath(out))) as tmp:
        img = os.path.join(tmp, "image.squashfs")
        cmd = [mksquashfs, appdir, img, "-root-owned", "-noappend", "-comp", "zstd", "-Xcompression-level",
               str(level), "-b", "1M", "-quiet", "-no-progress"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"mksquashfs failed ({r.returncode}): {r.stderr.strip()[-400:]}")
        with open(part, "wb") as o:
            with open(runtime, "rb") as f:
                shutil.copyfileobj(f, o)
            with open(img, "rb") as f:
                shutil.copyfileobj(f, o, 1 << 20)
    os.chmod(part, os.stat(part).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    os.replace(part, out)
    return out


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: appimage.py APPDIR OUT.AppImage")
    print(build(sys.argv[1], sys.argv[2]))
