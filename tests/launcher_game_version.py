"""The launcher's game version (beta.6): the one-time offer of the official v1.01 update on Play, and Install / Manage >
Game version -- the checks in plain words, a disabled Install explained with the remedies the launcher offers, Install
enabled once every check passes. Hidden (--nav-test), offline (tests/launcher_nav_pages.py), on a made-up installation
(a v1.00 record and a stand-in build: nothing is built or played; nothing is installed -- the scripts stop at the
button). Scenarios:

  no-support   a checkout without the v1.01 program's configuration (no config/programs/*/program.json naming
               BLUS30490-01.01): offered, then explained ("can't install the v1.01 update yet"); Continue with v1.00
               offered -- skipped where the configuration is there
  no-package   offline, no update package anywhere: explained; Choose the update package... and the others offered
  ready        the official package where a download is kept (data/updates) and the keys downloaded before
               (data/keys): every check passes, Install v1.01 reachable

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
OFFER = ("until 200000 PLAY\nexpect PLAY\nwait 240\nsee Official Game Update v1.01\nshot 1-offer\n"
         "seek 6000 down Install v1.01\nexpect Install v1.01\na\nwait 60\n")
SCRIPTS = {
    "no-support": OFFER + "seek 60000 down Continue with v1.00\nexpect Continue with v1.00\nsee Game Version\n"
                          "shot 2-explained\nquit\n",
    "no-package": OFFER + "seek 60000 down Choose the update package\nexpect Choose the update package\n"
                          "shot 2-explained\nquit\n",
    "ready": OFFER + "seek 60000 down Install v1.01\nexpect Install v1.01\nsee Game Version\nshot 2-ready\nquit\n",
    # declined: Not Now takes the offer away (remembered: launcher.json update_offer "declined"), and Manage > Game
    # Version still offers the update -- its full check, then Install v1.01
    "not-now": ("until 200000 PLAY\nexpect PLAY\nwait 240\nsee Official Game Update v1.01\n"
                "seek 6000 down Install v1.01\nexpect Install v1.01\nright\nwait 20\nexpect Not Now\na\nwait 60\n"
                "shot 1-declined\nrb\nwait 60\n"
                "seek 60000 down See what v1.01 needs\nexpect See what v1.01 needs\nsee Game Version\na\nwait 120\n"
                "seek 60000 down Install v1.01\nexpect Install v1.01\nshot 2-later\nquit\n"),
}


def make_install(root, pkg=None, keys_dir=None):
    """a made-up v1.00 installation: its record (the supported program's pin), one stand-in build, no saves"""
    gv = json.load(open(os.path.join(ROOT, "config", "game_version.json"), encoding="utf-8"))
    os.makedirs(os.path.join(root, "config"))
    with open(os.path.join(root, "config", "launcher.json"), "w", encoding="utf-8") as f:
        json.dump({"disc": "", "keys": "", "rpcs3": ""}, f)
    b = os.path.join(root, "builds", "20261125-000000-test")
    os.makedirs(b)
    exe = os.path.join(b, "dotheroes.exe" if os.name == "nt" else "dotheroes")
    with open(exe, "wb") as f:
        f.write(b"a stand-in build: never run")
    with open(os.path.join(b, "build.json"), "w") as f:
        json.dump({"id": os.path.basename(b), "lift": "test", "objects": [], "shader_cache": "test", "version": "test",
                   "exe_size": os.path.getsize(exe), "exe_sha256": hashlib.sha256(open(exe, "rb").read()).hexdigest(),
                   "made": "2026-11-25T00:00:00"}, f)
    with open(os.path.join(root, "active.json"), "w") as f:
        json.dump({"layout": 2, "active": os.path.basename(b), "previous": [], "retention": 1}, f)
    os.makedirs(os.path.join(root, "data", "gamedata"))
    with open(os.path.join(root, "data", "install.json"), "w") as f:
        json.dump({"elf_sha256": gv["decrypted_elf"]["sha256"], "lift": "test"}, f)
    if pkg:
        os.makedirs(os.path.join(root, "data", "updates"))
        dst = os.path.join(root, "data", "updates", gv["update"]["file"])
        try:
            os.link(pkg, dst)                   # a second name for the same file: nothing of it copied
        except OSError:
            os.symlink(pkg, dst)
    if keys_dir:
        shutil.copytree(keys_dir, os.path.join(root, "data", "keys"))


def main():
    gv = json.load(open(os.path.join(ROOT, "config", "game_version.json"), encoding="utf-8"))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--launcher", required=True)
    ap.add_argument("--pkg", default=os.path.join(ROOT, "build", "v101", "pkg", gv["update"]["file"]))
    ap.add_argument("--keys-dir", default=os.path.join(ROOT, "build", "keys", "downloaded"))
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
        if name in ("ready", "not-now") and not have_inputs:
            print("SKIP: ready needs the official package and downloaded keys (local only: --pkg, --keys-dir)")
            continue
        if (name == "no-support") == SUPPORTED:
            print(f"SKIP: {name} -- this checkout " + ("has" if SUPPORTED else "hasn't") + " the v1.01 program's "
                  "configuration")
            continue
        for size in [s.strip() for s in a.sizes.split(",") if s.strip()]:
            d = os.path.join(work, name, size)
            shutil.rmtree(d, ignore_errors=True)
            root = os.path.join(d, "Cubic.Heroes")
            full = name in ("ready", "not-now")
            make_install(root, a.pkg if full else None, a.keys_dir if full else None)
            sp = os.path.join(d, f"{name}.nav")
            with open(sp, "w", encoding="utf-8", newline="\n") as f:
                f.write(f"# {name} (tests/launcher_game_version.py)\n" + script)
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
