# Milestones

A milestone is complete only with recorded evidence: logs, captures, test
results. Compiling is not evidence.

| # | Milestone | Status |
|---|---|---|
| 1 | Analysis and feasibility | **Complete**, see FEASIBILITY.md |
| 2 | Native executable with diagnostics | **Met on Windows** (2026-11-02): strict-clean, 0 unresolved NIDs; crash/hang reports, the frame trace and hitch captures, the game-state probes. Debug console and golden boot log still open |
| 3 | Initial Vulkan rendering and native audio | **Met on Windows**: 60 fps in the scenes measured, with the validated configuration (HANDOFF.md, "Current status"); colours vs RPCS3 not verified |
| 4 | Menus and new-game startup | **Met**: title, new game, CONTINUE, the system menu and its pages, the dot editor, save and load |
| 5 | Core gameplay | **In progress**. Manual play (the owner): the castle, the field, the river's fights, the first dungeon and its boss (2026-11-02/03). Automated: the field, the river's fights, the first dungeon to the boss's door (checkpoint dungeon1-boss-door); the boss route is exploratory (its key chest does not open yet) |
| 6 | Full-game coverage | Not started (HANDOFF.md, "Game coverage") |
| 7 | Stability and release preparation | **In progress**: long-session soaks with reloads; the setup app's headless conversion run end to end on Windows (2026-11-02: a working install from the owner's disc and keys); its GUI, the frozen bundle and a first release not yet |

The native-port milestones (R0-R7: the PS3 graphics path replaced, then
retired) are defined in NATIVE_PORT.md; their records follow its section 10
and are logged at the end of this file.

The current state, open problems and next actions are in HANDOFF.md,
"Current status"; this file keeps the evidence log.

## M1 evidence
- Disc verified and extracted; EBOOT decrypted with HMAC verified; hashes pinned.
- Middleware identified from the binary: FMOD Ex 4, Bink 1, Lua 5.1.3,
  Bullet (SPURS), Edge zlib; graphics through the "asp" engine's ps3gcm
  backend and the PPFX post-effects library on a statically linked libgcm
  (first listed as PSGL; corrected 2026-11-04: the "PSGL" strings are PPFX's
  shader file names, FEASIBILITY.md §3).
- 237 imports and 26 SPU images inventoried.
- Full PPU lift with 0 undecoded instructions and 638/638 jump tables; all
  26 SPU images lifted.
- Upstream Vulkan host scenes 18/18 and SPIR-V tests 28/28 on RADV.
- All 5 Bink movies decode error-free with FFmpeg's LGPL decoders.

## M2 exit criteria
1. [x] `tools/pipeline.py` reproduces generated sources from the ISO.
2. [x] Linux build links all lifted PPU + SPU code and the runtime into one
   executable (`build/port/dotheroes`, 186 MB). A clean build takes 34.5 min
   at -j4 and never dropped below 5.9 GB of free RAM. Incremental rebuilds
   after runtime changes take 15–40 s.
3. [x] Strict mode: unresolved NID (both paths), garbage/unresolved indirect
   call, unlifted function, unknown syscall, SPU interpreter entry, SPU
   unlifted branch, and SPU thread without code are fatal, with a
   symbolized report. Verified live: it stopped the boot at three real gaps.
4. [~] Linux crash handler and hang watchdog done, and verified on real
   crashes. Still to do: debug console, and symbolized SPU stacks.
5. [x] Boot reaches the first `cellGcm` flip with zero strict-mode
   violations. Evidence: `tests/boot_smoke.sh` runs (headless, null RSX
   backend) present 4,169 guest flips before the FIFO stall described below,
   and one 150 s run had no stall at all; strict mode reported nothing.
6. [ ] Boot-log golden in a regression test.
7. [ ] Windows build compiles and links.

- 2026-10-29: first boots of the recompiled game. Each blocker was found by
  the diagnostics and fixed at its root cause:
  1. **r2 (TOC) = 0 during C++ static init.** The HLE fast path
     (sys_lwmutex_lock/unlock) skipped the import stub's `std r2,40(r1)`.
     Found with the new `PS3_TOCCHECK`, fixed in patch 0002.
  2. **Crash reports lost or hung.** ps3_stdio re-registered SIGABRT with
     `signal()` and called our SA_SIGINFO handler with one argument (patch
     0003). Core dumps of the 4 GB guest space took tens of seconds, so the
     test harness now sets `ulimit -c 0`.
  3. **cellGamePatchCheck unimplemented** (patch 0004).
  4. **Guest setjmp/longjmp unsupported by the lifter.** Lua 5.1's error
     handling needs it. Added `--sjlj` (patch 0005, `config/sjlj.json`).
     Not yet exercised: no Lua error has occurred so far.
  5. **Main-thread priority 0.** The game asserts `primary_prio - 1 >= 0` and
     exits cleanly. The loader now reads PT_PROC_PARAM (patch 0006).
  6. **_sys_spu_printf_initialize unimplemented** (patch 0007).
