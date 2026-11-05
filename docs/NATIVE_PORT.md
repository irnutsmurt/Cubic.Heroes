# Native port plan

Status, 2026-11-04: **direction approved by the owner**; the evidence stage is
done (NATIVE_EVIDENCE.md); **R1 is done** (NATIVE_R1.md: both passes, both
scenes, bit-exact; it removes nothing from the game yet); **R2's scope awaits
review** (NATIVE_R1.md, last section). The effort figures are provisional and
are revised from actual progress at each milestone.

This file replaces, for decisions, the four local review documents of
2026-11-04 (architecture-proposal.md, native-render-design.md,
zero-emulation-audit.md and psgl-assessment.md in
build/ab/2026-11-04-opt-review). They remain the background: the address map,
the full mechanism inventory and the reasoning. Where they disagree with this
file, this file holds.

Labels on claims: **[M]** measured (source named), **[C]** read from code or
binary data, **[I]** inferred, **[U]** unknown.

---

## 1. The requirement and the owner's decisions

The destination is a native PC implementation with no runtime hardware
emulation. Performance sets priorities and validation depth, never the
destination: retiring a PS3 mechanism does not depend on an fps target.

Decided by the owner, 2026-11-04:

1. **Graphics.** The game's asp PS3 graphics backend (asp's ps3gcm layer) and
   PPFX's graphics submission are replaced with native Vulkan. The game does
   not use PSGL (FEASIBILITY.md §3, corrected).
2. **The destination bypasses** RSX command generation, the command ring,
   FIFO decoding and reconstructed hardware registers. Making that translation
   path faster does not complete the requirement.
3. **Kept:** the AOT-recompiled game logic, its data layouts and the system
   API compatibility it needs. Translating the player's shader and texture
   assets into native formats (at install or load time) is acceptable.
4. **Retired as their workloads receive native implementations:** the SPU
   execution environment, DMA and mailbox machinery, and PS3-specific
   scheduling.
5. **No permanent FMOD/SPU exception.** Its migration may come later; the
   final architecture accounts for it (priority 6).
6. **Physics:** the existing native physics work is kept. A newer Bullet is
   optional and must justify any behaviour change.

Working rules: a working baseline at every step (the tagged checkpoint and the
existing renderer as the reference path); small reviewable commits; every
behaviour change separate from the replacement that motivates it (section 7);
no broad code-generation campaign while the rendering boundary is settled.

## 2. Priorities

