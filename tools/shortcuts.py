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
import hostenv  # noqa: E402 -- the system's programs without the frozen worker's library path

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
        p = subprocess.run(["xdg-user-dir", kind], capture_output=True, text=True, env=hostenv.host_env()).stdout.strip()
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


DESKTOP_FILE = "dotheroes-native.desktop"
ICON_NAME = "cubicheroes"   # in the user's icon theme (M11.4): a name, not a path -- an AppImage's mount is gone once
                            # it ends, and a portable folder moves


def _data_home():
    return os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")


def _icon_source(name="icon.png"):
    """one of the project's own images (assets/: not the game's) -- the icon, or the Steam library logo
    (steam-logo.png) -- beside the tools in a checkout or the worker"""
    for base in (os.path.dirname(os.path.dirname(os.path.abspath(__file__))), os.path.dirname(sys.executable)):
        p = os.path.join(base, "assets", name)
        if os.path.isfile(p):
            return p
    return None


STEAM_LOGO = ("_logo", "steam-logo.png")   # Steam's grid name <appid>_logo.png: the project's own logo, shipped with it


def steam_logo(source=None):
    """the project's own Steam library logo (assets/steam-logo.png, shipped with Cubic.Heroes): (suffix, PNG bytes), or
    raises ArtError (missing, not a PNG, too large)"""
    p = source or _icon_source(STEAM_LOGO[1])
    if not p or not os.path.isfile(p):
        raise ArtError("The launcher's Steam logo is missing.", "logo_missing")
    with open(p, "rb") as f:
        data = f.read(ART_MAX_BYTES + 1)
    if len(data) > ART_MAX_BYTES or image_ext(data) != ".png":
        raise ArtError("The launcher's Steam logo isn't a PNG image it can use.", "logo_invalid")
    return STEAM_LOGO[0], data


def _install_icon(icons_dir=None):
    """the icon into the user's theme (hicolor, 256x256: its size) once shortcuts are asked for; its name, or ''"""
    src = _icon_source()
    if not src:
        return ""
    d = os.path.join(icons_dir or os.path.join(_data_home(), "icons"), "hicolor", "256x256", "apps")
    os.makedirs(d, exist_ok=True)
    shutil.copy2(src, os.path.join(d, ICON_NAME + ".png"))
    return ICON_NAME


def _linux_targets(apps_dir=None, desktop_dir=None):
    return [os.path.join(apps_dir or os.path.join(_data_home(), "applications"), DESKTOP_FILE),
            os.path.join(desktop_dir or _xdg_dir("DESKTOP", "~/Desktop"), DESKTOP_FILE)]


def _linux_shortcuts(exe, icon, desktop, apps_dir=None, desktop_dir=None, icons_dir=None):
    entry = _desktop_entry(exe, icon or _install_icon(icons_dir))
    targets = _linux_targets(apps_dir, desktop_dir)[:2 if desktop else 1]
    made = []
    for path in targets:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:   # the desktop entry spec's encoding (a Unicode folder's path)
            f.write(entry)
        os.chmod(path, 0o755)
        if path != targets[0]:
            # GNOME only launches desktop files it has been told to trust
            subprocess.run(["gio", "set", path, "metadata::trusted", "true"], capture_output=True, env=hostenv.host_env())
        made.append(path)
    return made


def remove_linux_shortcuts(exe, apps_dir=None, desktop_dir=None, icons_dir=None):
    """the menu and desktop entries Cubic.Heroes made for the launcher `exe` (those whose Exec is exactly it: another
    copy's are left), and the icon once no entry of ours is left. Returns the paths removed."""
    want = "Exec=" + re.search(r"^Exec=(.*)$", _desktop_entry(exe, ""), re.M).group(1)
    removed = []
    for path in _linux_targets(apps_dir, desktop_dir):
        try:
            with open(path, encoding="utf-8") as f:
                ours = want in f.read().splitlines()
        except OSError:
            continue
        if ours:
            os.remove(path)
            removed.append(path)
    if removed and not any(os.path.exists(p) for p in _linux_targets(apps_dir, desktop_dir)):
        icon = os.path.join(icons_dir or os.path.join(_data_home(), "icons"), "hicolor", "256x256", "apps",
                            ICON_NAME + ".png")
        if os.path.exists(icon):
            os.remove(icon)
            removed.append(icon)
    return removed


