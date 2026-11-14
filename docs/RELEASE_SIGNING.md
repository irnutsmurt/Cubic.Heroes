# Release signing: the key, signing a release, rotation and recovery

**Status: M9.3, 2026-11-14.** The owner's decision (docs/M9_LAUNCHER_PLAN.md section 7, decision 2): every release
manifest is signed with an Ed25519 key the owner holds; the launcher refuses anything unsigned or not signed by a
key it trusts. The private key is never in the repository, on a build machine or in CI.

## 1. What is signed, and what the launcher checks

A GitHub release carries its downloads (the launcher's package per platform), **release.json** (the manifest: the
version, the channel -- stable or prerelease --, the notes shown before the update, the minimum launcher version,
the id of the signing key, and for each download its name, platform, size and SHA-256) and **release.json.sig**
(the manifest's Ed25519 signature, 64 bytes).

The launcher's pipeline worker (tools/updates.py, tools/release_trust.py), before anything is installed:
1. reads the manifest and its signature (small: nothing else is downloaded yet);
2. takes the key the manifest names from the keys it trusts (config/release_keys.json, shipped with the launcher)
   -- a key it does not trust: refused (`untrusted_key`); no signature: refused (`unsigned`); a signature that does
   not verify (the manifest altered, or signed by another key): refused (`bad_signature`); no trusted key at all:
   every update refused (`no_trusted_keys`);
3. checks the manifest's own rules (its format, the version, each download's record);
4. downloads the platform's file and checks its size and SHA-256 against the manifest before it is kept
   (`bad_download`: removed).
A refused release is reported and skipped; the newest release that passes is offered. The verification is
OpenSSL's Ed25519 (the libcrypto Python's own ssl module carries), tested against RFC 8032's vectors
(tests/test_updates.py).

## 2. Making the key (once, by the owner)

On the owner's own machine, with the repository checked out (OpenSSL's command line: Git for Windows ships one):

```
py -3 tools/release.py keygen --out <a folder outside the repository>
```

- OpenSSL asks for a passphrase: the private key (`release-signing-key.pem`) is written encrypted with it.
- The tool prints the public key's entry -- `{"id": ..., "ed25519": ..., "added": ..., "note": ...}` -- for
  **config/release_keys.json**'s `keys`. Add it in a commit of its own; that file holds public keys only.
- Keep the private key offline (an encrypted removable drive, a password manager's file storage) and **make a
  second copy in another place**; keep the passphrase apart from both. Without the key no release can be signed
  for the launchers that trust it.
- Recommended: make a **second key** the same way (a backup key, kept in yet another place) and add its public key
  to config/release_keys.json too. A launcher trusts both, so losing one key (or retiring it) does not strand the
  players (section 5).

## 3. Signing a release

```
py -3 tools/release.py manifest --version 0.2.0 --channel stable --notes NOTES.md --min-launcher 0.2.0 \
    --key-id <the key's id> --out release.json  dist/CubicHeroes-windows-x86_64.zip dist/CubicHeroes-linux-x86_64.tar.xz
py -3 tools/release.py sign --key <folder>/release-signing-key.pem release.json
```

`sign` writes `release.json.sig` and then verifies it exactly as a launcher will (with config/release_keys.json).
Upload the downloads, `release.json` and `release.json.sig` to the GitHub release (tag `v0.2.0`; a prerelease is
marked as one on GitHub and has `--channel prerelease`). Before publishing, the downloads pass
tools/boundary_check.py as tools/make_bundle.py makes them; the release notes pass tools/push_guard.py's text check.

## 4. Rotation (a planned change of key)

1. Make the new key (section 2) and add its public key to config/release_keys.json **beside** the old one.
2. Publish a release signed with the **old** key that ships this keys file: every launcher that updates to it now
   trusts both.
3. Sign the following releases with the new key.
4. After a while (when players have had time to update), mark the old key `"revoked": true` in
   config/release_keys.json in a release; a revoked key is no longer trusted.

## 5. Recovery

- **The key is lost, a second trusted key exists**: sign with the second key (its id in the manifest); rotate to a
  new second key (section 4).
- **The key is lost and no other trusted key exists**: the installed launchers cannot be updated by any release.
  Make a new key, publish a release with the new public key in config/release_keys.json, and tell players (the
  release notes, the project's page) to download that launcher by hand once; it then updates normally. Their
  installs, saves and settings are untouched (a new launcher migrates nothing it does not have to).
- **The key may have been exposed**: treat it as lost -- revoke it (`"revoked": true`) in a release signed with
  the other trusted key, then make a new second key. A release signed with a revoked key is refused from the
  launcher that carries the revocation on.
- **A bad release was published**: players are not forced to update (updates are offered, never applied without
  the player); the previous build of the game stays for rollback in every install (docs/M9_LAUNCHER_PLAN.md
  section 10); publish a fixed release with a higher version.
