"""The environment for the system's own programs, started from the frozen worker (M11.4).

PyInstaller's bootloader puts the worker's own _internal/ first on LD_LIBRARY_PATH on Linux. The worker's own helper
processes (the lifting tools, run by this same program) need it: a child of the frozen program reuses its parent's
set-up, and found no libffi when it was taken away. Every other program must not inherit it -- the game (its --info,
--memtest and --prepare: Mesa's Vulkan driver would find the worker's copies of zlib, zstd and libffi before the
system's), Steam, gio, xdg-user-dir: they get the player's own value back (kept by PyInstaller as
LD_LIBRARY_PATH_ORIG), or none. Elsewhere than a frozen Linux worker, the environment as it is.
"""
import os
import sys


def host_env(env=None):
    """a copy of `env` (os.environ by default) fit for a program of the system's or the player's"""
    e = dict(os.environ if env is None else env)
    if getattr(sys, "frozen", False) and sys.platform.startswith("linux"):
        orig = e.pop("LD_LIBRARY_PATH_ORIG", None)
        if orig:
            e["LD_LIBRARY_PATH"] = orig
        else:
            e.pop("LD_LIBRARY_PATH", None)
    return e
