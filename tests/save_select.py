#!/usr/bin/env python3
"""Which game save a scripted run loads (tests/boot_smoke.py; 0.2.0-beta.5).

With the save list dialog off (the test harness's default: PS3_SAVEDATA_PICKER=0), the game takes the first entry of
its save list, and the runtime lists the save folder in the file system's own order -- by name on Windows (NTFS), in
hash order on Linux (ext4). A checkpoint holding several game saves then loaded a different save on each system. So
the harness copies only the one game save the run names into the run's save data:
  SAVE=<dir> (or a drive's `#! save <dir>`), else the checkpoint's own named save (checkpoint.json "save"), else the
  first game save by name (what Windows has always loaded) -- the same on every system.
The other folders (the system data, the editor's characters) are copied as before. With the dialog on
(PS3_SAVEDATA_PICKER=1: the save-list tests), every save is copied and the player chooses, as in play. The player-
facing dialog and the runtime are unchanged."""
import json
import os

SAVE_PREFIXES = ("BLUS30490", "BLJM60180")   # the game's saves; its editor's data (the Japanese release's ID)
GAME_PREFIX = "BLUS30490GAME"                 # one folder per game save (BLUS30490SYSTEM: the system data)


def named_in_checkpoint(saves_dir):
    """the checkpoint's own named save (checkpoint.json beside the saves: {"save": "<dir>"}), or None"""
    try:
        with open(os.path.join(saves_dir, "checkpoint.json"), encoding="utf-8") as f:
            v = json.load(f).get("save")
        return v if isinstance(v, str) and v else None
    except (OSError, ValueError, AttributeError):
        return None


def choose(saves_dir, named=None, picker=False):
    """(folders to copy, the game save loaded or None): see the module's docstring. SystemExit if a named save isn't
    in the checkpoint."""
    names = sorted(n for n in os.listdir(saves_dir)
                   if n.startswith(SAVE_PREFIXES) and os.path.isdir(os.path.join(saves_dir, n)))
    games = [n for n in names if n.startswith(GAME_PREFIX)]
    if picker:
        return names, None
    named = named or named_in_checkpoint(saves_dir)
    if not named:
        if len(games) <= 1:
            return names, games[0] if games else None
        named = games[0]   # sorted: the same choice on every system
    if named not in games:
        raise SystemExit(f"the save {named} isn't in {saves_dir} (game saves there: {', '.join(games) or 'none'})")
    return [n for n in names if n not in games or n == named], named
