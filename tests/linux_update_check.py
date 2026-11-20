#!/usr/bin/env python3
"""Signed launcher updates on Linux, from the package (M11.4: the owner's closing list).

    python3 tests/linux_update_check.py --package DIR [--work DIR]

DIR: the Linux package's folder (build/dist/DotHeroes-Setup-linux-x86_64). A copy of it is the installation root (a
tarball's); its own frozen worker answers everything (never this checkout's). Releases come from a local mirror in
GitHub's API's shape, signed with a throwaway Ed25519 key made for the run and trusted only through --trusted-keys --
the project's release key is never involved. Cases:
  1. a signed newer release with a tar.xz and an AppImage: offered; the tarball install downloads the tar.xz, an
     AppImage install (APPIMAGE set) the AppImage; each checked against the manifest
  2. its tar.xz staged: unpacked inside the root, CubicHeroes still executable; the root's own items untouched
  3. the swap: the staged launcher in place, started (--after-update, SDL's offscreen video: no display), healthy, kept
  4. a manifest whose signature does not match: refused
  5. a manifest signed by a key the launcher does not trust: refused
  6. a release with only a Windows package: nothing offered to Linux to download
  7. a download whose bytes are not the manifest's: refused, nothing kept
  8. a zip handed to Linux's staging (another system's package): refused
Exit 1 if any case fails. Offscreen and off the network (the mirror is a folder).
"""
import argparse
import base64
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import desktop_guard  # noqa: E402
import release_trust as trust  # noqa: E402

NEW = "9.9.9-test.1"
results = []


def case(name, bad):
    results.append((name, bad))
    print(("PASS" if not bad else "FAIL") + f": {name}" + (f" -- {'; '.join(bad)}" if bad else ""), flush=True)


def openssl(*args, **kw):
    return subprocess.run(["openssl", *args], check=True, capture_output=True, **kw)


def make_key(work, name):
    k = os.path.join(work, name + ".pem")
    openssl("genpkey", "-algorithm", "ed25519", "-out", k)
    der = openssl("pkey", "-in", k, "-pubout", "-outform", "DER").stdout
    pk = trust.public_key_from(der)
    return k, trust.key_id(pk), pk


def sha(p):
    return hashlib.sha256(open(p, "rb").read()).hexdigest()


class Mirror:
    def __init__(self, work):
        self.dir = os.path.join(work, "mirror")
        os.makedirs(self.dir)
        self.list = []

    def release(self, tag, files, key, kid, assets=None, tamper=False, prerelease=True):
        d = os.path.join(self.dir, tag)
        os.makedirs(d)
        recs, links = [], []
        for path, platform, kind in files:
            dst = os.path.join(d, os.path.basename(path))
            shutil.copy2(path, dst)
            recs.append({"name": os.path.basename(path), "platform": platform, "kind": kind,
                         "size": os.path.getsize(dst), "sha256": sha(dst)})
            links.append({"name": os.path.basename(path), "browser_download_url": dst})
        m = {"format": 1, "version": tag.lstrip("v"), "channel": "prerelease", "min_launcher": None,
             "published": "2026-11-18T00:00:00+00:00", "notes": "test", "key_id": kid, "assets": assets or recs}
        mp = os.path.join(d, "release.json")
        with open(mp, "w") as f:
            json.dump(m, f, indent=1)
        openssl("pkeyutl", "-sign", "-inkey", key, "-rawin", "-in", mp, "-out", mp + ".sig")
        if tamper:
            with open(mp, "a") as f:
                f.write("\n")
        links += [{"name": "release.json", "browser_download_url": mp},
                  {"name": "release.json.sig", "browser_download_url": mp + ".sig"}]
        self.list.insert(0, {"tag_name": tag, "prerelease": prerelease, "draft": False, "assets": links})
        with open(os.path.join(self.dir, "releases.json"), "w") as f:
            json.dump(self.list, f)
        return m

    def reset(self):
        shutil.rmtree(self.dir)
        os.makedirs(self.dir)
        self.list = []


