#!/usr/bin/env python3
"""Signed launcher updates on Linux, from the package (M11.4: the owner's closing list).

    python3 tests/linux_update_check.py --package DIR [--work DIR] [--https] [--new-package TAR.XZ] [--legacy-tls]

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
  9. a staged launcher that does not start: put back -- the previous launcher's items as they were, saves untouched
 10. the update server unreachable (a closed port): offline, nothing offered
With --https (beta.4) the mirror is served over HTTPS from 127.0.0.1 with a throwaway CA made for the run, which the
worker trusts only through SSL_CERT_FILE (as a player could set it); cases 1-9 go through it, and:
 11. the same server without SSL_CERT_FILE (the system's store doesn't know the test CA): refused, "tls"
 12. an expired certificate (the test CA trusted): refused, "tls"
 13. another host's certificate (the test CA trusted): refused, "tls"
 14. the download cut off part-way: reported, then resumed by the next download and checked
--new-package: the update is that real package (the next version's tar.xz) instead of a copy of DIR with a marker --
DIR can then be the previous release's (beta.3's folder: "starting from beta.3"). --legacy-tls: the installed worker
predates the "tls" result (beta.3): 11-13 pass when the request is refused as offline (still nothing fetched).
Exit 1 if any case fails. Offscreen; off the network (the mirror is a folder, or 127.0.0.1).
"""
import argparse
import base64
import hashlib
import http.server
import json
import os
import re
import shutil
import socket
import ssl
import subprocess
import sys
import tarfile
import threading
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