- 2026-10-29, continued. Fixed at their root causes (patches 0009–0017, the
  pipeline and `config/`):
  7. **SPURS task attribute misread.** `_cellSpursTaskAttributeInitialize`
     takes a `CellSpursTaskSaveConfig*` in r7 (seen at `0x4736E0`); the size
     had been read as a stack address. Added
     `cellSpursTaskAttributeSetExitCodeContainer` and
     `cellSpursTaskExitCodeGet` (patch 0009).
  8. **Flip status stuck at WAITING.** `cellGcmSetFlipStatus` takes no
     argument and marks the flip done; upstream stored the caller's leftover r3
     (patch 0010). The game then reached its first flip.
  9. **FMOD init retried forever:** the MultiStream MP3 codec is a firmware
     file. Handled with an instruction patch (`config/insn_patches.json`); see
     FEASIBILITY.md, explicit exceptions. The retries had also filled the
     runtime's 64k lightweight-mutex table, which made every later mutex create
     crawl.
  10. **FMOD's SPU mixer spun 300M times on `stop 0x110`:**
     `sys_spu_thread_bind_queue` was a stub and lifted SPU threads had no
     receive-event service. Both now follow lv2/RPCS3 semantics (patch 0012).
  11. **`cellSaveDataEnableOverlay`** (NID 0xE7FA820B) added (patch 0013).
  12. **Bink movies not found** through raw `sys_fs` (it ignored the derived VFS
     root); the game quit when its title movie failed to open (patch 0014).
  13. **FMOD codec plugin runs at a run-time LS address.** Added relocatable SPU
     lifting and dispatch (patch 0016, `config/spu_relocatable.json`). The
     codec now runs lifted at LS 0x156F0, with no interpreter.
  Also: an executed untranslated SPU instruction is fatal in strict mode
  (patch 0015), and there are new probes `PS3_FS_FAIL_ABORT`, `SPU_OVL_TRACE` and
  `GCM_BADBRANCH_REREAD`. The pipeline no longer writes an unused
  `build/gen/ppu_hle_nids.cpp`; the port build generates the NID table itself.
- Current stop: the RSX command-FIFO walker mis-decodes the game's command stream
  every few thousand frames: a "JUMP"/"CALL" to unmapped IO, then a resync to
  `put`. Re-reading shows the bad word is stable (not a late write), and the
  walker has also run past the end of the 2 MB command buffer. A resync drops
  label writes, and the game later waits forever on one of those fences
  (`func_0045AC78`). It is timing-dependent: one 150 s run never hit it. The
  game does not use libgcm's buffer-full callback here, so its own libgcm code
  (asp and PPFX; first written up as PSGL) manages its
  own segments. This is RSX/FIFO work (start of M3). Next step: log every
  JUMP/CALL/RET the walker takes, and every `put` the game publishes, and find
  the first command whose size the walker gets wrong.
- Not yet exercised: FMOD's 16 DSP plugins (relocatable lifts registered; none
  loaded during boot), the Water SPURS task (waits for its first signal), and
  Bink playback beyond opening the file.
- Harness note: the port ignores SIGTERM, so `boot_smoke.sh` ends a run that
  never hangs with SIGKILL (exit 137).

## Known limitations and open questions
Tracked in FEASIBILITY.md §6 and the `[unknown]` markers there.

