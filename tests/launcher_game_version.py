"""The launcher's game version (beta.6): the one-time offer of the official v1.01 update on Play, and Install / Manage >
Game version -- the checks in plain words, a disabled Install explained with the remedies the launcher offers, Install
enabled once every check passes. Hidden (--nav-test), offline (tests/launcher_nav_pages.py), on a made-up installation
(a v1.00 record and a stand-in build: nothing is built or played; nothing is installed -- the scripts stop at the
button). Scenarios:

  no-support   a checkout without the v1.01 program's configuration (no config/programs/*/program.json naming
               BLUS30490-01.01): offered, then explained ("can't install the v1.01 update yet"); Continue with v1.00
               offered -- skipped where the configuration is there
  no-package   offline, no update package anywhere: Play's Install v1.01 says why it can't be installed yet, and its
               "Open Game Version" opens Install / Manage on Game Version, its first remedy focused (Choose the update
               package... and the others)
  ready        the official package where a download is kept (data/updates) and the keys downloaded before
               (data/keys): Play's Install v1.01 asks to confirm the update at once (beta.6, the owner's playtest: it
               only opened Install / Manage); Cancel goes back to Play; then Game Version's Install v1.01, its
               confirmation, Cancel
  disc         (the owner's playtest) the same, with the disc remembered, so "Repair from the disc" is enabled: the
               D-pad goes down from it into Game Version ("See what v1.01 needs" -- the card was skipped, wholly below
               the view), then Install v1.01 and its confirmation
  v101         a v1.01 installation with no v1.00 build (installed with the disc in one run): Game Version offers
               "Install v1.00" and asks to confirm it (built from the disc's files kept, the disc not needed)
  v101-both    the same with a v1.00 build kept: "Switch to v1.00" and its confirmation
Every scenario ends with a "reach" walk: every enabled button of Install / Manage takes the focus by the D-pad alone.

With --real DIR (a real installation of v1.00 made for testing: it is UPDATED), the whole update by the D-pad alone:
  upgrade      Play's offer, Install v1.01, its confirmation, the real update (download, build), its message closed;
               then Install / Manage > Game Version: Switch to v1.00 (its confirmation), and Switch to v1.01 again --
               the shaders prepared in the background by the setting, waited for (idle)

no-package and ready need the program's configuration (skipped without it). ready needs the official package and the downloaded keys (local only, never in the
repository): --pkg (default build/v101/pkg/<its official name>) and --keys-dir (a data/keys folder the launcher
downloaded, default build/keys/downloaded); without them ready is skipped.

  py -3 tests/launcher_game_version.py --launcher <the launcher to test> [--only NAME ...] [--sizes 1280x800]
"""
import argparse
import hashlib
import json
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import launcher_nav_pages as nav  # noqa: E402
import update_preflight  # noqa: E402

SUPPORTED = bool(update_preflight.program_dir(update_preflight.UPDATE_ID))
# A script's `wait N` counts the launcher's frames, and a hidden launcher's frame rate is the system's: on Windows a
# frame takes a few milliseconds, on Linux offscreen ~0.5 ms -- there `wait 240` ended before the worker's check
# answered (found by the Linux instance on the 0.2.0-beta.6 candidate: every scenario passed with the waits x50).
# The waits are scaled per system (NAV_WAIT_SCALE overrides); until/seek bounds are maxima and stay as written.
WAIT_SCALE = int(os.environ.get("NAV_WAIT_SCALE") or (1 if os.name == "nt" else 50))


def scaled(script):
    return "\n".join(f"wait {int(ln.split()[1]) * WAIT_SCALE}" if ln.startswith("wait ") else ln
                     for ln in script.split("\n"))
# A `shot` reads the frame back and writes its picture on the launcher's main thread, so the next frame is slow (Linux,
# hidden: 190-236 ms). A key held across it repeats: a seek's two-frame press moved the focus one past its target, and
# the next A pressed "Check for updates" (found by the Linux instance on the 0.2.0-beta.6 candidate). No input follows
# a shot until the frames have settled (`wait 20`). A player's launcher takes no screenshots.
OFFER = ("until 200000 PLAY\nexpect PLAY\nwait 240\nsee Official Game Update v1.01\nshot 1-offer\nwait 20\n"
         "seek 6000 down Install v1.01\nexpect Install v1.01\na\nwait 60\nsettle 2000000\nwait 60\n")
