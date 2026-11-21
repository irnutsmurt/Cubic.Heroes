# Cubic.Heroes 0.1.0 for Windows: release qualification (local)

**Status: published 2026-11-18 as [v0.2.0-beta.1](https://github.com/irnutsmurt/Cubic.Heroes/releases/tag/v0.2.0-beta.1),
a pre-release** (the owner's authorization of 2026-11-18). M11.3 qualified the candidate locally (sections 2-3); the
release itself is recorded in section 8.

## 1. The candidate

| | |
|---|---|
| Package | `build/dist/DotHeroes-Setup-windows-x86_64.zip` (made by `tools/make_bundle.py --prebuilt --platform windows` on Windows 11; local, not committed) |
| Contents | `CubicHeroes.exe` (the launcher and the command line), `SDL2.dll`, `licenses/`, `LICENSE.txt`, `README.txt`, `worker/` (the worker program with its Python runtime, the compiler (Zig), the prebuilt runtime and libraries, the lifting tools, `licenses/` with the sources the LGPL asks for) |
| Version | 0.1.0 (tools/version.py) |
| Built from | the commit that adds B-014's fix (the package is remade from it; it is local and not committed) |
| The candidate qualified | the package before B-014's fix (SHA-256 e099d8db...) installed fresh and taken through every step; the final candidate (c56af08d...: the same but for the prebuilt native library with B-014's fix) applied to that installation as an update in place, then the installed checks again |

## 2. The checklist

| # | Item | How | Result |
|---|---|---|---|
| 1 | A clean release build | `tools/make_bundle.py --prebuilt --platform windows`: the runtime library rebuilt from clean, the worker frozen, the launcher built; the boundary check of the folder and of the zip | pass (rc 0; the runtime library rebuilt from clean with the build-tree compiler cache) |
| 2 | Nothing game-derived in the package | `tools/boundary_check.py` on the folder and the archive (make_bundle) | pass |
| 3 | Nothing private in the package | `tools/push_guard.py package` (now also run by make_bundle before archiving) | **found and fixed**: section 3.1 |
| 4 | Licences of everything shipped | the package's binaries against `licenses/` | **found and fixed**: section 3.3 |
| 5 | A fresh installation from the package, the command line only | `tests/release_qualification.py` (unpack, `--install`, interrupted once and resumed) | pass: in a folder named with an accent and Japanese; `--install` interrupted after 240 s (CubicHeroes ended from outside: the install left `partial`), `--install` again resumed it and finished in 224 s; `--status`: installed, the game inside the folder |
| 6 | The shaders prepared | `--prepare-shaders` | pass: 2458 pipelines, the cache valid for this GPU and driver (the install's own preparation; `--prepare-shaders` then had nothing to do: 1.4 s) |
| 7 | Never rebuilt without a reason | `--update` with nothing changed: nothing rebuilt | pass: `--update` with nothing changed: nothing rebuilt (0.9 s) |
| 8 | Offline | every step with a dead proxy; `--check-update` answers "offline" at once (exit 0: a check never blocks play), nothing else needs a network | pass (simulated: a dead proxy for every step, not a disconnected network): the install, the preparation, the updates and the game needed none; `--check-update` answered "offline" in 2.3 s |
| 9 | The game played from the installation | `--play` (ended in order through its exit question); `tests/devtools_suite.py --installed` (developer mode on, then off) | pass after B-014's fix: `--play` -- the game's own end, status 0, no crash or hang report; the installed checks -- developer mode on (its own test saves), a player's save continued, developer mode off (nothing of the tools active, the player's saves byte for byte) |
| 10 | An update that rebuilds, and rollback | a lifting tool changed: `--update` makes a new build and keeps the first; the worker's `rollback` back to it; played again | pass: a lifting tool changed -- `--update` relifted and rebuilt in 53 s into a second build, the first kept; the worker's `rollback` made the first active again, nothing rebuilt; `--play` again: the game's own end, status 0 |
| 11 | The disc image and the keys file untouched; no key in any file | size and time, SHA-256, before and after; every installation file searched for the keys' lines | pass: the image's size and time and the keys' SHA-256 the same after the install; none of the keys file's 56 key strings in any file of the installation (the lift's and the compiler's output and the disc copy, derived and large, not searched) |
| 12 | Uninstall | the launcher removed by its worker: the installation, saves and settings kept | pass: the launcher's worker removed CubicHeroes, its worker and libraries; the installation (builds, data, config, backups, logs, active.json) kept, its 12 save, backup and settings files byte for byte |
| 13 | The portable root and the command line | `tests/cli_integration.py --with-game` (docs/M11_PLAN.md section 32) | pass: 17 of 17 |
| 14 | The unit tests | `tests/test_*.py` | pass: 215 tests in 17 files |
| 15 | The launcher put back over the folder | the package unpacked over the installation again (after 12) | pass: the installation found as it was, none of its files changed |
| 16 | A new version's update in place | the new package over the folder, `--update` | pass: the changed prebuilt library noticed, the game relinked into a new build (the first kept), its shaders prepared (5 s) |

## 3. Found and fixed by the qualification

### 3.1 The build machine's user name in the prebuilt libraries

The package's privacy scan found the developer's home folder -- with the account name -- 404 times in
`worker/lib/libps3recomp_runtime.a` and `libdotheroes_native.a`: Zig compiles each object into its cache's `tmp/`
folder first, and the default cache is in the user's `AppData`; the objects record that path. No release was ever
published (the repository has none), so nothing left the machine. Fixed at the root: the prebuilt build uses a
compiler cache inside the build tree (`build/zig-cache-prebuilt`; tools/zig_build.py `build_prebuilt`, rebuilt from
clean when the cache moves), and make_bundle refuses to archive a package in which `push_guard.py package` finds a
home path with a private name or a private literal. (A player's own build already used a cache inside the
installation: the paths it records are the player's, on the player's machine.)

### 3.2 B-014: QUIT GAME's label took the developer tools' memory

The installed developer-mode-off check failed on the candidate: its log had the developer tools' "[dev]" line. QUIT
GAME's label (M11.2d) is set by one call into the game's Lua, which needs a little guest memory, and it used the
tools' 384 KB region -- in every player's game, with developer mode off. Fixed: the label has one 64 KB page of its
own with developer mode off; the tools' region only with it on (docs/M11_VALIDATION.md B-014). The fix is in the
prebuilt native library, so the candidate was remade with `--prebuilt`, and the qualification's installation took it
as a player would: the new package over the folder, `--update` (the changed library relinked into a new build).
Then the installed checks passed.

Two test faults found on the way, not the game's: the qualification first gave the keys file by a relative path from
another working folder (the install, rightly, did not find it), and the installed checks need a player's save to
continue (a new installation has none: one written by the game is brought in).

### 3.3 Licences

- The worker's Python runtime as PyInstaller packs it (Python 3.12, with OpenSSL, libffi, zlib, bzip2 and the rest of
  its Windows build), pycryptodome, and PyInstaller's bootloader shipped without their licence notices: now
  `worker/licenses/Python.txt` (Python's licence file, which carries its libraries' notices), `pycryptodome.txt`,
  `PyInstaller.txt` (its bootloader exception), copied from the Python the worker is made with; make_bundle fails if
  one is missing.
