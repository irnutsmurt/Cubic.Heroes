"""Issue #6: the disc's folder chosen in the launcher, hidden (--nav-test) -- Install / Manage, "Your disc", Choose...:
the file browser, as a player meets it, opens the folder holding PS3_GAME and chooses it with "Use this folder" (or
PS3_GAME itself, whose parent is then the disc's folder); an ISO is still chosen as a file in the same browser.
Windows, Linux and SteamOS alike.

Scenarios (each in a fresh folder under --work: the disc folder's files hard links of --folder's, the ISO a hard link
of --iso -- nothing of the game copied, and both checked unchanged at the end):

  folder       the folder holding PS3_GAME, browsed to and chosen: the disc checked (BLUS-30490 v01.00), its keys
               beside it found, Install available; the folder is what launcher.json remembers
  ps3-game     PS3_GAME itself browsed to and chosen: its parent, the disc's folder, is what is used and remembered
  iso          the ISO chosen as a file in the same browser: Install available
  no-param     a folder holding an empty PS3_GAME: chosen, then Install blocked and explained (no PARAM.SFO)
  not-a-disc   a folder without PS3_GAME, as launcher.json may hold one: Install blocked and explained
  remembered   the disc's folder already in launcher.json (chosen before): checked at once, Install available
With --install (a real install each, a few minutes and ~4 GB, removed after):
  install      the folder scenario, then Install: the game installed from the disc's folder (with --online and no keys
               beside the disc, the program's keys are downloaded, as on a computer without any)
  incomplete   the disc's folder with one of the game's files missing: chosen, then Install refused at its check

The old launcher fails folder, ps3-game and install at "Use this folder" (its disc browser took files only).

Needs the disc image and a keys file of your own (local only, never in the repository): --iso, --keys, and the disc's
folder -- --folder, or extracted from --iso (tools/iso9660.py) into --work and removed after; skipped (exit 0)
without them.

  py -3 tests/launcher_disc_folder.py --launcher <the launcher to test> [--iso ISO] [--keys KEYS] [--folder DIR]
      [--only NAME ...] [--install] [--online] [--sizes 1280x800]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import launcher_nav_pages as nav  # noqa: E402

DISC = "3D Dot Game Heroes [BLUS30490]"   # the disc's folder, named as a player's copy may be
ISO = "3D Dot Game Heroes (USA).iso"
MISSING = "PS3_GAME/ICON0.PNG"            # incomplete: a game file the disc check doesn't read, the install's does

HEAD = "until 20000 Install the game\nexpect Install the game\na\nwait 300\n"   # Install / Manage, a fresh folder
OPEN = ("expect Choose...\na\nwait 40\nsee Choose your disc\nexpect browse: ..\n"   # the browser, at the install's folder
        "a\nwait 40\nseek 600 down browse: games\nexpect browse: games\na\nwait 40\n")   # up, then into games/
INTO_DISC = f"seek 600 down browse: {DISC}\nexpect browse: {DISC}\na\nwait 40\n"
INTO_GAME = "seek 600 down browse: PS3_GAME\nexpect browse: PS3_GAME\na\nwait 40\n"
USE = "shot folder\nseek 200 up Use this folder\nexpect Use this folder\nshot browser\na\nwait 900\n"
PICK_ISO = f"seek 600 down browse: {ISO}\nexpect browse: {ISO}\nshot browser\na\nwait 900\n"
READY = "seek 3000 down Install\nexpect Install\nshot install\nquit\n"
BLOCKED = ("wait 600\ndown\nwait 20\ndown\nwait 20\ndown\nwait 20\ndown\nwait 20\ndown\nwait 40\n"
           "shot install\nsee Install can't start yet\nquit\n")
INSTALL = ("seek 3000 down Install\nexpect Install\nshot install\na\nwait 120\n"
           "until 200000 Precompile Now\nsee The game is installed\nshot installed\nquit\n")
REFUSED = ("seek 3000 down Install\nexpect Install\na\nwait 120\n"
           "until 60000 OK\nsee The install didn't finish\nshot refused\nquit\n")


def scenarios(install):
    """name: (the disc's layout, the disc launcher.json holds at the start, the script, the disc it should remember,
    a real install)"""
    out = {
        "folder": ("folder", None, HEAD + OPEN + INTO_DISC + USE + READY, "folder", False),
        "ps3-game": ("folder", None, HEAD + OPEN + INTO_DISC + INTO_GAME + USE + READY, "folder", False),
        "iso": ("iso", None, HEAD + OPEN + PICK_ISO + READY, "iso", False),
        "no-param": ("no-param", None, HEAD + OPEN + INTO_DISC + USE + BLOCKED, "folder", False),
        "not-a-disc": ("folder", "games", HEAD + BLOCKED, "games", False),
        "remembered": ("folder", "folder", HEAD + "wait 600\n" + READY, "folder", False),
    }
    if install:
        out["install"] = ("folder", None, HEAD + OPEN + INTO_DISC + USE + INSTALL, "folder", True)
        out["incomplete"] = ("incomplete", None, HEAD + OPEN + INTO_DISC + USE + REFUSED, "folder", False)
    return out


def tree_state(path):
    """(size, mtime) of every file under path (or of the file): the game's files must come out as they went in"""
    if os.path.isfile(path):
        st = os.stat(path)
        return {"": (st.st_size, st.st_mtime_ns)}
    out = {}
    for d, _, files in os.walk(path):
        for n in files:
            st = os.stat(os.path.join(d, n))
            out[os.path.relpath(os.path.join(d, n), path)] = (st.st_size, st.st_mtime_ns)
    return out


