#!/usr/bin/env python3
"""M11.2e: the public command line (docs/CLI.md) and the portable installation root on disposable installations.

    tests/cli_integration.py --launcher DIR --from INSTALL [--work DIR] [--with-game] [--keep]

DIR: a built launcher (tools/build_launcher.py --out DIR: CubicHeroes.exe, SDL2.dll). Its copies go into work folders
inside this checkout (the launcher then finds the checkout's worker, tools/worker.py, as a development launcher does),
in folders whose names hold spaces and non-ASCII letters. INSTALL: an installation (slots) copied as the disposable
one -- only read. --with-game adds the cases that start the game (--play: one at a time, offscreen through the
desktop guard; never beside another game run). Every case prints PASS or FAIL with what differed; the exit status is
1 if any failed.

Cases: the version and help; the default root (the launcher's folder) from another working folder; CUBIC_INSTALL_DIR;
cubicheroes-root.txt (relative: from the launcher's folder); a relative --install-dir (from the working folder);
--remember; a root that can't be used (exit code 7); nothing installed (--play: 3; --install without a disc: 2); the
installation in the launcher's folder (portable); settings through --set (kept) and a value the schema refuses; the
folder moved; --migrate-from into a second root (the source unchanged, file by file) and refused into a root that
holds one; two installations side by side. With the game: --play's session options (not kept), its exit status passed
on, the game ended with CubicHeroes (its job), and --update's up-to-date marker on an installation with no downloads/.
"""
import argparse
import ctypes
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import desktop_guard  # noqa: E402
import install_layout  # noqa: E402

NAME = "Cubic Héroes ü テスト"   # spaces, an accent, Japanese
RESULTS = []


def case(name, bad):
    RESULTS.append((name, bad))
    print(f"{'PASS' if not bad else 'FAIL'}: {name}" + ("" if not bad else " -- " + "; ".join(bad)), flush=True)


def cli(exe, *args, cwd=None, env=None, timeout=600):
    e = dict(os.environ)
    for k in ("CUBIC_INSTALL_DIR", "CUBIC_DIRECT_PLAY", "CUBIC_LAUNCHER", "CUBIC_LAUNCHER_PID"):
        e.pop(k, None)
    e.update(env or {})
    desktop_guard.no_display(e)   # the command line never opens a window; on Linux it cannot reach a display
    r = subprocess.run([exe, *args], cwd=cwd or os.path.dirname(exe), env=e, capture_output=True, timeout=timeout)
    out = r.stdout.decode("utf-8", "replace")
    return r.returncode, out, r.stderr.decode("utf-8", "replace")


