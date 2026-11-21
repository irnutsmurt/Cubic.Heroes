#!/usr/bin/env python3
"""The launcher's own update as a player does it, through the launcher's window (0.2.0-beta.4): the scripted
controller (--nav-test) opens Updates, checks, downloads, presses Restart and update and confirms; the launcher
closes, its helper installs the new version and starts it; the new version's window comes up. Linux (the tarball and
the AppImage) and Windows (the zip).

    python3 tests/launcher_gui_update_check.py --from PKG_A --to PKG_B --key PEM --install ROOT [--work DIR]

PKG_A and PKG_B: two packages of the same kind (tools/make_bundle.py / make_appimage.py), B newer than A. PEM: the
Ed25519 key B's manifest is signed with, which A's worker/config/release_keys.json must already trust (local test
packages only, made with a test key; the project's key is never involved). ROOT: an installation with the game
installed (e.g. tests/launcher_update_e2e.py's), copied for the run -- never changed. B is served by a local HTTPS
mirror (a throwaway CA trusted through SSL_CERT_FILE; CUBIC_UPDATE_SOURCE points the launcher at it). Cases:
  update    through the window: B downloaded, checked, installed on restart; B's window up (its health record);
            --version says B; the launcher's files are B's byte for byte; the installation's saves, settings and
            builds untouched; A kept for rollback
  back      through B's window: Go back to version A -- A's window up, A's files byte for byte
  failed    B whose window never comes up (its program replaced by one that exits): the update undone, A's files
            byte for byte, A started again (--update-failed), its Updates page saying the update wasn't installed
  crash     the helper killed in the middle of its swap: the next start of the launcher finishes or undoes it
            (recover-launcher) -- a launcher that answers --version afterwards, A or B whole, never a mixture
Every window is hidden (the desktop guard: offscreen on Linux); each run ends with no launcher of this test left.
Exit 1 if any case fails. The scripts' logs (FILE.log, the launcher's own) are kept in WORK for the record.
"""
import argparse
import base64
import hashlib
import json
import os
import shutil
import signal
import subprocess
import sys
import tarfile
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import desktop_guard  # noqa: E402
import release_trust as trust  # noqa: E402
import updates  # noqa: E402
from linux_update_check import Mirror, Served, Tls, openssl  # noqa: E402

WIN = os.name == "nt"
EXE = "CubicHeroes.exe" if WIN else "CubicHeroes"
HELPERS = ("SSL_CERT_FILE", "SSL_CERT_DIR", "CUBIC_UPDATE_SOURCE", "http_proxy", "https_proxy", "HTTP_PROXY",
           "HTTPS_PROXY", "all_proxy", "ALL_PROXY", "APPIMAGE", "APPDIR", "CUBIC_INSTALL_DIR")
INSTALL_ITEMS = ("data/gamedata", "config", "builds")
results = []

# the player's way through the window: Updates (two pages right of Play), check, download, install, confirm (the
# question's safe choice is focused first: left to its Restart and update). `until N TEXT` waits (at most N frames)
# for the focus on TEXT with no operation on screen: a hidden window's frame rate differs between systems (about
# 2000 frames a second offscreen on Linux, 33 on Windows). After the confirmation the launcher quits by itself to
# install; only a refusal's message (OK) keeps it, and the script then ends it
UPDATE_SCRIPT = """\
until 200000 PLAY
rb
rb
until 200000 Check for updates
expect Check for updates
a
wait 60
until 400000 Check for updates
down
expect Download it
a
wait 60
until 800000 OK
expect OK
a
wait 60
until 100000 Download it
down
expect Restart and update
a
until 20000 Cancel
left
expect Restart and update
a
until 400000 OK
quit
"""
BACK_SCRIPT = """\
until 200000 PLAY
rb
rb
until 400000 Check for updates
expect Check for updates
down
down
expect Go back to version
a
until 20000 Cancel
left
expect Go back
a
until 400000 OK
quit
"""
LOOK_SCRIPT = """\
until 200000 PLAY
rb
rb
until 400000 Check for updates
shot updates
quit
"""


