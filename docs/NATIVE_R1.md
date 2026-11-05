# R1: one captured PPFX pass, rendered natively (2026-11-04)

Milestone R1 of NATIVE_PORT.md, approved by the owner on 2026-11-04. **R1
removes nothing from the game's active graphics path**: the game renders
exactly as before. R1 proves that two real PPFX passes, captured from the
running game, render identically through a standalone native Vulkan
executor that consumes only a native pass description. It establishes no
in-game frame-rate gain.

Labels as elsewhere: **[M]** measured, **[C]** from code, **[I]** inferred.

## Result

| Scene, frame | Pass | Output | Pixels | Bit-exact | From a poisoned target | Differing pixels R/G/B/A | Max / mean error | Programs (HLSL, SPIR-V) | Validation errors / warnings |
|---|---|---|---|---|---|---|---|---|---|
| river combat, frame 12900 | 3, first downsample | 640x360 R16G16B16A16_SFLOAT | 230,400 | yes | yes | 0 / 0 / 0 / 0 | 0 / 0 | identical | 0 / 0 |
| river combat, frame 12900 | 48, final composite | 1280x720 R8G8B8A8_UNORM | 921,600 | yes | yes | 0 / 0 / 0 / 0 | 0 / 0 | identical | 0 / 0 |
| exploration, frame 13672 | 3 | 640x360 R16G16B16A16_SFLOAT | 230,400 | yes | yes | 0 / 0 / 0 / 0 | 0 / 0 | identical | 0 / 0 |
| exploration, frame 13672 | 48 | 1280x720 R8G8B8A8_UNORM | 921,600 | yes | yes | 0 / 0 / 0 / 0 | 0 / 0 | identical | 0 / 0 |

[M; build/r1/captures/{river1,field1}/{pass3,pass48}/compare.json,
compare_poisoned.json, replay.json.] The images (reference, native,
difference, and the inputs) are kept locally in those directories, not in
Git.

Comparison at each format's own precision (tools/r1_compare.py): half floats
by their 16 bits (also as ulps and absolute values), unorm8 by their 8 bits.
No tolerance was needed, and none is set.

"From a poisoned target": the executor's target started as 0xA5 bytes
instead of its captured content (`r1_replay --poison`). The output still
matched everywhere, so the native draw produced every pixel itself. (Without
this, only the pixels the pass changed would have been tested: 54,839 of
230,400 in the river's pass 3, because last frame's identical downsample was
already there.)

## What was built (all kept for R2-R4)

| Part | Where | Role |
|---|---|---|
| Entry observers | ps3recomp 0101 (lifter `--entry-observers`); config/dev_capture_observers.json; port/native/render/r1_observe.cpp; tools/r1_dev_lift.sh | the boundary capture: the 29 libgcm entry points PPFX reaches log each call (arguments, caller, thread, ring position, the bytes behind pointer arguments). A development lift only; the production lift and config/insn_patches.json are unchanged; the dev lift differs from it only by the observers' 232 lines [M] |
| Consumption capture | ps3recomp 0102-0104 (rsx_draw_engine.c, rsx_vulkan_backend.c) | a named draw's inputs, state and output as the GPU used them, written as a native pass description |
| Pass description v0 | desc.json and its files | Vulkan terms only: images (size, VkFormat, levels, layers, swizzle), samplers, SPIR-V (and the source microcode with its conversion parameters, for provenance), constant blocks, vertex input, draw, rasterisation, depth-stencil, blend, viewport, scissor. Guest addresses appear only under "provenance" |
| Executor | port/native/render/r1_replay.c, r1_json.c (target r1_replay) | renders a description with its own instance, device, images (proper layouts), buffers, descriptor set, render pass and pipeline; validation, poison, timing |
| Checks | tools/r1_compare.py, r1_boundary_check.py, r1_check_deps.py; tools/r1_capture.sh | the pixel comparison, the boundary cross-check, the dependency check, the capture run |

Production code changed in two places, each a refactor with no change in
what is built [M]: the backend's pipeline create info filled by one function
(0102), and the engine's program HLSL made by one function (0103). A river
fight with the shader and pipeline caches of 2026-11-03 translated none of
its 284 programs (the cache key is the whole HLSL text) and made all 142
pipelines as driver cache hits (at most 0.29 ms each); neither cache gained
an entry. The main build at these patches passed a fight smoke.

## Capturing exactly what the GPU consumes

**The queued program patch.** PPFX sets a pass's fragment-program constants
by an inline transfer queued in the command stream just before it binds the
program (0x0069FC38, called from libgcm 0x006A9D54). At the bind call, the
program's bytes in memory are not yet patched, and the call names an RSX
local-memory offset (0x018F03A0), not a guest address: reading guest memory
there gives unrelated bytes [M, boundary.txt]. So the capture takes the
program where the GPU consumes it: the engine's copy at the draw, after the
walker has applied every queued write. The boundary log is then checked
against it: each transfer's queued words equal the consumed program's bytes
at that offset (one transfer in pass 3, nine in pass 48, all equal) [M].

