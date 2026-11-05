# R3 follow-up: first-load frames, explicit records, synchronisation, the SPU read, reverse dependencies (2026-11-05)

The owner reviewed R3 (docs/NATIVE_R3.md) and asked for its open issues
closed and R4 prepared before R4 starts. The existing renderer stays what
the player sees; the native path is still development-only and off by
default. R4 has not started.

Labels as elsewhere: **[M]** measured, **[C]** from code, **[I]** inferred,
**[U]** unknown.

## Summary

- **The first-load frames match** [M]. Of R3's two "inherited" items, the
  colour mask was the existing renderer misreading PPFX's report copy (a
  memory-to-memory transfer on FIFO subchannel 1) as the 3D colour mask --
  a one-frame tint after every load that the game does not have; fixed in
  the existing renderer (a behaviour change, commit 81539ff). Unit 0's
  address mode is asp's: the recorder now follows it, and asp's colour
  mask, through asp's own state API. Dual runs since: 0 cross-check
  differences and every compared output identical, the first-load frames
  included (section 1).
- **Records are explicit descriptions** [C]: surface, viewport, scissor,
  programs with their parameters, textures, samplers, vertex inputs, render
  state, in libgcm's vocabulary; the 754-word register block is gone. What
  PS3-shaped machinery remains is listed in section 2.
- **The native GPU time falls from 1.73 to 0.52 ms a frame** [M] (same
  build, comparisons off): per-image barriers instead of full ones take
  0.76 ms off, uploads in device memory instead of system memory 0.45 ms
  (section 3). R2's standalone executor needed 0.48 ms. Validation clean,
  hazards caught.
- **The SPU read** [M] was the runtime's own diagnostic probe dereferencing a
  float (-116.0 = 0xC2E80000) as an RSX address; no SPU program or game code
  read a PPFX target. Probe off by default (section 4).
- **Other work does take state from PPFX** [M]: asp's scene and the game's
  draws render into the colour target PPFX set, and asp's vertex programs
  read a matrix PPFX's pass left in the constants (c457-c460); nothing reads
  a PPFX output as a texture (section 5).
- **Coverage** [M]: menus over the game, the map, the editor and its
  preview, the album, title/CONTINUE cycles, 30/72/60 fps pacing and 144 Hz
  pacing with the vblank clock at the display's period: all identical
  (section 6); every comparison now ends counted (section 7).
- **R4's plan** is revised (section 9): about 2,900 command-writing calls a
  frame bypassed, about 350 kept because other work reads their state,
  PPFX's occlusion query decided by a check, three to four stream markers a
  frame carrying sequence numbers, all of it in ppfx_bridge.cpp.

## 1. The first-load frames

