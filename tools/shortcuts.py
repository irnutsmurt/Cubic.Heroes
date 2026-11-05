#!/usr/bin/env python3
"""Shortcuts for the installed game: the Start Menu / app menu, the desktop, and the
Steam library (as a non-Steam game).

    create_shortcuts(exe, icon, desktop=True)
    add_to_steam(exe, icon)          -> the files written (shortcuts.vdf, artwork)
    steam_running() / close_steam()

Steam keeps non-Steam games in userdata/<account>/config/shortcuts.vdf (binary
VDF) and rewrites that file from memory when it exits, so it must be closed
while the game is added.
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib

NAME = "Cubic.Heroes"
COMMENT = "3D Dot Game Heroes Recomp: native PC port of the PS3 version (static recompilation)"
# The name earlier versions gave the shortcuts and the Steam entry. Where this one
# makes its own, it takes away an old one that starts the game (dotheroes[.exe])
# rather than leave both; one that starts anything else (RPCS3's, say) stays.
OLD_NAMES = ["3D Dot Game Heroes"]
NO_WINDOW = 0x08000000 if os.name == "nt" else 0  # CREATE_NO_WINDOW


# ---------------- menu and desktop ----------------

def _powershell(script, **env):
    """Run a PowerShell snippet; values are passed through the environment (no quoting)."""
    subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script],
                   env=dict(os.environ, **env), check=True, capture_output=True, creationflags=NO_WINDOW)


def _windows_shortcuts(exe, desktop):
    made = []
    for folder in ["Programs"] + (["Desktop"] if desktop else []):
        _powershell(
            "$d = [Environment]::GetFolderPath($env:DDGH_FOLDER); $w = New-Object -ComObject WScript.Shell;"
            "$s = $w.CreateShortcut((Join-Path $d ($env:DDGH_NAME + '.lnk')));"
            "$s.TargetPath = $env:DDGH_EXE; $s.WorkingDirectory = $env:DDGH_DIR;"
            "$s.IconLocation = $env:DDGH_EXE + ',0'; $s.Description = $env:DDGH_COMMENT; $s.Save();"
            # an old-named shortcut in the same folder that starts the game (any install of it) goes
            "foreach ($n in $env:DDGH_OLD.Split('|')) { $p = Join-Path $d ($n + '.lnk');"
            " if ($n -and (Test-Path -LiteralPath $p) -and ([IO.Path]::GetFileName($w.CreateShortcut($p).TargetPath)"
            " -eq [IO.Path]::GetFileName($env:DDGH_EXE))) { Remove-Item -LiteralPath $p } }",
            DDGH_FOLDER=folder, DDGH_NAME=NAME, DDGH_EXE=exe, DDGH_DIR=os.path.dirname(exe), DDGH_COMMENT=COMMENT,
            DDGH_OLD="|".join(OLD_NAMES))
        made.append(folder)
    return made


def _xdg_dir(kind, fallback):
    try:
        p = subprocess.run(["xdg-user-dir", kind], capture_output=True, text=True).stdout.strip()
        if p and p != os.path.expanduser("~"):
            return p
    except OSError:
        pass
    return os.path.expanduser(fallback)


def _desktop_entry(exe, icon):
    def q(p):  # Exec quoting: '\' before " ` $ \, then the backslash itself doubled (desktop entry spec)
        return '"' + re.sub(r'(["`$\\])', r"\\\\\1", p) + '"'
    return ("[Desktop Entry]\nType=Application\n"
            f"Name={NAME}\nComment={COMMENT}\nKeywords={';'.join(OLD_NAMES)};\n"
            f"Exec={q(exe)}\nPath={os.path.dirname(exe)}\nIcon={icon}\n"
            "Terminal=false\nCategories=Game;ArcadeGame;\n")


def _linux_shortcuts(exe, icon, desktop, apps_dir=None, desktop_dir=None):
    entry = _desktop_entry(exe, icon)
    apps_dir = apps_dir or os.path.join(os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share"),
                                        "applications")
    targets = [os.path.join(apps_dir, "dotheroes-native.desktop")]
    if desktop:
        targets.append(os.path.join(desktop_dir or _xdg_dir("DESKTOP", "~/Desktop"), "dotheroes-native.desktop"))
    made = []
    for path in targets:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(entry)
        os.chmod(path, 0o755)
        if path != targets[0]:
            # GNOME only launches desktop files it has been told to trust
            subprocess.run(["gio", "set", path, "metadata::trusted", "true"], capture_output=True)
        made.append(path)
    return made


def create_shortcuts(exe, icon, desktop=True, **dirs):
    """The menu entry, and a desktop shortcut if asked. Returns what was made."""
    if os.name == "nt":
        return _windows_shortcuts(exe, desktop)
    return _linux_shortcuts(exe, icon, desktop, **dirs)


# ---------------- Steam ----------------

def steam_roots():
    """Steam's installation folders on this machine (native and Flatpak on Linux)."""
    roots = []
    if os.name == "nt":
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
                roots.append(winreg.QueryValueEx(k, "SteamPath")[0])
        except OSError:
            pass
        roots.append(os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Steam"))
    else:
        roots += [os.path.expanduser(p) for p in (
            "~/.steam/steam", "~/.local/share/Steam", "~/.var/app/com.valvesoftware.Steam/data/Steam")]
    seen, out = set(), []
    for r in roots:
        r = os.path.realpath(os.path.normpath(r))
        if r not in seen and os.path.isdir(os.path.join(r, "userdata")):
            seen.add(r)
            out.append(r)
    return out


def steam_accounts(root):
    """userdata/<account id>/config folders (one per Steam account used on this machine)."""
    base = os.path.join(root, "userdata")
    return [os.path.join(base, d, "config") for d in sorted(os.listdir(base))
            if d.isdigit() and d != "0" and os.path.isdir(os.path.join(base, d))]


def steam_running():
    try:
        if os.name == "nt":
            out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq steam.exe", "/NH"], capture_output=True,
                                 text=True, creationflags=NO_WINDOW).stdout
            return "steam.exe" in out.lower()
        return subprocess.run(["pgrep", "-x", "steam"], capture_output=True).returncode == 0
    except OSError:
        return False


