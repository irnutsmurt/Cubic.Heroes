#!/usr/bin/env python3
"""Diagnostics for a report (M9: docs/M9_LAUNCHER_PLAN.md sections 8.7 and 13).

    collect(install, out_dir)   the report's files written into out_dir for the player to review (the launcher
                                shows them; the player may remove any): about.json (the versions, the operating
                                system, the processor and memory, the GPU and its driver, the install's state, the
                                caches' sizes) and the logs, every text privacy-sanitized
    pack(out_dir, zip_path)     exactly what is in out_dir after the review, as one zip

Never in a report: saves (they hold the player's hero's name), decryption keys (never read here), the disc's or
the game's files, the recipes and shader caches (made from the player's game: only their sizes), the disc image's
path. Sanitized in every text: the home folder and the install folder (placeholders), the user's and the
computer's names, e-mail addresses. Nothing is sent anywhere: the player decides what to do with the zip.
"""
import datetime
import getpass
import json
import os
import platform
import re
import socket
import subprocess
import sys
import zipfile

ROOT = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import install_layout as layout  # noqa: E402

LOG_TAIL = 400_000   # bytes of a log kept (its end: the part that says what happened last)
EMAIL = re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}")


class Sanitizer:
    def __init__(self, install=None, extra=()):
        pairs = []
        home = os.path.expanduser("~")
        if install:
            pairs.append((os.path.abspath(install), "<install>"))
        if home and home not in ("~", "/"):
            pairs.append((home, "<home>"))
        for p in list(pairs):
            pairs.append((p[0].replace("\\", "/"), p[1]))
            pairs.append((p[0].replace("\\", "\\\\"), p[1]))
        self.pairs = sorted(set(pairs), key=lambda x: -len(x[0]))
        names = set()
        for n in (os.environ.get("USERNAME"), os.environ.get("USER"), self._user(), os.environ.get("COMPUTERNAME"),
                  socket.gethostname(), *extra):
            if n and len(n) >= 3:
                names.add(n)
        self.names = sorted(names, key=len, reverse=True)

    @staticmethod
    def _user():
        try:
            return getpass.getuser()
        except Exception:  # noqa: BLE001
            return None

    def text(self, s):
        s = EMAIL.sub("<email>", s)   # first: an address may hold a name
        for a, b in self.pairs:
            if a:
                s = re.sub(re.escape(a), b.replace("\\", "\\\\"), s, flags=re.IGNORECASE)
        for n in self.names:
            s = re.sub(r"(?<![A-Za-z0-9])" + re.escape(n) + r"(?![A-Za-z0-9])", "<name>", s, flags=re.IGNORECASE)
        return EMAIL.sub("<email>", s)


def _run_json(cmd, timeout=60):
    kw = {"creationflags": 0x08000000} if os.name == "nt" else {}
    env = {k: v for k, v in os.environ.items() if not k.startswith(("DOTHEROES_", "PS3_", "PS3RECOMP_"))}
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env, stdin=subprocess.DEVNULL,
                           **kw)
        lines = [ln for ln in r.stdout.splitlines() if ln.strip().startswith("{")]
        return json.loads(lines[-1]) if lines else {"exit": r.returncode}
    except (OSError, subprocess.TimeoutExpired, ValueError) as e:
        return {"error": type(e).__name__}


def memory_mb():
    if os.name == "nt":
        import ctypes

        class MS(ctypes.Structure):
            _fields_ = [("len", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                        ("avail", ctypes.c_ulonglong), ("pt", ctypes.c_ulonglong), ("pa", ctypes.c_ulonglong),
                        ("vt", ctypes.c_ulonglong), ("va", ctypes.c_ulonglong), ("ve", ctypes.c_ulonglong)]
        m = MS()
        m.len = ctypes.sizeof(MS)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)):
            return m.total >> 20
        return None
    try:
        return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") >> 20
    except (ValueError, OSError):
        return None