R3 reported two items PPFX's first frames after a load took from other
code: the colour mask (green and blue only, one whole frame of 46 draws) and
texture unit 0's address mode (3 draws). Traced to their writers with a
frame trace started by the state itself (patch 0110: the recorder arms it at
PPFX's first call after 30 frames without one; build/r3/lt1, lt2, lt3), they
turn out to be two different things.

**The colour mask was never PPFX's state [M].** In the frame 18 frames after
each load, PPFX runs an occlusion query and copies its report into main
memory: cellGcmSetReport (0x006A829C, from 0x007C2F48), then a
memory-to-memory transfer (0x006A1D1C, from 0x007C2F94) on FIFO subchannel 1
-- libgcm's NV0039 object:

```
0017E330 00082184 006A1D1C 1 lr=007C2F94 M 1 0184 2 66626660 FEED0001
0017E33C 0020230C 006A1D1C 1 lr=007C2F94 M 1 030C 8 00003010 01A2C500 00000000 00000000 00000010 00000001 00000101 00000000
```

(DMA in: the local report area; out: main memory; offset in 0x3010, out
0x01A2C500, a 16-byte line, once, FORMAT 0x101.) The runtime's FIFO walker
fed subchannel 1 to the 3D engine (a default other titles needed), so the
copy's FORMAT word, method 0x0324 on subchannel 1, landed in the 3D
engine's colour mask: 0x00000101, green and blue. PPFX's next 46 draws ran
with it in the existing renderer -- a one-frame tint after every load that
the game does not have: on a PS3 the 3D engine never sees subchannel 1's
methods. The same transfer also wrote 0x30C-0x328 of the 3D engine (alpha
reference, blend enable 0x01A2C500 -- bit 0 clear, so off -- blend factors,
blend colour and equation, stencil test enable 0). The copy itself was
never carried out.

Fix (commit 81539ff, a behaviour change of the displayed renderer, separate
from the rest): the port now sets GCM_SUBCH1_2D=1, routing subchannel 1 to
the 2D path, where NV0039's methods are counted as unhandled; the copy is
not carried out, as before. In 75 traced frames nothing else uses
subchannel 1. GCM_SUBCH1_2D=0 restores the old routing. This replaces the
"inherited colour mask" R3 asked the owner to decide on: preserving it would
have meant the native renderer reproducing a decoding error of the port's
existing renderer.

**Unit 0's address mode is asp's [M].** PPFX's first-load chains (the 38-,
49- and 51-draw variants) sample unit 0 in three draws without setting its
address mode in that frame; the last writer of that address mode before
them is asp, the engine, through its own state API (lt1, lt2): its sampler
bind (func_006C37A0, called from 0x006CB1C0) or its re-emission of its
state store (func_006D0BE0, the game's memcpy from 0x006D0C20). What it
means:

- asp keeps a sampler table, one 48-byte entry a texture unit (TOC entry
  -0x2D88): wrap S at +0x20, wrap T at +0x24, LOD and anisotropy fields
  around them. Its sampler bind (device, unit, enable) turns a unit's entry
  into the GPU's sampler state; the address mode is
  `(wrapT + 1) << 8 | (wrapS + 1)` with R clamped and the depth comparison
  GEQUAL (0x60030101 for wrap S = wrap T = 0: both CELL_GCM_TEXTURE_WRAP).
- asp keeps its whole render state in a state store, a command block at the
  CellGcmContextData its device holds (+0x124), one slot per state item at
  offsets from its own tables (TOC entries -0x2D98 for the render state,
  -0x2D54 for the units' address modes, -0x2D28 their control 0); every
  setter writes the item both into the command buffer and into its slot,
  and asp re-emits ranges of the store into the command buffer -- the whole
  store (94 methods) at each pass it begins, the colour-mask slot alone, or
  the alpha-function pair -- by its block append (func_006D0BE0 through
  0x006D0C4C/0x006D0C70).

**At the native boundary [C].** The development observers now include
asp's state API (14 functions, config/dev_capture_observers.json). The
recorder follows:

- asp's colour mask from its setter's arguments (func_006C02E8: four bytes,
  red, green, blue, alpha);
- a unit's address mode from asp's state store once a bind of that unit has
  run (resolved at the next call on the thread: the store then holds it),
  and the colour mask and address modes in any range asp re-emits into the
  command buffer -- read at the item's slot, whose method header is checked
  against the item expected there (a mismatch is counted; 0 in every run);
- asp's pass apply (func_006D1764), which sets a colour mask the model does
  not follow, as "unknown" until the next setter or re-emission: PPFX draws
  made while it is unknown are counted (0 in every run).

Each item records who set it last (PPFX, the game's libgcm calls, asp, the
reset state), and the cross-check names it with the item.

**The complete list of what PPFX's draws take from others [M, lt1, 367
PPFX draws over the trace's 8 frames with PPFX, steady and first-load]:**

| Item | Set by | Matters? |
|---|---|---|
| colour mask (0x0324) | asp's colour mask setter (all four channels), every draw | yes: followed |
| unit 0's address mode (the first-load chains' three draws) | asp's sampler bind, asp's store re-emission | yes: followed |
| blend factors, equation (0x0314-0x0320) | asp's blend setter | no: PPFX turns blending off itself |
| stencil function, operations, masks (0x032C-0x0344) | asp's stencil setters and store | no: PPFX turns the stencil test off itself |
| depth function, depth write (0x0A6C, 0x0A70) | asp's setters | no: PPFX turns the depth test off itself |
| cull face, front face (0x1830, 0x1834) | asp's cull setter | no: PPFX turns culling off itself |
| polygon offset (0x0A68 off, 0x0A78/0x0A7C) | asp's store | no: off, and PPFX draws no depth |
| line width (0x03B8) | asp's line width setter | no: triangles only |
| shade model, blend/colour mask for targets 1-3, logic op, depth bounds (0x0368-0x0380) | the game's libgcm calls at a frame's start (observed) | followed through the calls; logic op off |
| user clip planes, branch bits, zcull, and the other never-written items | the reset state | the explicit state's reset values |

The native renderer refuses a draw with blending, depth, stencil, alpha
test or culling on (counted as unsupported: 0 in every run), so an
inherited enable could not pass silently.

**Result [M].** Before the subchannel fix (rx1, river): 171,073 PPFX draws
cross-checked, 46 differ (the colour mask, one frame); unit 0's address
mode 0 differences -- asp's state is followed. After it (rx2): 0 of 170,979
draws differ in any item; 12,739 of 12,739 compared outputs identical, the
3,671 after the structure change (the area's first frames) among them.

## 2. The record format: explicit descriptions

**What a record holds now [C]** (port/native/render/ppfx_native.h, `State`):

| Part | From | Fields |
|---|---|---|
| colour target | cellGcmSetSurface's CellGcmSurface | colour format, target selection, antialiasing, locations, offsets, pitches, depth surface, width, height, origin |
| viewport, scissor | cellGcmSetViewport, cellGcmSetScissor | rectangle, depth range, scale and offset vectors; rectangle |
| fragment program | the bind (CgBinaryProgram, offset, location) and its shader control | the bytes the GPU will read (a blob by content), attributeInputMask, texture coordinate masks, registerCount, outputFromH0, depthReplace, pixelKill, txp, gamma output |
| vertex program | cellGcmSetVertexProgram's CgBinaryVertexProgram | the instructions from the start slot (a blob), start slot, attributeInputMask, register count class |
| constants | cellGcmSetVertexProgramParameter | the 512 vertex constants, which were ever set |
| textures | cellGcmSetTexture's CellGcmTexture | format, levels, dimension, cube, location, crossbar, size, pitch, offset |
| samplers | cellGcmSetTextureControl/Filter/Address/BorderColor, or asp's state API | enable, LOD range, anisotropy, filters, bias, wrap modes, depth comparison, gamma, border; who set the address mode |
| vertex inputs | cellGcmSetVertexDataArray, cellGcmSetVertexDataBase, cellGcmSetFrequencyDividerOperation | size, type, stride, frequency, location, offset; base offset and index; divider; current values |
| render state | the single-value calls, cellGcmSetAntiAliasingControl, cellGcmSetPolygonOffset, asp's colour mask | colour mask and who set it, the enables, depth, cull, polygon modes, shading, antialiasing control, polygon offset; the occlusion-query settings kept for R4 |
| clear | cellGcmSetClearColor, cellGcmSetClearDepthStencil, the clear's mask | ARGB colour, mask |
| inputs as bytes | guest memory at the draw | the vertex ranges the draw reads, small static textures |

The values are libgcm's own constants (CELL_GCM_TEXTURE_WRAP, CELL_GCM_LESS,
...) and the CellGcm structures' fields -- the API's vocabulary, not
register words. The 754-word register block, its range table and the
scratch register file are gone; nothing in the recorder or the native
renderer reads or writes an RSX register image.

**How the renderer uses it [C].** Pipelines, samplers, the vertex fetch and
the constant buffers come from the fields directly: the sampler from the
filter, LOD and wrap fields; the vertex fetch plan filled from the vertex
inputs (the toolkit's fetch and conversion code reused); the colour write
mask and front face from the render state; the window transform from the
surface. The translators' per-unit parameters (gamma and sign-expansion
lanes, depth comparison, unnormalised coordinates) are computed from the
texture and sampler fields by small functions that do what the toolkit's
register-word helpers do (rsx_texture_gamma_mask and the like). Where a
toolkit helper's interface itself takes a hardware word -- the fragment
translator's control word, the remap crossbar the view swizzle is read
from -- it is made at the call from the record's fields.

**What PS3-shaped machinery remains in the native path [C]:**

- *Programs are RSX microcode.* They are the game's assets; the toolkit's
  translators turn them into SPIR-V (through HLSL), cached by content. The
  fragment translator takes the RSX control word, made at the call.
- *The fragment programs' constants are patched in RSX local memory's
  terms.* PPFX patches them with inline transfers to offsets in local
  memory; the recorder applies those to a shadow of the program pages
  (keyed by offset) to know the bytes at each bind. Natively this is a
  constant buffer update; the shadow is how the calls describe it today.
- *Asset encodings.* Texture formats, the crossbar, vertex types, surface
  formats: the game's data and libgcm's enums, decoded by the toolkit's
  asset helpers (rsx_texture_mip_chain, rsx_texture_decode,
  rsx_texture_component_remap, rsx_vertex_attrib_size).
- *Resources are known by guest address.* A target or texture is
  (location, offset) in guest memory; the native images are a registry by
  those keys. Natively they would be PPFX's own surface objects.
- *asp's state is read from asp's state store*, a command block (a method
  header and its value per slot), at asp's API calls -- PS3-shaped data at
  asp's boundary, until asp itself is native (R5).
- *The temporary bridge* (ppfx_bridge.cpp): matching records to the FIFO
  walker by ring lap and IO offset, the walker's hooks, the handoff slots,
  the engine's surfaces copied in the engine's stream. It goes with the
  FIFO path.
- *Development only:* the cross-check, which decodes the engine's register
  file into the explicit form (engine_state) and compares field by field;
  the frame trace and the target watch.

**Checked [M].** Every PPFX draw is cross-checked against the engine at the
same point, now field by field: 0 differences in the river (rx2, 170,979
draws) and, with the comparisons, outputs bit-exact (section 7). The
same records render with the same results as R3's: the steady variants
had 0 differing draws then and have 0 now.

## 3. Synchronisation and the native GPU time

**What changed [C].** The native renderer tracks, per image, its layout,
the stage and access of its last write, and the stages that read it since,
and gives each access the barrier its hazard needs -- read after write
once per reading stage, write after read or write, a layout change -- in
the layout the access needs (colour attachment, shader read, transfer
source or destination), as R2's executor does (r1_replay.c r2_access).
DOTHEROES_PPFX_BARRIERS=full keeps R3's way (every image in GENERAL, a full
barrier between operations) for comparison. The batches' upload buffers
(vertices, indices, constants) are in device-local memory the CPU writes,
where the device has it (DOTHEROES_PPFX_UPLOAD=host: system memory, R3's
way). Unchanged, and required:

