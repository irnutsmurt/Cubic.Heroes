#!/usr/bin/env python3
"""M11.3: a local release candidate qualified from its package, as a player gets it -- Windows (M11.3) and, since
M11.4, Linux: its tarball and its AppImage.

    tests/release_qualification.py --package ZIP|TAR.XZ|APPIMAGE --disc ISO --keys KEYS [--work DIR] [--offline]
                                   [--from STEP] [--only STEP ...] [--interrupt-after SECONDS]

The package (tools/make_bundle.py's zip or tar.xz, or tools/make_appimage.py's AppImage) is unpacked into a fresh
folder whose name holds spaces and non-ASCII letters (an AppImage: copied into such a folder, its installation root
the Cubic.Heroes folder beside it, as a player's); everything then goes through CubicHeroes's command line, as a player's launcher or script would (docs/CLI.md),
and the worker program in the package (never this checkout's). --offline: every run with a dead proxy
(HTTP(S)_PROXY=127.0.0.1:9 -- the update check fails as without a network; nothing else may need one). The disc image
and the keys file are only read: their size and time (the image) and SHA-256 (the keys) checked before and after,
and no line of the keys file may appear in any file under the installation. The game runs offscreen (the desktop
guard); never beside another game run.

Steps (in order; --from STEP resumes, --only runs some):
  unpack     the package unpacked; its top items are the launcher package's
  empty      --status: nothing installed, the folder is the root
  install    --install, interrupted once after --interrupt-after seconds (default 240), then --install again: resumed,
             installed; the disc and keys unchanged
  status     --status: installed, the game inside the root
  shaders    --prepare-shaders; --status: prepared
  noop       --update: nothing rebuilt (the tools unchanged)
  checkup    --check-update: an answer (offline: "offline", exit 0, at once -- never a hang)
  play       --play: the game started directly, ended in order through its exit question, status 0
  suite      tests/devtools_suite.py --installed: developer mode on (its test saves), then off -- a player's save
             brought in first when the installation has none (--saves: a game-written save folder), as the suite
             continues one
  rebuild    a lifting tool changed in the package: --update relifts and rebuilds into a new build, the old kept
  rollback   the worker's rollback: the first build active again (nothing rebuilt); --play again
  gameappimage  (Linux) the player's game AppImage, made by the install, update and rollback (exports/): it starts
             the active build, through FUSE and with --appimage-extract-and-run
  relocate   the whole installation (the AppImage with its folder) moved to another folder: found there, the game
             played, its game AppImage still finding it
  keys       no line of the keys file in any file of the installation
  remove     the launcher removed by its worker (remove-launcher): the installation, saves and settings kept
             (an AppImage: not applicable -- it is a file the player deletes, never unpacked into the folder)
  reunpack   the package unpacked over the folder again (a player putting the launcher back): the installation found
             as it was, nothing of it replaced (an AppImage: not applicable, as remove)
Results: <work>/qualification.json and a PASS/FAIL line per step.
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
import install_layout  # noqa: E402

STEPS = ["unpack", "empty", "install", "status", "shaders", "noop", "checkup", "play", "suite", "rebuild",
         "rollback", "gameappimage", "relocate", "keys", "remove", "reunpack"]
WIN = os.name == "nt"
EXE = ".exe" if WIN else ""
NAME = "Cubic H\u00e9roes RC \u00fc \u30c6\u30b9\u30c8"   # an accent and Japanese: the game must open its files by them


class Q:
    def __init__(self, o):
        self.o = o
        self.work = os.path.abspath(o.work)
        low = o.package.lower()
        self.kind = "appimage" if low.endswith(".appimage") else "tar.xz" if low.endswith((".tar.xz", ".txz")) \
            else "zip"
        self.set_place(os.path.join(self.work, NAME))
        self.res_path = os.path.join(self.work, "qualification.json")
        self.res = json.load(open(self.res_path, encoding="utf-8")) if os.path.exists(self.res_path) else {}
        self.env = dict(os.environ)
        for k in ("CUBIC_INSTALL_DIR", "CUBIC_DIRECT_PLAY", "CUBIC_LAUNCHER", "CUBIC_LAUNCHER_PID", "CUBIC_WORKER"):
            self.env.pop(k, None)
        desktop_guard.game_env(self.env, False)   # the game offscreen, whatever starts it
        self.env["ALSOFT_DRIVERS"] = "null"
        if o.offline:
            for k in ("HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy"):
                self.env[k] = "http://127.0.0.1:9"
            self.env["NO_PROXY"] = self.env["no_proxy"] = ""

    def set_place(self, folder):
        """where the package is: the folder it was unpacked to (the root itself), or for an AppImage the folder holding
        the .AppImage file and, beside it, its root"""
        self.folder = folder
        if self.kind == "appimage":
            self.exe = os.path.join(folder, os.path.basename(self.o.package))
            self.root = os.path.join(folder, "Cubic.Heroes")
            self.worker_exe = os.path.join(self.work, "extracted", "squashfs-root", "worker", "DotHeroes-Setup")
        else:
            self.root = folder
            self.exe = os.path.join(folder, "CubicHeroes" + EXE)
            self.worker_exe = os.path.join(folder, "worker", "DotHeroes-Setup" + EXE)

    def save(self):
        with open(self.res_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(self.res, f, indent=1)

    def record(self, step, bad, **info):
        self.res[step] = {"ok": not bad, "problems": bad, "at": time.strftime("%Y-%m-%d %H:%M:%S"), **info}
        self.save()
        print(f"{'PASS' if not bad else 'FAIL'}: {step}" + (f" -- {'; '.join(bad)}" if bad else "") +
              (f" ({info.get('note')})" if info.get("note") else ""), flush=True)

    def cli(self, *args, timeout=3600, env=None):
        e = dict(self.env, **(env or {}))
        t0 = time.time()
        r = subprocess.run([self.exe, *args], cwd=self.work, env=e, capture_output=True, timeout=timeout)
        return r.returncode, r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace"), \
            round(time.time() - t0, 1)

    def status(self):
        code, out, err, _ = self.cli("--status", "--json", timeout=600)
        try:
            return code, json.loads(out.strip().splitlines()[-1])
        except (ValueError, IndexError):
            return code, {"_raw": (out + err)[-500:]}

    def worker(self, *args, timeout=3600):
        r = subprocess.run([self.worker_exe, "--worker", *args], cwd=self.work, env=self.env, capture_output=True, timeout=timeout)
        out = r.stdout.decode("utf-8", "replace")
        result = None
        for line in out.splitlines():
            try:
                ev = json.loads(line)
            except ValueError:
                continue
            if ev.get("ev") == "result":
                result = ev
        return r.returncode, result, out

    def last_result(self, out):
        for line in reversed(out.strip().splitlines()):
            try:
                ev = json.loads(line)
            except ValueError:
                continue
            if "ok" in ev:
                return ev
        return {}

    # ---- the inputs, only read ----
    def inputs(self):
        st = os.stat(self.o.disc)
        h = hashlib.sha256(open(self.o.keys, "rb").read()).hexdigest()
        return {"disc_size": st.st_size, "disc_mtime": st.st_mtime_ns, "keys_sha256": h}

    # ---- the steps ----
    def unpack(self):
        if os.path.exists(self.work):
            shutil.rmtree(self.work)
        os.makedirs(self.work)
        self.res = {}
        if self.kind == "appimage":
            # the player's way: the .AppImage file in a folder of theirs; its root appears beside it (Cubic.Heroes).
            # The worker steps (builds, rollback) run the image's own worker, unpacked once for them
            os.makedirs(self.folder)
            shutil.copy2(self.o.package, self.exe)
            ex = os.path.join(self.work, "extracted")
            os.makedirs(ex)
            subprocess.run([self.exe, "--appimage-extract"], cwd=ex, check=True, stdout=subprocess.DEVNULL)
            items = sorted(os.listdir(os.path.join(ex, "squashfs-root")))
            want = sorted(install_layout.launcher_package()) + ["AppRun", "cubicheroes.desktop", "cubicheroes.png"]
            bad = [] if set(install_layout.launcher_package()) <= set(items) <= set(want) | {".DirIcon"} and \
                os.access(self.exe, os.X_OK) else [f"the image's top items {items}"]
            self.record("unpack", bad, items=items, files=sum(len(f) for _, _, f in os.walk(ex)))
            return
        if self.kind == "zip":
            with zipfile.ZipFile(self.o.package) as z:
                names = z.namelist()
                top = names[0].split("/")[0]
                for n in names:
                    if not n.startswith(top + "/"):
                        raise SystemExit(f"the package has more than one top folder ({n})")
                z.extractall(self.work)
        else:
            with tarfile.open(self.o.package) as t:
                names = t.getnames()
                top = names[0].split("/")[0]
                for n in names:
                    if n != top and not n.startswith(top + "/"):
                        raise SystemExit(f"the package has more than one top folder ({n})")
                t.extractall(self.work, filter="data")
        os.replace(os.path.join(self.work, top), self.root)
        items = sorted(os.listdir(self.root))
        want = sorted(install_layout.launcher_package())
        bad = [] if set(items) <= set(want) and "CubicHeroes" + EXE in items and "worker" in items else \
            [f"top items {items}, the package's {want}"]
        if not WIN and not os.access(self.exe, os.X_OK):
            bad.append("CubicHeroes is not executable (the archive lost its mode)")
        self.record("unpack", bad, items=items, files=len(names))

    def empty(self):
        code, st = self.status()
        bad = [] if code == 0 and st.get("installed") is False and \
            os.path.normcase(st.get("install_dir", "")) == os.path.normcase(self.root) and \
            st.get("install_dir_from") == ("beside the AppImage" if self.kind == "appimage" else "the launcher's folder") \
            else [f"{code}: {st}"]
        self.record("empty", bad)

    def install(self):
        before = self.inputs()
        args = ["--install", "--disc", self.o.disc, "--keys", self.o.keys, "--json"]
        bad, info = [], {}
        p = subprocess.Popen([self.exe, *args], cwd=self.work, env=self.env, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
        try:
            p.wait(self.o.interrupt_after)
            info["interrupted"] = False
            bad.append(f"the install ended before the interruption ({p.returncode})")
        except subprocess.TimeoutExpired:
            p.kill()   # CubicHeroes ended from outside: its job ends the worker with it
            p.wait()
            info["interrupted"] = True
        time.sleep(3)
        code, st = self.status()
        info["after_interruption"] = st.get("install", {}).get("kind")
        if info["interrupted"] and st.get("install", {}).get("kind") not in ("partial", "none"):
            bad.append(f"after the interruption the install is {st.get('install', {}).get('kind')}")
        code, out, err, secs = self.cli(*args, timeout=4 * 3600)
        info.update(seconds=secs, exit=code)
        r = self.last_result(out)
        if code != 0:
            bad.append(f"--install again: exit {code} {r.get('message', (out + err)[-300:])}")
        after = self.inputs()
        if after != before:
            bad.append(f"the disc or keys changed: {before} -> {after}")
        self.record("install", bad, note=f"resumed and installed in {secs} s", **info)

    def status_step(self):
        code, st = self.status()
        game = st.get("game") or ""
        inside = bool(game) and os.path.normcase(os.path.commonpath([game, self.root])) == os.path.normcase(self.root)
        bad = [] if code == 0 and st.get("installed") is True and os.path.isfile(game) and inside \
            else [f"{code}: {st}"]
        self.record("status", bad, active=st.get("builds", {}).get("active"))

    def shaders(self):
        code, out, err, secs = self.cli("--prepare-shaders", "--json", timeout=4 * 3600)
        code2, st = self.status()
        sh = st.get("shaders", {})
        bad = [] if code == 0 else [f"exit {code}: {self.last_result(out).get('message', (out + err)[-300:])}"]
        if sh.get("cache") != "reused" or not sh.get("comprehensive_done"):
            bad.append(f"shaders after: {json.dumps(sh)[:300]}")
        self.record("shaders", bad, seconds=secs, pipelines=sh.get("coverage", {}).get("pipelines"))

    def noop(self):
        code, out, err, secs = self.cli("--update", "--json", timeout=3600)
        r = self.last_result(out)
        data = r.get("data", {})
        bad = [] if code == 0 and data.get("changed") is False else [f"exit {code}: {json.dumps(r)[:400]}"]
        self.record("noop", bad, seconds=secs)

    def checkup(self):
        code, out, err, secs = self.cli("--check-update", "--json", timeout=300)
        r = self.last_result(out)
        if self.o.offline:   # offline is an answer, never a failure (docs/M9_WORKER_PROTOCOL.md: check-update)
            up = r.get("update") or {}
            bad = [] if code == 0 and up.get("offline") is True and not up.get("available") and secs < 120 else \
                [f"offline: exit {code} in {secs} s: {r}"]
        else:
            bad = [] if code == 0 else [f"exit {code}: {r}"]
        self.record("checkup", bad, seconds=secs, answer=r.get("message") or r.get("update"))

    def play(self, step="play"):
        log = os.path.join(self.root, "logs", "game.log")
        env = dict(self.env, DOTHEROES_OVERLAY_TEST="1")
        p = subprocess.Popen([self.exe, "--play", "--json"], cwd=self.work, env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
        first = p.stdout.readline().decode("utf-8", "replace")
        time.sleep(45)
        with open(os.path.join(self.root, "overlay.input"), "w") as f:
            f.write("exit wait wait wait wait right wait wait a\n")
        try:
            rest, _ = p.communicate(timeout=120)
            code = p.returncode
        except subprocess.TimeoutExpired:
            p.kill()
            rest, _ = p.communicate()
            code = None
        text = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
        bad = []
        if '"ev":"started"' not in first:
            bad.append(f"no started event: {first[:200]!r}")
        if code != 0:
            bad.append(f"exit {code}")
        if "[exit] the game's end (status 0)" not in text:
            bad.append("the game did not end itself in order")
        crash = os.path.join(self.root, "logs", "crash")
        reports = sorted(os.listdir(crash)) if os.path.isdir(crash) else []
        self.res.setdefault("_reports_seen", [])
        new = [r for r in reports if r not in self.res["_reports_seen"]]
        if new:
            bad.append(f"crash or hang reports: {new}")
        self.res["_reports_seen"] = reports
        self.record(step, bad)

    def suite(self):
        """the installed game's own checks (tests/devtools_suite.py --installed): developer mode set through the
        command line for them (they refuse an install without it), set back after"""
        saves = os.path.join(self.root, "data", "gamedata", "dev_hdd0", "home", "00000001", "savedata")
        brought = []
        if not any(n.startswith("BLUS30490GAME") for n in (os.listdir(saves) if os.path.isdir(saves) else [])):
            os.makedirs(saves, exist_ok=True)
            for n in os.listdir(self.o.saves):
                if os.path.isdir(os.path.join(self.o.saves, n)):
                    shutil.copytree(os.path.join(self.o.saves, n), os.path.join(saves, n), dirs_exist_ok=True)
                    brought.append(n)
        code, out, err, _ = self.cli("--set", "DOTHEROES_DEVELOPER_MODE=1", timeout=300)
        st = json.load(open(os.path.join(self.root, "active.json"), encoding="utf-8"))
        game = os.path.join(self.root, "builds", st["active"], "dotheroes" + EXE)
        r = subprocess.run([sys.executable, os.path.join(HERE, "devtools_suite.py"), "--installed", self.root,
                            "--bin", game], cwd=ROOT, env=self.env, capture_output=True, timeout=3 * 3600)
        out2 = r.stdout.decode("utf-8", "replace") + r.stderr.decode("utf-8", "replace")
        tail = [ln for ln in out2.splitlines() if "PASS" in ln or "FAIL" in ln][-12:]
        code3, _, _, _ = self.cli("--set", "DOTHEROES_DEVELOPER_MODE=0", timeout=300)
        bad = [] if r.returncode == 0 else [f"exit {r.returncode}: {tail[-3:]}"]
        if code != 0 or code3 != 0:
            bad.append(f"--set developer mode: {code}/{code3} {out[-200:]}")
        self.record("suite", bad, lines=tail, saves_brought_in=brought)

    def rebuild(self):
        code, b0, _ = self.worker("builds", "--install", self.root)
        first = (b0 or {}).get("data", {}).get("active")
        tool = os.path.join(self.root, "worker", "tools", "fix_functions.py")
        with open(tool if self.kind != "appimage" else os.devnull, "a", encoding="utf-8", newline="\n") as f:
            f.write("\n# M11.3 qualification: a lifting tool changed, as a new version's would be\n")
        # and the game's own code (M11.4): Linux's build is reproducible -- a comment in a tool relifts to the same
        # program, and the worker rightly keeps the identical build (Windows' PE differed every time) -- so the new
        # version changes compiled code too, as a real one does
        src = os.path.join(self.root, "worker", "src", "port", "stubs.cpp")
        if self.kind == "appimage":
            # an AppImage's new version is a new .AppImage file (the root beside it stays): the unpacked image
            # changed, made into an AppImage again (tools/appimage.py) and put in the old one's place
            unpacked = os.path.dirname(os.path.dirname(self.worker_exe))
            src = os.path.join(unpacked, "worker", "src", "port", "stubs.cpp")
            with open(os.path.join(unpacked, "worker", "tools", "fix_functions.py"), "a", encoding="utf-8",
                      newline="\n") as f:
                f.write("\n# M11.4 qualification: a lifting tool changed, as a new version's would be\n")
        with open(src, "a", encoding="utf-8", newline="\n") as f:
            f.write(f'\nextern "C" {{ int dotheroes_qualification_rebuild = {int(time.time())}; }}\n')
        if self.kind == "appimage":
            import appimage
            appimage.build(unpacked, self.exe + ".new")
            os.replace(self.exe + ".new", self.exe)
        code, out, err, secs = self.cli("--update", "--json", timeout=4 * 3600)
        r = self.last_result(out)
        data = r.get("data", {})
        code2, b1, _ = self.worker("builds", "--install", self.root)
        b1 = (b1 or {}).get("data", {})
        bad = []
        if code != 0 or not data.get("changed"):
            bad.append(f"--update: exit {code} {json.dumps(r)[:300]}")
        if not b1.get("active") or b1.get("active") == first or first not in (b1.get("previous") or []):
            bad.append(f"builds after: {b1}")
        self.res["_first_build"] = first
        self.record("rebuild", bad, seconds=secs, first=first, second=b1.get("active"))

    def rollback(self):
        first = self.res.get("_first_build")
        code, r, out = self.worker("rollback", "--install", self.root, "--to", first)
        code2, b, _ = self.worker("builds", "--install", self.root)
        b = (b or {}).get("data", {})
        bad = [] if code == 0 and b.get("active") == first else [f"rollback: {code} {r}; builds {b}"]
        self.record("rollback", bad, active=b.get("active"))
        self.play("rollback-play")

    def keys(self):
        lines = [ln.strip() for ln in open(self.o.keys, encoding="utf-8", errors="replace") if len(ln.strip()) >= 16]
        tokens = set()
        for ln in lines:
            for tok in ln.replace("=", " ").replace(":", " ").split():
                if len(tok) >= 16:
                    tokens.add(tok.lower())
        found = []
        for d, dirs, files in os.walk(self.root):
            dirs[:] = [x for x in dirs if x not in ("gen", "objects", "zig-global", "zig-local", "disc")]
            for f in files:
                p = os.path.join(d, f)
                if os.path.getsize(p) > 64 << 20:
                    continue
                try:
                    blob = open(p, "rb").read().decode("latin-1").lower()
                except OSError:
                    continue
                if any(t in blob for t in tokens):
                    found.append(os.path.relpath(p, self.root))
        self.record("keys", [f"key text in {found[:5]}"] if found else [], tokens=len(tokens),
                    note="the lift's and the compiler's output and the disc copy not searched (derived, large)")

    def gameappimage(self):
        """Linux: the game's own AppImage in exports/, of the active build, started through FUSE and without it"""
        if WIN:
            self.record("gameappimage", [], note="Windows: no AppImage")
            return
        img = os.path.join(self.root, "exports", "Cubic.Heroes-Game-x86_64.AppImage")
        bad = []
        if not os.access(img, os.X_OK):
            self.record("gameappimage", [f"no game AppImage at {img}"])
            return
        code, st = self.status()
        for how, args in (("FUSE", ["--info"]), ("extract-and-run", ["--appimage-extract-and-run", "--info"])):
            r = subprocess.run([img, *args], cwd=self.work, env=self.env, capture_output=True, timeout=300)
            out = r.stdout.decode("utf-8", "replace")
            try:
                info = json.loads([ln for ln in out.splitlines() if ln.startswith("{")][-1])
            except (ValueError, IndexError):
                info = {}
            if r.returncode != 0 or info.get("build_id") != st.get("install", {}).get("active") or \
                    info.get("layout") != "slots":
                bad.append(f"{how}: exit {r.returncode}, {info or out[-300:]}")
        self.record("gameappimage", bad, size_mb=round(os.path.getsize(img) / 2**20))

    def relocate(self):
        """the installation moved whole (for an AppImage, its folder with the .AppImage file and the root) to another
        folder: found and played there; the game AppImage still finds it"""
        old = self.folder
        new = os.path.join(self.work, NAME + " moved \u79fb\u52d5")
        os.replace(old, new)
        self.set_place(new)
        bad = []
        code, st = self.status()
        if code != 0 or not st.get("installed") or os.path.normcase(st.get("install_dir", "")) != os.path.normcase(self.root):
            bad.append(f"status after the move: {code} {st}")
        self.play("relocate-play")
        if not self.res.get("relocate-play", {}).get("ok"):
            bad.append("--play after the move failed")
        if not WIN:
            self.gameappimage()
            if not self.res.get("gameappimage", {}).get("ok"):
                bad.append("the game AppImage after the move failed")
        self.record("relocate", bad, moved_to=os.path.basename(new))

    def remove(self):
        if self.kind == "appimage":
            self.record("remove", [], note="not applicable: an AppImage is a file the player deletes; nothing of it is "
                                           "unpacked into the installation folder")
            return
        saves0 = {k: v for k, v in tree(self.root).items() if k.startswith(("data/gamedata", "backups", "config"))}
        code, r, out = self.worker("remove-launcher", "--launcher", self.root, "--install", self.root, "--dry-run")
        plan = (r or {}).get("data", {})
        bad = [] if code == 0 and plan.get("install_kept") else [f"plan: {code} {r}"]
        code, r, out = self.worker("remove-launcher", "--launcher", self.root, "--install", self.root, "--confirm",
                                   "--wait-pid", "0", timeout=600)
        t0 = time.time()   # the removal runs in a detached helper (a copy of the worker outside the folder)
        while time.time() - t0 < 180 and (os.path.exists(self.exe) or os.path.isdir(os.path.join(self.root, "worker"))):
            time.sleep(2)
        time.sleep(3)
        saves1 = {k: v for k, v in tree(self.root).items() if k.startswith(("data/gamedata", "backups", "config"))}
        left = sorted(os.listdir(self.root)) if os.path.isdir(self.root) else []
        if code != 0:
            bad.append(f"remove: {code} {r}")
        if os.path.exists(self.exe) or os.path.isdir(os.path.join(self.root, "worker")):
            bad.append(f"the launcher's items remain: {left}")
        changed = sorted(k for k in set(saves0) | set(saves1) if saves0.get(k) != saves1.get(k))
        if changed:
            bad.append(f"saves or settings changed: {changed[:5]}")
        for n in ("builds", "data", "active.json"):
            if not os.path.exists(os.path.join(self.root, n)):
                bad.append(f"{n} removed")
        self.record("remove", bad, left=left, saves_checked=len(saves0))


    def reunpack(self):
        """the package's items unpacked over the folder (as a player puts the launcher back): the installation's own
        files untouched, the installation found again"""
        if self.kind == "appimage":
            self.record("reunpack", [], note="not applicable: an AppImage is never unpacked over the installation")
            return
        keep = tree(self.root)
        keep = {k: v for k, v in keep.items() if not k.split("/")[0] in install_layout.launcher_package()}
        if self.kind == "tar.xz":
            with tarfile.open(self.o.package) as t:
                top = t.getnames()[0].split("/")[0]
                for m in t.getmembers():
                    rel = m.name[len(top) + 1:]
                    if not rel:
                        continue
                    m.name = rel
                    t.extract(m, self.root, filter="data")
        else:
            with zipfile.ZipFile(self.o.package) as z:
                top = z.namelist()[0].split("/")[0]
                for m in z.infolist():
                    rel = m.filename[len(top) + 1:]
                    if not rel:
                        continue
                    dst = os.path.join(self.root, *rel.split("/"))
                    if m.is_dir():
                        os.makedirs(dst, exist_ok=True)
                        continue
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    with z.open(m) as src, open(dst, "wb") as out:
                        shutil.copyfileobj(src, out)
        after = tree(self.root)
        changed = sorted(k for k in keep if after.get(k) != keep[k] and not k.startswith("logs/"))
        code, st = self.status()
        bad = [] if code == 0 and st.get("installed") is True else [f"status: {code} {st}"]
        if changed:
            bad.append(f"the installation's files changed: {changed[:5]}")
        self.record("reunpack", bad, checked=len(keep))


