# The Character Store (M12, read-only)

The Character Store is the third tab of the launcher's Characters page, beside *My Library* and *Installed*. It shows
the approved community characters from the public characters repository
(`https://github.com/irnutsmurt/Cubic.Heroes-Characters`). The player browses them with the same cards, search,
filters and order as the library, and downloads one into *My Library* (and, if asked, into the game) when they
choose it. Adding approved characters to the repository needs no new Cubic.Heroes release.

The store is **read-only**: publishing and submissions are separate and are not part of the launcher. The tab is
hidden until the owner enables it (`CUBICHEROES_CHARACTER_STORE=1` shows it for development).

## The repository

```
catalog/v1/catalog.json              the catalog (versioned: v1 here; a newer launcher may read a newer one)
characters/<id>/<id>.chchar          each character: a Cubic.Heroes character file
characters/<id>/thumb.png            its thumbnail, drawn by the previews' renderer
```

`<id>` is the first 16 hex digits of the character data's SHA-256 (the library's own id), so a character has one id
everywhere.

### The catalog (schema version 1)

```json
{
 "format": "cubic.heroes.catalog",
 "schema_version": 1,
 "catalog": {
  "catalog_version": 7,
  "updated": "2026-11-24",
  "characters": [
   {
    "id": "49b26d42bf3f49b7", "version": 1,
    "edt_sha256": "49b26d42bf3f49b7...", "previous_sha256": [],
    "label": "Rook", "name": "ROOK", "class": "KNIGHT",
    "creator": "...", "creator_url": "https://...", "license": "CC-BY-4.0",
    "attribution": "\"Rook\" by ..., CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/)",
    "tags": ["knight"], "description": "...", "added": "2026-11-24", "updated": "2026-11-24",
    "file":  {"path": "characters/49b26d42bf3f49b7/49b26d42bf3f49b7.chchar", "sha256": "...", "size": 4219},
    "thumb": {"path": "characters/49b26d42bf3f49b7/thumb.png", "sha256": "...", "size": 6150}
   }
  ]
 },
 "signatures": [{"key_id": "...", "ed25519": "<64 bytes, base64>"}]
}
```

- **Versions.** `schema_version` is the layout. A launcher refuses a newer schema than it knows, and keeps the saved
  catalog. `catalog_version` is the catalog's own number, and it goes up with every change. A launcher never goes
  back to a lower one, and it refuses a catalog that changed without a new number.
  - A character's `version` goes up when its creator replaces it. The `id` stays the same, `edt_sha256` is the
    current data's, and `previous_sha256` lists the earlier versions'. A player who has an earlier version sees
    UPDATE, and the new version is added beside the old one.
- **Authentication.** `signatures` holds an Ed25519 signature of the `catalog` member in canonical JSON: keys
  sorted, no spaces, UTF-8.
  - It is checked with OpenSSL, through the same code as the release check (tools/release_trust.py), against the
    keys in `config/catalog_keys.json`.
  - The private key is the owner's, kept offline. It is made and used like the release key (docs/RELEASE_SIGNING.md):
    `tools/char_catalog.py keygen` / `pubkey` / `sign`.
  - Until a key is in `config/catalog_keys.json`, every catalog is refused.
  - Because the signature sits in the same file as what it signs, one request (one ETag) always brings the two
    together. The files the catalog lists are then checked against its SHA-256s and sizes, so each download is as
    authentic as the catalog.
- **Licences and attribution.** `creator`, `license` and `attribution` are required. The store accepts
  `CC-BY-4.0` only, for original community characters whose creator explicitly agreed to it. An entry with any
  other licence is left out. The character repository's software and tooling stay under MIT, its `LICENSE`.
- A launcher reads each entry on its own: an entry that fails a check is left out and logged, and the others are
  shown.

### Submissions, building and checking a copy (`tools/char_catalog.py`)

Each character comes with a **submission record**, the creator's explicit agreement:

```json
{"character": "rook.chchar", "label": "Rook", "creator": "Their name", "creator_url": "https://...",
 "license": "CC-BY-4.0", "tags": ["knight"], "description": "...",
 "consent": {"agreed": true, "date": "2026-11-24", "record": "where the agreement is kept"},
 "replaces": "<catalog id, for a new version only>"}
```

```
python tools/char_catalog.py build --repo DIR SUBMISSION.json...     # add them; the catalog version goes up
python tools/char_catalog.py sign  --repo DIR --key catalog-signing-key.pem
python tools/char_catalog.py check --repo DIR                         # what the launcher would accept
```

- **No licence is ever assigned by default.** A record without `consent.agreed`, its date and its record, without
  a creator, or with a licence the store doesn't accept, is refused. The historical Hall of Heroes characters are
  never given a licence this way.
- The consent record stays with the maintainers: the catalog carries the creator, the licence and the attribution
  only.
