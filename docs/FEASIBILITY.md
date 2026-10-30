# Milestone 1 — Feasibility and architecture report

Investigated 2026-10-29 against BLUS-30490 v01.00 (hashes in
`config/game_version.json`) and ps3recomp `4e8f3ca` (v0.18.0).

Every statement below is marked by how it was established:

- **[measured]**: produced from the game files or by running the tool here
- **[code]**: read in ps3recomp source
- **[doc]**: claimed in ps3recomp docs, not verified
- **[unknown]**: not yet determined

---

## 1. The game's technical architecture

### Disc
- **[measured]** Decrypted dump: one plaintext region spans all 1,627,350 sectors.
- **[measured]** 2,800 files, 3.33 GB.
- **[measured]** The game opens files by their mixed-case Joliet names (`data/movie/OpeningMovie_eng.bik`). The ISO9660 primary names are upper-case 8.3, so extraction must use Joliet. `tools/iso9660.py` does.

| Content | Files | Size | Format **[measured]** |
|---|---|---|---|
| Movies | 5 | 1.27 GB | Bink 1 (`BIKi`), 1280×720 @ 29.97 fps |
| Sound | 326 | 669 MB | Plain RIFF PCM WAV, 16-bit: 282 mono 44.1 kHz SFX/voice, 44 stereo 44.1/48 kHz BGM |
| Maps | 167 `.ps3.pac.gz` + others | 714 MB | gzip of a voxel/block table with embedded `DSBF` scenes |
| Textures | 763 `.ps3.tpk` | 654 MB | `TPKG` packages of named GCM textures (`*.dds` entries) |
| Scenes/models | 460 `.ps3.dsb` | 185 MB | `DSBF` binary scene (meshes, materials, `-fx` effect refs) |
| Scripts | 620 `.lua.eu` | 9 MB | **Plain Lua 5.1 source text**, Japanese comments |
| Shaders | `shaders.ps3.pkg` + 2 `.spk` | 1.2 MB | `spkg` container: 567 precompiled Cg (NV40 VP/FP microcode) with parameter names |
| Editor data | `.edt`/`.EDT`/`.sav` | 11 MB | `EDTF` sprite-editor data, default character saves |
| Leftovers | `MakeLuaDump.exe`, `AdditionEditCharMessage.exe`, `all_test.lua.eu`, `Thumbs.db` | | Developer tools shipped by accident; unused by the game |

None of these formats needs a native reimplementation. The recompiled game
parses them exactly as on PS3. Parsers are only worth writing as debugging
aids, for example to identify which `TPKG` entry a texture came from.

### Executable
**[measured]** `EBOOT.BIN` is a retail SELF:
- Key revision 0x0001, auth ID `0x1010000001000003`, firmware 3.15.
- `tools/self_decrypt.py` decrypts it with a user-supplied keys file. The HMAC of every segment verifies.
- The decrypted ELF's SHA-256 is pinned.

**[measured]** It is statically linked with GCC 4.1.1 (SDK 3.00):

| Segment | VA | Size |
|---|---|---|
| text + rodata + SPU images | 0x10000 | 16.5 MB |
| `.text` proper | 0x10240 | 10.9 MB → **2,727,512 PPU instructions** |
| data | 0xFE0000 | 6.0 MB file, **30.5 MB in memory** (24 MB BSS) |
| TLS | 0x1079F40 | 0x1F4 |

Code profile **[measured]**:
- **20,672 unique OPD functions** spread over several TOCs (3 dominant in the OPD table, 6 detected by the lifter); cross-TOC calls go through 2,858 linker trampolines.
- **10,536 indirect calls** (`bctrl`: C++ virtuals, Lua C functions, FMOD callbacks).
- 638 indirect jumps (`bctr`: switch tables).
- 448 inline `sc` syscalls.
- 62 `lwarx`/`ldarx` atomics.
- 76k FPU instructions and only **474 VMX instructions**. SIMD-heavy maths runs on SPU, not PPU.

### Middleware: what is actually used
Verified from strings and code in the decrypted ELF **[measured]**:

