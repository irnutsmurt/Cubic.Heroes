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
| Tag | v0.2.0-beta.1, on c00feaa; marked as a pre-release |
| Download | `Cubic.Heroes-0.2.0-beta.1-windows-x86_64.zip` (133,992,382 bytes, SHA-256 3d2081ddb5f31539395fa1846b16958ad4163c065c3e237dfedd80bed57d659b), unpacking to one `Cubic.Heroes` folder |
| Signature | `release.json` (the version, the channel, the notes, the download's size and SHA-256) signed by the owner's key 6b5e12ec2cc5791f (`release.json.sig`); `SHA256SUMS.txt` |
| Audits before upload | the boundary check (folder and zip), the private-information check (make_bundle now refuses on a finding), the licences and the sources the LGPL asks for (section 3) |
| The actual download verified | downloaded back from the release: the checksum, the manifest's hash and its signature (the launcher's own check); then a fresh installation of that download, offline, in a folder named with an accent and Japanese -- unpack, nothing installed, `--install` interrupted and resumed, status, shaders, `--update` with nothing to do, `--check-update` offline, `--play`, no key text: 9 of 9 |
| The update engine | against the live release: offered on the pre-release channel (signature verified), not on the stable one |

Two defects found on the way, fixed before the upload: the launcher's incremental build kept a 0.1.0 launcher in the
0.2.0-beta.1 package (a changed version define now rebuilds it: tools/build_launcher.py), and a moved installation's
`game` link (an absolute junction) pointed at the old place, so preparing shaders after a move failed (the worker
repairs it, and the preparation runs the build itself: c00feaa). The package was then made again, re-uploaded and
verified again from the download as above.