class Served(http.server.BaseHTTPRequestHandler):
    """the mirror's folder over HTTP(S), with Range; a file named in `cut` is cut off part-way at its first full GET"""
    root, cut = "", {}

    def do_GET(self):
        p = os.path.join(self.root, *[x for x in self.path.split("?")[0].split("/") if x not in ("", ".", "..")])
        if not os.path.isfile(p):
            self.send_error(404)
            return
        with open(p, "rb") as f:
            data = f.read()
        rng = self.headers.get("Range")
        start = int(rng.split("=")[1].split("-")[0]) if rng else 0
        body = data[start:]
        self.send_response(206 if rng else 200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        name = os.path.basename(p)
        if not rng and self.cut.get(name):
            self.cut[name] = 0
            self.wfile.write(body[:len(body) // 3])
            self.wfile.flush()
            self.connection.shutdown(socket.SHUT_RDWR)
            return
        self.wfile.write(body)

    def log_message(self, *a):
        pass


class Tls:
    """a throwaway CA and HTTPS servers for 127.0.0.1 over one folder (tests/test_update_tls.py's Pki: OpenSSL's
    command line)"""

    def __init__(self, work, folder):
        from test_update_tls import Pki
        self.dir = os.path.join(work, "pki")
        os.makedirs(self.dir)
        Served.root = folder
        self.pki = Pki(self.dir)
        self.ca = self.pki.ca
        self.servers = []

    def cert(self, name, **kw):
        c = self.pki.server(name, **kw)
        if not c:
            sys.exit(f"openssl could not make the {name} certificate")
        return c

    def serve(self, cert):
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Served)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(*cert)
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
        srv.handle_error = lambda *a: None   # the handshakes refused on purpose
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.servers.append(srv)
        return f"https://127.0.0.1:{srv.server_address[1]}"

    def close(self):
        for srv in self.servers:
            srv.shutdown()
            srv.server_close()


class Mirror:
    def __init__(self, work, base=None):
        self.dir = os.path.join(work, "mirror")
        os.makedirs(self.dir)
        self.list = []
        self.base = base   # https://127.0.0.1:PORT: the links and the list served over HTTPS; None: the folder
        self.source = f"{base}/releases.json" if base else self.dir

    def link(self, path):
        return f"{self.base}/{os.path.relpath(path, self.dir).replace(os.sep, '/')}" if self.base else path

    def release(self, tag, files, key, kid, assets=None, tamper=False, prerelease=True):
        d = os.path.join(self.dir, tag)
        os.makedirs(d)
        recs, links = [], []
        for path, platform, kind in files:
            dst = os.path.join(d, os.path.basename(path))
            shutil.copy2(path, dst)
            recs.append({"name": os.path.basename(path), "platform": platform, "kind": kind,
                         "size": os.path.getsize(dst), "sha256": sha(dst)})
            links.append({"name": os.path.basename(path), "browser_download_url": self.link(dst)})
        m = {"format": 1, "version": tag.lstrip("v"), "channel": "prerelease", "min_launcher": None,
             "published": "2026-11-18T00:00:00+00:00", "notes": "test", "key_id": kid, "assets": assets or recs}
        mp = os.path.join(d, "release.json")
        with open(mp, "w") as f:
            json.dump(m, f, indent=1)
        openssl("pkeyutl", "-sign", "-inkey", key, "-rawin", "-in", mp, "-out", mp + ".sig")
        if tamper:
            with open(mp, "a") as f:
                f.write("\n")
        links += [{"name": "release.json", "browser_download_url": self.link(mp)},
                  {"name": "release.json.sig", "browser_download_url": self.link(mp + ".sig")}]
        self.list.insert(0, {"tag_name": tag, "prerelease": prerelease, "draft": False, "assets": links})
        with open(os.path.join(self.dir, "releases.json"), "w") as f:
            json.dump(self.list, f)
        return m

    def reset(self):
        shutil.rmtree(self.dir)
        os.makedirs(self.dir)
        self.list = []


def launcher_items(root):
    """the launcher package's items in a folder, file by file (links as their targets): what a swap replaces"""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import install_layout
    out = {}
    for n in install_layout.launcher_package():
        p = os.path.join(root, n)
        if os.path.isfile(p):
            out[n] = sha(p)
        for d, dirs, files in os.walk(p) if os.path.isdir(p) else ():
            for f in files:
                q = os.path.join(d, f)
                out[os.path.relpath(q, root)] = ("link", os.readlink(q)) if os.path.islink(q) else sha(q)
    return out


def saves(root):
    g = os.path.join(root, "data", "gamedata")
    return {os.path.relpath(os.path.join(d, f), g): sha(os.path.join(d, f)) for d, _, fs in os.walk(g) for f in fs}


def main():
    ap = argparse.ArgumentParser(description="signed launcher updates on Linux, from the package")
    ap.add_argument("--package", required=True)
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "qual", "updates"))
    ap.add_argument("--https", action="store_true", help="the mirror over HTTPS (a throwaway CA) and the TLS cases")
    ap.add_argument("--new-package", help="the update: this real tar.xz (the next version's) instead of a marked copy")
    ap.add_argument("--legacy-tls", action="store_true",
                    help="the installed worker predates the tls result (beta.3): refused-as-offline passes 11-13")
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
    for k in ("APPIMAGE", "SSL_CERT_FILE", "SSL_CERT_DIR", "HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy",
              "ALL_PROXY", "all_proxy"):
        env.pop(k, None)
    key, kid, pk = make_key(work, "test")
    other, other_id, _ = make_key(work, "other")
    keys = os.path.join(work, "keys.json")
    with open(keys, "w") as f:
        json.dump({"keys": [{"id": kid, "ed25519": base64.b64encode(pk).decode()}]}, f)

    tls = Tls(work, os.path.join(work, "mirror")) if a.https else None
    trusted = {"SSL_CERT_FILE": tls.ca} if tls else {}

    def w(*args, extra_env=None, source=None, trust_ca=True):
        e = dict(env, **(trusted if trust_ca else {}), **(extra_env or {}))
        r = subprocess.run([worker, "--worker", *args, "--source", source or mirror.source, "--trusted-keys", keys,
                            "--installed", "0.2.0-beta.2", "--channel", "prerelease"] if args[0] in (
            "check-update", "download-update") else [worker, "--worker", *args], capture_output=True, text=True,
            env=e, timeout=600)
        res = None
        for ln in r.stdout.splitlines():
            try:
                ev = json.loads(ln)
            except ValueError:
                continue
            if ev.get("ev") == "result":
                res = ev
        return r.returncode, res or {}

    # the new version's packages: a real one (--new-package), or the same files with a marker in README.txt; an
    # AppImage stand-in and a Windows zip beside it
    if a.new_package:
        tarball = os.path.abspath(a.new_package)
        m = re.search(r"Cubic\.Heroes-(.+)-linux-x86_64\.tar\.xz$", os.path.basename(tarball))
        new = m.group(1) if m else NEW
        newdir = os.path.join(work, "new")
        os.makedirs(newdir)
        with tarfile.open(tarball) as t:
            t.extractall(newdir, filter="data")
        newdir = os.path.join(newdir, "Cubic.Heroes")
    else:
        new = NEW
        newdir = os.path.join(work, "new", "Cubic.Heroes")
        shutil.copytree(os.path.abspath(a.package), newdir, symlinks=True)
        with open(os.path.join(newdir, "README.txt"), "a") as f:
            f.write(f"\nversion {new} (the update test's)\n")
        tarball = os.path.join(work, f"Cubic.Heroes-{new}-linux-x86_64.tar.xz")
        with tarfile.open(tarball, "w:xz", preset=1) as t:
            t.add(newdir, arcname="Cubic.Heroes")
    appimage = os.path.join(work, f"Cubic.Heroes-{new}-x86_64.AppImage")
    with open(appimage, "wb") as f:
        f.write(b"an AppImage stand-in for the download test")
    zipped = os.path.join(work, f"Cubic.Heroes-{new}-windows-x86_64.zip")
    with open(zipped, "wb") as f:
        f.write(b"PK\x05\x06" + b"\0" * 18)

    mirror = Mirror(work, tls.serve(tls.cert("good")) if tls else None)
    try:
        run_cases(a, w, mirror, tls, root, work, env, new, newdir, tarball, appimage, zipped, key, kid, other,
                  other_id)
    finally:
        if tls:
            tls.close()
    failed = [n for n, b in results if b]
    print(f"{len(results) - len(failed)} of {len(results)} passed")
    return 1 if failed else 0


