#!/usr/bin/env python3
"""The launcher's own update, end to end, with real packaged builds and the installed launcher (0.2.0-beta.4):
Windows (the zip), Linux (the tarball; the AppImage with --key, below).

    python tests/launcher_update_e2e.py --from PKG_A --to PKG_B [--disc ISO --keys FILE] [--work DIR]
                                         [--key PEM] [--manifest release.json --signature release.json.sig]

PKG_A and PKG_B: two packages of the same kind made by tools/make_bundle.py, B a newer version than A. A is
installed in a folder whose name has spaces and non-ASCII characters (with the game from --disc/--keys: the player's
own files, read here and never kept), B is served by a local HTTPS release mirror (a throwaway CA, trusted through
SSL_CERT_FILE; the launcher pointed at it by CUBIC_UPDATE_SOURCE). Its manifest is signed:
  - by default with a throwaway Ed25519 key, which the installed A's worker/config/release_keys.json is made to
    trust (that test copy only; the project's key is never involved);
  - --key PEM: with that key, which A already trusts (an AppImage A made with a test key: its image can't be
    changed after it is made);
  - --manifest/--signature: the release's own manifest and signature (the owner's signature over B's records): A
    is left exactly as packaged and trusts only the project's key -- the final demonstration.
Then, every step through the installed launcher's command line (CubicHeroes --apply-update / --rollback-launcher,
--version, --status, --update, --play) and the window's own path (the worker's apply-update and rollback-launcher
with a restart, the window hidden), each followed by what it must and mustn't have changed:
  refused   an altered manifest, a key A doesn't trust, an altered package, a release with no package for this
            system, an untrusted HTTPS certificate, no server, the game running, nothing newer: each refused (or
            nothing offered), A's launcher and the installation unchanged, byte for byte
  apply     a download cut off part-way: the next --apply-update resumes it, verifies it, installs it -- the
            launcher exits, the helper swaps, checks and keeps it; --version says B (launcher and worker), the
            launcher's files are B's byte for byte, the installation's own files untouched, A kept for rollback
  game      --update brings the game up to date with B; --play: the game starts and ends in order (status 0); the
            saves and settings are the same
  rollback  --rollback-launcher: A back, byte for byte; B kept in turn
  window    the window's path: the worker's apply-update with a restart -- the new launcher's window started
            (hidden), reporting its version, kept; rollback-launcher the same way
Every program runs in the desktop guard's environment (no window shown) with a time limit. Exit 0 when every
step passes. The results go to <work>/e2e-report.json.
"""
import argparse
import hashlib
import json
import os
import shutil
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
from linux_update_check import Mirror, Served, Tls, make_key, openssl  # noqa: E402

WIN = os.name == "nt"
EXE = "CubicHeroes.exe" if WIN else "CubicHeroes"
WORKER = "DotHeroes-Setup.exe" if WIN else "DotHeroes-Setup"
HELPERS = ("SSL_CERT_FILE", "SSL_CERT_DIR", "CUBIC_UPDATE_SOURCE", "http_proxy", "https_proxy", "HTTP_PROXY",
           "HTTPS_PROXY", "all_proxy", "ALL_PROXY", "APPIMAGE", "APPDIR")
results = []


def step(name, bad, **info):
    results.append({"step": name, "ok": not bad, "bad": bad, **info})
    print(("PASS" if not bad else "FAIL") + f": {name}" + (f" -- {'; '.join(bad)}" if bad else ""), flush=True)


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def kind_of(path):
    n = path.lower()
    return "zip" if n.endswith(".zip") else "tar.xz" if n.endswith(".tar.xz") else "appimage"


