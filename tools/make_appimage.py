#!/usr/bin/env python3
"""The launcher's AppImage (M11.4; docs/LINUX_PACKAGING.md), from the same folder as the Linux tarball.

    python3 tools/make_appimage.py

Takes build/dist/DotHeroes-Setup-linux-x86_64/ (tools/make_bundle.py --platform linux, run by
tools/linux_package.py), adds AppRun (starting CubicHeroes with the arguments given), the .desktop entry and the
project's icon, and makes build/dist/Cubic.Heroes-<version>-x86_64.AppImage with tools/appimage.py. The two packages
hold the same files: the AppImage is then unpacked again (`--appimage-extract`) and compared with the folder, file
for file, and the unpacked files go through the boundary check and the privacy scan, as the tarball does.

The AppImage's installation root is a Cubic.Heroes folder beside the .AppImage file (launcher/main.cpp resolve_root,
$APPIMAGE); the image itself is read-only and never written.
"""
import filecmp
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import appimage  # noqa: E402
from version import VERSION  # noqa: E402

DIST = os.path.join(ROOT, "build", "dist")
PACKAGE = os.path.join(DIST, "DotHeroes-Setup-linux-x86_64")
ICON = "cubicheroes"
APPDIR_EXTRA = ("AppRun", ICON + ".desktop", ICON + ".png", ".DirIcon")


def compare(a, b):
    """[] when the trees hold the same files with the same bytes and modes, else the differences"""
    diffs = []
    for d, dirs, files in os.walk(a):
        rel = os.path.relpath(d, a)
        for f in files:
            pa, pb = os.path.join(d, f), os.path.join(b, rel, f)
            if not os.path.lexists(pb):
                diffs.append(f"missing {os.path.join(rel, f)}")
            elif os.path.islink(pa) != os.path.islink(pb) or (
                    not os.path.islink(pa) and (not filecmp.cmp(pa, pb, shallow=False) or
                                                (os.stat(pa).st_mode & 0o111) != (os.stat(pb).st_mode & 0o111))):
                diffs.append(f"differs {os.path.join(rel, f)}")
    for d, dirs, files in os.walk(b):
        rel = os.path.relpath(d, b)
        for f in files:
            if rel == "." and f in APPDIR_EXTRA:
                continue
            if not os.path.lexists(os.path.join(a, rel, f)):
                diffs.append(f"extra {os.path.join(rel, f)}")
    return diffs


def main():
    if not os.path.isdir(PACKAGE):
        sys.exit(f"{PACKAGE}: make the Linux package first (tools/linux_package.py)")
    out = os.path.join(DIST, f"Cubic.Heroes-{VERSION}-x86_64.AppImage")
    with tempfile.TemporaryDirectory(dir=DIST) as tmp:
        appdir = os.path.join(tmp, "Cubic.Heroes.AppDir")
        shutil.copytree(PACKAGE, appdir, symlinks=True)
        appimage.write_apprun(appdir, "CubicHeroes")
        appimage.write_desktop(appdir, "Cubic.Heroes", "CubicHeroes", ICON,
                               "3D Dot Game Heroes Recomp: native PC port of the PS3 version (static recompilation)")
        shutil.copy2(os.path.join(ROOT, "assets", "icon.png"), os.path.join(appdir, ICON + ".png"))
        os.symlink(ICON + ".png", os.path.join(appdir, ".DirIcon"))
        print(f"[appimage] {out}", flush=True)
        appimage.build(appdir, out)
        # the same files as the tarball's folder: unpacked again and compared
        check = os.path.join(tmp, "check")
        os.makedirs(check)
        subprocess.run([out, "--appimage-extract"], cwd=check, check=True, stdout=subprocess.DEVNULL)
        unpacked = os.path.join(check, "squashfs-root")
        diffs = compare(PACKAGE, unpacked)
        if diffs:
            os.remove(out)
            sys.exit("[appimage] the AppImage does not hold the package's files:\n  " + "\n  ".join(diffs[:20]))
        print("[appimage] unpacked again: the same files as the package's folder (bytes and executable bits)")
        import boundary_check
        if boundary_check.check_artifact(unpacked, bundle=True):
            os.remove(out)
            sys.exit("[appimage] the boundary check failed; the AppImage was removed")
        guard = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "push_guard.py"), "package", unpacked])
        if guard.returncode == 1:
            os.remove(out)
            sys.exit("[appimage] private information found; the AppImage was removed")
    print(f"[appimage] {out} ({os.path.getsize(out) / 2**20:.0f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