def tree(root):
    out = {}
    for d, dirs, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            try:
                out[os.path.relpath(p, root).replace("\\", "/")] = hashlib.sha256(open(p, "rb").read()).hexdigest()
            except OSError:
                pass
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--package", required=True)
    ap.add_argument("--disc", required=True)
    ap.add_argument("--keys", required=True)
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "qual", "rc"),
                    help="a fresh folder for the run (default build/qual/rc; Windows' M11.3 run used F:)")
    ap.add_argument("--offline", action="store_true")
    ap.add_argument("--from", dest="start", choices=STEPS)
    ap.add_argument("--only", nargs="*", choices=STEPS)
    ap.add_argument("--interrupt-after", type=float, default=240)
    ap.add_argument("--saves", default=os.path.join(ROOT, "build", "checkpoints", "castle-front"),
                    help="a game-written save folder brought in when the installation has none (the suite)")
    o = ap.parse_args()
    # paths given relative to where this runs: CubicHeroes runs from the work folder (any relative path it is given
    # is taken from there, as a command line's is)
    o.package, o.disc, o.keys, o.saves = (os.path.abspath(x) for x in (o.package, o.disc, o.keys, o.saves))
    for stream in (sys.stdout, sys.stderr):   # the folder's name is not in every console's code page
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    q = Q(o)
    steps = o.only or STEPS[STEPS.index(o.start):] if (o.only or o.start) else STEPS
    fn = {"status": q.status_step}
    for s in steps:
        try:
            fn.get(s, getattr(q, s))()
        except Exception as e:   # a step that broke: recorded, the rest still run where they can
            q.record(s, [f"{type(e).__name__}: {e}"])
    failed = [s for s in steps if not q.res.get(s, {}).get("ok")]
    print(f"{len(steps) - len(failed)} of {len(steps)} steps passed" +
          (f"; failed: {', '.join(failed)}" if failed else ""))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
