#!/usr/bin/env python3
"""The packaged updater against the real update server (beta.4): GitHub's API and its release downloads, over HTTPS,
with the package's own launcher and frozen worker -- never this checkout's Python. Windows and Linux.

    python3 tests/live_update_check.py --package DIR|APPIMAGE [--work DIR] [--no-download]

DIR: an unpacked package's folder (the zip's or the tarball's); APPIMAGE: the .AppImage file (its worker run from
its extracted image, APPIMAGE set as the runtime sets it, so the AppImage's own kind is chosen). beta.3's Linux
packages passed every local-mirror test while their updater could reach nothing: its OpenSSL looked for the
certificate authorities in the build environment's folder, and every check read as offline. So nothing here may
help the package: SSL_CERT_FILE, SSL_CERT_DIR and the proxy variables are removed from its environment, and any
"offline" or "tls" answer fails. Steps:
  cli        CubicHeroes --check-update --json: an answer from the server (not offline, not an error)
  newer      the worker's check-update as an old version (0.0.1, prerelease channel): a newer release offered, its
             manifest verified with the project's release key (config/release_keys.json in the package), its
             downloads this system's and this package's kind only, nothing refused; on Linux, HTTPS verified with
             the system's certificate store (the answer's tls_store), never the build environment's
  current    the same as the newest version: offered, not newer
  download   download-update as 0.0.1: the package's kind downloaded from GitHub, resumed or not, checked by the
             worker against the signed manifest (size, SHA-256) and checked again here; deleted afterwards
             (--no-download: skipped, and reported as not done -- never as passed)
  env        (Linux) SSL_CERT_FILE naming an empty file: the setting honoured -- "tls", nothing else trusted instead
Verdict: PASS when every step passes. A failed step is FAIL, unless this machine itself can't reach GitHub's API
(an independent request, with this Python and the system's own trust): then INCONCLUSIVE -- an outage, a rate limit
or a captive portal says nothing about the package, and is never a pass either. Exit 0 PASS, 1 FAIL, 2 INCONCLUSIVE.
No window (the desktop guard's environment); the network is needed, nothing else.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import desktop_guard  # noqa: E402

WIN = os.name == "nt"
API = "https://api.github.com/repos/irnutsmurt/Cubic.Heroes/releases?per_page=1"
HELPERS = ("SSL_CERT_FILE", "SSL_CERT_DIR", "http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY", "all_proxy",
           "ALL_PROXY", "no_proxy", "NO_PROXY")


def clean_env(appimage=None):
    """the player's environment: nothing that could make a broken updater work (or a working one fail)"""
    env = desktop_guard.no_display({k: v for k, v in os.environ.items() if k not in HELPERS})
    env.pop("APPIMAGE", None)
    if appimage:
        env["APPIMAGE"] = appimage
    return env


def result(out):
    res = None
    for ln in out.splitlines():
        try:
            e = json.loads(ln)
        except ValueError:
            continue
        if isinstance(e, dict) and (e.get("ev") == "result" or "ok" in e):
            res = e
    return res or {}


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def probe():
    """None when this machine reaches GitHub's API (independently of the package: this Python, the system's own
    trust); else why not"""
    try:
        req = urllib.request.Request(API, headers={"User-Agent": "CubicHeroes-qualification",
                                                  "Accept": "application/vnd.github+json"})
        with urllib.request.urlopen(req, timeout=30) as r:
            r.read(1 << 16)
            return None if r.status == 200 else f"HTTP {r.status}"
    except Exception as e:   # anything: the network's state, not the package's
        return f"{type(e).__name__}: {e}"


def verdict(res, report=print):
    """PASS, FAIL or INCONCLUSIVE (see the module's doc) for run_checks' steps"""
    if not [n for n, b in res if b]:
        return "PASS"
    why = probe()
    if why:
        report(f"INCONCLUSIVE: this machine can't reach GitHub's API itself ({why}): rerun when it can")
        return "INCONCLUSIVE"
    return "FAIL"


