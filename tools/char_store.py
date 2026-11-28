"""M12, the Character Store -- read-only (docs/CHARACTER_STORE.md).

The approved community characters live in a public GitHub repository; this module reads it like a static web site
(raw files, no GitHub API, no account, no token):

  catalog/v1/catalog.json            the catalog, signed: its schema version, its own version (catalog_version,
                                     only ever increasing) and each character's description, creator, licence,
                                     version, file and thumbnail (paths in the repository, sizes, SHA-256)
  characters/<id>/<id>.chchar        the character (a Cubic.Heroes character file)
  characters/<id>/thumb.png          its thumbnail (the previews' renderer, 8-bit RGB)

What it does, and what it never does:
- The catalog is authenticated: its "catalog" member, in canonical JSON, carries an Ed25519 signature by a key the
  launcher trusts (config/catalog_keys.json; the private key is the owner's, offline). An unsigned or mis-signed
  catalog, or one older than the catalog already saved (a lower catalog_version), is refused and the saved one stays.
  The files it lists are then checked against its SHA-256s, so every download is as authentic as the catalog.
- The catalog is fetched when the store is shown, at most every AUTO_CHECK_S unless the player refreshes, with a
  conditional request (If-None-Match / If-Modified-Since): an unchanged catalog costs one small 304. A rate limit
  (429, or 403 with Retry-After / X-RateLimit-Remaining 0) is honoured: no request until it has passed. Without a
  connection, the cached catalog is shown and says so.
- The catalog and the thumbnails are cached under the library (<library>/store/). A character's file is downloaded
  only when the player asks, never twice (its data's SHA-256 is compared with the library first).
- Everything downloaded is data, never run: the catalog is JSON checked field by field within limits; a thumbnail is
  checked against its size and SHA-256 and decoded only as the renderer's own PNG form; a character file is checked
  against its size and SHA-256, may hold only a character's members (no PS3 save files, no executables), and goes
  through the library's own import (format, limits, the data's recorded SHA-256), as any character file does.
- Files come only from the repository's own address: paths in the catalog are plain relative paths; a redirect to
  another host is refused.
"""
import hashlib
import json
import os
import re
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib

import base64
import sys

import char_library
import char_preview

CATALOG_FORMAT = "cubic.heroes.catalog"
SCHEMA_VERSION = 1                   # the catalog's layout this launcher reads
ROOT = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KEYS_FILE = os.path.join(ROOT, "config", "catalog_keys.json")
KEYS_ENV = "CUBICHEROES_STORE_KEYS"   # development: another keys file -- honoured only with a copy on this computer
# the licences a store character may carry: original community characters whose creator agreed to CC BY 4.0
LICENSES = {"CC-BY-4.0": ("CC BY 4.0", "https://creativecommons.org/licenses/by/4.0/")}
# the store's two kinds of character, kept apart:
#   community   submitted to the Character Store with the creator's explicit CC BY 4.0 agreement
#   historical  recovered from an earlier community site (the 2010 Hall of Heroes, say) and preserved with its
#               creator's name and its archival source: no licence is claimed for it -- its creator keeps every
#               right -- and the catalog says how to ask for its removal
CATEGORIES = ("community", "historical")
HISTORICAL_RIGHTS = ("Recovered community creation, preserved for players. No licence is claimed: all rights remain "
                     "with its creator.")
DEFAULT_BASE = "https://raw.githubusercontent.com/irnutsmurt/Cubic.Heroes-Characters/main/"
CATALOG_PATH = "catalog/v1/catalog.json"
BASE_ENV = "CUBICHEROES_STORE_URL"   # development: another copy of the repository (https, or http on this computer)

CATALOG_MAX = 4 * 1024 * 1024
ENTRIES_MAX = 5000
CHCHAR_MAX = 4 * 1024 * 1024
THUMB_MAX = 512 * 1024
THUMB_DIM_MAX = 1024
THUMB_WORKERS = 6                     # thumbnails fetched at once (raw files; a rate limit stops them all)
AUTO_CHECK_S = 10 * 60
RATE_LIMIT_DEFAULT_S = 15 * 60
TIMEOUT_S = 20
USER_AGENT = "CubicHeroes-Launcher"
CHCHAR_MEMBERS = {"character.json", "character.edt", "preview/thumb.png", "preview/views.png"}

