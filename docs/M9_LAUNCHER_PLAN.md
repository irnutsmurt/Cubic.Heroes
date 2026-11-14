# M9: the launcher -- architecture and implementation plan, for review

**Status: reviewed by the owner 2026-11-13; the decisions and the owner's further requirements are in sections 7
and 8, and they take precedence over anything earlier in this plan that differs. Implementation starts once M8 is
closed, in the steps of section 4, each validated and recorded before the next.** The milestone as the owner set it
(docs/MILESTONES.md, "Roadmap: M9-M14", revised 2026-11-13): the launcher; shader and Vulkan pipeline
precompilation; installation; cache management; updates from GitHub Releases; rollback. Dusklight is the
inspiration for the overall launcher experience, not a model to copy.

Labels as in the other records: [C] read in the code, [M] measured, [I] inferred, [H] a hypothesis to test.

## 1. Where we start

**The setup app** (setup_app.py, Python/Tk, frozen with PyInstaller; tools/setup_pipeline.py) [C]:
- converts the player's disc into an install: verify, extract, decrypt, lift, the SPU tasks, build, finish;
- updates an install in place, redoing only the steps whose inputs changed (install.json's per-step fingerprints)
  and reusing compiled objects by fingerprint (<install>/data/cache);
- updates itself from the project's GitHub Releases (tools/self_update.py: the newest release's asset for the
  platform, staged, swapped in with the old one put back if the swap fails), adds a Steam shortcut, starts the game,
  writes dotheroes.cfg (the frame limit, fullscreen, the performance switches).

**What it does not do** [C]:
- keep the previous game build: an update rebuilds in place, so a failed or bad update leaves no way back;
- know about shaders or pipelines: the SPIR-V cache is preloaded by the game (PS3RECOMP_SPIRV_CACHE, validated
  against the build, glslang, GPU and driver); the native renderer's pipelines are not cached between sessions
  at all, and nothing is prepared before play (docs/SHADER_PREPARATION.md section 1);
- verify a release's origin beyond HTTPS from GitHub;
- back up saves, or bundle diagnostics.

**The install today** (setup_pipeline.Paths) [C]: `<install>/dotheroes[.exe]` and its libraries (SDL2, OpenAL
Soft, FFmpeg, with their notices); `<install>/data/{disc, gen, cache, gamedata}`; `<install>/data/install.json`;
`<install>/dotheroes.cfg`. The game itself exists only after the conversion: it is built on the player's machine
from their own disc, so whatever starts the experience must exist, and run, before any game does.

## 2. The reference: Dusklight

Dusklight (TwilitRealm's native PC port of Twilight Princess; https://twilitrealm.dev,
https://github.com/TwilitRealm/dusklight) is the reference for the overall experience. What its own pages and
release coverage show [verified 2026-11-13]:
- one download per platform from GitHub Releases (a zip on Windows, an AppImage on Linux), unpacked and run --
  no installer, no account;
- the player supplies their own dump of the game; nothing copyrighted ships;
- a native application with its own menus: settings, graphics backends, and (2.0) an integrated mod browser to
  discover, download and update mods inside the application, with the randomizer's updates arriving apart from
  the application's.
Not found on its pages [I]: whether the application updates itself in place, and how its first-run disc
selection looks. The plan takes the verified parts as the bar: **one download, your own disc, one native
application whose menus do the rest** -- installing, updating, preparing, playing, and later (M13) the community
library as Dusklight's mod browser does.

Our case differs in one way that shapes the design: Dusklight ships a finished program and reads the disc's data
at run time; here **the game program is generated on the player's machine** (the lift and the build, minutes the
first time, an update's rebuild after), so the launcher owns a build pipeline, not only a download.

## 3. Architecture

### 3.1 Three parts

| Part | What | Built from | Ships |
|---|---|---|---|
| **The launcher** | the application the player runs: the window, the menus, updates, the caches, the saves, diagnostics; starts the game | this project's sources, prebuilt | in the release download |
| **The pipeline worker** | the conversion and the rebuilds: today's tools/setup_pipeline.py and the lifting tools, headless, reporting progress as JSON lines on stdout | the existing Python tools (frozen, as today's app) | in the release download, inside the launcher's folder |
| **The game** | dotheroes, generated from the player's disc; also runs headless for the shader and pipeline preparation (section 3.4) | the player's lift and this project's port, built on the player's machine | never (generated locally) |