def create_shortcuts(exe, icon, desktop=True, **dirs):
    """The menu entry, and a desktop shortcut if asked. Returns what was made."""
    if os.name == "nt":
        return _windows_shortcuts(exe, desktop)
    return _linux_shortcuts(exe, icon, desktop, **dirs)


# ---------------- Steam ----------------

def steam_roots():
    """Steam's installation folders on this machine (native, Flatpak and Snap on Linux)."""
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
            "~/.steam/steam", "~/.local/share/Steam", "~/.var/app/com.valvesoftware.Steam/data/Steam",
            "~/snap/steam/common/.local/share/Steam")]   # Ubuntu's Snap package of Steam
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
        return subprocess.run(["pgrep", "-x", "steam"], capture_output=True, env=hostenv.host_env()).returncode == 0
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
                                 creationflags=NO_WINDOW, env=hostenv.host_env())
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


# Library artwork (0.2.0-beta.5, the owner's choice of images): downloaded onto the player's own computer when they add
# the game to Steam -- never shipped with Cubic.Heroes. kind -> (suffix in Steam's grid folder, URL, credit). Each image
# is fetched once per Add to Steam (no background fetching, no retries), checked (PNG by its bytes, at most
# ART_MAX_BYTES), and placed in every account's grid folder without ever replacing artwork the player put there. Any
# failure leaves the game added to Steam without that image; the player can then choose the files themselves
# (import_steam_art).
STEAM_ART = {
    "poster": ("p", "https://cdn2.steamgriddb.com/grid/eaccbf3d792ad75cc1ad699862126724.png",
               "the poster by Fami-Kon (SteamGridDB)"),
    "hero": ("_hero", "https://cdn2.steamgriddb.com/hero/cbe68589759f566354b16b634b2ac6b8.png",
             "the hero image by Jib (SteamGridDB)"),
}
ART_SLOTS = {"poster": "p", "hero": "_hero"}   # Steam's grid names: <appid>p.<ext> (the library's poster), <appid>_hero
ART_MAX_BYTES = 20 << 20
ART_TIMEOUT_S = 10
ART_RECORD = "cubicheroes-art.json"   # in a grid folder: the SHA-256 of each file Cubic.Heroes placed there
_ART_EXTS = (".png", ".jpg", ".jpeg")


class ArtError(Exception):
    """artwork that can't be placed: a plain message and a code (never stops the game being added to Steam)"""

    def __init__(self, message, code):
        super().__init__(message)
        self.code = code


def image_ext(data):
    """the file type an image's bytes say (never its name): .png, .jpg, or None"""
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return ".png"
    if data.startswith(b"\xff\xd8\xff"):
        return ".jpg"
    return None


def _sha256(data):
    import hashlib
    return hashlib.sha256(data).hexdigest()


def _art_record(grid_dir):
    try:
        import json
        with open(os.path.join(grid_dir, ART_RECORD), encoding="utf-8") as f:
            r = json.load(f)
        return r if isinstance(r, dict) else {}
    except (OSError, ValueError):
        return {}


def _write_art_record(grid_dir, rec):
    import json
    path = os.path.join(grid_dir, ART_RECORD)
    with open(path + ".part", "w", encoding="utf-8", newline="\n") as f:
        json.dump(rec, f, indent=1, sort_keys=True)
    os.replace(path + ".part", path)


def _place_art(grid_dir, appid, suffix, data, ext, replace=False):
    """data into grid_dir as <appid><suffix><ext>. A file in that slot Cubic.Heroes didn't place -- the player's own
    artwork -- stays unless replace (the player said so). Returns (the path written or None, the player's own files
    kept)."""
    os.makedirs(grid_dir, exist_ok=True)
    rec = _art_record(grid_dir)
    slot = [os.path.join(grid_dir, f"{appid}{suffix}{e}") for e in _ART_EXTS]
    theirs = []
    for p in slot:
        if os.path.isfile(p):
            with open(p, "rb") as f:
                ours = rec.get(os.path.basename(p)) == _sha256(f.read())
            if not ours:
                theirs.append(p)
    if theirs and not replace:
        return None, theirs
    for p in slot:   # ours (or, the player having said so, theirs): one file per slot, whatever its extension
        if os.path.isfile(p):
            os.remove(p)
        rec.pop(os.path.basename(p), None)
    dest = os.path.join(grid_dir, f"{appid}{suffix}{ext}")
    with open(dest + ".part", "wb") as f:
        f.write(data)
    os.replace(dest + ".part", dest)
    rec[os.path.basename(dest)] = _sha256(data)
    _write_art_record(grid_dir, rec)
    return dest, []