| # | Priority | State, 2026-11-04 |
|---|---|---|
| 1 | Native Vulkan graphics replacing the PS3 graphics path | Evidence stage done; R1 proposed (section 5) |
| 2 | Native implementations of the remaining SPU workloads | Collision and solver native (DOTHEROES_NATIVE_CD, _SOLVER), commands 1-2 still lifted; Edge zlib native with a lifted fallback; FMOD mixer, codecs and DSPs lifted |
| 3 | Native task scheduling and synchronisation, retiring SPURS, DMA, mailboxes and polling | Not started; follows 2 workload by workload |
| 4 | Remaining PS3 platform mechanisms as native services | Partly (file system, save data, input are native-backed HLE); inventory in zero-emulation-audit.md §2.8-2.10 |
| 5 | Physics correctness and further optimisation; Bullet upgrades only if worthwhile | Native collision is not bit-exact (BULLET_NATIVE.md); no upgrade planned |
| 6 | Native audio, including an open-source replacement for the FMOD mixing path | Not started |
| 7 | Native video decoding and playback | Bink runs recompiled; FFmpeg's decoders verified on all 5 movies (M1) |
| 8 | Asset preparation, loading, decompression, persistent shader and pipeline caches | translated shaders and a VkPipelineCache kept on disk between sessions and preloaded at start-up (ps3recomp 0090; the install's data/cache/shaders); shaders are still translated at first sight, not at install |
| 9 | Further x64 code generation and CPU optimisation | Paused at the 2026-11-04 checkpoint (ps3recomp 0094-0096) |
| 10 | Full-game correctness, stability and performance validation throughout | Continuous; the checkpoint below is the current baseline |

Baseline: tag `checkpoint-2026-11-04` (repo 9ea01df, ps3recomp 578c9aa),
executable, symbols and measurements kept locally in build/checkpoint/2026-11-04.

---

## 3. Rendering: destination and boundary

**Destination.** Game code produces native render records (native resource
handles, native formats and state, no RSX method numbers or register words).
A native render thread consumes them and does all Vulkan work. The main game
thread does no Vulkan work. The FIFO walker, the command ring, method decoding
and the reconstructed RSX state are retired when the last producer is native.

**Producers, measured** (NATIVE_EVIDENCE.md §1): every command of a frame is
written by the main thread. By command headers: asp 82-83%, PPFX 16%, game
code calling libgcm 1-2%. The record queue can therefore be single-producer,
single-consumer, provided the remaining scenes confirm it (R0b below).

**Boundaries, by producer:**
- **PPFX**: its device and effect classes (GPUInterfaceDevice, TinyEffect).
  PPFX draws 47 times a frame, on the main thread; 45 of the draws go through
  its generic rect draw func_0078BBA4, which makes five virtual device and
  effect calls a pass (NATIVE_EVIDENCE.md §2). PPFX's effect logic and shaders
  stay above them. Below them, the ~20 libgcm entry points PPFX reaches have
  documented arguments: they are R1's capture point and R3's dual-run oracle.
- **asp**: its ps3gcm backend's virtual methods (0x006C0000-0x006D5FFF,
  reached from asp-common through indirect calls, all on the main thread).
  Mapped in R5.
- **Game code**: the display and depth surface set-up at frame start, the
  draws from 0x000245CC-0x00024CE8, and gcmtextures.cpp (Bink), all through
  libgcm trampolines. Mapped in R6.

**The seams that matter first** [M]:
- PPFX binds the HDR scene target that asp draws into (1280x720 RGBA16F,
  local 0x01AE0000), and then reads it.
- PPFX's last pass writes the display buffer; asp's HUD then draws onto it;
  the flip shows it.
- PPFX's first pass draws into that same scene target before asp renders
  the scene over it (NATIVE_R2.md).
- Every render state a PPFX draw uses was last set by PPFX itself, with the
  value the GPU consumed (NATIVE_R2.md; R1 had read the colour mask as
  inherited from asp).

## 4. One implementation order

The native render core is built only as far as the next producer needs it. No
general core comes first, and nothing is built on the FIFO path to be thrown
away.

| Step | What | Builds (kept) | Removes from the game path |
|---|---|---|---|
| R0 | Evidence stage | the evidence counters (dev) | nothing |
| R0b | The same counters on the remaining scenes | - | nothing |
| **R1** | **One captured PPFX pass rendered natively** (section 5) | capture at PPFX's libgcm calls; the pass description; a headless Vulkan executor; offline shader translation; the comparison tool | nothing |
| **R2** | **The whole PPFX chain, captured and rendered natively offline (done: NATIVE_R2.md)** | the chain capture and description; the executor's chain mode with dependency-driven barriers and the feedback copy; the inventory of chain structures | nothing |
| R3 | PPFX in the game, dual-run | the render thread and record queue; interception at the PPFX device (decided at R3's start with R1-R2 data); a resource registry shared with the existing backend; ordering against the FIFO stream; PPFX's waits | nothing: the FIFO path still renders PPFX as the reference |
| R4 | PPFX native in production | - | PPFX's command generation, ring traffic, decoding and register state: ~6,900 command headers a frame (16%) |
| R5 | asp's backend native | records from asp's backend methods; asp's programs, textures and vertex buffers native | ~82% of the commands |
| R6 | the game's own libgcm users; flips and the vblank as native services | - | the last producers of commands |
| R7 | retire the FIFO walker, the ring, method decoding, the RSX state and the GCM command HLE; release builds exclude them | - | the PS3 command processor |

Each step keeps the previous step's reference path selectable in development
builds. A step is complete only with its record (section 10).

This settles the earlier documents' disagreement (one put the PPFX prototype
first, the other a native rendering core): the core grows with its first
producer, PPFX, in R1-R3.

