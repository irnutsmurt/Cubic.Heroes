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

## 11. M9.2: shaders and pipelines prepared before play (2026-11-14) [M, C]

**What was missing** (section 9): the native renderer made its pipelines with no VkPipelineCache, so a session was
warm only when NVIDIA's own driver cache was; nothing was prepared before play; the driver's pipeline creation was
the cold cost.

**The native renderer** (port/native/render/ppfx_render.cpp):
- **One builder**: the two creation sites (PPFX's, asp's) now fill a `PipeState` -- everything they read besides the
  programs -- and call `make_pipeline`, which builds exactly what each built inline before, through the cache.
- **Its own pipeline cache**: `<shader folder>/native-pipelines.vkpc`, used only for the same renderer version,
  GPU, driver version and driver cache UUID, its data checked by a checksum, at most 256 MB; anything else starts
  empty and the report says why ("made with another driver version", "damaged (its checksum differs)", ...). Saved
  every 10 s when grown, after a preparation, at exit and at a test's stop.
- **Recipes** (`native-recipes.bin`; in an install `data/cache/`, shared by every build): each pipeline the game
  first makes is recorded once -- its key, its `PipeState`, the translation's parameters and the two programs'
  bytes as the game gave them, with a checksum; a record cut short by a crash ends the file there. They are the
  player's own game's programs and stay in their install. Programs, not translated text: a later build's
  translator makes the text again, so coverage survives a translator change.
- **`dotheroes --prepare [--check] [--cache DIR]`**: no window and no game -- the backend's device offscreen, the
  renderer set up on its own -- every recipe translated by this build and made through the pipeline cache, which
  is then saved; JSON progress on stdout. `--check`: the cache's state for this GPU and driver and the recipes
  counted, nothing made (the launcher's question at every start).
- **The worker** (docs/M9_WORKER_PROTOCOL.md): `prepare [--check]`; `install`, `update` (when the build changed)
  and `migrate` prepare after activation when there are recipes. Unseen variants are still made when first drawn,
  as before, and recorded for the next preparation.

**Measured** (build/m9/m92_run.sh, m92b_run.sh on rel52/rel53: rel51 with these changes; NVIDIA's own cache
pointed at an empty folder per run so that only ours could be warm; F:\3d dot game heroes\m9\evidence\m92, m92b):

| Run | Pipelines | In all (the driver) | Longest | Over 16 ms | The worst frame (compiles in it) | Frames over 33 ms |
|---|---|---|---|---|---|---|
| the river, cold | 65 | 1,079 ms (906) | 62 ms | 31 | 560 ms (489) | 20 (11 with compiles) |
| a driver change, then `--prepare` (65 recipes, 0.79 s), then the river with the driver's cache empty | 65 | 67 ms (64) | 2.6 ms | 0 | 226 ms, the load's (26) | 12 (3) |
| the fields with only the river's recipes prepared | 38 | 86 ms | 40 ms | 1 | -- | 7 |
| the fields after `--prepare` with its 7 new recipes | 38 | 45 ms | 2.8 ms | 0 | -- | 7 |
| a new translator (an empty shader folder, the same recipes): `--prepare` translated 73 shaders and made 65 pipelines in 0.99 s; then the river | 63 | 100 ms | 38 ms (1 not recorded before) | 1 | -- | -- |

- A cache made on another driver, or damaged (a byte changed), is refused and said why; `--prepare` makes it again
  from the recipes (65 of 65, none failed).
- **The install test** (on the real conversion, then the game from game/: a river fight that recorded 68 recipes):
  the check found the cache valid; after the cache's recorded driver version was changed, not valid ("made with
  another driver version"); `prepare` made 68 of 68; the check valid again.
- **The rendering is unchanged** by the refactoring: the reference scenes against RPCS3, 5 of 5 within the accepted
  figures; the menus, dungeon and snapshot drives; the memory test.

**Coverage, stated as measured** (requirement 4 of docs/SHADER_PREPARATION.md): a preparation covers the pipelines
this install has met -- each scene's set is mostly shared (the river's recipes covered 31 of the fields' 38), and a
run of the same route meets one or two pipelines the run before did not (the river: 1-2 of 65). **A player's first
session is not prepared** beyond what the driver's own cache holds: nothing is distributed yet.

**The enumeration from the player's own data, investigated** (requirement 5's question): the disc's
`shaders.ps3.pkg` is a container ("spkg", version 4) of 567 records, each a program beginning "CGB", with a table of
16-byte entries (offset, size, group, an id) after a 64-byte header, uncompressed. Against the 68 recipes of the
install test's run: 16 of the 31 vertex programs the renderer was given are in the package with each 32-bit word's
bytes reversed; 25 of the 49 fragment programs match the package in their first 16 bytes (the game patches a
fragment program's constants into its microcode when it loads it), 1 whole, 2 are in the EBOOT; the rest (PPFX's own
among them) are not in the package as they are. So a distributed list could name variants by independently authored
metadata (a program's package record and a hash of its normalized form, the translation's parameters, the pipeline
state -- no program bytes, no text), resolved on the player's machine against their own package: possible for about
half the programs seen, after the package's record format and the game's load transforms are worked out. Not done in
M9.2 -- the next step for first-session coverage, with the coverage it reaches measured as above.