def run_checks(cli, worker, work, appimage=None, download=True, report=print):
    """[(step, problems)] -- cli: the launcher's command ([path] or [appimage]); worker: the frozen worker's path"""
    os.makedirs(work, exist_ok=True)
    env = clean_env(appimage)
    kind = "zip" if WIN else ("appimage" if appimage else "tar.xz")
    out = []

    def step(name, bad, note=""):
        out.append((name, bad))
        report(("PASS" if not bad else "FAIL") + f": live {name}" + (f" -- {'; '.join(bad)}" if bad else "") +
               (f" ({note})" if note and not bad else ""))

    def w(*args, timeout=300):
        r = subprocess.run([worker, "--worker", *args], cwd=work, env=env, capture_output=True, text=True,
                           timeout=timeout)
        return r.returncode, result(r.stdout), (r.stdout + r.stderr)[-400:]

    def refused_net(d, why):
        if d.get("offline") or d.get("code") in ("offline", "tls"):
            return [f"{why}: {d.get('why') or d.get('message') or d}"]
        return []

    r = subprocess.run(cli + ["--check-update", "--json"], cwd=work, env=env, capture_output=True, text=True,
                       timeout=300)
    res = result(r.stdout)
    up = res.get("update") or {}
    bad = refused_net(up, "the update server was not reached") + refused_net(res, "the check failed")
    if r.returncode != 0 or not res.get("ok") or "available" not in up:
        bad.append(f"exit {r.returncode}: {json.dumps(res)[:300] or (r.stdout + r.stderr)[-300:]}")
    step("cli", bad, f"installed {up.get('installed')}, available {up.get('available')}")

    rc, res, tail = w("check-update", "--installed", "0.0.1", "--channel", "prerelease")
    d = res.get("data") or {}
    bad = refused_net(d, "the update server was not reached") + refused_net(res, "the check failed")
    names = [a.get("name", "") for a in d.get("assets", [])]
    if rc != 0 or not d.get("available") or not d.get("newer"):
        bad.append(f"no newer release offered: exit {rc} {json.dumps(res)[:300] or tail}")
    if d.get("refused"):
        bad.append(f"releases refused: {d['refused']}")
    plat = {a.get("platform") for a in d.get("assets", [])}
    if plat and (len(plat) != 1 or not plat.pop().startswith("windows-" if WIN else "linux-")):
        bad.append(f"downloads of another system offered: {names}")
    if not [n for n in names if n.endswith("." + {"zip": "zip", "tar.xz": "tar.xz", "appimage": "AppImage"}[kind])]:
        bad.append(f"no {kind} among {names}")
    store = d.get("tls_store")
    if not WIN and (not store or store.startswith("/opt/")):   # the system's store, never the build environment's
        bad.append(f"verified with {store!r}, not the system's certificate store")
    newest = d.get("version")
    step("newer", bad, f"{newest} offered to 0.0.1: {', '.join(names)}; verified with {store}")

    if newest:
        rc, res, tail = w("check-update", "--installed", newest, "--channel", "prerelease")
        d = res.get("data") or {}
        bad = refused_net(d, "the update server was not reached")
        if rc != 0 or not d.get("available") or d.get("newer") or d.get("version") != newest:
            bad.append(f"exit {rc}: {json.dumps(res)[:300] or tail}")
        step("current", bad, f"{newest} installed: not newer")
    else:
        step("current", ["no release to compare with (the newer step failed)"])

    if not WIN:   # Linux: an explicit SSL_CERT_FILE is used as given (beta.4's ca_store) -- an empty one trusts nothing
        empty = os.path.join(work, "empty-ca.pem")
        open(empty, "w").close()
        r = subprocess.run([worker, "--worker", "check-update", "--installed", "0.0.1", "--channel", "prerelease"],
                           cwd=work, env=dict(env, SSL_CERT_FILE=empty), capture_output=True, text=True, timeout=300)
        res = result(r.stdout)
        os.remove(empty)
        step("env", [] if res.get("code") == "tls" else [f"SSL_CERT_FILE=<empty file>: {json.dumps(res)[:300]}"],
             "SSL_CERT_FILE honoured: tls")

    if not download:
        out.append(("download", ["not done (--no-download)"]))
        report("NOT DONE: live download (--no-download)")
        return out
    root = os.path.join(work, "live-root")
    dest = os.path.join(work, "live-download")
    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(root, exist_ok=True)
    try:
        rc, res, tail = w("download-update", "--install", root, "--dest", dest, "--installed", "0.0.1",
                          "--channel", "prerelease", timeout=1800)
        d = res.get("data") or {}
        bad = refused_net(res, "the download failed")
        path = d.get("path") or ""
        if rc != 0 or not path or not os.path.isfile(path):
            bad.append(f"exit {rc}: {json.dumps(res)[:300] or tail}")
        else:
            with open(os.path.join(dest, "release.json"), encoding="utf-8") as f:
                rec = [a for a in json.load(f)["assets"] if a["name"] == os.path.basename(path)]
            if len(rec) != 1 or rec[0]["size"] != os.path.getsize(path) or rec[0]["sha256"] != sha(path):
                bad.append(f"{os.path.basename(path)} is not the signed manifest's record ({rec})")
            elif not path.endswith({"zip": ".zip", "tar.xz": ".tar.xz", "appimage": ".AppImage"}[kind]):
                bad.append(f"{os.path.basename(path)}: not this package's kind ({kind})")
        secs = d.get("seconds")
        step("download", bad, f"{os.path.basename(path)}, {(d.get('size') or 0) >> 20} MB in "
                              f"{secs.get('download') if isinstance(secs, dict) else secs} s")
    finally:
        shutil.rmtree(dest, ignore_errors=True)
        shutil.rmtree(root, ignore_errors=True)
    return out


def package_programs(package, work):
    """(cli, worker, appimage) for a package folder or an AppImage (extracted into work for its worker)"""
    exe = ".exe" if WIN else ""
    if package.endswith(".AppImage"):
        ext = os.path.join(work, "live-extracted")
        shutil.rmtree(ext, ignore_errors=True)
        os.makedirs(ext)
        subprocess.run([package, "--appimage-extract"], cwd=ext, capture_output=True, check=True,
                       env=clean_env())
        return [package], os.path.join(ext, "squashfs-root", "worker", "DotHeroes-Setup"), package
    return [os.path.join(package, "CubicHeroes" + exe)], os.path.join(package, "worker", "DotHeroes-Setup" + exe), None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--package", required=True)
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "qual", "live"))
    ap.add_argument("--no-download", action="store_true")
    a = ap.parse_args()
    package, work = os.path.abspath(a.package), os.path.abspath(a.work)
    os.makedirs(work, exist_ok=True)
    try:
        cli, worker, appimage = package_programs(package, work)
        res = run_checks(cli, worker, work, appimage, download=not a.no_download)
    finally:
        shutil.rmtree(os.path.join(work, "live-extracted"), ignore_errors=True)
    failed = [n for n, b in res if b]
    v = verdict(res)
    print(f"{v}: {len(res) - len(failed)} of {len(res)} live steps passed" +
          (f"; failed: {', '.join(failed)}" if failed else ""))
    return {"PASS": 0, "FAIL": 1}.get(v, 2)


if __name__ == "__main__":
    sys.exit(main())