REACH = "wait 60\nsettle 2000000\nreach 60000\n"   # every enabled button of the page, by the D-pad alone
# the update's confirmation, then Cancel: nothing is installed (the made-up installation has no game to build)
CONFIRM = "see Install the official v1.01 update?\nexpect Install v1.01\nshot 3-confirm\nwait 20\nb\nwait 60\n"
SCRIPTS = {
    "no-support": OFFER + "see The official update can't be installed yet\nshot 2-explained\nwait 20\na\nwait 120\n"
                          "seek 60000 down Continue with v1.00\nexpect Continue with v1.00\nsee Game Version\n"
                          + REACH + "quit\n",
    "no-package": OFFER + "see The official update can't be installed yet\nexpect Open Game Version\nshot 2-explained\n"
                          "wait 20\na\nwait 120\nexpect Try the official download again\nseek 100 right Choose the update package\nseek 300 down Choose the update package\nexpect Choose the update package\nshot 3-manage\n"
                          + REACH + "quit\n",
    "ready": OFFER + CONFIRM + "expect Install v1.01\nsee Official Game Update v1.01\nrb\nwait 120\n"
                     "seek 60000 down Install v1.01\nexpect Install v1.01\nsee Game Version\nshot 4-manage\nwait 20\n"
                     "a\nwait 60\n" + CONFIRM + "expect Install v1.01\n" + REACH + "quit\n",
    # the owner's playtest: the disc remembered -- Repair enabled, Game Version's card below it
    "disc": ("until 200000 PLAY\nexpect PLAY\nwait 240\nseek 6000 down Install v1.01\nexpect Install v1.01\nright\nwait 20\nexpect Not Now\na\nwait 60\nrb\n"
             "wait 120\nsettle 2000000\nwait 60\nseek 6000 down Repair from the disc\nexpect Repair from the disc\n"
             "shot 1-repair\nwait 20\ndown\nwait 30\nexpect See what v1.01 needs\nsee Game Version\nshot 2-reached\n"
             "wait 20\na\nwait 60\nsettle 2000000\nwait 60\nseek 60000 down Install v1.01\nexpect Install v1.01\n"
             "a\nwait 60\n" + CONFIRM + "expect Install v1.01\n" + REACH + "quit\n"),
    "v101": ("until 200000 PLAY\nexpect PLAY\nwait 240\nrb\nwait 120\nsettle 2000000\nwait 60\n"
             "seek 60000 down Install v1.00\nexpect Install v1.00\nsee Game Version\nshot 1-v101\nwait 20\na\nwait 60\n"
             "see Install v1.00?\nexpect Cancel\nshot 2-confirm\nwait 20\nb\nwait 60\nexpect Install v1.00\n"
             + REACH + "quit\n"),
    # a real v1.00 installation updated, and switched both ways, by the D-pad alone (--real)
    "upgrade": ("until 200000 PLAY\nexpect PLAY\nwait 240\nsee Official Game Update v1.01\n"
                "seek 6000 down Install v1.01\nexpect Install v1.01\na\nwait 60\nsettle 2000000\nwait 60\n"
                "see Install the official v1.01 update?\nexpect Install v1.01\nshot 1-confirm\nwait 20\na\nwait 120\n"
                "until 50000000 OK\nsee v1.01 is installed\nshot 2-installed\nwait 20\na\nwait 60\nidle 50000000\n"
                "rb\nwait 120\nsettle 2000000\nwait 60\nseek 60000 down Switch to v1.00\nexpect Switch to v1.00\n"
                "shot 3-v101\nwait 20\na\nwait 60\nsee Switch back to v1.00?\nexpect Cancel\nleft\nwait 20\n"
                "expect Switch to v1.00\na\nwait 120\nsettle 50000000\nidle 50000000\nsettle 2000000\nwait 60\n"
                "seek 60000 down Switch to v1.01\nexpect Switch to v1.01\nshot 4-v100\nwait 20\na\nwait 120\n"
                "until 50000000 OK\nsee Switched to v1.01\nshot 5-switched\nwait 20\na\nwait 60\n" + REACH + "quit\n"),
    "v101-both": ("until 200000 PLAY\nexpect PLAY\nwait 240\nrb\nwait 120\nsettle 2000000\nwait 60\n"
                  "seek 60000 down Switch to v1.00\nexpect Switch to v1.00\nsee Game Version\nshot 1-v101\nwait 20\na\n"
                  "wait 60\nsee Switch back to v1.00?\nexpect Cancel\nshot 2-confirm\nwait 20\nb\nwait 60\n"
                  "expect Switch to v1.00\n" + REACH + "quit\n"),
    # declined: Not Now takes the offer away (remembered: launcher.json update_offer "declined"), and Manage > Game
    # Version still offers the update -- its full check, then Install v1.01
    "not-now": ("until 200000 PLAY\nexpect PLAY\nwait 240\nsee Official Game Update v1.01\n"
                "seek 6000 down Install v1.01\nexpect Install v1.01\nright\nwait 20\nexpect Not Now\na\nwait 60\n"
                "shot 1-declined\nwait 20\nrb\nwait 60\n"
                "seek 60000 down See what v1.01 needs\nexpect See what v1.01 needs\nsee Game Version\na\nwait 120\n"
                "seek 60000 down Install v1.01\nexpect Install v1.01\nshot 2-later\nquit\n"),
}