def unpack(pkg, dest):
    """a package as a player installs it: the zip or tarball extracted (its Cubic.Heroes folder), or the AppImage
    put in the folder; returns (the launcher's command, the installation's root, the launcher's folder)"""
    os.makedirs(dest, exist_ok=True)
    k = kind_of(pkg)
    if k == "zip":
        with zipfile.ZipFile(pkg) as z:
            z.extractall(dest)
    elif k == "tar.xz":
        with tarfile.open(pkg) as t:
            t.extractall(dest, filter="data")
    else:
        app = os.path.join(dest, os.path.basename(pkg))
        shutil.copy2(pkg, app)
        os.chmod(app, 0o755)
        return [app], os.path.join(dest, "Cubic.Heroes"), os.path.join(dest, "Cubic.Heroes")
    root = os.path.join(dest, "Cubic.Heroes")
    return [os.path.join(root, EXE)], root, root


def files_of(folder, names):
    """the named items under folder, file by file: {relative path: SHA-256}"""
    out = {}
    for n in names:
        p = os.path.join(folder, n)
        if os.path.isfile(p):
            out[n] = sha(p)
        elif os.path.isdir(p):
            for d, _, fs in os.walk(p):
                for f in fs:
                    q = os.path.join(d, f)
                    out[os.path.relpath(q, folder)] = sha(q)
    return out


PACKAGE_ITEMS = ("CubicHeroes.exe", "CubicHeroes", "SDL2.dll", "lib", "licenses", "LICENSE.txt", "README.txt", "worker")
INSTALL_ITEMS = ("active.json", "builds", "data", "config", "backups")


def installation(root):
    """the installation's own files (saves, settings, builds, shader caches, backups) and its game link"""
    snap = files_of(root, INSTALL_ITEMS)
    g = os.path.join(root, "game")
    snap["game ->"] = os.path.normcase(os.path.realpath(g)) if os.path.lexists(g) else None
    return snap