def status(exe, *args, **kw):
    code, out, err = cli(exe, "--status", "--json", *args, **kw)
    try:
        return code, json.loads(out.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return code, {"_raw": out + err}


def same(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def tree_hashes(root, skip=("logs",)):
    """every file under root (links not followed; the top items in skip left out) -> its SHA-256"""
    out = {}
    for d, dirs, files in os.walk(root):
        rel = os.path.relpath(d, root)
        if rel == "." :
            dirs[:] = [x for x in dirs if x not in skip and not os.path.islink(os.path.join(d, x))]
        dirs[:] = [x for x in dirs if not os.path.islink(os.path.join(d, x)) and
                   not (os.name == "nt" and _is_junction(os.path.join(d, x)))]
        for f in files:
            p = os.path.join(d, f)
            h = hashlib.sha256()
            with open(p, "rb") as fh:
                for blk in iter(lambda: fh.read(1 << 20), b""):
                    h.update(blk)
            out[os.path.relpath(p, root).replace("\\", "/")] = h.hexdigest()
    return out


def _is_junction(p):
    try:
        return bool(os.stat(p, follow_symlinks=False).st_file_attributes & 0x400)   # a reparse point
    except (OSError, AttributeError):
        return False


def copy_install(src, dst):
    """the installation's items copied (game/, a link, left out: the worker's repair-link remakes it). Never the
    launcher's own package (its program, its worker) nor its root mark: the copy is the installation, and the
    launcher under test stays the one given (before beta.4's qualification a packaged installation's own launcher
    replaced it here, so the game cases ran that one)"""
    os.makedirs(dst, exist_ok=True)
    own = {n.lower() for n in install_layout.launcher_owned()}
    for n in os.listdir(src):
        s = os.path.join(src, n)
        if n == "game" or n.lower() in own or os.path.islink(s) or _is_junction(s):
            continue
        if os.path.isdir(s):
            shutil.copytree(s, os.path.join(dst, n), symlinks=True, dirs_exist_ok=True)
        else:
            shutil.copy2(s, os.path.join(dst, n))


def cfg_value(root, key):
    try:
        for line in open(os.path.join(root, "config", "dotheroes.cfg"), encoding="utf-8"):
            if line.startswith(key + "="):
                return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return None


def no_game_cases(pkg_src, install_src, work):
    exe_name = "CubicHeroes.exe" if os.name == "nt" else "CubicHeroes"
    a = os.path.join(work, NAME)
    shutil.copytree(pkg_src, a)
    for n in os.listdir(a):   # only the launcher's program and its libraries: a fresh package
        if n not in (exe_name, "SDL2.dll", "lib", "licenses", "LICENSE.txt", "README.txt"):
            p = os.path.join(a, n)
            shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
    exe = os.path.join(a, exe_name)
    other = os.path.join(work, "another working folder")
    os.makedirs(other)

    code, out, err = cli(exe, "--version")
    bad = [] if code == 0 and out.startswith("CubicHeroes ") and "worker" in out else [f"--version: {code} {out!r}{err!r}"]
    # M11.2f: --resolution -- a size outside 320x180..7680x4320 or not a size is a wrong command line (2)
    for r in ("99x99", "1920", "8000x5000", "1920x1080x2"):
        code, out, err = cli(exe, "--play", "--resolution", r, cwd=other)
        if code != 2 or "--resolution takes" not in out + err:
            bad.append(f"--resolution {r}: {code} {out!r}{err!r}")
    code, out, err = cli(exe, "--resolution", "1920x1080", cwd=other)
    if code != 2 or "apply to --play" not in out + err:
        bad.append(f"--resolution without --play: {code} {out!r}{err!r}")
    if os.path.exists(os.path.join(a, "logs")):
        bad.append("--version wrote into the folder (logs/)")
    code, out, err = cli(exe, "--help")
    if code != 0 or "--play" not in out or "--install-dir" not in out:
        bad.append(f"--help: {code}")
    case("the version and the help", bad)

    code, st = status(exe, cwd=other)
    bad = []
    if code != 0 or not same(st.get("install_dir", ""), a) or st.get("installed") is not False:
        bad.append(f"status {code}: {st}")
    if st.get("install_dir_from") != "the launcher's folder":
        bad.append(f"from {st.get('install_dir_from')!r}")
    case("the default root is the launcher's folder, from another working folder", bad)

    envroot = os.path.join(work, "env root é")
    code, st = status(exe, env={"CUBIC_INSTALL_DIR": envroot}, cwd=other)
    bad = [] if code == 0 and same(st.get("install_dir", ""), envroot) and         st.get("install_dir_from") == "CUBIC_INSTALL_DIR" else [f"{code}: {st}"]
    if os.path.exists(envroot):
        bad.append("--status created the folder it was asked about")
    case("CUBIC_INSTALL_DIR (a folder not there: --status leaves it not there)", bad)

    ptr = os.path.join(a, "cubicheroes-root.txt")
    with open(ptr, "w", encoding="utf-8", newline="\n") as f:
        f.write("../pointed root ü\n")
    code, st = status(exe, cwd=other)
    os.remove(ptr)
    case("cubicheroes-root.txt (a relative path: from the launcher's folder)",
         [] if code == 0 and same(st.get("install_dir", ""), os.path.join(work, "pointed root ü")) and
         st.get("install_dir_from") == "cubicheroes-root.txt" else [f"{code}: {st}"])

    code, st = status(exe, "--install-dir", "relative root", cwd=other)
    case("a relative --install-dir (from the working folder)",
         [] if code == 0 and same(st.get("install_dir", ""), os.path.join(other, "relative root")) else [f"{code}: {st}"])

    blocker = os.path.join(work, "a file, not a folder")
    open(blocker, "w").close()
    bad_root = os.path.join(blocker, "sub")
    code, out, err = cli(exe, "--set", "DOTHEROES_FPS=60", "--json", "--install-dir", bad_root, cwd=other)
    bad = [] if code == 7 and "root_not_writable" in out else [f"--set: {code} {out!r}"]
    code, st = status(exe, "--install-dir", bad_root, cwd=other)
    if code != 0 or st.get("writable") is not False:
        bad.append(f"status: {code} {st}")
    case("a root that can't be used: exit code 7, reported by --status", bad)

    code, out, err = cli(exe, "--play", "--json", cwd=other)
    bad = [] if code == 3 else [f"--play: {code} {out!r}"]
    code, out, err = cli(exe, "--install", "--json", cwd=other)
    if code != 2:
        bad.append(f"--install without --disc: {code} {out!r}")
    case("nothing installed: --play 3, --install without a disc 2", bad)

    # the portable installation: the launcher's folder is the install's root
    copy_install(install_src, a)
    code, st = status(exe, cwd=other)
    game = st.get("game") or ""
    case("the installation in the launcher's folder", [] if code == 0 and st.get("installed") is True and
         same(os.path.commonpath([os.path.abspath(game), a]), a) else [f"{code}: {st}"])

    before = cfg_value(a, "DOTHEROES_FPS")
    code, out, err = cli(exe, "--set", "DOTHEROES_FPS=120", cwd=other)
    bad = [] if code == 0 and cfg_value(a, "DOTHEROES_FPS") == "120" else [f"--set: {code} {out!r}{err!r}"]
    code, out, err = cli(exe, "--set", "DOTHEROES_FPS=fast", "--json", cwd=other)
    if code == 0 or cfg_value(a, "DOTHEROES_FPS") != "120":
        bad.append(f"a refused value: {code} {out!r}")
    code, out, err = cli(exe, "--set", "DOTHEROES_FPS=" + (before or "60"), cwd=other)
    case("--set keeps a setting; a value the schema refuses changes nothing", bad)

    # moved: the launcher and the installation together
    moved = a + " moved"
    os.replace(a, moved)
    a, exe = moved, os.path.join(moved, exe_name)
    code, st = status(exe, cwd=other)
    game = st.get("game") or ""
    case("the folder moved: still installed, its game inside the new place",
         [] if code == 0 and st.get("installed") is True and os.path.isfile(game) and
         same(os.path.commonpath([os.path.abspath(game), a]), a) else [f"{code}: {st}"])

    # --migrate-from: a copy into a second root, the source unchanged
    b = os.path.join(work, "second root é")
    src0 = tree_hashes(a)
    code, out, err = cli(exe, "--migrate-from", a, "--install-dir", b, "--json", cwd=other, timeout=1800)
    src1 = tree_hashes(a)
    bad = [] if code == 0 else [f"--migrate-from: {code} {out[-400:]!r}{err[-200:]!r}"]
    changed = sorted(k for k in set(src0) | set(src1) if src0.get(k) != src1.get(k))
    if changed:
        bad.append(f"the source changed: {changed[:5]}")
    dst = tree_hashes(b)
    for rel in ("active.json", "config/dotheroes.cfg"):
        if src0.get(rel) != dst.get(rel):
            bad.append(f"{rel} differs in the copy")
    saves = [k for k in src0 if k.startswith(("data/gamedata/", "data/devmode/", "backups/"))]
    miss = [k for k in saves if src0[k] != dst.get(k)]
    if miss:
        bad.append(f"{len(miss)} of {len(saves)} saves' files differ: {miss[:3]}")
    code, st = status(exe, "--install-dir", b, cwd=other)
    if code != 0 or st.get("installed") is not True:
        bad.append(f"status of the copy: {code} {st}")
    case(f"--migrate-from into a second root ({len(saves)} save files checked; the source unchanged)", bad)

    b0 = tree_hashes(b)
    code, out, err = cli(exe, "--migrate-from", a, "--install-dir", b, "--json", cwd=other)
    b1 = tree_hashes(b)
    case("--migrate-from refused into a root that holds an installation",
         [] if code != 0 and "install_exists" in out and b0 == b1 else [f"{code} {out[-300:]!r}; unchanged {b0 == b1}"])

    code, out, err = cli(exe, "--set", "DOTHEROES_FPS=144", "--install-dir", b, cwd=other)
    case("two installations side by side: a setting of one leaves the other",
         [] if code == 0 and cfg_value(b, "DOTHEROES_FPS") == "144" and cfg_value(a, "DOTHEROES_FPS") == (before or "60")
         else [f"{code}: {cfg_value(a, 'DOTHEROES_FPS')} / {cfg_value(b, 'DOTHEROES_FPS')}"])

    code1, st1 = status(exe, "--install-dir", b, "--remember", cwd=other)
    code2, st2 = status(exe, cwd=other)
    code3, st3 = status(exe, "--install-dir", a, "--remember", cwd=other)
    bad = []
    if not (code2 == 0 and same(st2.get("install_dir", ""), b) and st2.get("install_dir_from") == "cubicheroes-root.txt"):
        bad.append(f"remembered: {code1}/{code2} {st2}")
    if os.path.exists(os.path.join(a, "cubicheroes-root.txt")):
        bad.append("the pointer stays after --remember of the launcher's own folder")
    case("--remember keeps a root; the default removes the pointer", bad)
    return exe, a, b


def game_cases(exe, a):
    """--play on the portable installation: offscreen (the desktop guard), the exit question answered through the
    in-game menu's test input"""
    other = os.path.join(os.path.dirname(a), "another working folder")
    env = dict(os.environ)
    desktop_guard.game_env(env, False)
    env.update({"DOTHEROES_OVERLAY_TEST": "1", "ALSOFT_DRIVERS": "null"})
    for k in ("CUBIC_INSTALL_DIR", "CUBIC_DIRECT_PLAY", "CUBIC_LAUNCHER", "CUBIC_LAUNCHER_PID"):
        env.pop(k, None)
    log = os.path.join(a, "logs", "game.log")
    cfg0 = cfg_value(a, "DOTHEROES_FPS")
    res0 = cfg_value(a, "DOTHEROES_RENDER_RES")
    wj = os.path.join(a, "config", "window.json")
    wj0 = open(wj, "rb").read() if os.path.exists(wj) else None

    def play(*args, answer=True, wait_title=40):
        if os.path.exists(log):
            os.remove(log)
        p = subprocess.Popen([exe, "--play", "--json", *args], cwd=other, env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
        desktop_guard.Watch(p, label="CubicHeroes --play").start()   # the tree: the game it starts included
        first = p.stdout.readline().decode("utf-8", "replace")
        try:
            ev = json.loads(first)
        except ValueError:
            ev = {"_raw": first}
        if answer:
            t0 = time.time()
            while time.time() - t0 < wait_title and p.poll() is None:
                time.sleep(1)
            with open(os.path.join(a, "overlay.input"), "w") as f:
                f.write("exit wait wait wait wait right wait wait a\n")
        return p, ev

    p, ev = play("--fps", "120", "--width", "1024", "--height", "600", "--windowed", "--resolution", "2560x1440")
    try:
        rest, err = p.communicate(timeout=120)
        code = p.returncode
    except subprocess.TimeoutExpired:
        p.kill()
        rest, err = p.communicate()
        code = None
    text = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
    bad = []
    if ev.get("ev") != "started":
        bad.append(f"no started event: {ev}")
    if code != 0:
        bad.append(f"exit {code}")
    if "frame limit 120.00 fps" not in text:
        bad.append("the session's frame limit (120) not in effect")
    if "rendering resolution 2560x1440" not in text:
        bad.append("the session's rendering resolution (2560x1440) not in effect")
    if cfg_value(a, "DOTHEROES_RENDER_RES") != res0:
        bad.append(f"the session's rendering resolution was kept ({cfg_value(a, 'DOTHEROES_RENDER_RES')})")
    if "[exit] the game's end (status 0)" not in text:
        bad.append("the game did not end itself")
    if '"ev":"ended","exit":0' not in rest.decode("utf-8", "replace"):
        bad.append("no ended event")
    if cfg_value(a, "DOTHEROES_FPS") != cfg0:
        bad.append(f"the session's frame limit was kept ({cfg_value(a, 'DOTHEROES_FPS')})")
    wj1 = open(wj, "rb").read() if os.path.exists(wj) else None
    if wj1 != wj0:
        bad.append("the session's window size was kept in config/window.json")
    case("--play: session options in effect, not kept; the game's own end passed on (0)", bad)

    # the game's status passed on: Windows sets 42 from outside (TerminateProcess); Linux cannot set a status from
    # outside -- the game killed (SIGKILL) ends with the signal, which CubicHeroes passes on as 128 + 9 (M11.4)
    k32 = ctypes.windll.kernel32 if os.name == "nt" else None
    want = 42 if os.name == "nt" else 128 + 9
    p, ev = play(answer=False)
    time.sleep(25)
    pid = ev.get("pid")
    bad = []
    if k32 and pid:
        h = k32.OpenProcess(0x0001, False, pid)   # PROCESS_TERMINATE
        k32.TerminateProcess(h, 42)
        k32.CloseHandle(h)
    elif pid:
        os.kill(pid, 9)
    try:
        p.wait(60)
    except subprocess.TimeoutExpired:
        p.kill()
        bad.append("CubicHeroes did not end with the game")
    if p.returncode != want:
        bad.append(f"exit {p.returncode}, not the game's {want}")
    case(f"--play passes on the game's exit status ({want})", bad)

    def alive(pid):
        if os.name == "nt":
            return str(pid) in subprocess.run(["tasklist", "/fi", f"pid eq {pid}", "/nh"], capture_output=True,
                                              text=True).stdout
        try:   # a zombie (ended, not yet reaped) is not running
            with open(f"/proc/{pid}/stat") as f:
                return f.read().rsplit(")", 1)[1].split()[0] != "Z"
        except OSError:
            return False

    p, ev = play(answer=False)
    time.sleep(25)
    pid = ev.get("pid")
    running = p.poll() is None and bool(pid) and alive(pid)
    p.kill()   # CubicHeroes ended from outside (as a launcher's "stop")
    p.wait()
    gone = False
    for _ in range(40):
        if not alive(pid):
            gone = True
            break
        time.sleep(0.5)
    case("the game ends with CubicHeroes (its job; Linux: PR_SET_PDEATHSIG)",
         ([] if running else [f"the game was not running when CubicHeroes was ended ({ev})"]) +
         ([] if gone else [f"game {pid} still running 20 s later"]))

    # beta.4: --update on an installation with no downloads/ (a beta.2 or beta.3 one upgraded by hand, which never
    # downloaded an update) records the game as up to date -- downloads/game-up-to-date-with.txt, which the window
    # reads at its start. Before the fix the marker's folder was never made: every start updated the game again
    dl = os.path.join(a, "downloads")
    if os.path.isdir(dl):
        shutil.rmtree(dl)
    code, out, err = cli(exe, "--version", "--json")
    try:
        version = json.loads(out.strip().splitlines()[-1])["cubicheroes"]
    except (ValueError, IndexError, KeyError):
        version = None
    off = {"HTTP_PROXY": "http://127.0.0.1:9", "HTTPS_PROXY": "http://127.0.0.1:9", "NO_PROXY": ""}
    code, out, err = cli(exe, "--update", "--json", cwd=other, env=off, timeout=3600)
    marker = os.path.join(dl, "game-up-to-date-with.txt")
    got = open(marker, encoding="utf-8").read().strip() if os.path.exists(marker) else None
    bad = [] if version and code == 0 and got == version else \
        [f"--update: exit {code}; the marker {got!r}, not {version!r}; {(out + err)[-300:]!r}"]
    case("--update without downloads/: the game's up-to-date marker written (the window won't update it again)", bad)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--launcher", required=True)
    ap.add_argument("--from", dest="src", required=True)
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "m11", "cli-it"))
    ap.add_argument("--with-game", action="store_true")
    ap.add_argument("--keep", action="store_true", help="leave the work folder")
    o = ap.parse_args()
    work = os.path.abspath(o.work)
    if os.path.commonpath([work, ROOT]) != ROOT:
        sys.exit("--work must be inside this checkout (the launcher finds its worker there)")
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    try:
        exe, a, b = no_game_cases(os.path.abspath(o.launcher), os.path.abspath(o.src), work)
        if o.with_game:
            game_cases(exe, a)
    finally:
        if not o.keep:
            shutil.rmtree(work, ignore_errors=True)
    failed = [n for n, bad in RESULTS if bad]
    print(f"{len(RESULTS) - len(failed)} of {len(RESULTS)} passed" + (f"; failed: {', '.join(failed)}" if failed else ""))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