def stand_in_build(root, bid, program=None):
    """a stand-in build (never run): its file and record; program: the official update's id for a v1.01 build"""
    b = os.path.join(root, "builds", bid)
    os.makedirs(b)
    exe = os.path.join(b, "dotheroes.exe" if os.name == "nt" else "dotheroes")
    with open(exe, "wb") as f:
        f.write(b"a stand-in build: never run")
    rec = {"id": bid, "lift": "test", "objects": [], "shader_cache": "test", "version": "test",
           "exe_size": os.path.getsize(exe), "exe_sha256": hashlib.sha256(open(exe, "rb").read()).hexdigest(),
           "made": "2026-11-25T00:00:00"}
    if program:
        rec.update(program=program, update_dir=f"data/official/{program}")
    with open(os.path.join(b, "build.json"), "w") as f:
        json.dump(rec, f)
    return bid


def make_install(root, pkg=None, keys_dir=None, disc="", version="v100"):
    """a made-up installation: its record (the supported program's pin), stand-in builds, no saves. version "v101":
    v1.01 active (the update's record and files' place), without a v1.00 build; "v101-both": with one kept"""
    gv = json.load(open(os.path.join(ROOT, "config", "game_version.json"), encoding="utf-8"))
    os.makedirs(os.path.join(root, "config"))
    with open(os.path.join(root, "config", "launcher.json"), "w", encoding="utf-8") as f:
        json.dump({"disc": disc, "keys": "", "rpcs3": ""}, f)
    up = update_preflight.UPDATE_ID
    if version.startswith("v101"):
        prev = [stand_in_build(root, "20261125-000000-test")] if version == "v101-both" else []
        active = stand_in_build(root, "20261125-000001-test", up)
    else:
        prev, active = [], stand_in_build(root, "20261125-000000-test")
    with open(os.path.join(root, "active.json"), "w") as f:
        json.dump({"layout": 2, "active": active, "previous": prev, "retention": 1}, f)
    os.makedirs(os.path.join(root, "data", "gamedata"))
    rec = {"elf_sha256": gv["decrypted_elf"]["sha256"], "lift": "test"}
    if version.startswith("v101"):
        rec.update(official_content=[update_preflight.BASE_ID, up], game_update={"program": up})
    with open(os.path.join(root, "data", "install.json"), "w") as f:
        json.dump(rec, f)
    if pkg:
        os.makedirs(os.path.join(root, "data", "updates"))
        dst = os.path.join(root, "data", "updates", gv["update"]["file"])
        try:
            os.link(pkg, dst)                   # a second name for the same file: nothing of it copied
        except OSError:
            os.symlink(pkg, dst)
    if keys_dir:
        shutil.copytree(keys_dir, os.path.join(root, "data", "keys"))


