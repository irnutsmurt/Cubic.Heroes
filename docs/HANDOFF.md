# Handoff: continuing development on Windows

Written 2026-10-30 when development moved from the Linux laptop (16 GB RAM,
Ryzen 4000 / Radeon integrated, RADV) to the Windows PC (32 GB RAM, RTX 3070).
Read this first, then [MILESTONES.md](MILESTONES.md) (the detailed progress
log) and [FEASIBILITY.md](FEASIBILITY.md) (design and the explicit exceptions).

## The project's rules (from the owner; they are not optional)

- **Native recompilation, not emulation.** PPU and SPU code are recompiled
  ahead of time. Vulkan is the renderer on Windows *and* Linux (do not swap in
  Direct3D 12). Native audio and video. No RPCS3 at run time; RPCS3 may only be
  an offline behavioural reference. HLE of PS3 system libraries is fine. Any
  unavoidable exception is named explicitly in FEASIBILITY.md ("Explicit
  exceptions": E1 MP3 firmware codec skipped, E2 ANY2ANY LF queue run as
  PPU2SPU), never relaxed silently. The SPU interpreter / LLE are dev-only.
- **Legal model** (same as the owner's SFA3 port): ship tools only. The player's
  own disc -> verify -> extract -> decrypt -> recompile -> build locally. Never
  commit or package game bytes, extracted files, generated C/C++, keys or
  firmware. Everything derived from the game lives under `build/` (git-ignored)
  or in an install folder. No personal names or home paths in public files.
- **Keys:** the SELF (APP) keys are in `build/keys/self_keys.txt`, local only,
  never committed. Copy that file over from the old machine; the setup takes a
  keys file or a pre-decrypted `EBOOT.elf` from the user.
- **Milestones need evidence** (logs, screenshots, measurements); compiling is
  not evidence.
- **Clean up test artifacts.** Screenshots, frame dumps and old runs must not
  pile up (another project of the owner's reached 300 GB). `tests/boot_smoke.sh`
  keeps the last 5 runs; delete captures after use.
- **Keep baselines past that cleanup.** Each run records what it ran in
  `run.json` (executable hash, repo and ps3recomp revisions, switches, and
  whether the Windows desktop was locked: a locked desktop shows no window,
  so window grabs see the lock screen and a windowed run's presents reach
  no display -- use the renderer's readback, `frame.ppm`, to look at it);
  `tests/keep_baseline.py <name> <run>...` copies a run's compact summary
  (fps, `[native-cd]` lines, frame_times / frame_costs / phys_breakdown) and
  its run.json into `build/baselines/<name>/`, listed in
  `build/baselines/INDEX.md`, which no run deletes. Representative failure
  inputs live there or in `build/snap/` (guest-memory snapshots: game data,
  local only).
- **Isolate tests** from the desktop and personal settings; avoid modifying
  unrelated projects or system settings; signal only processes you started.
- **Debugging tools for testing the game** (the owner asked for these early
  on; restated 2026-11-01). Add gameplay debug controls incrementally, when
  they help a test, building on the existing tools (tests/boot_smoke.py,
  tests/drive.py, the frame trace, `port/native/dev_state.cpp`) rather than a
  separate debugger. Wanted:
  - a development-only **"can't die"** switch that stops only lethal damage:
    hits, knockback and enemy behaviour stay normal. Never keep health full
    or grant permanent invincibility -- at full health the sword beam stays
    available, which changes the fight and its workload;
  - a repeatable way to **restart a test encounter from a known state**;
  - a way for the owner to **mark a noticeable hitch** while playing, so it
    can be found in the trace;
  - (asked 2026-11-01) a **frame-rate readout** in the window's top-right
    corner: fps and frame time in ms, optionally a small frame-time graph,
    so single spikes show even when the counter reads ~60.
  All are off by default. Every test records which debug options it ran with
  (its `run.json` switches); a fix found with them on is validated again
  with normal gameplay settings. The priority stays a smooth 60+ fps,
  debris-heavy fights included.
  **Done so far** (2026-11-01; port/native/dev_state.cpp, ps3recomp patch
  0069; switches in "Diagnostics and switches"): the readout
  (`PS3_OSD_FPS=1|2`, F7 cycles it); hitch marks (`DOTHEROES_DEV_KEYS=1`: F8
  or L3+R3 -- "MARK n" on screen, a trace record, a log line; a drive script
  marks with `touch mark.trigger`; `tests/slow_frames.py --marks` lists the
  frames before each); can't-die (`DOTHEROES_DEV_CANT_DIE=1`: the player's own
  life, found in memory, raised to 4.5 apples whenever it falls to 4 or
  less -- never to full; hits, knockback and enemies untouched; each top-up
  logged). **It heals below full, it does not only stop the lethal hit:**
  every hit that leaves 4 apples or less is undone up to 4.5, so the hero
  spends the fight near 4-4.5 apples, which a normal fight would not.
  Benchmark arms must use the same setting (on in both, with the same
  floor and refill, or off in both); runs with it are reported apart and
  results checked again with normal damage. A version that stops only lethal
  damage would need the game's damage path (not done); the game's state
  for tests (`DOTHEROES_DEV_STATE=1`: life, field area, camera position,
  physics collision tasks -- one per 64 pairs of a step -- in `state.txt`
  and the trace; `tests/drive.py`'s `state` waits on it); for frame-rate
  checks (2026-11-02), the player's object logged every 4 ms
  (`DOTHEROES_DEV_PLAYER_LOG=1`) and the guest functions storing to its
  fields (`DOTHEROES_DEV_WATCH=<offset>`). **Restarting an encounter from a known state** is, for now,
  the save plus a state-checked route (tests/scripts/fight_bench.drive: in
  play in the castle's area, then confirmed in the river's area before the
  fight is measured); a true in-game restart (reloading an area, or putting
  the hero at a known spot) is not done. Since 2026-11-02: named scenarios start from kept checkpoints,
  check every stage and retry from the checkpoint a bounded number of times
  ("Current status", "How tests run now").
- Commit identity for this repo is the owner's own git config; ps3recomp
  patches are committed as `dotheroes <dotheroes@localhost>` on the
  `dotheroes` branch of `build/ref/ps3recomp`.
- Repo: public, GPL-3.0, `main` branch. A similar recompilation
  project for Street Fighter Alpha 3 on the Dreamcast (SFA3) is the model for the
  setup app and release downloads.

## Current status (2026-11-02)

Read this section first; the sections after it are the record it is drawn
from, oldest diagnoses in "History" at the end.

**What runs.** The Windows build (RTX 3070, Ryzen 7 5800X) boots, plays and
saves strict-clean with no unresolved imports: title, new game and prologue,
CONTINUE, the castle's field and interior, the river's area and its fights,
GAME OVER / Continue / Save, the system menu and its pages, the dot editor,
INSTALL, SNAPSHOT / ALBUM, and round trips of installed data, snapshots and
edited characters across restarts. That is a small, early part of the game
(see "Game coverage" below); nothing past the castle area has been played.

**Performance configuration.** Four switches, all experimental, hold 60 fps
in the scenes measured so far. **Every fresh player install turns all four
on by default** (2026-11-03, commit 45033a0): the setup app writes them into
a new install's dotheroes.cfg and adds missing ones to an older install's,
keeping a player's own values; the executable's built-in defaults and the
development runs leave them off unless set. So players now run native
collision, whose known behavioural differences are tracked as open problem 3
below; a player turns it off with `DOTHEROES_NATIVE_CD=0` in dotheroes.cfg.
The switches:
`DOTHEROES_NATIVE_CD=1 DOTHEROES_NATIVE_SOLVER=4,5,6 GCM_WRAP_LAP=1
DOTHEROES_PRESENT_ONCE=1` -- the baseline every timing uses
(build/baselines/2026-11-01-config-validation; river fight p99 17.2-17.4 ms,
exploration p99 17.3 ms, no frame over 25 ms in exploration). **Native
collision (`DOTHEROES_NATIVE_CD=1`) is not bit-exact with the game's SPU
task and its contact differences are unresolved** (docs/BULLET_NATIVE.md,
"How close it is"): 6.1-7.8% of mesh/box manifolds have genuinely different
contacts, 0.6-0.85% materially different normals; similar aggregate rates do
not show equivalent behaviour. The three native solver commands are
bit-exact. The default (all four off) still drops to p99 29-38 ms in
debris-heavy fights. Native inflate (the Edge zlib map packs) is on by
default and exact (below), not one of the four.

**Stable baseline.** Tag `stable-2026-11-02` (commit db11101; ps3recomp
0001-0085, its local tag `dotheroes-stable-2026-11-02`): the build the
20-reload soak, the dungeon route and the fullscreen tests ran, with the
four switches. Its executable (sha256 9a42d867...) is kept locally in
build/stable/2026-11-02/ with a README; `BIN=build/stable/2026-11-02/
dotheroes.exe` runs the tests on it. What came after (native inflate, the
Finalize fix, wait-free creation) is measured against it.

**Open problems, most important first.**
1. **Long sessions with area reloads lost 60 fps -- four causes found and
   fixed (2026-11-02, ps3recomp patches 0080, 0081, 0083, 0084); a crash
   found and fixed on the way (0082). The 30-minute soak on the final build
   held: 20 of 20 cycles, every fight p99 17.2-17.9 ms, the last five as clean
   as the first, 3 frames over 25 ms in 11 minutes of fighting, no crash
   (soak-reload-30m, build/baselines/2026-11-02-long-sessions).** Memory
   after the first six minutes grows ~2 MB a minute (~2.6 MB a cycle;
   1,262 -> 1,300 MB over 21 minutes): small, tracked.
   A 35-minute play soak cycling the river fight through TITLE -> CONTINUE
   held 60 for four cycles; from the fifth on, 18-22% of fight frames took
   ~33 ms (p99 34-36 ms), and 8% while idle. All three causes sent the
   scene's draws to the draw engine's copy path (converted on the CPU every
   frame), where the walker's drain goes from ~5 to 11-18 ms a frame and the
   main thread waits for the flip:
   - *Dynamic for good* (0080): a cached draw whose memory had been
     rewritten four times in its life stayed on the copy path, and each
     reload rewrites the area's static geometry once. After the fourth,
     ~145k of ~165k draws per 5 s took the copy path (~20k before). Six
     river cycles a run, one build (FIGHTS=6 ROUNDS=8; build/test
     geomfix-*): the old rule (`RSX_GEOM_DYNAMIC_LIFETIME=1`) p99 17.2-17.4
     ms in fights 1-4, then 35.5 and 33.6 ms; the new one 17.2-17.4 in all
     six, in three runs.
   - *The geometry arena ran out* (0081), which the first had hidden: a
     20-cycle soak with that fix (soak-fix-30m) held fights 1-7, then ~10% of
     frames at 33 ms from fight 8 and ~70% from fight 16 (30 fps). The Vulkan
     backend gave a request the smallest free block of its size or larger,
     whole, and freed released blocks only when out of room; each reload
     re-uploads ~130 MB, and after about six the six 64 MB chunks were spent
     and cut up. Now a buddy allocator: blocks split for smaller requests and
     merged with free buddies, released blocks freed once the batches that
     could read them have finished. Eight cycles with every block checked
     (`RSX_GEOM_ARENA_CHECK=1`, buddy-check): no overlap, no wait for room,
     ~187 MB in blocks throughout, fights p99 17.2-17.4 ms, none over 25.
   - *Loads still tripped the rule* (0083): with rebuilds up to 60 frames
     apart counting, a load rewriting an area several times within a second
     made parts of the scene dynamic now and then, and their retry waits
     doubled at every load -- 22k, then 76k, then 158k draws per 5 s on the
     copy path over 17 cycles (soak-buddy-30m). Now only rebuilds within 8
     frames of each other count, and an entry that stays valid 600 frames
     after a retry starts its backoff over.
   - *Hot pages for good* (0084): with all of that (soak-final-30m), fights
     1-8 held (p99 17.3-17.4 ms), 9-15 had 2-3% of frames over 18 ms and
     16-20 had 24-38%, dynamic draws jumping in steps to ~65k and ~140k per
     5 s. vm_watch stopped watching a page after 16 writes in its life
     (`VW_HOT`), and each reload writes the area's pages again: once its
     pages were "hot", an entry's every retry failed to store, it was
     rebuilt each frame and went dynamic again with a doubled wait. Now only
     writes within 1 s of each other count, and a hot page not seen written
     for 10 s is watched again.
   *The crash* (0082): three of these soaks crashed inside the NVIDIA driver
   (nvoglv64.dll: a null read, or two threads writing one address), each
   just after a hitch mark -- the 10th or the 100th, when "MARK n" on screen
   grows a digit and the overlay's staging buffer is made again. The overlay
   freed the old buffer with vk_destroy_buffer, which first flushes the
   walker's batch: from the present thread, while the walker was recording.
   tests/scripts/mark_stress.drive (120 marks 0.3 s apart) crashed at the
   10th every time; with the overlay's buffers freed without that flush,
   120 marks and no crash. The overlay came with patch 0069 (2026-11-01):
   any session that marks ten hitches could have crashed this way. A run
   under the Vulkan validation layer (`VK_INSTANCE_LAYERS=
   VK_LAYER_KHRONOS_validation`, four fights) reported no error. On the way,
   vm_watch's fault handler looked at a page another thread had released
   outside its lock, where a consumer could watch the page again in between
   (0084; found by reading, not shown to have caused anything).
   Any long session -- area changes in normal play reload geometry too --
   needs all of these.
   **Memory over repeated visits -- bounded (2026-11-02).** The dungeon
   route's "+68 MB a minute" was the first trip filling caches (785 -> 1400
   MB working set over the first cycle: town, field 15, the dungeon). A
   10-cycle soak (dungeon-mem10, memory.csv now with threads and handles,
   `RSX_GEOM_CACHE_STATS=1` with a census of device objects, ps3recomp 0088)
   then grew +17, +6, +7, +9, +3, +2 MB a cycle. The caches fill and stop:
   textures 89 -> 102 (the engine's cache holds 1,024), pipelines 171-182,
   colour targets 65 throughout, the geometry arena at its six chunks by
   design (0081). One thing grew with every visit: threads, 54 -> 73. The
   game builds a SPURS instance for each area load, runs one Edge zlib task
   in it and finalizes it; on hardware Finalize ends the task, here it stayed
   parked in WAIT_SIGNAL -- one SPU pool worker per load (26 after ten
   trips), and the parked-task table's 64 slots would have run out in a long
   session (a task parked past them is woken only by its 1 s timeout). Fixed
   (ps3recomp 0087: a task whose instance is finalized ends at its next
   WAIT_SIGNAL; `SPURS_FINALIZE_KEEP_TASKS=1` for the old way). The same
   soak after the fix (dungeon-mem10b): 10 of 10 cycles; working set 1399,
   1420, 1426, then 1427-1428 MB from cycle 5 to 10 (+0 the last three);
   threads 52-54; two pool workers (build/baselines/2026-11-02-dungeon-memory).
   The river soak's ~2 MB a minute (above) is probably the same leak at one
   load a cycle -- not re-measured since.
2. **Fight spikes** (about one frame of 25-37 ms a minute; the spike run,
   build/test spikes: three 80-second river fights with hitch captures, GPU
   timing and physics stats): at debris bursts. Either the main thread
   itself is busy 25-30 ms a frame for a few frames (two to four physics
   steps a frame on 180-370 contact points, its physics wait only 1-2 ms:
   the game's own code with the new debris; a 54 ms frame in one fight), or
   the walker's drain takes 16-19 ms as the draw count jumps, once with a
   10 ms command-ring wait. The first frames in the river's area after boot
   (only the first entry) have a 30-39 ms frame from the walker alone (16 ms
   drain, the main thread idle). Next: a main-thread profile of the burst
   frames (`DIAG_PROF` with the trace's clock) to see which game code grows.
   The isolated 66.6 ms frame of a 45-minute attract run: an attract run
   with hitch captures of 40 ms+ is the trace (`SCENARIO=attract`; not run
   yet -- twice preempted on 2026-11-02).
   **The dungeon's entry -- traced and its largest part fixed (2026-11-02).**
   The 93-95 ms frame came ~0.5 s after walking in (area 37), not at the
   floor switch as first written (the segment's clock started at the area
   change). Its work: the room's map pack (map/pac_data/D_1_1_1.ps3.pac.gz,
   2.6 MB of deflate to 13.2 MB) inflated by Sony's Edge zlib SPU task, in
   lifted code, ~100 ms (90% zlib's inflate loop, 10% DMA copies), while the
   game's main thread waited in cellSpursEventFlagWait (trace runs
   entry-trace 1-4: PS3_FTRACE, DIAG_PROF on the main thread, the SPU jobs
   and the walker; `tests/prof_report.py --at` takes one frame's samples).
   **Now native** (ps3recomp 0086: SPU entry hooks in the lifter;
   port/native/inflate.c, edge_inflate.c, config/spu_entry_hooks.json): the
   pack inflates in 13 ms; the overworld's 42-43 MB packs in 40-53 ms instead
   of ~300 ms inside their loads. Identical to the lifted routine's output on
   7 of 7 loads (`DOTHEROES_NATIVE_INFLATE=check`) and to zlib on all 167
   gzip files of the disc (tests/native/inflate_check.py); on by default,
   `DOTHEROES_NATIVE_INFLATE=0` for the lifted task
   (build/baselines/2026-11-02-dungeon-inflate). The entry's worst frames
   went from 95/84/53, 93/54/33, 93/47/33 ms (three cycles) to 83/53/33,
   50/33, 49/33. What is left: a ~50 ms frame of the game's own area set-up
   on the main thread (PPU code, func_009731A8 and many others; every
   visit), a 33 ms frame, and on the first visit an 83-100 ms frame on the
   walker -- 35 new colour targets and 10 textures, each flushing the batch
   and waiting for the GPU twice (~30 ms of waits), and 48 shader
   translations (~20 ms). The creations no longer wait (ps3recomp 0089: a
   new image's first barrier and clear go into the open batch;
   `VK_SYNC_CREATE=1` the old way): that frame 83.4-83.5 -> 66.8-66.9 ms
   (ABAB, three cycles each; later visits unchanged), no validation-layer
   error in a cycle with it (build/baselines/2026-11-02-dungeon-create).
   **Shaders and pipelines are now kept between sessions** (ps3recomp
   0090). The translator already had a disk cache of SPIR-V
   (`PS3RECOMP_SPIRV_CACHE`, one file per shader) but it was opt-in and
   nothing set it, so every session translated again; and pipelines were
   built with no `VkPipelineCache` at all. Now the installed game sets the
   directory (`<install>/data/cache/shaders`, port/main.cpp), it also holds
   a pipeline cache, and startup checks and preloads both; test runs keep it
   off unless they name a directory (play sessions: build/play-route/cache). The dungeon entry measured three ways (one cache directory,
   NVIDIA's own disk cache moved to an empty test folder with
   `__GL_SHADER_DISK_CACHE_PATH`, `VK_COMPILE_LOG=1`;
   build/baselines/2026-11-02-shader-cache): **cold**, ~1 s after walking
   in, one 267-278 ms freeze while 40 pipelines are built and 10 shaders
   translated (59 pipelines and 11 translations in the segment, 261 ms);
   **warm** after a restart, twice: no translation, the same 59 pipelines in
   3 ms in all, no frame over 61 ms. (The earlier runs' 67-83 ms frame was
   the in-between case: NVIDIA's cache warm from earlier runs, ours absent.)
   The startup check: a translation file of another glslang or translator
   build is removed, the pipeline file is used only for the same vendor,
   device, driver version and driver cache UUID -- a copy with the driver
   version changed started empty and said so (cache-tamper). What still
   compiles during play, warm: nothing on this route; the compile log
   (`[vk-compile]` lines, counted per segment by tests/batch_report.py)
   shows any that does. A damaged file: one translation with its tail
   garbled was handed to the driver as it was, and its pipeline failed on
   every draw (11,912 failures in one run). Translation files now end with a
   checksum of key and words (ps3recomp 0091, cache format 2), so a damaged
   or truncated one is a miss and is translated again, and a pipeline the
   driver refuses is remembered and not asked for again. NVIDIA's own cache
   follows `__GL_SHADER_DISK_CACHE_PATH` for Vulkan on Windows (its files
   appeared in the test folder), which is how the cold runs were made cold.
   **The room set-up frames, tracked apart:** walking into the dungeon's
   first room costs two frames of ~50-53 ms, ~0.6 s apart, every visit,
   warm or cold -- the game's main thread busy in its own code (the profile
   at the entry: func_009731A8 and many others, with demand-commit and
   write-watch faults among them: vm_commit_veh, vw_fault), not shaders.
   Next for it: a main-thread profile of just those frames
   (`tests/prof_report.py --at`) to see whether the host's page faults are a
   real share.
3. **Native collision is on in every player install; its behavioural
   differences are tracked, not resolved.** Known (docs/BULLET_NATIVE.md,
   "How close it is"; check mode on one field and one river run): mesh/box
   manifolds 85-91% identical, 5-6% with other contact points, 0.4-0.7%
   with a normal more than 5 deg off; the lifted task checked against itself
   differs by similar rates, but similar rates do not show the same
   behaviour. No gameplay difference has been seen or ruled out: two scenes
   checked, and no person has played at length with it. Track: check mode
   (`DOTHEROES_NATIVE_CD=2`) on each newly driven scene (the dungeon, the
   fields after it), and any report of debris, blocks or bodies behaving
   differently from the PS3 (compare with RPCS3 on the same save). If a
   gameplay difference shows, the install's default goes back to 0 until
   it is fixed.
4. 144 fps uncapped is not sustained (~125-130 fps; the main thread, the
   game's own code, is the limit). Stretch goal.
5. Not verified: save portability (PARAM.PFD is not rewritten) and the colour
   comparison with RPCS3 (its capture never got past preloading pipelines).

**Milestones.**

| Milestone | State | Evidence |
|---|---|---|
| Windows native build, strict-clean, 0 unresolved NIDs | done | every run's `strict: none`, `hle: 0` |
| Boot -> title -> new game / CONTINUE -> play | done | tests/scripts/new_game.drive, load_save.drive |
| 60 fps in exploration (validated configuration) | done in the scenes measured | build/baselines/2026-11-01-config-validation |
| 60 fps in debris-heavy river fights (validated configuration) | done in short runs | same; fight_bench / river_fight scenario |
| 60 fps held across a long session with reloads | done for 20 river reload cycles (every fight p99 17.2-17.9 ms, no crash) | build/baselines/2026-11-02-long-sessions |
| Gameplay speed independent of frame rate (30/60/120/144) | done for the hero, river enemies, physics, timers | build/baselines/2026-11-02-frame-rate |
| Save / load, GAME OVER save | done | tests/scripts/save_game.drive |
| INSTALL, SNAPSHOT/ALBUM, dot editor SAVE, across restarts | done | tests/scripts/roundtrip_*.drive, editor_roundtrip_*.drive |
| First dungeon, first boss | **manual:** played by the owner through the dungeon and its boss (no crash, p99 17.6 ms). **Automated:** the way there and the first room (two dialogues, a floor switch), 3 of 3 and 10 of 10 cycles; a save in the first room continues at the dungeon's door (checkpoint dungeon1-entrance); the deeper rooms and the boss not yet | owner session 2026-11-02; tests/scenarios/dungeon1_approach.drive, dungeon1_save_continue.drive |
| Memory bounded over repeated area loads | done for the dungeon route (flat from the 5th of 10 cycles) | build/baselines/2026-11-02-dungeon-memory |
| 144 fps uncapped | not met (~125-130) | build/baselines/2026-11-02-frame-rate |
| Save portability, colour vs RPCS3 | not verified | -- |
| Setup app end to end, first release | not started | -- |

**Next actions, in order.**
1. Keep the 30-minute cycling soak (river_fight, FIGHTS=20 ROUNDS=8) as the
   standing long-session test after any renderer or memory change: reloads,
   deaths and area changes, not idle time. Then longer sessions with real
   area changes (the owner's play session gives them).
2. The first dungeon to the boss: the east room's north door is solved (the
   boomerang, "Game coverage"); dungeon1_east from checkpoint
   dungeon1-boomerang drives to the six-pillar room north-east whose west
   door stays barred (its wall switches and chest: the owner's record
   1,466-1,486 s), then the record's 1,486-1,534 s to the portal, then the
   boss in area 38 (record ~1,535-1,592 s), measured like the river fight
   with normal damage and real audio in representative runs. The area
   changes' 60-94 ms frames stay tracked (open problem 2's room set-up
   frames and the field's pack inflates); a pack-cache experiment is
   measured on the same revisits before it counts.
3. The hitches: what is left of the dungeon entry (the walker's first-visit
   frame: resource creation without GPU waits, then the shader translations
   -- a disk cache of them; the main thread's ~50 ms area set-up), the fight
   spikes (profile the main thread in the debris-burst frames, the walker's
   drain when draws jump), the first area entry's walker frame, and the
   attract run for the 66.6 ms frame (tracked; reproducible problems first).
4. Then the stretch goal (144 fps), colours vs RPCS3, save portability, the
   setup app and a first release.

**How tests run now (2026-11-02).** A test is a named scenario
(tests/scenarios/*.drive) run by tests/boot_smoke.py with `SCENARIO=<name>`:
its header names the kept checkpoint it starts from (build/checkpoints/,
copies of the owner's saves, never cleaned up by runs) and the settings it
needs; the drive checks the game's state at every stage (state.txt: life,
area, the hero's position, the system menu's word), fails a stage on a
timeout, a death, leaving the area or a menu left open, records each
failure with the state and the log's tail (failures.jsonl), takes the way
back to the checkpoint and retries a bounded number of times, and records
each stretch it measures as a segment (segments.jsonl): fights must show
debris physics and enemies losing life, or they do not count. The run ends
when the drive ends; a run stopped by the on-screen guard is stopped the
game's own way too (its frame log is kept), and a run that left a crash
report says CRASH whatever ended it after (a crash first showed up as "game
window covered"). tests/batch_report.py turns a batch of runs into a
report: completed and failed scenarios, combat time, workload, frame-time
percentiles and spikes per minute per kind of segment, worst frames, memory
growth, errors, the exact build and settings; invalid runs first and kept out
of the totals; settings never pooled across configurations. Hitch captures
(`PS3_FTRACE_ROLL=hitch`) keep a rolling frame trace and write the seconds
around each slow frame or tester's mark (tests/slow_frames.py reads them);
the copy takes 0.3-4 ms and the write 3-9 ms, on a thread of their own. Their
cost in frames does not show: two river runs of two 80-second fights each way
(ABBA, build/test ovh-*), without p99 17.3 ms, 0.37% of frames over 18 ms,
0.7 a minute over 25; with p99 17.3, 0.31%, 0.5 a minute. Play sessions
(`SCENARIO=owner_play`, tests/scenarios/OWNER_PLAY.md) record inputs, areas
and positions, hitch captures and a copy of every save, fullscreen at the
desktop's resolution (the 720p picture scaled); one starting stops any test
run and holds further ones (build/test/HOLD; `tests/stop_tests.py` does the
same by hand, `--resume` lifts it). `tests/run_batch.py <name> <tag:secs:ENV=..>
...` runs a batch, ends at a hold, and writes its report to build/reports/;
scenarios share their blocks through `include common.drive`; routes from a
play session's record (`tests/route_from_record.py --buttons`) walk by
position (`goto`, fine pad steps for narrow ways), cross area edges and doors
until the area changes (`cross`), replay the tester's action buttons where
they pressed them (dialogues, switches), and step free of a dialogue left
open (`unstick`); `fight <class>` clears one class of enemies near the hero
(actors.txt now lists each character with life left: class, x, z, life);
tests/route_from_record.py turns the record into a route of `goto`
waypoints and area checks.

**Game coverage (2026-11-03).** What has been run -- scenes, not the whole
game. Driven tests start from the owner's early saves (the castle, the first
sword) or, since the owner's first play session, from its saves.
- *Gameplay, driven:* the castle's field; the castle's interior (the gate's
  OPEN, the throne room, out again; tests/scripts/explore_castle.drive); the
  river's area east of it and its fights (sword, the full-life giant sword,
  debris, damage, deaths with can't-die and with normal damage); GAME OVER,
  Continue and Save; a new game (character select, name, the prologue, the
  first steps).
- *Automated scenarios, gameplay:* the first dungeon's approach -- town,
  field 15, the dungeon's door -- and its first room's two dialogues and floor
  switch (dungeon1_approach: 3 of 3 cycles, then 10 of 10 in the memory soak,
  normal damage); a save in that room and CONTINUE (dungeon1_save_continue).
  Past the switch (from checkpoint dungeon1-entrance; can't-die on; the
  owner's record 1,006-1,091 s): the north room's fight, the east room's
  fight, its chest (the key) and the locked door west of the north room are
  driven and pass (dungeon1_route.drive's room2, east_key, chest,
  locked_door; dungeon1_room2, dungeon1_persist). `fight` walks to the
  nearest enemy with life and swings; the chest is tried across its width
  and the door retried with it, because whether the key was had shows only
  at the door. **What a dungeon save keeps** (dungeon1_persist, 2026-11-02):
  saved past the key's door, TITLE, CONTINUE -- the hero is back at the
  dungeon's entrance door (area 37, (88.8, -130.4)); the floor switch's door
  and the key's door stay open (walked straight through); the rooms'
  enemies come back. That save is checkpoint dungeon1-past-key-door.
  **North of the key's door** (2026-11-03, dungeon1_deeper from
  dungeon1-past-key-door, can't-die on): the west rooms (a block pushed into
  an enclosure; its chest, approached from below at x 37.6-38.0, gave the
  key to the door north-west of the bat room), the bat room cleared (its west
  door is barred until then), that door, the fire-pit and enclosure rooms
  cleared, the enclosure room's north door (it opened in the runs that also
  killed a life-32 enemy in the fire-pit room, so it is retried with both
  rooms cleared), the room beyond it (a 10 G chest) and back south; saved
  there as checkpoint dungeon1-north-rooms (CONTINUE: the dungeon's door,
  the doors kept). **The boomerang, and the east room's north door**
  (2026-11-03): the owner's record shows that door passed at 1,331 s, two
  seconds after CIRCLE throws across the lava at the east wall's red switch
  -- the boomerang, which no checkpoint had. It is in the enclosure room's
  chest (the record's 1,240 s; a walkthrough agrees): the enclosure's block
  (FE6878 at x 58.4) pushed south with a 2 s hold (a goto gives up before
  it moves), then up into the chest's front (it opens from (64.2, -47.0),
  the right half) and CROSS; the HUD's circle slot then shows it ("It can
  briefly stun, ..."). north_rooms now takes it, the room beyond's wall
  switch and chests too; a save after it is checkpoint dungeon1-boomerang.
  From it (dungeon1_east, can't-die on): the rooms south and east of the
  entrance, the east room's switch hit (a hit switch turns from a red
  cylinder out of the wall to a disc facing out) and its north door passed;
  the north-east rooms (a fight on entering each room -- one with enemies
  bars its doors --, the key's chest in the four-block room, out along its
  west wall); three pushable blocks across the way north (the two side ones
  up, the middle one west, as the owner did at 1,459-1,464 s); into the
  room beyond (six pillars, three wall switches, a chest, a stone: "Glory
  shines upon one who banishes darkness."): its west door opened once the
  west wall's switch was hit too (east24; thrown from in line with each
  switch -- the east room's misses from 1.6 off its z), then the rooms west
  and north to the boss's door (the owner's 1,486-1,534 s), saved there
  (checkpoint dungeon1-boss-door). **The boss's door wants a special key**
  ("You need a special key."), not found yet. Ruled out: the six-pillar
  room's chest (it appears once the room's last enemy dies and gives a Small
  Key -- opened from its front at (140.4, -48.6) with a short step up, east34;
  holds and stalled gotos slide off its corners), the room west of it (no
  wall switches there; the owner's four throws in it hit nothing listed),
  the portal room's four pots (FE65F0, broken: the owner's swings). The
  portal room has two floor plates by the door: FE6ED8 (84.0, -24.8) lights
  green when walked over; FE6A20 (95.2, -24.8) stays dark, and the owner
  stood beside it at (93.9, -24.8) for 3 s with 3 presses (1,528-1,531 s),
  then walked straight through the door. The map (L1) shows every room
  explored but one, behind a red door north of the middle room (cell
  (3, -3); a guide's Red Door that needs a bought Red Key). Asked the owner.
  New drive steps: `switches` (the boomerang at each wall switch in the
  room, in line with it). Not automated yet: the special key, the boss
  (area 38). Smoothness on the way (east36, can't-die, 13.5 minutes of the
  east, north-east and approach rooms with their fights): p99 17.2 ms, 0.09%
  of frames over 18 ms, 4 over 25 ms, worst 34 ms; memory 1,002 -> 1,042 MB.
  The route is not yet reliable end to end: about one run in three stalls
  in the room before the north-east rooms (x 95, a way east the goto does
  not find) and starts over. Drive changes for the dungeon: facing
  taps became short holds (a tap does not turn the hero), `rooms 25.6 19.2`
  (a goto into the next room that stalls goes through the door at that
  wall's middle), `open` nearest first and soft.
  Normal damage with sound (rep-dmg1, dungeon1_persist): the north and east
  room fights at p99 17.2 ms, under 0.2% of frames over 18 ms, worst 33 ms
  (the 50 ms frame is the first room's set-up frame); the hero died in the
  east room and near its chest (GAME OVER, Continue, the way back driven).
- *How 60 fps looks on a 144 Hz display* (2026-11-03, build/baselines/
  2026-11-03-display-pace, fullscreen on the owner's 143.98 Hz desktop): the
  game's 60 fps frames do not divide the display's refresh, so in FIFO order
  each is held for 2 refreshes or 3 -- 2 for 64-73% of frames, 3 for 27-35%,
  the hold changing from one frame to the next on 49-59% of them: uneven
  motion with every frame on time (the presents the owner's first session
  marked). `DOTHEROES_FPS=display` paces the vblank to a whole number of
  refreshes (71.99 fps here; the game's logic follows time): 93-95% held
  exactly 2, the hold changing on 8-12%. It holds in fights: the river fight
  p99 17.0 ms (19.8 at 60), worst 27.9 (33.5), 1.5% of frames over 16 ms
  (a refresh missed: that frame held 3); the field route p99 17.3, 1.9%
  over 16 ms. Not yet the default: the owner's boss session runs with it,
  to be judged by eye. One drive artefact: an item menu opened and closed
  in the owner's recorded rhythm took a later press to close at 72 fps.
- *New field areas* (fields_beyond from field-area14, 2026-11-03): field 14
  is walled in round the owner's ground -- a cliff west of x 56.8, a row of
  rocks south of z -183.3 with a golden chest (FE6660 at (58.4, -188.0), on
  a ledge) beyond it, likely an item's job (bombs: the hero has none);
  `cross DIR any` there found no other way out. The ways on from here are
  for the owner's next session or the map.
- *After the first dungeon* (field_after_dungeon from after-dungeon1,
  2026-11-03): the town to field 15, area 8 north of it, the cave 117 (an old
  man: "Come back when you have a bow."), field 14, the house 146 (villagers,
  a well, a YES/NO question) and out, every area change confirmed: passes
  with can't-die (p99 17.2 ms); with normal damage the hero died in the
  fields (the route's swings are the owner's, not fight-aware) and the game
  put it back at the inn. Memory is bounded on this route (field-mem2c, two
  cycles with the title and CONTINUE between, build/baselines/2026-11-03-
  field-memory): it rises on the first visits (1,067 MB at 60 s, 1,535 at
  the end of cycle 1), the reload adds ~20 MB, and cycle 2 holds 1,557-1,560
  MB through the same areas. The shader cache on this route (build/baselines/2026-11-03-
  field-shader-cache): cold, 20 translations and 123 pipelines (482 ms) in
  the route and three stalls of 100, 108 and 133 ms; warm, nothing translated,
  116 pipelines in 5 ms, no frame of 100 ms+, p99.9 43.2 -> 19.9 ms. Frames of
  60-94 ms remain in both, not shader work: tracked with the room set-up
  frames. **What they are** (field-prof, build/baselines/2026-11-03-field-
  area-frames: DIAG_PROF on the main thread with the frame trace, the seven
  55-110 ms frames of the route): about a third of the main thread's samples
  wait in cellSpursEventFlagWait and a third run game code around
  func_0091BC28/func_0091C080/func_0091C620 (the area's set-up); no page-fault
  handlers among them. The SPU work waited for is the overworld's map packs:
  39-47 MB inflated natively in 42-66 ms at each field-area change, and the
  same pack again on every return (three times each on this route). **A
  pack cache does not shorten them** (2026-11-03, build/baselines/2026-11-03-
  pack-cache-ab: field_after_dungeon, cache off/on/off/on, warm shader cache,
  tests/inflate_frames.py on the same revisits): a hit (the input compared
  byte for byte, the kept output copied) took 20-27 ms against 47-55 ms
  inflated, yet the revisit frames stayed 60-72 ms (43 MB pack, second and
  third visits: off 60.7/69.0 and 60.7/64.9 ms, on 71.6/65.5 and 66.4/65.0)
  and 47-52 ms (39 MB pack); first visits got slower (the extra copies:
  inflates 54-83 ms, frames up to 66 against 51). The main thread's work in
  those frames, not the inflate, sets their length. Dropped; every inflate's
  time stays logged (DOTHEROES_INFLATE_LOG=1, on in field_after_dungeon) so
  tests/inflate_frames.py tracks these frames per area change. Next for
  them: the game's area set-up code (func_0091BC28 and around) profiled on
  its own.
- *Played by the owner* (manual coverage, not counted as automated)
  (2026-11-02, 38.5 minutes, the validated
  configuration with every fix of the day -- the session's binary is the one
  the hot-page soak ran; windowed, sound on, normal damage, diagnostics on:
  build/sessions/boot-20261102-134835-owner-play): field areas 11 and 14-18,
  ten or so building interiors, and **the first dungeon** (areas 37-39,
  reached from field area 15) **with its boss** (area 38: a hard fight, life
  10 -> 6, then a heart container, 12). No crash; 136,000 frames, p99 17.6 ms,
  0.71% over 18 ms; per area p99 17.1-19.8 ms -- the roughest field areas 14
  and 15 (2.5-3% over 18 ms), the dungeon's main area 37 (9 minutes) 17.5 ms.
  The owner's three marks fell where the game's frames were on time (18-21
  ms) but window presents were uneven (53 of 300 over 20 ms in the 5 s before
  the first two; 27-28 ms around the third). Three saves kept as checkpoints
  (before-dungeon1, after-dungeon1, field-area14) and the route as positions
  (tests/scenarios/dungeon1_approach.drive).
- *Menus and features:* the title menu (NEW GAME, CONTINUE, EDITOR, ALBUM,
  INSTALL), the system menu (RESUME, SAVE, LOAD's neighbours, OPTIONS,
  SNAPSHOT, ALBUM, BESTIARY, LOADING ART, TITLE), the item pages, the map; the
  dot editor (draw, save, reload); install, snapshots, album and edited
  characters across restarts.
- *Frame rates:* 30, 60, ~120 and 144-paced (movement, timers, physics).
- *Rendering only* (the attract mode's demos, no input): desert, a lava
  dungeon room, the red volcano area, the forest. They render at 60 fps; none
  has been played.
- *Long sessions* (stability evidence; only the cycling soak is gameplay):
  - the attract mode, 45 minutes: rendering and stability only -- no crash,
    memory 805 -> 807.5 MB, p99 17.3 ms, five frames over 25 ms, one of them
    an isolated 66.6 ms at +1081 s (tracked, not yet traced);
  - in play but idle, 30 minutes standing in the river's area after its
    drive stalled: stability only, not gameplay coverage -- p99 17.1 ms,
    nothing over 19.5, memory 1107 -> 1115 MB;
  - the cycling play soak (soak-play-30m-b, 35 minutes,
    tests/scripts/soak_play.drive): **14 of 14 intended cycles completed**,
    each checked in play at the castle and then in the river's area; debris
    physics in every fight, the hero hit in 7. Of the 35 minutes: combat 3.7
    (fourteen 16-second fights of swings at the river's entrance), scripted
    walking 5.4 (the 23-second route each cycle), menus, title, loads and
    transitions ~6.4, and ~18.6 idle at the castle after the drive ended
    (stability only). Frames: fights 1-4 p99 17.3-18.1 ms; from cycle 5 on
    p99 34-36 ms with 18-22% of frames at ~33 ms, and 8% in the idle tail --
    open problem 1. Memory 1321 -> 1393 MB over the cycles (~5 MB a cycle),
    flat while idle.
- *Not played at all:* dungeons and bosses past the first, shops, NPC dialogue
  beyond the castle, items other than the sword (bombs, boomerang, lamp, ...),
  magic, fairies, side quests, later areas, the ending; OPTIONS' settings
  changed; the editor's SETTING, PREVIEW, other poses and classes; trophies;
  a measured run with real audio; Linux.

## The platform and the early speed work (to 2026-10-31)

*The record of how the port came to run on Windows. Its numbers are from
before the physics work and are superseded by "Current status" above; the
setup app's state is still current.*

**Linux (tested):** boots, plays Bink movies and FMOD audio, title -> new game
-> opening -> overworld with dialogue and movement, strict-clean. Gameplay is
~31 fps on the laptop with the frame cap lifted (menus ~60), limited by
hardware there: the FIFO walker thread (CPU vertex work, copies into the
Vulkan ring) and ~22 ms of GPU time per frame on the integrated GPU. History of
the speed work (18 -> 31 fps) is in MILESTONES.md.

**Frame cap:** removed by default (`DOTHEROES_FPS`, `30` / `60` / `0`). The
game's logic is delta-time based; verified by `tests/scripts/speed.drive`
(equal walk distances at 20 and 31 fps). A runtime bug that broke the game's
vblank pacing was fixed (patch 0034).

**Windows (RTX 3070, Ryzen 7 5800X; 2026-10-30): runs natively.**
- The developer pipeline runs on Windows (`build/venv` is a Windows venv; the
  laptop's is `build/venv-linux`); a lift is byte-identical to Linux (patch
  0036). `zig_build.cmake_toolchain()` writes `.cmd` wrappers on a Windows
  host. `.gitattributes` keeps LF everywhere; patches are `-text`.
- `build/port-win` (CMake + Zig, Vulkan): links and runs, strict-clean, 0
  unresolved NIDs. Title, CONTINUE with a save, overworld, walking. Imports
  only the CRT, kernel32, SDL2, winmm, ws2_32, bcrypt (Vulkan loaded at run
  time; no D3D12: `rsx_live_draw.c` is the no-op in Vulkan builds). Dev
  builds write a PDB (`port/CMakeLists.txt`).
- Fixed on the way: the Vulkan backend ignored the guest's polygon offset
  (`set_depth_bias`), so the shadow pass shadowed the ground itself: that was
  the "swirl" (moire rings). The diag watchdog and `DIAG_FPS` now run on
  Windows; `DIAG_PROF` has a Windows sampler (SuspendThread + unwind tables)
  and `tests/prof_report.py` symbolizes through DbgHelp.
- Tests: `tests/boot_smoke.py` (both OSes; `SAVES=`, `WINDOW=1`, `DUMP=0` for
  timing, `PASS_DUMP=1`, `WRAP=` for a profiler), `tests/nsys_capture.py`
  (Nsight Systems; CPU sampling needs an Administrator shell),
  `tests/scripts/load_save.drive`, `perf_walk.drive`, `save_passes.drive`.
- *(Superseded: 2026-10-30 numbers, before the physics work and the validated
  configuration.)* **Speed (perf_walk.drive, gameplay): a solid 60 fps** (2026-10-30), from
  ~45. Measured with PS3_FTRACE + `tests/frame_times.py`, 20 s of walking:
  60.0 fps, median 16.7 ms, p99 ~21 ms, no frame over 25 ms, windowed or
  headless. With the vblank at 125 Hz (DIAG_VBLANK_MS=8) the game runs ~101
  fps: 60 has ~65% headroom; the game's own logic (~9 ms) is the next limit.
  Gameplay speed is unchanged (tests/scripts/speed_save.drive: same position
  at 60 and ~100 fps). Two changes:
  - **Resident geometry** (ps3recomp `rsx_draw_engine.c` eng_geom_*, the
    Vulkan backend's geom_* arena, `runtime/memory/vm_watch.c`): a draw's
    converted vertices and index list stay in GPU memory under a key of the
    registers they were built from, valid while no guest page they were read
    from has been written. Writes are caught by page protection (a watched
    page is read-only; the first write faults once into a vectored handler,
    any thread, zero cost otherwise); kernel writes into guest memory release
    their range first (fs reads). The walker's drain went 15.2 -> 4.8 ms.
    RSX_GEOM_CACHE=0 is the old path; RSX_GEOM_CACHE_CHECK=1 rebuilds every
    hit and compares (0 misses through boot, title, load, walking); the
    castle frame is pixel-identical on and off. Windows only for now
    (vm_watch is VEH-based; Linux needs the mprotect/SIGSEGV twin).
    GetWriteWatch was tried first and dropped: ~7 us a call, polled per FIFO
    pass it cost more than it saved.
  - **Admission and eviction** (patch 0041). The keys that kept arriving
    were not UI or particles: the post-processing quads (tone map, bloom
    and blur chain) draw from a streaming vertex ring at 0x01A00000 whose
    offset moves every draw (`tests/geom_newkeys.py` on an
    RSX_GEOM_CACHE_LOG run). Their unwatchable pages left a placeholder per
    key, which filled the table and forced full resets. Now: no
    placeholder; an entry is admitted on its second sighting through a
    fixed 4096-key filter; out of room, the entries drawn longest ago go
    (never one drawn in the last 2 s), their GPU blocks reused once the
    GPU is idle (power-of-two blocks, deferred free); still no room, new
    entries pause 30 frames instead of retrying every draw. Five-minute
    soaks (tests/scripts/soak_timing.drive: walking, menu, map, guard,
    sword): ~1,300 entries / 76 MB, flat after the first minute, no
    eviction needed, 60.0 fps, p99 21 ms, 2 frames over 25 ms in 15,000.
    Forced small (RSX_GEOM_CACHE_MAX_ENTRIES, RSX_GEOM_ARENA_CHUNKS), it
    evicts old entries and keeps the scene in view: 1100 entries / 128 MB
    still gives 88% resident and p99 21 ms.
  - Found on the way: crossing into another field area stalls the game for
    ~1.2-2.6 s (the main thread busy loading; the same with the cache off).
    Compare with RPCS3 or hardware before treating it as a bug.
  - **Cut grass slowed the game to 6-40 fps** (owner's play session, "around
    the water"; tests/scripts/grass_cut.drive reproduces it). A cut tuft's
    debris is Bullet rigid bodies, simulated on SPU: per step one
    BulletCD task (runtime image 4, spu_0003) and ~70 BulletSolver tasks
    (image 5, spu_0004) -- the registry's image ids are the spu_NNNN file
    index plus one. The main thread creates them (cellSpursCreateTask, lr
    0x955d74) and spins on cellSpursQueuePopBody for the results
    (func_00955BD4), and each task got a new host thread: ~116 us to start,
    and 361 us to run cold (SPU_TASKPROF=<secs>). Now async tasks run on
    kept worker threads (spu_workload.c, SPU_POOL=0 is the old way): 4 us to
    start, 122 us to run, the burst at 51-56 fps instead of ~40, worst frame
    34 ms instead of 110. No soak drive ever cut a tuft, so every timing
    before this missed it. Their per-task log lines (65 MB in a three-minute
    session) are now the first few only (SPU_TASK_LOG=1 for all). Still to
    find: the rest of the burst (the solver runs 3.7 ms a task).
  - **Logging is verbose whenever stderr is redirected** (ps3_log_verbose):
    every test run, and the installed game, whose log goes to
    data/dotheroes.log. A play session wrote ~920 [WAIT]/[evt] lines a
    second besides the task lines; measure timing with PS3_VERBOSE=0, and
    consider defaulting players to quiet.
  - **Emergency ring recycle under a working callback** (cellGcmSys): the
    drain recycles a title's ring itself when its buffer-full callback
    seems not to, judged by `current >= end`. But `current` sits exactly at
    `end` whenever a title fills the ring to the byte, until its next
    reserve calls the callback, and here the callback is the runtime's own.
    Recycling then rewound the ring under the title: a 3.4 s stall crossing
    areas. Now never for a ring with the runtime's callback, and only
    `current > end` counts as an overrun.
  - **Controller on Windows**: SDL found the Xbox pad but its input never
    arrived. SDL reads it through raw-input messages to a hidden window
    owned by the thread that initialised the subsystem (a guest thread), and
    the window thread's own SDL_PumpEvents runs the joystick update too;
    cellPad now sets SDL_JOYSTICK_THREAD=1 on Windows. Confirmed by the
    owner. PAD_DEBUG=1 logs every button/stick change as SDL reports it.
  - **The vblank on its own clock** (`port/main.cpp` vblank_clock): the game
    paces every frame by the vblank count, which the walker used to tick from
    a GetTickCount64 loop (16 ms steps, delayed by drains and presents, then
    fired twice to catch up): ~7% of frames took two vblanks. Now a monotonic
    clock ticks it within a fraction of a ms, and completes the flips the
    walker has reached.
  - **A freeze that needed a slow walker** (patch 0043). After a flip the
    runtime wraps the title's 2 MB command ring back to its start once
    little is left, and the title writes its whole next frame (~0.9 MB)
    from there before it waits on a flip again. With the walker still at
    the start of the frame just flipped, a frame larger than the one two
    before it wrote over commands the walker had not read; the walker
    decoded them (a CALL into the ring with no RET, a JUMP to an unmapped
    offset), circled past the flip and resynced to put, and the title
    waited for that flip forever. With the walker slowed by
    RSX_GEOM_CACHE_CHECK, 5 of ~12 runs froze within two minutes (all 6
    with flips completed on the walker instead, tried first as the
    suspected cause); at normal speed the race is only rarer. Now the new
    lap ends at the walker until it gets there (the title's buffer-full
    callback waits for it): 9 check-mode runs of 160-240 s, no freeze or
    resync, the title waiting 4-17 ms about once a run; at full speed it
    never waits, and frame times are unchanged (59.5 fps, p99 21.7 ms
    against 21.2 without it, same drive). Traced with
    GCM_FLIP_RING=1 (patch 0042): the flip handshake and the walker's own
    path (passes, JUMP/CALL/RET, flip markers, resyncs), dumped when a flip
    stays WAITING.
- Before (for the record): the FIFO walker set the frame, drain 15.2 ms
  (~9.4 ms of it re-reading, re-hashing and re-copying static vertex data:
  the vertex cache saw ~100% hits and 0 changes, 491 blocks, 58 MB) plus
  3.7 ms waiting at the flip for the previous present.
- **Still emulation scaffolding** (to replace, not to keep): the RSX command
  stream walked and translated to Vulkan (draw engine), the walker's 1 ms
  FIFO poll, the command ring's wrap/recycle/resync rules (the ring limit
  of patch 0043 included: it keeps the emulated ring honest, nothing
  more), the present path (full-frame readback; present_mu held across
  the GPU fence: the walker still waits ~3 ms a frame there), the emulated
  vblank counter the game paces by, SPU code run inside the SPURS/lv2 HLE
  (async tasks on a pool of host threads, one per task running at once,
  rather than a fixed set of SPUs).
  Native and meant to stay: resident geometry, vm_watch, the vblank clock.
- SPU in play: FMOD's mixer (`spu FMOD:0`, image 5) ~18% of a core and
  `water_task` (image 0, SPURS taskset on `spu job`) ~20%; neither is on the
  frame's critical path. No Bullet task activity in the overworld sample.
- **Colours:** the game's final pass (tone curve 1-e^-x, colour matrix
  brightness ~0.8 / saturation ~1.1, gamma 1/2.2) is translated correctly; its
  input, the scene buffer, holds gamma-encoded albedo where the game expects
  linear light (no texture gamma bits are set). Decoding the albedo sRGB ->
  linear saturates the picture close to real-hardware screenshots but slightly
  dark; find what the PS3 does (compare RPCS3's scene buffer) before fixing.

**Setup app (SFA3 style): the headless conversion run end to end on Windows;
the GUI and the frozen bundle not yet.** `setup_app.py --convert <disc> <keys>
<folder>` (2026-11-02, the owner's disc and keys, a fresh folder) produced a
working install: the game starts with no arguments, keeps its own saves and
shader cache under `data/`, plays with sound. Tested as a player starts it
(`INSTALLED=<install>` in tests/boot_smoke.py: its exe, data folder,
dotheroes.cfg and defaults, the harness's switches dropped):
- *Smoothness:* with the settings the app then wrote (FPS and window only) an
  install fought the river at p99 26.8/27.3 ms, 10-16% of frames over 18 ms
  -- the original collision task, not what development measures. The app now
  writes the validated settings (`DOTHEROES_NATIVE_CD=1`,
  `DOTHEROES_NATIVE_SOLVER=4,5,6`, `GCM_WRAP_LAP=1`,
  `DOTHEROES_PRESENT_ONCE=1`) into a new install's dotheroes.cfg and adds
  missing ones to an older one (a player's own values kept); with them the
  same fights ran p99 17.3/17.5 ms, under 1% over 18 ms, as in development.
  Checked end to end (2026-11-03): a fresh `--convert` (build/player-install-
  1103) wrote them, and its first launch -- shader cache empty, real audio --
  fought the river at p99 17.2 ms, 0.26% over 18 ms, native collision on.
  Native collision is experimental: not bit-exact with the game's SPU task.
- *Cache:* on by default at `data/cache/shaders`; a relaunch read 83
  translations (0.9 MB, 7 ms) and reused the 1.5 MB pipeline cache.
- *Saves:* installed_save / installed_continue: an arrow bought in field
  area 14's shop (HUD wallet 0224 -> 0222, arrows 14 -> 15), the system
  menu's SAVE, the game exited; relaunched, CONTINUE in area 14 at (87.2,
  -119.2) with wallet 0222 and 15 arrows.
- `setup_app.py` (Tkinter GUI, adapted from SFA3's converter), with
  `--convert DISC KEYS FOLDER`, `--update FOLDER`, and `app <tool>.py args`
  (runs the lifting tools inside the frozen app).
- `tools/setup_pipeline.py`: verify -> extract -> decrypt -> recomp -> spu ->
  build -> finish into an install folder; per-step fingerprints for updates.
- `tools/zig_build.py`: player-side compile + link with the bundled Zig
  (flag groups mirror the CMake build), object cache, memory-aware job count;
  developer-side `build_prebuilt()` (runtime library + NID table + deps) and
  `windows_resources()` (icon + UTF-8/long-path manifest).
- `tools/build_deps.py`: SDL2 2.32.10 (shared), glslang 16.6.0 (static),
  Vulkan-Headers 1.4.363 for both platforms with Zig; source hashes pinned in
  `config/deps.sha256`. Built and working on the Linux host for both targets.
- `tools/make_bundle.py`: PyInstaller app + trimmed Zig (keeps libc++) +
  prebuilt libs + sources/headers + tools + licences -> `build/dist/`.
- `tools/shortcuts.py`, `tools/self_update.py` (from SFA3, renamed; no Steam
  artwork chosen yet), `tools/version.py` (0.1.0), `assets/icon.png` (original).
- The installed game (`port/main.cpp`) starts with no arguments: it finds
  `data/disc/PS3_GAME/USRDIR/EBOOT.elf` next to itself, keeps saves in
  `data/gamedata`, reads `dotheroes.cfg` (NAME=value environment defaults),
  logs to `data/dotheroes.log` and opens a window.

## Setting up on Windows

1. Clone the repo. Copy over from the old machine (they are not in git):
   the disc image `3D DOT GAME HEROES [BLUS-30490].iso` (into the repo root,
   git-ignored) and `build/keys/self_keys.txt`.
2. Install Python 3.12+ (with `pip install pycryptodome pillow pyinstaller`),
   Git, CMake, Ninja. Zig 0.17.0 for Windows unpacked as
   `build/toolchain/zig-x86_64-windows-0.17.0/` (`zig_build.find_zig()` looks
   in `build/toolchain/zig-*`).
3. Developer pipeline (clones ps3recomp at the pinned revision, applies
   `patches/ps3recomp/*.patch`, extracts, decrypts, lifts into `build/gen/`):
   `python tools/pipeline.py --iso "3D DOT GAME HEROES [BLUS-30490].iso" --keys build/keys/self_keys.txt`

## Reference tools and saves on the Windows PC

The owner set these up on the PC; ask them for the locations (keep them in
your local notes, never in this public repo):

- **Vulkan SDK** (validation layers, `vulkaninfo`, Vulkan Configurator),
  **NVIDIA Nsight Systems** (CPU/GPU timelines) and **Nsight Graphics**
  (single-frame capture: walk the frame's passes to find the swirl).
- **RPCS3**, outside the project folder, with the PS3 firmware installed and
  the game booting from the owner's disc. Offline reference only (never part
  of the port): compare the same scene for colour and the swirl, see what
  "Install Game Data" should do, capture frames from it with Nsight Graphics
  or RenderDoc and compare pass by pass with ours. Its firmware and files
  must never enter this repo or a release. For stability it needs
  *Advanced -> PPU Event Wake-Up Delay = 35* (its fix for this game's
  random crashes). Window grabs of it without touching its settings:
  `tests/rpcs3_capture.py <rpcs3 dir> <game> <out dir> --config <copy>`,
  where the copy is the game's RPCS3 config with fullscreen off, resolution
  scale 100 and `Shader Mode: Async Recompiler (multi-threaded)` -- with
  the interpreter mode RPCS3 first builds ~6,650 shader variants (over an
  hour) and shows only its boot splash; a first boot still spends minutes
  loading its cached pipelines.
- **Saves past the opening**, made by the owner, so testing can skip it.
  RPCS3 and the port use the same layout,
  `dev_hdd0/home/00000001/savedata/<save folder>`: copy a save folder into
  the port's `PS3_GAMEDATA_ROOT/dev_hdd0/home/00000001/savedata/`
  (`tests/boot_smoke.sh` gives each run its own root: seed it from a copy,
  never from the owner's originals). Saves are personal data: keep them out
  of git.

## Work log: diagnoses and measurements, 2026-10-30 to 2026-11-02

*As written at the time, by topic. "Current status" above is the summary and
the list of next actions; diagnoses a later entry replaced are in "History:
superseded diagnoses" at the end.*

The owner's goal (2026-10-30) is a performant native PC port: the recompiled
code is the starting point, and PS3-specific machinery (SPU workloads,
SPURS/lv2 scheduling and polling, DMA, the RSX-command-to-Vulkan layer) is
replaceable once its purpose is understood. Every replacement is incremental,
backed by before/after timings and correctness checks, and keeps gameplay
speed independent of the frame rate. Steps 1-3 of the original list (Windows
tooling, the native Windows build, measuring on the 3070) are done; see
"Where things stand".

0. **Physics and frame pacing (current, 2026-11-01).** The goal (owner,
   2026-11-01): sustained, smoothly paced 60 fps through exploration and
   debris-heavy fights, the river included; 144 fps uncapped is a stretch
   goal, after which broader game testing. Judged by frame-time percentiles,
   spikes and worst frames (tests/frame_log.py on `DIAG_FRAMES`), loads and
   transitions reported apart.
   **The configuration that holds 60 in the known scenarios** (all four
   switches experimental, off by default):
   `DOTHEROES_NATIVE_CD=1 DOTHEROES_NATIVE_SOLVER=4,5,6 GCM_WRAP_LAP=1
   DOTHEROES_PRESENT_ONCE=1`. To play with it (the owner's save in place,
   sound, the frame-rate readout, F8 to mark a hitch):
   `env WINDOW=1 AUDIO=1 DUMP=0 SHOT_EVERY=0 DOTHEROES_NATIVE_CD=1
   DOTHEROES_NATIVE_SOLVER=4,5,6 GCM_WRAP_LAP=1 DOTHEROES_PRESENT_ONCE=1
   PS3_OSD_FPS=2 DOTHEROES_DEV_KEYS=1 GAMEDATA=<the save root>
   build/venv/Scripts/python.exe tests/boot_smoke.py 3600`. To measure it:
   the benchmark (tests/scripts/fight_bench.drive with
   `DOTHEROES_DEV_STATE=1 DOTHEROES_DEV_KEYS=1 DIAG_FRAMES=150:frames.log`,
   `DOTHEROES_DEV_CANT_DIE=1` or not, `VISIBLE=1 TOPMOST=1`; then
   tests/frame_log.py). Validated on screen (build/baselines/
   2026-11-01-config-validation), guest frames (flip request to flip
   request) in the measured segments:
   - river fight, can't-die, 6 runs: p99 17.2-17.4 ms, p99.9 20-22, over 18
     ms 0.2-0.6%, over 25 ms 0.03-0.05% (~1.2-1.7 a minute, mostly in the
     first seconds of a fight), worst 33.7 ms;
   - river fight, normal damage, 3 runs (the hero lived through every
     segment): p99 17.3-17.4, p99.9 19.7-22.2, over 18 ms 0.3-0.4%, over 25
     ms ~1.2 a minute, worst 33.6;
   - exploration (tests/scripts/soak_timing.drive: walking, sword, menu,
     map, guard), 2 runs of 154 s: p99 17.3, p99.9 ~18.7, worst 23.7, no
     frame over 25 ms;
   - for reference, the default configuration in the same fight: p99 29-38
     ms, ~4.9% over 18 ms, 86-115 frames over 25 ms a minute.
   The diagnostics' own cost does not show: the frame trace, physics stats
   and GPU timing on against only the frame log, p99 17.2 against 17.4 ms,
   over 18 ms 0.2 against 0.6% (within run-to-run spread).
   Present-call completion intervals vary more than the frames (p99 21-23
   ms, 3-6% over 20 ms): when the GPU finishes each frame varies. That is
   not display timing; this display runs at 143 Hz, where 60 fps frames are
   held for 2 or 3 refreshes whatever the pacing -- evenly spaced frames on
   it need the game paced to the display (72 or 144 fps), the stretch goal's
   ground.
   Remaining outliers: area entry (the first seconds after arriving in the
   river's area: a 74 ms frame of main-thread work and 50-67 ms drains,
   transition costs) and about one frame of ~30 ms a minute in fights, at
   debris bursts: the walker's drain of that frame takes 16-21 ms (draws
   jump ~680 -> ~840 and the physics runs up to 6 steps over 270 new bodies
   in it). Shader and pipeline compiles are not it (73 ms and 9 ms in a whole
   run, 13 ms after the fight began; trace kind 24). Geometry is not cached
   between frames for every draw: ~120 draws a frame (~75-100k vertices)
   are converted on the CPU every frame, not only at first sight (trace kind
   25) -- part of the walker's drain; not what limits frames uncapped (the
   main thread is, below), but it is in the slow frames' drains.
   **Slow frames, traced** (the owner's own play, ~7.7 min traced, 26,503
   frames; native collision and solver, default ring; tests/slow_frames.py):
   92% of frames under 18 ms. In the 18-25 ms frames (7.4%) the main thread
   waits 8-10 ms a frame for room in the command ring (0.3 ms in the others);
   its physics wait is now ~1 ms. The walker, which frees that room, drains
   for twice as long (8.5-9.7 against 4.3 ms; ~770 draws a frame against
   ~555) and waits 4-15 ms a frame for the previous present
   (`present.prlock`). That lock is held 162 s of the ~460 s traced: per
   present (two a frame) median 2.0 ms, p90 6.0 -- the GPU fence of the
   previous frame (median 1.2, p90 5.7), then the window present (0.35).
   **What the present lock waits for, exactly** (traced event by event, then
   counted over the session): the walker loop presented every flip twice.
   The drain stops at a FIFO flip and sets both the flip count and the
   pending-flip flag; the loop presents on the count right after the drain
   (the completed frame, 0.1 ms to record and submit its copy) and again on
   the flag at the top of its next pass -- the same surface, 0 draws. That
   second present takes `present_mu`, which the `present` thread holds while
   it finishes the first: the GPU fence for the frame, the readback, the
   window present. 97.3 s of the walker's 97.5 s of present-lock waiting in
   the session was in those repeats (3.8 ms each); the first presents waited
   0.2 s in all. What the lock guards (rsx_vulkan_backend.c, `vk_state`): the
   present's one command buffer (the copy, then reused for the window blit),
   its fence, the readback buffer, the display image the frame is blitted
   into and the window blits from, the in-flight and ordering flags, the
   window's acquire semaphore, fence and the overlay's staging buffers. The
   GPU wait must come before any of these is reused; it blocks command
   processing only when the walker starts another present before the last
   one finished, which the repeat did every frame. Presenting once per flip
   (`DOTHEROES_PRESENT_ONCE=1`, port/main.cpp: the count path takes the flag
   too) removes it: repeats 7,541 -> 72, the walker's present-lock wait 2-9
   ms a frame -> 0, half the copies, readbacks and window presents. Frames
   barely moved (the walker had slack: it still sleeps 8-10 ms of a slow
   frame): fights with can't-die over 18 ms 3.8% -> 4.0%, p99 22.6 -> 22.1;
   normal damage 4.2% -> 3.2%, p99 24.5 -> 20.9 (one run measured: the other
   had no debris). Frame start -> present returned (pipeline timing, not
   input latency) median 27.2 -> 27.8 ms, p90 30.1 -> 32.4. Kept for the
   configuration: the same frames shown with half the present work.
   What remains in slow frames once the repeat is gone: the main thread's own
   work (12.6-13.8 ms against 10.2), a ring wait of 5.5-6 ms -- the wrap
   protocol, which the capped lap (below, retested) removes safely -- and the
   walker draining 10.7-11.7 ms (against 5.7): its own CPU work on ~770
   draws. A further present redesign (two sets of present objects, so the
   fence wait never meets the walker) has nothing left to win here; the
   walker's drain cost is the next candidate if headroom is needed.
   **The state-aware fight benchmark** (tests/scripts/fight_bench.drive,
   tests/fight_bench.py; build/baselines/2026-11-01-fight-bench): the river
   fight with the location confirmed (field area 18) before measuring, from
   the first debris, ended and classified by a death, a departure or the
   drive's end mark; can't-die keeps the segment going (each top-up
   counted). Native solver (4,5,6) against the lifted one, native collision,
   default ring, three runs each way, all alive and in the area throughout:
   steps 3.47 -> 1.55 ms; guest frames over 18 ms 6.4% -> 3.3%, over 20 ms
   4.2% -> 2.2%, p99 24.8 -> 22.0 ms; present intervals over 25 ms 0.46% ->
   0.26%; at matched contact points physics frames' guest mean 17.62 / 18.51
   / 20.42 -> 17.15 / 17.03 / 16.37 ms (1-100 / 101-250 / 251+), over 18 ms
   34.6 / 41.8 / 70.6% -> 32.4 / 28.4 / 20.8%; latency unchanged (frame start
   -> present median 27.5 -> 27.6 ms). One pair with normal damage: the
   lifted run lived, the native one died 30 s into its segment -- too short
   to compare. In those runs the drive's swing rounds took ~16 s each, not
   the ~3.4 s their sleeps add up to, so runs ended before its end mark
   ("unfinished": alive and in the area to the trace's end, measured to
   there). **Found and fixed** (tests/drive.py; `DRIVE_TIMING=1` logs each
   step's time and each press's waits): every press first grabbed the
   current frame for a later `change` step, and with frame dumps off
   (`DUMP=0`, every timing run) there is no frame.ppm, so the grab retried
   for ~1 s before each press; the game then took each press in ~50 ms. The
   inputs and holds were as written (a 150 ms direction tap, sword presses of
   10 reports), only spaced ~1.4 s apart instead of ~0.4 s -- in every
   dumps-off run of fight_bench.drive and fight_river.drive so far, both
   arms of each comparison alike. A press now grabs only when the script has
   a `change` step: a round takes 4.0 s (3.4 s of sleeps and twelve ~50 ms
   admissions), the drive reaches its end mark, and the fight is now 20
   rounds (~80 s). Fights at this cadence are busier: results from before the
   fix are comparable among themselves, not with later ones.
   **Above 60** (the configuration above, on screen; build/baselines/
   2026-11-01-config-validation, vb2-* and vb6_944-*). `DOTHEROES_FPS=0` alone
   still gives 60: the game waits for each flip, and a flip completes on a
   tick of the port's vblank clock (60 Hz). With the clock at 2 ms
   (`DIAG_VBLANK_MS=2`, a test setting) the game runs as fast as the host
   lets it: ~120 fps, guest frames mean 8.1-8.4 ms, p99 12.7-13.4, in
   exploration and in the river fight alike. Paced to this 143 Hz display
   (`DIAG_VBLANK_MS=6.944`): mean 8.9-10 ms, so 144 is not sustained; it needs
   ~15-20% more headroom. **What limits it** (tests/frame_costs.py on the
   uncapped fight's trace; build/baselines/2026-11-02-frame-rate): the PPU
   main thread, the game's own code -- idle 0.5 ms of an ~8.5 ms frame, while
   the walker drains 5.6 ms and the GPU takes 2.6. Its profile (`DIAG_PROF`,
   `DIAG_PROF_THREADS="ppu main"`, 40 s of the uncapped fight) is flat: no
   game function over 3.4% of samples; 80% of the time is under an indirect
   call, and the runtime's dispatcher for those was the largest single cost,
   ~7% (its lookup's cache misses, two dozen per-call switch checks). Patch
   0071 gives it a fast path and a one-array table: uncapped exploration
   8.16 / 8.10 -> 7.66 / 7.77 ms a frame (~5%, ~123 -> ~130 fps; the same
   binary with `PS3_TOCCHECK=1` forcing the old path). 144 now needs ~10%
   more off the main thread; what is left is spread thin over the game's
   code (the lifter's code generation as a whole), so no single fix gets
   there. Whether gameplay stays right at these rates is checked separately
   (below).
   **Gameplay at other frame rates** (tests/scripts/fps_check.drive,
   tests/fps_check.py: the same inputs at 30 -- the original pacing -- 60,
   ~120 with a 2 ms vblank and ~110 with a 144 Hz one; the game's state from
   memory snapshots, the player's object every 4 ms (`DOTHEROES_DEV_PLAYER_LOG`)
   and the frame trace; build/baselines/2026-11-02-frame-rate):
   - *Timers* follow real time. Over 10 s standing still, the static-memory
     words that count (a seconds clock at 0x029A64CC with seven countdown
     copies, a millisecond counter at 0x02C69D50, a 60-a-second one at
     0x02CF3544) moved the same at every rate; seven words count frames
     (0x02CF3570, 0x02B37CEC and others moving 1, 3, 19, 85 or 374 a frame)
     and one counts vblanks (0x0159C414, read by the frame-pacing wait
     func_00029BD8 through cellGcmGetVBlankCount) -- none of them a gameplay
     timer found so far. The game's frame delta is measured time
     (sys_time_get_system_time; port/game_patches.cpp).
   - *Physics* steps at a fixed 60 a second whatever the frame rate. The same
     grass cut (12 swings) at 30 fps and at ~110-120: 358 and 359 steps, 60.2
     and 60.0 a second, the same debris (55 bodies) moving for 5.9 and 6.0 s
     (two steps a frame at 30, one every other frame or so above 60). In the
     river fight, 60.1-60.4 steps a second while debris is active at 60 fps
     and 60.2 at 116-118 fps (tests/solver_breakdown.py's step count; a step
     has a collision task per 64 pairs).
   - *Hero states and animation* follow time: the walk state lasts the 1 s
     hold at every rate; the walk animation's phase (+0x524) advances 1 per
     100 ms at every rate. A swing (+0x590: 2, then 3) ends each phase on the
     first frame after its time (0.3 s, then 0.1 s) is up, so at 30 fps each
     phase runs a frame long: 333 + 133 ms against ~300 + ~103 at 60 and
     above -- a swing of ~466 ms in the original pacing, ~400 above. Presses
     0.4 s apart therefore swing every other time at 30 fps (6 of 12), 9 of 12
     at 60, all 12 at ~120 (0.5 s apart, all 12 at every rate): quantisation
     of a time-based timer, closer to its intended length the higher the
     rate. Not changed.
   - *Movement* is time-based (9 units a second) but the game truncates each
     frame's displacement to a multiple of 0.01 (steps of 0.29/0.30 at 30 fps,
     0.14/0.15 at 60, 0.06/0.07 at ~120; the position written by the mover,
     func_000A7C78, at +0x428/+0x42C/+0x430 of the player's object, found with
     `DOTHEROES_DEV_WATCH`), so the hero is a little slower the higher the
     rate: cruising 8.82-8.87 units/s at 30, 8.70-8.75 at 60 (-1.5%),
     8.27-8.45 at ~120 and 8.40-8.42 at ~110 (-5%). Where: the step is made
     unquantised by func_00119BEC into +0x768 (x) and +0x770 (z) of the
     player's object, then func_00106CB8 passes each through func_000A2C14
     (via the cross-module stub func_00983A78, calls at 0x00106FB8 and
     0x00106FCC): `(float)(int)(v * 100 + 0.005) * 0.01` -- truncation toward
     zero, 0.005 hundredths of slack. 22 other functions in that module
     (0x00102BB4-0x0018BA08) call the same stub -- but each to snap a value
     just before converting it to an integer, not a step: only the hero's
     two calls accumulate (one chain, func_0012476C -> func_00119BEC ->
     func_00106CB8). It is enough to move a timed route: at ~120 fps the
     first drive's 0.5 s walk right ended 0.35 units short and the hero slid
     round a ledge the 30 and 60 fps runs stop at, missing the grass; the
     drive now ends its moves against walls.
     **Fixed (2026-11-02):** an entry hook (ps3recomp patch 0077: the lifter
     puts `if (guard) { host(ctx); return; }` at a listed function's start;
     config/insn_patches.json) sends func_000A2C14 to
     `dotheroes_step_round` (port/game_patches.cpp), which rounds exactly as
     before except at the hero's two calls, where it carries the dropped part
     into the next frame's step on that axis (restarting when the step is 0,
     reverses or the object changes). Steps stay multiples of 0.01, as the
     collision code has always had them. On by default;
     `DOTHEROES_STEP_CARRY=0` truncates as the game does. Measured on the same
     route (fpscarry-* runs): cruising 9.02 / 8.97 units a second at 30 fps,
     9.01 / 8.96 at 60, 8.96 / 9.06 at ~130, 9.01 / 8.99 at ~125 (144 Hz
     vblank); every run stops at the same ledge (z -173.70) and cuts the
     grass; physics 359-361 steps, 55 bodies, 6.0 s at all four. At 30 fps
     the hero is now 1.7% faster than the original's truncated 8.85 -- the
     speed its step is computed from.
   - *Enemies* move by time and never went through that rounding
     (`DOTHEROES_DEV_ACTOR_LOG=1`, tests/actor_steps.py; the river fight at
     30, 60, ~120 and 144-paced): the river's enemies (class 0x00FE4500)
     take steps that are not multiples of 0.01 (1-6%, chance), and over 0.25
     s windows move at the same discrete speeds at every rate (4.0, 6.0, 8.0,
     10.0 units a second; which ones a run shows depends on what they did).
     An object of class 0x00FE3DE8 that moves with the hero runs at 9.0 at
     every rate with the carry. Bosses and other areas' enemies are not
     measured yet.
1. **Next replacements:** the present path (frames in flight, no readback,
   no lock held across GPU waits). The display-driven vblank exists now as
   `DOTHEROES_FPS=display` (2026-11-03, below "Game coverage"'s pacing
   note); phase-locking it to the display's own vblank is the next step. vm_watch on Linux (mprotect + SIGSEGV), so
   resident geometry runs there too. Validate each with perf_walk.drive +
   `tests/frame_times.py` and pass-dump/screenshot A/B.
2. **Colours:** see "Where things stand"; compare RPCS3's scene buffer.
3. **boot_smoke.sh:** retire it once `boot_smoke.py` is tested on Linux.
4. **Run the setup app end to end** on both OSes (the GUI, `python
   setup_app.py`; `--convert` passed on Windows 2026-11-02), then `tools/make_bundle.py --prebuilt` and the frozen apps.
   Watch for: modules the lifting tools need inside the frozen app
   (`make_bundle.tool_imports()`), multiprocessing in the frozen lifter, the
   exact flag groups in `zig_build` vs the CMake build, link libraries.
5. **First GitHub release** (v0.1.0) with both downloads, like SFA3's.
6. Found by the owner playing on Linux (Xbox controller, 2026-10-30):
   - **A "swirl" / CRT-like pattern over the whole picture** that real-hardware
     screenshots of the game don't show. Not the game's intended vignette and
     tilt-shift blur (those are real). Suspects: a post-process pass sampling a
     noise/dither texture with the wrong filtering, addressing or swizzle; a
     half-resolution buffer upsampled wrongly; moiré from a mis-decoded
     texture. Compare a frame dump against reference screenshots, then bisect
     the frame's passes (`RSX_ENGINE_PASS_DUMP_TRIGGER`).
   - **Colours look washed out by default.** The opening prologue is meant to
     be washed out: a "vision of the past", `sepia_tone = 0.60` in the game's
     playerParam.lua.eu. Only muted colour *after* the prologue would be a bug.
     The game's own Display menu has a colour slider; raising it made the
     picture look right. Compare the same scene in RPCS3: does the default
     really look that dull, or do we lose
     saturation somewhere: gamma/sRGB handling in render targets or the
     present blit (swapchain format 44, `B8G8R8A8_UNORM`), or a colour-matrix
     constant.
   - **"Install Game Data onto the HDD?" -> Yes crashed. Fixed (2026-11-02,
     patch 0072).** NID `0x0D5B4A14` is `cellFsReadWithOffset` (NIDs are
     SHA-1 of the name with the fixed salt); the install copies the disc
     with it and `cellFsWriteWithOffset`, both now positional transfers in
     ppu_fs.cpp. The main menu's INSTALL (tests/scripts/install_data.drive,
     no `SAVES=`) runs to the end in ~25 s -- 1,945 files, 1.28 GB, each
     byte-identical to the disc -- and returns to the title; the next start
     on that data root runs, reading the installed copy after its data
     check. The clean shutdown after an install failure was not revisited
     (it no longer fails). Open, minor: the runtime's cellMsgDialog is
     headless and answers every yes/no dialog YES by itself
     (`MSGDIALOG_ANSWER` switches it), so a player cannot decline one. The
     only one this game has shown is the install confirmation, which comes
     after the player chose INSTALL; an on-screen dialog (the OSD and the
     pad) would be needed for any other.
   - **No window on Wayland:** the swapchain could not be created and the
     backend went headless. `SDL_VIDEODRIVER=x11` (XWayland) works; fall back
     to X11 automatically.
   - Linux laptop with GameMode: 20-30 fps in the overworld, dips to 6-12 at
     scene changes. Too slow to enjoy there.
   - **Reference: RPCS3's fix for this game's random crashes (announced
     2026-10-14).** On the PS3 the PPU can wake from an event and free a
     SPURS task's context before the SPU has finished saving it, corrupting
     the heap; RPCS3 works around it with "PPU Event Wake-Up Delay" = 35.
     Our SPURS is HLE with SPU tasks on host threads, so the same race may
     exist here (not seen yet; sessions so far were short). Fix it by
     ordering, not a delay: signal the PPU only after the task's context save
     (cellSpurs task save/exit path, libs/spurs) has completed. Long play
     sessions are the test.
7. Open issues: debug console and golden boot log (M2). **Not a bug: the
   "flat beige quad over the castle path"** in `intro.drive` (2026-11-02):
   it starts at the hero's feet and runs to the screen's edge, a flat blade
   with a lighter centre ridge, and shows only after a sword press with the
   hero facing down at full life -- the game's full-health giant sword,
   drawn in the prologue's washed-out palette (white in the attract mode's
   fights). **Save and load work** (2026-11-02,
   tests/scripts/save_game.drive: the river route, the hero's life set to
   half an apple with `life.trigger`, killed by the next hit; GAME OVER ->
   "Continue?" NO -> "Save your progress?" YES): the system save list wrote
   DATA.SAV, ICON0.PNG and PARAM.SFO into the loaded slot (3 bytes of
   DATA.SAV changed: a 16-bit counter at 0x4C, play time likely, and a flag
   at 0x835), and the next start's CONTINUE loaded it and played. "Continue?"
   YES respawns at the castle without saving. The headless save list writes
   into the slot that was loaded; PARAM.PFD is not rewritten (a save copied
   back to RPCS3 or a PS3 may need that). The game's attract mode (5 min, no
   input: field, river, desert, a lava dungeon, the red volcano area, the
   forest) ran at 60 fps with nothing over 25 ms and nothing visibly wrong.
   **Menus and features** (2026-11-02): START in play opens the system menu
   -- RESUME, SAVE, LOAD, OPTIONS, SNAPSHOT, ALBUM, BESTIARY, LOADING ART,
   TITLE (the title screen's menu has NEW GAME, CONTINUE, EDITOR, ALBUM,
   INSTALL); TRIANGLE the item pages. The dot editor opens (EDITOR -> NEW
   DATA -> gender -> class -> its frames; LOAD with no edit data saved
   returns, the save list being empty; tests/scripts/menu_tour.drive).
   **SNAPSHOT and ALBUM fixed** (patches 0073-0075; tests/scripts/
   snapshot.drive, menu_album.drive): ALBUM called cellPhotoImport2, which
   the runtime did not have -- in normal play the screen went black for
   good, waiting for its callback; SNAPSHOT then needed cellJpgEnc's real
   API and cellPhotoExport. Now the camera's shutter encodes the frame
   (a display buffer, written back from the GPU first: it was black before)
   to a JPEG with the game's copyright line, exports it to
   `<data root>/dev_hdd0/photo`, and ALBUM imports the newest picture there
   and shows it; with none it backs out to the title. The system menu's
   SAVE saves at once (the headless save list; the menu stays open), OPTIONS
   opens SOUND / SCREEN / TEXT, BESTIARY its card grid ("booked monsters 0"
   on this save) and LOADING ART its gallery (0 of 105), each left with
   CIRCLE (tests/scripts/system_menu.drive) -- no unresolved import on the
   way. **The dot editor's SAVE fixed** (patch 0076; tests/scripts/
   editor_save.drive): it encodes the save's icon with cellPngEnc, which
   was a placeholder, and stopped there. Now the icon (320x176, the EDIT
   card with the logo, from a GPU surface written back) and DATA.EDT are
   saved as edit data (`BLJM60180T...`, the game's edit-data prefix).
   **Round trips across a restart** (2026-11-02; one persistent data root,
   `GAMEDATA=`, a copy of the owner's saves; each step a separate game
   process): INSTALL, then the next session plays from the installed copy
   (150 reads from `BLUS30490DATA` after the game's data check, the rest
   from the disc before it), crosses into the river's area and takes a
   SNAPSHOT, and the session after that shows that same river picture in
   ALBUM (tests/scripts/roundtrip_1-3.drive). In the dot editor, a new
   character with six blocks placed on its Stand pose, saved, comes back
   after a restart through EDITOR -> LOAD with the same six blocks in the
   pose's thumbnail and its grid, and the save's own thumbnail (ICON0.PNG)
   shows them on the EDIT card (tests/scripts/editor_roundtrip_1-2.drive).
   **Found on the way and fixed (patch 0078):** a data root whose path is
   longer than ~110 characters made the game exit at boot -- cellGame held
   host paths in the guest's 128-byte buffers and cut /game off.
   **Not verified:** save portability (a port save in RPCS3 or on a PS3:
   PARAM.PFD is not rewritten) and the colour comparison with RPCS3. An
   automated RPCS3 capture (tests/rpcs3_capture.py) did not reach the game
   in 10 minutes -- RPCS3 was still preloading its cached pipelines (48 of
   126), showing only its splash; let RPCS3 finish that once interactively,
   then captures can compare the title and the field for the washed-out
   colours and the swirl (item 6). Shader preloading is RPCS3's and stays
   out of any performance measurement here.
   Game coverage: see "Current status", "Game coverage".
8. Found by the owner playing on Windows (2026-10-30), the same with
   RSX_GEOM_CACHE=0: killing enemies left a bright flash that stayed a long
   time; enemies flashing after a hit were invisible; the water "not
   rendering right". **Cause:** the title uploads its fragment programs into
   local memory with inline transfers (NV308A COLOR) as it draws, and the
   walker took COLOR methods only up to 0x07FC -- 256 words. Its 1,056-byte
   water program lost its last instruction, the engine found no end to it
   and drew with the built-in program (flat blue), or with a stale tail
   left by an earlier upload (the white flicker); ~19,000 such draws in 35
   s by the moat (RSX_FALLBACK_LOG=1 names them). COLOR now runs to 0x1FFC
   (0x700 words, patch 0047): no fallback draw since, the water draws its reflection
   every frame, a kill's flash shows and is gone within 0.2 s, and hit
   rabbits flash pink and stay visible (route: right from the castle past
   the raised block, then up over the bridge by the sign). To confirm in
   the owner's own play. Also added on the way, not shown to matter here:
   the Vulkan backend's CONSTANT_COLOR/ALPHA blend factors and
   SET_BLEND_COLOR (RSX_BLEND_LOG=1 lists such draws; this title drew none).
   **Second cause, the invisible flashing:** one area further (Field_11,
   the forest left of the field above the castle), the hit hero and enemies
   still vanished while invulnerable; RPCS3 draws them blinking pink and
   lavender. The walker fed every subchannel's methods to the draw engine,
   which strips the subchannel, so a 2D-engine word landed on the NV4097
   register with the same low bits: the inline transfers' point/size words
   (0x0300-0x0308) overwrote alpha test, and a long fragment program's
   COLOR words reached SET_SCISSOR (0x08C0/0x08C4). The flash program's
   draw came right after its upload, scissored to rows 7324-7340. Found by
   painting that program solid magenta (`RSX_FP_DEBUG=<hash>:0:m`: no
   pixel), then a register dump of the draw (`RSX_ENGINE_REGDUMP_FP`). The
   engines now get only what the walker routes to the 3D engine, plus the
   driver methods on subchannel 7 (patch 0048): the hit hero blinks pink/lavender like
   RPCS3 and a kill's flare is gone within a second.
   Still open: fighting there drops to 18-33 fps -- dead rabbits throw far
   more debris than a tuft, ~3,000 Bullet tasks a second (item 9).
9. **Fights and physics, 2026-10-31 to 11-01** (the owner's session: fights
   and the wide river at 7-13 fps). Fixed step by step -- the SIMD level of
   the lifted SPU code, the MFC lookup cache, native collision, render-target
   thrash, the ring wait and the capped lap, task creation, the native solver
   commands -- to the validated configuration in item 0. The measurements and
   diagnoses on the way, several superseded by later ones, are in "History:
   superseded diagnoses" at the end; native collision's open contact
   differences are tracked in docs/BULLET_NATIVE.md.
10. **"Saving doesn't bring up the save menu"** (owner, 2026-10-31). It
    saved: the save-data module skips the PS3's system save list and takes
    the slot the title offers, so nothing appeared. A save now shows GAME
    SAVED (or SAVE FAILED) in the window's corner for a few seconds
    (`include/ps3emu/osd.h`, drawn after the window blit; not in frame
    dumps). The owner's active save lives in `%LOCALAPPDATA%\dotheroes\gamedata`
    (a copy, verified, of the session's): play sessions use it in place with
    `GAMEDATA=` (tests/boot_smoke.py); tests seed copies from it with `SAVES=`.

## History: superseded diagnoses (kept for the record)

*Moved here from the work log on 2026-11-02. These were right about what they
measured at the time; later entries changed the picture (the validated
configuration, the native solver, the present fix). Do not quote their numbers
as the current state.*

**Fights and the wide river at 7-13 fps** (owner's session, 2026-10-31).
   Bullet physics: its SPU tasks are `PS3_SPURS_BulletCD` (image 4, Bullet's
   SpuNarrowPhaseCollisionTask) and `PS3_SPURS_BulletSolver` (image 5, the
   libsync parallel solver). Two collision runs a step, then ~24 solver
   phases of 3 tasks, each phase sequenced by the PPU. A slow frame makes
   Bullet's fixed step catch up with 2-6 steps the next frame (4-12
   collision runs, ~270 solver tasks): the spiral. Each step was slow for
   two reasons, both emulation overhead, both fixed:
   - The build targeted baseline x86-64 (SSE2), so lifted SPU code got the
     byte-loop `shufb` and one-lane-at-a-time float helpers. On 301
     identical collision runs (tests/scripts/grass_cut.drive): 3.77 ms each,
     1.12 ms at x86-64-v2, 0.80 ms at v3. `zig_build.host_cpu()` picks v3
     when the machine has AVX2/FMA/BMI2, else v2 (`DOTHEROES_CPU`
     overrides); the shipped prebuilt runtime and dependencies stay at v2
     (`PORTABLE_CPU`). The helpers stay exact at v3: test_spu_float_exact
     446,840,832 lanes bit-identical, test_spu_shufb passes.
   - Every SPU DMA command took the MFC registry's shared lock and walked
     every live context, while each of ~4,000 tasks a second added and
     removed its own under the exclusive lock: half the solver's time. The
     per-thread lookup cache was there but off; now on
     (`SPU_MFC_LOOKUP_CACHE=0` turns it off): 118 -> 59 us a solver task.
   Same save and script, 10-s windows 60-120 s, before -> after: the river
   (tests/scripts/fight_river.drive) 50 50 15 25 22 53 -> 56 50 53 60 fps,
   frames over 50 ms 257 -> 0; the rabbit field (fight_field.drive)
   40 13 51 -> 58 58 54 60, 170 -> 0. A step now takes ~2.8 ms (both
   collision runs in parallel 0.8 ms, the solver ~2 ms). `PS3_FTRACE` has
   each task run (kind 16) and tests/phys_frames.py tabulates them per frame.
   What is left in a slow fight frame, measured per frame with the frame
   trace (tests/frame_costs.py: the walker's drain passes, each batch's GPU
   time with VK_GPU_TIME=1, every wait, the main thread's sleep; the river
   fight, no frame dumps):
   | per frame | normal | 25-34 ms | 34-50 ms |
   |---|---|---|---|
   | draws / render passes | 640 / 52 | 935 / 55 | 951 / 55 |
   | walker (its waits included) | 5.4 | 15.2 | 22.6 ms |
   | waiting for the previous batch | 0.0 | 4.8 | 6.4 ms |
   | GPU execution | 3.6 | 7.4 | 8.6 ms |
   | main thread asleep | 8.7 | 2.5 | 1.7 ms |
   | Bullet tasks running (wall) | 0.1 | 10.0 | 11.9 ms |
   The PPU is the limit: busy nearly the whole frame, most of the extra
   being its wait for the Bullet tasks (DIAG_PROF: 19% of the main thread
   in a fight polls for them, 4% is Bullet's own PPU code). The GPU has
   room (8.6 of 16.7 ms); the walker comes next (10-15 us a draw in those
   frames against 8). Debris does not multiply draws or render passes: an
   earlier reading of 2,300 draws and 140 passes a frame counted the GPU
   batches that finished in a slow frame, two or three frames' worth.
   GPU compute for Bullet is out: a CPU->GPU->CPU round trip on this queue
   is 87 us median, 0.8 ms p90, and a step needs at least two, with ~24
   PPU-sequenced solver phases on top. Native CPU Bullet in place of the two
   tasks is the next step; what they read and write is mapped in
   docs/BULLET_NATIVE.md.
   A native collision task exists, experimental and off by default
   (`DOTHEROES_NATIVE_CD=1`, port/native/; the lifted task stays the
   default; not bit-exact with the SPU). A/B, three runs each: collision 1.25 -> 0.07 ms a step (field), 2.74 -> 0.13 (river); a
   step's span 3.60 -> 2.78, 6.02 -> 3.76; the main thread's wait 2.81 ->
   1.95, 5.21 -> 2.86. Frames barely moved (river frames with a step 25.6 ->
   24.2 ms mean, p95 35.6 -> 34.2, frames over 25 ms 123 -> 121): in the
   slow ones the walker (~19 ms for ~935 draws) is now the limit. Details,
   and how close its contacts are to the SPU's, in docs/BULLET_NATIVE.md.
   **The walker, split** (tests/walker_breakdown.py; kind-18 walker spans
   and the GPU waits inside drain passes). Per guest frame: lag = flip
   request -> walker reaching that flip; then the present. The guest's frame
   end (func_00029BD8) waits for the vblank (usleep at lr 0x29C20) and for the
   previous flip to complete (lr 0x29CC0): that second wait is how walker lag
   reaches the frame rate. "19 ms" was the drain passes with their waits.
   Native collision, windowed river, 25-34 ms frames: lag 13.8 ms = 9.8 active
   drain + 2.6 waiting for the GPU batch in flight + 0.4 flush + 1.0 sleep; the
   guest then waits 5.2 ms at 0x29CC0 (with lifted collision 1.3: physics holds
   it first). The walker sleeps 13-23 ms of such a frame: its work comes after
   the guest's. Each frame is presented twice (the FIFO's flip, then the walker
   loop's present of the same buffer): 2-4 ms a frame of the walker waiting on
   the present lock, outside the lag. Active drain, profiled: the FIFO decode
   loop's own per-word work 30% (every word took the per-word diagnostic path
   because the draw engine counted as a diagnostic), draw recording ~57%
   (NVIDIA driver ~22%). The run fast path for the engine (patch 0059;
   GCM_WORD_LOOP=1 restores the loop) cut the loop's share 10.8% -> 4.2% of the
   walker and active drain ~0.7-1.0 ms a frame, but the slow frames' lag only
   13.7 -> 13.2 ms: the walker reaches its flushes sooner and waits longer on
   the GPU. Frame-to-present intervals unchanged within noise (A/B in
   build/baselines/2026-10-31-walker-wordloop; whether the desktop was locked
   was not recorded then, so not established as on-screen timing). Checked
   in play only then; the FIFO
   unit tests (libs/video/tests/test_gcm_*) were not built on Windows. Since
   2026-11-01 tests/native/run_gcm_tests.sh builds nine of them with the
   bundled Zig and they pass (it works around two things in build/: the tests
   allocate guest memory to the inject base + 64 KB, but cellGcmInit now
   clears the report area 8 MB past it; and the COFF linker wants two stubs).
   flip_status and user_command (stubs they lack) and legacy_state_filter
   (POSIX setenv) still do not build there.
   **Render-target thrash.** The in-flight waits came from address 0
   alternating 1280x720 <-> 1024x1024 in fights (RSX_SURFACE_LOG=1): each
   switch recreated the target with two GPU syncs, 4.3 + 0.9 ms of a slow
   frame's lag. RSX_SURFACE_TRACE=0:0 showed what the two are: address 0 is
   display buffer 0 (1280x720, colour target selector 1), cleared whole, then
   the 1024x1024 work, then 36 draws with no clear, then presented; the
   1024x1024 work is the shadow pass with colour target selector NONE (and
   the colour mask on), which writes no colour on the RSX. Nothing reads
   address 0 but the present. So the shadow pass has no contents of its own
   and must not touch address 0, while the display buffer's contents must
   survive it -- recreating it threw them away. Fix: a pass with target NONE
   renders into the scratch target of its size, as a colour-masked one
   already did (PS3RECOMP_ENG_NONE_TARGET=1 restores binding the address).
   River fight: 43 colour targets made in the whole run, all new addresses,
   none recreated (was 1,098 recreations). Visual check from the renderer's
   readback, both collision modes: shadows, debris and the HUD as before
   (build/baselines/2026-10-31-rtfix-visual). A/B, same build, windowed,
   DUMP=0, 3 runs per scene, collision mode and binding (24 runs,
   build/baselines/2026-10-31-rtfix-ab): time in those GPU syncs 600-4,200
   ms a run -> 5-42; the slow frames' walker lag 1.4-3.4 ms less. The
   desktop was locked when the A/B began (window grabs showed the lock
   screen, LogonUI was running) and, as far as is known, stayed locked; the
   per-run lock flag in run.json was wrong then (it read the session's
   connection state; fixed), so that is not verified run by run. What follows
   is the game's internal frame and presentation timing (guest flip requests
   to the end of each frame's present call), not displayed-frame performance.
   Fights differed run to run (0 to 1,728 physics steps), so long frames are
   compared within frames of the same physics load. Frames with one physics
   step taking over 25 ms to present: river native 36% -> 12%, field lifted
   19% -> 1.5%, field native 9% -> 0.4%, river lifted unchanged (~34%: lifted
   physics holds those). Frames with two steps: river lifted over 33 ms
   43% -> 19%; the others mixed on small samples (18-157 frames). Frames
   without physics: unchanged (0.1-0.4% over 25 ms).
   **On screen** (desktop unlocked, VISIBLE=1 guard, DUMP=0, 34 runs,
   build/baselines/2026-10-31-onscreen): pacing as present-call completion
   intervals -- each frame's first window present call returning (FIFO
   swapchain, 144 Hz display). Not display timing: the distance to scan-out
   is not measured (PresentMon, which would show it, needs elevation), so
   none is claimed. The game rendered visibly
   and correctly in every grab (debris, shadows, HUD, water), both collision
   modes. 10 runs had no intended fight -- fight_field.drive leaves the hero
   held at the cave screen about half the time -- and were left out
   (classified from the grabs, CLASSIFICATION.tsv). Intended fights, collision
   workload counted (DOTHEROES_PHYS_STATS=1): river lifted 6 runs / 2,359
   physics steps / 168k pairs, native 5 / 2,207 / 162k: present intervals over 25 ms
   3.45% -> 1.45%, over 33 ms 1.39% -> 0.21%, p99 35.4 -> 26.5 ms. By pairs
   processed in the frame: 41-120 pairs, over 25 ms 29.7% (lifted) / 5.9%
   (native); 121+ pairs 84% / 41% (median 32.0 / 23.5 ms). Field (rabbit
   field): lifted 4 runs over 25 ms 0.46%, native 2 runs (one with a death in
   the window) 0.27%; heavy frames (121+ pairs) 17.5% / 9.1% over 25 ms on 80 /
   33 frames. **The 60 fps target is not met in debris-heavy river frames in
   either mode**; the field is close in both. Every river run has one ~380 ms
   stall at the screen load into the river (72-74 s).
   Frame dumps (PS3RECOMP_VK_DUMP, on in tests/boot_smoke.py unless DUMP=0)
   are written on the present path: every 60th frame took 10-20 ms longer.
   Time with DUMP=0; for play sessions grab the window instead.
   Checked: five-minute soak 59.4-59.8 fps, p99 21-22 ms, longest stall
   364 ms (was 1,288); two check-mode runs clean.
   On the way, `DIAG_PROF` itself crashed the game in 3 of 9 profiled
   fights: its Windows sampler's RtlVirtualUnwind read a garbage frame
   pointer of a suspended thread. The walk now stops where Rsp or the
   function's frame register leaves the thread's stack (port/diag/diag.cpp);
   6 of 6 since. Catching the fault and jumping out of ntdll instead froze
   the game.
   **The solver, profiled** (docs/BULLET_NATIVE.md, "The solver: where a
   step's time goes"; build/baselines/2026-10-31-solver-profile): a step's
   solver is ~71 tasks in 5 commands the PPU sequences as Bullet 2.74 does;
   the main thread waits 2.51 ms a step (iterate 1.24, write-back 0.66,
   manifold setup 0.39) and spends ~0.85 ms issuing. Removing the whole wait
   is the upper bound and still leaves debris-heavy frames over 16.67 ms: a
   command-buffer wrap wait and extra game work stay. A native solver task
   was the candidate there; since done for three commands (below:
   write-back, copy-back, iterate; experimental, off by default).
   **Task creation, cleaned up** (ps3recomp patch 0064): `cellSpursCreateTask`
   printed a line for every task and looked up an environment switch on every
   call, and the async task run four more; now the first 48 lines (all with
   SPU_TASK_LOG=1) and the switches read once. River fight on screen, three
   runs each way (build/baselines/2026-10-31-createtask-ab): a round's three
   solver tasks created in 10.2 us instead of 20.2; a physics step 0.4-0.56 ms
   shorter at matched collision workload (the arms' fights differed, ~86 vs
   ~64 pairs a step, so only binned comparisons count). Frames: those with
   41-120 pairs 17.19 -> 16.98 ms; the heaviest too few to compare.
   **The command-buffer wrap wait** (ps3recomp patch 0065, frame-trace kind
   21, tests/ring_wait.py; six on-screen river fights, both collision modes,
   build/baselines/2026-10-31-ring-wait). Every wait seen is the buffer-full
   wrap: the title reaches the end of its 2 MB ring mid-frame and
   `cellGcm_fifo_recycle` waits, polling with Sleep(1), until the walker has
   read everything up to the jump back -- 1,614 waits, median 6.4 ms, p90
   9.7. Split by the walker noting when it made the room: the walker's part
   88% (while it made the room: drawing 57%, blocked on the previous present
   26%, its own 1 ms sleep between passes 16%), the waiter's own 12% (median
   0.8 ms: the rest of a sleep and waking). The flip-time wrap, which hands
   the title a lap ending at the walker instead of waiting, ran 1,570 times
   in the two runs that counted it and never ended in a wait. At every one
   of their 427 buffer-full wraps the walker was 1,469 KB or more past the
   ring's begin (median 1,620): a lap ending there would have started with
   more room than a frame. Frames with a ring wait: 6.5% without physics, 29%
   with one step. The earlier "4.1 ms in the heaviest frames" picked frames
   by main-thread busy time, which counts this wait, so it was biased; by
   guest frame length (all six runs) the wait is ~4.3-5.2 ms in frames of
   17.5-33 ms. Upper bounds over the 839 guest frames of 18 ms or more: with
   no physics wait 358 would be under 16.67 ms, with no ring wait 450, with
   neither 761; without only the waiter's own part, 6.
   **The capped lap** (ps3recomp patch 0067, `GCM_WRAP_LAP=1`, experimental,
   off by default; test aids patch 0066). At the buffer-full wrap the title
   gets a lap ending at the walker, as at a flip-time wrap, instead of waiting
   for the walker to reach the jump back -- only when the ring as it is makes
   that safe (the runtime's ring, no lap limit in force, the walker in this
   lap by the back-jump count and behind the title, room up to the walker for
   the reserve that called and at least 128 KB); otherwise the wait as before.
   Checked: libs/video/tests/test_gcm_wrap_lap.c; `GCM_RING_CHECK=1` (the
   unread tail copied at every wrap that leaves it unread, each command
   compared before the walker decodes it) with the walker slowed
   (`GCM_WALKER_DELAY_NS=250`, ~30 fps), a 1 MB ring (`GCM_RING_TEST_BYTES`,
   3,800 wraps a run) and both: no command overwritten unread in 30-145
   million compared a run, no flip missed or out of order, no stall; frame
   dumps and window grabs look right (build/baselines/2026-11-01-wrap-lap-
   stress). On screen, frame dumps off (build/baselines/2026-11-01-wrap-lap-
   ab: river fights 7 lifted and 3 native pairs, a 5-minute soak each way):
   the ring wait fell 85-94% a frame (catch-up waits: none). Whether frames
   got faster: soak (no physics) -- no: present intervals unchanged (p99 22.4
   both), the main thread's usleep grew by what the ring wait lost: the wait
   moved. Native collision -- physics frames' tails better at the same step
   cost (41-120 pairs: over 25 ms 3.8% -> 2.2%; 121+: 17.8% -> 5.0%; few
   frames). Lifted collision, the default -- mixed: over 25 ms 1.87% ->
   2.16%, over 33 ms 0.69% -> 0.45%, guest p99 29.7 -> 27.9; at matched
   contact points steps took ~1-1.4 ms longer at light-medium load and less at
   heavy load, not explained by the walker overlapping the tasks (it did so
   less); the capped-lap fights ran more catch-up steps, and two of its seven
   runs had no fight. Queued work: flip request -> walker at the flip median
   +0.05 ms (soak), +0.46 (lifted), -0.39 (native); frame start -> present
   +0.13 / +0.55 / -0.57. Not shown to be a win where it matters (lifted, the
   default): kept selectable and off. Wrap waits reduced; no default-mode
   frame-time gain demonstrated.
   **Retested with the faster solver** (2026-11-01, the state-aware fight
   benchmark, tests/fight_bench.py; build/baselines/2026-11-01-fight-bench:
   native collision and solver, can't-die, three runs each way, every segment
   alive and in the river's area): a frame-time gain, in every run. Guest
   frames over 18 ms 2.4% -> 0.5% (per run 2.3-2.7% -> 0.3-0.6%), over 20 ms
   2.1% -> 0.3%, p99 23.4 -> 17.4 ms (17.4 in all three); present-call
   completion intervals over 25 ms 0.38% -> 0.10%; the ring wait 0.16 -> 0.02
   ms a frame. Physics frames at matched contact points over 18 ms: 32-45% ->
   4-7%. The time did not move into queued work: frame start -> present
   returned median 28.2 ms both ways (p90 30.7 -> 30.8), the walker's lag at
   the flip 7.7 -> 7.8 ms median. Still experimental and off by default until
   decided; its ring-safety checks (above) stand, and a run with normal damage
   and one with lifted collision are still to do.
   **Solver, one command native** (docs/BULLET_NATIVE.md, "Solver structures,
   checked" and "Candidate"). The structures write-back uses were checked
   against captured steps, not taken from Bullet 2.74 -- the game's contact
   constraint keeps its manifold-point pointer at +0x78, not 2.74's +0x70. The
   write-back runs natively with `DOTHEROES_NATIVE_SOLVER=6` (experimental,
   off), each task keeping the lifted task's protocol (its command, the round's
   ticket lock and cell masks, its own completion); its output matched the
   lifted task's (offline 146 of 146 contacts, in the game 21,647, none
   different) and it wrote every contact in 600 steps. The phase fell from 1.04
   to 0.21 ms a step; frames showed no gain in five river pairs with lifted
   collision, whose cost differed between the arms by as much (0.45-1.2 ms a
   step: the write-back cannot change collision already done in its step, but
   later steps and the schedule are not excluded).
   All ten write-back passes still run: shown redundant, not relied on.
   With native collision in both arms (on screen, default ring, all ten
   rounds): steps 0.33-1.11 ms shorter at matched contact points, physics
   frames at matched load faster and less often over 25 ms; pooled frames not
   comparable (the arms' fights differed in load). Native collision does not
   make the workload vary less (contact points CV 0.46 either way), only the
   cost and catch-up. On identical inputs the native write-back's work takes
   0.5-1.8 us a step against the lifted phase's 504-784 us; its in-game phase,
   ~0.21 ms, is the PPU's issuing of 30 tasks. Gameplay benefit unresolved.
   **Copy-back native** (`DOTHEROES_NATIVE_SOLVER=5`, experimental, off): its
   layouts checked on four captured steps (the SPU rounds the velocity sums
   toward zero, and so must native code); the same as the game's byte for byte
   offline and in the game (32,005 + 21,768 bodies, none different). It cost
   only 0.053 ms a step; the phase is now 0.01, a change too small to show in
   frames (gameplay effect not resolved). Both native options stay
   experimental; native collision's contact differences are unresolved and
   tracked apart ("How close it is").
   **Iterate native** (`DOTHEROES_NATIVE_SOLVER=4`, experimental, off; body
   setup was not needed first): each row as the SPU task computes it, in its
   order of operations, with the runtime's exact SPU float helpers; the ten
   rounds, the cell claims and each task's completion as the game's. The same
   as the game's bit for bit: offline on five captured steps (every round of
   the two with a body in two cells' lists), and in the game against the
   lifted rounds (16,000 rounds; the lifted tasks taking turns with
   `DOTHEROES_SOLVER_SERIAL=1` for a fixed cell order) and as itself (6,000
   rounds). Compute only, identical inputs: 64 against 252 us a round. On
   screen, only iterate switched: its phase 0.90-3.24 -> 0.24-0.67 ms a step
   at matched contact points, steps 0.6-2.8 ms shorter -- a demonstrated
   physics-step speedup; frames did not show it (the native arm's fights were
   heavier, and it waited longer on the command ring): the gameplay benefit
   is unresolved. With all three native, the
   command-ring wait in a one-step physics frame (2.2 ms a frame) is about
   twice the main thread's physics wait (1.0 ms a step).

## Diagnostics and switches (environment variables)

| Variable | What |
|---|---|
| `PS3_STRICT=1` | strict mode: silent fallbacks become fatal (dev default) |
| `DIAG_FPS=1` | guest flips per second every 10 s |
| `DIAG_PROF=<start>,<secs>` | all-thread wall-clock sampler -> `profile.raw`; `tests/prof_report.py`. With `PS3_FTRACE` also `profile.times`, each sample's time on the trace's clock (`tests/prof_report.py --at T0-T1` keeps one frame's samples); the thread list is refreshed every ~20 ms, so a task on a new SPU pool worker is seen |
| `DIAG_PROF_THREADS=<name>[,<name>]` | sample only threads whose name contains one of these (Windows), e.g. `spu job,ppu main`: all ~50 threads fill the 400,000 samples in ~10 s |
| `DIAG_DUMP_AT`, `DIAG_CRASH_DIR`, `DIAG_HANG_EXIT` | crash/hang reports |
| `VK_WAIT_STATS=1`, `VK_GPU_TIME=1` | CPU waits on the GPU by call site; GPU time per 5 s and why render passes restarted; with `PS3_FTRACE`, per-batch GPU spans and per-frame draws (tests/frame_costs.py) |
| `VK_DRAW_BATCH` (default on), `VK_DRAW_BATCH_MAX` | draw batching (4096 per submission) |
| `RSX_VERTEX_CACHE=0`, `RSX_VERTEX_CACHE_STATS=1` | the cross-frame vertex cache |
| `RSX_GEOM_CACHE=0`, `RSX_GEOM_CACHE_STATS=1` | resident geometry off; its counters every 5 s, with the arena and a census of device objects (textures and targets with their bytes, programs, pipelines, framebuffers, samplers; patch 0088) |
| `RSX_GEOM_CACHE_CHECK=1` | rebuild every resident hit and compare (slow; reports missed writes) |
| `RSX_GEOM_CACHE_LOG=<n>` | the first n new keys, for `tests/geom_newkeys.py` |
| `RSX_GEOM_CACHE_MAX_ENTRIES`, `RSX_GEOM_ARENA_CHUNKS` | cap the table / the 64 MB GPU chunks (eviction tests) |
| `GCM_FLIP_RING=1` | flip-event and walker-path recorders, dumped when a flip stays WAITING |
| `SPU_TASKPROF=<secs>`, `SPU_TASK_LOG=1`, `SPU_POOL=0` | async SPU task timing per image; every task's log lines, CreateTask's among them (by default the first 48); a thread per task again |
| `PAD_DEBUG=1` | every controller button/stick change as SDL reports it |
| `RSX_BLEND_LOG=1` | draws using the constant blend colour, with the colour |
| `PS3_VERBOSE=0` | quiet chatty logs even when stderr is redirected (timing runs) |
| `RSX_FALLBACK_LOG=1` | draws that got the built-in programs, and why (missing vertex or fragment program) |
| `SPU_MFC_LOOKUP_CACHE=0` | the per-thread MFC lookup cache off (on by default), for A/B |
| `DOTHEROES_CPU=<zig cpu>` | build-time: the CPU level this machine compiles for (default its own: x86_64_v3 or x86_64_v2) |
| `SPURS_SNAP=<entry>:<entry>:<n>:<prefix>` | guest memory before and after a Bullet step, for docs/BULLET_NATIVE.md (`tests/snap_read.py`) |
| `GCM_WORD_LOOP=1` | every FIFO data word through the per-word decode loop, as before the run fast path (A/B) |
| `GCM_WRAP_LAP=1` | experimental: the buffer-full ring wrap hands the title a lap ending at the walker when that is safe, instead of waiting for the walker to read the whole ring (default 0: wait) |
| `GCM_RING_CHECK=1`, `GCM_WALKER_DELAY_NS=<ns>`, `GCM_RING_TEST_BYTES=<n>` | ring tests: report commands overwritten before the walker read them; slow the walker per command; give the title a smaller ring |
| `RSX_SURFACE_LOG=1` | every colour render target the draw engine creates, and why (new address, resized, evicted) |
| `RSX_SURFACE_TRACE=<location>:<hex offset>` | what the title does with the colour target at that address, per frame: each stretch at one size (how it starts, its clears and draws, the surface registers) and every read of the address |
| `PS3RECOMP_ENG_NONE_TARGET=1` | a pass with colour target NONE binds the guest address again, as before the render-target fix (A/B) |
| `DOTHEROES_PHYS_STATS=1` | each Bullet collision task's workload (pairs processed, awake bodies, contact points) as frame-trace kind 19 and [phys-stats] lines, either collision mode; each solver task's command as kind 20 (`tests/solver_breakdown.py`) |
| `DOTHEROES_SOLVER_SNAP=<step>[,<step>]:<prefix>` | game memory as each solver command of the chosen steps starts, the tasks' descriptors, the lock and cell masks as tasks run (tests/solver_layout.py, tests/solver_iterate_layout.py, tests/native/bullet_solver_snapcheck.cpp); ~1.4 GB a step, game memory: local only |
| `DOTHEROES_SOLVER_SNAP_ROUNDS=1` | with `DOTHEROES_SOLVER_SNAP`: every iterate round's start too (c4r1..c4r10; ~2.9 GB a step) |
| `DOTHEROES_SOLVER_SERIAL=1` | for captures and checks, not timing: the lifted iterate tasks of a round run one at a time, so the cells are taken in index order (the order a native task alone takes) |
| `DOTHEROES_NATIVE_SOLVER=<cmd>[,<cmd>]` | experimental, off: Bullet solver commands done natively -- 6 the warm-start write-back, 5 the copy-back, 4 iterate (docs/BULLET_NATIVE.md); separate from `DOTHEROES_NATIVE_CD` |
| `DOTHEROES_NATIVE_SOLVER_CHECK=1` | each step's write-back compared, as its last task finishes, with what it must leave; the copy-back, as the write-back starts; each iterate round against the native iterate run on its inputs as it starts (then undone) |
| `DOTHEROES_SOLVER_BENCH=<file>` | with the lifted commands: each step's native write-back, each copy-back task's and each iterate round's, timed on its own inputs (compute only), then undone (tests/solver_bench.py) |
| `DOTHEROES_PRESENT_ONCE=1` | experimental, off: each guest flip presented once (the walker loop presented every FIFO flip twice -- the second the same surface, waiting on the present lock while the first finished) |
| `DIAG_FRAMES=<file>` | every guest flip request and window present, on the trace's clock, written at exit -- a frame-time record cheap enough to run with every other diagnostic off (tests/frame_log.py: percentiles, spikes, worst frames between the drive's marks) |
| `DRIVE_TIMING=1` | tests/drive.py logs each step's time and each press's waits |
| `PS3_OSD_FPS=1\|2` | testing: frame-rate readout, window's top-right corner: fps, mean and longest frame of the last second; `2` adds a 120-frame frame-time graph (ps3recomp patch 0069) |
| `DOTHEROES_DEV_KEYS=1` | testing, while the window has focus: F8 or L3+R3 marks a hitch ("MARK n", trace kind 22, `[mark]` log line); F7 cycles the readout; a drive script marks with `touch mark.trigger` |
| `DOTHEROES_DEV_STATE=1` | testing: the game's state every 100 ms -- life (half apples), field area, camera target x/z, physics collision tasks, can't-die top-ups, the hero's position (hx hy hz), the system menu's two words (menu: 3 in play, 1 in the menu; scene: 11 / 3), the guest frame counter -- in `<run>/state.txt` (`tests/drive.py` `state`, `expect`, `goto`) and the trace (kind 23); `actors.trigger` counts the characters within 40 units into `actors.txt` (`n`, `c<class>`, `hp<class>`) |
| `DOTHEROES_DEV_STOP=1` | tests/boot_smoke.py sets it: `stop.trigger` in the run makes the game write its frame log and trace, then exit -- how a run ends when its drive ends |
| `DOTHEROES_DEV_RECORD=1` | testing: `record.log` -- controller/keyboard input (PS3 mask and stick on change), field area changes, life, the hero's position every second, hitch marks, on the trace's clock: for deriving routes from a play session |
| `PS3_FTRACE_ROLL=<dir>` | hitch captures: a rolling frame trace (`PS3_FTRACE_CAP` spans, default 1M); a frame of `PS3_FTRACE_ROLL_MS` (25) to 100 ms writes `<dir>/hitch_<n>_<s>s_<why>.txt` from `PS3_FTRACE_ROLL_BEFORE` (2) s before to `_AFTER` (1) s after, on its own thread; one per `_GAP` (15) s, `_MAX` (20) in all, none in the first `_FROM` (0) s, load bursts skipped; a tester's mark (F8, L3+R3) from its own budget, `_MARKS` (100), 2 s more before it, `hitch_m<n>_..._mark<n>.txt` (patch 0085); tests/slow_frames.py reads them |
| `RSX_GEOM_ARENA_CHECK=1` | the geometry arena's blocks checked against a map of held granules: overlaps counted, reported with `RSX_GEOM_CACHE_STATS=1` (whose "geometry arena" line gives chunks, device-local chunks, MB in blocks, pending, waits for room, failed requests) |
| `DOTHEROES_NATIVE_INFLATE=0\|check` | Edge zlib's large inflate (every area's map pack) natively: on by default (port/native/edge_inflate.c, an SPU entry hook); `0` the lifted routine; `check` both, each output compared (`[edge-inflate] check`) |
| `SPURS_FINALIZE_KEEP_TASKS=1` | A/B: cellSpursFinalize leaves its tasks parked in WAIT_SIGNAL, as before patch 0087 (one SPU pool worker leaked per area load) |
| `PS3RECOMP_SPIRV_CACHE=<dir>\|off` | translated shaders and the Vulkan pipeline cache kept there between sessions (the port: `<data>/cache/shaders`; tests/boot_smoke.py: off unless set); checked at startup against this build, glslang, GPU and driver (patch 0090) |
| `VK_COMPILE_LOG=1` | a `[vk-compile]` line per shader translation (glslang, memory or disk) and per pipeline built, with its time on the trace's clock; tests/batch_report.py counts them per segment |
| `VK_SYNC_CREATE=1` | A/B: a new texture or render target flushes the batch and waits for the GPU (twice) instead of its first barrier and clear going into the batch (patch 0089) |
| `RSX_GEOM_DYNAMIC_LIFETIME=1` | A/B: resident geometry's old rule -- an entry rebuilt four times in its life goes to the copy path for good (made every static draw dynamic after four area reloads) |
| `SCENARIO=<name>` | tests/boot_smoke.py: `DRIVE=tests/scenarios/<name>.drive`; its `#! saves`, `#! env`, `#! secs` lines apply unless set; `FIGHTS`, `ROUNDS` and other `${NAME:-default}` values in the drive come from the environment |
| `HOLD_AFTER_DRIVE=1`, `ALLOW_CONCURRENT=1`, `MEM_EVERY=<s>`, `RUNS_IN=<dir>`, `PREEMPT_TESTS=1` | tests/boot_smoke.py: keep the game running after the drive ends; start even though another game is running; memory.csv's period (30 s); where run directories go (default build/test; play sessions build/sessions); stop running tests and hold further ones (build/test/HOLD) before starting (play sessions) |
| `DOTHEROES_DEV_CANT_DIE=1\|<floor>,<refill>` | testing: keeps the hero alive by topping life up below full -- 8 half apples or less raised to 9 (default; never full, so no sword beam). It heals, not only stops lethal hits; each top-up logged and counted. Same setting in every arm of a comparison; report runs with it apart; recheck with normal damage |
| `DOTHEROES_DEV_LIFE_LOG=1` | testing: every change of the player's life and the HUD's copies, logged |
| `DOTHEROES_DEV_MEMSNAP=1` | testing: a drive script's `touch memsnap.trigger` writes all committed guest memory to `<run>/memsnap_<n>.bin` (game memory: local only; tests/native/find_state.cpp); copied in memory first, the log gives the copy's start and end |
| `DOTHEROES_DEV_PLAYER_LOG=1\|<bytes>` | testing: the player's object (first 0x1000 bytes) every 4 ms into `<run>/player.bin`, ~1 MB/s (game memory: local only; tests/fps_check.py) |
| `DOTHEROES_DEV_ACTOR_LOG=1` | testing: the characters within 40 units of the hero (class vtable at +0, position at +0x428), their positions every 4 ms into `<run>/actors.bin` (game memory: local only; tests/actor_steps.py) |
| `DOTHEROES_STEP_CARRY=0` | the hero's per-frame step truncated to 0.01 as the game does; by default the truncated part is carried into the next step, so the hero walks at 9 units a second at any frame rate (port/game_patches.cpp) |
| `DOTHEROES_DEV_WATCH=<hex offset>` | testing, with `PPU_WWATCH=10000` (any address: it routes the lifted stores through the runtime's hooks): when a drive touches `watch.trigger`, the first 48 stores to the player's object from `<offset>` (0x80 bytes) are logged as `[sync-write] +<offset - 0x40 + n>` with the storing guest function (tests/scripts/watch_walk.drive) |
| `PPU_WWATCH=<hex ea>` | the runtime's store watch: every lifted PPU store to that 16-byte line, with the writing guest function (found the HUD's life writer) |
| `VISIBLE=1`, `GRAB_EVERY=<s>` | tests/boot_smoke.py: an on-screen run -- refused on a locked desktop, stopped if it locks, the window is minimised/hidden, or another window covers more than a tenth of it for 3 s (a covered window ran at ~35 fps); guard.json has the foreground and covered shares; window grabs to grabs/ |
| `TOPMOST=1` | tests/boot_smoke.py, with VISIBLE=1: the game's window kept above other windows without taking the focus |
| `DOTHEROES_NATIVE_CD=<0-3>` | the Bullet collision task: lifted (default), native (experimental), check (native against lifted, `[native-cd]` lines), self-check (native against itself rounding toward zero); `DOTHEROES_NATIVE_CD_DUMP=n` prints differing manifolds, `DOTHEROES_NATIVE_CD_FP=rz` runs native toward zero (docs/BULLET_NATIVE.md) |
| `GAMEDATA=<dir>` | tests/boot_smoke.py: play on a persistent save root in place (`%LOCALAPPDATA%\dotheroes\gamedata`) |
| `RSX_DROP_LOG=1`, `VK_DRAW_SKIP_LOG=1` | draws the engine / the Vulkan backend discard before drawing, with the reason |
| `RSX_FP_DEBUG=<hash>:0:m` | paint that fragment program solid magenta: does the draw produce pixels at all |
| `RSX_ENGINE_PASS_DUMP_FP=<hash>` | with `PASS_DUMP=1`: start the pass capture at the first draw through that program |
| `PS3RECOMP_VK_SYNC_PRESENT=1` | old blocking present (A/B) |
| `PS3RECOMP_VK_WINDOW`, `PS3RECOMP_VK_FULLSCREEN` | window; fullscreen |
| `PS3RECOMP_VK_DUMP=<ppm>`, `PS3RECOMP_VK_DUMP_EVERY=<n>` | frame dumps (tests) |
| `PS3RECOMP_VK_GUEST_PROGRAMS` (default on) | the game's own shaders via glslang |
| `DOTHEROES_FPS` | frame cap: 60 (default), 30, 0, `display` (a frame every whole number of the display's refreshes, at most 90 fps: 72 on a 144 Hz display, 60 on 60 or 120 Hz; the exact period from the compositor, main.cpp display_frame_ms) |
| `DIAG_VBLANK_MS=<ms>` | vblank period, frame-rate tests only |
| `PAD_FILE=<file>` | closed-loop input (`tests/drive.py`): `0xMASK [reports] [ms]` |

## Working on the ps3recomp runtime

Edit in `build/ref/ps3recomp` (branch `dotheroes`), commit there as
`dotheroes <dotheroes@localhost>`, then export with
`git format-patch --start-number <next> -o ../../../patches/ps3recomp HEAD~<n>`
and commit the patch files here. `tools/pipeline.py` refuses to run with
uncommitted changes in that checkout, and re-applies the whole series when
the patch files change. 91 patches (0001-0091) as of 2026-11-02.
