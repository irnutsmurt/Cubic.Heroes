#!/usr/bin/env python3
"""The game's settings file, <install>/dotheroes.cfg: NAME=value lines the game reads at start
(port/main.cpp read_settings). Shared by the setup app and the pipeline worker (tools/worker.py); moved here
from setup_app.py unchanged so the worker needs no window toolkit. M9.1 replaces it with the authoritative
configuration of docs/M9_LAUNCHER_PLAN.md section 8.9."""
import os


def game_settings_path(install):
    """<install>/config/dotheroes.cfg in an install with build slots (M9.1: independent of the builds), else the
    setup app's <install>/dotheroes.cfg"""
    if os.path.exists(os.path.join(install, "active.json")) or os.path.isdir(os.path.join(install, "config")):
        return os.path.join(install, "config", "dotheroes.cfg")
    return os.path.join(install, "dotheroes.cfg")


def read_game_settings(install):
    """<install>/dotheroes.cfg: NAME=value lines the game reads at start (port/main.cpp)"""
    out = {}
    try:
        with open(game_settings_path(install)) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, v = line.split("=", 1)
                    out[k.strip()] = v.strip()
    except OSError:
        pass
    return out


# The settings that hold 60 fps in debris-heavy fights (docs/HANDOFF.md,
# "Performance configuration"): native collision and five native solver
# commands (the set-up's two since 2026-11-10, docs/SPU_NON_AUDIO.md section 1),
# the capped command-ring lap, one present per flip. Without them an
# install fights at p99 ~27 ms (10-16% of frames over 18 ms); with them 17.3-17.5,
# as in development. Experimental: native collision is not bit-exact with the
# game's SPU task. Written for a new install and added to an older one's
# settings if missing; a player's own value is never replaced.
PERFORMANCE = {"DOTHEROES_NATIVE_CD": "1", "DOTHEROES_NATIVE_SOLVER": "1,2,4,5,6", "GCM_WRAP_LAP": "1",
               "DOTHEROES_PRESENT_ONCE": "1"}
# The values earlier versions of this file wrote as defaults: a setting that
# still holds one is the old default, not a player's choice, and an update
# brings it to the current one. Any other value is the player's and is kept.
PREVIOUS_DEFAULTS = {"DOTHEROES_NATIVE_SOLVER": ("4,5,6",)}
# A new install's own choices (the setup app's defaults)
NEW_INSTALL = {"DOTHEROES_FPS": "60", "PS3RECOMP_VK_FULLSCREEN": "0"}


def ensure_performance_settings(install):
    cur = read_game_settings(install)
    missing = {k: v for k, v in PERFORMANCE.items() if k not in cur}
    stale = {k: PERFORMANCE[k] for k, olds in PREVIOUS_DEFAULTS.items() if cur.get(k) in olds}
    if missing or stale or not os.path.exists(game_settings_path(install)):
        write_game_settings(install, **missing, **stale)


def write_game_settings(install, **values):
    cur = read_game_settings(install)
    cur.update(values)
    try:
        with open(game_settings_path(install), "w") as f:
            f.write("# Cubic.Heroes settings (the setup app writes this; editing by hand is fine)\n")
            f.write("# DOTHEROES_FPS: 60, 30 (the original) or 0 (no limit)\n")
            f.write("# PS3RECOMP_VK_FULLSCREEN: 1 = fullscreen\n")
            f.write("# DOTHEROES_NATIVE_CD, DOTHEROES_NATIVE_SOLVER, GCM_WRAP_LAP, DOTHEROES_PRESENT_ONCE: the\n")
            f.write("#   settings that hold 60 fps in fights (0 / empty: the original's way, slower)\n")
            for k, v in sorted(cur.items()):
                f.write(f"{k}={v}\n")
    except OSError:
        pass


def settle_after_conversion(install):
    """after an install or an update: a new install's defaults, the performance switches kept current"""
    if not os.path.exists(game_settings_path(install)):
        write_game_settings(install, **NEW_INSTALL)
    ensure_performance_settings(install)