- at3_standalone (LGPL-2.1, the ATRAC decoder from PPSSPP) is compiled into the shipped runtime library: its licence
  was in the package, its source was not. Now `worker/licenses/source/at3_standalone.tar.gz` and a notice naming the
  ps3recomp revision, as OpenAL Soft's and FFmpeg's sources already were.
- Tcl/Tk left out of the frozen worker: it never opens a window (Tk is the source checkout's window only), so its
  runtime and its notices are no longer shipped.
- README's licence table: Dear ImGui (the launcher), the Python runtime and PyInstaller's bootloader added.

## 4. Release notes (draft, 0.1.0 beta)

**Cubic.Heroes 0.1.0** -- the first Windows beta of the native PC port of the US PlayStation 3 release of 3D Dot
Game Heroes, recompiled on your PC from your own disc.

- **Your own game, recompiled locally**: point the launcher at your disc (an ISO or a folder) and your keys file; it
  recompiles the game into a native Windows program on your PC (10-30 minutes once). No game code or data is in the
  download.
- **Native**: a Vulkan renderer, OpenAL Soft audio, FFmpeg for the movies; no emulator, no proprietary middleware.
- **One portable folder**: everything -- the recompiled game, saves and backups, settings, shader caches, logs --
  stays in the folder you unpacked into. Move it and it keeps working.
- **A controller-first launcher**, and a command line for other launchers and Steam (`CubicHeroes --play`,
  docs/CLI.md).
- **PC settings**: windowed, borderless or exclusive fullscreen; VSync; frame limits from 30 to 240 or unlimited with
  the game's speed unchanged; volumes; your own controller and keyboard bindings; an in-game menu (hold L3+R3, or F1).
- **QUIT GAME** on the title menu.
- **Safe updates**: the launcher updates itself and the game, rebuilding only what changed, backing up saves first and
  keeping the previous build to go back to.

Requirements: Windows 10 or 11 (64-bit), a GPU with a Vulkan driver, about 6 GB of free space, and your own copy of
the game (BLUS-30490) with its keys.

## 5. Installation (for the release page)

1. Unpack the zip into a folder of your choice (for example `D:\Games\Cubic.Heroes`).
2. Run `CubicHeroes.exe`. Choose your disc (ISO or folder) and your keys file; the launcher recompiles the game
   (10-30 minutes) and prepares its shaders.
3. Press Play. Your saves and settings stay in that folder.

Other launchers: `CubicHeroes.exe --play`. The full command line: docs/CLI.md.

## 6. Known issues (draft)

| | |
|---|---|
| B-011 | A hang once in the priest's arena, 13 minutes into a long fight (not reproduced since, in about 130 minutes of the same fight). The hang report now records every thread's stack. |
| B-007 | One effect frame left out when arriving at the castle battle's arena (its input is undefined on the console too) |
| Not yet validated | Linux (planned: docs/LINUX_PACKAGING.md); AMD and Intel GPUs; Windows 10; the rows `todo` or `partial` in docs/M11_VALIDATION.md (the minigames to a result, side quests, consumables, the editor's own world, new game plus's carry-over, a visual review of every map) |
| Minigames | entered and started; not yet played to a result in a test |
| The extra Block Defense stage | drawn pink in play: not yet compared with the original |

## 7. Blockers and decisions for the owner

| | Decision | Recommended |
|---|---|---|
| 1 | **Publishing**: a GitHub release of the candidate (not authorized) | after this checklist passes and the owner's own play of the candidate |
| 2 | **Signing**: the release manifest signed with the owner's key (docs/RELEASE_SIGNING.md; not authorized) | with the release |
| 3 | The owner's hands-on checks still open (docs/M11_VALIDATION.md: QUIT GAME with a keyboard and a mouse, with the launcher; the earlier milestones' hands-on lists) | before publishing |