| Credited | What the game contains | How it reaches hardware |
|---|---|---|
| **FMOD** | FMOD Ex 4 for PS3 (`c:/anthill_builds/fmod4/...`), statically linked. Its SPU mixer `fmodex_spu.elf` (plus a NeuralTHX variant), ADPCM/raw codec SPU images and 16 DSP-effect SPU overlays (reverb, echo, lowpass, pitchshift, …) are embedded | Mixes on SPU and writes PCM to **`cellAudio`** ports. No libsnd3, libmixer, cellAdec or ATRAC |
| **Bink** | Bink 1 library (`binkliba`), with "Bink IO Thread" and "Bink Audio Thread" PPU threads. Frames become GCM textures ("Create_Bink_textures in gcmtextures"). No Bink SPU ELF is embedded **[unknown]**: whether part of the decode runs on SPU as a raw blob | Textures through cellGcm; audio presumably through FMOD or cellAudio **[unknown]** |
| **Lua** | Lua 5.1.3 + tolua, statically linked. Scripts are loaded from disc as source and compiled by the game's own VM | Pure game code |
| **Bullet** | Bullet physics (`btRigidBody`, `btCollisionObject`, …). Collision detection and constraint solving offloaded as SPURS tasks `PS3_SPURS_BulletCD_spu_release.elf` (174 KB) and `PS3_SPURS_BulletSolver_spu_release.elf` (30 KB) | cellSpurs **taskset** |
| (not credited) | **PSGL** (Sony's OpenGL ES over libgcm; 1,879 strings), **Edge zlib** SPU inflate task, two game SPU tasks (`water_task`, `waver_task`) | cellGcmSys, cellSpurs |

Conclusion: no credited component has to be reimplemented. All four are
ordinary game code in the EBOOT and recompile with it. The only open choice
is Bink's decoder (section 4).

### Firmware imports
**[measured]** 237 NIDs from 25 libraries:

| Library | NIDs |
|---|---|
| cellSpurs | 32 |
| cellGcmSys | 31 |
| sysPrxForUser | 27 |
| cellSysutil (incl. save data) | 24 |
| sys_fs | 19 |
| sys_net | 14 |
| cellResc | 11 |
| sceNpTrophy | 10 |
| cellAudio | 10 |
| cellMic | 8 |
| cellJpgEnc, cellPngEnc | 7 each |
| sys_io (pad) | 6 |
| cellGame | 5 |
| others | 1–4 each |

Not imported: cellVdec, cellAdec, cellDmux, cellPamf, cellSail, libsnd3,
libmixer, cellAtrac. The game does no media decoding through system
libraries.

SPURS usage is **tasksets only**: tasks, event flags, LF queues, queues,
barriers, exit codes. There are no job chains and no SPURS policy modules of
the game's own.

### Save data and progression
**[measured]** Imports cellSaveData List/Fixed/Auto Load2/Save2, FixedDelete,
ListDelete and cellGameCreateGameData/BootCheck. It also uses
`cellSysCacheMount` (a scratch cache on hdd1).

The save format is game-defined and written through the callback API, so it
is preserved byte for byte if the HLE is correct.

**[unknown]** Whether PS3 saves can be imported directly. That depends on
PARAM.PFD handling, which is not needed for the port's own saves.

---

## 2. What can realistically be statically recompiled

### PPU: all of it **[measured]**
The ps3recomp pipeline was run on this EBOOT:

1. ps3recomp sized the last OPD function (a 52-byte CRT stub) to the end of the executable segment. That would have lifted 5.4 MB of rodata, Lua text and SPU images as PPU code.
2. The cross-TOC trampolines were each emitted together with every trampoline after them.
3. The result was 3.2 GB of C++ containing 2.1 M undecodable words.

**Fixed for this game** by `tools/fix_functions.py`. It clamps functions to executable sections and splits out the 2,858 trampolines. After the fix:
- **24,026 functions lifted in 37 s, 172 MB of C++** (48 stable chunks, about 63k lines each).
- **638/638 indirect jumps resolved as jump tables** (5,009 case targets).
- 258 merged functions split by prologue scan.
- **0 undecoded instructions.** The 9,032 remaining `.word` placeholders are all jump-table offset data inside `.text`: values in ±64 K, primary opcode 0 or 63.
- The 10,536 indirect calls go through `ps3_indirect_call`'s address→function registry. Their coverage can only be proven at runtime, so misses must be fatal and logged (section 5).
- **Non-local control flow:** the game's libc has one `setjmp` (0x971D2C) and one `longjmp` (0x971E50), each with a single call site in Lua 5.1's `luaD_rawrunprotected`/`luaD_throw`.
  - Upstream ps3recomp could not express this: a lifted `longjmp` simply returns.
  - Added `ppu_lifter.py --sjlj` (patch 0005). It takes a host `setjmp` in the caller's frame and resumes there after the guest `longjmp` restores the registers. The guest code itself is unchanged.
  - Found later: during bring-up (docs/MILESTONES.md).

**[measured]** Compile cost on this laptop (clang 23.1, -O1, 4 jobs): about 2 minutes and ~550 MB per chunk. Full figures are in `docs/BUILD_COST.md` when the first full build completes.

### SPU: all 26 embedded images **[measured]**
- `extract_spu_images.py` and `build_spu_workloads.py` extracted and lifted all 26 images (544 KB).
- Per-image function byte coverage is 86–97%. **[unknown]** The remainder looks like data or padding and must be verified per image.

Where the SPU code runs:
- **SPURS tasks** (Bullet CD/Solver, Edge zlib, water ×2) run as lifted code inside the cellSpurs HLE, matched by image fingerprint **[code]**.
- **The FMOD mixer** is started as a raw SPU thread or a SPURS task **[unknown]**. It **DMAs its codec and DSP overlays into its own local store at run time and branches into them**. Today ps3recomp handles code it did not see at load time through its SPU interpreter **[code]**, which this project forbids in the shipped game. Overlay entry must dispatch to the lifted overlay by content fingerprint plus local-store address. That is new work and the biggest SPU-side item.

### What cannot be AOT-recompiled
- Nothing has been found so far.
- Lua scripts are **not** CPU code: the game's own recompiled Lua VM interprets them, as on PS3. This is original game behaviour, not runtime CPU instruction interpretation, and it stays.

---

## 3. Vulkan

**[measured]** Upstream builds with `PS3RECOMP_RSX_VULKAN=ON` and glslang 16.6 here. All 18 `ps3recomp_host` scenes pass on this laptop's RADV (Renoir) with guest programs on:
- draw, depth, quads, shader, tex, tex-io, mip, rtt, depthtex, mrt, mrt-a, cube, depthbounds, split-present, feedback, clip, clip-keep

`test_shader_spirv` passes 28/28. The validation layer is not installed here, so these runs were unvalidated.

### Architecture [code]
cellGcm FIFO → shared register-file **draw engine** (`rsx_draw_engine.c`, shared with Metal) → backend interface. The game's NV40 vertex and fragment microcode is decompiled to HLSL and turned into SPIR-V by glslang at runtime. The SPIR-V is cached in memory and on disk (`PS3RECOMP_SPIRV_CACHE`). The game's 567 precompiled Cg programs go through this path unchanged.

### Gaps relevant to this game [code]

| Gap | Impact | Work |
|---|---|---|
| **Linux-only** (`#ifndef _WIN32`, CMake FATAL on Windows, `dlopen`, pthreads) | Blocks the Windows requirement | Small: LoadLibrary/thread shim; SDL2 Vulkan works on Windows |
| No game template selects Vulkan (Linux games get the null backend) | Blocks M3 | Small |
| Constant-colour blend factors fall back to ONE; engine has no blend-colour state | Wrong transparency wherever the game uses them **[unknown]** | Medium |
| Logic ops implemented nowhere | Probably unused **[unknown]** | Small if needed |
| **Polygon offset missing on Vulkan** (Metal has it) | Decals, outlines, shadow acne | Small |
| Colour targets only RGBA8/RGBA16F; R32F/RGBA32F draws are **dropped** | Shadow maps or post-FX could vanish **[unknown]** which the game uses | Medium |
| Depth-only passes unhandled | Shadow-map passes | Medium |
| No anisotropic filtering, border colour always transparent black | Visual fidelity | Small |
| No MSAA (AA surfaces drawn at sample resolution) | Fidelity if the game enables RSX AA **[unknown]** | Medium |
| Fully synchronous submit-and-wait, readback every present, FIFO-only present, no `VkPipelineCache`, all images in GENERAL layout | Performance and frame pacing; faithful-first is fine, but this is a **likely M7 rewrite area** | Large |
| Validation never enabled in code; no debug-utils messenger | Debug tooling | Small |
| No RenderDoc in-app capture or texture identification on Vulkan (D3D12-only) | Debug tooling | Medium |

Windows: Vulkan needs the loader (`vulkan-1.dll`, present with every current GPU driver) and SDL2. Linux: Mesa RADV, ANV or NVIDIA. Both need glslang at build time.

The PSGL layer is game code that emits ordinary GCM command buffers, so it needs nothing special. Its 1,879 strings suggest the game may render mostly through PSGL **[unknown]**.

---

## 4. Proposed native audio and video

### Audio
- **[measured]** All sound content is plain 16-bit PCM WAV.
- **[measured]** The game drives FMOD Ex, which mixes on SPU and outputs to `cellAudio`.

**Recommendation: keep FMOD's own mixing and run it as recompiled native code.** This is the faithful path:
- The game's FMOD calls, its SPU mixer, codecs and DSP effects all run as AOT-lifted C on the PC CPU, producing the same samples as on PS3.
- Volume curves, pitch, 3D panning, reverb, voice stealing and timing all stay exactly original.
- `cellAudio` HLE then hands the finished 48 kHz float mix to the PC's audio API: WASAPI on Windows, SDL2 on Linux (PipeWire/Pulse/ALSA underneath).
- No PS3 audio hardware is emulated. cellAudio is a system API whose job is "take these PCM blocks and play them".

**Rejected for the baseline:** replacing FMOD's API with a different mixer. It would mean reimplementing FMOD Ex semantics from the outside and would audibly diverge. It also gains nothing, because the content is already PCM. FMOD itself cannot be the replacement: it is proprietary and the PC FMOD Ex 4 runtime is end-of-life.

Work required:
- Fix Linux cellAudio pacing. **[code]** It sleeps 2–5 ms per 5.33 ms block, so latency grows without bound.
- Make FMOD's overlay loading dispatch to lifted code (section 2).
- Add per-port mix diagnostics.

### Video
- **[measured]** FFmpeg 9.0.2's LGPL `binkvideo` and `binkaudio_dct` decoders decode **all five movies end to end with zero errors** in strict mode.
- Two movies have Bink audio (44.1 and 48 kHz stereo).
- The opening and title movies have no audio track; their music comes from FMOD.

**Recommendation: decode Bink natively with FFmpeg (libavcodec, LGPL-2.1, dynamically linked).**
- Intercept the game's Bink API at function level: BinkOpen, BinkDoFrame, BinkNextFrame, BinkCopyToBuffer, BinkWait, BinkClose, sound-track selection.
- Feed decoded YUV/RGB into the same GCM texture memory the game expects, so its own movie-drawing code still runs.
- Bink audio becomes PCM through the same native audio output.
- Timing follows the game's own BinkWait/frame calls, which keeps pacing and A/V sync identical.

Prerequisite: **[unknown]** identifying Bink's entry points in the stripped binary. Anchors exist: "Not a Bink file.", "Error reading Bink header.", the thread names, and the Lua movie list in `data/movie/Movie.lua.eu`.

Fallback and reference: the game's own Bink decoder is already lifted. If interception proves fragile, it can run as recompiled code. That is still native, but less aligned with the "native library" requirement, so it stays a reference oracle.

Licensing:
- FFmpeg built LGPL-only (`--disable-gpl --disable-nonfree`) and loaded dynamically is compatible with distribution. Bink 1 decoding in FFmpeg is clean-room.
- No cellVdec or cellPamf path is involved.
- RAD's own SDK is neither needed nor shipped.

---

## 5. Debugging capabilities

### What ps3recomp already provides [code]
- **Symbolized guest stack traces:**
  - `func_XXXXXXXX+off` from host frames
  - guest stack dump with all GPRs
- **Call tracing:**
  - `PS3_CALLTRACE` (indirect calls with filters)
  - `BCTRL_RING` (per-thread ring of indirect targets)
  - `PS3_HLE_TRACE` and `PS3_SCTRACE` (firmware calls and syscalls)
- **Timeline:** `PS3_FTRACE` plus `ftrace_report.py` (syscalls, SPU, RSX, flips).
- **Bad indirect target report:** registers, vtable walk, LR, host backtrace.
- **Watchpoints:**
  - PPU: `PPU_WWATCH`/`RWATCH`/`WVAL`/`GUARD_EA`
  - SPU: `SPU_WATCHEA`/`WATCHLSA`
- **Sync diagnostics:** `MUTEX_WATCH`, lwmutex self-deadlock detection, `PS3_SYNCLOG`.
- **Filesystem logging:** `PS3_FSLOG`, including ENOENT.
- **Audio:** `AUDIO_WAV` dump of the final mix, `AUDIO_PEAK`, `AUDIO_RATE` (underruns).
- **Graphics:**
  - `RSX_ENGINE_PASS_DUMP` (per-pass colour/depth plus `passes.txt`/`draws.txt`)
  - `RSX_ENGINE_SHADER_DUMP`
  - `RSX_TRACE`
  - `PS3RECOMP_VK_DUMP_EVERY` frame dumps
- **Scripted input:** `PAD_SCRIPT` (time- or flip-based), `PAD_FILE` (closed-loop driver).
- **Regression gate:** `tools/regress.py`, milestone-stream goldens.
- **RPCS3 probe:** `tools/rpcs3_probe`, an offline GDB-stub oracle. Never a runtime dependency.

### Gaps that matter here
1. **Linux has no crash handler, hang watchdog or debug console.** They are `#ifdef _WIN32` in `boot_main.cpp`. POSIX thread suspend/context shims already exist, so this is a port, not a rewrite. **M2 work.**
2. **Silent failures:** unresolved NIDs return CELL_OK, unresolved indirect calls log and return, SPU threads without code "complete" instantly, SPU code hands off to the interpreter. A **strict mode** must turn each of these into a fatal, symbolized report. **M2 work.**
3. **Diagnostics are always compiled in:** about 714 `getenv` knobs ship in Release. Needs a compile-time `DEV_DIAGNOSTICS` gate for release builds. M2 design, M7 enforcement.
4. **No determinism:** wall-clock vblank and free-running threads. No savestates, no input recording or replay.
5. **No game-state probes:** current map/room, player HP, inventory, flags, Lua state.
6. **Graphics debugging on Vulkan is thin.** RenderDoc in-app capture, texture identification and frame comparison are D3D12-only or absent.
7. **No A/V-sync measurement, no Bink/FMOD-level tracing.**

---

## 6. Highest-risk obstacles (ranked)

1. **SPURS taskset HLE completeness.** Bullet physics and Edge zlib depend on it.
   - Upstream rates cellSpurs Partial. Its strongest results use Sony's own libsre (LLE), which this project excludes.
   - 6 of the game's 32 SPURS imports have no handler (queue pop, LF-queue push, task exit codes, queue detach).
   - The policy module must be a C reimplementation; no firmware bytes may ship.
   - Wrong behaviour here shows up as physics or collision bugs, not crashes. This is the area most likely to cause subtle gameplay divergence.
2. **FMOD SPU mixer and overlays without an interpreter.**
   - Runtime-loaded DSP and codec overlays must dispatch to lifted code. If any cannot, that is an *explicit exception* to document, not a silent fallback.
   - Upstream has met exactly this case. **[code]** `runtime/spu/spu_channels.c` (`SPU_INTERP_UNLIFTED`) notes that *You Don't Know Jack*'s FMOD mixer "relocates a DSP plugin into local store and calls it", and upstream's only answer is the SPU interpreter.
   - Our 16 FMOD DSP images are already lifted, but at their link address. Relocated execution needs position-independent dispatch: the lifted entry looked up by image fingerprint plus offset from the relocation base. That is new runtime work.
   - Strict mode reports any occurrence as `spu-unlifted-branch`.
   - **Update 2026-10-29 [measured]:** this happened as predicted. The mixer loads its codec (`spu_0008`, linked at LS 0x80) at LS 0x156F0 and branches in. It is now handled without an interpreter by relocatable lifting (ps3recomp patch 0016, `config/spu_relocatable.json`). The plugins are lifted at their link address, every PC-derived value is displaced by the load delta, and dispatch verifies the code bytes at the load address first. All 19 plugin images are registered; only the codec has been exercised so far. Whether the 16 DSP plugins behave correctly when relocated is **[unknown]** until gameplay uses effects.
3. **No title has booted on Linux from upstream ps3recomp.** The per-game boot scaffold compiles on Linux but has never run a game there. Downstream ports reached menus with unmerged patches. Expect a stretch of platform bring-up.
4. **Vulkan renderer maturity.** Gaps in section 3. Upstream performance is synchronous, with a readback every frame. Needs effort roughly in M3 and again in M7.
5. **Silent-failure defaults** (section 5.2). Without strict mode, defects would surface hours of gameplay later.
6. **Floating-point fidelity.** The lifter does not update FPSCR status bits **[code]**. Single-precision rounding and FMA contraction must match for Bullet and gameplay timing. We compile with `-ffp-contract=off`. Whether the game reads FPSCR is **[unknown]**.
7. **No determinism and no savestates.** Full-game testing must rely on save-file checkpoints plus scripted input, with tolerance for timing variance.
8. **Build cost.** 172 MB of C++ means about 30+ minutes for a clean -O1 build on this laptop. The stable chunking keeps rebuilds incremental.
9. **Bink entry-point identification** in a stripped binary **[unknown]**. The recompiled-Bink fallback de-risks it.
10. **Windows bring-up.** Upstream's Windows path is the mature one (clang-cl), but its Vulkan backend has never been built there.

### Explicit exceptions (recorded as they arise)

**E1: FMOD's MP3 codec (firmware file), skipped. [measured]**
- **What the game does:** FMOD Ex's PS3 MultiStream backend loads `/dev_flash/sys/external/flashMP3.pic` from the console firmware while it initialises. That file is an SPU ELF that FMOD runs on its mixer SPU. Without it, FMOD init fails and the game retries it forever.
- **Why we can't supply it:**
  - this project ships no firmware;
  - running the codec would mean executing firmware SPU code loaded at run time, which only an interpreter could do.
- **What we do instead:** the instruction patch in `config/insn_patches.json` (guard `g_dotheroes_skip_flash_mp3`, `port/game_patches.cpp`) turns the loader's failure exit into success.
- **Consequence:** the game cannot play MP3 audio. It has none. All 326 sound files are PCM WAV, and no ID3, LAME, Xing or FSB signature appears in any of its 512 packed archives.
- **If an MP3 decode were attempted anyway:** FMOD's mixer would branch into an empty codec buffer. A development build stops there in strict mode (`spu-unlifted-branch`); a release build would produce silence or wrong audio for that sound.

**E2: ANY2ANY lock-free queues run as PPU2SPU. [code]**
- **The problem:** no implementation of CellSyncLFQueue's ANY2ANY protocol is public; RPCS3's "…2" functions are stubs.
- **What we do:** a SPURS LF queue created ANY2ANY is laid out and run as PPU2SPU, which is fully implemented on both sides. Edge zlib's inflate queue is used exactly that way (PPU pushes, SPU tasks pop).
- **Consequence:** an SPU push or a PPU pop on such a queue would fail visibly, with CELL_SYNC_ERROR_PERM from the title's own library or an unresolved import, rather than misbehave. No such use has been observed.

---

## 7. Recommended approach and first development milestone

### Approach
- Use ps3recomp as a **pinned dependency**, forked for our changes. Keep fixes upstreamable: Windows Vulkan, Linux crash handler, strict mode, SPU overlay dispatch, cellAudio pacing.
- Keep game-specific code in this repository: `runtime/` for game HLE overrides, Bink bridge and game-state probes; `tests/` for scenarios.
- All generated code stays local under `build/gen`.
- Linux first (this machine), with Windows kept compiling from M2 onward via clang-cl/MSVC CI and checked under Wine on this machine.
- Vulkan on both.

### Milestone 2: native executable with diagnostics (first concrete milestone)
**Exit criteria (all measured, not "it compiles"):**

1. `tools/pipeline.py` reproduces the generated sources from the ISO (done).
2. The port builds on Linux into one native executable linking all 24,026 lifted functions, 26 SPU images and the ps3recomp runtime, built with CMake and Ninja.
3. **Strict mode** makes each of these fatal with a symbolized report:
   - unresolved NID
   - unresolved indirect call
   - unknown syscall
   - SPU interpreter entry
   - SPU thread with no lifted image
4. The Linux crash handler, hang watchdog and debug console are ported from the Windows code.
5. The executable boots from `main` through CRT and C++ static init, creates its threads, starts SPURS and FMOD, and reaches its first `cellGcm` flip with zero strict-mode violations. Or the first violation is documented with its cause.
6. A boot-log golden is registered in a regression test.
7. The Windows build compiles and links. A full boot on Windows is not yet required.

M3 then wires Vulkan into the port, implements the gaps the game actually hits, and brings up native audio output and the Bink bridge.