def close_steam(timeout=30):
    """Ask Steam to exit and wait for it. Returns True once it's gone."""
    for root in steam_roots() or [None]:
        exe = (os.path.join(root, "steam.exe") if os.name == "nt" and root else
               "steam" if os.name != "nt" else None)
        if exe:
            try:
                subprocess.Popen([exe, "-shutdown"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 creationflags=NO_WINDOW)
                break
            except OSError:
                continue
    end = time.time() + timeout
    while time.time() < end:
        if not steam_running():
            time.sleep(1)  # let it finish writing its files
            return True
        time.sleep(0.5)
    return False


# Library artwork: none chosen yet for this game (Steam shows its default). Each entry
# would be kind -> (file name suffix in Steam's grid folder, URL, credit), downloaded
# onto the player's machine only when they add the game to Steam.
STEAM_ART = {}


def download_steam_art(grid_dir, appid, art=STEAM_ART, timeout=20):
    """Fetch the artwork into a Steam account's config/grid folder, named after the
    game's id. Returns the files written; a failed download is skipped."""
    import urllib.request
    os.makedirs(grid_dir, exist_ok=True)
    written = []
    for kind, (suffix, url, _) in art.items():
        ext = os.path.splitext(url)[1].lower()
        dest = os.path.join(grid_dir, f"{appid}{suffix}{ext}")
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "DotHeroes-Setup"})
            with urllib.request.urlopen(req, timeout=timeout) as r:
                data = r.read()
            if not data.startswith((b"\x89PNG", b"\xff\xd8")):
                continue  # not an image (an error page)
            for old in (".png", ".jpg", ".jpeg"):  # an earlier copy with another extension wins otherwise
                old_path = os.path.join(grid_dir, f"{appid}{suffix}{old}")
                if old_path != dest and os.path.exists(old_path):
                    os.remove(old_path)
            with open(dest, "wb") as f:
                f.write(data)
            written.append(dest)
        except OSError:
            continue
    return written


# binary VDF: 0x00 map, 0x01 string, 0x02 int32, 0x08 end of map (all keys are C strings)

def _cstr(buf, i):
    j = buf.index(b"\0", i)
    return buf[i:j].decode("utf-8", "replace"), j + 1