def _user_agent():
    try:
        from version import VERSION
    except ImportError:
        VERSION = "dev"
    return f"Cubic.Heroes launcher {VERSION}"


def _urlopen(req, timeout):
    """urllib's, with the updates' certificate store for HTTPS (tools/updates.py tls_context: the frozen Linux worker
    has no usable compiled-in one)"""
    import urllib.request
    ctx = None
    if req.full_url.lower().startswith("https:"):
        try:
            import updates
            ctx = updates.tls_context()
        except Exception:   # noqa: BLE001 -- no store found: urllib's own default (and its error, if any)
            ctx = None
    return urllib.request.urlopen(req, timeout=timeout, context=ctx)


def _why_not_downloaded(e):
    """a failed download in the player's words (the technical cause goes to the report's causes)"""
    import urllib.error
    if isinstance(e, urllib.error.HTTPError):
        return f"the image server answered with an error ({e.code})"
    if isinstance(e, (urllib.error.URLError, OSError)):   # offline, refused, a timeout, a certificate
        return "it couldn't be downloaded -- check the internet connection"
    if isinstance(e, ValueError):   # the download came, but not as an image it can use
        return "the download wasn't a usable image"
    return "it couldn't be downloaded"


def fetch_steam_art(art=STEAM_ART, timeout=ART_TIMEOUT_S, urlopen=None, causes=None):
    """Each image downloaded once: ({kind: (suffix, PNG bytes)} for those that came whole as a PNG of at most
    ART_MAX_BYTES -- the cap enforced while reading -- , {kind: why, in the player's words} for the rest; causes
    (a dict) gets each failure's technical cause). Never raises."""
    import urllib.request
    urlopen = urlopen or _urlopen
    got, failed = {}, {}
    for kind, (suffix, url, _) in art.items():
        try:
            req = urllib.request.Request(url, headers={"User-Agent": _user_agent(), "Accept": "image/png"})
            buf = bytearray()
            with urlopen(req, timeout) as r:
                while True:
                    chunk = r.read(1 << 16)
                    if not chunk:
                        break
                    buf += chunk
                    if len(buf) > ART_MAX_BYTES:
                        raise ValueError(f"larger than {ART_MAX_BYTES >> 20} MB")
            if image_ext(bytes(buf)) != ".png":
                raise ValueError("not a PNG image")
            got[kind] = (suffix, bytes(buf))
        except Exception as e:   # noqa: BLE001 -- offline, an error page, a timeout...: that image isn't added
            failed[kind] = _why_not_downloaded(e)
            if causes is not None:
                causes[kind] = str(e) or type(e).__name__
    return got, failed


def download_steam_art(grid_dir, appid, art=STEAM_ART, timeout=ART_TIMEOUT_S, urlopen=None):
    """STEAM_ART's images into one Steam account's config/grid folder, named after the game's id; never over the
    player's own artwork. Returns the files written; a failed download is skipped."""
    images, _ = fetch_steam_art(art, timeout, urlopen)
    written = []
    for suffix, data in images.values():
        try:
            p, _ = _place_art(grid_dir, appid, suffix, data, ".png")
        except OSError:
            continue
        if p:
            written.append(p)
    return written


def _steam_accounts_with_game(exe, roots=None):
    """the config folders of every Steam account whose library has this launcher's entry"""
    exe_field = f'"{exe}"'
    out = []
    for root in (roots if roots is not None else steam_roots()):
        for config in steam_accounts(root):
            try:
                items = read_shortcuts(os.path.join(config, "shortcuts.vdf")).values()
            except (ValueError, IndexError, OSError):
                continue
            if any(s.get("AppName") == NAME and s.get("Exe") == exe_field for s in items):
                out.append(config)
    return out


