#!/usr/bin/env python3
"""The one authoritative settings file (M9: docs/M9_LAUNCHER_PLAN.md section 13; config/settings_schema.json).

<install>/config/dotheroes.cfg holds every setting as NAME=value lines -- the game's (it reads them at start,
port/main.cpp read_settings) and the launcher's (LAUNCHER_*) -- so the launcher's settings page and the game's own
menus read and write the same file and always agree. Every value is checked against the schema: a value that
breaks its rule is reported and its default used (never passed on as it is); a line that is not NAME=value is
reported and left out; a file that cannot be read is reported, and `repair` writes it again with the defaults --
the damaged one kept beside it. Keys the schema does not know (a newer version's, a developer's switch) are kept.
"""
import datetime
import json
import os
import shutil
import sys

ROOT = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import game_settings  # noqa: E402

SCHEMA_FILE = os.path.join(ROOT, "config", "settings_schema.json")


class SettingsError(Exception):
    def __init__(self, message, code="invalid_setting"):
        super().__init__(message)
        self.code = code


def schema():
    with open(SCHEMA_FILE, encoding="utf-8") as f:
        return {s["key"]: s for s in json.load(f)["settings"]}


def check(spec, value):
    """(True, normalized) or (False, why)"""
    v = str(value).strip()
    t = spec["type"]
    if t == "bool":
        m = {"1": "1", "0": "0", "true": "1", "false": "0", "on": "1", "off": "0", "yes": "1", "no": "0"}
        return (True, m[v.lower()]) if v.lower() in m else (False, "not 0 or 1")
    if t == "enum":
        return (True, v) if v in spec["values"] else (False, f"not one of {', '.join(spec['values'])}")
    if t == "int":
        try:
            n = int(v)
        except ValueError:
            return False, "not a whole number"
        if not spec.get("min", n) <= n <= spec.get("max", n):
            return False, f"not between {spec.get('min')} and {spec.get('max')}"
        return True, str(n)
    if t == "list":
        items = [x.strip() for x in v.split(",") if x.strip()] if v else []
        bad = [x for x in items if x not in spec["values"]]
        if bad:
            return False, f"{', '.join(bad)} not among {', '.join(spec['values'])}"
        return True, ",".join(x for x in spec["values"] if x in items)   # the schema's order, no repeats
    return True, v


def read_file(path):
    """(values {key: raw value}, problems [str], readable)"""
    values, problems = {}, []
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except FileNotFoundError:
        return values, problems, True
    except OSError as e:
        return values, [f"the settings file can't be read ({e.strerror})"], False
    if b"\0" in raw:
        return values, ["the settings file is damaged (it holds binary data)"], False
    text = raw.decode("utf-8", errors="replace")
    for n, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        if "=" not in s:
            problems.append(f"line {n} is not NAME=value: left out")
            continue
        k, v = s.split("=", 1)
        k = k.strip()
        if not k or any(c.isspace() for c in k):
            problems.append(f"line {n} has no valid name: left out")
            continue
        values[k] = v.strip()
    return values, problems, True


def effective(install):
    """every schema setting's value as the game and launcher will use it: {settings: {key: {value, default, source,
    applies, owner, ...}}, problems, unknown: {key: value}, path}"""
    sc = schema()
    path = game_settings.game_settings_path(install)
    values, problems, readable = read_file(path)
    out = {}
    for k, spec in sc.items():
        src, v = "default", spec["default"]
        if k in values:
            ok, norm = check(spec, values[k])
            if ok:
                src, v = "file", norm
            else:
                problems.append(f"{k}={values[k]}: {norm}; the default ({spec['default']}) is used")
                src = "default (the file's value is not valid)"
        out[k] = {"value": v, "default": spec["default"], "source": src, "owner": spec["owner"],
                  "applies": spec["applies"], "category": spec.get("category"), "label": spec.get("label")}
    unknown = {k: v for k, v in values.items() if k not in sc}
    return {"settings": out, "problems": problems, "unknown": unknown, "path": path, "readable": readable}


def _write(install, values):
    try:
        game_settings.write_game_settings(install, strict=True, **values)
    except OSError as e:
        raise SettingsError(f"The settings file can't be written ({e.strerror}).", "io_error")


def set_values(install, changes):
    """the changes, each checked first (none written if any is invalid); the rest of the file kept"""
    sc = schema()
    normal = {}
    for k, v in changes.items():
        if k not in sc:
            raise SettingsError(f"{k} is not a setting this version knows.", "unknown_setting")
        ok, norm = check(sc[k], v)
        if not ok:
            raise SettingsError(f"{k}={v}: {norm}.")
        normal[k] = norm
    values, _, readable = read_file(game_settings.game_settings_path(install))
    if not readable:
        raise SettingsError("The settings file is damaged: repair it first (its values are kept where they can be "
                            "read).", "damaged_settings")
    os.makedirs(os.path.dirname(game_settings.game_settings_path(install)), exist_ok=True)
    _write(install, normal)
    return effective(install)


def reset(install, keys=None):
    """the defaults back for keys (None: every setting); the others kept"""
    sc = schema()
    keys = list(sc) if not keys else keys
    for k in keys:
        if k not in sc:
            raise SettingsError(f"{k} is not a setting this version knows.", "unknown_setting")
    path = game_settings.game_settings_path(install)
    values, _, readable = read_file(path)
    if not readable:
        raise SettingsError("The settings file is damaged: repair it first.", "damaged_settings")
    _write(install, {k: sc[k]["default"] for k in keys})
    return effective(install)


def repair(install):
    """the file written again: the valid values kept, invalid ones and unreadable lines dropped (their defaults then
    apply), unknown keys kept; the file as it was kept beside it (dotheroes.cfg.damaged-<time>)"""
    sc = schema()
    path = game_settings.game_settings_path(install)
    values, problems, readable = read_file(path)
    if os.path.exists(path):
        keep = f"{path}.damaged-{datetime.datetime.now().strftime('%Y%m%d-%H%M%S')}"
        shutil.copy2(path, keep)
    else:
        keep = None
    good = {}
    for k, v in values.items():
        if k in sc:
            ok, norm = check(sc[k], v)
            if ok:
                good[k] = norm
        elif readable:
            good[k] = v
    if os.path.exists(path):
        os.remove(path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    _write(install, good)
    game_settings.ensure_performance_settings(install)
    out = effective(install)
    out["repaired_from"] = keep
    out["dropped"] = problems
    return out