def run_cases(a, w, mirror, tls, root, work, env, new, newdir, tarball, appimage, zipped, key, kid, other, other_id):
    mirror.release("v" + new, [(tarball, "linux-x86_64", "tar.xz"), (appimage, "linux-x86_64", "appimage"),
                               (zipped, "windows-x86_64", "zip")], key, kid)
    rc, r = w("check-update")
    d = r.get("data", {})
    case("1a. a signed newer release offered" + (" (HTTPS)" if tls else ""),
         [] if rc == 0 and d.get("available") and d.get("newer") and d.get("version") == new and
         {x["name"] for x in d.get("assets", [])} == {os.path.basename(tarball), os.path.basename(appimage)}
         else [f"{rc} {r}"])
    rc, r = w("download-update", "--install", root)
    got = r.get("data", {}).get("path", "")
    case("1b. the tarball install downloads the tar.xz, checked", [] if rc == 0 and got.endswith(".tar.xz") and
         sha(got) == sha(tarball) else [f"{rc} {r}"])
    rc2, r2 = w("download-update", "--install", root, "--dest", os.path.join(work, "dl-appimage"),
                extra_env={"APPIMAGE": os.path.join(work, "Cubic.Heroes.AppImage")})
    case("1c. an AppImage install downloads the AppImage", [] if rc2 == 0 and
         r2.get("data", {}).get("path", "").endswith(".AppImage") else [f"{rc2} {r2}"])

    before = saves(root)
    rc, r = w("stage-launcher", "--package", got, "--launcher", root)
    staged = r.get("data", {}).get("staged", "")
    helper = r.get("data", {}).get("helper")
    bad = [] if rc == 0 and staged and os.access(os.path.join(staged, "CubicHeroes"), os.X_OK) and helper else [f"{rc} {r}"]
    case("2. staged inside the root, CubicHeroes executable, the worker's helper copied", bad)

    def swap(helper):
        # the swap, as the launcher starts it when it exits: the new launcher comes up offscreen (no display). The
        # helper's output to a file, not a pipe: the launcher it starts inherits the descriptor and stays up
        swap_out = os.path.join(work, "swap.out")
        with open(swap_out, "w") as so:
            subprocess.run([helper, "--worker", "swap-launcher", "--launcher", root, "--start", "CubicHeroes",
                            "--health-timeout", "60"], stdout=so, stderr=subprocess.STDOUT, text=True,
                           env=dict(env, SDL_VIDEODRIVER="offscreen"), timeout=300)
        text = open(swap_out, encoding="utf-8", errors="replace").read()
        res = [json.loads(ln) for ln in text.splitlines() if ln.startswith("{")]
        subprocess.run(["pkill", "-f", os.path.join(root, "CubicHeroes")], capture_output=True)   # the launcher, up
        return ((res[-1].get("data") or {}).get("result") if res else None), text

    # 3. swapped: the root's launcher items are the new package's, byte for byte; the saves untouched
    out, text = swap(helper)
    want = launcher_items(newdir)
    have = launcher_items(root)
    diff = sorted(k for k in set(want) | set(have) if want.get(k) != have.get(k))
    after = saves(root)
    case("3. swapped: the new launcher in place and healthy, the root's own files untouched",
         [] if out == "updated" and not diff and after == before else
         [f"result {out}", f"launcher items not the new package's: {diff[:5]}", f"saves same: {after == before}",
          text[-300:]])

    mirror.reset()
    mirror.release("v" + new, [(tarball, "linux-x86_64", "tar.xz")], key, kid, tamper=True)
    rc, r = w("check-update")
    d = r.get("data", {})
    case("4. a manifest whose signature does not match: refused",
         [] if not d.get("available") and any(x.get("code") == "bad_signature" for x in d.get("refused", [])) else [f"{rc} {r}"])

    mirror.reset()
    mirror.release("v" + new, [(tarball, "linux-x86_64", "tar.xz")], other, other_id)
    rc, r = w("check-update")
    d = r.get("data", {})
    case("5. a manifest signed by an untrusted key: refused",
         [] if not d.get("available") and d.get("refused") else [f"{rc} {r}"])

    mirror.reset()
    mirror.release("v" + new, [(zipped, "windows-x86_64", "zip")], key, kid)
    rc, r = w("download-update", "--install", root, "--dest", os.path.join(work, "dl-win"))
    case("6. a release with only a Windows package: nothing for Linux", [] if rc != 0 and r.get("code") == "no_release"
         and not os.path.exists(os.path.join(work, "dl-win", os.path.basename(zipped))) else [f"{rc} {r}"])

    mirror.reset()
    mirror.release("v" + new, [(tarball, "linux-x86_64", "tar.xz")], key, kid,
                   assets=[{"name": os.path.basename(tarball), "platform": "linux-x86_64", "kind": "tar.xz",
                            "size": os.path.getsize(tarball), "sha256": "0" * 64}])
    rc, r = w("download-update", "--install", root, "--dest", os.path.join(work, "dl-bad"))
    left = os.listdir(os.path.join(work, "dl-bad")) if os.path.isdir(os.path.join(work, "dl-bad")) else []
    case("7. a download not the manifest's: refused, nothing kept", [] if rc != 0 and
         not [n for n in left if n.endswith(".tar.xz")] else [f"{rc} {r} left {left}"])

    rc, r = w("stage-launcher", "--package", zipped, "--launcher", root)
    case("8. a zip (another system's package) refused by Linux's staging", [] if rc != 0 and
         r.get("code") == "bad_download" else [f"{rc} {r}"])

    # 9. a launcher that does not start: the swap puts the previous one back (its items byte for byte)
    broken = os.path.join(work, "broken", "Cubic.Heroes")
    shutil.copytree(newdir, broken, symlinks=True)
    with open(os.path.join(broken, "CubicHeroes"), "w") as f:
        f.write("#!/bin/sh\nexit 1\n")
    os.chmod(os.path.join(broken, "CubicHeroes"), 0o755)
    broken_tar = os.path.join(work, f"Cubic.Heroes-{new}-broken-linux-x86_64.tar.xz")
    with tarfile.open(broken_tar, "w:xz", preset=1) as t:
        t.add(broken, arcname="Cubic.Heroes")
    before_items, before = launcher_items(root), saves(root)
    rc, r = w("stage-launcher", "--package", broken_tar, "--launcher", root)
    helper = (r.get("data") or {}).get("helper")
    out, text = swap(helper) if rc == 0 and helper else (None, json.dumps(r))
    now = launcher_items(root)
    diff = sorted(k for k in set(before_items) | set(now) if before_items.get(k) != now.get(k))
    case("9. a launcher that does not start: the previous one put back, the saves untouched",
         [] if out == "restored" and not diff and saves(root) == before else
         [f"result {out}", f"items changed: {diff[:5]}", text[-300:]])

    with socket.socket() as sk:
        sk.bind(("127.0.0.1", 0))
        port = sk.getsockname()[1]
    dead = f"{'https' if tls else 'http'}://127.0.0.1:{port}/releases.json"
    rc, r = w("check-update", source=dead)
    d = r.get("data", {})
    case("10. the update server unreachable: offline, nothing offered",
         [] if rc == 0 and d.get("offline") and not d.get("available") else [f"{rc} {r}"])

    if not tls:
        return
    mirror.reset()
    mirror.release("v" + new, [(tarball, "linux-x86_64", "tar.xz")], key, kid)

    def refused_tls(rc, r, name, word=None):
        why = r.get("message") or (r.get("data") or {}).get("why") or ""
        if r.get("code") == "tls" and (not word or word in why):
            return []
        if a.legacy_tls and (r.get("data") or {}).get("offline") and not (r.get("data") or {}).get("available"):
            return []
        return [f"{rc} {r}"]

    rc, r = w("check-update", trust_ca=False)
    case("11. the server's CA not trusted (no SSL_CERT_FILE): refused as tls", refused_tls(rc, r, "11"))
    expired = tls.serve(tls.cert("expired", expired=True))
    rc, r = w("check-update", source=f"{expired}/releases.json")
    case("12. an expired certificate: refused as tls", refused_tls(rc, r, "12", "expired"))
    other_host = tls.serve(tls.cert("other", san="DNS:example.invalid"))
    rc, r = w("check-update", source=f"{other_host}/releases.json")
    case("13. another host's certificate: refused as tls", refused_tls(rc, r, "13"))

    Served.cut = {os.path.basename(tarball): 1}
    dest = os.path.join(work, "dl-cut")
    rc, r = w("download-update", "--install", root, "--dest", dest)
    first = r.get("code")
    rc2, r2 = w("download-update", "--install", root, "--dest", dest)
    got = (r2.get("data") or {}).get("path", "")
    case("14. a download cut off part-way: reported, then resumed and checked",
         [] if rc != 0 and first in ("interrupted", "bad_download") and rc2 == 0 and got and sha(got) == sha(tarball)
         else [f"first {rc} {r}", f"then {rc2} {r2}"])


if __name__ == "__main__":
    sys.exit(main())