| Boundary | How it is ordered |
|---|---|
| the engine's copies into slots (the other queue) -> the native reads of them | the binary semaphore the engine's submission of those copies signals, waited on (all stages) before the first record that needs it; the slot stays in GENERAL, as the engine's copy left it |
| a slot's reuse by the engine after a native read | the native batch's fence, seen by the CPU before the slot is handed back |
| native writes -> the comparison's CPU read | a host barrier at the batch's end and its fence |
| native batches among themselves | the same queue: barriers apply across command buffers in submission order |
| queue access | every native submission under the backend's queue lock; the native command pool used by its thread only |

**Measured separately [M]** (river, 150 s each, comparisons and cross-check
off, 6,656-6,669 frames with PPFX records each; each batch's timestamps
give the GPU time from one operation's end to the next's to the operation
between them, its barriers included):

| Per PPFX frame | full barriers, uploads in system memory (ov4) | per-image barriers, system memory (ov3) | per-image barriers, device memory (ov5, the default) |
|---|---|---|---|
| native GPU, all | 1.73 ms | 0.97 ms | **0.52 ms** |
| -- draws and clears (47 a frame) | 1.55 ms | 0.78 ms | 0.41 ms |
| -- copies: handoffs (2.0 a frame) and the own-target snapshot (1.0) | 0.16 ms | 0.14 ms | 0.10 ms |
| -- the batch's end (host barrier) | 0.02 ms | 0.04 ms | 0.01 ms |
| barriers recorded | 119 (full) | 98 (image) | 98 (image) |
| the native thread (CPU) | 0.42 ms | 0.42 ms | 0.44 ms |
| fight frame times (82 s): p50 / p99 / max; over 25 ms | 16.7 / 17.2 / 20.6 ms; 0 | 16.7 / 17.4 / 20.2 ms; 0 | 16.7 / 17.2 / 21.8 ms; 0 |

Two causes, each measured: the full barriers made each of the 47 small
passes wait for the whole pipeline to drain (0.76 ms a frame), and the
GPU read every draw's vertices and 8 KB of constants from system memory
over PCIe (0.45 ms a frame).