def _read_map(buf, i):
    out = {}
    while True:
        t = buf[i]
        i += 1
        if t == 0x08:
            return out, i
        key, i = _cstr(buf, i)
        if t == 0x00:
            out[key], i = _read_map(buf, i)
        elif t == 0x01:
            out[key], i = _cstr(buf, i)
        elif t == 0x02:
            out[key] = struct.unpack_from("<i", buf, i)[0]
            i += 4
        elif t == 0x07:
            out[key] = ("u64", struct.unpack_from("<Q", buf, i)[0])
            i += 8
        else:
            raise ValueError(f"shortcuts.vdf: unknown field type {t:#x}")


def _write_map(m):
    out = bytearray()
    for k, v in m.items():
        key = k.encode() + b"\0"
        if isinstance(v, dict):
            out += b"\x00" + key + _write_map(v)
        elif isinstance(v, tuple):
            out += b"\x07" + key + struct.pack("<Q", v[1])
        elif isinstance(v, int):
            out += b"\x02" + key + struct.pack("<i", v)
        else:
            out += b"\x01" + key + str(v).encode() + b"\0"
    return out + b"\x08"


def read_shortcuts(path):
    if not os.path.exists(path):
        return {}
    buf = open(path, "rb").read()
    if not buf:
        return {}
    root, _ = _read_map(buf, 0)
    return root.get("shortcuts", {})


def write_shortcuts(path, shortcuts):
    data = _write_map({"shortcuts": shortcuts})
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def _starts_game(exe_field, exe):
    """A Steam entry's Exe ("quoted path") names the game's executable (any install of it)."""
    path = str(exe_field or "").strip().strip('"')
    return os.path.basename(path.replace("\\", "/")).lower() == os.path.basename(exe).lower()


def steam_app_id(exe_field, name):
    """The id Steam gives a non-Steam game (also the name of its artwork files)."""
    return (zlib.crc32((exe_field + name).encode()) & 0xFFFFFFFF) | 0x80000000


def add_to_steam(exe, icon, roots=None, artwork=True):
    """Add (or update) the game in every Steam account's library on this machine, with
    its library artwork (STEAM_ART) unless artwork=False. Steam must not be running.
    Returns the files written."""
    exe_field = f'"{exe}"'
    appid = steam_app_id(exe_field, NAME)
    entry = {
        "appid": struct.unpack("<i", struct.pack("<I", appid))[0],
        "AppName": NAME,
        "Exe": exe_field,
        "StartDir": f'"{os.path.dirname(exe)}"',
        "icon": icon,
        "ShortcutPath": "",
        "LaunchOptions": "",
        "IsHidden": 0,
        "AllowDesktopConfig": 1,
        "AllowOverlay": 1,
        "OpenVR": 0,
        "Devkit": 0,
        "DevkitGameID": "",
        "DevkitOverrideAppID": 0,
        "LastPlayTime": 0,
        "FlatpakAppID": "",
        "tags": {},
    }
    written = []
    for root in (roots if roots is not None else steam_roots()):
        for config in steam_accounts(root):
            path = os.path.join(config, "shortcuts.vdf")
            try:
                items = list(read_shortcuts(path).values())
            except (ValueError, IndexError, OSError):
                continue  # a file we can't read safely: leave it alone
            if os.path.exists(path) and not os.path.exists(path + ".dotheroes-backup"):
                shutil.copy2(path, path + ".dotheroes-backup")
            # replace an earlier entry for this game (same name or same executable, or the
            # old name on an entry that starts the game)
            items = [s for s in items if s.get("AppName") != NAME and s.get("Exe") != exe_field
                     and not (s.get("AppName") in OLD_NAMES and _starts_game(s.get("Exe"), exe))]
            items.append(entry)
            os.makedirs(config, exist_ok=True)
            write_shortcuts(path, {str(i): s for i, s in enumerate(items)})
            written.append(path)
            if artwork:
                written += download_steam_art(os.path.join(config, "grid"), appid)
    return written


if __name__ == "__main__":
    # python3 tools/shortcuts.py dump <shortcuts.vdf>
    if len(sys.argv) == 3 and sys.argv[1] == "dump":
        for k, v in read_shortcuts(sys.argv[2]).items():
            print(k, v)