- `build` imports each character through the library, so it gets the same checks as any import. A PS3 save that is
  still encrypted is reported and skipped, never decrypted.
- It writes each character as the library exports it: the character and its description only, with no PS3 save
  files (no PARAM.SFO or account data, no PARAM.PFD) and no keys. It also writes the renderer's thumbnail and the
  entry, then drops the old signatures, so the catalog must be signed again.
- The tool writes only into `DIR` and never uses the network. Publishing a copy, as a commit to the public
  repository, is a separate step that the owner reviews.

## The launcher's side (`tools/char_store.py`, worker `characters --char-action store...`)

- **Address.** `https://raw.githubusercontent.com/irnutsmurt/Cubic.Heroes-Characters/main/` holds the repository's
  files as a static web site. There are no GitHub API calls, no account and no token, and nothing is ever sent but
  the request. `CUBICHEROES_STORE_URL` points a development build at another copy, over https, or over http on
  this computer only. For a copy on this computer, `CUBICHEROES_STORE_KEYS` may name a test keys file.
- **Never blocking.** The catalog, the thumbnails and the downloads are fetched by the worker process. The launcher
  draws on while they run, and a downloading card says DOWNLOADING. The worker's answers reach the UI thread by
  move, not copy.
- **When it asks.** The catalog is checked when the store tab is shown, at most every 10 minutes, and whenever the
  player presses *Refresh*. Each check is conditional (`If-None-Match` / `If-Modified-Since`), so an unchanged
  catalog costs one small `304` answer.
- **Rate limits.** A `429` answer, or a `403` that carries `Retry-After` or `X-RateLimit-Remaining: 0`, stops all
  requests (catalog, thumbnails, downloads) until the time it gives, or 15 minutes. The saved catalog is shown
  meanwhile.
- **Offline.** Without a connection the saved catalog is shown and the tab says so. With no saved catalog, the tab
  explains and offers *Refresh*.
- **Cache.** `<library>/store/`: `catalog.json` (the last accepted catalog), `state.json` (its ETag, its date, when
  it was checked, any rate-limit wait) and `thumbs/` (the launcher's pictures). Thumbnails are fetched in the
  background, a few at a time, and only the missing ones.
- **Downloads.** A character's file is downloaded only when the player chooses *Download* or *Download and
  install*. It is never downloaded twice: when its data's SHA-256 is already in the library, the launcher makes
  no request and says so. A downloaded character is recorded in the library with `source.kind = "store"`, its
  catalog id and its licence.

## What is checked, and what is never done

- Every file is data, never run or opened by another program.
- The catalog: JSON, at most 4 MB and 5,000 characters. Each field is checked for type and length. Ids and
  SHA-256s are hex. Paths are plain relative paths inside the repository: no scheme, no host, no `..`, no leading
  `/`. A redirect to another host is refused.
- A thumbnail: its size (at most 512 KB) and SHA-256 must match the catalog. It is decoded only in the renderer's
  own PNG form (8-bit RGB, unfiltered rows), at most 1024 x 1024, decompressed within its exact expected size.
- A character file: its size (at most 4 MB) and SHA-256 must match the catalog. It may hold only
  `character.json`, `character.edt`, `preview/thumb.png` and `preview/views.png`: anything else, a PS3 save file
  for example, refuses it. The data inside must be the character the catalog describes (`edt_sha256`). It then
  goes through the library's own import, like any character file: archive limits, the format, and the data's
  recorded SHA-256.
- The launcher holds no GitHub credentials or tokens, and the store needs none.

Only player-made characters belong in the repository. The game's own models, PS3 save files, account data and keys
never do. `build` removes everything but the character, and the review before publication confirms that each one is
the player's own creation and carries a licence its creator gave.

## Tests

`tests/test_char_store.py` covers the client without the network. It uses a copy built from made-up submissions
and signed with a key made for the test, and a fake server that answers with ETags, `304`, offline, rate limits and
tampered files, plus one run through a real local HTTP server. The checks:

- the catalog is cached and checked conditionally;
- only a catalog signed by a trusted key is accepted (unsigned, another key's, and changed after signing are all
  refused), and a launcher with no key refuses every catalog;
- an older catalog version, or a changed catalog without a new version, is refused;
- the catalog builds with creator, licence, attribution, versions, SHA-256s and sizes, and without the consent
  record;
- no licence without an explicit agreement;
- a new version of a character is recognised, and so is an older version in the library;
- an offline check shows the saved catalog, and a rate limit is honoured;
- a bad entry, including one with another licence, is left out alone;
- a newer schema or a foreign catalog keeps the saved one;
- addresses are checked, and the development keys file is honoured only for a copy on this computer;
- thumbnails are checked;
- a download happens once, can install too, and a tampered file or one with more than a character is refused;
- a redirect to another host is refused.
