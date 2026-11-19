#!/usr/bin/env python3
"""The player's game as an AppImage (M11.4; the owner's D2): made on the player's own machine, from the active build,
never distributed.

    <root>/exports/Cubic.Heroes-Game-x86_64.AppImage

A thin AppImage: the active build's program, its build.cfg and its lib/ (tens of MB) -- the converted game data, the
saves, the settings and the caches stay in the installation root, never copied. Its start script (AppRun) finds the
root, in this order:
  1. CUBIC_INSTALL_DIR, as the launcher takes it;
  2. cubicheroes-root.txt beside the .AppImage file (one line, a folder; a relative one from the file's folder);
  3. the folder above the .AppImage's when that holds an installation -- exports/ inside the root: so the root and
     its AppImage moved or copied together keep working (relative, nothing absolute recorded);
  4. the root it was made in, when the AppImage was copied elsewhere on its own.
and starts the game there as `CubicHeroes --play` does (direct play: no launcher to return to), its arguments passed
on. Remade whenever the active build changes (an install, an update, a rollback: tools/worker.py), offline, with the
runtime and mksquashfs the worker ships (tools/appimage.py). Linux only.
"""
import os
import shutil
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import appimage  # noqa: E402

NAME = "Cubic.Heroes-Game-x86_64.AppImage"
SLOT_FILES = ("dotheroes", "build.cfg", "build.json")


def path_of(root):
    return os.path.join(os.path.abspath(root), "exports", NAME)


def _sh_quote(s):
    return "'" + s.replace("'", "'\\''") + "'"


def apprun(made_root):
    return f"""#!/bin/sh
# Cubic.Heroes's game, the player's own AppImage (tools/game_appimage.py): finds the installation, then plays there
HERE="$(dirname "$(readlink -f "$0")")"
IMG="${{APPIMAGE:-$HERE/AppRun}}"
D="$(dirname "$IMG")"
if [ -n "$CUBIC_INSTALL_DIR" ]; then
    ROOT="$CUBIC_INSTALL_DIR"
elif [ -f "$D/cubicheroes-root.txt" ]; then
    ROOT="$(head -n 1 "$D/cubicheroes-root.txt")"
    case "$ROOT" in /*) ;; *) ROOT="$D/$ROOT" ;; esac
elif [ -f "$D/../active.json" ]; then
    ROOT="$D/.."
else
    ROOT={_sh_quote(made_root)}
fi
if [ ! -f "$ROOT/active.json" ]; then
    echo "Cubic.Heroes: no installation found for this AppImage (looked at $ROOT)." >&2
    echo "Put cubicheroes-root.txt naming the installation folder beside it, or set CUBIC_INSTALL_DIR." >&2
    exit 3
fi
DOTHEROES_INSTALL_ROOT="$(cd "$ROOT" && pwd)"
export DOTHEROES_INSTALL_ROOT
export CUBIC_DIRECT_PLAY=1
unset CUBIC_LAUNCHER CUBIC_LAUNCHER_PID
cd "$DOTHEROES_INSTALL_ROOT" || exit 3
exec "$HERE/dotheroes" "$@"
"""


def make(lay, log=print):
    """the AppImage of `lay`'s active build (an install_layout.Layout); returns its path. Raises on failure (the
    caller decides whether that fails its operation)."""
    if os.name == "nt":
        return None
    st = lay.read_active()
    slot = lay.build_dir(st["active"])
    out = path_of(lay.root)
    with tempfile.TemporaryDirectory(prefix="game-appimage-", dir=os.path.dirname(out) if os.path.isdir(
            os.path.dirname(out)) else None) as tmp:
        appdir = os.path.join(tmp, "AppDir")
        os.makedirs(appdir)
        for f in SLOT_FILES:
            if os.path.exists(os.path.join(slot, f)):
                shutil.copy2(os.path.join(slot, f), appdir)
        shutil.copytree(os.path.join(slot, "lib"), os.path.join(appdir, "lib"), symlinks=True)
        with open(os.path.join(appdir, "AppRun"), "w", encoding="utf-8", newline="\n") as f:
            f.write(apprun(os.path.abspath(lay.root)))
        os.chmod(os.path.join(appdir, "AppRun"), 0o755)
        appimage.write_desktop(appdir, "Cubic.Heroes (game)", "AppRun", "cubicheroes-game",
                               "3D Dot Game Heroes, recompiled on this PC from your own disc")
        icon = os.path.join(slot, "icon.png")
        if os.path.exists(icon):
            shutil.copy2(icon, os.path.join(appdir, "cubicheroes-game.png"))
            os.symlink("cubicheroes-game.png", os.path.join(appdir, ".DirIcon"))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        appimage.build(appdir, out, level=15)
    log(f"the game's AppImage: {os.path.relpath(out, lay.root)} (build {st['active']}, "
        f"{os.path.getsize(out) / 2**20:.0f} MB)")
    return out
