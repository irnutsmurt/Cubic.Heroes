"""The Character Store's pictures in the launcher (--nav-test, hidden): the loading blocks while a thumbnail is fetched
or decoded, a placeholder when it can't be had, the cards on screen fetched first, and nothing fetched twice.

A copy of the store's repository is made for the test: made-up characters with generated thumbnails, a catalog signed
with a key made for the test (CUBICHEROES_STORE_KEYS, honoured only for a copy on this computer), served over http on
127.0.0.1 by a server that can be slow, refuse some thumbnails, drop the connection or be down, and that records
every request. Two thumbnails on the first screen are refused (one missing, one not matching the catalog).

The launcher logs the store's pictures with each step (--nav-test): the cards drawn by state (shown, loading,
unavailable), the loading blocks drawn (most in one frame since the step before, fetched and decoded apart), whether a
fetch runs, how many were refused, whether a fetch stopped.

  first    an empty cache: the blocks while the first screen is fetched; the first screen asked for first; pictures
           shown before the fetch ends; the refused two get the placeholder; the cards reached by the D-pad while
           pictures load, and a card's details opened and closed; every thumbnail fetched once
  return   (the same launcher) another tab and back, another page and back: everything shown at once, no blocks,
           nothing fetched again
  reopen   a new launcher, the cache full: shown at once, never the decoding blocks; only the refused two asked again
  offline  the cache full, no connection (the catalog's check due): the cached shown, the others the placeholder,
           no blocks, nothing left turning
  drop     an empty cache, the connection lost after a few thumbnails: those shown, the rest the placeholder (no
           retry loop), tried again once when the store is shown again
  scroll   an empty cache, slow thumbnails, the D-pad down to the catalog's last row: the last screen's pictures asked
           for together, ahead of pictures before them in the store's order (however long the scroll takes: the
           hidden launcher's frames come fast on Linux, at the display's rate on Windows); everything there in the end

Each runs at each of --sizes (1280x800 and 1280x720 by default). The launcher is a build or a package's (--launcher);
every test installation is made under --work and nothing else is written.

  py -3 tests/launcher_store_thumbs.py --launcher <CubicHeroes> [--only first return] [--sizes 1280x800]
"""
import argparse
import base64
import functools
import hashlib
import http.server
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import desktop_guard  # noqa: E402

HELPERS = ("CUBIC_INSTALL_DIR", "RECOMPLAUNCHER", "CUBIC_LAUNCHER", "CUBIC_DIRECT_PLAY")
SCENARIOS = ("first", "return", "reopen", "offline", "drop", "scroll")
N_CHARS = 96
REFUSED = (0, 2)            # Hero 000: missing in the repository; Hero 002: its bytes don't match the catalog
EVENT = re.compile(r"\[store pictures t=([\d.]+) shown=(\d+) loading=(\d+) unavailable=(\d+) spin=(\d+) "
                   r"spun=(\d+)/(\d+) fetching=(\d)\s+failed=(\d+)( stalled)?\]")


# -- the store's copy -------------------------------------------------------------------------------------------------
def thumb_png(i):
    """a made-up thumbnail in the renderer's own form (8-bit RGB, unfiltered rows): a block of its own colour"""
    import char_preview as P
    c = P.Canvas(96, 96, bg=(20 + i * 7 % 60, 30, 70))
    rgb = (60 + i * 53 % 190, 60 + i * 97 % 190, 60 + i * 31 % 190)
    c.polygon([(24, 16), (72, 16), (72, 80), (24, 80)], rgb)
    return c.png()


def make_store(d):
    """the repository's copy (N_CHARS characters, "Hero 000".. in A-Z order) and its keys file -> (repo, keys file,
    tampered {path: bytes})"""
    import char_catalog as C
    from test_char_store import TestKey, submission
    src = os.path.join(d, "src")
    repo = os.path.join(d, "repo")
    os.makedirs(src)
    added, problems = C.build(repo, [submission(src, "TEMPLATE", (0.2, 0.6, 0.3))], "2026-11-27", log=lambda *a: None)
    if len(added) != 1 or problems:
        sys.exit(f"the store's copy couldn't be made: {problems}")
    doc = json.load(open(C._path(repo), encoding="utf-8"))
    tpl = doc["catalog"]["characters"][0]
    chars, tampered = [], {}
    for i in range(N_CHARS):
        e = json.loads(json.dumps(tpl))
        cid = hashlib.sha256(f"store-thumbs test {i}".encode()).hexdigest()[:16]
        png = thumb_png(i)
        path = f"characters/{cid}/thumb.png"
        e.update(id=cid, label=f"Hero {i:03d}", name=f"HERO{i:03d}",
                 thumb={"path": path, "size": len(png), "sha256": hashlib.sha256(png).hexdigest()})
        chars.append(e)
        if i == REFUSED[0]:
            continue   # missing: a 404
        os.makedirs(os.path.join(repo, "characters", cid), exist_ok=True)
        with open(os.path.join(repo, *path.split("/")), "wb") as f:
            f.write(png)
        if i == REFUSED[1]:
            tampered[path] = png[:-8] + b"tampered"
    doc["catalog"]["characters"] = chars
    key = TestKey()
    C._save(repo, key.sign_doc(doc))
    keys = os.path.join(d, "keys.json")
    with open(keys, "w", encoding="utf-8") as f:
        json.dump({"keys": [{"id": key.id, "ed25519": base64.b64encode(key.public).decode()}]}, f)
    return repo, keys, tampered


