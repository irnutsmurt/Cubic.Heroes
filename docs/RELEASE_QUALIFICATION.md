# Cubic.Heroes 0.1.0 for Windows: release qualification (local)

**Status: M11.3, 2026-11-17 (draft).** The owner's M11.3: a clean Windows release build, a fresh installation from
its package, the whole conversion, compilation, shader and gameplay flow, offline use, update, rollback, uninstall and
recovery, the dependency and licence audit, the boundary and private-information checks, and drafts of the release
notes, the installation instructions, the known issues and this checklist. **A local release candidate only:
nothing is published, and the release is not signed with the production key** (both the owner's decisions, not yet
given). Results are filled in as the runs finish; nothing below is marked passed without its evidence.

## 1. The candidate

| | |
|---|---|
| Package | `build/dist/DotHeroes-Setup-windows-x86_64.zip` (made by `tools/make_bundle.py --prebuilt --platform windows` on Windows 11; local, not committed) |
| Contents | `CubicHeroes.exe` (the launcher and the command line), `SDL2.dll`, `licenses/`, `LICENSE.txt`, `README.txt`, `worker/` (the worker program with its Python runtime, the compiler (Zig), the prebuilt runtime and libraries, the lifting tools, `licenses/` with the sources the LGPL asks for) |
| Version | 0.1.0 (tools/version.py) |
| Built from | the commit that adds this file (the package is remade from it) |

## 2. The checklist

| # | Item | How | Result |
|---|---|---|---|
| 1 | A clean release build | `tools/make_bundle.py --prebuilt --platform windows`: the runtime library rebuilt from clean, the worker frozen, the launcher built; the boundary check of the folder and of the zip | see 3 |
| 2 | Nothing game-derived in the package | `tools/boundary_check.py` on the folder and the archive (make_bundle) | pass |
| 3 | Nothing private in the package | `tools/push_guard.py package` (now also run by make_bundle before archiving) | **found and fixed**: section 3.1 |
| 4 | Licences of everything shipped | the package's binaries against `licenses/` | **found and fixed**: section 3.2 |
| 5 | A fresh installation from the package, the command line only | `tests/release_qualification.py` (unpack, `--install`, interrupted once and resumed) | pending |
| 6 | The shaders prepared | `--prepare-shaders` | pending |
| 7 | Never rebuilt without a reason | `--update` with nothing changed: nothing rebuilt | pending |
| 8 | Offline | every step with a dead proxy; `--check-update` answers "offline" at once (exit 0: a check never blocks play), nothing else needs a network | pending |
| 9 | The game played from the installation | `--play` (ended in order through its exit question); `tests/devtools_suite.py --installed` (developer mode on, then off) | pending |
| 10 | An update that rebuilds, and rollback | a lifting tool changed: `--update` makes a new build and keeps the first; the worker's `rollback` back to it; played again | pending |
| 11 | The disc image and the keys file untouched; no key in any file | size and time, SHA-256, before and after; every installation file searched for the keys' lines | pending |
| 12 | Uninstall | the launcher removed by its worker: the installation, saves and settings kept | pending |
| 13 | The portable root and the command line | `tests/cli_integration.py --with-game` (docs/M11_PLAN.md section 32) | pass: 17 of 17 |
| 14 | The unit tests | `tests/test_*.py` | pass: 215 tests in 17 files |

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

### 3.2 Licences

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