**Constants.** The engine hoists a fragment program's inline constants into
a constant block (pass 3: 1 constant, pass 48: 18); the capture records that
block as bound. The vertex constants PPFX sets (3 registers in pass 3, 2 in
pass 48) equal the consumed block's, the last write winning: pass 3 sets
c[0x1D3] twice, (1, -1, 0, 0) and then (1, 1, 0, 0), and the GPU used the
second [M].

**Inherited state.** The capture records the effective pipeline state, not
only what PPFX set. Neither pass sets the colour mask in its own calls: its
write mask 15 (all channels) is inherited [M, boundary check: no colour-mask
call in the pass]. *Corrected by R2 (NATIVE_R2.md):* R1 read this as asp's;
the whole frame's log shows the last colour-mask call before every PPFX draw
is PPFX's own, at its first pass (drawn before asp's scene), and its value
is what the GPU consumed. Pass 3 also never sets its texture unit's address
or filter modes: mirrored-repeat, mirrored-repeat, clamp-to-edge and linear
filtering are inherited from earlier calls [M]. A native PPFX device (R3)
must set these explicitly.

**The same frame, the right point.** Before the named draw the backend
submits and waits for everything recorded so far, then copies each bound
texture and the target; after it, it submits and waits again and copies the
target: the reference output. Both copies are on the GPU timeline right
around the draw, in the frame it belongs to. Both passes of a scene come
from one frame.

**The programs.** The executor makes the HLSL again from the captured
microcode and conversion parameters with the toolkit's decompilers (the
engine's recipe), and SPIR-V with glslang. Both are byte-identical to the
capture's in all four cases, and the capture's HLSL hashes equal those of the
pipeline that drew (programs_match_pipeline) [M].

## The boundary cross-check

tools/r1_boundary_check.py finds the captured draw in the observers' log by
where its commands went: the draw-arrays call whose ring position is 0x20
before the walker's offset at the draw (the END word). The walker's frame
number and the log's differ by a constant per run (93 and 99 here: the two
count flips from different starts), and the ring repeats every three frames,
so candidates a lap apart are each checked. Checks, call by call, in order:
surface, textures, the fragment program's offset, every inline transfer's
bytes, the vertex constants, the draw.

| Scene | Candidates by ring position | Frames where every check of both passes holds |
|---|---|---|
| river combat | 12804, 12807, 12810 | **12807** only (pass 48's display buffer, 0x398000, alternates with 0x0 each frame) |
| exploration | 13573 | **13573** |

In each identified frame every check holds for both passes [M,
boundary_frames.json, boundary_check.json].

## Validation

`r1_replay --validate`: the Khronos validation layer (Vulkan SDK
1.4.363.0) with synchronisation validation enabled, every warning and error
logged. **0 errors and 0 warnings in all eight runs** (four captures, with
and without the poisoned target) [M]. Negative control: `--hazard-selftest`
drops the dependency between the draw and the readback copy; the layer
reports it (1 error, a write-after-write hazard on the layout transition),
so the layer was active and looking [M].

## The executor's independence

tools/r1_check_deps.py on build/r1/bld [M, r1_deps.json]:
- link inputs: r1_replay.c, r1_json.c, rsx_fp_decompiler.c,
  rsx_vp_decompiler.c, rsx_shader_spirv.cpp; libraries libglslang,
  libSPIRV, libglslang-default-resource-limits; nothing else;
- external symbols of every object (their COFF symbol tables): none of the
  runtime's (cellGcm, gcm_, rsx_dsp_, rsx_process, rsx_state,
  rsx_draw_engine, vk_eng_, vm_, ppu_, spu_, ps3_hle, lv2_ ...);
- imports: KERNEL32, ntdll and the C runtime's DLLs; Vulkan is loaded at run
  time (vulkan-1.dll), with its own instance and device;
- none of the runtime's log strings in the binary.
No FIFO walker, RSX register reconstruction or GCM HLE is linked.

## GPU timing

**Method.** In the executor: a timestamp at TOP_OF_PIPE after a barrier
that waits for the previous repetition, just before the render pass begins,
and one at BOTTOM_OF_PIPE after it ends; 10 command buffers of 50 untimed
warm-up and 200 timed repetitions (2,000 timed). Uploads, readback and disk
I/O are outside the timed command buffers. In the game the same bracket
(VK_GPU_PASSES: after the pass's full barrier, before it begins; after it
ends), over the steady frames dumped in the evidence runs. Timestamp period
1 ns; the values move in steps of about 1.02 us. RTX 3070.