## M2 progress log
- 2026-10-29: strict mode added as ps3recomp patch 0001
  (`patches/ps3recomp/`, applied by the pipeline's toolchain stage). It covers
  unresolved NID, garbage/unresolved indirect call, unlifted function,
  unknown syscall, SPU interpreter entry, SPU unlifted branch, and SPU
  thread without code.
- 2026-10-29: `port/` created from the ps3recomp template. Development
  diagnostics in `port/diag/` (crash report with guest back-chain and host
  backtrace symbolized to lifted functions; hang watchdog that dumps every
  thread by real-time signal). Gated by `DOTHEROES_DEV_DIAGNOSTICS`.

## M3/M4 progress log
- 2026-10-29/30 (patches 0018-0028):
  - **FIFO corruption.** Main-memory GPU reports (DMA 0xBAD68000) are
    relative to IO 0x0E000000; written at the bare offset they overwrote the
    command buffer. Report areas now live in guest memory (patch 0018).
  - **Vulkan.** The port selects the Vulkan backend (`PS3RECOMP_RSX_VULKAN`),
    with the game's own shaders translated to SPIR-V. Evidence: frames of the
    logos, title, menus, character select, opening and in-game world
    (`tests/boot_smoke.sh` screenshots).
  - **SPURS queues.** Edge zlib's LF queue: RPCS3's CellSyncLFQueue push with
    SPURS signalling (patch 0020; exception E2). Bullet's CellSpursQueue: the
    layout read from the title's own SPU library; push and pop implemented
    (0021). Upstream's push had been a silent no-op.
  - **Bullet task vtables.** Function discovery takes lone code pointers in
    SPU data segments (0022).
  - **Audio.** POSIX mixing paced like Windows (0024). A 340 s capture held
    music and effects throughout.
  - **Input.** Keyboard on Linux through SDL (0019): arrows, Z/X/A/S =
    Cross/Circle/Square/Triangle, Q/W = L1/R1, 1/2 = L2/R2, Enter = Start,
    Tab = Select.
  - **Speed.** Gameplay went 6 → ~17.5 fps on RADV Renoir: draw batching, an
    index-range fast path, a scratch target for colour-less passes,
    persistent render passes and double-buffered submission (0026-0028).
    Then ~26 fps (`intro.drive`, 110-210 s, A/B on the same route):
    - The frame clock records and submits a present on the walker
      (`_present_begin`); a finisher thread waits for it, reads it back
      and shows it (`port/main.cpp`). Waiting on the walker put the whole
      frame's GPU time between the guest's flips: `DIAG_PROF` showed 42%
      of the walker blocked there. Blocking: 18.8 fps; split: 26.
      `PS3RECOMP_VK_SYNC_PRESENT=1` restores the blocking present.
    - One batch per frame (up to 4096 draws, was 512; patch 0030). At 512
      a frame split in two and waited at its present for the first half:
      20.9 → 25.9 fps with the split present.
    - Views of feedback snapshots are destroyed after their batch's fence
      instead of after a synchronous flush (0030); half-float and float
      vertex decode made branchless (0029, bit-identical).
    - The port now defaults `PS3RECOMP_VK_GUEST_PROGRAMS=1` and
      `VK_DRAW_BATCH=1`; the environment overrides them.
    - Then ~28 fps: the frame clock polls the FIFO every 1 ms (was 4), and
      the vertex fetch runs in 128-vertex chunks with SSE2 half and float
      decode (0031; bit-identical, tested exhaustively for halves).
    - Then 29-30 fps, the game's cap in steady play (patch 0032), from a
      cross-draw vertex cache in the draw engine. The game fetched ~900k
      vertices a frame, 97% of them from source ranges fetched within the
      last second. Fetched blocks are kept under a key of the draw's
      references, layout and attribute ranges, plus a 64-bit hash of the
      guest bytes in those ranges. Every lookup re-hashes the bytes and
      refetches on any change. Over the explore route: 2.46M lookups,
      434 misses (first loads), no changed data, ~50 MB resident.
      `RSX_VERTEX_CACHE=0` turns it off; `RSX_VERTEX_CACHE_STATS=1`
      reports hits. Screens match the pre-cache run.
    - Validation layer, with sync validation enabled: no messages over
      the intro route.
    - `VK_GPU_TIME=1`: the GPU is busy ~22 ms a frame, 50-57% of the wall
      clock. `VK_WAIT_STATS=1`: the walker's GPU waits are now ~2 ms per 5 s.
  - **Frame cap (removed by default, at the user's request).** The game
    caps itself at 30 fps twice: its main loop (0x2A5C8) sleeps until each
    frame has taken 30 ms, and func_00029BD8 waits for two vblanks per
    frame. Its logic runs on measured time: a fixed 1.5 s walk ends in the
    same place at 20 and at 31 fps (`tests/scripts/speed.drive` with
    `DIAG_VBLANK_MS=25`). Three guarded instruction patches
    (`config/insn_patches.json`) select the cap through `DOTHEROES_FPS`:
    `30` (original), `60` (one vblank per frame, default), `0` (none).
    On this laptop menus run at 57-60 fps; gameplay runs at ~31, limited
    by the hardware.
    - Runtime bug found on the way (patch 0034): `cellGcmGetVBlankCount`
      added a vblank on every query, so the two-vblank wait returned after
      two polls. It now returns the ticked count. The frame clock ticks at
      60 Hz (16/17/17 ms), not 62.5 Hz. With the real wait, the original
      30 fps pacing gives ~25 fps in gameplay on this laptop: a frame
      takes slightly over two vblanks, so it waits for a third. Real
      hardware behaves the same way when a frame runs long.
    - `PAD_FILE` takes an optional hold time in milliseconds (patch 0033)
      and `drive.py` a `hold <BUTTON> <sec>` step. A hold counted in pad
      reports would halve at 60 fps.
  - **Open rendering issue.** A large flat beige quad covers the path to the
    castle at one point of the intro route (screenshot at ~180 s). It is
    present with the old blocking present too, so it predates these changes.
  - **Colour.** The opening's muted look is intended: `playerParam.lua.eu`
    sets `sepia_tone = 0.60` for the legendary-hero prologue.
- Test tooling:
  - `tests/drive.py` with scripts in `tests/scripts/` provides closed-loop
    input, using log markers and screen-change detection.
  - `tests/contact_sheet.py`, `tests/peek.py` and `tests/thread_cpu.sh`.
  - `tests/prof_report.py` reports on `DIAG_PROF` samples.
  - `boot_smoke.sh` keeps the last 5 runs. Pruning had never worked: the
    project path contains spaces and `xargs` split it. No path fragment
    existed, so nothing else was removed. It now splits on newlines only.
  - `boot_smoke.sh` refreshes `frame.ppm` every 15th present by default.
    Dumping every frame cost several fps and skewed measurements.
- Next:
  - hold 30 fps in busier scenes. The walker still copies each frame's
    vertices into the Vulkan ring (~1/4 of its time; cached blocks could
    live in GPU-visible buffers instead), and the GPU takes ~22 ms a frame;
  - the beige quad;
  - windowed play with a launcher;
  - save/load;
  - Windows build.

## Windows port and setup app (2026-10-30, handed over: see HANDOFF.md)

- The runtime builds for Windows with Zig's clang (MinGW ABI), Vulkan backend
  included (patch 0035); the full Windows game build had not linked yet.
- SFA3-style setup app written (`setup_app.py`, `tools/setup_pipeline.py`,
  `tools/zig_build.py`, `tools/build_deps.py`, `tools/make_bundle.py`); not
  yet run end to end.
- Linux re-checked after these changes: boots and plays the intro route,
  strict-clean, guest shaders on.

## Windows: 60 fps, long sessions, test tooling (2026-10-31 to 2026-11-02)

Details and numbers in HANDOFF.md ("Current status", the work log).
- The validated 60 fps configuration (native collision, three native solver
  commands, the capped lap, one present per flip; all experimental, off by
  default): river fights p99 17.2-17.4 ms, exploration p99 17.3 ms
  (build/baselines/2026-11-01-config-validation). Native collision's contact
  differences from the SPU task remain unresolved.
- Gameplay speed independent of the frame rate at 30 / 60 / ~120 / 144-paced
  (the hero's per-frame step rounding carried over; build/baselines/
  2026-11-02-frame-rate).
- Menus and features: SNAPSHOT/ALBUM, the dot editor's SAVE, INSTALL, round
  trips across restarts (patches 0072-0078).
- Long sessions: area reloads made the draw engine's resident geometry
  dynamic for good after the fourth (fights 14-28% of frames at ~33 ms);
  fixed (patch 0080), six-cycle A/B on one build.
- Tests reach and verify an encounter: named scenarios from kept checkpoints,
  state checks at every stage, bounded retries, measured segments with
  required activity, batch reports, rolling hitch captures (patch 0079), play
  session recording for routes and checkpoints.
- 20 area-reload cycles hold 60 fps (patches 0080-0084), a crash at the 10th
  hitch mark fixed (0082); stable baseline tagged `stable-2026-11-02`.
- The first dungeon's entry: its 93-95 ms frame was Edge zlib inflating the
  room's map pack on a lifted SPU task; now native (SPU entry hooks, patch
  0086; port/native/inflate.c), 13 ms, exact against the lifted output and
  zlib; render targets and textures created without GPU waits (0089).
- Memory bounded over repeated area loads: a finalized SPURS instance's
  parked task ended (0087), the SPU pool no longer grows a worker per load.

## Native port: R0, the rendering evidence stage (2026-11-04)

- **Dependencies:** the 2026-11-04 checkpoint (tag checkpoint-2026-11-04,
  smoke-tested: 2 river fights ok, exploration 240 s, strict none).
- **Removed:** nothing (evidence only).
- **Remains:** the whole PS3 graphics path.
- **Evidence:** NATIVE_EVIDENCE.md. Six instrumented runs (river fight,
  exploration) and the counters in ps3recomp 0097-0099: one producer thread
  for every command; PPFX's pass anatomy and entry points; the game's waits
  by event (consumption, GPU completion, presentation); GPU time per render
  pass; no release-path fallback observed in the two scenes.
- **Performance:** the counters are off by default and leave the main
  thread's paths unchanged when off. The evidence build with every switch
  off: combat main-thread busy 6.26 ms against the checkpoint's 6.15 (two
  fights a side, both ok): +1.7%, inside the 1-2% batch drift, and too small
  for two runs a side to resolve (build/ab/2026-11-04-evidence/offcheck.sh).