## 8. The published release: 0.2.0-beta.1 (2026-11-18)

| | |
|---|---|
| Version | 0.2.0-beta.1 (semantic versioning; a beta's launcher is offered later betas -- the pre-release channel -- by default) |
| Tag | v0.2.0-beta.1, on 5b395eb; marked as a pre-release |
| Download | `Cubic.Heroes-0.2.0-beta.1-windows-x86_64.zip` (133,992,382 bytes, SHA-256 3d2081ddb5f31539395fa1846b16958ad4163c065c3e237dfedd80bed57d659b), unpacking to one `Cubic.Heroes` folder |
| Signature | `release.json` (the version, the channel, the notes, the download's size and SHA-256) signed by the owner's key 6b5e12ec2cc5791f (`release.json.sig`); `SHA256SUMS.txt` |
| Audits before upload | the boundary check (folder and zip), the private-information check (make_bundle now refuses on a finding), the licences and the sources the LGPL asks for (section 3) |
| The actual download verified | downloaded back from the release: the checksum, the manifest's hash and its signature (the launcher's own check); then a fresh installation of that download, offline, in a folder named with an accent and Japanese -- unpack, nothing installed, `--install` interrupted and resumed, status, shaders, `--update` with nothing to do, `--check-update` offline, `--play`, no key text: 9 of 9 |
| The update engine | against the live release: offered on the pre-release channel (signature verified), not on the stable one |

Two defects found on the way, fixed before the upload: the launcher's incremental build kept a 0.1.0 launcher in the
0.2.0-beta.1 package (a changed version define now rebuilds it: tools/build_launcher.py), and a moved installation's
`game` link (an absolute junction) pointed at the old place, so preparing shaders after a move failed (the worker
repairs it, and the preparation runs the build itself: 5b395eb). The package was then made again, re-uploaded and
verified again from the download as above.

**After M7** (docs/M7_HISTORY_PLAN.md; the owner wanted the history cleaned before the beta was public): the release
was returned to a draft (it had been public for about an hour), the history rewritten and force-pushed (the tag moved
to its rewritten twin 5b395eb, the same tree), and the release verified again from GitHub -- the download's checksum,
the manifest's hash and the owner's signature; a fresh offline installation of that download in a folder named with
an accent and Japanese, `--install` interrupted and resumed, status, shaders, `--update` with nothing to do,
`--check-update`, `--play`, no key text: 9 of 9; then the installed game's checks (developer mode on, a player's save
continued and played, developer mode off): pass. **Published again as a pre-release on 2026-11-18**, with the owner's
approval.

