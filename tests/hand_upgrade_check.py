#!/usr/bin/env python3
"""A12 (docs/RELEASE_QUALIFICATION.md): the release notes' hand upgrade, on an installation of a published beta whose
launcher can't install updates itself, then the new launcher started plainly. Linux (the tarball and the AppImage).

    python3 tests/hand_upgrade_check.py --work DIR --kind tar|appimage --to PACKAGE

DIR: tests/release_qualification.py's work folder after `--only unpack empty install status shaders` of the
published package (its folder `<DIR>/<NAME>`: the tarball's root, or the folder holding the AppImage and its
Cubic.Heroes). PACKAGE: the candidate's tarball or AppImage. Steps, as the notes say:
  tarball   the installation's folder named Cubic.Heroes inside a parent (as a player's); the candidate's tarball
            extracted into that parent (`tar -xJf ... -C <parent>`), over the folder
  AppImage  the candidate's AppImage put beside the old one, made executable
then the new launcher started plainly (its window, hidden by the desktop guard): it brings the game up to date by
itself AND records it (downloads/game-up-to-date-with.txt says the launcher's version -- also where the installation
had no downloads/); started again, it doesn't update the game again; saves, settings and builds unchanged byte for
byte; --status: installed. Play is left to release_qualification (--only status play) on the same folder.
Exit 1 if any step fails.
"""
import argparse
import hashlib
import json
import os
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import desktop_guard  # noqa: E402
import updates  # noqa: E402

NAME = "Cubic Héroes RC ü テスト"   # release_qualification's folder name
results = []


def step(name, bad):
    results.append((name, bad))
    print(("PASS" if not bad else "FAIL") + f": {name}" + (f" -- {'; '.join(bad)}" if bad else ""), flush=True)


