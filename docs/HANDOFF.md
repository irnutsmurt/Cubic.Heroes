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
- **Isolate tests** from the desktop and personal settings; avoid modifying
  unrelated projects or system settings; signal only processes you started.
- Commit identity for this repo is the owner's own git config; ps3recomp
  patches are committed as `dotheroes <dotheroes@localhost>` on the
  `dotheroes` branch of `build/ref/ps3recomp`.
- Repo: public, GPL-3.0, `main` branch. A similar recompilation
  project for Street Fighter Alpha 3 on the Dreamcast (SFA3) is the model for the
  setup app and release downloads.

## Where things stand

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
- **Speed (perf_walk.drive, gameplay): a solid 60 fps** (2026-10-30), from
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
  - **The vblank on its own clock** (`port/main.cpp` vblank_clock): the game
    paces every frame by the vblank count, which the walker used to tick from
    a GetTickCount64 loop (16 ms steps, delayed by drains and presents, then
    fired twice to catch up): ~7% of frames took two vblanks. Now a monotonic
    clock ticks it within a fraction of a ms.
- Before (for the record): the FIFO walker set the frame, drain 15.2 ms
  (~9.4 ms of it re-reading, re-hashing and re-copying static vertex data:
  the vertex cache saw ~100% hits and 0 changes, 491 blocks, 58 MB) plus
  3.7 ms waiting at the flip for the previous present.
- **Still emulation scaffolding** (to replace, not to keep): the RSX command
  stream walked and translated to Vulkan (draw engine), the walker's 1 ms
  FIFO poll, the present path (full-frame readback; present_mu held across
  the GPU fence: the walker still waits ~3 ms a frame there), the emulated
  vblank counter the game paces by, SPU code run inside the SPURS/lv2 HLE.
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

**Setup app (SFA3 style): written, not yet run end to end.**
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
  random crashes).
- **Saves past the opening**, made by the owner, so testing can skip it.
  RPCS3 and the port use the same layout,
  `dev_hdd0/home/00000001/savedata/<save folder>`: copy a save folder into
  the port's `PS3_GAMEDATA_ROOT/dev_hdd0/home/00000001/savedata/`
  (`tests/boot_smoke.sh` gives each run its own root: seed it from a copy,
  never from the owner's originals). Saves are personal data: keep them out
  of git.

## Next steps, in order

The owner's goal (2026-10-30) is a performant native PC port: the recompiled
code is the starting point, and PS3-specific machinery (SPU workloads,
SPURS/lv2 scheduling and polling, DMA, the RSX-command-to-Vulkan layer) is
replaceable once its purpose is understood. Every replacement is incremental,
backed by before/after timings and correctness checks, and keeps gameplay
speed independent of the frame rate. Steps 1-3 of the original list (Windows
tooling, the native Windows build, measuring on the 3070) are done; see
"Where things stand".

1. **Next replacements:** the present path (frames in flight, no readback,
   no lock held across GPU waits), then a display-driven vblank (refresh-rate
   aware, e.g. 120 Hz panels). vm_watch on Linux (mprotect + SIGSEGV), so
   resident geometry runs there too. Validate each with perf_walk.drive +
   `tests/frame_times.py` and pass-dump/screenshot A/B.
2. **Colours:** see "Where things stand"; compare RPCS3's scene buffer.
3. **boot_smoke.sh:** retire it once `boot_smoke.py` is tested on Linux.
4. **Run the setup app end to end** on both OSes (`python setup_app.py`, then
   `--convert`), then `tools/make_bundle.py --prebuilt` and the frozen apps.
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
   - **"Install Game Data onto the HDD?" -> Yes crashes.** The install thread
     calls an unimplemented system function (unresolved NID `0x0D5B4A14`),
     then the game exits and the shutdown aborts. The half-written
     `dev_hdd0/game/BLUS30490DATA` it leaves makes the next start exit at
     once. Implement the function; make the shutdown path clean.
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
7. Open issues: a flat beige quad over the castle path at ~180 s of
   `intro.drive` (pre-existing, both present modes); remaining walker cost
   (vertex data could stay in GPU buffers instead of being copied each
   frame); save/load untested; debug console and golden boot log (M2).

## Diagnostics and switches (environment variables)

| Variable | What |
|---|---|
| `PS3_STRICT=1` | strict mode: silent fallbacks become fatal (dev default) |
| `DIAG_FPS=1` | guest flips per second every 10 s |
| `DIAG_PROF=<start>,<secs>` | all-thread wall-clock sampler -> `profile.raw`; `tests/prof_report.py` |
| `DIAG_DUMP_AT`, `DIAG_CRASH_DIR`, `DIAG_HANG_EXIT` | crash/hang reports |
| `VK_WAIT_STATS=1`, `VK_GPU_TIME=1` | CPU waits on the GPU by call site; GPU time per 5 s |
| `VK_DRAW_BATCH` (default on), `VK_DRAW_BATCH_MAX` | draw batching (4096 per submission) |
| `RSX_VERTEX_CACHE=0`, `RSX_VERTEX_CACHE_STATS=1` | the cross-frame vertex cache |
| `PS3RECOMP_VK_SYNC_PRESENT=1` | old blocking present (A/B) |
| `PS3RECOMP_VK_WINDOW`, `PS3RECOMP_VK_FULLSCREEN` | window; fullscreen |
| `PS3RECOMP_VK_DUMP=<ppm>`, `PS3RECOMP_VK_DUMP_EVERY=<n>` | frame dumps (tests) |
| `PS3RECOMP_VK_GUEST_PROGRAMS` (default on) | the game's own shaders via glslang |
| `DOTHEROES_FPS` | frame cap: 60 (default), 30, 0 |
| `DIAG_VBLANK_MS=<ms>` | vblank period, frame-rate tests only |
| `PAD_FILE=<file>` | closed-loop input (`tests/drive.py`): `0xMASK [reports] [ms]` |

## Working on the ps3recomp runtime

Edit in `build/ref/ps3recomp` (branch `dotheroes`), commit there as
`dotheroes <dotheroes@localhost>`, then export with
`git format-patch --start-number <next> -o ../../../patches/ps3recomp HEAD~<n>`
and commit the patch files here. `tools/pipeline.py` refuses to run with
uncommitted changes in that checkout, and re-applies the whole series when
the patch files change. 35 patches as of this handoff.