| Pass | Native median (mean, p90) | Existing path in the game, median (range) |
|---|---|---|
| 3, river | 24.6 us (25.3, 25.6) | combat 23.6 us (20.5-32.8, 24 frames) |
| 3, exploration | 24.6 us (25.5, 25.6) | exploration 20.5 us (18.4-31.7, 32 frames) |
| 48, river | 29.7 us (30.2, 30.7) | combat 24.6 us (21.4-33.8) |
| 48, exploration | 29.7 us (30.2, 30.7) | exploration 22.5 us (19.5-35.8) |

**A difference remains, unexplained.** Native pass 3 is within 1-4 us of the
game's, native pass 48 5-7 us slower. Ruled out [M]:
- GPU clocks: 1920 MHz, P0, 94% busy during a 60,000-repetition run (a
  read-only nvidia-smi query; no setting changed), same result;
- warm-up: 50, 2,000 and 20,000 repetitions give the same median;
- the measurement bracket: the barrier waits before the first timestamp;
- image layouts: GENERAL everywhere, as the game keeps them
  (`--general-layouts`), same result;
- input images created attachment-capable (`--attachment-inputs`), same;
- buffer memory: both take the first host-visible coherent type.
Still possible [I]: the game's inputs sit in the GPU's cache from the pass
that wrote them just before, and the driver's compression state of targets
it renders every frame. R1 makes no performance claim: these are single
passes of tens of microseconds, and the existing path's per-pass cost is
dominated by its surroundings (NATIVE_EVIDENCE.md §4).

## Assumptions and limits

- One frame per scene, two scenes. The passes' structure was the same in
  every traced frame of the evidence stage.
- The observers slow the main thread while they log (about 9 frames a
  second for two seconds); they change no result, and the frame is matched
  by ring position, not by time.
- The boundary log sees PPFX's patches and arguments, not a program's base
  microcode, which was uploaded earlier; the consumed program's unpatched
  bytes are taken as the GPU read them.
- The capture derives the description from the existing renderer's
  translated state (a development tool); the executor sees only the
  description.
- The executor covers what these passes need: one colour target, no depth
  or stencil, 2D textures, one draw. Cube maps, multiple targets, depth and
  feedback (a pass sampling its own target, PPFX pass 11) are R2's.
- Inputs produced outside the pass (asp's scene target) come from the
  capture's copies; unbound units sample a 1x1 dummy, as in the game.

## Reproducing

```
tools/r1_dev_lift.sh
tools/r1_capture.sh river1 river_fight 100
tools/r1_capture.sh field1 field_after_dungeon 120 160
build/r1/bld/r1_replay.exe build/r1/captures/river1/pass3 --validate      (and --poison; --reps 200)
build/venv/Scripts/python.exe tools/r1_compare.py build/r1/captures/river1/pass3 --png
build/venv/Scripts/python.exe tools/r1_boundary_check.py build/r1/captures/river1 pass3 pass48
build/venv/Scripts/python.exe tools/r1_check_deps.py build/r1/bld
```
(VK_LAYER_PATH pointing at the Vulkan SDK's Bin for --validate.)

## R2: what it reuses, what it needs, a proposed scope

**Reuses as is:** the entry observers and boundary log; the consumption
capture's per-draw machinery; the description's vocabulary; the executor's
device set-up, image, sampler, buffer and pipeline creation, program
conversion and comparison, validation, poison and timing; the three checks.

**Needs:**
1. A chain description (v1): resources by identity across draws (guest
   offset and format at capture, a resource id in the description) with
   their first and last use, and the pass list in order; only the chain's
   external inputs carry pixels (asp's scene target, and any resource read
   before it is written: cross-frame feedback such as exposure or
   afterimage, if present).
2. Capture of a whole PPFX chain of one frame (46-47 draws), with the
   reference content of every chain target at the end and per pass on
   request.
3. The executor running a pass list: a render pass and framebuffer per
   target, barriers from actual read-after-write dependencies between passes
   (the dependency tracking H1 needs), and the feedback pass (pass 11 reads
   the scene target it writes; the game takes a copy first: the native chain
   does the same, or renders into a second image).
4. Every distinct PPFX program converted and checked (a traced combat frame
   binds 19 distinct fragment programs in PPFX's program area).
5. The boundary cross-check over every pass of the chain.

**Exit criteria (proposed):** for the river fight, exploration and the
owner's scenes for depth of field, afterimage and exposure (NATIVE_PORT.md
§11): every chain target and the final composite bit-exact, or each
difference explained before any tolerance; every program identical;
validation clean; the executor still independent; the chain's GPU time
measured against the game's PPFX time (0.70 ms a combat frame with its
gaps). R2 removes nothing from the game either.

**Effort, revised from R1:** R1 was estimated at 2-3 weeks and took one
working session (about two hours of implementation and validation on top of
the evidence stage). R2 is provisionally 2-4 days, most of it in the chain
description, the dependency-driven barriers and the feedback pass.