Provisional effort, revised at every milestone. R1 was estimated at 2-3
weeks and took one working session; R2, estimated at 2-4 days, took one
long session (most of it in capture runs, comparisons and coverage, not in
the executor). R3, from that: 1-2 weeks -- the recorder and its state block,
the shared resource registry, the render thread and its ordering against
the walker are new code in the game, unlike R1-R2's offline tools; R4 is
re-estimated after R3, R5-R7 after R4. The earlier 5-9 months for the
whole graphics replacement stays provisional until then.

## 5. Milestones R1 and R2 (done); R3 proposed

R2 (the whole chain, offline) is done: results, coverage and R3's proposed
scope -- where PPFX produces native render records from its own libgcm
calls, without the old renderer's reconstructed state -- are in
NATIVE_R2.md. R1's plan as approved follows.

### Milestone R1: one captured PPFX pass, rendered natively (done)

Approved and done on 2026-11-04; the results, the corrections the owner asked
for on approval (capturing what the GPU consumes, format-specific
acceptance, synchronisation validation, honest timing) and R2's proposed
scope are in NATIVE_R1.md. The plan as approved:

**Goal.** Show that a real PPFX pass, described at PPFX's own boundary,
renders correctly through native Vulkan code with its own resources, and
matches the existing path's output for the same frame.

**The pass.** PPFX pass 3, the first downsample: one draw from the scene
target (1280x720 RGBA16F, rendered by asp in the same frame) into a 640x360
RGBA16F target, with one texture and a fragment program whose constants PPFX
patches each pass (NATIVE_EVIDENCE.md §2). The second case is pass 48, the
final composite: two inputs, written into the RGBA8 display buffer.

**Work: the minimum infrastructure, all of it kept for R2-R4.**
1. **Capture point.** The lifter gains `--entry-observers <file>`: at an
   observed function's entry, the generated code calls a host function, then
   runs the lifted body unchanged. A separate dev file
   (config/dev_capture_observers.json) lists the ~20 libgcm entry points PPFX
   reaches, and only a dev lift uses it. The production lift and
   config/insn_patches.json are unchanged.
2. **Capture.** For calls whose caller is in PPFX, on one chosen frame and
   pass (`DOTHEROES_CAPTURE_PPFX=<s>:<pass>`), the observers decode libgcm's
   arguments: surface, texture, sampler, programs, constants, vertex arrays,
   draw and state enables. The fragment program is copied when it is bound,
   after its patch. The walker reads back the input image when it reaches the
   pass, and the output image after the GPU has run it.
3. **The pass description (v0).** In native terms only: images (size,
   VkFormat), samplers, shaders (SPIR-V, keeping the source microcode for
   provenance), constants, vertex streams, the draw, blend, depth and raster
   state, and bindings. No RSX method numbers or register words.
4. **Executor.** A headless tool with its own Vulkan device. It creates the
   images, uploads the inputs, translates the programs with the toolkit's
   existing fragment and vertex program translators, draws, and reads back.
   It links none of the walker, the RSX state or the GCM HLE.
5. **Comparison.** Per pixel, at the target's precision: maximum and mean
   error, mismatching pixels and PSNR, with both images and a difference
   image.

**Exit criteria.**
- Passes 3 and 48, in the river fight and in exploration: output identical,
  or within one half-float ulp with the cause explained.
- The executor's link map has no walker, RSX state or GCM HLE symbol.
- The pass description has no RSX method number or register word (review).
- With the capture off, nothing changes: the production build is unchanged,
  and the dev capture build passes the smoke test.
