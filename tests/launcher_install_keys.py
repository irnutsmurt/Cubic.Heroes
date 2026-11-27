"""The launcher's first install from the disc and its keys (issue #13), hidden (--nav-test): Install / Manage on a fresh
installation folder, the disc already chosen, the keys looked for by themselves -- Windows, Linux and SteamOS alike.

A greyed-out Install must never stand unexplained: whenever it can't start, "Install can't start yet" is shown above it
with each reason and what to do. Scenarios (each in a fresh folder under --work, the disc image a hard link -- nothing
of the game copied):

  nothing-beside   the disc alone, offline: Install blocked, explained (no keys found; your own under Advanced)
  eboot-chosen     the disc's encrypted program chosen as "the keys" (issue #13's likeliest case): blocked, explained
  binary-chosen    any other binary file chosen: blocked, explained
  keys-beside      a keys file beside the disc: found by itself; Install available (the focus reaches it)
  keys-utf16       the same keys file saved as UTF-16 text: found and read; Install available
  zip-beside       that keys file inside a ZIP (keys.zip) beside the disc: found, read in memory; Install available
  archive-beside   (with --key-zip) your own key archive -- e.g. a ZIP of a folder of binary key files -- beside the
                   disc under its own name: found and read; Install available

Needs the disc image and a keys file of your own (local only, never in the repository): --iso and --keys, by default
the repository's "3D DOT GAME HEROES [BLUS-30490].iso" and build/keys/self_keys.txt; skipped (exit 0) without them.

  py -3 tests/launcher_install_keys.py --launcher <the launcher to test> [--iso ISO] [--keys KEYS] [--key-zip ZIP]
      [--only NAME ...] [--sizes 1280x800,1280x720]
"""
import argparse
import io
import os
import shutil
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import launcher_nav_pages as nav  # noqa: E402

# (0.2.0-beta.6) the checks waited for as they end (settle), not a count of frames: Linux's hidden launcher draws one
# in ~1 ms. Blocked: the explanation above the greyed Install, and Advanced -- open by itself under it, with "Look for
# the keys again" -- where the player's own keys are chosen
BLOCKED = ("until 200000 Install the game\nexpect Install the game\na\nsettle 50000000\n"
           "seek 600 down Look for the keys again\nexpect Look for the keys again\n"
           "shot install\nsee Install can't start yet\nsee Advanced: your own program keys\nquit\n")
READY = ("until 200000 Install the game\nexpect Install the game\na\nsettle 50000000\n"
         "seek 3000 down Install\nexpect Install\nshot install\nquit\n")


def scenarios(keys, key_zip=None):
    text = open(keys, encoding="utf-8").read()
    z = io.BytesIO()
    with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as f:
        f.writestr("keys.txt", text)
    out = {
        "nothing-beside": ({}, None, BLOCKED),
        "eboot-chosen": ({}, ("EBOOT.BIN", b"SCE\0" + os.urandom(4092)), BLOCKED),
        "binary-chosen": ({}, ("eid_root_key", os.urandom(48)), BLOCKED),
        "keys-beside": ({"keys.txt": text.encode("utf-8")}, None, READY),
        "keys-utf16": ({"keys.txt": text.encode("utf-16")}, None, READY),
        "zip-beside": ({"keys.zip": z.getvalue()}, None, READY),
    }
    if key_zip:
        with open(key_zip, "rb") as f:
            out["archive-beside"] = ({os.path.basename(key_zip): f.read()}, None, READY)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--launcher", required=True, help="the launcher to test (a build)")
    ap.add_argument("--iso", default=os.path.join(ROOT, "3D DOT GAME HEROES [BLUS-30490].iso"))
    ap.add_argument("--keys", default=os.path.join(ROOT, "build", "keys", "self_keys.txt"))
    ap.add_argument("--key-zip", help="a key archive of your own (local only) for archive-beside")
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "qual", "launcher-install-keys"))
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--sizes", default="1280x800,1280x720")
    a = ap.parse_args()
    if not (os.path.isfile(a.iso) and os.path.isfile(a.keys)):
        print("SKIP: the disc image and a keys file are local only (--iso, --keys)")
        return 0
    launcher = os.path.abspath(a.launcher)
    work = os.path.abspath(a.work)
    ok = True
    for name, (beside, chosen, script) in scenarios(a.keys, a.key_zip).items():
        if a.only and name not in a.only:
            continue
        for size in [s.strip() for s in a.sizes.split(",") if s.strip()]:
            d = os.path.join(work, name, size)
            shutil.rmtree(d, ignore_errors=True)
            games = os.path.join(d, "games")
            os.makedirs(games)
            iso = os.path.join(games, "game.iso")
            try:
                os.link(a.iso, iso)           # a second name for the same file
            except OSError:
                os.symlink(a.iso, iso)
            for n, data in beside.items():
                with open(os.path.join(games, n), "wb") as f:
                    f.write(data)
            keys = ""
            if chosen:
                keys = os.path.join(d, "picked", chosen[0])
                os.makedirs(os.path.dirname(keys))
                with open(keys, "wb") as f:
                    f.write(chosen[1])
            root = os.path.join(d, "Cubic.Heroes")
            os.makedirs(os.path.join(root, "config"))
            import json
            with open(os.path.join(root, "config", "launcher.json"), "w", encoding="utf-8") as f:
                json.dump({"disc": iso, "keys": keys, "rpcs3": ""}, f)
            sp = os.path.join(d, f"{name}.nav")
            with open(sp, "w", encoding="utf-8", newline="\n") as f:
                f.write(f"# {name} (tests/launcher_install_keys.py)\n" + script)
            ok = nav.run(sp, launcher, root, os.path.join(d, "work"), a.timeout, size) and ok
            os.remove(iso)                    # the link only: the disc image itself untouched
    print("launcher install keys:", "PASS" if ok else "FAIL", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