def step(name, bad, **info):
    results.append({"case": name, "ok": not bad, "bad": bad, **info})
    print(("PASS" if not bad else "FAIL") + f": {name}" + (f" -- {'; '.join(bad)}" if bad else ""), flush=True)


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def tree(folder, names):
    out = {}
    for n in names:
        p = os.path.join(folder, n)
        if os.path.isfile(p) and not os.path.islink(p):
            out[n] = sha(p)
        for d, dirs, fs in os.walk(p) if os.path.isdir(p) and not os.path.islink(p) else ():
            for f in fs:
                q = os.path.join(d, f)
                out[os.path.relpath(q, folder)] = ("link", os.readlink(q)) if os.path.islink(q) else sha(q)
    return out


def version_of(pkg):
    import re
    m = re.match(r"Cubic\.Heroes-(.+?)-(?:windows-|linux-)?x86_64\.", os.path.basename(pkg))
    return m.group(1)


class Run:
    def __init__(self, a):
        self.a = a
        self.work = os.path.abspath(a.work)
        shutil.rmtree(self.work, ignore_errors=True)
        os.makedirs(self.work)
        self.kind = "zip" if a.frm.endswith(".zip") else "tar.xz" if a.frm.endswith(".tar.xz") else "appimage"
        self.a_version, self.b_version = version_of(a.frm), version_of(a.to)
        # the installation, copied (its game, saves, settings and builds); the launcher A's package put in place
        self.folder = os.path.join(self.work, "Cubic GUI Update é テスト")
        os.makedirs(self.folder)
        if self.kind == "appimage":
            self.root = os.path.join(self.folder, "Cubic.Heroes")
            subprocess.run(["cp", "-a", os.path.abspath(a.install), self.root], check=True)
            self.appimage = os.path.join(self.folder, os.path.basename(a.frm))
            shutil.copy2(a.frm, self.appimage)
            os.chmod(self.appimage, 0o755)
            self.cli = [self.appimage]
            self.items = lambda: {"appimage": sha(self.appimage)}
            self.b_items = {"appimage": sha(a.to)}
        else:
            self.root = os.path.join(self.folder, "Cubic.Heroes")
            if WIN:
                shutil.copytree(os.path.abspath(a.install), self.root, symlinks=True)
            else:
                subprocess.run(["cp", "-a", os.path.abspath(a.install), self.root], check=True)
            self.unpack_over(a.frm, self.root)
            self.cli = [os.path.join(self.root, EXE)]
            import install_layout
            self.names = list(install_layout.launcher_package())
            self.items = lambda: tree(self.root, self.names)
            bdir = os.path.join(self.work, "B")
            self.unpack_over(a.to, bdir)
            self.b_items = tree(bdir, self.names)
        dl = os.path.join(self.root, "downloads")   # the copy starts with no update downloaded, staged or kept
        for n in os.listdir(dl) if os.path.isdir(dl) else ():
            if n != "game-up-to-date-with.txt":
                q = os.path.join(dl, n)
                shutil.rmtree(q) if os.path.isdir(q) and not os.path.islink(q) else os.remove(q)
        self.a_items = self.items()
        self.install_before = tree(self.root, INSTALL_ITEMS)
        self.dirs = updates.update_dirs(self.root)
        # B served over HTTPS, signed with the key A trusts
        self.key = os.path.abspath(a.key)
        der = openssl("pkey", "-in", self.key, "-pubout", "-outform", "DER").stdout
        self.kid = trust.key_id(trust.public_key_from(der))
        self.tls = Tls(self.work, os.path.join(self.work, "mirror"))
        self.mirror = Mirror(self.work, base=self.tls.serve(self.tls.cert("good")))
        Served.root = self.mirror.dir
        env = {k: v for k, v in os.environ.items() if k not in HELPERS}
        desktop_guard.game_env(env, False, quiet=True)
        desktop_guard.launcher_env(env)   # every launcher window of this test hidden (offscreen on Linux)
        env.update(SSL_CERT_FILE=self.tls.ca, CUBIC_UPDATE_SOURCE=self.mirror.source, PYTHONIOENCODING="utf-8")
        self.env = env

    def unpack_over(self, pkg, dest):
        tmp = dest + ".unpack"
        os.makedirs(tmp, exist_ok=True)
        if pkg.endswith(".zip"):
            with zipfile.ZipFile(pkg) as z:
                z.extractall(tmp)
        else:
            with tarfile.open(pkg) as t:
                t.extractall(tmp, filter="data")
        (top,) = os.listdir(tmp)
        os.makedirs(dest, exist_ok=True)
        for n in os.listdir(os.path.join(tmp, top)):
            d = os.path.join(dest, n)
            if os.path.isdir(d) and not os.path.islink(d):
                shutil.rmtree(d)
            elif os.path.lexists(d):
                os.remove(d)
            shutil.move(os.path.join(tmp, top, n), d)
        shutil.rmtree(tmp)

    def publish(self, pkg):
        self.mirror.reset()
        Served.root = self.mirror.dir
        plat = ("windows-" if WIN else "linux-") + "x86_64"
        self.mirror.release("v" + self.b_version, [(pkg, plat, self.kind)], self.key, self.kid)

    def version(self):
        r = subprocess.run(self.cli + ["--version", "--json"], cwd=self.work, env=self.env, capture_output=True,
                           text=True, timeout=300, stdin=subprocess.DEVNULL)
        for ln in reversed(r.stdout.splitlines()):
            try:
                d = json.loads(ln)
                return d.get("cubicheroes"), d.get("worker")
            except ValueError:
                continue
        return None, None

    def last(self):
        try:
            with open(os.path.join(self.dirs["downloads"], updates.LAST), encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    def launchers(self):
        """this test's launcher processes: the program in the root, or the AppImage's (its runtime and its mount)"""
        mine = []
        for pid, path in updates._processes():
            if path.startswith(self.root + os.sep) and os.path.basename(path) == EXE:
                mine.append(pid)
            elif self.kind == "appimage" and (path == os.path.realpath(self.appimage) or
                                              (path.startswith("/tmp/.mount_") or "appimage_extracted_" in path)
                                              and os.path.basename(path) == EXE and self.mine_by_env(pid)):
                mine.append(pid)
        return mine

    def mine_by_env(self, pid):
        try:
            with open(f"/proc/{pid}/environ", "rb") as f:
                return self.mirror.source.encode() in f.read()
        except OSError:
            return False

    def end_launchers(self):
        for _ in range(20):
            pids = self.launchers()
            if not pids:
                return
            for p in pids:
                try:
                    os.kill(p, signal.SIGTERM if not WIN else signal.SIGTERM)
                except OSError:
                    pass
            time.sleep(1)

    def drive(self, name, script, args=(), wait_new=True, timeout=900):
        """A's (or whichever launcher is in place) window, driven by the script; then the window the helper starts,
        if any, waited for. Returns the script's log"""
        path = os.path.join(self.work, name + ".nav")
        with open(path, "w", encoding="utf-8") as f:
            f.write(script)
        t0 = time.time()
        p = subprocess.Popen(self.cli + ["--nav-test", path, *args], cwd=self.work, env=self.env,
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        watch = desktop_guard.Watch(p, "the launcher").start() if hasattr(desktop_guard, "Watch") else None
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
        if watch:
            watch.stop()
        deadline = time.time() + 300
        while wait_new and time.time() < deadline:   # the helper's record of this run
            last = self.last()
            if last and last.get("at", last.get("time", 0)) and os.path.getmtime(
                    os.path.join(self.dirs["downloads"], updates.LAST)) >= t0:
                break
            time.sleep(1)
        log = ""
        if os.path.exists(path + ".log"):
            with open(path + ".log", encoding="utf-8", errors="replace") as f:
                log = f.read()
        return log, (watch.violation if watch else None)

    def install_unchanged(self):
        now = tree(self.root, INSTALL_ITEMS)
        return sorted(k for k in self.install_before if self.install_before[k] != now.get(k))

    # ---- the cases ----

    def update(self):
        self.publish(self.a.to)
        log, viol = self.drive("update", UPDATE_SCRIPT)
        time.sleep(5)
        bad = [viol] if viol else []
        last = self.last()
        if last.get("result") != "updated":
            bad.append(f"the helper's record: {last}")
        up = self.launchers()
        if not up:
            bad.append("no launcher running after the restart (B's window should be up)")
        self.end_launchers()
        v = self.version()
        if v != (self.b_version, self.b_version):
            bad.append(f"--version {v}, not {self.b_version}")
        if self.items() != self.b_items:
            bad.append("the launcher's files aren't B's byte for byte")
        ch = self.install_unchanged()
        if ch:
            bad.append(f"the installation changed: {ch[:5]}")
        if "Restart and update" not in log:
            bad.append("the script never reached Restart and update: " + log[-600:])
        step(f"update through the window: {self.a_version} -> {self.b_version}, B's window up", bad,
             log=log[-2000:])

    def back(self):
        log, viol = self.drive("back", BACK_SCRIPT)
        time.sleep(5)
        bad = [viol] if viol else []
        last = self.last()
        if last.get("result") != "updated" or last.get("source") != "rollback":
            bad.append(f"the helper's record: {last}")
        if not self.launchers():
            bad.append("no launcher running after going back (A's window should be up)")
        self.end_launchers()
        if self.items() != self.a_items:
            bad.append("A's files aren't back byte for byte")
        if self.version() != (self.a_version, self.a_version):
            bad.append(f"--version {self.version()}")
        if "Go back to version" not in log:
            bad.append("the script never reached Go back: " + log[-600:])
        step(f"back through the window: {self.a_version} again", bad, log=log[-2000:])

    def broken_b(self):
        """B whose window never comes up: its program exits at once (its --version still answers B: the swap's own
        check passes, the window's health record never comes)"""
        if self.kind == "appimage" or WIN:
            return None   # an AppImage can't be changed without breaking its signature, and this stand-in is made
                          # for Linux: the failed window is also covered by tests/test_launcher_update.py
        d = os.path.join(self.work, "broken")
        self.unpack_over(self.a.to, os.path.join(d, "Cubic.Heroes"))
        prog = os.path.join(d, "Cubic.Heroes", EXE)
        real = prog + ".real"
        os.rename(prog, real)
        # a real program of this system (the update refuses anything else before it changes a byte): --version and
        # --help run B's own launcher (CubicHeroes.real beside it); any window start exits at once
        src = os.path.join(self.work, "broken.c")
        with open(src, "w") as f:
            f.write('#include <string.h>\n#include <unistd.h>\n#include <limits.h>\n#include <stdlib.h>\n'
                    'int main(int c, char** v) {\n'
                    '  if (c > 1 && (!strcmp(v[1], "--version") || !strcmp(v[1], "--help"))) {\n'
                    '    char p[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", p, sizeof p - 6);\n'
                    '    if (n > 0) { p[n] = 0; strcat(p, ".real"); v[0] = p; execv(p, v); }\n'
                    '  }\n  return 3;\n}\n')
        zig = sorted(__import__("glob").glob(os.path.join(ROOT, "build", "toolchain", "zig-x86_64-*", "zig")))
        if not zig:
            raise SystemExit("no Zig in build/toolchain (tools/fetch_zig.py) to make the broken launcher with")
        subprocess.run([zig[-1], "cc", "-target", "x86_64-linux-gnu.2.31", "-O1", "-o", prog, src], check=True)
        pkg = os.path.join(self.work, os.path.basename(self.a.to))
        with tarfile.open(pkg, "w:xz", preset=0) as t:
            t.add(os.path.join(d, "Cubic.Heroes"), arcname="Cubic.Heroes")
        return pkg

    def failed(self):
        pkg = self.broken_b()
        if not pkg:
            step("failed restart through the window (not here for an AppImage or Windows: in the unit tests)", [], skipped=True)
            return
        self.publish(pkg)
        log, viol = self.drive("failed", UPDATE_SCRIPT)
        time.sleep(5)
        bad = [viol] if viol else []
        last = self.last()
        if last.get("result") != "restored":
            bad.append(f"the helper's record: {last}")
        if not self.launchers():
            bad.append("A wasn't started again after the failed update")
        self.end_launchers()
        if self.items() != self.a_items:
            bad.append("A's files aren't back byte for byte")
        if os.path.lexists(os.path.join(self.root, EXE + ".real")):
            bad.append("the failed version's extra file is left in the folder")
        if self.version() != (self.a_version, self.a_version):
            bad.append(f"--version {self.version()}")
        look, _ = self.drive("failed-look", LOOK_SCRIPT, wait_new=False, timeout=300)
        self.end_launchers()
        step("a new version whose window never comes up: undone, A back and started", bad, log=log[-2000:])

    def crash(self):
        """the helper killed while it swaps: the next start recovers"""
        self.publish(self.a.to)
        # download and stage through the command line, then start the swap and kill its helper part-way
        r = subprocess.run(self.cli + ["--apply-update", "--json"], cwd=self.work, env=self.env, capture_output=True,
                           text=True, timeout=1800, stdin=subprocess.DEVNULL)
        killed = None
        deadline = time.time() + 120
        while time.time() < deadline and not killed:
            try:
                with open(os.path.join(self.dirs["downloads"], updates.JOURNAL), encoding="utf-8") as f:
                    j = json.load(f)
            except (OSError, ValueError):
                j = {}
            if j.get("state") in ("swapping", "swapped") and j.get("helper_pid"):
                try:
                    os.kill(j["helper_pid"], getattr(signal, "SIGKILL", signal.SIGTERM))   # Windows: TerminateProcess
                    killed = j["state"]
                except OSError:
                    pass
            time.sleep(0.002)   # as early in the swap as it can be caught
        if killed:   # the helper gone for good before the next start (killed, and reaped)
            for _ in range(100):
                try:
                    with open(f"/proc/{j['helper_pid']}/stat") as f:
                        if f.read().rsplit(")", 1)[1].split()[0] == "Z":
                            break
                except OSError:
                    break
                time.sleep(0.1)
            print(f"  (killed at {killed}; downloads/ then: {sorted(os.listdir(self.dirs['downloads']))})", flush=True)
        self.end_launchers()
        bad = []
        if not killed:
            last = self.last()
            step("the helper killed mid-swap: recovered on the next start (not reached: the swap ended first)", [],
                 note=f"apply {r.returncode}; last {last}")
            return
        # a player's next start (not --version, which never waits on an update): the launcher sees the unfinished
        # journal, starts the recovery and says so (exit 5)
        st = subprocess.run(self.cli + ["--status", "--json"], cwd=self.work, env=self.env, capture_output=True,
                            text=True, timeout=300, stdin=subprocess.DEVNULL)
        print(f"  (the next start said: {st.stdout.strip()[-200:]})", flush=True)
        deadline = time.time() + 300
        seen = set()
        while time.time() < deadline and os.path.exists(os.path.join(self.dirs["downloads"], updates.JOURNAL)):
            seen.update(f"{pid}:{os.path.basename(p)}" for pid, p in updates._processes()
                        if p.startswith(self.root + os.sep) or "launcher-helper" in p)
            time.sleep(1)
        if seen:
            print(f"  (running from the installation while it recovered: {sorted(seen)[:12]})", flush=True)
        self.end_launchers()
        now = self.items()
        v = self.version()
        if os.path.exists(os.path.join(self.dirs["downloads"], updates.JOURNAL)):
            bad.append("the journal is still there: nothing recovered")
        if now == self.b_items and v == (self.b_version, self.b_version):
            outcome = "B kept"
        elif now == self.a_items and v == (self.a_version, self.a_version):
            outcome = "A put back"
        else:
            outcome = "a mixture"
            bad.append(f"neither A nor B whole after recovery: --version {v}")
        ch = self.install_unchanged()
        if ch:
            bad.append(f"the installation changed: {ch[:5]}")
        last = self.last()
        if not last.get("recovered") and "interrupted" not in (last.get("why") or ""):
            bad.append(f"the record isn't the recovery's: {last}; the start said: {st.stdout.strip()[-300:]}")
        step(f"the helper killed while {killed} (done: {len(j.get('done', []))} of {len(j.get('names', []))}): "
             f"recovered on the next start (status exit {st.returncode}; {outcome})", bad)

    def main(self):
        print(f"A {self.a_version} -> B {self.b_version} ({self.kind}) in {self.root}", flush=True)
        # Windows: any program of this test's installation showing a window, whoever started it (the helper and the
        # launchers it starts are detached from this run), is ended at once and fails the run
        folder_watch = desktop_guard.FolderWatch([self.work]).start() if hasattr(desktop_guard, "FolderWatch") else None
        try:
            for c in self.a.only or ("update", "back", "failed", "crash"):
                getattr(self, c)()
        finally:
            self.end_launchers()
            self.tls.close()
            if folder_watch:
                folder_watch.stop()
                step("no window shown by any program of the installation", list(folder_watch.violations))
        with open(os.path.join(self.work, "gui-report.json"), "w", encoding="utf-8") as f:
            json.dump({"from": self.a_version, "to": self.b_version, "kind": self.kind, "results": results}, f,
                      indent=1)
        failed = [r for r in results if not r["ok"]]
        print(f"== {len(results) - len(failed)} of {len(results)} cases passed")
        return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--from", dest="frm", required=True)
    ap.add_argument("--to", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--install", required=True, help="an installation with the game (copied, never changed)")
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "qual", "launcher-gui-update"))
    ap.add_argument("--only", nargs="*", choices=("update", "back", "failed", "crash"))
    a = ap.parse_args()
    a.frm, a.to = os.path.abspath(a.frm), os.path.abspath(a.to)
    return Run(a).main()


if __name__ == "__main__":
    sys.exit(main())