- Recorded: the executor's GPU time for each pass, against that pass's time
  on the existing path (7-22 us for pass 3, NATIVE_EVIDENCE.md §4).

**What R1 removes:** nothing from the game path. **What remains:** all of it.
It proves the description, the executor and the shader translation on a real
pass, and gives R3 its oracle.

**Dependencies:** R0 (done); the toolkit's program translators; this approval.

**Effort, provisional: 2-3 weeks.**
- Observers and the dev lift: 2 days.
- Capture: 3-4 days.
- The pass description: 1 day.
- Executor: 3-4 days.
- Comparison and the four results: 1-2 days.

No gameplay from the owner is needed: both scenes are in the test scenarios.

## 6. The synchronisation model

Three events, never merged:
- **C, consumption**: the render thread has taken every record up to
  sequence S.
- **G, GPU completion**: the timeline semaphore of the submission holding S
  has signalled.
- **P, presentation**: the frame was handed to the swapchain (the present
  returned).

The game's waits (NATIVE_EVIDENCE.md §3) and their native mapping:

| Mechanism | What the game relies on | Native |
|---|---|---|
| flip request | the frame is shown | a present record; flip status, time and handler at P |
| WaitFlip (RSX acquire of label 0x10) | a display buffer on screen is not drawn over | GPU queue order: the next frame's writes to a display buffer follow the previous present's copy. Kept as an explicit ordering point, with a development assertion. Not a no-op: today it never blocks only because the walker stops at the flip word |
| flip-status spin | at most one flip outstanding (pacing) | status DONE at P, as today |
| Finish | all earlier work complete before memory is rebound | wait for G of everything recorded before. Not weakened (one call at start-up, none in play so far) |
| back-end and texture-read labels | the GPU is done with memory | released at G (behaviour change B2: today at decode) |
| front-end semaphores, the reference word | the RSX has passed a point | at C |
| occlusion reports | visibility | unchanged ("visible") until B1 |

Records that copy guest data at call time need no wait: constants, inline
uploads, the patched fragment program, small vertex streams. Data referenced
by address is read by the render thread. Any sync value the game can see is
published only after the render thread has consumed what came before it, so
the game's own waits still bind the render thread, as they would a slow RSX.

## 7. Behaviour changes, kept separate