def upgrade(a, launcher, work, script):
    """the upgrade scenario on the real installation --real, at the first size: online (the official download, the
    keys); the shaders prepared after updates (the setting a player chose at the install), so no question follows"""
    import subprocess
    size = [s.strip() for s in a.sizes.split(",") if s.strip()][0]
    d = os.path.join(work, "upgrade", size)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    root = os.path.abspath(a.real)
    worker = os.path.join(root, "worker", "DotHeroes-Setup.exe" if os.name == "nt" else "DotHeroes-Setup")
    subprocess.run([worker, "--worker", "settings", "--install", root, "--set", "LAUNCHER_SHADERS_AFTER_UPDATE=1"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    sp = os.path.join(d, "upgrade.nav")
    with open(sp, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# upgrade (tests/launcher_game_version.py; waits x{WAIT_SCALE})\n" + scaled(script))
    ran = nav.run(sp, launcher, root, os.path.join(d, "work"), 3600, size, online=True)
    state = update_preflight.game_state(root)["state"]
    print(("PASS" if state == "v101" else "FAIL") + f": upgrade: the installation is {state} at the end", flush=True)
    return ran and state == "v101"


def main():
    gv = json.load(open(os.path.join(ROOT, "config", "game_version.json"), encoding="utf-8"))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--launcher", required=True)
    ap.add_argument("--pkg", default=os.path.join(ROOT, "build", "v101", "pkg", gv["update"]["file"]))
    ap.add_argument("--keys-dir", default=os.path.join(ROOT, "build", "keys", "downloaded"))
    ap.add_argument("--disc", help="the disc's folder or image, remembered (the disc scenario; local only)")
    ap.add_argument("--real", help="upgrade: a real installation of v1.00 made for testing -- it is UPDATED to v1.01")
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "qual", "launcher-game-version"))
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--sizes", default="1280x800")
    a = ap.parse_args()
    launcher, work = os.path.abspath(a.launcher), os.path.abspath(a.work)
    have_inputs = os.path.isfile(a.pkg) and os.path.isdir(a.keys_dir)
    ok = True
    for name, script in SCRIPTS.items():
        if a.only and name not in a.only:
            continue
        if name in ("ready", "not-now", "disc") and not have_inputs:
            print(f"SKIP: {name} needs the official package and downloaded keys (local only: --pkg, --keys-dir)")
            continue
        if name == "disc" and not (a.disc and os.path.exists(a.disc)):
            print("SKIP: disc needs the disc's folder or image (local only: --disc)")
            continue
        if name == "upgrade":
            if not (a.real and os.path.isfile(os.path.join(a.real, "active.json"))):
                print("SKIP: upgrade needs a real v1.00 installation made for testing (--real)")
                continue
            ok = upgrade(a, launcher, work, script) and ok
            continue
        if name not in ("no-support", "v101", "v101-both") and not SUPPORTED or name == "no-support" and SUPPORTED:
            print(f"SKIP: {name} -- this checkout " + ("has" if SUPPORTED else "hasn't") + " the v1.01 program's "
                  "configuration")
            continue
        for size in [s.strip() for s in a.sizes.split(",") if s.strip()]:
            d = os.path.join(work, name, size)
            shutil.rmtree(d, ignore_errors=True)
            root = os.path.join(d, "Cubic.Heroes")
            full = name in ("ready", "not-now", "disc")
            make_install(root, a.pkg if full else None, a.keys_dir if full else None,
                         disc=os.path.abspath(a.disc) if name == "disc" else "",
                         version=name if name.startswith("v101") else "v100")
            sp = os.path.join(d, f"{name}.nav")
            with open(sp, "w", encoding="utf-8", newline="\n") as f:
                f.write(f"# {name} (tests/launcher_game_version.py; waits x{WAIT_SCALE})\n" + scaled(script))
            ran = nav.run(sp, launcher, root, os.path.join(d, "work"), a.timeout, size)
            if ran and name == "not-now":   # the answer remembered: no offer again
                import json as _json
                with open(os.path.join(root, "config", "launcher.json"), encoding="utf-8") as f:
                    answer = _json.load(f).get("update_offer")
                ran = answer == "declined"
                print(("PASS" if ran else "FAIL") + f": not-now: the offer's answer remembered ({answer!r})", flush=True)
            ok = ran and ok
            upd = os.path.join(root, "data", "updates")
            if os.path.isdir(upd):
                shutil.rmtree(upd)          # the link only: the package itself untouched
    print("launcher game version:", "PASS" if ok else "FAIL", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