class E2E:
    def __init__(self, a):
        self.a = a
        self.work = os.path.abspath(a.work)
        shutil.rmtree(self.work, ignore_errors=True)
        os.makedirs(self.work)
        self.kind = kind_of(a.frm)
        place = os.path.join(self.work, "Cubic Update E2E é 測試")
        self.cli, self.root, self.launcher = unpack(os.path.abspath(a.frm), place)
        self.appimage = self.cli[0] if self.kind == "appimage" else None
        bdir = os.path.join(self.work, "B")
        if self.kind == "appimage":
            self.b_items = None
        else:
            _, broot, _ = unpack(os.path.abspath(a.to), bdir)
            self.b_items = files_of(broot, PACKAGE_ITEMS)
        self.platform = ("windows-" if WIN else "linux-") + "x86_64"
        self.tls = Tls(self.work, os.path.join(self.work, "mirror"))
        self.good = self.tls.serve(self.tls.cert("good"))
        self.mirror = Mirror(self.work, base=self.good)
        Served.root = self.mirror.dir
        if a.manifest:
            self.key = self.kid = None
        elif a.key:
            self.key = os.path.abspath(a.key)
            der = openssl("pkey", "-in", self.key, "-pubout", "-outform", "DER").stdout
            self.kid = trust.key_id(trust.public_key_from(der))
        else:   # a throwaway key, trusted by this test copy of A only
            self.key, self.kid, pk = make_key(self.work, "test")
            import base64
            keys = os.path.join(self.launcher, "worker", "config", "release_keys.json")
            with open(keys, "w", encoding="utf-8") as f:
                json.dump({"keys": [{"id": self.kid, "ed25519": base64.b64encode(pk).decode(), "note": "e2e test"}]}, f)
        self.other, self.other_id, _ = make_key(self.work, "other")
        self.a_items = self.launcher_now()
        env = {k: v for k, v in os.environ.items() if k not in HELPERS}
        desktop_guard.game_env(env, False, quiet=True)
        desktop_guard.launcher_env(env)   # any launcher window this test starts stays hidden
        env["SSL_CERT_FILE"] = self.tls.ca
        env["PYTHONIOENCODING"] = "utf-8"
        self.env = env

    # ---- running the installed launcher ----

    def run(self, *args, timeout=600, source=None):
        env = dict(self.env, CUBIC_UPDATE_SOURCE=source or self.mirror.source)
        r = subprocess.run(self.cli + list(args), cwd=self.work, env=env, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout, stdin=subprocess.DEVNULL)
        js = [ln for ln in r.stdout.splitlines() if ln.startswith("{")]
        try:
            last = json.loads(js[-1]) if js else {}
        except ValueError:
            last = {}
        return r.returncode, last, r

    def worker(self, *args, timeout=600, source=None):
        """the installed A's (or whichever launcher is in place) worker, as the window runs it"""
        if self.kind == "appimage":
            raise SystemExit("the window's path for an AppImage is tested through its own window, not here")
        w = os.path.join(self.launcher, "worker", WORKER)
        env = dict(self.env, CUBIC_UPDATE_SOURCE=source or self.mirror.source)
        r = subprocess.run([w, "--worker", *args], cwd=self.work, env=env, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout, stdin=subprocess.DEVNULL)
        res = [json.loads(ln) for ln in r.stdout.splitlines() if ln.startswith("{") and '"result"' in ln]
        return res[-1] if res else {"ok": False, "message": r.stderr[-400:]}

    def version(self):
        code, d, r = self.run("--version", "--json", timeout=180)
        return d.get("cubicheroes"), d.get("worker")

    def last(self):
        p = os.path.join(self.root if self.kind == "appimage" else self.launcher, "downloads",
                         "launcher-update-last.json")
        try:
            with open(p, encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def wait_last(self, since, timeout=300):
        """the helper's record of how the swap ended, once it is newer than `since`"""
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout:
            d = self.last()
            if d and d.get("time", 0) >= since:
                time.sleep(1)   # the helper removes its own copy just after
                return d
            time.sleep(0.5)
        return None

    def launcher_now(self):
        return files_of(self.launcher, PACKAGE_ITEMS) if self.kind != "appimage" else {"app": sha(self.cli[0])}

    def unchanged(self, name, before_inst, extra=()):
        bad = list(extra)
        if self.launcher_now() != self.a_items:
            bad.append("A's launcher changed")
        if installation(self.root) != before_inst:
            bad.append("the installation's files changed")
        step(name, bad)

    # ---- the releases the mirror lists ----

    def release(self, version, pkg=None, platform=None, kind=None, key=None, kid=None, tamper=False):
        self.mirror.reset()
        pkg = pkg or os.path.abspath(self.a.to)
        if self.a.manifest and key is None and not tamper:   # the release's own signed manifest, as published
            d = os.path.join(self.mirror.dir, "v" + version)
            os.makedirs(d)
            for f in (pkg, self.a.manifest, self.a.signature):
                shutil.copy2(f, os.path.join(d, os.path.basename(f)))
            links = [{"name": os.path.basename(f), "browser_download_url": self.mirror.link(os.path.join(d, os.path.basename(f)))}
                     for f in (pkg, self.a.manifest, self.a.signature)]
            self.mirror.list = [{"tag_name": "v" + version, "prerelease": True, "draft": False, "assets": links}]
            with open(os.path.join(self.mirror.dir, "releases.json"), "w") as f:
                json.dump(self.mirror.list, f)
            return
        self.mirror.release("v" + version, [(pkg, platform or self.platform, kind or self.kind)],
                            key or self.key or self.other, kid or self.kid or self.other_id, tamper=tamper)

    # ---- the steps ----

    def install_game(self):
        if not self.a.disc:
            step("game installed (skipped: no --disc)", [])
            return
        code, d, r = self.run("--install", "--disc", self.a.disc, "--keys", self.a.keys, "--json", timeout=7200)
        step("A installed from the player's disc", [] if code == 0 else [f"exit {code}: {r.stderr[-300:]}"])

    def refused(self):
        a_v, b_v = self.a_version, self.b_version
        inst = installation(self.root)

        def apply(source=None):
            return self.run("--apply-update", "--json", timeout=900, source=source)

        def expect(name, codes, source=None, ok_zero=False):
            code, d, r = apply(source)
            bad = []
            if ok_zero:
                if code != 0 or d.get("installing"):
                    bad.append(f"exit {code}, {d}")
            elif code == 0 or d.get("code") not in codes:
                bad.append(f"exit {code}, code {d.get('code')}: {d.get('message')}")
            self.unchanged(name, inst, bad)

        self.release(b_v, tamper=True)
        expect("refused: an altered manifest", ("bad_signature", "untrusted_key"))
        if not self.a.manifest:
            self.release(b_v, key=self.other, kid=self.other_id)
            expect("refused: a key A doesn't trust", ("untrusted_key",))
        self.release(b_v)
        tampered = os.path.join(self.mirror.dir, "v" + b_v, os.path.basename(self.a.to))
        with open(tampered, "r+b") as f:
            f.seek(os.path.getsize(tampered) // 2)
            b = f.read(1)
            f.seek(-1, 1)
            f.write(bytes([b[0] ^ 0xFF]))
        expect("refused: an altered package", ("bad_download",))
        shutil.rmtree(os.path.join(self.root, "downloads", b_v), ignore_errors=True)
        if not self.a.manifest:
            other = "linux-x86_64" if WIN else "windows-x86_64"
            fake = os.path.join(self.work, "other" + (".tar.xz" if WIN else ".zip"))
            shutil.copy2(self.a.to, fake)
            self.release(b_v, pkg=fake, platform=other, kind="tar.xz" if WIN else "zip")
            expect("nothing offered: a release with no package for this system", (), ok_zero=True)
        self.release(b_v)
        untrusted = self.tls.serve(Tls(os.path.join(self.work, "rogue"), self.mirror.dir).cert("rogue"))
        expect("refused: an HTTPS certificate nobody trusts", ("tls",), source=untrusted + "/releases.json")
        expect("refused: no server (offline)", ("offline",), source="https://127.0.0.1:9/releases.json")
        busy_dir = os.path.join(self.root, "builds", "e2e-busy")
        os.makedirs(busy_dir, exist_ok=True)
        if WIN:
            prog = os.path.join(busy_dir, "ping.exe")
            shutil.copy2(os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "PING.EXE"), prog)
            busy = subprocess.Popen([prog, "-n", "300", "127.0.0.1"], stdout=subprocess.DEVNULL,
                                    creationflags=0x08000000)
        else:
            prog = os.path.join(busy_dir, "sleep")
            shutil.copy2(shutil.which("sleep"), prog)
            busy = subprocess.Popen([prog, "300"])
        inst_busy = installation(self.root)
        try:
            code, d, r = apply()
            bad = [] if code != 0 and d.get("code") == "game_running" else [f"exit {code}, {d}"]
            self.unchanged("refused: a game running from the installation", inst_busy, bad)
        finally:
            busy.kill()
            busy.wait()
            shutil.rmtree(busy_dir, ignore_errors=True)
        shutil.rmtree(os.path.join(self.root, "downloads", b_v), ignore_errors=True)
        if not self.a.manifest:
            self.release(a_v, pkg=os.path.abspath(self.a.frm))
            expect("nothing offered: no newer release", (), ok_zero=True)

    def apply(self):
        inst = installation(self.root)
        self.release(self.b_version)
        Served.cut[os.path.basename(self.a.to)] = 1   # the first download is cut off part-way
        code, d, r = self.run("--apply-update", "--json", timeout=900)
        step("a download cut off: refused, kept to resume", [] if code != 0 and d.get("code") in ("interrupted",
             "bad_download") else [f"exit {code}, {d}"])
        t0 = time.time()
        code, d, r = self.run("--apply-update", "--json", timeout=900)
        bad = [] if code == 0 and d.get("installing") and d.get("version") == self.b_version else [f"exit {code}, {d}"]
        last = self.wait_last(t0) if not bad else None
        if not bad and (not last or last.get("result") != "updated"):
            bad.append(f"the helper's record: {last}")
        v = self.version()
        if v != (self.b_version, self.b_version):
            bad.append(f"--version: launcher {v[0]}, worker {v[1]}")
        if self.b_items is not None and self.launcher_now() != self.b_items:
            diff = sorted(k for k in set(self.b_items) | set(self.launcher_now())
                          if self.b_items.get(k) != self.launcher_now().get(k))
            bad.append(f"the launcher's files aren't B's ({len(diff)}: {diff[:5]})")
        if installation(self.root) != inst:
            bad.append("the installation's files changed")
        code, st, r = self.run("--status", "--json", timeout=300)
        lu = st.get("launcher_update", {})
        if lu.get("rollback", {}).get("version") != self.a_version:
            bad.append(f"A not kept for rollback: {lu}")
        step("the update: downloaded, verified, installed, B running", bad, last=last)

    def game(self):
        if not self.a.disc:
            step("game steps (skipped: no --disc)", [])
            return
        saves = files_of(self.root, ("data/gamedata", "config"))
        code, d, r = self.run("--update", "--json", timeout=3600)
        bad = [] if code == 0 else [f"--update exit {code}: {r.stderr[-300:]}"]
        log = os.path.join(self.root, "logs", "game.log")
        env = dict(self.env, DOTHEROES_OVERLAY_TEST="1")
        p = subprocess.Popen(self.cli + ["--play", "--json"], cwd=self.work, env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
        first = p.stdout.readline().decode("utf-8", "replace")
        time.sleep(45)
        with open(os.path.join(self.root, "overlay.input"), "w") as f:
            f.write("exit wait wait wait wait right wait wait a\n")
        try:
            p.communicate(timeout=180)
        except subprocess.TimeoutExpired:
            p.kill()
            p.communicate()
        text = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
        if '"ev":"started"' not in first:
            bad.append(f"--play: no started event ({first[:200]!r})")
        if p.returncode != 0 or "[exit] the game's end (status 0)" not in text:
            bad.append(f"--play: exit {p.returncode}, the game's own end {'seen' if 'status 0' in text else 'not seen'}")
        now = files_of(self.root, ("data/gamedata", "config"))
        changed = [k for k in saves if saves[k] != now.get(k) and not k.startswith(("config" + os.sep + "launcher",))]
        if changed:
            bad.append(f"saves or settings changed: {changed[:5]}")
        step("the game brought up to date with B, played to its own end", bad)

    def rollback(self):
        inst = installation(self.root)
        t0 = time.time()
        code, d, r = self.run("--rollback-launcher", "--json", timeout=300)
        bad = [] if code == 0 and d.get("version") == self.a_version else [f"exit {code}, {d}"]
        last = self.wait_last(t0) if not bad else None
        if not bad and (not last or last.get("result") != "updated"):
            bad.append(f"the helper's record: {last}")
        if self.launcher_now() != self.a_items:
            bad.append("A's files aren't back byte for byte")
        if self.version() != (self.a_version, self.a_version):
            bad.append(f"--version: {self.version()}")
        if installation(self.root) != inst:
            bad.append("the installation's files changed")
        step("the launcher's rollback: A back, B kept", bad)

    def window(self):
        if self.kind == "appimage":
            step("the window's path (AppImage: tested through its own window by the Linux instance)", [])
            return
        dummy = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(3)"])
        t0 = time.time()
        r = self.worker("apply-update", "--install", self.root, "--launcher", self.launcher, "--version",
                        self.b_version, "--wait-pid", str(dummy.pid))
        bad = [] if r.get("ok") else [f"apply-update: {r}"]
        last = self.wait_last(t0) if not bad else None
        if not bad and (not last or last.get("result") != "updated"):
            bad.append(f"the helper's record: {last}")
        if self.version() != (self.b_version, self.b_version):
            bad.append(f"--version: {self.version()}")
        shown = self.close_window()
        if shown:
            bad.append(f"a launcher window was visible: {shown}")
        step("the window's path: installed on restart, the new window up (hidden) and kept", bad)
        dummy = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(3)"])
        t0 = time.time()
        r = self.worker("rollback-launcher", "--install", self.root, "--launcher", self.launcher, "--wait-pid",
                        str(dummy.pid))
        bad = [] if r.get("ok") else [f"rollback-launcher: {r}"]
        last = self.wait_last(t0) if not bad else None
        if not bad and (not last or last.get("result") != "updated"):
            bad.append(f"the helper's record: {last}")
        shown = self.close_window()
        if shown:
            bad.append(f"a launcher window was visible: {shown}")
        if self.launcher_now() != self.a_items:
            bad.append("A's files aren't back byte for byte")
        step("the window's path: the launcher's rollback, A's window up (hidden)", bad)

    def close_window(self):
        """the launcher windows the helper started (hidden), ended: only this test's, by their program's path. Returns
        any that was visible (the desktop guard's rule: none may be)"""
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import updates
        shown = []
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            mine = [(pid, p) for pid, p in updates.running_from(self.launcher)
                    if os.path.basename(p).lower() == EXE.lower()]
            if not mine:
                return shown
            for pid, _ in mine:
                shown += desktop_guard.visible_windows(pid)
                if WIN:
                    subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], capture_output=True)
                else:
                    try:
                        os.kill(pid, 15)
                    except OSError:
                        pass
            time.sleep(1)
        return shown

    def main(self):
        self.a_version = self.version()[0]
        self.b_version = self.b_version_of()
        print(f"A {self.a_version} ({self.kind}) in {self.root}; B {self.b_version}", flush=True)
        if not self.a_version or not self.b_version or \
                trust.parse_version(self.b_version) <= trust.parse_version(self.a_version):
            sys.exit("B must be a newer version than A")
        self.install_game()
        self.refused()
        self.apply()
        self.game()
        self.rollback()
        self.window()
        self.tls.close()
        with open(os.path.join(self.work, "e2e-report.json"), "w", encoding="utf-8") as f:
            json.dump({"from": self.a_version, "to": self.b_version, "kind": self.kind, "results": results}, f, indent=1)
        failed = [r for r in results if not r["ok"]]
        print(f"== {len(results) - len(failed)} of {len(results)} steps passed", flush=True)
        return 1 if failed else 0

    def b_version_of(self):
        if self.a.manifest:
            with open(self.a.manifest, encoding="utf-8") as f:
                return json.load(f)["version"]
        n = os.path.basename(self.a.to)   # Cubic.Heroes-<version>-<platform>.<ext>, or ...-<version>-x86_64.AppImage
        import re
        m = re.match(r"Cubic\.Heroes-(.+?)-(?:windows-|linux-)?x86_64\.", n)
        return m.group(1) if m else None


def main():
    ap = argparse.ArgumentParser(description="the launcher's own update, end to end, with real packages")
    ap.add_argument("--from", dest="frm", required=True, help="package A (installed)")
    ap.add_argument("--to", required=True, help="package B (the newer one, served by the mirror)")
    ap.add_argument("--disc")
    ap.add_argument("--keys")
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "qual", "launcher-update-e2e"))
    ap.add_argument("--key", help="sign with this key, which A already trusts (an AppImage A)")
    ap.add_argument("--manifest", help="B's own signed manifest (A unchanged: the project's key)")
    ap.add_argument("--signature")
    a = ap.parse_args()
    if bool(a.manifest) != bool(a.signature):
        ap.error("--manifest and --signature go together")
    if bool(a.disc) != bool(a.keys):
        ap.error("--disc and --keys go together")
    if kind_of(a.frm) != kind_of(a.to):
        ap.error("A and B must be the same kind of package")
    sys.exit(E2E(a).main())


if __name__ == "__main__":
    main()
