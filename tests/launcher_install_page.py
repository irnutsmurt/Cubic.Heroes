"""The launcher's install page by controller, hidden (--nav-test): Install / Manage as a player meets it -- two numbered
steps (Your disc, Where to install) and Install; the game's program keys found or downloaded by themselves and shown as
a status under the disc; the player's own keys under Advanced; an encrypted image's disc key a step of its own, for it
alone. Windows, Linux and SteamOS alike, at each of --sizes.

Issue #6 (0.2.0-beta.6): the disc's folder is chosen in the disc browser with "Use this folder" -- the folder holding
PS3_GAME, or PS3_GAME itself (its parent is then the disc's folder); an ISO is still chosen as a file in the same
browser. The owner, 2026-11-27: the program keys out of the numbered steps (Advanced).

"Two steps" below: from the disc's Choose..., one press down reaches Where to install's Choose..., the next the choices
made before installing (beta.6, the owner's playtest: the game version and the shaders), then Install -- no key control
between them, and Install enabled. Scenarios (each in a fresh folder under --work: the disc folder's
files hard links of --folder's, an image a hard link -- nothing of the game copied, all checked unchanged at the end):

  folder        the folder holding PS3_GAME browsed to and chosen; its keys beside it found by themselves: two steps
  ps3-game      PS3_GAME itself chosen: its parent is what is used and remembered; two steps
  iso           the decrypted ISO chosen as a file in the same browser: two steps
  no-param      a folder holding an empty PS3_GAME: chosen, then Install blocked and explained
  not-a-disc    a folder without PS3_GAME, as launcher.json may hold one: Install blocked and explained
  remembered    the disc's folder already in launcher.json: checked at once; two steps
  advanced      keys found by themselves (two steps), then the player's own keys file chosen anyway under Advanced:
                Install, the choice remembered
  keys-missing  no keys anywhere, offline: the failure explained, Advanced open by itself with "Look for the keys
                again"; the player's keys file chosen there: Install
  elf           (--elf) the same, the player's decrypted EBOOT.elf chosen
  key-folder    (--key-folder) the same, a folder of key files chosen
  encrypted     (--encrypted and --disc-key) an encrypted image: its disc key's step, the .dkey chosen in its own
                browser; then Where to install and Install
  download      (--online) no keys beside the disc: downloaded by themselves; two steps
  choices       (beta.6) offline: the official update can't be checked, so v1.00 is chosen and why is said; v1.01
                chosen by the D-pad blocks Install with the reason; v1.00 again, the shaders "Not now"; Install asks to
                confirm v1.00, Cancel; every enabled button reached by the D-pad (reach)
  choices-online (--online) the update ready: v1.01 chosen by itself, "Also build v1.00" offered; Install asks to
                confirm v1.01, Cancel; reach
With --install (a real install each, a few minutes and ~4 GB, removed after unless --keep):
  install       the folder scenario, then Install and its confirmation: the game installed from the disc's folder in
                one run -- with --online, v1.01 (the update downloaded, ONE build: v1.01's) and its shaders prepared,
                no question after it; offline, v1.00. With --online and no keys beside the disc, the program's keys
                are downloaded, as on a computer without any
  install-v100  (--online) v1.00 chosen and the shaders "Not now": v1.00 installed, nothing prepared, no question
  incomplete    the disc's folder with one of the game's files missing: chosen, then Install refused at its check

The launcher before issue #6's fix fails the folder scenarios at "Use this folder"; the one before the keys left the
numbered steps fails the two-step checks (its keys step in between).

Waits that end by themselves (settle: the worker's checks, a download; until: an install) are bounded by frames only
loosely: Linux's hidden launcher draws a frame in ~1 ms, Windows' at the display's rate.

Needs the disc image and a keys file of your own (local only, never in the repository): --iso, --keys, and the disc's
folder -- --folder, or extracted from --iso (tools/iso9660.py) into --work and removed after; skipped (exit 0)
without them. Keys are never written to a script, a log or an argument: only file paths.

  py -3 tests/launcher_install_page.py --launcher <the launcher to test> [--iso ISO] [--keys KEYS] [--folder DIR]
      [--elf ELF] [--key-folder DIR] [--encrypted ISO --disc-key DKEY] [--only NAME ...] [--install] [--online]
      [--sizes 1280x800,1280x720]
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
ENC = "3D Dot Game Heroes (encrypted).iso"
DKEY = "3D Dot Game Heroes (encrypted).dkey"
MISSING = "PS3_GAME/ICON0.PNG"            # incomplete: a game file the disc check doesn't read, the install's does
# the player's own keys, beside the disc under names the automatic search doesn't take (it takes "key" names and
# EBOOT.elf): only what they choose under Advanced finds them
MINE = {"file": "mine.txt", "elf": "my-program.elf", "folder": "my-ps3"}

LONG = 50000000   # frames at most for what ends by itself
HEAD = f"until 200000 Install the game\nexpect Install the game\na\nsettle {LONG}\n"   # Install / Manage, a new folder
OPEN = ("expect Choose...\na\nwait 40\nsee Choose your disc\nexpect browse: ..\n"   # the browser, at the install's folder
        "a\nwait 40\nseek 600 down browse: games\nexpect browse: games\na\nwait 40\n")   # up, then into games/
INTO_DISC = f"seek 600 down browse: {DISC}\nexpect browse: {DISC}\na\nwait 40\n"
INTO_GAME = "seek 600 down browse: PS3_GAME\nexpect browse: PS3_GAME\na\nwait 40\n"
USE = f"shot folder\nseek 200 up Use this folder\nexpect Use this folder\nshot browser\na\nwait 20\nsettle {LONG}\n"


def pick(name):   # a file in the browser shown
    return f"seek 600 down browse: {name}\nexpect browse: {name}\nshot browser\na\nwait 20\nsettle {LONG}\n"


# the two steps, down to the choices made before installing (the focus on the version's row), then Install
TO_CHOICES = ("expect Choose...\nsee Your disc\ndown\nwait 20\nexpect Choose...\nsee Where to install\n"
              f"settle {50000000}\ndown\nwait 20\nexpect v1.00|v1.01\nsee Before installing\n")
TWO = TO_CHOICES + "seek 600 down Install\nexpect Install\nshot install\n"
BLOCKED = ("seek 600 down Advanced: your own program keys\nexpect Advanced: your own program keys\nshot install\n"
           "see Install can't start yet\nquit\n")
FAILED = ("shot status\nseek 600 down Look for the keys again\nexpect Look for the keys again\nshot failed\n"
          "see Install can't start yet\nsee Advanced: your own program keys\ndown\nwait 20\n")
ADV_OPEN = ("down\nwait 20\nexpect Advanced: your own program keys\na\nwait 20\nexpect Hide the advanced options\n"
            "down\nwait 20\nsee Advanced: your own program keys\nshot advanced\n")
# the card's first row holds two buttons, and the D-pad lands on either: the one wanted is sought along the row
FIRST = "seek 100 left A keys file, ZIP or EBOOT.elf...\n"
# the keys browser opens where the disc was chosen (the disc's folder): up to games/, where the player's keys are
CHOOSE_FILE = (FIRST + "expect A keys file, ZIP or EBOOT.elf...\na\nwait 40\nsee Choose your keys\nexpect browse: ..\na\nwait 40\n"
               + pick(MINE["file"]))
CHOOSE_ELF = (FIRST + "expect A keys file, ZIP or EBOOT.elf...\na\nwait 40\nsee Choose your keys\nexpect browse: ..\na\nwait 40\n"
              + pick(MINE["elf"]))
CHOOSE_FOLDER = ("seek 100 right A folder of keys...\nexpect A folder of keys...\na\nwait 40\n"
                 "see Choose the folder of key files\nexpect browse: ..\na\nwait 40\n"
                 f"seek 600 down browse: {MINE['folder']}\nexpect browse: {MINE['folder']}\na\nwait 40\n"
                 f"seek 200 up Use this folder\nexpect Use this folder\nshot browser\na\nwait 20\nsettle {LONG}\n")
AFTER_MINE = "seek 600 up Install\nexpect Install\nshot install\nquit\n"
ENC_KEY = ("expect Choose...\nshot needs-key\ndown\nwait 20\nexpect Choose...\nsee Its disc key\na\nwait 40\n"
           "see Choose the disc key\n" + pick(DKEY)
           # the key opens the image (the focus is back on one of the steps' buttons, by the window's size): from
           # Install up, its three steps -- where, the disc key, the disc
           + "shot key-chosen\nseek 600 down Install\nexpect Install\nshot install\nseek 600 up Choose...\nexpect Choose...\n"
             "see Where to install\nup\nwait 20\nexpect Choose...\nsee Its disc key\nup\nwait 20\nexpect Choose...\n"
             "see Your disc\nquit\n")
# Install, its confirmation (the plan: from where, into where, what is built), then the install in one go, and the
# end said once -- no question about the shaders after it (chosen before)
INSTALL = (f"a\nwait 60\nsee Install 3D Dot Game Heroes\nexpect Install\nshot confirm\nwait 20\na\nwait 120\n"
           f"until {LONG} OK\nsee installed and ready to play\nshot installed\nquit\n")
REFUSED = (f"a\nwait 60\nsee Install 3D Dot Game Heroes\nexpect Install\na\nwait 120\nuntil {LONG} OK\n"
           "see The install didn't finish\nshot refused\nquit\n")
# the choices offline: v1.00 by itself (the update can't be checked), v1.01 chosen blocks Install with the reason
CHOICES = ("seek 600 down v1.00\nexpect v1.00\nshot choices\nsee Game version\nleft\nwait 20\nexpect v1.01\na\nwait 20\n"
           "seek 600 down Advanced: your own program keys\nsee Install can't start yet\nshot v101-blocked\nwait 20\n"
           "seek 600 up v1.00|v1.01\nseek 100 right v1.00\nexpect v1.00\na\nwait 20\n"
           "seek 600 down Not now|Prepare\nseek 100 right Not now\nexpect Not now\na\nwait 20\nseek 600 down Install\n"
           "expect Install\na\nwait 60\nsee Install 3D Dot Game Heroes v1.00?\nshot confirm\nwait 20\nb\nwait 60\n"
           f"settle {LONG}\nreach 60000\nquit\n")
CHOICES_ONLINE = (f"settle {LONG}\nwait 60\nseek 600 down v1.00|v1.01\nseek 100 left v1.01\nexpect v1.01\nshot choices\n"
                  "wait 20\nseek 600 down Also build v1.00\nexpect Also build v1.00\nseek 600 down Install\nexpect Install\n"
                  "a\nwait 60\nsee Install 3D Dot Game Heroes v1.01?\nshot confirm\nwait 20\nb\nwait 60\n"
                  f"settle {LONG}\nreach 60000\nquit\n")
# v1.00 and no shaders chosen by the D-pad on the way down (TO_CHOICES leaves the focus on the version's row,
# v1.00 by the right-hand step above it), then Install
PICK_V100 = ("seek 100 right v1.00\nexpect v1.00\na\nwait 20\nseek 600 down Not now|Prepare\nseek 100 right Not now\n"
             "expect Not now\na\nwait 20\nseek 600 down Install\nexpect Install\nshot install\n")
PICK_FOLDER = HEAD + OPEN + INTO_DISC + USE


def scenarios(a):
    """name: {layout, script, disc (what launcher.json should remember), keys (the player's keys it should remember),
    beside (a keys file beside the disc), mine (the player's own keys, beside it under another name), online, real}"""
    s = {
        "folder": dict(script=PICK_FOLDER + TWO + "quit\n"),
        "ps3-game": dict(script=HEAD + OPEN + INTO_DISC + INTO_GAME + USE + TWO + "quit\n"),
        "iso": dict(layout="iso", script=HEAD + OPEN + pick(ISO) + TWO + "quit\n", disc="iso"),
        "no-param": dict(layout="no-param", script=PICK_FOLDER + BLOCKED),
        "not-a-disc": dict(prefill="games", script=HEAD + BLOCKED, disc="games"),
        "remembered": dict(prefill="folder", script=HEAD + TWO + "quit\n"),
        "advanced": dict(mine="file", script=PICK_FOLDER + TWO + ADV_OPEN + CHOOSE_FILE + AFTER_MINE, keys="file"),
        "keys-missing": dict(beside=False, mine="file", script=PICK_FOLDER + FAILED + CHOOSE_FILE + AFTER_MINE,
                             keys="file"),
    }
    if a.elf:
        s["elf"] = dict(beside=False, mine="elf", script=PICK_FOLDER + FAILED + CHOOSE_ELF + AFTER_MINE, keys="elf")
    if a.key_folder:
        s["key-folder"] = dict(beside=False, mine="folder", script=PICK_FOLDER + FAILED + CHOOSE_FOLDER + AFTER_MINE,
                               keys="folder")
    if a.encrypted and a.disc_key:
        s["encrypted"] = dict(layout="encrypted", script=HEAD + OPEN + pick(ENC) + ENC_KEY, disc="encrypted")
    s["choices"] = dict(prefill="folder", script=HEAD + CHOICES)
    if a.online:
        s["download"] = dict(beside=False, online=True, script=PICK_FOLDER + TWO + "quit\n", downloaded=True)
        s["choices-online"] = dict(prefill="folder", online=True, script=HEAD + CHOICES_ONLINE)
    if a.install:
        s["install"] = dict(beside=not a.online, online=a.online, real=True, downloaded=a.online,
                            version="v101" if a.online else "v100", shaders=True, script=PICK_FOLDER + TWO + INSTALL)
        if a.online:
            s["install-v100"] = dict(online=True, real=True, version="v100", shaders=False,
                                     script=PICK_FOLDER + TO_CHOICES + PICK_V100 + INSTALL)
        s["incomplete"] = dict(layout="incomplete", script=PICK_FOLDER + TWO + REFUSED, slow=True)
    return s


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


def link(src, dst):
    """a second name for the same file (a copy where links can't be made: another volume)"""
    try:
        os.link(src, dst)
    except OSError:
        try:
            os.symlink(src, dst)
        except OSError:
            shutil.copyfile(src, dst)


def link_tree(src, dst, skip=()):
    """the disc's folder as hard links: its files never written"""
    for d, _, files in os.walk(src):
        rel = os.path.relpath(d, src)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for n in files:
            r = os.path.normpath(os.path.join(rel, n)).replace(os.sep, "/")
            if r not in skip:
                link(os.path.join(d, n), os.path.join(dst, rel, n))


def prefs(root):
    try:
        with open(os.path.join(root, "config", "launcher.json"), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def same(a, b):
    return bool(a) and bool(b) and os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def installed_as(name, size, root, sc):
    """a real install's result (beta.6): the version chosen -- v1.01 in ONE build, v1.00's files kept but not built --
    the shaders prepared only when chosen, the choices remembered (no offer of v1.01 on Play again, the setting)"""
    ok = True

    def bad(msg):
        nonlocal ok
        ok = False
        print(f"FAIL: {name} at {size}: {msg}", flush=True)
    with open(os.path.join(root, "active.json"), encoding="utf-8") as f:
        active = json.load(f)["active"]
    builds = {}
    for b in os.listdir(os.path.join(root, "builds")):
        p = os.path.join(root, "builds", b, "build.json")
        if os.path.isfile(p):
            with open(p, encoding="utf-8") as f:
                builds[b] = json.load(f)
    program = builds.get(active, {}).get("program")
    if sc.get("version") == "v101":
        if program != "BLUS30490-01.01":
            bad(f"the active build's program is {program!r}, not v1.01's")
        if len(builds) != 1:
            bad(f"{len(builds)} builds, not one (v1.00 built too?)")
    elif sc.get("version") == "v100" and program:
        bad(f"the active build's program is {program!r}, not the disc's")
    shaders = os.path.join(root, "data", "cache", "shaders", builds.get(active, {}).get("shader_cache") or "default",
                           "prepare-result.json")
    if sc.get("shaders") is not None and os.path.isfile(shaders) != sc["shaders"]:
        bad("the shaders were " + ("not " if sc["shaders"] else "") + "prepared")
    got = prefs(root)
    want_offer = "accepted" if sc.get("version") == "v101" else "declined"
    if got.get("update_offer") != want_offer:
        bad(f"launcher.json's update_offer is {got.get('update_offer')!r}, not {want_offer!r}")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--launcher", required=True, help="the launcher to test (a build, or a package's)")
    ap.add_argument("--iso", default=os.path.join(ROOT, "3D DOT GAME HEROES [BLUS-30490].iso"))
    ap.add_argument("--keys", default=os.path.join(ROOT, "build", "keys", "self_keys.txt"))
    ap.add_argument("--folder", help="the disc's folder (holding PS3_GAME); default: extracted from --iso")
    ap.add_argument("--elf", help="an EBOOT.elf you decrypted (the elf scenario)")
    ap.add_argument("--key-folder", help="a folder of key files (the key-folder scenario)")
    ap.add_argument("--encrypted", help="an encrypted image of the disc (the encrypted scenario), with --disc-key")
    ap.add_argument("--disc-key", help="its disc key file (.dkey)")
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "qual", "launcher-install-page"))
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--install", action="store_true", help="also the real installs (install, incomplete)")
    ap.add_argument("--online", action="store_true", help="download, install: no keys beside the disc -- downloaded")
    ap.add_argument("--keep", action="store_true", help="keep each scenario's folder (an install: ~4 GB)")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--sizes", default="1280x800,1280x720")
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
    watched = {"folder": folder, "iso": a.iso}
    if a.encrypted:
        watched["encrypted"] = a.encrypted
    before = {k: tree_state(v) for k, v in watched.items()}
    with open(a.keys, "rb") as f:
        keys = f.read()
    ok = True
    for name, sc in scenarios(a).items():
        if a.only and name not in a.only:
            continue
        layout = sc.get("layout", "folder")
        for size in [s.strip() for s in a.sizes.split(",") if s.strip()]:
            d = os.path.join(work, name, size)
            shutil.rmtree(d, ignore_errors=True)
            games = os.path.join(d, "games")
            os.makedirs(games)
            places = {"folder": os.path.join(games, DISC), "iso": os.path.join(games, ISO),
                      "encrypted": os.path.join(games, ENC), "games": games}
            if layout == "iso":
                link(a.iso, places["iso"])
            elif layout == "encrypted":
                link(a.encrypted, places["encrypted"])
                shutil.copyfile(a.disc_key, os.path.join(games, DKEY))
            elif layout == "no-param":
                os.makedirs(os.path.join(places["folder"], "PS3_GAME"))
            else:
                link_tree(folder, places["folder"], skip=(MISSING,) if layout == "incomplete" else ())
            if sc.get("beside", True):   # the program's keys beside the disc (else: none, or downloaded)
                with open(os.path.join(games, "keys.txt"), "wb") as f:
                    f.write(keys)
            mine = sc.get("mine")
            mine_path = os.path.join(games, MINE[mine]) if mine else None
            if mine == "file":
                with open(mine_path, "wb") as f:
                    f.write(keys)
            elif mine == "elf":
                link(a.elf, mine_path)
            elif mine == "folder":
                shutil.copytree(a.key_folder, mine_path)
            root = os.path.join(d, "Cubic.Heroes")
            os.makedirs(os.path.join(root, "config"))
            with open(os.path.join(root, "config", "launcher.json"), "w", encoding="utf-8") as f:
                json.dump({"disc": places.get(sc.get("prefill"), ""), "keys": "", "rpcs3": ""}, f)
            sp = os.path.join(d, f"{name}.nav")
            with open(sp, "w", encoding="utf-8", newline="\n") as f:
                f.write(f"# {name} (tests/launcher_install_page.py)\n" + sc["script"])
            slow = sc.get("real") or sc.get("slow")
            r = nav.run(sp, launcher, root, os.path.join(d, "work"), a.timeout * (12 if slow else 1), size,
                        online=bool(sc.get("online")))
            got = prefs(root)
            expect = places[sc.get("disc", "folder")]
            if not same(got.get("disc"), expect):
                print(f"FAIL: {name} at {size}: launcher.json's disc is {got.get('disc')!r}, not {expect!r}", flush=True)
                r = False
            if sc.get("keys") and not same(got.get("keys"), mine_path):
                print(f"FAIL: {name} at {size}: launcher.json's keys are {got.get('keys')!r}, not {mine_path!r}",
                      flush=True)
                r = False
            keys_dir = os.path.join(root, "data", "keys")
            got_keys = os.path.isdir(keys_dir) and bool(os.listdir(keys_dir))
            if sc.get("downloaded") and r and not got_keys:
                print(f"FAIL: {name} at {size}: the keys weren't downloaded into data/keys", flush=True)
                r = False
            if not sc.get("online") and got_keys:   # offline: nothing downloaded
                print(f"FAIL: {name} at {size}: keys were downloaded offline", flush=True)
                r = False
            if sc.get("real") and r:
                info = None
                try:
                    with open(os.path.join(root, "data", "install.json"), encoding="utf-8") as f:
                        info = json.load(f)
                except (OSError, ValueError):
                    pass
                if not info or not os.path.isdir(os.path.join(root, "game")):
                    print(f"FAIL: {name} at {size}: no installed game (data/install.json, game/)", flush=True)
                    r = False
                else:
                    r = installed_as(name, size, root, sc) and r
                    print(f"{'PASS' if r else 'FAIL'}: {name} at {size}: installed from the disc's folder (source "
                          f"{info.get('source', '?')!r}" + (", the keys downloaded" if sc.get("downloaded") else "")
                          + ")", flush=True)
            ok = r and ok
            for p in (places["iso"], places["encrypted"]):
                if os.path.isfile(p):
                    os.remove(p)   # the link only: the disc image itself untouched
            if not a.keep:
                shutil.rmtree(d, ignore_errors=True)
    after = {k: tree_state(v) for k, v in watched.items()}
    for k in watched:
        if before[k] != after[k]:
            ok = False
            print(f"FAIL: the disc's {k} changed", flush=True)
    if before == after:
        print("PASS: the disc's folder and images unchanged", flush=True)
    if extracted:
        shutil.rmtree(os.path.join(work, "source"), ignore_errors=True)
    print("launcher install page:", "PASS" if ok else "FAIL", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