- **Next:** R1 (one captured PPFX pass rendered natively) awaits approval.

## Native port: R1, one captured PPFX pass rendered natively (2026-11-04)

- **Dependencies:** R0; the owner's approval with corrections (2026-11-04).
- **Removed:** nothing from the game's active graphics path.
- **Remains:** the whole PS3 graphics path; the game renders as before.
- **Evidence:** NATIVE_R1.md. PPFX passes 3 (640x360 RGBA16F) and 48
  (1280x720 RGBA8, the final composite) from one frame of river combat and
  one of exploration, rendered by the standalone executor from a native pass
  description: bit-exact in all four (no pixel or channel differs), also
  from a poisoned target; programs remade byte-identical; synchronisation
  validation 0 errors and 0 warnings (negative control caught); the
  boundary log's queued program patches and constants equal what the GPU
  consumed; the executor links no runtime code (tools/r1_check_deps.py).
- **Performance:** the executor's GPU time per pass, 2,000 repetitions after
  warm-up: 24.6 us (pass 3) and 29.7 us (pass 48), against 20.5-23.6 and
  22.5-24.6 us in the game; the difference is unexplained, the obvious
  causes ruled out. No in-game performance claim. The two production
  refactors (ps3recomp 0102, 0103) change nothing built: no shader or
  pipeline cache miss in a river fight.
