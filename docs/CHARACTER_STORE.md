# The Character Store (M12, read-only)

The Character Store is the third tab of the launcher's Characters page, beside *My Library* and *Installed*. It shows
the approved community characters from the public characters repository
(`https://github.com/irnutsmurt/Cubic.Heroes-Characters`). The player browses them with the same cards, search,
filters and order as the library, and downloads one into *My Library* (and, if asked, into the game) when they
choose it. Adding approved characters to the repository needs no new Cubic.Heroes release.

The store is **read-only**: publishing and submissions are separate and are not part of the launcher. It is
available to every player from 0.2.0-beta.6. The first published catalog holds the 352 characters recovered from
the 2010 Hall of Heroes and a 2010 GameSpy article, all in the historical category (see below).

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
    "shape_sha256": "... (optional: the model's hash, for 'the same model in your library')",
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
  - The private key is the owner's, kept offline, and is a key of its own, never the release key (see "The catalog
    key" below).
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

## Two categories: community and historical

Each entry has a `category`. The two kinds are kept apart in the catalog, in the tool and in the launcher:

- **`community`:** submitted to the store with the creator's explicit agreement to CC BY 4.0. `license` is
  `CC-BY-4.0`, and `creator` and `attribution` are required (`char_catalog.py build`, from a submission record with
  the consent).
- **`historical`:** recovered from an earlier community site that no longer exists, such as the Hall of Heroes
  (2010).
  - **No licence is claimed.** `license` is absent; an entry that names one is refused, and `char_catalog.py
    historical` refuses records that carry one.
  - **Rights statement.** The launcher shows that all rights remain with the creator.
  - **Provenance.** The entry keeps the original name and the creator as recorded. Its `source` holds the
    collection, the original date, the original address and the Wayback Machine capture it was recovered from.
  - **After download.** A character downloaded into a library records `source.kind = "historical"` with the same
    provenance and rights statement, and keeps them in any `.chchar` it is exported to.
- **Removal requests.** The signed catalog's `removal_url` points to the repository's `REMOVAL.md`: a GitHub issue
  (removal-request template), no proof asked beyond naming the character and stating the requester's relation to
  it, removal usually within 7 days.
  - `char_catalog.py remove --repo DIR --id ID` takes out the entry and its files and raises the catalog version.
    The catalog is then signed and published again. Nothing about who asked is written into the repository.
  - Launchers stop offering the character at their next catalog check. Copies players already downloaded stay
    in their libraries.

```
python tools/char_catalog.py historical --repo DIR RECORDS.json   # [{character, original_name, creator, collection,
                                                                   #   original_date, original_url, archived_url}]
python tools/char_catalog.py remove --repo DIR --id ID
```

## Official content: characters a player already has from the game

Some catalog entries are characters that also come with official game content. The US v1.01 update (BLUS30490-01.01)
brings 51 characters; 45 of them are recovered Hall of Heroes characters the historical category also preserves.
The two are separate sources. The catalog keeps every historical record as it is -- downloadable, with its
attribution, provenance and removal requests unchanged -- and only an installation's state changes.

- **The mapping.** `config/official_characters.json` is explicit and versioned.
  - Per official content package (the game itself, an update), it lists the characters by slot. Each one names the
    catalog entry it is (or none) and how that was established: `identical` (the same character file) or
    `same-model` (every voxel, colour, setting and pose box the same; `differs` says what isn't, e.g. the in-game
    name).
  - Characters with no catalog entry are listed with none. No entry is made for them automatically; one can be
    added through the normal catalog process like any other character.
  - It holds no data, hashes or fingerprints of the official files; the verification against a player's own files
    is the maintainers' and stays private.
  - Japanese and European releases add their packages to the same file.
- **What an installation has.** `tools/official_content.py` reads it from the installation's own record, never from
  the game's character files:
  - `data/install.json`'s `official_content` list, written by whatever installs, removes or replaces a package;
  - otherwise the installation's recorded program, which identifies the game itself.
  - It is worked out at every look, so an update installed, removed or replaced changes the store's state the next
    time it is shown.