class Server:
    """the repository's copy over http on 127.0.0.1: `delay` per request (`thumb_delay` more for a thumbnail),
    `tampered` bodies, the connection dropped
    after `drop_after` thumbnails (closed unanswered), or not listening at all (`down`). Every request is recorded
    (time, path, what was answered)."""

    def __init__(self, repo, tampered):
        self.repo, self.tampered = repo, tampered
        self.delay, self.thumb_delay, self.drop_after, self.thumbs = 0.0, 0.0, None, 0
        self.log, self.lock, self.srv = [], threading.Lock(), None
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        self.port = s.getsockname()[1]
        s.close()

    def up(self):
        if self.srv:
            return
        outer = self

        class H(http.server.SimpleHTTPRequestHandler):
            def do_GET(self):
                p = self.path.lstrip("/")
                thumb = p.endswith("/thumb.png")
                time.sleep(outer.delay + (outer.thumb_delay if thumb else 0.0))
                with outer.lock:
                    if thumb:
                        outer.thumbs += 1
                    dropped = thumb and outer.drop_after is not None and outer.thumbs > outer.drop_after
                    there = os.path.isfile(os.path.join(outer.repo, *p.split("/")))
                    outer.log.append((time.time(), p, "dropped" if dropped else "tampered" if p in outer.tampered
                                      else "file" if there else "404"))
                if dropped:
                    self.close_connection = True
                    self.connection.close()
                    return
                if p in outer.tampered:
                    body = outer.tampered[p]
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                super().do_GET()

            def log_message(self, *a):
                pass

        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", self.port), functools.partial(H, directory=self.repo))
        self.srv.daemon_threads = True
        self.srv.handle_error = lambda *a: None   # a client gone mid-answer (the launcher closed, a dropped connection)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def down(self):
        if self.srv:
            self.srv.shutdown()
            self.srv.server_close()
            self.srv = None

    def thumb_requests(self, since=0):
        return [(t, p.split("/")[1], what) for t, p, what in self.log[since:] if p.endswith("/thumb.png")]


# -- the launcher ------------------------------------------------------------------------------------------------------
TO_STORE = """wait 200
rb
wait 60
rb
wait 60
rb
wait 100
until 20000 card:|Import
rt
wait 30
rt
until 20000 card:
"""
# a few steps a while apart, the pictures' state logged with each (the hidden launcher's frames come fast; a
# window's at 60 a second)
SAMPLE = "wait 300\n" * 12