**Against R2's 0.48 ms [M, I].** The standalone executor's chain cost 0.48
ms in the river (copies and barriers included, its external inputs seeded
outside the timing; NATIVE_R2.md); the live renderer now 0.52 ms. What is
left: the two handoff copies a frame the replay seeded outside its timing
(part of the 0.10 ms of copies) [M]; and the engine's rendering on the
other queue at the same time, which stretches the native batches' elapsed
time on the shared GPU [I] -- the timestamps measure elapsed time, not
cost, and R2's executor ran alone.

**R3's figures, recomputed [M].** R3 divided its per-frame costs by every
flip since start-up (8,863 in ov2), about 2,200 of them before PPFX's first
frame. Per frame with PPFX records (6,655 in ov2) its figures were: the
recorder 0.33 ms, the bridge 0.10 ms, the native thread 0.61 ms, the native
GPU 1.69 ms (not 0.245, 0.078, 0.46 and 1.27). Every figure here is per
frame with PPFX records.

**The other costs [M, ov5]:** the recorder 0.33 ms a frame on the game's
thread (every observed call, the encoders, asp's state API, records); the
bridge 0.10 ms on the walker's; the native thread 0.44 ms (0.61 ms with
R3's register block). The native thread's waits for the engine's signals
and for the walker are idle time, not cost.

## 4. The SPU read

R3 said an SPU job read one page of a PPFX intermediate once and called it
harmless because it happened once. Neither the attribution nor the
conclusion held up.

**What read it [M].** The access (build/r3/tw3, the target watch over the
river) was at dotheroes.exe+0x91016A6, which the build's PDB resolves to
spu_workload.c line 1046 in spu_async_run: not SPU code, and not a DMA,
but a diagnostic probe of the runtime's own. For every dispatch of SPU
image 3 it prints the 256-byte argument block and, for every word of it
that looks like an address, the words at +0x00, +0x40, +0x50 and +0x54
there. It was written for another title, whose image 3 is a Bink video
task. In 3D Dot Game Heroes image 3 is the Edge zlib inflate task
(spu_0002_at_00F49F00: "edgezlib_inflate_task.cpp"), dispatched twice at
the river's area load (Field_18's packs). Its second argument block, at
0x16741500, holds floats: 3F800000 (1.0), 430D999A (141.6), and
C2E80000 at +0x54 and +0x94 -- -116.0, which the probe took for an address
in RSX local memory, 0xC2E80000 (local offset 0x02E80000, inside the
640x360 FP16 intermediate at 0x02DD0000).