- **In the store.** An entry that an installed package includes is shown as **INSTALLED**, with its source ("Included
  with the official v1.01 update"). It is never downloaded, copied into the library or installed into the saves: the
  game reads it from its own files. Without the package (v1.00), it is offered like any other entry.
- **One entry, sources kept apart.** Each entry carries `sources`:
  - official package;
  - library: `origin` `store` (downloaded from this store) or `import` (the player's own file), and `same_model`
    when the library copy is the same model under another name or file -- when the catalog gives the entry's
    `shape_sha256`;
  - the game's saves.

  A character in several places is one entry, never labelled a store download when it came with an update.
- **The player's own files stay theirs.** The library has no official-content rule: a player may import any
  character file, official ones included. Nothing here publishes, uploads or redistributes official content.

## The catalog key

The catalog has its **own** Ed25519 key, never the release key (docs/RELEASE_SIGNING.md):

- **Separate trust.** A launcher trusts `config/catalog_keys.json` for the catalog and `config/release_keys.json`
  for software. A key that appears in `release_keys.json`, revoked or not, is never trusted for the catalog, even
  if it is listed there. `char_catalog.py` refuses to sign with a release key or to add one to the catalog keys.
- **Nothing executable.** The update check reads only `release_keys.json`. The catalog lists data only: character
  files that are imported and never run. So a stolen catalog key can at worst sign a bad catalog. Its downloads
  are still checked, and its entries can't be anything but characters. It can never authorise an update.
- **Private keys stay offline.** They live with the owner, encrypted with a passphrase, and are never in the
  repository, a build machine, CI, a release or the NAS. The repository holds public keys only.
  `tests/test_char_store.py` fails if a tracked file contains a private key, or if the shipped
  `config/catalog_keys.json` lists a test key.
- **Test keys.** The tests make throwaway keys in memory. A local copy of the repository for development is signed
  with a throwaway key under `build/tmp` (never tracked), and that key is deleted when the tests end, before any
  release.

### Making the key (once, by the owner)

```
py -3 tools/char_catalog.py keygen --out <a folder outside the repository>
py -3 tools/char_catalog.py keys --add <that folder>/catalog-signing-key.pub.json
```

The first command writes `catalog-signing-key.pem`, encrypted with a passphrase that OpenSSL asks for, and its
public entry. The second adds the public entry to `config/catalog_keys.json`; commit it on its own. Keep the
private key offline with a second copy somewhere else, and keep the passphrase apart from both. A **backup key**,
made the same way and added beside the first, lets the catalog survive the loss of one.

### Rotation (planned)

1. Make the new key and add it beside the old one (`keys --add`). Ship that keys file in a launcher release; the
   release key signs it, as every update.
2. Sign each catalog with **both** keys (`sign --key old.pem`, then `sign --key new.pem`). Launchers that trust
   either key accept it.
3. Once players have the launcher that trusts the new key, revoke the old one (`keys --revoke <id>`, in another
   launcher release) and stop signing with it. Its entry stays in the file, marked `revoked`.

### A lost key

Nothing is exposed, but no new catalog can be signed with that key.
- With a backup key, sign with the backup.
- Without one, make a new key and add it to `config/catalog_keys.json` in a launcher release.
- Meanwhile, launchers keep showing the last catalog they saved. The lost key need not be revoked, though
  revoking it does no harm.

### A compromised key

Someone else could sign catalogs with it.
1. **Revoke it** (`keys --revoke <id>`) and add a new or backup key, in a **launcher release** signed with the
   release key. A launcher learns of a revocation only through an update; the catalog can't revoke its own keys.
2. Sign the real catalog with the new key, and publish it with a `catalog_version` above every version published
   before.
3. **What a launcher does after the update.** It no longer trusts catalogs signed by the revoked key, including the
   one it saved: that catalog is dropped. It also ignores the rollback floor (below) that the revoked key's catalogs
   set. A version the attacker inflated, say 2,000,000, therefore can't lock players out of the next real catalog.
4. **Before the update.** A launcher could have shown or downloaded characters from a catalog the attacker signed.
   Those are still checked as characters, so nothing in them can run, and the update and its keys are untouched.
   Players can remove any such character from My Library. The library records which catalog id and version each
   character came from.

### Rollback protection

A launcher refuses a catalog whose `catalog_version` is lower than the highest one it accepted before, and a
catalog that changed without a new `catalog_version`. That highest version is kept with the key that signed it in
`<library>/store/state.json`. It holds across launcher restarts, and even when the saved catalog is deleted, for as
long as that key is trusted. Tested in `Keys.test_rollback_protection_survives_restarts` and
`Keys.test_a_revoked_key_is_no_longer_trusted`.

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
- keys: rollback protection holds across restarts and without the saved catalog; rotation works (a catalog signed
  by two keys is accepted by launchers that trust either); a revoked key is no longer trusted and its floor is
  ignored; a release key is never trusted for the catalog, and a catalog key can't sign a release; adding and
  revoking keys works; no private key in the repository and no test key shipped;
- thumbnails are checked;
- a download happens once, can install too, and a tampered file or one with more than a character is refused;
- a redirect to another host is refused;

`tests/test_official_content.py` covers official content, with made-up characters and a test mapping:
- the shipped mapping: US v1.01's 51 characters -- 39 identical, 6 same model (the name only), 6 with no catalog
  entry -- versioned, refused when malformed, and free of the official files' hashes or data;
- what an installation has: v1.00 from its program; v1.01 installed, removed and replaced; another release's package
  through the same mechanism; never a read of the game's character files;
- the store:
  - v1.00 shows nothing included and downloads as before;
  - with v1.01, the mapped entries are installed, never downloaded or written, and no entry is made for an
    official-only character;
  - the update removed, they are offered again;
  - a character the player already downloaded or imported is one entry with its sources apart, and the library is
    untouched;
  - the same model under another name is recognised;
  - computing the state writes nothing;
  - nothing publishes or uploads.
- two local-only checks (skipped without the maintainers' files): all 352 historical records kept, with the 45
  mapped entries among them; the mapping agrees with its private verification.