## 12. M9.3: signed updates and release management (2026-11-14) [C, M]

**Trust** (tools/release_trust.py; docs/RELEASE_SIGNING.md): a release's manifest (release.json: the version, the
channel, the notes, the minimum launcher, each download's name, platform, size and SHA-256, the signing key's id) and
its Ed25519 signature (release.json.sig). The worker verifies with OpenSSL's Ed25519 -- the libcrypto Python's own
ssl module already carries, called through ctypes: nothing downloaded, no cryptography of the project's own --
against the public keys in config/release_keys.json; an unsigned, altered, foreign-signed or rule-breaking release is
refused and the newest release that passes is offered; every download is checked against the manifest's size and
SHA-256 before it is kept. **config/release_keys.json ships with no key**: until the owner makes theirs
(`tools/release.py keygen`, on their own machine, the private key encrypted and kept offline) and adds its public
key, every update is refused and no request is made at all.

**The owner's tooling** (tools/release.py): `keygen`, `manifest`, `sign` (OpenSSL's command line; then verified as
a launcher will), `verify`. docs/RELEASE_SIGNING.md: making the key and a backup key, signing a release, rotation
(a new key trusted beside the old in a release signed by the old), recovery (a lost key, an exposed key, a bad
release).

**The update engine** (tools/updates.py; the worker's `check-update`, `download-update`, `stage-launcher`,
`swap-launcher`): the release list from GitHub's API (or a mirror for the tests); the channel (stable by default,
prerelease opt-in); offline is a result, never a block on play; downloads resumed from their `.part` (a Range
request), too little space refused first; the launcher's own update -- its package staged beside it, swapped in by
a helper after the launcher exits, kept only if the new launcher comes up (it writes `.started-ok` within 60 s),
else put back and the previous one started. The setup app's own self-update (tools/self_update.py) now offers only
a release whose manifest verifies and unpacks only a download that matches it, until the launcher replaces it. The
game itself is rebuilt by the new launcher's worker into a new slot (section 10): the active build untouched until
the new one is checked, the previous kept for rollback.

**Validated** (tests/test_updates.py, 20 tests; with the other modules 94 pass): Ed25519 against RFC 8032's vectors
(a changed bit, another message, a short signature refused) and signatures from OpenSSL's command line; a mirror
signed with throwaway keys made in a temporary folder: the channels; an altered, an unsigned and a foreign-signed
release refused and the next valid one offered; a manifest naming a key other than the one that signed it refused;
no trusted key: refused before any request; a download checked, a damaged one refused and removed, an interrupted
one resumed (over a local HTTP server that cuts the connection), too little space refused; offline reported by the
worker as a result; a package with a path escaping it refused; the launcher's swap kept when the new launcher comes
up and put back when it does not (stand-in launchers); the worker's check and download end to end; the setup app
offering nothing unverified.

