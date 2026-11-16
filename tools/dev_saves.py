#!/usr/bin/env python3
"""Developer mode's test saves (M11.0, docs/M11_PLAN.md section 3.2).

With developer mode on (DOTHEROES_DEVELOPER_MODE=1) the game plays on <install>/data/devmode/gamedata instead of the
player's <install>/data/gamedata (port/main.cpp). This module manages that tree from outside the game:
  copy_player   the test saves replaced by a copy of the player's -- the player's tree is only read; the copy is
                checked file by file (sizes) before the old test tree is removed; nothing ever goes the other way
  keep NAME     the test saves kept as a named test state (data/devmode/states/NAME)
  restore NAME  the test saves replaced by a kept state's copy
  delete NAME   a kept state removed
  states        the kept states (name, time, bytes)
Every operation refuses while the game runs from the install (it reads its save folder at start and writes it while
it plays). Nothing here touches anything outside data/devmode but reading data/gamedata.
"""
import datetime
import json
import os
import re
import shutil
import sys

ROOT = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import install_layout as layout  # noqa: E402

NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,39}")


class DevSavesError(Exception):
    def __init__(self, message, code="failed"):
        super().__init__(message)
        self.code = code


def paths(lay):
    dev = os.path.join(lay.data, "devmode")
    return {"devmode": dev, "tests": os.path.join(dev, "gamedata"), "states": os.path.join(dev, "states"),
            "player": lay.gamedata}


def _files(tree):
    """{relative path: size} of every file under tree"""
    out = {}
    for dp, dn, fn in os.walk(tree):
        for f in fn:
            p = os.path.join(dp, f)
            out[os.path.relpath(p, tree).replace(os.sep, "/")] = os.path.getsize(p)
    return out


def _no_links(tree):
    for dp, dn, fn in os.walk(tree):
        for n in dn + fn:
            if os.path.islink(os.path.join(dp, n)):
                raise DevSavesError(f"a link inside {tree}: refused", "link")


def _not_running(lay, running=None):
    if running is None:
        import uninstall
        running = uninstall.running_from
    found = running(lay.root)
    if found:
        raise DevSavesError("the game is running from this install: close it first", "game_running")


def _replace_tree(src, dest):
    """dest replaced by a checked copy of src (src only read); the swap is a rename of whole folders"""
    _no_links(src)
    incoming, old = dest + ".incoming", dest + ".replaced"
    for p in (incoming, old):
        if os.path.exists(p):
            layout.rmtree(p)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if os.path.isdir(src):
        shutil.copytree(src, incoming)
    else:
        os.makedirs(incoming)
    if _files(incoming) != (_files(src) if os.path.isdir(src) else {}):
        layout.rmtree(incoming)
        raise DevSavesError("the copy does not match its source: nothing replaced", "copy_mismatch")
    if os.path.exists(dest):
        os.replace(dest, old)
    os.replace(incoming, dest)
    if os.path.exists(old):
        layout.rmtree(old)
    f = _files(dest)
    return {"files": len(f), "bytes": sum(f.values())}


def copy_player(lay, running=None):
    _not_running(lay, running)
    p = paths(lay)
    res = _replace_tree(p["player"], p["tests"])
    return dict(res, copied="player saves -> test saves")


def keep(lay, name, replace=False, running=None):
    if not NAME.fullmatch(name or ""):
        raise DevSavesError("a state's name: letters, digits, - and _ (up to 40)", "usage")
    _not_running(lay, running)
    p = paths(lay)
    dest = os.path.join(p["states"], name)
    if os.path.exists(dest) and not replace:
        raise DevSavesError(f"a test state named {name} exists (replace it with --confirm)", "exists")
    res = _replace_tree(p["tests"], os.path.join(dest, "gamedata"))
    with open(os.path.join(dest, "state.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump({"name": name, "kept": datetime.datetime.now().isoformat(timespec="seconds"), **res}, f)
    return dict(res, kept=name)


def restore(lay, name, running=None):
    p = paths(lay)
    src = os.path.join(p["states"], name or "", "gamedata")
    if not NAME.fullmatch(name or "") or not os.path.isdir(src):
        raise DevSavesError(f"no test state named {name}", "no_state")
    _not_running(lay, running)
    res = _replace_tree(src, p["tests"])
    return dict(res, restored=name)


def delete(lay, name, running=None):
    p = paths(lay)
    d = os.path.join(p["states"], name or "")
    if not NAME.fullmatch(name or "") or not os.path.exists(os.path.join(d, "state.json")):
        raise DevSavesError(f"no test state named {name}", "no_state")
    layout.rmtree(d)
    return {"deleted": name}


def states(lay):
    p = paths(lay)
    out = []
    if os.path.isdir(p["states"]):
        for n in sorted(os.listdir(p["states"])):
            try:
                with open(os.path.join(p["states"], n, "state.json"), encoding="utf-8") as f:
                    out.append(json.load(f))
            except (OSError, ValueError):
                pass
    tests = _files(p["tests"]) if os.path.isdir(p["tests"]) else {}
    return {"states": out, "test_saves_files": len(tests), "test_saves_bytes": sum(tests.values())}