None of these rides along with a replacement step. Each gets its own switch,
its own comparison against the reference path (and against RPCS3 where the
game's look is in question), and its own commit.

| # | Change | Why it is a behaviour change | Check |
|---|---|---|---|
| B1 | real occlusion queries for ZPASS reports | today every query reads "visible"; real counts can hide flares or glare | side by side with RPCS3, before and after, on scenes with flares |
| B2 | back-end and texture-read labels at GPU completion | today released at decode; a later release can change pacing | frame times and the label waits' durations, A/B |
| B3 | an RSX-side WaitFlip that really waits | today it never blocks | frame-pacing A/B; no deadlock across the scenario suite |
| B4 | a presentation mode other than FIFO | frames past the refresh, tearing | the owner's choice; pacing A/B |
| B5 | internal resolution above 720p | changes the image; every readback must scale | visual review; readback tests |
| B6 | the depth and colour snapshots replaced by direct sampling | must be exact; changes GPU timing | per-pixel comparison against the snapshot path |
| B7 | half-float rounding differences in the native shaders | tiny colour differences | the R1 comparison tolerances |

## 8. Release paths: disposition

What exists and what ran is in NATIVE_EVIDENCE.md §5. The proposal:

1. **A release build profile** (`DOTHEROES_RELEASE=ON`) compiles out the
   execution paths the requirement forbids:
   - the SPU interpreter (spu_interp.c), the SPU microstepper and
     `SPU_INTERP_UNLIFTED`;
   - the D3D12, live-draw and null backends;
   - other titles' image-id paths;
   - the development-only switches (`DEV_DIAGNOSTICS`).
2. **No silent success.** In release builds, one call,
   `ps3_unsupported(kind, ...)`, replaces every strict-off fallback:
   - Execution gaps are fatal: an unresolved NID, an unresolved indirect call,
     an unlifted function, an unknown syscall, an SPU thread without code, an
     SPU unlifted branch. The call writes a diagnostic report, shows a
     message and exits.
   - Rendering degradations are reported and counted: built-in shader
     programs, a FIFO resync, an acquire consumed unsatisfied. Each is shown
     once on screen and recorded in the report.
3. **Proof the paths are gone.**
   - A forbidden-symbol list (config/release_forbidden_symbols.txt) is checked
     against the release executable's symbols at build time; the build fails
     if one is present.
   - Every run writes a release-path summary to its log at exit.
4. **Temporary, visible fallbacks.** The lifted physics and inflate routines
   that run when a native one declines are correct but emulated. They are
   allowed until priority 2 replaces them, each with a counter in the
   summary; never silent.
5. **cellRescSetWaitFlip** (imported, no handler, on a path this game does not
   take): register a real implementation, or let it fail visibly.
6. **Development builds** keep every reference path, selectable, for
   comparison.

## 9. Findings and hypotheses

Measured after ps3recomp 0096 (NATIVE_EVIDENCE.md, and the optimisation
review's A/Bs):

| # | Finding |
|---|---|
| F1 | main-thread busy per frame: combat 6.15-6.22 ms, exploration 5.56-5.71 ms |
| F2 | GPU per frame: combat 2.41 ms (1.43 in passes), exploration 3.81 ms (3.02 in passes) |
| F3 | a pass-to-pass transition costs 7-8 us of GPU time; 46 a frame, 0.31-0.37 ms |
| F4 | the depth snapshot costs 0.33-0.40 ms of GPU time a frame |
| F5 | the walker waits for the GPU 2.07 ms a frame in exploration (1.73 at the present), ~0.01 ms in combat |
| F6 | the main thread waits for the flip 0.89 ms a frame (combat) and 1.02 ms (exploration) |
| F7 | one thread produces every command; PPFX is 47 one-draw passes and 16% of the command headers |

| # | Hypothesis | Test |
|---|---|---|
| H1 | narrower barriers save at most 0.3-0.4 ms of GPU time a frame, less in practice because PPFX's passes depend on each other | after R4, per-pass timestamps with dependency-tracked barriers |
| H2 | sampling depth directly removes most of the 0.33-0.40 ms snapshot | B6 |
| H3 | a render thread with frames in flight removes exploration's 1.73 ms present wait | R3-R4 timing |
| H4 | native PPFX saves walker time in proportion to its 16% of commands, plus the main thread's PPFX libgcm stores | R4 A/B |
| H5 | "53 full GPU drains" as the main GPU cost | refuted as stated, by F3 |

## 10. The milestone record

Every milestone's record (in MILESTONES.md, with evidence in the run folders)
states:
- **Dependencies**: what had to be done first.
- **Removed**: which PS3 mechanism no longer runs, shown by a counter, not
  asserted.
- **Remains**: what still runs on the PS3 path.
- **Correctness evidence**: comparisons, scenario results, soak runs.
- **Measured performance**: main-thread busy time, walker time, GPU time per
  pass, and frame mean and p99 by class, against the baseline; real audio,
  normal scheduling.
- **Commits**: small and reviewable, with the baseline build and its tag
  kept.

## 11. What the owner can provide

Nothing for R1. For R0b and R2, coverage of scenes the test bot does not reach
well. For each, a copy of a save near the scene and a sentence on what to do
there:
1. inside the first dungeon, at a dark room next to a bright one, to show the
   exposure adapting;
2. the first boss fight, just before it starts;
3. any moment with visible blur, afterimage or depth of field (a magic attack,
   a cut-in, a menu over the game);
4. a scene that plays one of the game's movies;
5. the dot editor, and a save or load.

Copies only: the original saves stay where they are.