_HEX16 = re.compile(r"^[0-9a-f]{16}$")
_HEX64 = re.compile(r"^[0-9a-f]{64}$")
_PATH = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*(/[A-Za-z0-9][A-Za-z0-9._-]*)*$")


class StoreError(Exception):
    pass


# -- where -----------------------------------------------------------------------------------------------------------
def base_url(env=None):
    """the repository's address: the official one, or a development copy named by CUBICHEROES_STORE_URL (https, or
    plain http only on this computer)"""
    env = os.environ if env is None else env
    b = env.get(BASE_ENV) or DEFAULT_BASE
    if not b.endswith("/"):
        b += "/"
    u = urllib.parse.urlsplit(b)
    local = u.hostname in ("127.0.0.1", "localhost", "::1")
    if u.scheme != "https" and not (u.scheme == "http" and local):
        raise StoreError(f"{BASE_ENV} must be an https address (or http on this computer)")
    if u.query or u.fragment or u.username or u.password:
        raise StoreError(f"{BASE_ENV}: a plain address only")
    return b


def is_local(base):
    return urllib.parse.urlsplit(base).hostname in ("127.0.0.1", "localhost", "::1")


def release_key_ids(path=None):
    """the ids of every key in config/release_keys.json, revoked or not: never trusted for the catalog"""
    import release_trust
    try:
        with open(path or release_trust.KEYS_FILE, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return set()
    out = set()
    for k in data.get("keys", []):
        try:
            out.add(release_trust.key_id(release_trust.public_key_from(k["ed25519"])))
        except (KeyError, ValueError, TypeError, release_trust.ReleaseError):
            pass
    return out


def trusted_keys(base, env=None, keys_file=None, release_file=None):
    """[(key id, 32-byte Ed25519 public key)] the catalog may be signed with: config/catalog_keys.json (the same form
    as config/release_keys.json: several keys for a rotation; a revoked key is kept with "revoked": true and no
    longer trusted). A key that is also a release key is never trusted here: the catalog and the software are signed
    apart, so a catalog key can't sign an update and the release key can't sign a catalog. A development copy on
    this computer may name another keys file (CUBICHEROES_STORE_KEYS)."""
    import release_trust
    env = os.environ if env is None else env
    path = keys_file or KEYS_FILE
    if not keys_file and env.get(KEYS_ENV) and is_local(base):
        path = env[KEYS_ENV]
    rel = release_key_ids(release_file)
    return [(kid, pk) for kid, pk in release_trust.trusted_keys(path) if kid not in rel]


def canonical(obj):
    """the bytes a catalog's signature covers: its "catalog" member as canonical JSON (keys sorted, no spaces, UTF-8)"""
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def verify(signed, signatures, keys):
    """the key id that signed it, or raises"""
    import release_trust
    if not keys:
        raise StoreError("this launcher has no catalog key yet")
    if not isinstance(signatures, list) or not signatures:
        raise StoreError("it is not signed")
    msg = canonical(signed)
    by_id = dict(keys)
    for sg in signatures[:8]:
        if not isinstance(sg, dict):
            continue
        pk = by_id.get(sg.get("key_id"))
        try:
            sig = base64.b64decode(sg.get("ed25519") or "", validate=True)
        except (ValueError, TypeError):
            continue
        if pk and release_trust.ed25519_verify(pk, msg, sig):
            return sg["key_id"]
    raise StoreError("its signature does not verify")   # not a trusted key's, or changed after it was signed


def resolve(base, path):
    """a file of the repository: a plain relative path (no scheme, no host, no .., no leading /) under base"""
    if not isinstance(path, str) or len(path) > 200 or not _PATH.match(path) or ".." in path.split("/"):
        raise StoreError(f"the catalog names a file outside the repository ({str(path)[:60]!r})")
    return base + path


# -- the network -----------------------------------------------------------------------------------------------------
class Response:
    def __init__(self, status, headers, body):
        self.status, self.headers, self.body = status, headers, body


def _tls_context(url):
    if not url.lower().startswith("https:"):
        return None
    try:
        import updates   # Linux: the system's certificate store (beta.4); Windows: urllib's default
        return updates.tls_context()
    except ImportError:
        return None


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """a redirect is followed only within the repository's own host"""
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if urllib.parse.urlsplit(newurl).netloc != urllib.parse.urlsplit(req.full_url).netloc:
            raise StoreError("the store's server sent the download elsewhere; refused")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def http_get(url, headers=None, limit=CATALOG_MAX, timeout=TIMEOUT_S):
    """(Response) of a GET: 200 with its body (at most limit bytes, else an error), 304, or an HTTP error's status
    and headers. No connection raises OSError (offline)."""
    h = {"User-Agent": USER_AGENT, "Accept": "*/*"}
    h.update(headers or {})
    ctx = _tls_context(url)
    handlers = [_NoRedirect()]
    if ctx is not None:
        handlers.append(urllib.request.HTTPSHandler(context=ctx))
    opener = urllib.request.build_opener(*handlers)
    try:
        with opener.open(urllib.request.Request(url, headers=h), timeout=timeout) as r:
            body = r.read(limit + 1)
            if len(body) > limit:
                raise StoreError(f"{url.rsplit('/', 1)[-1]} is larger than allowed")
            return Response(r.status, dict(r.headers), body)
    except urllib.error.HTTPError as e:
        return Response(e.code, dict(e.headers or {}), b"")
    except urllib.error.URLError as e:
        raise OSError(str(getattr(e, "reason", e))) from e


def _retry_after(r, now):
    """the time a rate limit ends (429, or 403 saying the limit was reached), else None"""
    hd = {k.lower(): v for k, v in r.headers.items()}
    limited = r.status == 429 or (r.status == 403 and ("retry-after" in hd or hd.get("x-ratelimit-remaining") == "0"))
    if not limited:
        return None
    try:
        return now + max(1, min(24 * 3600, int(hd.get("retry-after", ""))))
    except ValueError:
        pass
    try:
        return max(now + 1, min(now + 24 * 3600, int(hd.get("x-ratelimit-reset", ""))))
    except ValueError:
        return now + RATE_LIMIT_DEFAULT_S


# -- the catalog -----------------------------------------------------------------------------------------------------
def _text(v, n, required=False):
    if v is None and not required:
        return ""
    if not isinstance(v, str) or (required and not v.strip()):
        raise ValueError("a text field is missing")
    return "".join(ch for ch in v if ch >= " " or ch == "\n")[:n]


def _file(v, limit):
    if not isinstance(v, dict):
        raise ValueError("a file is missing")
    path, sha, size = v.get("path"), v.get("sha256"), v.get("size")
    if not isinstance(path, str) or not _PATH.match(path) or ".." in path.split("/"):
        raise ValueError("a file's path is not a plain path in the repository")
    if not isinstance(sha, str) or not _HEX64.match(sha):
        raise ValueError("a file's SHA-256 is missing")
    if not isinstance(size, int) or isinstance(size, bool) or not 0 < size <= limit:
        raise ValueError("a file's size is missing or too large")
    return {"path": path, "sha256": sha, "size": size}


def _hexes(v, n_max):
    if v is None:
        return []
    if not isinstance(v, list) or len(v) > n_max or not all(isinstance(x, str) and _HEX64.match(x) for x in v):
        raise ValueError("a list of SHA-256s is not one")
    return list(v)


def parse_catalog(data, keys):
    """(catalog, [problems]): the signed catalog, its signature verified first, then each character checked field by
    field; an entry that fails is left out and named (one bad entry never hides the others). An unsigned or
    mis-signed catalog, another format or a newer schema raises."""
    if len(data) > CATALOG_MAX:
        raise StoreError("the catalog is larger than allowed")
    try:
        doc = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        raise StoreError("the catalog is not readable")
    if not isinstance(doc, dict) or doc.get("format") != CATALOG_FORMAT:
        raise StoreError("not a Cubic.Heroes character catalog")
    sv = doc.get("schema_version")
    if not isinstance(sv, int) or isinstance(sv, bool):
        raise StoreError("the catalog has no schema version")
    if sv > SCHEMA_VERSION:
        raise StoreError(f"the catalog's schema is newer ({sv}); update Cubic.Heroes to browse it")
    signed = doc.get("catalog")
    if not isinstance(signed, dict):
        raise StoreError("the catalog has no signed part")
    key_id = verify(signed, doc.get("signatures"), keys)
    return parse_entries(signed, sv, key_id)


def parse_entries(signed, sv=SCHEMA_VERSION, key_id=""):
    """(catalog, [problems]) of a catalog's signed part, every entry checked -- parse_catalog's second half, after the
    signature (char_catalog.py check --unsigned uses it alone, before the owner signs)"""
    cv = signed.get("catalog_version")
    if not isinstance(cv, int) or isinstance(cv, bool) or cv < 1:
        raise StoreError("the catalog has no version")
    chars = signed.get("characters")
    if not isinstance(chars, list):
        raise StoreError("the catalog lists no characters")
    out, problems, seen = [], [], set()
    for i, e in enumerate(chars[:ENTRIES_MAX]):
        try:
            if not isinstance(e, dict):
                raise ValueError("not an entry")
            cid = e.get("id")
            if not isinstance(cid, str) or not _HEX16.match(cid):
                raise ValueError("no id")
            if cid in seen:
                raise ValueError("listed twice")
            ver = e.get("version")
            if not isinstance(ver, int) or isinstance(ver, bool) or ver < 1:
                raise ValueError("no character version")
            edt = e.get("edt_sha256")
            if not isinstance(edt, str) or not _HEX64.match(edt):
                raise ValueError("no SHA-256 of its data")
            kind = e.get("category") or "community"
            if kind not in CATEGORIES:
                raise ValueError(f"unknown category {str(kind)[:20]!r}")
            lic = e.get("license")
            label = _text(e.get("label"), 64, True)
            src = {}
            if kind == "community":
                if lic not in LICENSES:
                    raise ValueError(f"its licence ({str(lic)[:20]!r}) is not one the store accepts")
                creator = _text(e.get("creator"), 64, True)
                attribution = _text(e.get("attribution"), 200) or f"\"{label}\" by {creator}, {LICENSES[lic][0]}"
            else:   # historical: never a licence; its original source on record
                if lic not in (None, ""):
                    raise ValueError("a historical character carries no licence")
                lic = ""
                creator = _text(e.get("creator"), 64) or "unknown"
                s0 = e.get("source")
                if not isinstance(s0, dict) or not _text(s0.get("collection"), 80):
                    raise ValueError("a historical character names its original collection")
                src = {"collection": _text(s0.get("collection"), 80), "original_name": _text(s0.get("original_name"), 64),
                       "original_date": _text(s0.get("original_date"), 32)}
                for k in ("archived_url", "original_url"):
                    u = _text(s0.get(k), 300)
                    src[k] = u if u.startswith(("https://", "http://")) else ""
                attribution = (_text(e.get("attribution"), 200) or
                               f"\"{src['original_name'] or label}\" by {creator}, shared on {src['collection']}")
            url = _text(e.get("creator_url"), 200)
            if url and not url.startswith("https://"):
                url = ""
            tags = e.get("tags") or []
            if not isinstance(tags, list):
                raise ValueError("tags are not a list")
            shape = e.get("shape_sha256")
            if shape is not None and (not isinstance(shape, str) or not _HEX64.match(shape)):
                raise ValueError("its shape's SHA-256 is not one")
            out.append({
                "category": kind, "rights": HISTORICAL_RIGHTS if kind == "historical" else "", "source": src,
                "id": cid, "version": ver, "edt_sha256": edt, "previous_sha256": _hexes(e.get("previous_sha256"), 50),
                "label": label, "name": _text(e.get("name"), 32), "class": _text(e.get("class"), 32),
                "creator": creator, "creator_url": url, "license": lic, "attribution": attribution,
                "description": _text(e.get("description"), 600), "added": _text(e.get("added"), 32),
                "updated": _text(e.get("updated"), 32),
                "tags": [_text(t, 32) for t in tags[:12] if isinstance(t, str) and t.strip()],
                "shape_sha256": shape or "",   # optional: the model's hash, for "the same model in your library"
                "file": _file(e.get("file"), CHCHAR_MAX), "thumb": _file(e.get("thumb"), THUMB_MAX),
            })
            seen.add(cid)
        except ValueError as err:
            problems.append(f"catalog entry {i + 1}: {err}; left out")
    if len(chars) > ENTRIES_MAX:
        problems.append(f"the catalog lists more than {ENTRIES_MAX} characters; the rest are left out")
    removal = _text(signed.get("removal_url"), 300)
    cat = {"schema_version": sv, "catalog_version": cv, "updated": _text(signed.get("updated"), 32), "key_id": key_id,
           "removal_url": removal if removal.startswith("https://") else "", "characters": out}
    return cat, problems


def thumb_rle(png):
    """the launcher's picture of a catalog thumbnail: only the renderer's own PNG form (8-bit RGB, unfiltered rows),
    at most THUMB_DIM_MAX square, decompressed within its exact expected size"""
    if len(png) < 33 or png[:8] != b"\x89PNG\r\n\x1a\n" or png[12:16] != b"IHDR":
        raise StoreError("the thumbnail is not a PNG")
    w, h = int.from_bytes(png[16:20], "big"), int.from_bytes(png[20:24], "big")
    if not (0 < w <= THUMB_DIM_MAX and 0 < h <= THUMB_DIM_MAX):
        raise StoreError("the thumbnail's size is not allowed")
    expect = (1 + 3 * w) * h
    o, idat = 8, b""
    while o + 8 <= len(png):
        n = int.from_bytes(png[o:o + 4], "big")
        t = png[o + 4:o + 8]
        if t == b"IDAT":
            idat += png[o + 8:o + 8 + n]
        o += 12 + n
    d = zlib.decompressobj()
    try:
        raw = d.decompress(idat, expect + 1)
    except zlib.error:
        raise StoreError("the thumbnail is damaged")
    if len(raw) != expect:
        raise StoreError("the thumbnail is not in the renderer's form")
    try:
        return char_preview.rle_from_png(png)
    except (ValueError, zlib.error, IndexError) as e:
        raise StoreError(f"the thumbnail is not in the renderer's form ({e})")


def _sha(b):
    return hashlib.sha256(b).hexdigest()


def canonical_doc(data):
    """a catalog document's signed part, canonical (the same catalog whatever its spacing)"""
    try:
        return canonical(json.loads(data.decode("utf-8")).get("catalog"))
    except (UnicodeDecodeError, ValueError, AttributeError):
        return None


def canonical_doc_file(path):
    with open(path, "rb") as f:
        return canonical_doc(f.read())


def _write_atomic(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


# -- the store -------------------------------------------------------------------------------------------------------
class Store:
    """the catalog, its cache and the downloads, for one library (<library>/store/)"""

    def __init__(self, library, base=None, log=None, get=None, clock=None, keys=None):
        self.lib = library
        self.base = base or base_url()
        self.keys = trusted_keys(self.base) if keys is None else keys
        self.dir = os.path.join(library.root, "store")
        self.log = log or (lambda level, text: None)
        self.get = get or http_get
        self.clock = clock or time.time
        self.state_path = os.path.join(self.dir, "state.json")
        self.catalog_path = os.path.join(self.dir, "catalog.json")
        try:
            with open(self.state_path, encoding="utf-8") as f:
                self.state = json.load(f)
            if self.state.get("base") != self.base:   # another repository: nothing of the old one's cache applies
                self.state = {}
        except (OSError, ValueError):
            self.state = {}

    def _save_state(self):
        self.state["base"] = self.base
        _write_atomic(self.state_path, json.dumps(self.state, indent=1).encode("utf-8"))

    def cached(self):
        """(catalog, problems) from the cache -- its signature verified again (a key may have been withdrawn) -- or
        (None, []) when there is none"""
        try:
            with open(self.catalog_path, "rb") as f:
                return parse_catalog(f.read(), self.keys)
        except (OSError, StoreError):
            return None, []

    def refresh(self, force=False):
        """the catalog checked with the repository (conditionally); never raises for the network. Returns
        {status: updated|unchanged|recent|offline|limited|error, message, checked}"""
        now = self.clock()
        until = self.state.get("retry_after") or 0
        if until > now:
            return self._status("limited", f"GitHub asked to wait: the saved catalog is shown (checks again after "
                                           f"{time.strftime('%H:%M', time.localtime(until))})")
        have, _ = self.cached()
        if not force and have is not None and now - (self.state.get("checked") or 0) < AUTO_CHECK_S:
            return self._status("recent", "")
        headers = {}
        if have is not None:
            if self.state.get("etag"):
                headers["If-None-Match"] = self.state["etag"]
            if self.state.get("last_modified"):
                headers["If-Modified-Since"] = self.state["last_modified"]
        url = self.base + CATALOG_PATH
        try:
            r = self.get(url, headers, CATALOG_MAX)
        except (OSError, TimeoutError, StoreError) as e:
            self.log("warning", f"store: the catalog could not be checked ({type(e).__name__})")
            self.state.update(status="offline")
            self._save_state()
            return self._status("offline", "GitHub could not be reached: the saved catalog is shown" if have is not None
                                else "GitHub could not be reached, and no catalog was saved yet.")
        ra = _retry_after(r, now)
        if ra:
            self.state.update(retry_after=ra, status="limited")
            self._save_state()
            self.log("warning", f"store: rate limited ({r.status}); no request for {int(ra - now)} s")
            return self._status("limited", "GitHub asked to wait: the saved catalog is shown" if have is not None
                                else "GitHub asked to wait before the catalog can be read; try again later.")
        if r.status == 304 and have is not None:
            self.state.update(checked=now, status="unchanged", retry_after=0)
            self._save_state()
            return self._status("unchanged", "")
        if r.status != 200:
            self.state.update(checked=now, status="error")
            self._save_state()
            if r.status == 404 and have is None:   # the repository is there, its catalog not yet
                return self._status("error", "The Character Store has no catalog yet: nothing to show.")
            return self._status("error", f"GitHub answered {r.status} for the catalog" +
                                (": the saved catalog is shown" if have is not None else "."))
        try:
            cat, problems = parse_catalog(r.body, self.keys)
            floor = self.floor()
            if have is not None:
                floor = max(floor, have["catalog_version"])
            if cat["catalog_version"] < floor:
                raise StoreError(f"it is older than the catalog already seen (version {cat['catalog_version']} after "
                                 f"{floor})")
            if have is not None and cat["catalog_version"] == have["catalog_version"]:
                with open(self.catalog_path, "rb") as f:
                    if f.read() != r.body and canonical_doc(r.body) != canonical_doc_file(self.catalog_path):
                        raise StoreError("it changed without a new catalog version")
        except StoreError as e:
            self.state.update(checked=now, status="error")
            self._save_state()
            self.log("warning", f"store: the catalog was refused ({e})")
            return self._status("error", f"The new catalog was refused ({e})" +
                                ("; the saved catalog is shown" if have is not None else "."))
        _write_atomic(self.catalog_path, r.body)
        if cat["catalog_version"] >= self.floor():
            self.state["floor"] = {"catalog_version": cat["catalog_version"], "key_id": cat["key_id"]}
        hd = {k.lower(): v for k, v in r.headers.items()}
        self.state.update(checked=now, fetched=now, status="updated", retry_after=0, etag=hd.get("etag", ""),
                          last_modified=hd.get("last-modified", ""), problems=problems[:50])
        self._save_state()
        for p in problems[:10]:
            self.log("warning", f"store: {p}")
        return self._status("updated", "")

    def floor(self):
        """the highest catalog version accepted before (kept in state.json, so it holds across restarts and without
        the cached catalog) -- while the key that signed it is still trusted: once that key is revoked, its catalogs
        set no floor (a compromised key's inflated version can't lock the store out of the next real catalog)"""
        f = self.state.get("floor") or {}
        v = f.get("catalog_version")
        if not isinstance(v, int) or f.get("key_id") not in {k for k, _ in self.keys}:
            return 0
        return v

    def _status(self, status, message):
        return {"status": status, "message": message, "checked": self.state.get("checked") or 0,
                "fetched": self.state.get("fetched") or 0}

    def thumb_path(self, entry):
        return os.path.join(self.dir, "thumbs", entry["thumb"]["sha256"][:32] + ".rle")

    def fetch_thumbs(self, entries, budget=200, deadline_s=60, workers=THUMB_WORKERS):
        """the thumbnails not cached yet, each checked (size, SHA-256, the renderer's PNG form) and kept as the
        launcher's picture, a few at a time (workers); stops at the budget, the deadline, a rate limit or no
        connection. -> (made, failed)"""
        import concurrent.futures
        import threading
        t0 = self.clock()
        todo = [e for e in entries if not os.path.isfile(self.thumb_path(e))][:budget]
        stop = threading.Event()
        counts = {"made": 0, "failed": 0}
        lock = threading.Lock()

        def one(e):
            if stop.is_set() or self.clock() - t0 > deadline_s or (self.state.get("retry_after") or 0) > self.clock():
                stop.set()
                return
            try:
                r = self.get(resolve(self.base, e["thumb"]["path"]), None, THUMB_MAX)
                ra = _retry_after(r, self.clock())
                if ra:
                    with lock:
                        self.state.update(retry_after=ra)
                        self._save_state()
                    stop.set()
                    return
                if r.status != 200 or len(r.body) != e["thumb"]["size"] or _sha(r.body) != e["thumb"]["sha256"]:
                    raise StoreError("the thumbnail does not match the catalog")
                _write_atomic(self.thumb_path(e), thumb_rle(r.body))
                with lock:
                    counts["made"] += 1
            except (OSError, TimeoutError):
                stop.set()   # offline: the rest stay as placeholders
            except StoreError as err:
                with lock:
                    counts["failed"] += 1
                self.log("warning", f"store: thumbnail of {e['id']}: {err}")

        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, workers)) as pool:
            list(pool.map(one, todo))
        return counts["made"], counts["failed"]

    def entries(self, savedata, included=None):
        """the cached catalog's characters, each with where it is: in_library (its library id), installed (its folder
        in the game's saves), thumb (the cached picture, or ""); included_with ([{id, label}]: the installed official
        content packages that bring it -- `included`, tools/official_content.py's {catalog id: [...]}, worked out by
        the caller from the installation's record, never from the game's files); same_model_in_library (a library
        entry with the same model under another name or file, when the catalog gives the model's hash); sources (where
        the player has it: official / library, with whether the library's copy came from this store or was imported
        by the player / game, its folder in the saves) -- one entry per character, its sources kept apart. Reads only:
        nothing is written, imported or downloaded here."""
        cat, problems = self.cached()
        if cat is None:
            return None, []
        included = included or {}
        idx = self.lib.index()
        by_sha = {e["sha256"]: cid for cid, e in idx.items()}
        by_shape = {}
        for cid, e in idx.items():
            if e.get("shape"):
                by_shape.setdefault(e["shape"], cid)
        in_game = {}
        try:
            found, _ = char_library.Library.installed(savedata)
            in_game = {sha: folder for sha, folder in found.items()} if isinstance(found, dict) else {}
        except (OSError, char_library.LibraryError):
            pass
        out = []
        for e in cat["characters"]:
            e = dict(e)
            e["in_library"] = by_sha.get(e["edt_sha256"], "")
            e["installed"] = in_game.get(e["edt_sha256"], "")
            e["older_in_library"] = "" if e["in_library"] else next(
                (by_sha[x] for x in e["previous_sha256"] if x in by_sha), "")
            e["same_model_in_library"] = ("" if e["in_library"] or not e.get("shape_sha256")
                                          else by_shape.get(e["shape_sha256"], ""))
            e["included_with"] = [dict(x) for x in included.get(e["id"], [])]
            src = [{"kind": "official", "package": x["id"], "label": x["label"]} for x in e["included_with"]]
            lib_id = e["in_library"] or e["same_model_in_library"]
            if lib_id:
                k = (idx.get(lib_id) or {}).get("source", "files")
                src.append({"kind": "library", "library_id": lib_id,
                            "origin": "store" if k in ("store", "historical") else "import", "library_source": k,
                            "same_model": not e["in_library"]})
            if e["installed"]:
                src.append({"kind": "game_saves", "folder": e["installed"]})
            e["sources"] = src
            p = self.thumb_path(e)
            e["thumb_path"] = p if os.path.isfile(p) else ""
            out.append(e)
        return {"schema_version": cat["schema_version"], "catalog_version": cat["catalog_version"],
                "updated": cat["updated"], "key_id": cat["key_id"], "removal_url": cat.get("removal_url", ""),
                "characters": out}, problems

    def download(self, cid, savedata=None, install=False, included=None):
        """one character from the catalog into the library (and into the game when install): never twice, never one
        the installation's official content already brings (`included`, as for entries: then nothing is downloaded,
        copied or imported -- the game reads it from its own files). ->
        {status: imported|duplicate|installed|official|..., id, message, folder?}"""
        cat, _ = self.cached()
        e = next((x for x in (cat or {}).get("characters", []) if x["id"] == cid), None)
        if e is None:
            raise StoreError("this character is not in the catalog (refresh the store)")
        by = (included or {}).get(cid) or []
        if by:
            return {"id": None, "status": "official", "included_with": [dict(x) for x in by],
                    "message": f"\"{e['label']}\" is already in the game: {by[0]['label'].lower()}; nothing to download"}
        have = {v["sha256"]: k for k, v in self.lib.index().items()}
        lib_id = have.get(e["edt_sha256"])
        res = {"id": lib_id, "status": "duplicate", "message": f"\"{e['label']}\" is already in your library"}
        if not lib_id:
            if (self.state.get("retry_after") or 0) > self.clock():
                raise StoreError("GitHub asked to wait before more downloads; try again later")
            try:
                r = self.get(resolve(self.base, e["file"]["path"]), None, CHCHAR_MAX)
            except (OSError, TimeoutError):
                raise StoreError("GitHub could not be reached; nothing was downloaded")
            ra = _retry_after(r, self.clock())
            if ra:
                self.state.update(retry_after=ra)
                self._save_state()
                raise StoreError("GitHub asked to wait before more downloads; try again later")
            if r.status != 200:
                raise StoreError(f"GitHub answered {r.status}; nothing was downloaded")
            data = r.body
            if len(data) != e["file"]["size"] or _sha(data) != e["file"]["sha256"]:
                raise StoreError("the downloaded file does not match the catalog (size or SHA-256); refused")
            files = char_library.read_zip(data, e["label"])
            extra = sorted(set(files) - CHCHAR_MEMBERS)
            if extra:
                raise StoreError(f"the character file holds more than a character ({', '.join(extra[:3])}); refused")
            cands = char_library.from_chchar(data, f"Character Store: {e['label']}")
            c = cands[0]
            if _sha(c.files["character.edt"]) != e["edt_sha256"]:
                raise StoreError("the character inside is not the one the catalog describes; refused")
            c.meta = dict(c.meta or {})
            c.meta.update(label=e["label"], creator=e["creator"],
                          description=e["description"] or c.meta.get("description"), tags=e["tags"],
                          source=dict({"kind": "store" if e["category"] == "community" else "historical",
                                       "catalog_id": e["id"], "version": e["version"], "license": e["license"],
                                       "attribution": e["attribution"], "creator_url": e["creator_url"],
                                       "imported_from": "Character Store"},
                                      **({"collection": e["source"].get("collection", ""),
                                          "url": e["source"].get("archived_url", ""), "rights": e["rights"]}
                                         if e["category"] == "historical" else {})))
            rs = self.lib.import_candidates([c])
            r0 = rs[0] if rs else {"status": "failed", "message": "nothing was imported"}
            if r0["status"] not in ("imported", "similar", "duplicate"):
                raise StoreError(r0.get("message") or "the character could not be imported")
            res = {"id": r0.get("id"), "status": r0["status"], "message": r0.get("message", "")}
        if install and res.get("id"):
            folder, made = self.lib.install(res["id"], savedata)
            res.update(folder=folder, installed=True,
                       message=(res["message"] + "; " if res["status"] != "duplicate" else "") +
                       ("installed in the game" if made else "already in the game"))
        return res
