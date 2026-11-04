# Native rendering: the evidence stage (2026-11-04)

What the PS3 graphics path actually does in this game, measured before any
replacement code: who produces the frame's commands and on which thread, the
entry points and resources of one representative PPFX pass, the
synchronisation the game relies on, what the GPU spends its time on, and which
fallback paths execute. NATIVE_PORT.md draws the plan from it.

Labels: **[M]** measured (run named), **[C]** read from code or the lifted
output, **[I]** inferred, **[U]** unknown.

## Method and runs

Build: the 2026-11-04 checkpoint (repo 9ea01df, ps3recomp 578c9aa) plus the
evidence counters below, all off unless their switches are set. Real audio,
normal scheduling, uncapped (DIAG_VBLANK_MS=0.5), can't-die, warm shader
caches, PS3_STRICT=1. Runs and scripts: build/ab/2026-11-04-evidence (local).

| Run | Scene | What it records |
|---|---|---|
| ev-river-gpu | river fight, trace 70-140 s | per-pass GPU timestamps, every HLE call and wait, acquires by label |
| ev-field-gpu | exploration after the first dungeon, 70-122 s recorded | the same |
| ev-river-attrib, ev2-river-attrib | river fight, window 95-125 s (combat) | every guest store into RSX-visible memory by writer and thread; every indirect call into asp and PPFX; three frames traced command by command with each command's writer, thread and caller; ev2 also traces boot to 68 s |
| ev-field-attrib, ev2-field-attrib | exploration, window 120-150 s | the same |

The counters (ps3recomp, dev only):
- `PS3_ATTRIB_ST`, `PS3_ATTRIB_CALL`, `PS3_ATTRIB_SHADOW` (ppu_loader.cpp):
  stores into an address range counted by writer and thread; indirect calls
  into a code range counted by target, caller and thread; the last writer of
  every word kept. All stores go out of line while armed (the fight ran at
  ~41 fps), so these runs are for attribution, never for timing.
- `RSX_FRAME_TRACE` (cellGcmSys.c): every command header the walker reads in N
  frames, with its data words and, with the shadow, the guest function,
  thread and caller (lr) that stored it.
- `VK_GPU_PASSES` (rsx_vulkan_backend.c): a timestamp before and after every
  render pass of the draw engine.
- `GCM_SEMA_STATS` (cellGcmSys.c): RSX acquires by label offset, and how many
  found their value already there.
- Always on: a `[release-path]` line when the SPU microstepper runs, when
  built-in shader programs stand in for the game's, when an acquire is
  consumed unsatisfied; a `[gcm]` line for SPU DMA into RSX-visible memory.

Coverage limits: two scenes (river fight; exploration). Not covered: dungeons,
bosses, menus, the dot editor, movies, save and load. Vector stores (`stvx`)
bypass the store hook; no traced command header lacked a recorded writer, so
none of the commands was written that way [M].

## 1. Producers: one thread writes every command

| | combat (923 frames) | exploration (838 frames) |
|---|---|---|
| guest stores into RSX-visible IO memory (0x11100000-0x13400000) | 156,500 a frame, **all thread 1 (main)** | 129,900-138,200 a frame (two runs), all thread 1 |
| indirect calls into asp or PPFX | all thread 1 | all thread 1 |
| SPU DMA into RSX-visible memory | 0 | 0 |
| traced command headers without a recorded writer (3 frames) | 0 | 0 |

[M, ev-river-attrib, ev-field-attrib.] No other PPU thread, no SPU and no
runtime thread produces commands. The GCM context, the command ring and every
render target are touched by the main thread only, in program order.
**A single-producer queue is supported by the evidence** for these scenes; the
remaining scenes are to be checked with the same counters (NATIVE_PORT.md
R0b).

Command headers of one frame by producer (the writer, or for a libgcm
writer its caller) [M, ev2-river-attrib and ev2-field-attrib; identical in
all three traced frames of each]:

| Producer | Combat (42,196) | Exploration (43,400) | Notes |
|---|---|---|---|
| asp (backend 0x006C0000-0x006D5FFF, and its memcpy of prebuilt state blocks from 0x006D0C20) | 34,576 (81.9%) | 36,164 (83.3%) | scene, shadow and reflection passes, the HUD |
| PPFX through libgcm | 6,905 (16.4%) | 6,905 (15.9%) | 5,614 called directly from PPFX; 1,291 from libgcm wrappers PPFX calls [C, call graph] |
| game code through libgcm | 715 (1.7%) | 331 (0.8%) | the display buffer and depth surface at frame start (0x000220E8, the frame loop 0x00029D64); draws from 0x000245CC-0x00024CE8 inside the scene pass |

Correction to the 2026-11-04 assessment ("game code has no direct libgcm
calls"): game code reaches libgcm through trampoline stubs at
0x00981000-0x00988000 [C, call graph]: 0x000245CC-0x00024CE8 (vertex arrays,
indexed draws), 0x0045BB6C (gcmtextures.cpp), 0x000220E8, 0x000294E4.

## 2. The frame and one PPFX pass

Each traced frame has the same 50 render-target passes [M, ev-river-attrib
and ev-field-attrib, frames 0-2]:

| Passes | Producer | Targets |
|---|---|---|
| 0-1 | asp | shadow 1024x1024 (189 draws), reflection or shadow 512x512 (206 draws) |
| 2 | asp, into a target **PPFX binds** | the scene, 1280x720 RGBA16F at local 0x01AE0000, depth 0x00730000 (194 draws) |
| 3-48 | PPFX, one draw each | ~20 targets, 640x360 to 80x45 RGBA16F in local 0x02260000-0x04B6xxxx; pass 11 writes the scene target while sampling it; pass 48 writes the display buffer |
| 49 | asp | the HUD onto the display buffer (35 draws) |

PPFX's device binds the scene target before asp draws into it: the surface
set that precedes pass 2 comes from func_007B7390 (lr 0x007B7424,
GPUInterfaceDevice) [M]. **PPFX owns asp's render target**; this is the main
seam between a native PPFX and a FIFO asp.

### The representative pass: PPFX pass 3, the first downsample

| | |
|---|---|
| Entry | an effect function (one of 29 variants at 0x0078BEFC-0x0079FE0C [C]) calls the generic rect draw **func_0078BBA4** (45 of PPFX's 47 draws a frame; the other two come from 0x00788F50 and 0x0078A6D8 [M]), which makes 5 virtual calls per pass: the draw **0x007B6C18** (-> libgcm draw-arrays 0x006A1E28) and the effect calls **0x007BCBFC, 0x007BCC8C, 0x007BCBB4, 0x007BCC44** (-> TinyEffect 0x007CBEA4, 0x007CBEBC, 0x007CBEC0, 0x007D1D64; 0x007D1D64 -> 0x007D1354, the program, texture and constant set-up) |
| Rate | each device and effect method 47 a frame, combat and exploration alike [M, PS3_ATTRIB_CALL] |
| Thread | main (1) only [M] |
| libgcm entry points it reaches [M, frame trace] | surface 0x0069F944, viewport 0x0069E7EC, scissor 0x006A674C, vertex constants 0x006A39EC, vertex program 0x006A4E50, inline transfer 0x0069FC38, texture 0x0069F2B8 and 0x006A7834, fragment program 0x0069E99C and 0x0069E6E0, vertex arrays 0x0069F19C, vertex cache 0x006A7BA4 and 0x006A7CF8, draw arrays 0x006A1E28 |
| Output | 640x360 RGBA16F at local 0x02DD0000, no depth |
| Input | texture 0: the scene target 0x01AE0000, 1280x720 RGBA16F, bilinear, rendered by asp's pass 2 in the same frame |
| Programs | a 3-instruction vertex program uploaded inline, constants c[0x1D1-0x1D3]; the fragment program at local 0x018F0380, **its constants patched in place** by a 16-byte inline transfer just before it is bound |
| Geometry | 3 vertices (float3 position, float2 texcoord) in main memory at IO 0x01A021A0 and 0x01A0D7C0, one triangle-strip draw |
| State | set by PPFX before its first pass: blend, depth test, cull, alpha test and stencil off. **Inherited from asp:** the colour mask (all channels) and the rest of the GCM context's state |
| Also per frame | 96 calls of cellGcmAddressToOffset from TinyEffect (0x007CCEE8, 0x007CCD04) [M] |

Shared state the pass depends on [M]: the GCM context and command ring (shared
with asp, one thread); asp's scene target (input); the implicit GCM render
state left by asp; the fragment program's local-memory copy, which PPFX
rewrites every pass. Cross-frame PPFX resources (feedback, exposure) are not
yet identified [U]. No PPFX pass samples the depth buffer in these scenes [M].

## 3. Synchronisation: what the game waits on

Three events, kept distinct:
- **consumption (C)**: the RSX side has taken the commands (today: the walker
  decoded them);
- **GPU completion (G)**: the GPU has finished the work (a Vulkan fence);
- **presentation (P)**: the frame was handed to the display (the present
  returned).

| Mechanism | Rate [M] | What the game relies on | Today [C, M] | Native requirement |
|---|---|---|---|---|
| Flip request: libgcm cellGcmSetFlip (0x0069DD60) -> HLE _cellGcmSetFlipCommand | 1 a frame | the frame is shown | flip word in the ring; the walker stops its drain there; the present thread copies the display buffer to the swapchain and presents | a present record; flip status, flip time and the handler at P |
| WaitFlip: libgcm 0x0069E58C, an RSX acquire of label offset 0x10 == 0 | 1 a frame | later commands do not run until the flip has happened, so a buffer on screen is not drawn over | **never blocks**: all 101,955 acquires in six runs were satisfied on arrival, and 0x10 is the only label ever acquired; nothing writes it. The ordering comes from the walker stopping at the flip word instead | the next frame's writes to a display buffer ordered after the previous present's copy (GPU queue order); stated and asserted, **not removed** |
| Flip status spin: GetFlipStatus at 0x29CC0 with 200 us sleeps, then ResetFlipStatus | 5.4 polls a frame, 0.89 ms of waiting (combat); 6.1 polls, 1.02 ms (exploration) | pacing: at most one flip outstanding | status DONE when the present returns (P) | the same: P |
| Finish: libgcm 0x006A9ED8 (set reference, flush, poll the ref word) | 1 call from boot to 68 s (at 41 s, in both ev2 runs); 0 in 65 s of combat and 42 s of exploration [M, HLE calls from 0x006A9F0C] | callers [C]: PPFX set-up and GPUPostEffect, GPUInterfaceDevice tile and zcull binding (0x007B9664, 0x007BA18C), the game's display set-up 0x000294E4: everything before has finished before memory is rebound or reused | the ref published when the walker decodes it (C) | wait for G of everything recorded before: not weakened; its cost is irrelevant at this rate |
| Back-end and texture-read labels (0x1D70, 0x1D74) | 0 in every traced frame [M] | a CPU wait for the GPU to have finished with memory (the Bink texture fence func_0045AC78) [C, not exercised] | released at decode (C), before the GPU ran the draws [C rsx_commands.c] | released at G, their PS3 meaning (behaviour change B2) |
| Occlusion reports (GET_REPORT type 1) and cellGcmGetReport from 0x003AA9D8 | 11 reports and 1 read a frame | visibility of flares and glare [I] | always "visible" (0xFFFF), written at decode [C cellGcmSys.c] | unchanged at first; real queries are behaviour change B1 |
| Vblank count (cellGcmGetVBlankCount at 0x29BF4) | 1 a frame | the frame-rate cap (insn-patched, DOTHEROES_FPS) | the port's vblank clock | kept |

Not seen in either window: front-end label waits by the CPU, SPU-written
command blocks, ring-full waits.

## 4. GPU time: what the passes and the barriers cost

Per steady frame (one submission a frame; 24 combat and 38 exploration frames
dumped) [M, ev-river-gpu, ev-field-gpu]:

| | combat | exploration |
|---|---|---|
| GPU time of the frame's submission | 2.41 ms | 3.81 ms |
| in render passes | 1.43 ms | 3.02 ms |
| between passes | 0.98 ms | 0.79 ms |
| of which: the depth snapshot (a copy before the pass after it) | 0.40 ms | 0.33 ms |
| of which: pass-to-pass transitions (full barrier, next pass begins) | 46 a frame, 7-8 us each, **0.37 ms** | 46, ~6.7 us, **0.31 ms** |
| of which: colour snapshot, clears | 0.06, 0.09 ms | 0.06, 0.05 ms |
| PPFX-sized passes (640x360 to 80x45) | 43 passes, 0.36 ms in them + 0.34 ms between | 43, 0.31 + 0.29 ms |
| the 1280x720 passes (scene, composites, HUD) | 7.2 passes, 0.54 ms | 7.1, 1.66 ms |
| the 512x512 and 1024x1024 passes | 0.42 + 0.10 ms | 0.73 + 0.29 ms |
| the walker waiting for the GPU | ~0.01 ms a frame | **2.07 ms** (1.73 at the present, 0.34 in flight) |

Findings:
- "53 full GPU drains a frame" was a barrier count. As GPU time, each
  transition costs 7-8 us, 0.31-0.37 ms a frame (13-15% of combat GPU time,
  8% of exploration). Most PPFX transitions are true dependencies (each pass
  samples the one before), which need a barrier anyway; narrowing them can
  save at most that 0.3-0.4 ms [M bound, I saving].
- The depth snapshot is the largest single gap (0.33-0.40 ms); its cause is
  [U].
- In exploration the render side's limit is serialisation: the walker waits
  1.73 ms a frame at the present for the GPU [M]. That, not barrier count, is
  what a render thread with frames in flight addresses [I].
- The GPU is 34% (combat) and 50% (exploration) of a 7-7.6 ms frame [M].

## 5. Release paths: what exists and what ran

| Path | Exists | Ran in the 7 runs of 2026-11-04 (3 smoke, 4 evidence; strict on) |
|---|---|---|
| SPU interpreter (spu_interp.c) | linked | no: fatal under strict, none [M] |
| SPU microstepper (spu_smc_microstep) | linked; no strict check | **no** `[release-path]` line [M] |
| SPU_INTERP_UNLIFTED | opt-in switch | not set |
| unresolved NID -> CELL_OK (strict off) | yes | no: 0 unresolved [M]. The game imports cellRescSetWaitFlip (0x0D3C22CE), which has no handler; its call site is on the cellResc flip path this game does not take (0 calls, no cellResc set-up in the logs) [M, C] |
| unresolved indirect call, unlifted function, SPU thread without code, SPU unlifted branch | yes, silent with strict off | no [M] |
| ACQUIRE consumed unsatisfied | default behaviour | no: every acquire was satisfied [M] |
| built-in shader programs for a missing program | yes | no [M] |
| FIFO resync (drops commands) | yes | no [M] |
| SPU DMA into RSX-visible memory | watched | none [M] |
| lifted physics or inflate when native declines | yes, by design | collision: "lifted 0 of 0" [M]; solver and inflate fallbacks have no uniform counter [U] |
| D3D12, live-draw, null backends; other titles' image-id paths | compiled in | not used (no D3D12 import) [M]; the LBP Bink probe prints [M, 2026-11-04 audit] |

"Did not run" covers the two scenes only. The disposition is in
NATIVE_PORT.md section 8.