def link_tree(src, dst, skip=()):
    """the disc's folder as hard links (a copy where the volume differs): its files never written"""
    for d, _, files in os.walk(src):
        rel = os.path.relpath(d, src)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for n in files:
            r = os.path.normpath(os.path.join(rel, n)).replace(os.sep, "/")
            if r in skip:
                continue
            try:
                os.link(os.path.join(d, n), os.path.join(dst, rel, n))
            except OSError:
                shutil.copy2(os.path.join(d, n), os.path.join(dst, rel, n))


def remembered(root):
    try:
        with open(os.path.join(root, "config", "launcher.json"), encoding="utf-8") as f:
            return json.load(f).get("disc", "")
    except (OSError, ValueError):
        return None


def same(a, b):
    return bool(a) and bool(b) and os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--launcher", required=True, help="the launcher to test (a build, or a package's)")
    ap.add_argument("--iso", default=os.path.join(ROOT, "3D DOT GAME HEROES [BLUS-30490].iso"))
    ap.add_argument("--keys", default=os.path.join(ROOT, "build", "keys", "self_keys.txt"))
    ap.add_argument("--folder", help="the disc's folder (holding PS3_GAME); default: extracted from --iso")
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "qual", "launcher-disc-folder"))
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--install", action="store_true", help="also the real installs (install, incomplete)")
    ap.add_argument("--online", action="store_true", help="install: no keys beside the disc -- downloaded")
    ap.add_argument("--keep", action="store_true", help="keep each scenario's folder (an install: ~4 GB)")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--sizes", default="1280x800")
    a = ap.parse_args()
    if not (os.path.isfile(a.iso) and os.path.isfile(a.keys)):
        print("SKIP: the disc image and a keys file are local only (--iso, --keys)")
        return 0
    launcher = os.path.abspath(a.launcher)
    work = os.path.abspath(a.work)
    os.makedirs(work, exist_ok=True)
    folder, extracted = a.folder and os.path.abspath(a.folder), False
    if not folder:   # the disc's folder, as a disc dumper writes it: every file of the image
        folder = os.path.join(work, "source", DISC)
        if not os.path.isdir(os.path.join(folder, "PS3_GAME")):
            shutil.rmtree(os.path.join(work, "source"), ignore_errors=True)
            subprocess.run([sys.executable, os.path.join(ROOT, "tools", "iso9660.py"), "extract", a.iso, folder],
                           check=True)
        extracted = True
    if not os.path.isdir(os.path.join(folder, "PS3_GAME")):
        sys.exit(f"--folder: no PS3_GAME in {folder}")
    before = {"folder": tree_state(folder), "iso": tree_state(a.iso)}
    with open(a.keys, "rb") as f:
        keys = f.read()
    ok = True
    for name, (layout, prefill, script, want, real) in scenarios(a.install).items():
        if a.only and name not in a.only:
            continue
        for size in [s.strip() for s in a.sizes.split(",") if s.strip()]:
            d = os.path.join(work, name, size)
            shutil.rmtree(d, ignore_errors=True)
            games = os.path.join(d, "games")
            disc = os.path.join(games, DISC)
            os.makedirs(games)
            if layout == "iso":
                disc = os.path.join(games, ISO)
                try:
                    os.link(a.iso, disc)          # a second name for the same file
                except OSError:
                    try:
                        os.symlink(a.iso, disc)
                    except OSError:               # another volume, no right to make links: a copy, removed after
                        shutil.copyfile(a.iso, disc)
            elif layout == "no-param":
                os.makedirs(os.path.join(disc, "PS3_GAME"))
            else:
                link_tree(folder, disc, skip=(MISSING,) if layout == "incomplete" else ())
            if not (real and a.online):   # the program's keys beside the disc (else downloaded)
                with open(os.path.join(games, "keys.txt"), "wb") as f:
                    f.write(keys)
            root = os.path.join(d, "Cubic.Heroes")
            os.makedirs(os.path.join(root, "config"))
            with open(os.path.join(root, "config", "launcher.json"), "w", encoding="utf-8") as f:
                json.dump({"disc": {"games": games, "folder": disc}.get(prefill, ""), "keys": "", "rpcs3": ""}, f)
            sp = os.path.join(d, f"{name}.nav")
            with open(sp, "w", encoding="utf-8", newline="\n") as f:
                f.write(f"# {name} (tests/launcher_disc_folder.py)\n" + script)
            r = nav.run(sp, launcher, root, os.path.join(d, "work"), a.timeout * (12 if real or name == "incomplete"
                                                                                  else 1), size,
                        online=real and a.online)
            got = remembered(root)
            expect = {"folder": os.path.join(games, DISC), "iso": disc, "games": games}[want]
            if not same(got, expect):
                print(f"FAIL: {name} at {size}: launcher.json's disc is {got!r}, not {expect!r}", flush=True)
                r = False
            if real and r:
                info = None
                try:
                    with open(os.path.join(root, "data", "install.json"), encoding="utf-8") as f:
                        info = json.load(f)
                except (OSError, ValueError):
                    pass
                keys_dir = os.path.join(root, "data", "keys")
                got_keys = os.path.isdir(keys_dir) and bool(os.listdir(keys_dir))
                if not info or not os.path.isdir(os.path.join(root, "game")):
                    print(f"FAIL: {name} at {size}: no installed game (data/install.json, game/)", flush=True)
                    r = False
                elif a.online and not got_keys:
                    print(f"FAIL: {name} at {size}: the keys weren't downloaded into data/keys", flush=True)
                    r = False
                else:
                    print(f"PASS: {name} at {size}: installed from the disc's folder (source "
                          f"{info.get('source', '?')!r}" + (", the keys downloaded" if a.online else "") + ")",
                          flush=True)
            ok = r and ok
            for p in (os.path.join(games, ISO),):
                if os.path.isfile(p):
                    os.remove(p)   # the link only: the disc image itself untouched
            if not a.keep:
                shutil.rmtree(d, ignore_errors=True)
    after = {"folder": tree_state(folder), "iso": tree_state(a.iso)}
    for k in ("folder", "iso"):
        if before[k] != after[k]:
            ok = False
            print(f"FAIL: the disc's {k} changed", flush=True)
    if before == after:
        print("PASS: the disc's folder and image unchanged", flush=True)
    if extracted:
        shutil.rmtree(os.path.join(work, "source"), ignore_errors=True)
    print("launcher disc folder:", "PASS" if ok else "FAIL", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