## 9. The candidate 0.2.0-beta.2 (2026-11-18)

The owner's two requirements before the beta is finalised -- M11.2f (the rendering resolution) and M11.2g (the button
prompts throughout the game) -- done and pushed (904a16f, f94459f); the owner chose 0.2.0-beta.2 for the rebuilt
package (beta.1 stays published; its players are offered beta.2 on the pre-release channel).

- **The package**: `tools/make_bundle.py --prebuilt --platform windows` from 12788d6 -- the runtime library rebuilt
  from clean (patches 0168, 0169 in it), the worker frozen, the launcher built:
  `Cubic.Heroes-0.2.0-beta.2-windows-x86_64.zip`, 134,213,208 bytes, SHA-256
  `2720d3d887db98c40cc619d797ee795ccb874dc766a88b423939cd5f5b50d606`. The private-information check (folder) and the
  boundary check (folder and zip, 19,187 files): nothing found. The Kenney prompt images (163, CC0) are in the
  game's own executable as it is built on the player's machine -- from the port's assets, no game data.
- **Qualified from the package** (tests/release_qualification.py, offline, a folder named with an accent and
  Japanese, F: drive): unpack, empty, install (interrupted after 240 s, resumed and installed in 226 s), status,
  shaders, `--update` with nothing to do, `--check-update` (offline at once), `--play` (the game's own end, status 0),
  the installed game's suite (developer mode on with its test saves, a player's save brought in and continued,
  developer mode off: the tools inert, the player's saves byte for byte), a lifting tool changed (`--update` relifted
  into a new build, 52 s), rollback (the first build again, played), no key text in the installation, the launcher
  removed (the installation, saves and settings kept), the package unpacked over the folder again: **15 of 15 pass**.
- **The manifest**: build/release/v0.2.0-beta.2 (local): the zip, SHA256SUMS.txt, release.json (version 0.2.0-beta.2,
  prerelease, key 6b5e12ec2cc5791f, the notes) -- to be signed by the owner.
- **The command line** (tests/cli_integration.py --with-game, the launcher of this version, the qualified
  installation copied): 17 of 17 pass -- with M11.2f's cases: `--resolution` refuses sizes outside 320x180..7680x4320
  and non-sizes (exit 2) and needs `--play`; a session's `--resolution 2560x1440` in effect (the game's log) and not
  kept in the settings file.