def run(launcher, inst, d, name, script, size, server, keys, timeout):
    """the script run by the hidden launcher -> (its log's lines, exit code)"""
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, name + ".nav")
    with open(path, "w", encoding="utf-8") as f:
        f.write(script)
    env = {k: v for k, v in os.environ.items() if k not in HELPERS}
    desktop_guard.launcher_env(env)
    env["HTTP_PROXY"] = env["HTTPS_PROXY"] = "http://127.0.0.1:9"   # nothing outside this computer: the copy only
    env["NO_PROXY"] = "127.0.0.1,localhost"
    env["CUBICHEROES_STORE_URL"] = f"http://127.0.0.1:{server.port}/"
    env["CUBICHEROES_STORE_KEYS"] = keys
    p = subprocess.Popen([launcher, "--install-dir", inst, "--nav-test", path, "--nav-size", size], cwd=d, env=env,
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    watch = desktop_guard.Watch(p, "the launcher (store pictures)").start()
    try:
        rc = p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        rc = "timeout"
    watch.stop()
    lines = []
    if os.path.exists(path + ".log"):
        with open(path + ".log", encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    if watch.violation:
        lines.append("FAILED: " + watch.violation)
    return lines, rc


def steps(lines):
    """[(op, its line, {the pictures' state} or None)]"""
    out = []
    for ln in lines:
        if " page=" not in ln:
            continue
        m = EVENT.search(ln)
        st = None
        if m:
            st = dict(t=float(m[1]), shown=int(m[2]), loading=int(m[3]), unavailable=int(m[4]), spin=int(m[5]),
                      spun_fetch=int(m[6]), spun_decode=int(m[7]), fetching=int(m[8]), failed=int(m[9]),
                      stalled=bool(m[10]))
        out.append((ln.split()[0], ln, st))
    return out


class Check:
    def __init__(self, name):
        self.name, self.problems, self.notes = name, [], []

    def that(self, ok, what):
        if not ok:
            self.problems.append(what)
        return ok

    def report(self):
        for p in self.problems or [f"{len(self.notes) and '; '.join(self.notes) or 'ok'}"]:
            print(("FAIL" if self.problems else "PASS") + f": {self.name}: {p}", flush=True)
        return not self.problems


def label_of(cid_index):
    return f"Hero {cid_index:03d}"


def common(c, lines, rc):
    bad = [ln for ln in lines if ln.startswith("FAILED") or " page=" not in ln]
    c.that(not bad, "the script stopped: " + "; ".join(bad[:2]))
    c.that(rc in (0, None), f"the launcher's exit: {rc}")


def last_state(sts):
    return next((st for _, _, st in reversed(sts) if st), None)


def scenario_first(a, size, work, server, keys, ids, inst):
    c = Check(f"first at {size}")
    server.delay = 0.15
    since = len(server.log)
    script = TO_STORE + """expect card: Hero 000
right
wait 10
expect card: Hero 001
down
wait 10
expect card:
a
wait 30
b
wait 30
expect card:
up
wait 10
expect card: Hero 001
""" + SAMPLE + """settle 300000
wait 120
expect card:
quit
"""
    lines, rc = run(a.launcher, inst, work, "first", script, size, server, keys, a.timeout)
    common(c, lines, rc)
    sts = steps(lines)
    seen = [st for _, _, st in sts if st]
    if not c.that(seen, "no step logged the store's pictures"):
        return c.report()
    end = seen[-1]
    drawn = end["shown"] + end["loading"] + end["unavailable"]
    c.that(any(st["spun_fetch"] > 0 for st in seen),
           "the loading blocks were never drawn while the first screen was fetched")
    c.that(any(st["fetching"] and st["shown"] > 0 for st in seen),
           "no picture was shown before the fetch ended (they appear one by one, not all at the end)")
    c.that(end["loading"] == 0 and end["spin"] == 0 and not end["fetching"],
           f"after the fetch: {end['loading']} still loading, {end['spin']} blocks drawn, fetching {end['fetching']}")
    c.that(end["unavailable"] == len(REFUSED), f"{end['unavailable']} placeholders on screen, {len(REFUSED)} expected")
    c.that(end["failed"] == len(REFUSED), f"{end['failed']} refused, {len(REFUSED)} expected")
    reqs = server.thumb_requests(since)
    per = {}
    for _, cid, _ in reqs:
        per[cid] = per.get(cid, 0) + 1
    c.that(len(per) == N_CHARS and all(n == 1 for n in per.values()),
           f"{len(reqs)} thumbnail requests for {len(per)} characters: each once expected "
           f"(twice: {[ids.index(k) for k, n in per.items() if n > 1][:6]})")
    order = [ids.index(cid) for _, cid, _ in reqs]
    c.that(all(i in order[:drawn + 6] for i in range(drawn)),
           f"the first asked for: {order[:drawn + 6]} (the first screen's {drawn} expected first)")
    shown_at = next((st["t"] for st in seen if st["shown"] > 0), None)
    c.notes.append(f"{drawn} cards on screen; the first picture {shown_at - seen[0]['t']:.2f} s after the store "
                   f"showed its cards, all fetched by {end['t'] - seen[0]['t']:.2f} s; {len(reqs)} thumbnails, each "
                   f"once" if shown_at is not None else f"{drawn} cards on screen; {len(reqs)} thumbnails, each once")
    return c.report()


def scenario_return(a, size, work, server, keys, ids, inst):
    c = Check(f"return at {size}")
    since = len(server.log)
    script = TO_STORE + """settle 300000
expect card: Hero 000
lt
wait 60
rt
until 20000 card:
wait 30
settle 300000
expect card:
lb
wait 60
rb
until 20000 card:
wait 30
settle 300000
expect card:
quit
"""
    lines, rc = run(a.launcher, inst, work, "return", script, size, server, keys, a.timeout)
    common(c, lines, rc)
    # after this launcher's first visit (the refused two asked for again, once), the returns: from another tab
    # (lt), then from another page (lb)
    returns, back = [], False
    for op, _, st in steps(lines):
        back = back or op == "lt"
        if back and st:
            returns.append(st)
    c.that(len(returns) >= 4, f"{len(returns)} steps back in the store logged its pictures")
    for st in returns:
        c.that(st["loading"] == 0 and st["spun_decode"] == 0 and st["spun_fetch"] == 0,
               f"back in the store: {st['loading']} loading, blocks drawn {st['spun_fetch']}/{st['spun_decode']}")
    again = [ids.index(cid) for _, cid, _ in server.thumb_requests(since)]
    c.that(sorted(again) == sorted(REFUSED), f"thumbnails asked for: {again} (only the refused {list(REFUSED)}, once)")
    return c.report()


def scenario_reopen(a, size, work, server, keys, ids, inst):
    c = Check(f"reopen at {size}")
    since = len(server.log)
    script = TO_STORE + """expect card: Hero 000
""" + SAMPLE[:len(SAMPLE) // 3] + """settle 300000
quit
"""
    lines, rc = run(a.launcher, inst, work, "reopen", script, size, server, keys, a.timeout)
    common(c, lines, rc)
    seen = [st for _, _, st in steps(lines) if st]
    if c.that(seen, "no step logged the store's pictures"):
        c.that(all(st["spun_decode"] == 0 for st in seen), "the decoding blocks were drawn for a cached picture")
        c.that(all(st["spun_fetch"] <= len(REFUSED) for st in seen),
               f"the loading blocks on {max(st['spun_fetch'] for st in seen)} cards "
               f"(the refused {len(REFUSED)} at most)")
        end = seen[-1]
        c.that(end["unavailable"] == len(REFUSED) and end["loading"] == 0,
               f"in the end {end['unavailable']} placeholders, {end['loading']} loading")
    again = [ids.index(cid) for _, cid, _ in server.thumb_requests(since)]
    c.that(sorted(again) == sorted(REFUSED), f"thumbnails asked for: {again} (only the refused {list(REFUSED)}, once)")
    return c.report()


def scenario_offline(a, size, work, server, keys, ids, inst):
    c = Check(f"offline at {size}")
    state = os.path.join(inst, "data", "characters", "store", "state.json")
    with open(state, encoding="utf-8") as f:
        s = json.load(f)
    s["checked"] = 0   # the catalog's check due: it finds no connection
    with open(state, "w", encoding="utf-8") as f:
        json.dump(s, f)
    server.down()
    script = TO_STORE + """settle 300000
wait 200
expect card: Hero 000
down
wait 10
expect card:
quit
"""
    lines, rc = run(a.launcher, inst, work, "offline", script, size, server, keys, a.timeout)
    server.up()
    common(c, lines, rc)
    seen = [st for _, _, st in steps(lines) if st]
    if c.that(seen, "no step logged the store's pictures"):
        end = seen[-1]
        c.that(all(st["spun_fetch"] == 0 for st in seen), "the loading blocks were drawn with no connection")
        c.that(end["loading"] == 0 and not end["fetching"], f"{end['loading']} loading, fetching {end['fetching']}")
        c.that(end["unavailable"] == len(REFUSED) and end["shown"] > 0,
               f"{end['shown']} shown, {end['unavailable']} placeholders ({len(REFUSED)} expected)")
    return c.report()


def scenario_drop(a, size, work, server, keys, ids, inst):
    c = Check(f"drop at {size}")
    server.delay, server.drop_after = 0.05, 5
    server.thumbs = 0
    since = len(server.log)
    script = TO_STORE + """settle 300000
wait 600
lt
wait 60
rt
wait 40
settle 300000
wait 600
quit
"""
    lines, rc = run(a.launcher, inst, work, "drop", script, size, server, keys, a.timeout)
    server.drop_after = None
    common(c, lines, rc)
    sts = [(op, st) for op, _, st in steps(lines) if st]
    reqs = server.thumb_requests(since)
    if c.that(sts, "no step logged the store's pictures"):
        end = sts[-1][1]
        c.that(end["loading"] == 0 and not end["fetching"] and end["stalled"],
               f"after the drop: {end['loading']} loading, fetching {end['fetching']}, stopped {end['stalled']}")
        c.that(end["unavailable"] > 0 and end["shown"] > 0, f"{end['shown']} shown, {end['unavailable']} placeholders")
        c.that(end["failed"] == 0, f"{end['failed']} counted as refused: the ones not tried must stay retryable")
    dropped = [r for r in reqs if r[2] == "dropped"]
    # each showing of the store: one fetch at most stops (its few in flight); never a loop
    c.that(0 < len(dropped) <= 2 * 6, f"{len(dropped)} requests dropped: two fetches' few at most expected")
    c.notes.append(f"{len(reqs)} thumbnail requests, {len(dropped)} dropped")
    return c.report()


def scenario_scroll(a, size, work, server, keys, ids, inst):
    c = Check(f"scroll at {size}")
    # the thumbnails slow (the catalog not): the store's order alone can't reach the end while the D-pad goes down,
    # however long that takes (a hidden launcher's frames come fast on Linux, at the display's rate on Windows: 40
    # presses took 12 s there)
    server.thumb_delay = 2.0
    since = len(server.log)
    script = TO_STORE + """expect card: Hero 000
seek 40000 down card: Hero 09
expect card: Hero 09
wait 30
settle 400000
wait 120
quit
"""
    lines, rc = run(a.launcher, inst, work, "scroll", script, size, server, keys, a.timeout)
    server.thumb_delay = 0.0
    common(c, lines, rc)
    seen = [st for _, _, st in steps(lines) if st]
    reqs = server.thumb_requests(since)
    order = [ids.index(cid) for _, cid, _ in reqs]
    pos = {i: order.index(i) for i in set(order)}
    last = list(range(N_CHARS - 8, N_CHARS))   # the last two rows: on screen once the D-pad reaches the end
    where = sorted(pos[i] for i in last if i in pos)
    c.that(len(where) == len(last), f"the last screen's pictures not all asked for ({len(where)} of {len(last)})")
    behind = [i for i in range(N_CHARS - len(last)) if pos.get(i, -1) > (where[-1] if where else len(order))]
    if where:
        # asked for together (a fetch's few), and ahead of pictures before them in the store's order: a fill in the
        # store's order alone asks for them last, and one in the catalog's order spreads them over the whole fetch
        c.that(where[-1] - where[0] < 24, f"the last screen's pictures asked for at {where}: together expected")
        c.that(behind, f"the last screen's pictures asked for at {where} of {len(order)}, after every picture before "
                       f"them in the store's order: the cards on screen weren't fetched first")
    if c.that(seen, "no step logged the store's pictures"):
        end = seen[-1]
        c.that(end["loading"] == 0 and not end["fetching"], f"in the end: {end['loading']} loading")
    per = {}
    for i in order:
        per[i] = per.get(i, 0) + 1
    c.that(all(n == 1 for n in per.values()), "a thumbnail was asked for twice")
    c.notes.append(f"the last screen asked for at positions {where} of {len(order)}, ahead of {len(behind)} "
                   f"pictures before it")
    return c.report()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--launcher", required=True, help="the launcher to test (a build or a package's)")
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "qual", "launcher-store-thumbs"))
    ap.add_argument("--only", nargs="*", choices=SCENARIOS)
    ap.add_argument("--sizes", default="1280x800,1280x720")
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--keep", action="store_true", help="leave the work folder (the test installations, the logs)")
    a = ap.parse_args()
    a.launcher = os.path.abspath(a.launcher)
    if not os.path.isfile(a.launcher):
        sys.exit(f"no launcher at {a.launcher}")
    work = os.path.abspath(a.work)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    repo, keys, tampered = make_store(os.path.join(work, "store"))
    cat = json.load(open(os.path.join(repo, "catalog", "v1", "catalog.json"), encoding="utf-8"))
    ids = [e["id"] for e in cat["catalog"]["characters"]]
    server = Server(repo, tampered)
    server.up()
    only = a.only or list(SCENARIOS)
    ok = True
    fns = {"first": scenario_first, "return": scenario_return, "reopen": scenario_reopen,
           "offline": scenario_offline, "drop": scenario_drop, "scroll": scenario_scroll}
    try:
        for size in [s.strip() for s in a.sizes.split(",") if s.strip()]:
            d = os.path.join(work, size)
            full = os.path.join(d, "inst-full")    # first, return, reopen, offline: one installation, filled by first
            for name in only:
                inst = full if name in ("first", "return", "reopen", "offline") else os.path.join(d, "inst-" + name)
                os.makedirs(inst, exist_ok=True)
                server.delay, server.thumb_delay, server.drop_after = 0.0, 0.0, None
                ok = fns[name](a, size, os.path.join(d, name), server, keys, ids, inst) and ok
    finally:
        server.down()
    if not a.keep and ok:
        shutil.rmtree(work, ignore_errors=True)
    print("launcher store pictures:", "PASS" if ok else "FAIL", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
