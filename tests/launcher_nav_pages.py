"""The launcher's pages by controller (--nav-test), hidden: the scripts in tests/nav against an installation with the game.

  five_pages.nav     Play, Install / Manage, Saves, Settings, Diagnostics: each page's controls reached by the D-pad,
                     LB/RB through all five both ways, the side menu's entries
  settings_tabs.nav  the Settings tabs (Graphics, Performance, Audio, Controller, Launcher): focus starts on the chosen
                     tab, each tab chosen with A shows its own controls down to Restore the defaults, the next tab's
                     list starts from its top, and the tab is still chosen after another page

A script passes when every step ran, no expectation failed (the launcher stops the script at the first one) and it
reached its quit. The scripts press nothing that changes a setting or the installation (tabs and pages only): the
runner checks that no file of the installation changed, its logs aside. Every window stays hidden
(tests/desktop_guard.py); the update check runs offline.

Every script runs at each of --sizes, the test window's size (--nav-size): by default 1280x800 (the Steam Deck's) and
1280x720 -- the sizes at which a result card first fell below the D-pad's reach (0.2.0-beta.5).

  py -3 tests/launcher_nav_pages.py --install <installation> [--launcher <the launcher to test>] [--only settings_tabs]
      [--sizes 1280x800,1280x720]

The launcher tested is the installation's own unless --launcher names another (a build). The scripts' logs and their
pictures (shot steps, BMP) go to --work.
"""
import argparse
import glob
import hashlib
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import desktop_guard  # noqa: E402

WIN = os.name == "nt"
EXE = "CubicHeroes.exe" if WIN else "CubicHeroes"
NAV = os.path.join(ROOT, "tests", "nav")
HELPERS = ("CUBIC_INSTALL_DIR", "RECOMPLAUNCHER", "CUBIC_LAUNCHER", "CUBIC_DIRECT_PLAY")
SKIP_DIRS = {"logs"}   # what the launcher writes for itself on every start
SMALL = 1 << 20   # compared by contents up to this size (settings, records)


def snapshot(root):
    """every file of the installation: a small file's contents (a hash), a large one's size and time (the game's
    data included: a few thousand entries)"""
    out = {}
    for d, dirs, files in os.walk(root):
        rel = os.path.relpath(d, root)
        if rel == ".":
            dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
        for n in files:
            p = os.path.join(d, n)
            try:
                st = os.lstat(p)
            except OSError:
                continue
            if st.st_size <= SMALL and os.path.isfile(p):
                with open(p, "rb") as f:
                    out[os.path.relpath(p, root)] = hashlib.sha256(f.read()).hexdigest()
            else:
                out[os.path.relpath(p, root)] = (st.st_size, st.st_mtime_ns)
    return out


def run(script, launcher, install, work, timeout, size):
    name = os.path.splitext(os.path.basename(script))[0] + " at " + size
    d = os.path.join(work, size, os.path.splitext(os.path.basename(script))[0])
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    path = os.path.join(d, os.path.basename(script))
    shutil.copyfile(script, path)
    env = {k: v for k, v in os.environ.items() if k not in HELPERS}
    desktop_guard.launcher_env(env)   # hidden (offscreen on Linux)
    env["HTTP_PROXY"] = env["HTTPS_PROXY"] = "http://127.0.0.1:9"   # offline: the update check fails as without a network
    env.pop("NO_PROXY", None)
    p = subprocess.Popen([launcher, "--install-dir", install, "--nav-test", path, "--nav-size", size], cwd=d, env=env,
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    watch = desktop_guard.Watch(p, "the launcher (nav-test)").start() if hasattr(desktop_guard, "Watch") else None
    try:
        rc = p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        rc = "timeout"
    if watch:
        watch.stop()
    with open(script, encoding="utf-8") as f:
        steps = [ln.split()[0] for ln in f if ln.strip() and not ln.lstrip().startswith("#")]
    log = ""
    if os.path.exists(path + ".log"):
        with open(path + ".log", encoding="utf-8", errors="replace") as f:
            log = f.read()
    lines = log.splitlines()
    ran = [ln.split()[0] for ln in lines if " page=" in ln]   # a step done: logged with the page it left
    problems = []
    if watch and getattr(watch, "violation", None):
        problems.append("a window was shown: " + str(watch.violation))
    if rc == "timeout":
        problems.append(f"no end within {timeout} s")
    problems += [ln for ln in lines if " page=" not in ln and ln.strip()]   # FAILED: ..., a picture not taken
    if not problems and (ran != steps or rc not in (0, None)):
        problems.append(f"{len(ran)} of {len(steps)} steps ran, exit {rc}")
    expects = sum(1 for s in steps if s in ("expect", "see"))
    for ln in (problems or [f"{expects} expectations met, {len(steps)} steps, exit {rc}"]):
        print(("FAIL" if problems else "PASS") + f": {name}: {ln}", flush=True)
    return not problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--install", required=True, help="an installation with the game (used as it is, never changed)")
    ap.add_argument("--launcher", help="the launcher to test (default: the installation's own)")
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "qual", "launcher-nav"))
    ap.add_argument("--only", nargs="*", help="script names (without .nav)")
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--sizes", default="1280x800,1280x720", help="the test window's sizes, each WxH (960x600 at least)")
    a = ap.parse_args()
    sizes = [s.strip() for s in a.sizes.split(",") if s.strip()]
    for s in sizes:
        w, _, h = s.partition("x")
        if not (w.isdigit() and h.isdigit() and int(w) >= 960 and int(h) >= 600):
            sys.exit(f"--sizes: {s!r} is not WxH of 960x600 or more")
    install = os.path.abspath(a.install)
    launcher = os.path.abspath(a.launcher or os.path.join(install, EXE))
    if not os.path.isfile(launcher):
        sys.exit(f"no launcher at {launcher}")
    scripts = sorted(glob.glob(os.path.join(NAV, "*.nav")))
    if a.only:
        scripts = [s for s in scripts if os.path.splitext(os.path.basename(s))[0] in a.only]
    if not scripts:
        sys.exit("no script to run")
    work = os.path.abspath(a.work)
    os.makedirs(work, exist_ok=True)
    before = snapshot(install)
    ok = all([run(s, launcher, install, work, a.timeout, size) for size in sizes for s in scripts])
    after = snapshot(install)
    changed = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k))
    if changed:
        ok = False
        print(f"FAIL: the installation changed ({len(changed)} files): " + ", ".join(changed[:10]), flush=True)
    else:
        print(f"PASS: the installation unchanged ({len(after)} files)", flush=True)
    print("launcher nav:", "PASS" if ok else "FAIL", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