- **Signed and published** (2026-11-18): the owner signed the manifest with the release key (6b5e12ec2cc5791f);
  verified here (`tools/release.py verify`), tagged v0.2.0-beta.2 at 12788d6 (the package's source) and published as a
  **pre-release** with the zip, SHA256SUMS.txt, release.json and release.json.sig (the owner's standing approval to
  publish beta updates through the signing process). **Verified from GitHub**: the download byte for byte the
  qualified package, its checksum and the manifest's signature good; a fresh offline installation of the download
  (an accent and Japanese in the folder's name; `--install` interrupted and resumed, status, shaders, `--update` with
  nothing to do, `--check-update`, `--play`, no key text): 9 of 9 pass. 0.2.0-beta.1 stays published.

## 10. Acceptance criteria for every release, from 0.2.0-beta.4

Two defects reached players in 0.2.0-beta.3 although every test then in place passed:

- **The Linux launcher couldn't reach the update server.** Its packaged OpenSSL looked for the trusted certificate
  authorities in the build environment's folder, which no player has, so every check read as "offline". Every update
  test before then used a local mirror (a folder, or HTTP) or a dead proxy, never real HTTPS through the package's
  own OpenSSL.
- **No launcher ever installed a downloaded update** (Windows and Linux, since M9). The launcher downloaded and
  verified a release and said it would be applied on restart, but nothing started the swap. The tests drove the
  worker's swap themselves, never the installed launcher from download to restart, so a download was taken for an
  installation.

So each release candidate -- **the packages themselves, never a development checkout** -- must pass all of the
following before the owner is asked to approve it. **A result that can't be decided is never a pass**: the step is
run again until it passes or fails.

| # | Criterion | How | Packages |
|---|---|---|---|
| A1 | **Live HTTPS to GitHub**, with the package's own launcher and frozen worker and nothing helping them: SSL_CERT_FILE, SSL_CERT_DIR and the proxy variables removed | tests/live_update_check.py (release_qualification's mandatory `online` step) | the Windows zip, the Linux tarball, the Linux AppImage |
| A2 | The certificate store the packaged worker verified with: the system's (Linux: a distribution's bundle or certificate folder, the answer's `tls_store`; Windows: the Windows store); never the build environment's | the same, and tests/distro_check.py --live (Debian 11, Ubuntu 20.04 and 24.04, Fedora 41: a store found and used, or with none, "tls" -- never offline, never unverified) | Linux |
| A3 | **Discovery through the launcher**: `CubicHeroes --check-update --json` answers from the server (not offline, not an error) | A1's `cli` step | all three |
| A4 | The manifest's **Ed25519 signature** verified with the package's registered key (config/release_keys.json); nothing refused | A1's `newer` step | all three |
| A5 | **This system's package of this installation's kind** offered (the zip on Windows; a tarball installation the tar.xz; an AppImage the AppImage), never another system's or kind | A1; tests/test_launcher_update.py; tests/launcher_update_e2e.py's refusals | all three |
| A6 | **The download verified**: the real package downloaded over HTTPS and its size and SHA-256 checked against the signed manifest | A1's `download` step | all three |
| A7 | **An installed launcher installs a newer version and restarts into it**: a package of the candidate installed (with the game, in a folder named with spaces and non-ASCII characters), a newer package served over HTTPS; the update downloaded, verified, staged and swapped by the launcher's own commands; `--version` says the new version (the launcher and its worker); its files the new package's byte for byte; the installation's saves, settings, builds and shader caches untouched; the game brought up to date and played to its own end; the launcher's rollback, and the window's restart path | tests/launcher_update_e2e.py | the Windows zip, the Linux tarball, the Linux AppImage (`--key`) |
| A8 | **Refusals**: an altered manifest, an untrusted key, an altered package, another system's package, an untrusted HTTPS certificate, no server, the game running, nothing newer -- each refused (or nothing offered), the launcher and the installation unchanged byte for byte | tests/launcher_update_e2e.py; tests/test_launcher_update.py (also the version inside a package, another processor, too little disk space, a crash at every step of the swap and its recovery, one update at a time) | all three; the unit tests on CI (Windows and Linux) |
| A9 | Timeouts, interrupted downloads, TLS failures (untrusted CA, another host's certificate, an expired one) | tests/test_update_tls.py, tests/linux_update_check.py --https | CI; Linux |
| A10 | Windows' existing installations still offered the release: the published Windows beta.2 and beta.3 workers against GitHub | tests/live_update_check.py on those packages, after publication | Windows |
| A11 | **The owner-signed manifest itself**: the A7 run again with the release's own manifest and signature (`--manifest`, `--signature`): a package of the candidate's code labelled with an older version, trusting only the project's key, installs the signed candidate and restarts into it | tests/launcher_update_e2e.py | the Windows zip; the Linux tarball and AppImage |
| A12 | **The release notes' hand upgrade**, from each published beta whose launcher can't install updates itself (beta.2 and beta.3): its published package installed with the game from the disc; the candidate's package unpacked over it as the notes say; the launcher started plainly (its window, hidden by the desktop guard) brings the game up to date by itself **and records it** (`downloads/game-up-to-date-with.txt`, also on an installation with no `downloads/`), so the next start doesn't update the game again; the saves and settings unchanged; the installation's status, play to the game's own end; where the upgrade made a new build, the game's rollback, still in place after the launcher is started again | tests/cli_integration.py (the marker on an installation without `downloads/`); the candidate's qualification run on each system | the Windows zip; the Linux tarball and AppImage |

The local-mirror tests stay (tests/test_updates.py, tests/test_launcher_update.py, tests/linux_update_check.py): they
cover what a live server can't be made to do -- altered files, cut connections, refused keys -- deterministically.

**Verdicts for A1-A3, A10**: PASS only on the expected answers. FAIL on a "tls" error, a refused manifest, the wrong
package, or an "offline" answer while the machine itself can reach GitHub's API (an independent request with the
system's own trust). INCONCLUSIVE (exit 2) when the machine can't reach GitHub either, or GitHub answers 5xx or a
rate limit: an outage says nothing about the package, so the step is run again, and the release waits.

**After publication**: A1 against the published files of all three packages, and A10.