**The exact bytes:** four 32-bit reads, at local offsets 0x02E80000,
0x02E80040, 0x02E80050 and 0x02E80054 -- 16 bytes -- printed to stderr as
`[desc+54]=0xC2E80000 -> +00:00000000 +40:00000000 +50:00000000(gate)
+54:00000000` (the run's stderr.log, lines 2819-2840). Nothing else used
them.

**Whether it matters.** It is an instrumentation issue: no SPU program and
no game code read the target; the zlib task's own DMA never touched a PPFX
page in any watch run. It does not depend on the frame or on R3, and on a
PS3 the probe does not exist. What it did matter for: R3's report, whose
"a pre-existing PS3 difference" was wrong -- corrected here and in
NATIVE_R3.md.

**Fix and reproducible case.** Patch 0111 turns the probe off unless
SPU_IMAGE3_PROBE (or the probe's own LBP_REAL_POLICY) is set. The watch,
river, 100 s each [M]:

| Run | The probe | SPU accesses to PPFX's pages | Descriptor dumps |
|---|---|---|---|
| tw4 | off (the default now) | **none** | 0 |
| tw5 | on (SPU_IMAGE3_PROBE=1) | one read of page 0x02E80000 at 53.9 s, from spu_async_run (spu_workload.c line 1055: the probe's dereference) | 2 |

In both, the only accesses to PPFX's pages are the ones R3 listed: the
game's clearing routine at each area load, the game's memcpy into the
program pages, the walker applying PPFX's queued patches to them.

The case is kept: build/r3/tw3 (the original), tw4 and tw5.

## 5. State dependencies in the other direction

R4 removes PPFX's commands from the stream. Whatever other work's draws
take from them would change with them. Two checks:

- **the trace** (tools/r3_reverse_deps.py over build/r3/lt3: 60 frames from
  the river's load, every data word, every word's writer): for each draw
  that is not PPFX's, the state its derivation reads -- surface, viewport,
  scissor, both programs and their controls, the enabled vertex arrays and
  the values of the others, the enabled texture units, the render state
  with what its enables make irrelevant left out, and the vertex constants
  its program reads (directly; an indexed read's base noted) -- and which
  of it PPFX wrote last;
- **the bridge** (development, with the cross-check): for each draw that is
  not PPFX's, the PPFX targets its fragment program actually samples and
  the PPFX targets it renders into (the engine's state at that draw).

**Found [M, lt3: 14,952 other draws -- asp's 13,653 (its draw at
0x006C6F88), the game's 1,299 through libgcm (0x006A2164 called from
0x00024720)]:**

| What other work takes from PPFX | Who | Draws (60 frames) | Real? |
|---|---|---|---|
| the colour target and depth surface (cellGcmSetSurface's whole CellGcmSurface: the 1280x720 FP16 scene target at 0x01AE0000, depth at 0x00730000; the shader window) | asp's scene; the game's draws into the scene | 3,968; 999 | **yes**: PPFX's pass 1 sets the scene target and asp draws the scene into it without setting a surface of its own |
| vertex constants c457-c460, read directly | asp's | 541-591 | **yes**: a matrix PPFX's pass setup (func_007D1354) leaves in the constants, read by asp's programs |
| vertex constants through indexed reads (bases c16, c221) | asp's, the game's | 104-5,090 | **not decidable statically**: an index register's range bounds what they read; PPFX's constants are c450-c467 |
| texture unit 2 bound to a PPFX target (0x04720000, 320x180) | asp's, the game's | 9,506; 300 (unit enabled) | **no**: the unit is left enabled, but no fragment program of other work samples it -- the bridge's per-draw check finds other work sampling no PPFX target at all (mn1, ed1, rd2) |
| line smoothing and stipple, polygon smoothing and stipple, logic op, the vertex data base, the frequency divider, the shader packer | asp's, the game's | up to 13,474 | **no**: every writer of these, PPFX's and others', writes 0 in all 60 frames -- the reset value they would keep |

**The bridge's check [M]** (every other draw's fragment program and
colour target at the engine's draw: mn1 4.8 million other draws, ed1 0.7
million, rd2 -- the dungeon -- 2.4 million): other work samples no PPFX
target; it renders into three: the scene target 0x01AE0000 (asp's scene
and the game's draws, between PPFX's first pass and its resumption) and
the two display buffers 0x00000000 and 0x00398000 (the draws after
PPFX's final composite in a frame: the HUD, the menus). Those are the native-to-engine
handoffs R4 needs (section 9).

Nothing else of the effective state: viewport, scissor, both programs and
their controls, the vertex arrays and the render state's enables are set by
other work itself before its draws.

**The handoffs this asks for in R4 (section 9):** the surface PPFX leaves
for asp and the game, and PPFX's vertex constants (all of them, since the
indexed reads cannot be bounded statically) -- kept as PPFX's own calls
whose bodies still run, in their place in the stream, until asp is native;
then direct handoffs between native producers.

## 6. Coverage

All runs: the explicit-record build, dual run (records, matching,
cross-check, native rendering, comparisons), real audio, river or dungeon
checkpoints as before (tools/r3_scene.sh) [M]:

| Run | What | Records matched / missed | Cross-check differences | Compared: identical / differing / skipped / unfinished |
|---|---|---|---|---|
| rx2 | river combat | 170,981 / 0 | 0 | 12,739 / 0 / 0 / 0 |
| dg6 | the first dungeon, three times to the title and CONTINUE | 807,511 / 0 | 0 | 106,078 / 0 / 0 / 0 |
| mn1, mn2 | menus over the game (the item menu, the system menu, the map), each twice, TITLE and CONTINUE between | 364,441 / 0; 427,609 / 0 | 0 | 28,725 and 32,139 / 0 / 0 / 0 |
| ed1, ed2 | the dot editor (a new character, a pose's block grid), then its PREVIEW | 56,959 / 0; 50,990 / 0 | 0 | 6,587 and 6,276 / 0 / 0 / 0 |
| p30 | river, 30 fps requested (DOTHEROES_FPS=30) | 112,278 / 0 | 0 | 13,459 / 0 / 0 / 0 |
| p72 | river, the display's pace requested (DOTHEROES_FPS=display) | 274,099 / 0 | 0 | 18,383 / 0 / 0 / 0 |
| p144 | river, uncapped requested (DOTHEROES_FPS=0) | 229,355 / 0 | 0 | 15,913 / 0 / 0 / 0 |
| p144b | river, 144 Hz requested (DOTHEROES_FPS=0, DIAG_VBLANK_MS=6.944) | 346,339 / 0 | 0 | 22,174 / 0 / 0 / 0 |
| sn1 | the system menu's SNAPSHOT: the camera, pictures taken | 214,456 / 0 | 0 | 15,090 / 0 / 0 / 0 |
| sn2 | SNAPSHOT, pictures taken, then the system menu's ALBUM showing them | 201,437 / 0 | 0 | 14,445 / 0 / 0 / 0 |


**First-load frames.** Every area entry and every CONTINUE starts with
the first-load chains (38 draws; 49 and 51 with two 1x1 passes); the
structure trigger compares every draw of them and of the 60 frames after
them. In dg6 the 16 such triggers asked for 65,054 comparisons and all
65,054 ran, all identical; in rx2 3,671 of 3,671. By chain variant (dg6):
the 49-draw first-load chain 196 draws in 4 frames, the 51-draw 153 in 3,
the 38-draw 38 in 1, the dungeon's steady 49-draw 30,664 in 480 frames,
the steady 47-draw 74,926 in 1,430 -- 0 differing in each.

**Menus over the game.** Under the item menu, the system menu (the readout
shows it open: menu 1, scene 3) and the map, PPFX keeps drawing its steady
47-draw chain: no new chain variant appears while they are open (mn1), and
every comparison in those frames is identical. The window grabs (every
5 s, mn2) show each screen: the item menu's two pages, the system menu,
the map (twice each), the title and CONTINUE between.

**The editor, SNAPSHOT and the album.** The editor's own screens (the
character list, the pose grid) run no PPFX work: the records begin when
PREVIEW loads a small field with the character in it, and PPFX's
first-load and steady chains run there (ed1 1,213 frames with records,
ed2 1,086; the grabs show the screens). SNAPSHOT opens a camera over the
running game (PPFX's steady chain under it) and takes pictures. The
album (the system menu's ALBUM, "View snapshots") shows a picture full
screen (sn2's grabs). With the target watch on (sn1, sn2): while pictures
are taken, the game's main thread writes into the second display buffer's
memory for about 3 s (765 page writes in sn2) through a host-side copy --
not the game's own memcpy, which the recorder observes -- and no CPU code
reads a PPFX target or a display buffer. How the picture's pixels are
taken from the frame is not identified: not by a CPU read (the watch) and
not by a draw sampling a PPFX target (the bridge's check); a copy by the
engine's 2D path would read the display buffer after PPFX's final
composite (section 10).

**Pacing: requested and achieved [M]** (the frame log's flips, the run's
last 60 s, the river; dual run with comparisons):

| Requested | Achieved | Flip interval p50 / p99 / max | Over 25 ms |
|---|---|---|---|
| 30 (DOTHEROES_FPS=30, the original pacing) | 30.0 fps | 33.3 / 34.2 / 35.0 ms | -- |
| the display's pace, 144 Hz / 2 (DOTHEROES_FPS=display) | 72.0 fps | 13.9 / 14.5 / 25.4 ms | 1 |
| uncapped (DOTHEROES_FPS=0) | 59.9 fps | 16.7 / 17.3 / 33.6 ms | 5 |
| 144 (DOTHEROES_FPS=0 and the vblank clock at the display's period, DIAG_VBLANK_MS=6.944) | 85.9 fps (p144b) | 13.7 / 14.6 / 26.1 ms | 2 |
| 60 (the default) | 59.8 fps (rx2) | 16.7 / 17.4 / 95.6 ms | 6 |

DOTHEROES_FPS=0 alone does not go above 60: the game waits for each flip,
and a flip completes on a tick of the port's 60 Hz vblank clock
(HANDOFF.md); 144 Hz pacing needs the clock at the display's period as
well (DIAG_VBLANK_MS=6.944), and the dual run then reaches 86 fps, not
144 (it renders PPFX twice, compares and cross-checks; HANDOFF.md measured
100-112 fps for the plain game at this setting). Native PPFX matches at
every pace: a frame's chain does not depend on the rate.

**Not reached, needing the owner:**

- **the boss and a magic attack** -- tests/scenarios/owner_boss_r3.drive
  runs the dual-run build from the save before the boss's portal
  (build/checkpoints/dungeon1-boss-door); no automated route opens the
  special key's chest;
- **areas past the first dungeon** (the desert, the lava dungeon, the
  volcano, the forest and the dungeons there): no saves exist there. A save
  in any of them, or a session that reaches one, gives the same dual run
  there.

## 7. Comparison coverage and the repeated checks

**Every comparison is accounted for [C].** A comparison asked for (every
frame's final composite; every draw of every 30th frame; every draw of a
frame whose chain departs from the frame before and of the 60 frames
after it) ends as exactly one of: identical, differing, skipped -- no
engine copy (no buffer), no readback buffer, a size mismatch, the native
side did not draw the record -- or unfinished (in flight when the run
ended). The report prints all of them, and the ones after a structure
change apart. What changed to make them finish:

- the finals and the structure-change frames have buffers kept back for
  them (32 and 160 of the 512, and 1/8 and 3/8 of the 768 MB), and when
  every buffer is busy their copy waits for one (the engine's work
  submitted first) instead of being skipped; the sampled every-30th-frame
  comparisons still skip when nothing is free;
- after dl4 (below), a final or a structure-change comparison waits up to
  10 s for a buffer, not 2;
- at a test's stop (which ends the process without exit handlers) and at
  exit, the native renderer first records and submits everything walked
  and the comparison thread empties its queue (up to 15 s, inside the
  harness's 20 s), then the last report is written; R3's last report could
  be up to 10 s old and counted in-flight comparisons nowhere.

**Results [M]:** every normal run in section 6: 0 skipped, 0 unfinished;
the structure-change comparisons all ran (dg6 65,054 of 65,054; rx2 3,671
of 3,671; mn1 9,373 of 9,373).

**The repeated checks [M]:**

| Check | Run | Result |
|---|---|---|
| synchronisation validation (Khronos layer, sync validation with the shader-access heuristic), per-image barriers | vv3, river 100 s, 33,081 native draws | **0 errors**; the 10 warnings R2 had (four programs write a second colour output with one attachment bound) |
| negative control: two copies into one image without a barrier, every 5,000th draw | hz3, river | 7 injected, **7 reported** (SYNC-HAZARD-WRITE-AFTER-WRITE) |
| image lifetime: every native image retired every 60 frames, destroyed once no batch in flight uses it | rl2, river 100 s | 2,163 images replaced in flight, 76 live throughout; 12,808 of 12,808 comparisons identical |
| ordering: wraps inside a call, commands ending at the ring's end | dg6 | 2 and 3, all matched; 0 missed in every run |
| a slow consumer: the native side 5 ms slower a record, before the change below | dl4 | the game's thread waited for room 2,162 times (11.4 s); 0 records missed; 569 handoffs, none dropped; of 10,143 comparisons, 9,570 identical, 0 differing, 92 skipped, 481 unfinished at the end; of the 9,736 after a structure change 9,265 compared |
| the same, the comparisons' buffers kept back and waited for, the drain at the stop | dl5 | 6,901 waits for room (36.4 s); 0 missed; 686 handoffs, none dropped; of 7,670 comparisons, 7,510 identical, 0 differing, 92 skipped (sampled every-30th-frame ones, which still skip when no buffer is free), 68 unfinished; **the 7,020 after a structure change all compared** |

## 8. Memory, stability and the off switch

**Memory and stability [M].** dg6 (the dungeon, three title cycles, 361 s):
the working set rose while the comparison pools filled (839 MB at 30 s,
2,503 MB at 120 s) and stayed at 2,503-2,529 MB to the end; threads 56-58
and handles 859-866 throughout; native images 76-79 and pipelines 21-22 in
every run. No crash, hang or deadlock in the 33 game runs of this follow-up (27
development runs, 6 A/B runs): every one ended by its own stop.
Without comparisons (ov3) the process held 1,141-1,149 MB, as R3's ov2.

**The off switch [M]** -- the production build (no observers: none of the
native PPFX code linked; ps3recomp 0108-0111 in, and the subchannel-1
routing fix, which is part of the existing renderer now) against the
preserved checkpoint build (build/checkpoint/2026-11-04), the river fight,
real audio, alternated (build/r3/ab2):

| Build | Runs | p50 | p99 | p99.9 | frames over 25 ms | memory at the end |
|---|---|---|---|---|---|---|
| checkpoint 2026-11-04 | 3 | 16.7 | 17.4, 17.1, 17.3 | 18.6, 17.6, 18.2 | 1, 0, 0 | 1,233, 390, 1,065 MB |
| production now (0108-0111, the subchannel routing) | 3 | 16.7 | 17.2, 17.5, 17.5 | 17.6, 17.6, 17.8 | 0, 2, 1 | 1,137, 926, 1,065 MB |

No regression: the same distribution in both arms.

The checkpoint predates the subchannel fix, so the two differ in the one
frame after each load where the old routing tinted PPFX's draws; that frame
is outside the fight.

## 9. R4, revised: bypassing PPFX's command generation

R4 stops the libgcm bodies that write PPFX's commands; the records and the
native renderer produce PPFX's output; what other work needs from PPFX
reaches it through the temporary bridge. Not started: this is the plan for
review. What changed since NATIVE_R3.md section 9: the first-load question
is answered (section 1: nothing for the owner to decide), the reverse
dependencies are measured (section 5), and PPFX's side effects are listed.

**1. What is bypassed, what keeps running.** For a call attributed to PPFX
(its return address in PPFX's code, or a libgcm function's called from a
PPFX call), the lifter's entry observer gains a result: "the body is not
wanted", and the generated function returns at once with the body's normal
result (CELL_OK). Three groups:

| Group | Entry points | Calls a river frame | In R4 |
|---|---|---|---|
| PPFX's own state and draws | vertex arrays, textures and samplers, the fragment program bind and shader control, viewport, scissor, anti-aliasing control, the single-value state calls, draw arrays, the clear, the inline transfers (the fragment constants' patches) | about 2,900 | **bypassed**: only PPFX's own draws use them (section 5), the records carry them |
| state other work reads after PPFX | cellGcmSetSurface where other work draws next into the surface PPFX set (the scene target before asp's scene); cellGcmSetVertexProgramParameter (PPFX's constants, read by asp's and the game's vertex programs: 213 calls) and cellGcmSetVertexProgram (47: it writes each program's default constants, 94 nested calls, among them c450 and c462) | about 350 | **kept**: the body runs, its commands stay in the stream, counted with the reason; each one becomes a native handoff when asp is native (R5) |
| side effects | cellGcmSetReport, cellGcmSetZpassPixelCountEnable, cellGcmSetClearReport, cellGcmSetRenderEnable, the report copy (0x006A1D1C) | 5 in the frame after a load | **decided by a check first**: the port carries out the report request (the walker writes a report, value 0xFFFF) but never the copy, so the CPU's copy at 0x01A2C500 never changes, today or after R4; a data watch on the report slot and on 0x01A2C500 says who reads them -- if nobody, they are bypassed; if PPFX reads its copy, the native side writes what the port writes today |

Not bypassed either: the address and memory helpers (their return values
are used), the wrappers with no commands of their own (their nested
writers are handled one by one), the flip and the game's memcpy (not
PPFX's).

**2. The bridge's stream markers.** With PPFX's commands gone, the walker
can no longer find PPFX's records in the stream by their commands. In
their place, at the call that begins each segment of PPFX's work that
touches other work's surfaces, the recorder's observer writes one marker
into the command buffer: a single NV4097 no-operation method (0x0100,
which the engine ignores) with two words, a magic tag and the record's
sequence number -- where the bypassed commands would have been, so in the
stream's order relative to asp's work. Three to four a frame (the river:
pass 1 into the scene target; the resume after asp, reading the scene; the
final composite before the HUD), against about 6,900 command headers today.
The walker, at a marker:

- engine -> native: copies the engine's surface into a slot, in the
  engine's stream, ordered by a semaphore signalled by that submission (as
  R3, unchanged);
- native -> engine: when other work will draw into or sample what PPFX
  wrote (the scene target's initialisation by pass 1 before asp's scene;
  the final composite before the HUD), submits the engine's work so far,
  makes the engine's next submission wait for the native batch holding that
  record (a timeline semaphore the native queue signals with the record's
  sequence number), and records the copy from the native image into the
  engine's surface first in it.

**3. Ordering without PPFX's commands.** A marker sits where PPFX's
commands were, so the walker meets it exactly where the FIFO path met
them: after every earlier write of other work, before every later one. The
marker carries the record's sequence number, so matching is by sequence
(lap and IO offset are no longer needed: the walker reads the number),
and the native renderer takes records in sequence order. No cycle: the
engine's submission that copies asp's scene for the native side is made
before the walker records a wait on the native side; the native batch that
waits for it is submitted before the engine's next submission waits on it
(each wait follows the other side's submission, as R3's waits do), and a
wait is never recorded in a command buffer that also signals what the
native side waits for.

**4. Isolation, and how it is removed.** All of it in ppfx_bridge.cpp:
the marker's format and emission (one function the recorder calls), the
walker's hooks, slots, semaphores, the kept-writer table. When asp and the
HUD render natively (R5, R6), the engine no longer renders anything PPFX
hands over to: the markers, the walker's hooks, the slots and the
cross-queue semaphores go, the kept writers' state becomes a direct handoff
between the native producers, and ppfx_bridge.cpp is deleted with the
queue's `walked` index.

**5. Counters that prove the bodies no longer run.**

- per entry point, PPFX-attributed calls whose body ran (kept, with the
  reason from the table) and whose body was skipped -- ran = 0 for every
  bypassed entry point;
- a frame trace's PPFX-attributed command words: only the kept writers'
  and the markers;
- the walker's draws a frame: 47 fewer (49 in the dungeon); its decoded
  methods a frame: PPFX's share fewer;
- the walker's writes into PPFX's program pages (the inline transfers it
  applies today): 0 (the target watch);
- the reverse audit (tools/r3_reverse_deps.py) on an R4 trace: the state
  other work takes from PPFX is the kept writers' only;
- the dual run continues in R4 against a reference run of the same scene
  from the R3 build (the off switch), compared frame by frame.

**6. Before R4: the checks this report leaves open.** Who reads PPFX's
report (group 3); how a snapshot takes its picture (a frame trace of one). The indexed constant reads (section 5) are covered by
keeping every PPFX constant write, not proven harmless to drop.

**Expected savings**, from earlier measurements, not from this work: the
walker no longer decodes PPFX's ~6,900 command headers a frame (about 16%
of them) and the engine no longer draws PPFX's 47 draws (0.86 ms of GPU in
the river, NATIVE_R2.md); the native renderer adds 0.52 ms of GPU beside
the engine (section 3) and its CPU (the recorder 0.33 ms on the game's
thread, the native thread 0.44 ms on its own). R4 measures the difference
in the game.

## 10. Remaining gaps

- **The boss and a magic attack**, and **the areas past the first
  dungeon**: they need the owner (section 6).
- **How a snapshot takes its picture** (section 6): not a CPU read of a
  PPFX target and not a draw sampling one; if it is a copy by the engine's
  2D path, it reads the display buffer after PPFX's final composite, which
  R4 hands over at the same place in the stream -- to be confirmed with a
  frame trace of a snapshot before R4.
- **PPFX's report readers**: who reads the report slot and the copy at
  0x01A2C500 (section 9, group 3); a data watch answers it before R4.
- **Indexed constant reads** in asp's and the game's vertex programs (bases
  c16 and c221) cannot be bounded statically; R4 keeps PPFX's constant
  writes so they cannot change (section 5).
- **asp's pass apply** (func_006D1764) sets a colour mask the recorder does
  not follow; a PPFX draw made before asp's next colour-mask setter or
  re-emission would be counted -- 0 in every run so far.
- **The native GPU time is elapsed time on a GPU shared with the engine**;
  its share of a frame's GPU cost is known only once the engine stops
  drawing PPFX (R4).

## Evidence kept

Local only (game-derived: frame data, the game's memory), under build/r3/,
outside the test runs' pruning; per run `<tag>/report.txt` and `<tag>/run/`
(run.json with the build's hash and every switch, the drive log,
frames.log, memory.csv, stderr), as R3:

| Tag | What |
|---|---|
| lt1 | the first state-triggered frame trace, 40 frames from the river's load: the full list of what PPFX's draws take from others (section 1) |
| lt2 | state-triggered frame trace, 75 frames from the river's load: the report copy on subchannel 1 (section 1) |
| lt3 | the same, 60 frames, every data word: the reverse audit (`lt3/reverse_deps.txt`, section 5) |
| rx1, rx2 | river dual runs before and after the subchannel routing fix |
| dg6 | the dungeon, three title cycles |
| mn1, mn2, ed1, ed2, sn1, sn2 | menus over the game; the editor and its preview; SNAPSHOT and the album, with the target watch |
| p30, p72, p144, p144b | pacing |
| ov3, ov4, ov5 | the native renderer's cost: per-image barriers, full barriers, device-memory uploads |
| rd2 | the dungeon in check mode: the bridge's per-draw reverse check |
| vv3, hz3, dl4, dl5, rl2 | validation, the hazard control, the slow consumer, image lifetime |
| tw4, tw5 | the target watch with the SPU probe off and on (tw3 kept from R3: the original read) |
| ab2/ | the off-switch A/B against the preserved checkpoint build |
| b4, b8, prod3 | the development builds of tw4/tw5 and of the last runs (with their PDBs), and the production build of the A/B |

## Reproducing

```
tools/r1_dev_lift.sh                       (the development lift: 97 observers, asp's state API among them)
tools/r3_scene.sh <tag> river_fight 100    (a dual run; NAME=value switches after the length)
tools/r3_scene.sh <tag> river_fight 75 DOTHEROES_PPFX_NATIVE=check PS3_ATTRIB_SHADOW=1 PS3_ATTRIB_ST=11100000-13400000 \
    RSX_FRAME_TRACE=arm:60:frametrace.txt RSX_FRAME_TRACE_WORDS=all DOTHEROES_PPFX_TRACE_LOAD=1   (the load's trace)
tools/r3_reverse_deps.py build/r3/<tag>/run/frametrace.txt
tools/r3_scene.sh <tag> river_fight 150 DOTHEROES_PPFX_COMPARE=0 DOTHEROES_PPFX_XCHECK=0 [DOTHEROES_PPFX_BARRIERS=full] [DOTHEROES_PPFX_UPLOAD=host]
tools/r3_scene.sh <tag> menus_r3 400       (and editor_r3 200; DOTHEROES_FPS=30|display|0, DIAG_VBLANK_MS=6.944 for pacing)
```

New switches: DOTHEROES_PPFX_BARRIERS=full, DOTHEROES_PPFX_UPLOAD=host,
DOTHEROES_PPFX_TRACE_LOAD=1 (with RSX_FRAME_TRACE=arm:...),
RSX_FRAME_TRACE_WORDS=all, SPU_IMAGE3_PROBE=1, GCM_SUBCH1_2D=0 (the old
subchannel routing).