def main():
    ap = argparse.ArgumentParser(description="signed launcher updates on Linux, from the package")
    ap.add_argument("--package", required=True)
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "qual", "updates"))
    a = ap.parse_args()
    work = os.path.abspath(a.work)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    root = os.path.join(work, "Cubic Update Test é")
    shutil.copytree(os.path.abspath(a.package), root, symlinks=True)
    os.makedirs(os.path.join(root, "data", "gamedata"))
    with open(os.path.join(root, "data", "gamedata", "my save.bin"), "wb") as f:
        f.write(b"a player's save")
    worker = os.path.join(root, "worker", "DotHeroes-Setup")
    env = desktop_guard.no_display(dict(os.environ))
    env.pop("APPIMAGE", None)
    key, kid, pk = make_key(work, "test")
    other, other_id, _ = make_key(work, "other")
    keys = os.path.join(work, "keys.json")
    with open(keys, "w") as f:
        json.dump({"keys": [{"id": kid, "ed25519": base64.b64encode(pk).decode()}]}, f)

    def w(*args, extra_env=None):
        r = subprocess.run([worker, "--worker", *args, "--source", mirror.dir, "--trusted-keys", keys,
                            "--installed", "0.2.0-beta.2", "--channel", "prerelease"] if args[0] in (
            "check-update", "download-update") else [worker, "--worker", *args], capture_output=True, text=True,
            env=dict(env, **(extra_env or {})), timeout=600)
        res = None
        for ln in r.stdout.splitlines():
            try:
                e = json.loads(ln)
            except ValueError:
                continue
            if e.get("ev") == "result":
                res = e
        return r.returncode, res or {}

    # the new version's packages: the same files, a marker in README.txt; a tar.xz and an AppImage stand-in
    newdir = os.path.join(work, "new", "Cubic.Heroes")
    shutil.copytree(os.path.abspath(a.package), newdir, symlinks=True)
    with open(os.path.join(newdir, "README.txt"), "a") as f:
        f.write(f"\nversion {NEW} (the update test's)\n")
    tarball = os.path.join(work, f"Cubic.Heroes-{NEW}-linux-x86_64.tar.xz")
    with tarfile.open(tarball, "w:xz", preset=1) as t:
        t.add(newdir, arcname="Cubic.Heroes")
    appimage = os.path.join(work, f"Cubic.Heroes-{NEW}-x86_64.AppImage")
    with open(appimage, "wb") as f:
        f.write(b"an AppImage stand-in for the download test")
    zipped = os.path.join(work, f"Cubic.Heroes-{NEW}-windows-x86_64.zip")
    with open(zipped, "wb") as f:
        f.write(b"PK\x05\x06" + b"\0" * 18)

    mirror = Mirror(work)
    mirror.release("v" + NEW, [(tarball, "linux-x86_64", "tar.xz"), (appimage, "linux-x86_64", "appimage"),
                               (zipped, "windows-x86_64", "zip")], key, kid)
    rc, r = w("check-update")
    d = r.get("data", {})
    case("1a. a signed newer release offered", [] if rc == 0 and d.get("available") and d.get("newer") and
         {x["name"] for x in d.get("assets", [])} == {os.path.basename(tarball), os.path.basename(appimage)} else [f"{rc} {r}"])
    rc, r = w("download-update", "--install", root)
    got = r.get("data", {}).get("path", "")
    case("1b. the tarball install downloads the tar.xz, checked", [] if rc == 0 and got.endswith(".tar.xz") and
         sha(got) == sha(tarball) else [f"{rc} {r}"])
    rc2, r2 = w("download-update", "--install", root, "--dest", os.path.join(work, "dl-appimage"),
                extra_env={"APPIMAGE": os.path.join(work, "Cubic.Heroes.AppImage")})
    case("1c. an AppImage install downloads the AppImage", [] if rc2 == 0 and
         r2.get("data", {}).get("path", "").endswith(".AppImage") else [f"{rc2} {r2}"])

    before = {n: sha(os.path.join(root, "data", "gamedata", n)) for n in os.listdir(os.path.join(root, "data", "gamedata"))}
    rc, r = w("stage-launcher", "--package", got, "--launcher", root)
    staged = r.get("data", {}).get("staged", "")
    helper = r.get("data", {}).get("helper")
    bad = [] if rc == 0 and staged and os.access(os.path.join(staged, "CubicHeroes"), os.X_OK) and helper else [f"{rc} {r}"]
    case("2. staged inside the root, CubicHeroes executable, the worker's helper copied", bad)

    # 3. the swap, as the launcher starts it when it exits: the new launcher comes up offscreen (no display)
    swap_env = dict(env, SDL_VIDEODRIVER="offscreen")
    # the helper's output to a file, not a pipe: the launcher it starts inherits the descriptor and stays up
    swap_out = os.path.join(work, "swap.out")
    with open(swap_out, "w") as so:
        subprocess.run([helper, "--worker", "swap-launcher", "--launcher", root, "--start", "CubicHeroes",
                        "--health-timeout", "60"], stdout=so, stderr=subprocess.STDOUT, text=True, env=swap_env,
                       timeout=300)
    out_text = open(swap_out, encoding="utf-8", errors="replace").read()
    p = type("P", (), {"stdout": out_text})()
    res = [json.loads(ln) for ln in p.stdout.splitlines() if ln.startswith("{")]
    out = (res[-1].get("data") or {}).get("result") if res else None
    readme = open(os.path.join(root, "README.txt")).read()
    after = {n: sha(os.path.join(root, "data", "gamedata", n)) for n in os.listdir(os.path.join(root, "data", "gamedata"))}
    subprocess.run(["pkill", "-f", os.path.join(root, "CubicHeroes")], capture_output=True)   # the new launcher, up
    case("3. swapped: the new launcher in place and healthy, the root's own files untouched",
         [] if out == "updated" and NEW in readme and after == before else [f"result {out}", f"readme new: {NEW in readme}",
                                                                           f"saves same: {after == before}", p.stdout[-300:]])

    mirror.reset()
    mirror.release("v" + NEW, [(tarball, "linux-x86_64", "tar.xz")], key, kid, tamper=True)
    rc, r = w("check-update")
    d = r.get("data", {})
    case("4. a manifest whose signature does not match: refused",
         [] if not d.get("available") and any(x.get("code") == "bad_signature" for x in d.get("refused", [])) else [f"{rc} {r}"])

    mirror.reset()
    mirror.release("v" + NEW, [(tarball, "linux-x86_64", "tar.xz")], other, other_id)
    rc, r = w("check-update")
    d = r.get("data", {})
    case("5. a manifest signed by an untrusted key: refused",
         [] if not d.get("available") and d.get("refused") else [f"{rc} {r}"])

    mirror.reset()
    mirror.release("v" + NEW, [(zipped, "windows-x86_64", "zip")], key, kid)
    rc, r = w("download-update", "--install", root, "--dest", os.path.join(work, "dl-win"))
    case("6. a release with only a Windows package: nothing for Linux", [] if rc != 0 and r.get("code") == "no_release"
         and not os.path.exists(os.path.join(work, "dl-win", os.path.basename(zipped))) else [f"{rc} {r}"])

    mirror.reset()
    m = mirror.release("v" + NEW, [(tarball, "linux-x86_64", "tar.xz")], key, kid,
                       assets=[{"name": os.path.basename(tarball), "platform": "linux-x86_64", "kind": "tar.xz",
                                "size": os.path.getsize(tarball), "sha256": "0" * 64}])
    rc, r = w("download-update", "--install", root, "--dest", os.path.join(work, "dl-bad"))
    left = os.listdir(os.path.join(work, "dl-bad")) if os.path.isdir(os.path.join(work, "dl-bad")) else []
    case("7. a download not the manifest's: refused, nothing kept", [] if rc != 0 and
         not [n for n in left if n.endswith(".tar.xz")] else [f"{rc} {r} left {left}"])

    rc, r = w("stage-launcher", "--package", zipped, "--launcher", root)
    case("8. a zip (another system's package) refused by Linux's staging", [] if rc != 0 and
         r.get("code") == "bad_download" else [f"{rc} {r}"])

    failed = [n for n, b in results if b]
    print(f"{len(results) - len(failed)} of {len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