def about(install):
    """the facts a report starts from; the GPU and driver from the active game's own check (no game data read)"""
    from version import VERSION
    lay = layout.Layout(install)
    out = {"made": datetime.datetime.now().isoformat(timespec="seconds"), "launcher_version": VERSION,
           "os": platform.platform(), "machine": platform.machine(), "cpus": os.cpu_count(), "memory_mb": memory_mb(),
           "python": platform.python_version(), "install": lay.summary()}
    st = lay.read_active() if lay.kind() == "slots" else None
    if st and st["active"]:
        info = lay.build_info(st["active"]) or {}
        out["active_build"] = {k: info.get(k) for k in ("id", "version", "made", "lift", "shader_cache", "exe_size")}
        exe = os.path.join(lay.game_link, layout.exe_name())
        out["game_info"] = _run_json([exe, "--info"])
        out["graphics"] = _run_json([exe, "--prepare", "--check"], timeout=120).get("result")
    try:
        import shutil
        out["disk_free_mb"] = shutil.disk_usage(lay.root).free >> 20
    except OSError:
        pass
    sizes = {}
    for name, p in (("disc", lay.disc), ("lifts", lay.gen_root), ("objects", lay.objects), ("shaders", lay.shaders),
                    ("builds", lay.builds_dir), ("save_backups", lay.backups_dir)):
        sizes[name] = layout.tree_size(p) >> 20 if os.path.isdir(p) else None
    rec = os.path.join(lay.cache, "native-recipes.bin")
    sizes["recipes_kb"] = os.path.getsize(rec) >> 10 if os.path.exists(rec) else None
    out["sizes_mb"] = sizes
    out["saves"] = {"present": os.path.isdir(lay.gamedata) and bool(os.listdir(lay.gamedata)),
                    "note": "the saves themselves are never in a report"}
    return out


def collect(install, out_dir, launcher_logs=()):
    """the report's files into out_dir (made empty first): about.json and the logs, sanitized. Returns the list of
    {file, bytes, what}."""
    san = Sanitizer(install)
    os.makedirs(out_dir, exist_ok=True)
    for n in os.listdir(out_dir):
        p = os.path.join(out_dir, n)
        if os.path.isfile(p):
            os.remove(p)
    files = []

    def put(name, text, what):
        text = san.text(text)
        with open(os.path.join(out_dir, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        files.append({"file": name, "bytes": len(text.encode("utf-8")), "what": what})

    a = about(install)
    a["install"].pop("source", None)
    put("about.json", json.dumps(a, indent=1), "versions, system, GPU and driver, the install's state and sizes")
    lay = layout.Layout(install)
    logs = []
    if os.path.isdir(lay.logs):
        logs += [os.path.join(lay.logs, n) for n in sorted(os.listdir(lay.logs)) if n.endswith((".log", ".jsonl"))]
    logs += [p for p in launcher_logs if os.path.isfile(p)]
    for p in logs:
        with open(p, "rb") as f:
            f.seek(max(0, os.path.getsize(p) - LOG_TAIL))
            text = f.read().decode("utf-8", errors="replace")
        name = "log-" + os.path.basename(p)
        put(name, text, f"the end of {os.path.basename(p)}")
    with open(os.path.join(out_dir, "README.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("A Cubic.Heroes diagnostics report. Review the files; remove any you do not want to share.\n"
                "Paths are shown as <install> and <home>; user and computer names as <name>; e-mail addresses as\n"
                "<email>. Never included: saves, keys, the disc's or the game's files, shader caches.\n")
    files.append({"file": "README.txt", "bytes": os.path.getsize(os.path.join(out_dir, "README.txt")), "what": "this"})
    return files


def pack(out_dir, zip_path):
    """the reviewed folder, as it is now, into one zip"""
    names = sorted(n for n in os.listdir(out_dir) if os.path.isfile(os.path.join(out_dir, n)))
    tmp = zip_path + ".part"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
        for n in names:
            z.write(os.path.join(out_dir, n), n)
    os.replace(tmp, zip_path)
    return {"zip": zip_path, "files": names, "bytes": os.path.getsize(zip_path)}