- **Commits:** ps3recomp 0100-0104; repo 795314b, cdb48e3, fdb5118, 37d91e2,
  246dc39.
- **Next:** R2's scope (the whole PPFX chain offline) awaits review (approved
  2026-11-04; done, below).


## Native port: R2, the whole PPFX chain rendered natively (2026-11-04)

- **Dependencies:** R1; the owner's approval of R2's scope (2026-11-04).
- **Removed:** nothing from the game's active graphics path.
- **Remains:** the whole PS3 graphics path; the game renders as before.
- **Evidence:** NATIVE_R2.md. PPFX's complete chain -- 47 draws a frame
  (pass 1 before asp's scene, 46 after) in river combat and exploration, 49
  in the first dungeon -- captured from the running game and run by the
  standalone executor on its own images, only the external inputs seeded
  (the scene target where asp hands it back, static textures): 555 frames
  (sequences of 120 and 180 consecutive frames, 2-3 s), 26,685 passes and
  555 feedback copies, **every output bit-exact** with carried state and
  with every image poisoned each frame; 26,685 program conversions
  byte-identical (27 program offsets, 26 programs, including the variants
  other moments of the route use); synchronisation validation 0 errors
  (negative control caught); 24,578 passes' boundaries equal what the GPU
  consumed, and every render state PPFX uses is its own; no history (no
  chain resource read in a later frame), what changes over time comes from
  the CPU in each frame's patched constants and program bytes. The executor
  links no runtime code. The main build at 0105-0107 passed a fight smoke.
- **Performance:** the executor's chain, 2,000 repetitions after warm-up:
  pass 1 17.7-17.8 us; the rest 461.5-462.1 us (river, exploration) and
  501.6 us (dungeon), barriers and the copy included -- against 843.8,
  680.9 and 709.6 us from pass 3's start to the last pass's end in the game
  (its passes alone 450.3, 370.7 and 395.3 us). A standalone comparison of
  comparable work, no in-game claim; R1's per-pass difference stays
  unexplained.
- **Not covered:** the first 3 s inside the dungeon; the one-off chains of
  an area's first frames; the chain after a return to the title (programs
  reloaded at other offsets); the boss fight and other areas (saves needed;
  the attract mode's demos are movies).
- **Commits:** ps3recomp 0105-0107; repo 02122f4, 36d4eb6, 9053968, 20e8356,
  43c705b, 6ac23f5, 380617e, 17c87ca, c16eb0a, and the documents.
- **Next:** R3's scope (PPFX in the game, dual run; NATIVE_R2.md) awaits
  review.
