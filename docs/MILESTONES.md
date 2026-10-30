# Milestones

A milestone is complete only with recorded evidence: logs, captures, test
results. Compiling is not evidence.

| # | Milestone | Status |
|---|---|---|
| 1 | Analysis and feasibility | **Complete**, see FEASIBILITY.md |
| 2 | Native executable with diagnostics | **In progress** (boot criteria met; debug console, golden log, Windows build open) |
| 3 | Initial Vulkan rendering and native audio | **In progress**: renders and plays through Vulkan on Linux, audio mixed natively; ~17 fps in gameplay |
| 4 | Menus and new-game startup | **In progress**: title → new game → opening → free movement, driven by `tests/scripts/new_game.drive` |
| 5 | Core gameplay | Not started |
| 6 | Full-game coverage | Not started |
| 7 | Stability and release preparation | Not started |

## M1 evidence
- Disc verified and extracted; EBOOT decrypted with HMAC verified; hashes pinned.
- Middleware identified from the binary: FMOD Ex 4, Bink 1, Lua 5.1.3,
  Bullet (SPURS), PSGL, Edge zlib.
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
- Current stop: the RSX command-FIFO walker mis-decodes PSGL's command stream
  every few thousand frames: a "JUMP"/"CALL" to unmapped IO, then a resync to
  `put`. Re-reading shows the bad word is stable (not a late write), and the
  walker has also run past the end of the 2 MB command buffer. A resync drops
  label writes, and the game later waits forever on one of those fences
  (`func_0045AC78`). It is timing-dependent: one 150 s run never hit it. The
  game does not use libgcm's buffer-full callback here, so PSGL manages its
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
    The bottleneck is the FIFO walker thread: CPU vertex conversion and the
    remaining GPU waits.
  - **Colour.** The opening's muted look is intended: `playerParam.lua.eu`
    sets `sepia_tone = 0.60` for the legendary-hero prologue.
- Test tooling:
  - `tests/drive.py` with scripts in `tests/scripts/` provides closed-loop
    input, using log markers and screen-change detection.
  - `tests/contact_sheet.py`, `tests/peek.py` and `tests/thread_cpu.sh`.
  - `tests/prof_report.py` reports on `DIAG_PROF` samples.
  - `boot_smoke.sh` keeps the last 5 runs.
- Next:
  - reach 30 fps (vertex conversion, GPU waits);
  - windowed play with a launcher;
  - save/load;
  - Windows build.