def import_steam_art(exe, kind, image, roots=None, replace=False):
    """The player's own image file (downloaded by them, from wherever they choose) as this game's Steam library
    artwork -- kind "poster" or "hero" -- in every account whose library has the game. Only a PNG or JPEG (by its
    bytes), at most ART_MAX_BYTES. The player's own artwork already in that slot is kept unless replace. Returns
    {"placed": [paths], "kept": [the player's files left], "accounts": n}; ArtError otherwise."""
    if kind not in ART_SLOTS:
        raise ArtError(f"Unknown artwork kind: {kind}.", "bad_kind")
    try:
        size = os.path.getsize(image)
        with open(image, "rb") as f:
            data = f.read(ART_MAX_BYTES + 1)
    except OSError as e:
        raise ArtError(f"That image can't be read ({e.strerror}).", "image_unreadable") from None
    if size > ART_MAX_BYTES or len(data) > ART_MAX_BYTES:
        raise ArtError(f"That image is larger than {ART_MAX_BYTES >> 20} MB.", "image_too_large")
    ext = image_ext(data)
    if not ext:
        raise ArtError("That file isn't a PNG or JPEG image.", "not_an_image")
    roots = roots if roots is not None else steam_roots()
    if not roots:
        raise ArtError("Steam wasn't found on this PC.", "no_steam")
    accounts = _steam_accounts_with_game(exe, roots)
    if not accounts:
        raise ArtError("Add the game to Steam first: the artwork goes with its entry in your library.",
                       "not_in_steam")
    appid = steam_app_id(f'"{exe}"', NAME)
    placed, kept = [], []
    for config in accounts:
        p, theirs = _place_art(os.path.join(config, "grid"), appid, ART_SLOTS[kind], data, ext, replace)
        if p:
            placed.append(p)
        kept += theirs
    return {"placed": placed, "kept": kept, "accounts": len(accounts)}


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
    with open(path, "rb") as f:
        buf = f.read()
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


def add_to_steam(exe, icon, roots=None, artwork=True, report=None, fetch=None, logo=None):
    """Add (or update) the game in every Steam account's library on this machine, with
    its library artwork (STEAM_ART, each image downloaded once, and the project's own logo,
    assets/steam-logo.png -- logo: another file, for tests) unless artwork=False. Steam
    must not be running. Returns the files written; report (a dict) gets the artwork's
    outcome: placed, kept (the player's own, left), failed ({kind: why, in the player's words}),
    causes ({kind: the technical cause}). The artwork never
    stops the game being added."""
    images, art_failed, placed, kept, causes = {}, {}, [], [], {}
    if artwork:
        try:
            images, art_failed = (fetch or fetch_steam_art)(causes=causes)
        except Exception as e:   # noqa: BLE001 -- never a reason not to add the game
            images, art_failed = {}, {"artwork": "it couldn't be downloaded"}
            causes["artwork"] = str(e) or type(e).__name__
        try:   # the project's own logo: shipped with Cubic.Heroes, nothing downloaded
            images["logo"] = steam_logo(logo)
        except (ArtError, OSError) as e:
            art_failed["logo"] = "the Cubic.Heroes logo is missing from the launcher's files"
            causes["logo"] = str(e)
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
            for kind, (suffix, data) in images.items():
                try:
                    p, theirs = _place_art(os.path.join(config, "grid"), appid, suffix, data, ".png")
                except OSError as e:   # a grid folder that can't be written: that image isn't added there
                    art_failed[kind] = "Steam's artwork folder couldn't be written"
                    causes[kind] = e.strerror or str(e)
                    continue
                if p:
                    written.append(p)
                    placed.append(p)
                kept += theirs
    if report is not None:
        report.update(placed=placed, kept=kept, failed=art_failed, causes=causes)
    return written


if __name__ == "__main__":
    # python3 tools/shortcuts.py dump <shortcuts.vdf>
    if len(sys.argv) == 3 and sys.argv[1] == "dump":
        for k, v in read_shortcuts(sys.argv[2]).items():
            print(k, v)