**Not yet**: the owner's key (config/release_keys.json is empty: the owner makes the key); no release has been
published, so the GitHub path has run against the mirror only (the same JSON shape as GitHub's API); the launcher's
side of its self-update (`--after-update`, `--update-failed`, `.started-ok`) is M9.4's launcher.

## 13. M9.4, part 1: the launcher's back end -- settings, diagnostics, the GPU (2026-11-14) [C, M]

The native launcher's window (C++, SDL2, Vulkan, Dear ImGui) needs Dear ImGui's source, a download the owner is
asked for first (section 6); this part is everything the launcher stands on that needs no window.

**One authoritative settings file** (decision 7, section 8.9; tools/settings.py, config/settings_schema.json):
config/dotheroes.cfg holds the game's settings and the launcher's (LAUNCHER_*: the update channel, checking for
updates, the builds and automatic save backups kept, the log detail), so the launcher's settings page and the game's
own menus read and write one file. The schema gives each setting its type and rule, default, owner, whether it
applies live or at the game's next start, its category and label. A value that breaks its rule is reported and its
default used; a line that is not NAME=value is left out and reported; a damaged file (binary data, unreadable) is
reported and `repair` writes it again with its valid values, the damaged one kept beside it; keys the schema does not
know are kept; a change is checked whole before anything is written; defaults can be restored per setting or all.
The file is written whole (a temporary file renamed over it). The worker's `settings` command
(docs/M9_WORKER_PROTOCOL.md). Every existing setting applies at the game's next start; M10 adds the remaining PC
settings, the game's in-game settings menu over this file and the live ones.

**Diagnostics** (section 8.7; tools/diagnostics.py, the worker's `diagnostics`): a report for the player to review --
about.json (the versions, the operating system, the processor and memory, the GPU and its driver, the install's
state and sizes, whether there are saves) and the logs' ends -- every text with the home and install folders, the
user's and computer's names and e-mail addresses replaced by placeholders; never the saves, keys, the disc's or the
game's files, recipes or shader caches (their sizes only). The player reviews the files and may remove any; the zip
is exactly what remains. Nothing is sent anywhere.

**The GPU and its driver** (port/native/render/ppfx_render.cpp): `dotheroes --prepare` and `--prepare --check` now
report the device (name, vendor and device ids, the driver's version as its vendor writes it, the Vulkan version) --
for the diagnostics, and for the launcher's own "a preparation is due" decision.

**Validated**: tests/test_settings_diagnostics.py (9: the rules, invalid values and lines, a change refused whole,
defaults, a damaged file repaired, the worker's command; the report's sanitizing, saves and recipes never in it, the
zip as reviewed); with the other modules 103 unit tests pass. On the test install (F:\3d dot game heroes\m9\evidence\m94):
the check reported the GPU (an RTX 3070, driver 616.92, Vulkan 1.4.351); **a real update onto a new prebuilt** -- the
saves backed up first, the lift redone (its tools had changed), the build relinked with its objects reused, checked,
made active, the previous kept, then **prepared automatically**: the 68 recipes recorded by the earlier river fight
translated by the new build's translator into its new shader folder (71 translations) and their pipelines made in
0.45 s; the diagnostics report and its zip hold no user name, computer name, home path or e-mail address.

**Next, M9.4 part 2** (with Dear ImGui): the launcher's window and pages over these -- the first-run wizard (the
probes, the space check, the install with its progress, elapsed and remaining time, cancel and resume), Play (the
active build, an update and its notes, the preparation when its check says so), builds and rollback, caches,
saves, diagnostics, settings; keyboard, mouse and controller; DPI; window placement; the Steam shortcut; its side of
the self-update (`--after-update`, `--update-failed`, `.started-ok`); a fallback when Vulkan is missing.

## 14. M9.4, part 2: the launcher (2026-11-14) [C, M]

**What it is** (launcher/main.cpp, json.h, worker_client.h; built by tools/build_launcher.py into
build/launcher/CubicHeroes.exe): a native window (SDL2, Dear ImGui 1.93.0 -- the release archive pinned by its
SHA-256 in config/deps.sha256, compiled in, its MIT licence shipped beside the launcher with SDL2's) that runs every
operation through the pipeline worker (docs/M9_WORKER_PROTOCOL.md). Its pages:
- **Play**: the active build, PLAY (the game through `<install>/game`), the shaders' state for this GPU and driver
  (`prepare --check`; "Prepare now" when one is due), the update state; a setup app's install found: "Move it now"
  (`migrate`); none: the way to Install.
- **Install** (the wizard): 1 the disc (a file browser, or dropped onto the window; `probe-disc` names another game,
  version or a damaged image), 2 its keys (`probe-keys`), 3 the folder (`space`); Install only when all three are
  checked; an interrupted install continued (`--resume`); on an existing install: repair from the disc.
- **Updates**: the channel, a check, the notes, a download checked against the signed manifest; offline and "no
  release key" said plainly.
- **Builds**: every build, the active one and the kept ones; "Go back to it" (confirmed) -- `rollback`.
- **Saves**: a backup now (with a note typed on the on-screen keyboard), every backup with its date, size, files and
  why it was made; Restore and Delete, each confirmed.
- **Shaders**: the GPU, the driver, the cache's state and size, the pipelines known; Prepare now; Rebuild the cache
  (confirmed; `prepare --rebuild`).
- **Settings**: the authoritative file's settings by category, each changed with A or left/right, those that apply
  at the game's next start marked; Restore the defaults (confirmed); Add to Steam (no artwork downloaded); a
  Start-menu shortcut; another install folder.
- **Diagnostics**: a report made, every file listed and readable before it is packed, any left out (X), the zip in
  Documents.
- Over the pages: an operation's progress (the steps, the bar, elapsed and remaining time -- "about" when it rests
  on the reference PC's figures --, the step's detail), Cancel (B); messages with expandable technical details;
  confirmations whose safe choice has the focus.

**With a controller alone** (the owner's requirement): every page, button, list, the file browser (drives and the
known folders, A opens or chooses, B goes up, Y closes) and the on-screen keyboard (A types, X deletes, Y a space,
MENU done, B cancels) is reached with the D-pad or the left stick; A confirms, B goes back or cancels, LB/RB change
pages; the focused item carries a two-ring outline (white on the accent-coloured buttons); the bottom bar shows the
buttons that do something here and now. The prompts follow the device last used -- the controller's A/B/X/Y in their
colours and LB/RB, or Enter, Esc, X, Y, Q/E keycaps -- and start as the controller's when one is connected.
Controllers are taken as they are plugged in or out (SDL's game-controller layer, Dear ImGui's "all gamepads" mode).
The keyboard and mouse do everything too (Q/E or Page Up/Down for the pages, a click on a page's name).

**The window**: its size, position and maximized state remembered (%APPDATA%\CubicHeroes\launcher.json, with the
install folder and the last disc and keys paths -- paths only), per-monitor DPI-aware and scaled to the display, a GUI
program (no console; the worker started with no window). `--page NAME` opens on a page; `--after-update` writes the
health file the self-update waits for; `--update-failed` says the previous launcher was put back.

**Drawing, and a decision for the owner**: the launcher draws through SDL2's renderer (Direct3D 11 or OpenGL, or
software when neither works), not Vulkan. That is how it opens, with diagnostics and recovery reachable, on a PC
whose Vulkan device is missing or unusable (section 8.1) -- the game's own Vulkan check is what the launcher reports.
A Vulkan drawing path (Dear ImGui's Vulkan backend) can be added as the first choice with this as its fallback; it
changes nothing visible in a 2D interface.

**Tried** (build/m9: the launcher on the test install, each page grabbed from its own start, nothing typed into any
window): Play read the install, the build, the shaders' state (68 pipelines ready on the RTX 3070, driver 616.92) and
the update state; Install showed the three steps and the space check; Builds the active and the kept build with "Go
back to it"; Saves the automatic backup with Restore and Delete; Shaders the GPU and cache; Settings every setting in
its category; Updates "no release key"; the controller's prompts and the focus. **Not yet tried by hand**: a whole
install, a rollback, a restore, the file browser and the on-screen keyboard with a controller -- the owner's review.

**Not yet**: the launcher in the release download (M9.5's packaging: the launcher, the frozen worker beside it).
First-session preparation: section 15; the launcher's look after the owner's direction: section 16.

To try it from the checkout: `build/venv/Scripts/python.exe tools/build_launcher.py`, then
`build/launcher/CubicHeroes.exe` (it finds the worker in the checkout; choose the install folder on the Settings page,
or install).

## 15. The first session prepared: the pipeline manifest (2026-11-14) [M, C]

**The question** (the owner's requirement, 2026-11-14: a player's first session prepared before it is played, its
coverage measured): section 11 left a new install with no recipes until the game had been played, so the first
session made every pipeline as it was first drawn (the river, cold: 64 pipelines, about a second of the driver's
work, a 500 ms frame).

**Where the programs come from** (build/m9/program_search.py, local: counts and offsets, never bytes): every program
the renderer was handed in the recorded sessions is on the disc.
- **Vertex programs**: each whole, with every 32-bit word's bytes reversed (the game's loading swaps them to the
  RSX's order): 18 of 33 in `shaders.ps3.pkg`, 15 in the EBOOT.
- **Fragment programs**: in the package or the EBOOT as stored, but the game patches each one's inline constants
  (16-byte blocks after the instructions that read them) as it loads and draws; 10 of 60 are found whole, the other
  50 where the first block and most blocks are equal.
- So all of the recorded pipelines have both programs on the disc (70 of 70 at first; 98 of 98 with the four routes
  below: 36 vertex programs -- 21 in the package, 15 in the EBOOT, every one words-reversed -- and 49 fragment
  programs -- 19 in the package, 30 in the EBOOT, as stored; 3 of them byte for byte as the game handed them over,
  the others differing only in constant blocks). The renderer keeps fragment constants out of the translation (they
  are buffered; a pipeline's key holds the program's structural hash), so the stored program translates to the same
  shader as the patched one.

**The manifest** (tools/pipeline_manifest.py `make`, on a developer's machine, from recorded recipes and the disc):
a line per pipeline -- its recipe's record without the programs (the key; `PipeState`, the render state; `ProgFixed`,
the translation's parameters: the renderer's own structures, only numbers -- masks, formats, texture sizes, blend and
depth state), and for each program a file under PS3_GAME/USRDIR, an offset, the transform (0 as stored, 1 words
reversed) and a 64-bit FNV-1a hash of the bytes there. No program bytes, no shader text, nothing of the game's
copied: tools/boundary_check.py finds nothing in it.

**Resolved on the player's machine** (`resolve`; the worker's `first_session_recipes`): against the install's own
copy of the disc (`data/disc`, kept by the install), each listed pipeline the install's recipes lack is added -- its
programs read from the disc at the listed place, checked by the hash, the transform applied -- in the recipes'
format; a disc whose bytes differ adds nothing for that pipeline. The worker does this before every preparation
(after an install, a repair, an update, a migration, and "Prepare now"), so a first install prepares the listed
pipelines before it is played. The recipes it writes are the player's own programs and stay in their install.

**Checked**:
- The disc-resolved recipes against the recorded ones (build/m9/m95_verify.sh, rel54): the recorded set prepared
  into an empty shader folder (75 shaders translated, 70 pipelines); the resolved set then prepared into the same
  folder translated **none** again (140 of 140 from the cache) and made its 70 pipelines from the pipeline cache in
  12 ms; prepared alone into another empty folder, it made the same 75 SPIR-V files (names and sizes identical).
- tests/test_pipeline_manifest.py (7): a made-up disc and recipes -- places, transforms and hashes named and no
  program bytes; the recipes given back (a fragment program with its stored constants); a changed disc adds
  nothing; a second resolve adds nothing; held recipes kept, a damaged tail cut; the worker adds them to an install.

**The first session, measured** (build/m9/m95_run.sh on rel54, the install test's build; evidence on F:\3d dot game
heroes\m9\evidence\m95). Four routes cold -- our shader folder, NVIDIA's own disk cache and the recipes all empty --
each recording its recipes; the manifest made from those and the earlier river recordings (98 pipelines, none left
out); resolved against the disc into an empty recipes file, as a new install's (98 added); `--prepare` into an empty
shader folder (88 shaders translated, 98 pipelines made, 1.42 s, 1.5 MB); then the four routes again as a first
session, NVIDIA's cache emptied again for each:

| Route | Pipelines made in play | Their cost: in all, the longest, over 16 ms | The worst frame (compiles in it) | Frames over 33 ms (with compiles) | Met that the manifest lacked |
|---|---|---|---|---|---|
| the river, cold | 64 | 1,014 ms, 53 ms, 31 | 545 ms (475) | 20 (10) | -- |
| the river, first session | 63 | 76 ms, 3.6 ms, 0 | 1,556 ms (0: the title's load) | 11 (3) | 0 |
| the fields, cold | 38 | 725 ms, 48 ms, 23 | 475 ms (420) | 14 (9) | -- |
| the fields, first session | 38 | 58 ms, 3.5 ms, 0 | 451 ms (0: a load) | 10 (2) | 0 |
| the dungeon, cold | 81 | 1,202 ms, 55 ms, 36 | 545 ms (472) | 26 (12) | -- |
| the dungeon, first session | 79 | 91 ms, 3.4 ms, 0 | 478 ms (0: a load) | 19 (5) | 0 |
| the menus, cold | 56 | 765 ms, 55 ms, 24 | 537 ms (470) | 784 (8) | -- |
| the menus, first session | 56 | 52 ms, 2.5 ms, 0 | 259 ms (0) | 781 (4) | 0 |

(The menus' frames over 33 ms are their 30 fps screens, the same in both. "With compiles": a frame that also made a
pipeline -- 2 to 4 ms of it in a first session -- but was long for its load.)

- **On these routes the first session is prepared**: every pipeline they made was in the manifest; what is left
  of pipeline creation in play is the driver taking a pipeline from the prepared cache, 2-4 ms each (NVIDIA's own
  cache empty), never a frame's worth.
- **The river's 1,556 ms frame was the PC's, not the preparation's**: it is the title's load (20 s in, no pipeline
  made near it; 459 ms in the cold run), and it did not come back -- two more first-session runs of the river
  (m95r_run.sh) had worst frames of 234 and 230 ms with no pipeline in them, as the earlier prepared runs (229 ms).
  One of those two met one pipeline the manifest lacked (made in play: 198 ms), the other none -- the run-to-run
  variation section 11 found (a run meets one or two pipelines the run before did not): 63 of 64 covered there.
- **What a route not recorded meets** (leave one out: each route's pipelines against a manifest made without its own
  recordings): the dungeon 59 of 81, the fields 32 of 38, the river 55 of 64, the menus 56 of 56. That is the
  coverage to expect where no route has been recorded yet (later dungeons, other fields, the editor): most of a new
  place's pipelines are shared, a fifth to a quarter are its own -- made in play as before, recorded, and prepared at
  the next preparation. The manifest grows by recording more routes (`make` takes any number of recipes files).
- **Not distributed yet**: the manifest made here (98 lines, 112 KB of numbers, places and hashes; the boundary check
  finds nothing) stays out of the repository until the owner agrees that it is the "independently authored
  metadata" requirement 5 allows -- the worker uses config/pipeline_manifest.txt when it is there and nothing
  otherwise.

## 16. The launcher's look: the game's world as the reference (2026-11-14) [C]

**The owner's direction** (2026-11-14): Dusklight stays the reference for what the launcher does and how a
controller drives it; 3D Dot Game Heroes is the reference for how it looks -- voxel and pixel-art elements, colours
from the game's environments and menus, retro fantasy with modern usability, chunky borders and bevelled panels,
pixel icons, a selection and prompts in the same style, subtle animation, one consistent identity -- clean,
readable, at a desk or on a couch, usability first; original assets only, nothing taken from the game.

**What it is** (launcher/main.cpp; everything drawn in code -- no image file, no font file of the project's, nothing
extracted from the game):
- **The scene**: a night sky (a gradient, stars that twinkle, blocky clouds drifting slowly) over a row of voxel
  ground blocks -- grass on earth, seams and grains -- that the button prompts stand on.
- **A command menu** on the left, as in a role-playing game's menu window: the logo (a grass voxel with a small gold
  one floating over it; CUBIC.HEROES in the pixel font), the eight pages each with its own pixel icon (a sword,
  a chest, an arrow to the ground, a hammer, a book, a gem, a gear, a magnifying glass), the active page in gold
  with a gold bookmark; LB/RB with "PAGES" at its foot. A click on a page works too.
- **Windows**: the page sits in a retro window -- a light border with stepped corners, an ink outline, a deep blue
  inside; cards and dialogs are the same window, a card's border quieter.
- **Blocks for buttons**: square, bevelled (lit above and left, shaded below and right, an ink outline), pressed in
  when held; steel for most, gold for the main choice (PLAY, Install, OK), brick for what removes or cancels.
- **The selection**: a chunky gold ring with stepped corners, pulsing gently, and a white pixel cursor bobbing at
  its left -- visible from across a room, the same on every page, in every dialog, the file browser and the
  on-screen keyboard.
- **Type**: the project's own 5x7 pixel font (capitals, digits, signs) for titles, the logo, the PLAY block, the
  step numbers and the button glyphs; a clean system font (Segoe UI Semibold on Windows) for everything to be read
  -- descriptions, paths, values, messages.
- **Prompts**: the controller's A/B/X/Y as round-cornered pixel blocks in their colours, LB/RB/MENU/VIEW as steel
  blocks, the keyboard's keys as parchment keycaps -- switching with the device last used, as before.
- **Progress**: a trough of green blocks filling, a travelling group of blocks while the share is unknown.
- **Motion**: a new page slides in a little and fades up (0.22 s); the cursor bobs, the ring pulses, the gold block
  floats, stars twinkle, clouds drift -- all slow and small, none of them in the way.

**Colours** (the palette, `col::` in the source): night blues for the sky and windows, parchment for borders and
text, gold for what is chosen, grass greens and earth browns for the ground and for "done", steel blue for buttons,
brick red for danger.

**Usability fixes made with it** (found in the review grabs, each a fault of the controller path, not of the look):
- **The install steps' cards** were each one stop for the controller (the card first, then its button): cards are now
  flattened into the page, and the Install page opens on the first step's "Choose..." button.
- **A page's prompts and buttons under a dialog**: the page below kept adding its prompts ("Play", "Check for
  updates") and answering its buttons -- Y on the on-screen keyboard (a space) also started an update check. While a
  dialog is open the page's prompts and button presses now belong to the dialog.
- **A dialog's focus**: a page whose content arrived after a dialog opened (the Play page waits for the install's
  details) took the focus back from under the dialog. A page now waits for the dialog to close; a dialog puts the focus
  on its chosen button over its first frames (Cancel in a confirmation, OK in a message, the first key, the first
  entry of the browser) -- checked with Dear ImGui's own focus log (`CUBICHEROES_DEBUG_NAV=1`, on stdout).
- **A disabled main button** (Repair before the steps are checked) is plain steel with a readable label, not dim gold.

**Tried** (build/m9/shots10: every page with `--page NAME` and every dialog with the review's `--show
browser|keyboard|confirm|message`, each grabbed from its own start with the window kept on top; no input sent to any
window): the command menu, the windows, the blocks, the gold ring and cursor on the focused item of each page and
dialog, the prompts of the device in use (a controller connected) on the ground, the browser's folder icons, the
keyboard's keys. The focus ring and cursor move with the D-pad, the stick and the keys as before (Dear ImGui's
navigation is unchanged); the motion was checked by grabs, not by hand. **The owner's review**: by hand, with a
controller.

## 17. M9.5, part 1: the launcher in the download (2026-11-14) [C, M]

**The download's layout** (tools/make_bundle.py; the archive keeps its name, DotHeroes-Setup-windows-x86_64.zip,
which the update engine's release manifests already list):
- at its root the launcher -- `CubicHeroes.exe`, SDL2's library, `licenses/` (Dear ImGui's MIT licence, SDL2's zlib
  licence), the project's `LICENSE.txt`, a README saying to run the launcher;
- `worker/`: the pipeline worker the launcher runs (`worker/DotHeroes-Setup.exe --worker`, where the launcher already
  looked for it) -- the frozen app with its Python, the Zig toolchain, the libraries, the sources, the tools and
  configuration, and their licences and sources, exactly as the setup app's download held them.
- **Built on Windows itself** now: PyInstaller from the checkout's own venv and the Windows Zig beside the Linux one
  (Wine stays the way from Linux). The launcher is built on the platform it is for.
- **The boundary check** gained a rule (none loosened): a launcher shipped without Dear ImGui's licence is refused.
  The folder and the zip pass (19,904 files; the two pinned FFmpeg test references as before); 427 MB, 130 MB zipped.

**Tried** (build/m9/m95p_run.sh; evidence on F:\3d dot game heroes\m9\evidence\m95p):
- **The packaged launcher runs its own worker**: started from the download's folder, it found
  `worker/DotHeroes-Setup.exe` and read the install, the builds, the GPU and the cache's state through it.
- **A fresh install through the packaged worker hung at first -- found and fixed**: the frozen app ran the lifter
  (`worker/DotHeroes-Setup.exe ...ppu_lifter.py`), which started its parallel lift ("16 workers") and then waited for
  ever: an idle `--multiprocessing-fork` child, no progress for 8 minutes (the lift takes ~40 s). Stopped
  (`owned_run --cancel`), the logs kept. **Reproduced** with a four-worker pool run the same way (the frozen app given
  a script): it hangs too. **The cause**: Windows starts a pool's worker processes fresh (spawn), as the frozen
  executable; for a frozen executable Python's multiprocessing does not have them import the main script
  (`multiprocessing.spawn.WINEXE`), and here the main script is a tool the app runs by its path (setup_app.py's
  `run_script`) -- so the workers cannot find the pool's functions, and the pool waits. Not the new layout's doing:
  the frozen app is the same one folder down. **The fix** (setup_app.py `run_script`, frozen on Windows only):
  `spawn.WINEXE` cleared, which governs only that choice -- the workers import the tool by its path. The probe pool
  then ran in 0.16 s (0.64 s with 50 MB of start-up arguments), nothing left running.
- **Then the whole first session through the download** (m95p2): a fresh install by the packaged worker -- 373 s
  (verify 2, extract 12, decrypt 2, the lift 40, SPU 13, the build 302, the check and activation 1, the preparation
  1: the first-session manifest's 98 pipelines added from the install's own disc copy and prepared in 4 parts) --
  then the river played from the new install with NVIDIA's cache empty: 64 pipelines made in play in 65 ms, the
  longest 2.5 ms, none over 16 ms, none met that the preparation lacked; the worst frame 401 ms, a load with no
  pipeline in it. (The manifest was put into that test copy only; it is not distributed.)

## 18. Shader preparation in the launcher: the first-run offer, in the background (2026-11-14) [C, M]

**The owner's additions** (2026-11-14): after the first install, an offer -- "Would you like to precompile shaders
now? This is recommended to reduce stuttering during gameplay." (Precompile Now (recommended) / Skip for Now);
preparation in the background with a persistent bar along the bottom (the operation, the percentage, the pipeline
count, the time left), the launcher usable meanwhile, cancel and resume, a completion notice, no duplicate jobs; the
settings (automatic after installing, after game updates, after GPU or driver changes; precompile now; rebuild;
the cache's state and disk use), automatic preparation on once the offer is accepted; a segmented, voxel-style bar;
everything by controller.

**In the launcher** (launcher/main.cpp):
- **The offer**: after an install (or a move into the launcher's layout) that succeeded, by the setting
  "Precompile shaders after installing": Ask (the default) shows the offer, its recommended choice focused; On
  prepares at once; Off does nothing. Accepting sets the three automatic settings to On and starts the preparation;
  Skip for Now changes nothing (the Play page then says a preparation is due, with Prepare now).
- **In the background**: one preparation at a time, beside the launcher's other work -- pages, settings, saves and
  diagnostics stay usable; what would change the active build or its cache (install, update, move, rollback,
  another preparation) waits for it, and says so. A bar between the windows and the ground: a gem, the operation in
  the pixel font ("PREPARING SHADERS", "REBUILDING THE SHADER CACHE"), a trough of voxel blocks (a lit top, a shaded
  side; the newest one glinting; a travelling group while the share is unknown), and "19%   13 of 68 pipelines
  about 2 s left". The ground's prompts gain VIEW (F1) "Stop preparing" -- confirmed, then the worker stops the game's
  `--prepare`. Its end stays in the bar for eight seconds ("SHADERS READY", "68 pipelines ready for this GPU and
  driver (0 failed), in N s" -- the time the launcher measured; "PREPARATION STOPPED: what was made is kept"); a
  failure also opens a message with its details; the window's taskbar button flashes when the launcher is not in
  front.
- **Automatic preparation**, at each cache check (the launcher's start, after any job), once a session and never
  over another job: a cache made invalid (another driver, another GPU, damaged) when "Rebuild the shader cache after
  a GPU or driver change" is On; a new build's empty cache (an update; the launcher remembers having prepared
  before) when "Prepare shaders after a game update" is On.
- **The settings** (config/settings_schema.json, category Shaders, second on the Settings page): the three above;
  under them the cache's state and disk use, Precompile now and Rebuild the cache (or Stop preparing while one runs).
  The Shaders page: the GPU and driver, the cache's state and size, the pipelines known and those the first-session
  list adds, the disk the shaders use, the same actions, the automatic settings' values.
- `--show first-run|prepare|rebuild` opens the offer or starts a real preparation, for review.

**In the worker** (tools/worker.py; docs/M9_WORKER_PROTOCOL.md): `install`, `update` and `migrate` take
`--no-prepare` (the launcher's own preparation follows); `prepare` runs under the install's lock
(`data/cache/prepare.lock`; a second is refused, `already_preparing`, whoever starts it; a lock whose process is gone
is taken over) and in parts (32 recipes or more, at most eight), each part's pipeline cache saved as it ends -- a
preparation stopped part-way keeps what it made, and the next one passes quickly over it (resuming); `--check` adds
the disk the shaders use and the first-session list's pipelines not yet among the recipes (counted, nothing written).

**Tried** (build/m9/shots11-13; the test install, the checkout's worker; no input sent to any window): the offer
(its recommended choice focused, B = Skip for Now); a real preparation in the background (68 recipes in 3 parts) --
the bar at 17% and 79% with the count and the time left, then "SHADERS READY"; a rebuild with NVIDIA's cache empty;
the Play page's card saying a preparation is running; the Settings page's Shaders category and actions. Unit tests:
the parts and their progress under the lock, a stop part-way leaving no lock or part file and the recipes intact,
one at a time (a live holder refuses, a dead one is taken over), the check counting the list without writing (11 in
tests/test_pipeline_manifest.py). **Not tried by hand**: stopping from the bar with VIEW, the automatic triggers
after a real driver change or update -- the owner's review.