Separating the worker from the window keeps the Python lifting tools where they are, lets the launcher be a
small native program, and makes the pipeline testable without any UI (tests/player_install_test.py drives the
worker as today).

### 3.2 The launcher's technology (decision 1)

**Decided: native C++, SDL2 + Vulkan, Dear ImGui** (MIT), one executable per platform:
- SDL2 and Vulkan are already the game's own dependencies (shipped, licensed, validated on this PC); the
  launcher looks and behaves the same on Windows and Linux, and is navigable with a controller (SDL's game
  controller layer, as the game's);
- a native window fits Dusklight's single native application and the project's native-port direction, starts
  instantly, and needs no browser engine;
- the UI is drawn by the launcher itself (a theme of our own, the owner's `button assets/` prompts), so its look
  is ours to make polished.
**The alternative**: keep evolving the Python/Tk app (least new code; Tk's look and controller support are
limited, and the frozen Python runtime starts slowly). A web-view shell (Tauri, Electron) is not recommended:
a browser engine for a few pages, and Electron's size.

### 3.3 The install, with build slots (rollback)

```
<install>/
  launcher/                 the launcher and the worker (replaced by an update of the launcher)
  builds/<build-id>/        a generated game build: dotheroes, its libraries, build.json
                            (the release, the lift's fingerprint, the toolchain, the time it was made)
  active.json               the build the launcher starts, and the one before it (rollback)
  data/disc/                the player's extracted disc (unchanged by updates)
  data/gen/<lift-fp>/       the recompiled code, by the lift's fingerprint (an update with the same lift reuses it)
  data/cache/objects/       compiled objects by fingerprint (shared by every build: an update compiles what changed)
  data/cache/shaders/<build-id>/   SPIR-V and the pipeline cache, per build (section 3.4)
  data/gamedata/            the saves -- never written by the launcher or an update
  backups/saves/<time>/     save backups (section 3.6)
  config/                   dotheroes.cfg (the game's settings), launcher.json (the launcher's)
  logs/                     the launcher's, the worker's, the game's last sessions
```
- **An update builds into a new slot** (`builds/<new-id>`) while the active one stays playable; the new build is
  checked (a headless self-check: the memory test, then the preparation pass) and made active only then. The
  previous build is kept (one by default: decision 3) with its shader caches; **rollback** switches `active.json`
  back -- instant, offline, no rebuild. A build that fails to start twice in a row offers the rollback by itself.
- Objects and lifts are shared by fingerprint, so a build slot costs its executable and libraries (~100 MB), not a
  whole rebuild; the cost on disk is measured in M9.1.
- **Existing installs** (today's layout) are migrated in place on the first run of the launcher: the executable
  moved into a slot, the data where it was, nothing re-extracted or relifted.

### 3.4 Shaders and pipelines: prepared before play, caches managed

This is docs/SHADER_PREPARATION.md's plan (steps 0-6, requirements 1-6), taken into M9 as its core:
- **step 0 first**: the cold and warm costs of the native release measured (empty caches, the driver's own cache
  pointed at an empty folder), so the rest is sized by evidence;
- **a pipeline cache for the native renderer** (VkPipelineCache on disk, validated as the SPIR-V cache is: the
  build, glslang, the GPU, the driver and its pipeline-cache UUID; size-bounded; corruption detected);
- **a variant manifest** (each translation's and pipeline's key and where it came from), shipped with the release
  from all test routes and sessions, coverage stated by source, never claimed for the whole game;
- **the preparation pass in the game itself**: `dotheroes --prepare <manifest>` compiles the manifest's SPIR-V and
  pipelines on worker threads, headless, reporting progress to the launcher -- the game's own translator and
  renderer, so what is prepared is exactly what the game will ask for. The launcher runs it after an install or an
  update, and on its own when the caches are invalid (a driver, a GPU or the build changed: the launcher reads the
  same validation record before every launch); the game also runs it during the title for anything left;
- **cache management in the launcher**: each cache's state (valid for this build, GPU and driver; its size), a
  rebuild on request, the old builds' caches removed with their slots.

### 3.5 Updates from GitHub Releases

- **What a release is**: the tag vX.Y.Z; per platform the launcher's download (the launcher, the worker, the
  lifting tools, the prebuilt runtime, the port's sources, the configuration, the licences and FFmpeg's source as
  today's make_bundle.py assembles them -- no game data, checked by tools/boundary_check.py); a release manifest
  (the version, each asset's SHA-256, the minimum launcher version, the changelog).
- **Origin** (decision 2): the manifest signed with an Ed25519 key the owner holds (never in the repository or the
  build machines' files); the launcher carries the public key and refuses an unsigned or mismatched download.
  Without signing, HTTPS from GitHub and the checksums only.
- **The flow**: a check at start (and on request; never blocking play; offline is normal), the changelog shown,
  the download staged and verified, then (a) the launcher updated if its part changed (today's self_update.py
  swap, kept), (b) the game rebuilt into a new slot by the worker -- only the steps whose inputs changed (the lift
  when the tools or the configuration changed; objects reused by fingerprint), (c) the preparation pass, (d) the
  new slot made active. Any failure leaves the active build untouched and says why.
- **Channels** (decision 4): stable releases; prereleases opt-in.
- Saves are never touched; a save backup is made before an update (section 3.6).

### 3.6 Saves and diagnostics (decision 5: in M9; sections 8.6, 8.7)

- **Saves**: a backup of `data/gamedata` before every update and on request; a list to restore from (the game
  closed); the backups kept by count. The launcher never writes into `data/gamedata` otherwise.
- **Diagnostics**: a report bundle made on request -- the launcher's, the worker's and the last game sessions' logs,
  the build and release ids, the GPU, driver and OS -- with the project's privacy rules applied before anything is
  written: no saves (they hold the player's hero name), no user or home paths (replaced by placeholders), no
  account names; shown to the player before it is saved. No telemetry: nothing is sent anywhere by the launcher
  except the update check (decision 6).

### 3.7 Settings

M10 is the settings milestone (docs/PC_EXPERIENCE.md sections 1-7b). M9 builds the architecture (decision 7,
section 8.9): one authoritative configuration that the launcher's settings page and the game's own menus both read
and write, validated, with defaults and restart-required settings marked; today's options (the frame limit,
fullscreen, the performance switches) move onto it. M10 adds the rest.

## 4. Implementation, in steps (each its own change, tested and recorded)

| Step | What | Done when |
|---|---|---|
| M9.0 | Measurements: docs/SHADER_PREPARATION.md step 0 (cold and warm, empty caches, the river, the fields, the dungeon, the menus); a build slot's size on disk; the worker's progress protocol written down | the numbers recorded; the protocol reviewed |
| M9.1 | The install with build slots and active.json; the existing layout migrated; rollback; the worker headless with JSON progress | tests/player_install_test.py: fresh, update, relift, settings as today, plus migration from today's layout, an update into a new slot, a failed update leaving the old one active, a rollback |
| M9.2 | The native renderer's pipeline cache; the variant manifest; `dotheroes --prepare` | a warm run creates its pipelines from the cache; the tamper tests (driver, GPU, build) start empty and say so; the preparation pass's coverage reported by source |
| M9.3 | The update engine: the release manifest, checksums (and signatures, decision 2), staging, the slot build, the switch | an update from the previous release's install; a corrupted or unsigned download refused; offline start; an interrupted download resumed or discarded |
| M9.4 | The launcher (decision 1): first run (your disc, your keys, the install folder -- the conversion with its progress), the library page (play, the build and version, an update and its changelog), the caches page, the saves page, the diagnostics page, the settings page's shell; controller navigation | each page's behaviour tested headless where it can be (the worker, the update engine, the caches) and by a drive where it cannot |
| M9.5 | Packaging (make_bundle.py: the launcher and the worker, the licences), the launcher's self-update, the Steam shortcut, the documentation; the owner's acceptance | a fresh install and an update on Windows; the same on Linux as far as M11's platform validation reaches; the owner's acceptance session |

Order: M9.0 first (nothing sized without numbers), M9.1 and M9.2 independent of each other, then M9.3 on M9.1, then
M9.4 over all of them, then M9.5.

## 5. Tests

- The install test (tests/player_install_test.py) extended: migration, slots, rollback, a failed update, an update
  from the previous release, offline, a refused download.
- The caches: requirement 6 of docs/SHADER_PREPARATION.md (cold, warm, after a driver or build change), the hitch
  accounting (each on-demand compile during play logged with the frame it delayed).
- The report bundle checked by tools/push_guard.py's rules (no home path, user or account name) and by a test
  that no save is in it.
- The boundary: tools/boundary_check.py on the release download (as today), and on the launcher's own files.

## 6. Constraints carried over

The player-side boundary (no game data, code or proprietary middleware in anything distributed); the licences
shipped with every third-party part (OpenAL Soft, FFmpeg with its source, SDL2, Dear ImGui if chosen); the
privacy rules for anything public or bundled; the release's commit identity and push guard; M7 unauthorized.
Downloads during development (Dear ImGui's source, a signing tool) are asked for before they are made.

## 7. The owner's decisions (2026-11-13)

1. **The launcher's technology: native C++, SDL2, Vulkan and Dear ImGui**; the existing Python pipeline stays, as
   the headless worker.
2. **Signed releases: Ed25519 signatures on the release manifest**, the private key held by the owner only.
3. **Previous builds kept for rollback: one by default**, the number configurable.
4. **Channels: stable by default, prereleases opt-in.**
5. **Save management and diagnostics are part of M9.**
6. **No telemetry.**
7. **Settings from both the launcher and the game's own menus**, over one authoritative configuration.

## 8. The owner's requirements (2026-11-13)

What the review added; each is a requirement of M9 and is checked off in its step's record.

### 8.1 The launcher

The launcher **replaces the setup application completely**: it is the single entry point for installing,
configuring, updating, maintaining and playing.
- A visually polished consumer application, not a developer's debugging interface; Dusklight as inspiration, not a
  visual clone.
- Keyboard, mouse and controller navigation throughout; a responsive, DPI-aware interface; the window's size and
  position remembered.
- The Steam shortcut; no unnecessary console windows (the worker and the preparation pass run windowless).
- A missing, unusable or unsupported Vulkan device handled gracefully: the launcher still opens (a fallback way to
  draw its window), and diagnostics and recovery stay reachable.
- The interface laid out to take later milestones' pages (M13's community character library, M12's optional graphics
  enhancements) without rework.

### 8.2 The first-run installation wizard

A complete graphical installation; no command line and no separate setup application, ever.
- The player's original PS3 ISO, and its matching decryption key file, chosen with a file browser or by
  drag-and-drop.
- The ISO, the supported game version and the key's match validated before anything else happens.
- The installation folder chosen, its free disk space checked against what the install needs.
- Progress through decryption, extraction, recompilation, the native executable's build, and the shader and
  pipeline preparation: the current stage, the overall progress, the elapsed time, and the remaining time where it
  can reasonably be measured.
- Cancel, retry, and recovery from an interrupted installation.
- Actionable errors for an invalid ISO, a wrong key, an unsupported game version, too little disk space, a failed
  compilation; expandable technical details for troubleshooting.
- Existing installs (the setup app's) detected and migrated without unneeded recompilation.
- **Decryption keys never in logs, diagnostics, backups or anything distributed.**
- Then straight to the normal Play screen. The ISO and key are not asked for again except to reinstall or repair the
  game's data, and the installed game does not depend on the ISO staying where it was.

### 8.3 Release signing

- Ed25519-signed release manifests; the private key under the owner's control, outside the repository and CI.
- Key generation, signing, recovery and rotation documented.
- The signature and every asset's checksum verified before anything is installed; invalid or unsigned updates
  rejected.

### 8.4 Updates and rollback

- GitHub Releases; stable by default, prereleases opt-in; release notes shown before an update.
- No forced updates; offline play never blocked.
- One previous working build kept by default, the retention configurable; rollback offline and without
  recompilation.
- The active build never modified until its replacement has passed validation.
- Unchanged compiled objects, generated code and other valid cached parts reused.
- Interrupted updates, too little disk space, corrupted downloads and failed compilations handled.
- Saves and configuration independent of the build slots; rollback validated, and compatibility with existing saves.
- No cache entry or generated code deleted while a retained build needs it.
- The launcher's own self-update, with recovery if it fails.

### 8.5 Shader and Vulkan pipeline preparation (a core requirement)

- Both the shader variants and the Vulkan graphics pipelines compiled before gameplay.
- Prepared automatically after an install, a relevant update, a GPU change or a driver change.
- Progress, elapsed time and the estimated remaining work shown; cancel, skip and resume.
- Caches validated, corrupted or invalid ones rebuilt; a manual rebuild; cache status and disk use shown; valid
  parts kept rather than everything rebuilt.
- Enumeration from the player's own game data investigated, to cover more than the recorded test routes.
- Distributed manifests hold only independently authored metadata, never extracted shaders or assets.
- Gameplay stutter measured after preparation with genuinely cold driver caches.
- Unseen variants compiled safely when they appear, and recorded for the next preparation.
- The real shader and pipeline coverage reported; complete coverage never claimed without evidence.
The aim is to minimize shader-related stutter in play, not to fill a cache.

### 8.6 Save management

- Manual backups; automatic backups before updates; restore from any backup; retention configurable; dates and
  sizes shown.
- Confirmation before anything destructive; existing saves never overwritten or deleted silently; saves never
  stored inside a build slot.

### 8.7 Diagnostics and logging

- Logs of the launcher, the installation, updates, compilation and the game; log levels, rotation and retention.
- Useful error messages with expandable technical details.
- The installed version and build, the operating system, the GPU and its driver shown.
- Privacy-sanitized diagnostic exports on request, their contents reviewed by the player before export; never game
  assets, decryption keys, saves, personal paths, account names or other sensitive information.

### 8.8 Privacy and network

No telemetry. In M9 the network is used only for updates and for downloads the player explicitly asks for during
installation or repair. The launcher and the installed game stay usable offline. M13's community browsing will be
separate and started by the player.

### 8.9 Settings

- Reachable from the launcher and from the game's own menus, over **one authoritative configuration**, so the two
  always agree.
- Applied live where possible; settings that need a restart marked as such; values validated; defaults restorable;
  an invalid or corrupted configuration file handled gracefully.
- M9 builds this architecture and exposes the options that exist today; M10 adds the remaining PC settings and
  controls.
- **The monitor's refresh rate and the desktop's resolution are never changed.**

### 8.10 Steps and validation

The steps of section 4: M9.0 baseline measurements and the worker's progress protocol; M9.1 the installation's
architecture, build slots, migration and rollback; M9.2 shader and Vulkan pipeline preparation; M9.3 signed updates
and release management; M9.4 the native launcher and the installation wizard; M9.5 packaging, integration,
validation and the owner's acceptance. Each validated and documented before the next.

Tested across them: fresh installs, migration of an existing install, wrong ISOs and keys, interrupted installs,
failed updates, rollback, offline use, cache invalidation, corrupted caches, too little disk space, saves preserved.
The automated gameplay regression suite is kept running, and validation evidence is kept outside the automatic
cleanup folders.

### 8.11 Scope

No M10-M14 work during M9 unless the owner authorizes it (M10 now also removes the game's original PS3 INSTALL
option; M11 adds Linux and AMD validation). Every architecture, licensing, privacy and distribution safeguard stays
binding: no game data, proprietary implementation code, decryption keys or locally generated game executables
distributed. M7 stays unauthorized.

## 9. M9.0: the baseline and the worker's protocol (2026-11-14) [M, C]

**The worker** (tools/worker.py; docs/M9_WORKER_PROTOCOL.md): the existing pipeline run headless with one JSON
event per line -- `hello`, `plan`, `stage`, `progress` (the step's fraction, the overall fraction, the elapsed
time, the remaining time and what it rests on), `log`, `cancelling`, `result` -- and the probes the wizard needs
before anything is written: `probe-disc` (PARAM.SFO's title and version and EBOOT.BIN's SHA-256: another game,
another version, an encrypted or damaged image each named) and `probe-keys` (the keys file, or an EBOOT.elf,
decrypting this disc's program to the supported one, in memory); `space`; `info`; `install` and `update`.
Cancelling: a `cancel` line on stdin, or stdin's end (the launcher gone); a step that does not call back within
5 s is ended with the worker, whose job object (Windows) or process group ends everything it started. Failures
carry a code (`unsupported_version`, `wrong_key`, `disk_space`, `compile_failed`, ...) with a sentence for the
player and the technical details apart; the keys' contents are never in an event, a log or an error. The setup
app's settings helpers moved to tools/game_settings.py (unchanged) so the worker needs no window toolkit; the
frozen app runs the worker as `--worker`. tests/test_worker.py (19 tests): the framing, the exit codes, the three
ways a cancel ends, the probes on made-up discs and keys and on the real image and keys where present locally
(their output checked for the keys' contents), the remaining time on a reference and a slower PC.

**A fresh install through the worker** (the reference PC; the disc image in the file cache): 352 s -- verify 2.3,
extract 24.6, decrypt 0.5, the PPU recompile 39.1, the SPU programs 11.8, **the build 273.9** (76 files, ten at a
time, 3.6 s each on average; the link 0.27 s), finish 0.03. The first version's remaining time was badly wrong
(80 s said with 340 s to go: placeholder weights, and the fast disc steps taken for this PC's speed); with the
measured weights and the speed taken from the compute-bound steps only, a replay of the same install's events is
within 11% of what remained from the first second to 80% done, and about 20 s long at the end.

**On disk** (the same install): the extracted disc 3.2 GB, the recompiled code 166 MB, the compiled objects 99 MB,
the game and its libraries 93 MB. **A build slot** (section 3.3) is the 93 MB; an update whose lift changed adds a
second recompiled tree (166 MB) while the previous build is kept, one whose port changed only the objects that
changed. The worker asks for 6 GB free for an install and 1.5 GB for an update.

**Shaders and pipelines, cold and warm** (docs/SHADER_PREPARATION.md step 0; build/m9/m90_measure.sh on rel51:
the M8 release with the native renderer's compile accounting -- each pipeline's first use timed in its parts, a
`[vk-compile]` line per translation and pipeline with VK_COMPILE_LOG=1, the totals in the renderer's report).
Four scenes, three cache states each: **cold** (our SPIR-V cache empty and NVIDIA's own disk cache pointed at an
empty folder, `__GL_SHADER_DISK_CACHE_PATH`), **ours** (our SPIR-V cache kept, NVIDIA's emptied again), **warm**
(both kept):

| Scene | Pipelines | Cold: in all (glslang; the driver) | Ours: in all (the driver) | Warm: in all | Cold: the worst frame (compiles in it) | Warm: the worst frame with a compile |
|---|---|---|---|---|---|---|
| the river fight | 66 | 1,257 ms (161; 1,094) | 721 ms (718) | 11 ms | 542 ms (471) | 71 ms (5) |
| the fields | 38 | 694 ms (120; 572) | 540 ms (538) | 8 ms | 450 ms (397) | 57 ms (4) |
| the dungeon's approach | 81 | 1,127 ms (168; 954) | 881 ms (876) | 13 ms | 526 ms (452) | 73 ms (5) |
| the menus | 56 | 753 ms (132; 618) | 593 ms (590) | 9 ms | 529 ms (459) | 91 ms (6) |

- **The driver's pipeline creation is the cost**, not the translation: glslang is 13-17% of a cold scene's
  compile time; with our SPIR-V cache warm the driver still takes 0.5-0.9 s per scene; with the driver's own
  cache warm everything takes 8-13 ms. The programs to text: 2-4 ms per scene.
- **Where it lands**: cold, a scene's pipelines are created in 13-24 frames; the worst holds 400-470 ms of them
  (the first area's load: 23-39 pipelines in one burst), and 8-13 of a scene's frames over 33 ms have compiles in
  them (river 11 of 18, dungeon 13 of 25). Warm, the compile frames hold at most 6 ms of compiles; the frames over
  33 ms that remain (6-15 per scene; the loads' 210-240 ms frames among them) are not shaders'.
- **Our pipeline cache does not exist yet**: the native renderer creates its pipelines without a VkPipelineCache
  (section 1; docs/SHADER_PREPARATION.md section 1), so a warm run here is warm only because NVIDIA's driver kept
  its own cache. M9.2's own cache, filled by a preparation pass, is what makes "warm" ours to guarantee (and
  AMD's and Intel's alike).
- **Coverage so far**: the four scenes need 98 distinct pipelines between them (river 66, fields 38, dungeon 81,
  menus 56: most shared -- the title's, the HUD's, the common materials'); a scene's set varies a little between
  runs (63-66 for the river). 98 is what four test routes see, not the game's number.

**Decided for M9.1-M9.2 by these numbers**: the preparation pass must create pipelines (through a VkPipelineCache
kept per GPU and driver), not only translate shaders; the first area's load burst (400-470 ms) is the largest
single target; a pipeline's identity needs the programs' bytes, which are the player's own data -- so the variants
seen are recorded locally with what is needed to recreate them, and a distributed list (if any) names them only by
hash and parameters (section 8.5).

## 10. M9.1: the install with build slots, migration, rollback, saves (2026-11-14) [M, C]

**The layout** (tools/install_layout.py; section 3.3 as built):

```
<install>/
  game/                     the active build: a junction (Windows; no privilege needed) or a symlink to
                            builds/<id> -- the stable path shortcuts start, re-pointed when another build is made
                            active; a removal never follows it
  active.json               {"layout": 2, "active", "previous" (newest first), "retention" (default 1)}
  builds/<id>/              the game, its libraries and notices, build.json (its lift, objects, shader folder,
                            inputs, SHA-256, size, what checked it), build.cfg (its id and shader folder, for the game)
  config/dotheroes.cfg      the settings: outside every build
  data/disc/                the player's disc, extracted (the disc image is not needed after the install)
  data/gen/<lift-id>/       one lift (a done.json when finished), shared by the builds made from it
  data/cache/objects/       the compiled objects, shared
  data/cache/shaders/<id>/  the translations and pipelines of one translator and glslang (the build's link
                            fingerprint): a build never removes another's
  data/gamedata/            the saves: never in a build, never written by an update
  backups/saves/<id>/       save backups (backup.json: when, why, size, files, a note)
  logs/                     the game's: game.log and the five before it
```

**The game** (port/main.cpp): finds its install from the parent or grandparent holding active.json -- so it
starts the same through game/ or from its slot -- reads config/dotheroes.cfg and its build.cfg, logs to
logs/game.log (rotated), keeps its translations in its translator's folder. `dotheroes --info`: one JSON line --
the layout, the build id, the shader folder, the native release, the disc found, FFmpeg loading beside it (the
movie player's own loader) -- with no window and no game data read. The setup app's layout still works as before.

**The worker** (tools/worker.py, docs/M9_WORKER_PROTOCOL.md): `install` and `update` build into a staged slot
(builds/.staging-*), **check it** (`--info`: FFmpeg loads, the native release; its memory test) and only then
record it, make it active and re-point game/; any failure or cancel removes the staged slot and leaves the active
build as it was. An update with the same inputs as the active build (the lift, every object's fingerprint, the
libraries and linker) builds nothing. `update` backs the saves up first. `rollback` rewrites active.json and
game/ (the kept build's SHA-256 checked first). `retention`, `builds`, `repair-link`, `saves` (list, backup,
restore -- the current saves backed up first --, delete; restore and delete only with `--confirm`), `migrate` (the
setup app's install: the saves backed up, the current build made from its own disc copy, lift and objects, the
data moved, the old game removed), `install --resume` (an interrupted install's disc steps and finished lift kept).
Pruning removes only what no kept build uses: other slots, lifts, objects (the build's own clean-up of objects
now spares the kept builds'), shader folders. The setup app runs the same operations in-process until the
launcher replaces it.

**Found and fixed on the way**:
- **A hang**: the worker's cancel thread reads its stdin; every tool it started inherited that pipe, and on
  Windows a process started on a pipe with a read pending stops in its start-up until the read ends -- the
  resume test's lifter sat 25 minutes at 0.02 s of CPU. The worker's children get no stdin now
  (tools/setup_pipeline.py run_tool, tools/zig_build.py); tests/test_worker.py starts tools with the stdin held
  open (it hangs without the fix).
- **The lift's fingerprint** covered every Python and JSON file in tools/ -- any developer's script, the
  launcher's worker -- and missed tools/spu_native/ and spu_native_task.cpp.in, which the SPU step reads. It
  now covers ps3recomp's tools, the configuration and the lifting tools (setup_pipeline.LIFT_TOOLS, spu_native/).

**Validated** (tests/test_install_layout.py, 22 tests on made-up installs; tests/test_worker.py, 20; with the
pipeline and boundary tests 74 in all, pass; tests/player_install_test.py on the real conversion,
F:\3d dot game heroes\m9\evidence\m91):

| Stage | Result |
|---|---|
| fresh | 376 s; the build checked (FFmpeg n9.0.2; memory test 668 checks, 0 failed) and made active; game/ starts it |
| check | the slot's libraries and notices, build.json and build.cfg; `--info` from game/: the slots layout, the disc found; the settings in config/; the lift equal to the developers' gen17 (0 files differ) |
| update, nothing changed | 0 s: nothing built, the active build kept |
| an update whose link changed | 2 s: every object reused (76 of 76), a new slot active, the one before kept; the saves backed up first and byte for byte untouched; a slot is 94 MB |
| rollback | 0.06 s, offline: game/ starts the kept build (`--info` names it), its game as recorded, the saves untouched; forward again |
| a build that fails to compile; one that fails its check | each refused (compile_failed, build_invalid); the active build as it was, no staged slot left, the game still starts |
| relift | the lift redone (50 s), the same lift: nothing rebuilt |
| an install cancelled in its build, resumed | cancelled (the build ended by force: compiles in flight), no build active; `info` names the interrupted install and its done steps; `install --resume`: the disc's steps and the lift not redone, the 12 objects compiled before the cancel reused, 241 s |
| migration of a setup app's install (a copy of M9.0's) | 56 s: the disc not needed, relifted (its tools were older) with 75 of 76 objects reused, the saves untouched, the player's settings kept, the translated shaders kept in the build's folder, the old game removed |
| settings | the player's values kept; a missing switch added back; an old default brought up to date |
| the game (on screen) | game/ --memtest exit 0; a river fight from the install as a player starts it: completed, native physics (44,035 tasks); its translations in its build's shader folder |

**Not yet**: shortcuts (the setup app's still start its old game after a migration: the launcher, M9.4, makes
them again at game/); save compatibility across versions is the game's own format, the same in every build (the
saves are never converted); disk space is checked before an install (6 GB) and an update (1.5 GB) and a full drive
fails cleanly (the staged slot removed), not yet tested by filling a drive.