def sha(p):
    with open(p, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def snap(root):
    out = {}
    for part in ("data/gamedata", "config", "builds"):
        for d, _, fs in os.walk(os.path.join(root, part)):
            for f in fs:
                p = os.path.join(d, f)
                if not os.path.islink(p):
                    out[os.path.relpath(p, root)] = sha(p)
    return out


def lines(p):
    try:
        with open(p, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


def end(proc, root, appimage=None):
    """the launcher started here, and whatever it started from the installation, ended"""
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except OSError:
        pass
    try:
        proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait()
    for pid, path in updates._processes():
        if path.startswith(root + os.sep) or (appimage and path == os.path.realpath(appimage)):
            try:
                os.kill(pid, signal.SIGTERM)
            except OSError:
                pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", required=True)
    ap.add_argument("--kind", choices=("tar", "appimage"), required=True)
    ap.add_argument("--to", required=True)
    ap.add_argument("--minutes", type=float, default=20, help="how long the first start may take to update the game")
    ap.add_argument("--saves", default=os.path.join(ROOT, "build", "checkpoints", "castle-front"),
                    help="game-written save folders (BLUS30490*) put into the installation first: a fresh install has "
                         "none, and the upgrade must keep a player's saves")
    a = ap.parse_args()
    work, pkg = os.path.abspath(a.work), os.path.abspath(a.to)
    folder = os.path.join(work, NAME)
    appimage = None
    if a.kind == "tar":
        parent = os.path.join(work, "Games é テスト")
        os.makedirs(parent, exist_ok=True)
        root = os.path.join(parent, "Cubic.Heroes")
        if os.path.isdir(folder) and not os.path.exists(root):
            os.rename(folder, root)
    else:
        root = os.path.join(folder, "Cubic.Heroes")
    dl = os.path.join(root, "downloads")
    step("the published beta's installation has no downloads/ (as one that never downloaded an update)",
         [] if not os.path.exists(dl) else [f"{dl} exists"])
    sd = os.path.join(root, "data", "gamedata", "dev_hdd0", "home", "00000001", "savedata")
    if a.saves and os.path.isdir(a.saves):
        os.makedirs(sd, exist_ok=True)
        for n in os.listdir(a.saves):
            if n.startswith("BLUS30490") and os.path.isdir(os.path.join(a.saves, n)):
                shutil.copytree(os.path.join(a.saves, n), os.path.join(sd, n), dirs_exist_ok=True)
    before = snap(root)
    step(f"saves in the installation before the upgrade ({sum(1 for k in before if k.startswith('data/gamedata'))} files)",
         [] if any(k.startswith("data/gamedata") for k in before) else ["none"])
    # the notes' step
    if a.kind == "tar":
        r = subprocess.run(["tar", "-xJf", pkg, "-C", parent], capture_output=True, text=True)
        step("the candidate's tarball extracted over the folder's parent", [] if r.returncode == 0 else [r.stderr[-300:]])
        cmd = [os.path.join(root, "CubicHeroes")]
    else:
        appimage = os.path.join(folder, os.path.basename(pkg))
        shutil.copy2(pkg, appimage)
        os.chmod(appimage, 0o755)
        step("the candidate's AppImage beside the old one", [])
        cmd = [appimage]
    env = dict(os.environ)
    for k in ("APPIMAGE", "APPDIR", "CUBIC_UPDATE_SOURCE", "SSL_CERT_FILE", "SSL_CERT_DIR"):
        env.pop(k, None)
    desktop_guard.game_env(env, False, quiet=True)
    desktop_guard.launcher_env(env)   # the window hidden (offscreen on Linux)
    r = subprocess.run(cmd + ["--version", "--json"], capture_output=True, text=True, env=env, timeout=300)
    try:
        version = json.loads(r.stdout.strip().splitlines()[-1])["cubicheroes"]
    except (ValueError, IndexError, KeyError):
        version = None
    upd_log = os.path.join(root, "logs", "worker-update.jsonl")
    n0 = len(lines(upd_log))
    marker = os.path.join(dl, "game-up-to-date-with.txt")
    # 1. started plainly: the game brought up to date, and recorded
    p = subprocess.Popen(cmd, cwd=work, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, start_new_session=True)
    deadline = time.time() + a.minutes * 60
    got = None
    while time.time() < deadline:
        if os.path.exists(marker):
            with open(marker, encoding="utf-8") as f:
                got = f.read().strip()
            break
        time.sleep(2)
    time.sleep(3)
    new = lines(upd_log)[n0:]
    res = [json.loads(x) for x in new if '"result"' in x]
    end(p, root, appimage)
    bad = []
    if not res or not res[-1].get("ok"):
        bad.append(f"the worker's update: {res[-1] if res else 'never ran'}")
    if got != version or not version:
        bad.append(f"the marker {got!r}, not the launcher's version {version!r}")
    step(f"the new launcher ({version}) started plainly: the game brought up to date and recorded", bad)
    # 2. started again: no update of the game this time
    n1 = len(lines(upd_log))
    p = subprocess.Popen(cmd, cwd=work, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, start_new_session=True)
    time.sleep(60)
    end(p, root, appimage)
    again = [x for x in lines(upd_log)[n1:] if '"hello"' in x]
    step("started again: the game not updated again", [] if not again else [f"{len(again)} update run(s)"])
    after = snap(root)
    changed = sorted(k for k in before if before[k] != after.get(k))
    added = sorted(k for k in after if k not in before)
    # the window's first start writes its own preferences (config/launcher.json); nothing else may appear
    bad = ([f"changed/missing: {changed[:8]}"] if changed else []) + \
        ([f"new: {[k for k in added if k != os.path.join('config', 'launcher.json')][:8]}"]
         if [k for k in added if k != os.path.join("config", "launcher.json")] else [])
    step("saves, settings and builds byte for byte (the window's launcher.json the only new file)", bad)
    r = subprocess.run(cmd + ["--status", "--json"], cwd=work, capture_output=True, text=True, env=env, timeout=300)
    try:
        st = json.loads(r.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        st = {}
    step("--status: installed", [] if st.get("installed") else [r.stdout[-300:]])
    if a.kind == "tar":   # back where release_qualification's steps find it
        os.rename(root, folder)
    failed = [n for n, b in results if b]
    print(f"{len(results) - len(failed)} of {len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
