# R5: asp's draws described, rendered and bypassed (2026-11-05, complete)

R5 is the second half of the graphics replacement the owner authorised with
R4 (docs/NATIVE_PORT.md, docs/NATIVE_R4.md section 10): asp, the engine's
renderer -- about 230 draws a river frame -- becomes records the native
renderer draws, and then its command writers stop running. This document
follows R4's: what is measured [M], what is concluded from code [C], what is
inferred [I]; every run's evidence under build/r4/ (local, game-derived).

Steps, as for PPFX in R1-R4:

1. **Describe** asp's state at each of its draws from asp's own API -- its
   methods' arguments and the objects they point to -- in the explicit
   record format (ppfx_native.h `State`, libgcm's vocabulary), never from
   the command words it writes.
2. **Check** each description against the engine's state at the same draw
   (the bridge's cross-check), item by item, until nothing differs.
3. **Render** asp's records natively into the same images PPFX reads.
4. **Bypass** asp's command writers group by group, with R4's verify mode,
   markers and handbacks; measure which handbacks go and the cost against
   R4 and the production baseline.

Everything is opt-in: DOTHEROES_ASP_RECORD=1 (records and their
cross-check, with DOTHEROES_PPFX_NATIVE=check or =1), and the later steps'
switches; without them the build behaves as R4's.

## Summary (R5 complete, 2026-11-05)

- **Described**: asp's state at each draw is built from asp's own methods
  (sections 1-3; DOTHEROES_ASP_RECORD=1) and checked against the engine's
  item by item: the dungeon route's cross-check, 318 of 2.36 million draws
  differing (start-up fragment programs).
- **Rendered natively** (DOTHEROES_ASP_NATIVE=1; sections 4-6, 8): the
  river's scene against the engine's, 3,177 of 3,348 targets equal, 0.011%
  of pixels differing (the area's first frames, an occasional reflection
  band); vertex and index caches checked by the OS's write watch.
- **Shared** (DOTHEROES_ASP_SHARE=1): the scene's handoff from the engine
  goes -- once a frame in R4, once a run in R5.
- **Bypassed** (DOTHEROES_ASP_BYPASS=1, stage 1, validated): asp's draws'
  commands are taken back; six scenarios (19 million draws) with every
  marker met and every checked handback equal. Stage 2 (=2, its program
  uploads too) is valid in the river, not in the menus: experimental.
- **Performance** (section 8): uncapped 100 fps against R4's 91-95 and the
  production build's 136-137; capped as smooth as R4. The game's thread is
  now the bottleneck (the recording of asp's ~4,200 calls a frame).
- **Fixed on the way**: a device loss (images destroyed while the batch
  being recorded used them) and 50 ms frames every 3.4 s (static
  textures retired 256 at once).
- **Left** (section 9): the display buffer's handoff and handbacks, the
  HUD and menus' draws (which inherit asp's state and programs), the
  present, the snapshot, CPU-written surfaces, the occlusion query with
  real results -- R6, the next review checkpoint.

## 1. asp's writers and the state they set [M]

From R3's attributed frame trace (build/r3/lt3, 60 river frames, every
command word with the function that wrote it), the methods each of asp's
functions writes:

| Function | Writes | What it is |
|---|---|---|
| 0x006C01C4 | 0x0A70 | depth write mask (device, enable) |
| 0x006C49BC | 0x0A6C, 0x0A74 | depth test and function (device, func 0-7; 7 turns the test off) |
| 0x006C4D1C | 0x1830, 0x1834, 0x183C | cull (device, mode 0 none, 1 front, 2 back; clockwise front faces) |
| 0x006C048C | 0x0310 | blend off (device) |
| 0x006C5298 | 0x0310-0x0320 (and the blend colour) | blend on (device, src, dst, op, src alpha, dst alpha, op alpha) |
| 0x006C05AC | 0x0304 | alpha test off (device) |
| 0x006C4284 | 0x0304-0x030C | alpha test on (device, func 0-7, reference 0..1 in f1) |
| 0x006C4508 | 0x0328 | stencil test off (device) |
| 0x006C0A70 | 0x0328-0x0344 | stencil test on (device, func, ref, mask, fail, zfail, zpass) |
| 0x006C0E38 | 0x032C | stencil write mask (device, mask) |
| 0x006C4628 | 0x0A68, 0x0A78, 0x0A7C | polygon offset (device, factor f1, units f2; both 0: off) |
| 0x006C06CC | 0x1828, 0x182C | polygon mode (device, 0 point, 1 line, 2 fill) |
| 0x006C19A4 | 0x03B8 | line width (device, f1) |
| 0x006C02E8 | 0x0324 | colour mask (device, u8 rgba[4]); R3's |
| 0x006C0F60 | 0x0394/0x0398, 0x0A00/0x0A04, 0x0A20-0x0A3C | viewport (device, x, y, w, h, zmin f1, zmax f2) |
| 0x006C08F0, 0x006C5170 | 0x08C0/0x08C4 | scissor (device, x, y, w, h); scissor off |
| 0x006D11BC | 0x0324, 0x08C0 | a pass's end: all channels, scissor off |
| 0x006C8A34 | the surface (0x018C-0x02B8, 0x1D88) | render target set (a colour target: its CellGcmSurface at +0x40) |
| 0x006D1764 | the surface, 0x0324, 0x0A70/0x0A74, 0x0328 | a pass's start (a depth target: its CellGcmSurface at +0x2C; colour mask from +0x0C) |
| 0x006C2D3C | 0x1D8C-0x1D94 | clear |
| 0x006C37A0 | 0x1A08-0x1A1C (unit n) | sampler bind (device, unit, enable): asp's sampler table |
| 0x006CB070 | 0x1A00-0x1A18, 0x1840 | texture apply (this, {unit}, CellGcmTexture*) |
| 0x006D0E84 | unit 4's texture and sampler | the shadow map (a depth target's CellGcmTexture at +0x14) |
| 0x006D3190, 0x006D32D0 | 0x1680+, 0x1740+ | a vertex stream's bind; its unbind (size 0) |
| 0x006C9C74 | 0x1EFC+ | vertex constants (this, {start}, const float*, count) |
| 0x006CA3D0 | 0x0B80+, 0x1E9C/0x1EA0, 0x1EF8, 0x1EFC+, 0x1FF0 | vertex program upload and bind, with its own constants |
| 0x006CADE4 | an inline transfer into local memory | fragment program upload |
| 0x006C9624 | an inline transfer into a program | fragment constant patch |
| 0x006C99B0 | 0x08E4, 0x0B40+, 0x1D60, 0x1FF4 | fragment program bind (a program pair) |
| 0x006C6F88 | the index array, BEGIN/END, DRAW_INDEX_ARRAY | the draw |
| 0x00963EBC (the game's memcpy, from 0x006D0BE0) | 110 methods | asp's state store re-emitted |

## 2. How each argument maps: the setter probe [M]

DOTHEROES_ASP_PROBE=2 (development, ppfx_recorder.cpp asp_probe_enter/exit):
for each observed asp function but the draw, up to 24 distinct calls --
distinct by the command words they wrote -- with their arguments (r3-r10,
f1-f4), the first bytes of what r3-r5 point at (shader objects' containers
and parameter tables, the sampler table's entry), and the words written
between the call and its return. The development lift observes 14 more of
asp's functions for it (118 in all, config/dev_capture_observers.json).
Runs: build/r4/pr1 (river, 60 s), build/r4/pr2 (dungeon1_approach, 200 s:
the castle, the dungeon, the title and CONTINUE).

What the probe established, beyond the table above:

- **asp's enumerations.** Depth, alpha and stencil functions are 0-7 for
  NEVER..ALWAYS (0x0200 + n). Blend factors run in GL's order: 0 zero, 1
  one, 2-10 the nine 0x0300-0x0308, 11-14 the constant colour's
  0x8001-0x8004 (a constant-colour blend writes the blend colour too).
  Stencil operations run in D3D's order less one: 0 keep, 1 zero, 2
  replace, 3 incr, 4 decr, 5 invert, 6-7 the wrapping pair. The alpha
  reference is f1 x 255, truncated (0.5 -> 0x7F).
- **The viewport** maps depth with scale zmax - zmin and offset zmin (asp's
  0..1 clip space: scale 1, offset 0 in every call seen), where
  cellGcmSetViewport would halve both.
- **Surfaces.** asp's render targets are objects holding libgcm's own
  CellGcmSurface: the colour targets (vtable 0xFEAAA0: the 512x512 and
  1024x512 R5G6B5 targets with Z24S8 depth, linear) at +0x40, the depth
  target (0xFEAB88: the 1024x1024 shadow map, swizzled, colour target
  NONE) at +0x2C, with its CellGcmTexture at +0x14 for sampling.
- **Samplers.** The bind reads the unit's entry in asp's sampler table (TOC
  -0x2D88; the entries start at +0x0C, 48 bytes a unit): +0x03 the first
  mip level, +0x08 and +0x0C the magnification and minification filters
  (n - 1), +0x10 the anisotropy (a float ratio, to a code through the table
  at TOC -0x2D94 + 0x40), +0x14 and +0x18 wrap S and T (n - 1), +0x1C the
  border colour (four floats); R is clamped, the depth comparison GEQUAL,
  the maximum level 12, the convolution 1.
- **Vertex programs.** A shader object (0xFEAB48) holds its container
  ("CGB", +0x18) and microcode (+0x1C, the container's +0x20). The
  container gives the microcode's size (+0x08, high half) and the input
  mask (+0x0C, high half). asp loads and starts every program at slot 0
  (17 distinct programs in pr2, all at 0) with 32 registers; the object's
  own constants are +0x20 (count), +0x24 (12-byte entries, the register
  first) and +0x28 (the values) -- written with the upload (0x1EFC).
- **Fragment programs.** A fragment shader object (0xFEAB30) is uploaded
  into local memory at its +0x54 -- one slot, 0x01533D80, for every
  program: asp uploads each program again before its draws -- unchanged
  from the container's microcode. Its parameters (+0x70: 20 bytes each)
  list up to 8 offsets into the program (u16) and their count (+0x10); a
  constant patch writes the values, 16-bit halves swapped, at each. The
  bind takes the pair's fragment object (+0x1C): +0x58 the input mask,
  +0x5C the texture-coordinate input mask and its 2D mask, +0x64/+0x68 the
  shader control (the register count in +0x68's top byte).
- **Streams.** 0x006D32D0 is an unbind: it writes the input's format with
  size 0 (R5's first encoder took it for a bind).

## 3. The descriptions checked against the engine [M]

The bridge's cross-check (ppfx_bridge.cpp crosscheck, R3's) compares each
asp record with the engine's state at the draw it matches -- every item the
draw's programs read: the surface, viewport and scissor, both programs'
parameters and bytes, the sampled units' textures and samplers, the vertex
inputs the vertex program reads, the constants it reads, and the render
state with what the enables make irrelevant left out (now including blend
factors and equations, the alpha function and reference, the stencil
function, reference, masks and operations, and asp's polygon modes). The
river fight, check mode (DOTHEROES_PPFX_NATIVE=check DOTHEROES_ASP_RECORD=1),
60 s:

| Run | asp records (matched) | with any difference | What still differed |
|---|---|---|---|
| as3 (R5's first step) | 2,063,903 | all | programs, render state, samplers, viewport, scissor, surfaces, vertex inputs |
| as4 (the setters' encoders) | 647,669 | all | every vertex input off; scissor; viewport depth; 178 fragment programs |
| as5 (the store; unbinds after the draw) | 378,218 | 133,638 | the shadow pass's scissor; 185 fragment programs |
| as6 (the pass's inset scissor) | 382,651 | 354 | the first recorded frame's scissor (180); 191 fragment programs, all in the first three frames |
| as7 (the dungeon route, 200 s) | 2,218,291 | 13,063 | as as6, and unit 1's sampler in one area (12,464) |
| as10 (the dungeon route; the bind's write-if-changed model) | 2,210,831 | 12,698 | as as7: unit 1's sampler still open (12,095) |

The first frame's differences are a start-up gap of the description, not
of a state item: the store's initial contents come from asp's set-up, not
from a setter the recorder saw (the restore now leaves alone a group no
setter has set), and the fragment program bound in those frames was
uploaded before the recorder saw an upload to that offset (184 draws whose
bound program is not the one last uploaded there). Unit 1's sampler in the
dungeon area is open: its nearest filter and zero border come from the
store's re-emission (section 3's third finding), but modelling the bind's
write-if-changed and the rebuild's reset did not reach those draws; the
next check counts the rebuilds and the samplers restored.

Two findings along the way:

- **asp's draw unbinds its streams after the draw command** (0x006D32D0,
  called from the draw method through 0x006D38CC): a record built at the
  draw's return saw every input off. The unbind inside a draw now takes
  effect after the record has taken its state.
- **asp's state store is re-emitted over other code's state.** asp re-emits
  ranges of its store into the command buffer (its block append, 0x006D0BE0)
  -- the whole store as a pass begins, the alpha pair after a clear -- and
  so restores its own viewport and scissor after PPFX set PPFX's. The
  recorder keeps the store as items (every setter writes its item into both
  the command buffer and the store; a pass's own settings only into the
  command buffer), and a re-emission brings back the items whose slots (the
  setters' offsets in asp's table at TOC -0x2D98) lie in the range: 254,360
  groups restored in as5.

PPFX's own records are unaffected: their cross-check stays at 0 differences
(as4: 705,712 draws; as5: 412,334), the asp encoders change only asp's
items, and PPFX sets everything its draws use itself.

## 4. asp's draws and what native rendering needs [M, C]

**The draw** (0x006C6F88, its object's vtable 0xFEA9D0; the draw probe,
build/r4/as6): +0x08 the primitive in libgcm's numbering (5 triangles, 6
strip, 8 quads), +0x0C the index count, +0x10 the vertex count, +0x18 and
+0x1C the index buffer's address and local offset, +0x24 the index type (2:
16-bit). It binds stream 0 itself, writes DRAW_INDEX_ARRAY in batches of
256 from index 0 (DRAW_ARRAYS for a draw without indices), and unbinds its
streams after the draw command.

**What step 3 needs** (the native renderer draws PPFX's records only, with
blending, depth, stencil, alpha test and culling refused):

- *Depth and stencil attachments*: native depth images at a surface's
  depth offset (Z24S8 mostly; the shadow map, swizzled, colour target
  NONE), sampled later as a texture (unit 4, format DEPTH24_D8, the depth
  comparison GEQUAL).
- *The full render state in pipelines*: blend (factors, equations, the
  constant colour), depth, stencil, cull and front face, polygon offset and
  mode, the alpha test (the translator's alpha-test epilogue), the colour
  mask.
- *Indexed draws from guest memory*: asp's meshes live in local memory
  (static after a load; some streams rewritten per frame). A snapshot per
  draw, as PPFX's quads are, would copy megabytes a frame on the game's
  thread. The engine's own answer (rsx_draw_engine.c) is a cache of guest
  ranges checked by content hashes -- a sparse hash every use, a full hash
  spread over frames -- which the native renderer can keep on its own
  thread for vertex, index and texture data alike.
- *Textures*: asp samples dozens of static textures (DXT and swizzled ones
  included) from local memory: the same cache.
- *asp's clears* (0x006C2D3C: flags r4 -- 1 colour, 2 depth, 4 stencil --,
  the colour at r5, depth f1, stencil r6; and a pass's own clear) as
  records.
- *Ordering with PPFX*: asp's scene goes into the target PPFX then reads
  (its kept cellGcmSetSurface): once asp is native, that target's handoff
  (engine -> native) is no longer needed -- the first handoffs R5 removes.

**A third finding: asp's sampler bind writes only what changed** (the
dungeon route, as7: unit 1's filter and border differed in 12,464 draws of
one area). An attributed frame trace there (build/r4/ft1,
RSX_FRAME_TRACE at 110 s) shows unit 1's nearest filter and zero border
written by the store's re-emission (through a second memcpy, 0x00963C80),
never by the bind: the bind writes a unit's address mode, filter and border
only when they differ from what it last wrote, while the global state
block's rebuild (0x006D0B98) resets the store's samplers to nearest, wrap,
disabled. A unit whose values asp believes current thus samples the store's
reset values after the next re-emission -- on the PS3 as here. The recorder
now models exactly that: what the bind last wrote per unit, the rebuild's
reset values in the stored copy, and the samplers' slots in a re-emitted
range.

## 5. asp's draws rendered natively: the first runs [M]

DOTHEROES_ASP_NATIVE=1 (with DOTHEROES_PPFX_NATIVE=1 and
DOTHEROES_ASP_RECORD=1; development): the native renderer draws asp's
records too (ppfx_render.cpp asp_draw), into its own images, while the
engine still draws everything (R3's arrangement: PPFX's inputs from asp
still arrive by handoff). New in the renderer: depth images (Z24S8, Z16;
the shadow map sampled through its depth view, the comparison in the
translated shader), passes with a colour and a depth attachment or either
alone, pipelines with the whole render state (blend, depth, stencil, cull,
polygon offset, the alpha test in the fragment program), indexed draws
(16- and 32-bit indices, strips and quads rebuilt as lists), vertices and
indices read from guest memory on the native thread, and textures from
guest memory cached by their description and checked by a content hash
once a frame -- DXT1/3/5 as BC1/2/3.

| Run (river, 60 s) | asp draws drawn | not drawn | PPFX's compared outputs |
|---|---|---|---|
| an1 | 211,571 | 399,609 (no texture source) | 6,504: 6,498 equal, 6 differ |
| an4 (DXT textures) | 897,011 | 161 (the first frames' fragment program) | 16,083: 16,082 equal, 0 differ, 1 unfinished |

Not done yet: **no comparison of asp's native output with the engine's**
(the next step: at the handoff that brings the engine's scene target to
PPFX, compare it with the native image asp's records drew), and **the
cost**: every draw converts and uploads its vertices (76 GB in an4, about
21 MB a frame), where the engine keeps converted vertices in a cache
checked by content hashes -- the native renderer needs the same before
any timing. The blend colour (asp's constant-colour blends) is not
described yet.

**The first comparisons of asp's output** (build/r4/an5, an6; the dumps
converted with a half-float reader): without asp's clears every pixel
differed (the native depth buffer was never cleared: every fragment
failed); with them (an6, 7,274 clears, 4,269 dropped by a two-clear
buffer between draws -- to widen) 93% differ: the geometry is in place
(the castle, walls and bridge exactly where the engine draws them), the
shading about twenty times too dark, the ground missing. Next: compare
asp's draws one by one -- the bridge's copy of the engine's target after
a matched draw (R3's mechanism for PPFX) for every asp draw of a chosen
frame, so the first draw that differs names the state at fault.

**Comparing draw by draw** (DOTHEROES_ASP_COMPARE_DRAWS=<n>: the bridge
copies the engine's target after every asp draw of every n-th frame, as R3
did for PPFX; the report names each frame's first differing draw per
target, with dumps; DOTHEROES_ASP_DESCRIBE=<seq> writes a record's
translated programs, constants and inputs) found the cause of the dark
scene: asp's fragment programs sample the shadow map (unit 4, a depth
texture, compared in the shader), and the shadow pass's own depth clear --
written inline by the pass's start, not through asp's clear -- was not
recorded, so the native shadow map was never cleared and every pixel came
out in shadow. With that clear (build/r4/an11): **6.9% of the scene
target's pixels differ** (from 89.8%), the mean brightness 0.233 against
the engine's 0.263. Still different: the engine's float target holds
values up to 4.7 (and negative alpha) where the native output stays within
0..1 -- an additive or constant-colour blend, or an effect not yet drawn
natively, is the next thing the draw-by-draw comparison has to name. The
winding is as the engine maps it (flipping it: 99.99% differ). asp also
clears surfaces it then does not draw into (1,650 clears dropped past the
32 a draw keeps; harmless if they are older than a later clear of the same
surface, to be made exact).

**The engine's depth format, and where the rest differs** (build/r4/an13,
an14; DOTHEROES_ASP_DESCRIBE_FRAME=<frame> writes a line per asp draw of
that frame and every comparison of it): the engine keeps every guest depth
surface as D32_SFLOAT_S8_UINT (its first choice); with the same format the
native renderer's depth tests resolve as the engine's, and **asp's scene
target ends frame 2500 equal to the engine's, pixel for pixel**. What
still differs at the handoff (about 88,000 of 921,600 pixels, the same in
every frame) is the moat: the water samples the 512x512 reflection target
(0x010B3D80, R5G6B5 with Z24S8, drawn with the stencil test), which
differs from its first draw after its clear -- the stencil's state or its
clear is the next suspect.

**The reflection pass, open** (build/r4/an15): the 512x512 target's first
draw of frame 2500 comes right after asp's clear of colour, depth and
stencil (mask 0xF3, stencil 0) and draws with the stencil test EQUAL 1
(ref 1, masks 0xFF, every operation KEEP) -- the engine's own state at that
draw says the same (the cross-check). By that state no pixel passes; the
native renderer draws none, the engine draws 290. Either an earlier write
of the stencil is not described, or the engine's stencil differs from its
state (an aliased surface, a clear it does not apply): to be settled with
the engine's stencil read back at that draw before calling either side
right. Everything else asp draws in that frame matches the engine exactly.

## 6. The gaps closed (2026-11-05, second round) [M, C]

**The reflection's stencil: the reference was right; the game's own draws
were missing.** A log of every draw in the frame that is no record's
(DOTHEROES_ASP_DESCRIBE_FRAME also writes foreign_f<frame>.txt, the
engine's state at each) found 29 in frame 2500: 13 into the reflection
target with the colour mask off, depth off, the stencil test ALWAYS with
REPLACE 1 -- the water's stencil mask the reflection then tests EQUAL 1
against -- 2 more into it, and 14 into the scene target. They are the
game's own cellGcmSetDrawIndexArray (0x006A2164), drawn inside asp's scene
with asp's programs and state. R5 now records them with asp's records
(observed in the development lift, 119 observers; a record built at the
call's return from its arguments: mode, count, index type, location,
offset, the vertex count from the largest index), drawn natively the same
way. The HUD and menus, drawn with the same call after PPFX, are R6's.

**Unit 1's sampler in the dungeon: asp's dirty flags.** asp's sampler bind
does not compare values: each unit's entry in its sampler table carries
dirty bytes (+0x2E: the address mode and border colour; +0x2D or +0x2C:
the filter), set by asp's setters and cleared by the bind once written
(the lifted 0x006C37A0). The recorder reads them at the bind's call;
with a store rebuild's reset values for the units no bind rewrites, the
dungeon route's unit-1 difference is gone (as10: 12,095 draws; c31d: 0).

**The start-up gaps.** The store's rebuild (0x006D0B98) resets its scissor
to the whole range; a program asp binds without uploading it is one it
keeps resident in its slot (its microcode is taken from the object, the
constants asp patched into it since the slot last changed program kept);
asp's clears are kept per surface (a later clear of the same surface and
aspects replaces an earlier one: 3,796 merged, 0 dropped in c31n).

**The vertex cache** (DOTHEROES_ASP_VCACHE, on by default with
DOTHEROES_ASP_NATIVE): converted vertices and rebuilt index lists in
arenas of video memory, found by the streams' descriptions, the counts
and a hash of the guest bytes taken at every use (a rewrite of the game's
data is a different entry); the oldest arena retired whole past 512 MB.
River (c31n): 397,077 vertex hits and 606 misses, 397,473 index hits and
238 misses; converted vertices 81 MB over the run (before: 26-76 GB);
22.6 GB hashed (about 6 MB a frame).

**Where asp's native output stands** (c31n, the river, 50 s): at the
handoff that brings the engine's scene to PPFX, 567 of 721 compared
frames equal, 0.17% of all pixels differing; draw by draw, frame 2500 ends
equal on both targets. The frames that differ do so in the moat's band.
The dungeon route's cross-check (c31d): 2,355,742 asp records, 318
differing (fragment programs at start-up, addressed next); PPFX's own
records 0 differences in 2,639,501.

**The moat, and data the game rewrites after drawing** (build/r4/fh*,
the river, 60 s): what remained differed only in the moat's band, at
times. Measured, step by step:
- vertex and index bytes do not change between the game's draw and the
  native draw (DOTHEROES_ASP_RACE_CHECK=1: 100,665 draws checked, 0
  changed);
- the engine's own texture check (a sparse hash a frame, full every 8)
  explains a little (PS3RECOMP_ENG_TEXHASH_FULL=1: 0.17% -> 0.11%);
- the water samples a small linear A8R8G8B8 texture in local memory the
  game rewrites after issuing the draw: taken when the game drew (at the
  record) it was worse (0.11% -> 4.3%), taken by the walker at the draw's
  place in the stream -- when the GPU reads it, as the engine does -- it is
  right: **0.016% of the scene's pixels differ, 1,118 of 1,271 frames
  equal**. asp's small uncompressed textures (up to 256 KB) are now taken
  that way (the bridge's guest_texture, R4's warp fix); DXT textures and
  surfaces are not (static, or native images).
- The vertex cache needed two fixes on the way: its arenas in plain video
  memory filled by copies (the CPU-writable window into video memory is
  256 MB without resizable BAR, and a second arena failed: 30 GB of
  vertices were converted per run outside the cache), an entry admitted
  only when the same source holds the same bytes a second time, and only
  the vertices a draw references hashed and converted (the game's draws
  index small parts of large shared buffers). River (fh8): 685,856 vertex
  hits, 1,104 misses, 226 MB converted per run.

Still differing: the first two recorded frames (37,513 and 87,802
pixels), a few frames of 1,000-8,500 pixels (the moat's band), and the
steady frames by tens of pixels.

## 7. Shared images and the bypass [M]

**Sharing** (DOTHEROES_ASP_SHARE=1): a surface asp's records drew this
frame, and no other work wrote since, is not handed off -- PPFX reads the
native image asp drew. River, 50 s (build/r4/sh2), handoffs by surface:
asp's scene target (0:01AE0000) **once in the run** (its first use, before
asp had drawn it), against once a frame without sharing; the only handoffs
left each frame are the two display buffers (0:00000000 and 0:00398000,
373 and 374), which the engine's own HUD and menu draws write -- R6's; the
rest once each at start. 2,986 handoffs shared. In R3's arrangement
(sh1, the engine still drawing everything, PPFX's outputs compared):
5,514 of 6,566 outputs equal, 1,057 of 1,193 final composites -- the
differing ones downstream of the frames where asp's native scene still
differs (section 6: 0.016% of its pixels), spread by PPFX's blurs.

**The bypass, stage 1** (DOTHEROES_ASP_BYPASS=1, with R4's
DOTHEROES_PPFX_BYPASS=1 and sharing): asp's draws and the game's draws in
its scene run as before, and at their return the commands they wrote --
theirs and those of the calls inside them -- are taken back and an R4
marker written in their place; the engine draws nothing of asp's, the
native renderer all of it. River, 60 s (build/r4/by1): 709,958 draws
bypassed, 37.6 million command words taken back, 5 kept (their commands
wrapped the ring); markers 769,831 written, 769,141 met, 0 unknown;
2,547 handbacks, every 30th checked: 85 equal; no failure.

**Stage 2** (DOTHEROES_ASP_BYPASS=2): asp's state functions and program
uploads too -- the commands they write into the frame's command buffer
taken back at their return (not inside a draw, not when they wrapped the
ring or a marker was written inside them; the state store asp builds in
its own buffer left alone). Its check: the engine's remaining draws (the
HUD and menus) must run with the same states as with stage 1
(DOTHEROES_ASP_FOREIGN_STATES=<file>: the distinct engine states of every
draw that is no record's).

**Stage 1 through R4's scenarios** (build/r4/s5*, every 30th handback
checked; r5_stab.sh): river 100 s (2.30 M asp draws bypassed, 245
handbacks checked, all equal), dungeon route 200 s (2.40 M, 416), menus
300 s (5.00 M, 591), fields 200 s (1.77 M, 244), snapshot and album 200 s
(2.53 M, 309), the field after the dungeon 250 s (5.34 M, 781): every
marker met, no failure. The field route first hung (s5after): the record
queue (1,024) filled at an area's first frames, the producer waited for
room, and the walker could not reach the markers that would make it --
they were in commands the game had not flushed yet. Fixed: the queue holds
4,096 records (asp's add about 250 a river frame), a full queue first makes
the commands written so far visible (the put register moved forward, as
cellGcmFlush would), and a bypassed draw's commands are taken back before
anything can expose them (s5after2 showed the order mattered: 99,742
markers met unknown). s5after3: 14,773 frames, the queue never full.

**Stage 2, first try: asp's state is inherited by the engine's remaining
draws** (build/r4/st1-*, st2-*, DOTHEROES_ASP_FOREIGN_STATES). With every
asp state function's commands taken back (44 million calls, 859 million
words in the menus run), the river's remaining draws ran with exactly the
same engine states (13 of 13), the menus' did not: with stage 1, menu draws
into the display buffers ran with blending (SRC_ALPHA, 1-SRC_ALPHA) and the
alpha test (GREATER 0) on, the depth function LEQUAL and clockwise front
faces -- asp's last settings -- and one drew into asp's scene target, on
asp's surface; without asp's state commands those states were gone. The
engine's own HUD and menu draws inherit asp's render state and surface:
taking that state away would change what the menus show. So stage 2 is
the program traffic alone -- asp's vertex program uploads, fragment
program uploads and their constant patches, most of its words -- checked
by the same comparison, now with each remaining draw's vertex program,
fragment program and the constants it reads. Taking asp's render state
back too (=3) waits for R6, which makes those draws native.

**Stage 2, programs only: valid in the river, not in the menus**
(build/r4/sb*, sc*; stage2_check.sh). The river's remaining draws ran with
the same states, programs and constants with stage 2 as with stage 1 (519
of 519); stage 2 took back 1.67 million calls, 65.6 million words, on top
of stage 1's 37.6 million. The menus' did not (469 states with stage 1,
523 with stage 2, 4 and 58 found in one only): menu draws straight into
the display buffer, depth on, use vertex program slot 0 and asp's
fragment program slot (01533D80) without loading programs of their own --
they run whatever asp loaded last, so with asp's uploads taken back they
ran stale programs. Keeping the uploads after the frame's final composite
(sc*) did not close it: in the menus those draws come without one. A
correct stage 2 needs to know, when asp uploads, whether an engine draw
will run that program next -- which only making those draws native (R6)
settles. Stage 2 stays experimental (=2); stage 1 is R5's validated
bypass.

**R4's exit wait** (section 11 of NATIVE_R4.md): at exit the native thread
no longer waits 10 s for an engine submission that will not come; after
100 ms the batch goes without it (counted: exit_signal_skips).

**Stage 1 again, on the faster native path** (build/r4/s6*, b62, section
8's changes; every 30th handback checked): river 100 s (2.29 M asp draws
bypassed, 245 handbacks checked), dungeon route 200 s (2.35 M, 394), menus
300 s (5.06 M, 595), fields 200 s (1.75 M, 243), snapshot and album 200 s
(2.55 M, 310), the field after the dungeon 250 s (5.14 M, 784): every
checked handback equal, every marker met (none unknown), no failure, no
device loss. The river's comparison with the engine (R3's arrangement,
build/r4/fc1, 100 s): 3,177 of 3,348 scene targets equal, 0.0111% of
pixels differ (fh13 before: 1,118 of 1,271, 0.0158%).

## 8. Performance [M]

**The first measurement** (build/r4/perf5, b51; build/r4/r5_perf.sh; the
river fight, real audio, comparisons, cross-check and handback checks off,
arms alternated): the production reference, R4, and R5 (asp native, its
images shared, its draws bypassed) with bypass stage 1 and stage 2.
Capped, every arm held 60 fps on average, but R5's fight had 7-25 frame
intervals over 25 ms a run (p99.9 30-50 ms) against R4's 0-1, and
uncapped R5 ran at 77-80 fps against R4's 94-99 and the reference's
139-142. The native thread recorded asp's draws for 10.9 ms a frame (R4:
0.4 ms) and the walker waited for it 9.5-10 ms a frame before the display
handbacks.

Three of those R5 runs also **lost the Vulkan device** about 40 s in
(vkQueueSubmit -4; the engine's presents failed with it): their numbers
are not valid, and the cause was R5's -- see below.

**Where the native thread's time went** (DOTHEROES_ASP_PROFILE=1: asp_draw
timed by part; river 90 s, ~1.9 million draws a run, build/r4/pf*):

| Step | µs a draw | What changed |
|---|---|---|
| pf1 (b52) | 17.2 | -- (hashing 4.3: the vertex cache checked each draw's vertex and index ranges, 31 MB a frame; indices 4.2: every index decoded each draw; textures 3.2; a render pass a draw 2.2) |
| pf2 (b53) | 12.3 | indices: an index list's range [vmin, vmax] kept by its bytes' hash, decoded only to build it (4.2 -> 0.45); walk-time texture snapshots hashed with the fast hash (textures 3.2 -> 1.7); a range hashed once a frame (saved only 10%: the river's draws read 30 MB of distinct ranges a frame) |
| pf5 (b56) | 8.2 | **the OS's write watch** on the guest space (port/main.cpp reserves it with MEM_WRITE_WATCH under DOTHEROES_ASP_NATIVE=1): a range nobody wrote since it was hashed is not read again -- 117 GB of 117.3 GB not re-hashed; a poll a frame over the 82 page runs the caches rely on, 0.1 ms. GetWriteWatch's cost follows its output array, not the span (0.7 ms for a million entries, 1 µs for 17; build/r4 microbenchmark), so each run's array is sized to the run |
| pf7 (b58) | 5.8 | **one render pass for consecutive draws into the same targets** (98.7% of draws join the open pass; anything that must be outside a pass -- a barrier, a copy, a clear, a query reset, another pass, the command buffer's end -- ends it first, through wrapped entry points); commands 3.1 -> 0.7 |
| pf10 (b61) | 5.3 | a fragment program's analysis (units, structural hash, constants) and a vertex program's input mask kept by the program's content hash (pipeline 1.4 -> 0.8) |

**Two bugs found on the way:**

- **The device loss**: a retired image was destroyed once no batch was in
  flight, but the batch still being recorded could already use it (the
  static-texture table retired all its images at 256). Images retired
  while a batch is open are now destroyed after that batch completes.
  R4's PPFX path shared the hazard (its static textures too); not seen in
  R4's runs.
- **The 50 ms frames every 3.4 s**: the water's texture changes every
  frame, so its walk-time snapshot made a new image a frame; at 256 the
  table retired them all at once -- first every texture (re-uploaded
  within a frame), then, with a "not used for 8 frames" rule, the 200
  stale ones (their memory freed together). Swept once a frame now.

**The final measurement** (build/r4/perf6, b62; the same arms and
settings as perf5; no device loss in any run). Capped, the fight's guest
frame intervals (tools/r4_fight.py; three runs each, ~5,000 frames a run;
stage 2's first run is left out -- the bot route did not reach the fight,
so it measured the whole run, area loads included):

| Arm | p50 | p99 | p99.9 | max | over 25 ms |
|---|---|---|---|---|---|
| reference | 16.7 | 17.3, 17.3, 17.3 | 18.1, 17.7, 19.4 | 22.0, 23.3, 22.5 | 0, 0, 0 |
| R4 | 16.7 | 17.4, 17.5, 21.1 | 21.5, 21.8, 24.7 | 24.2, 24.0, 28.3 | 0, 0, 4 |
| R5, stage 1 | 16.7 | 17.4, 17.5, 17.4 | 22.0, 21.0, 21.6 | 50.2, 23.8, 27.6 | 1, 0, 1 |
| R5, stage 2 | 16.7 | -, 17.6, 17.5 | -, 21.9, 22.0 | -, 24.0, 25.7 | -, 0, 2 |

Every arm holds 60.0 fps; R5's tail is R4's (one 50 ms interval in nine
runs' 45,000 frames).

The native side's costs per frame with records (capped):

| Where | R4 | R5, stage 1 | R5, stage 2 |
|---|---|---|---|
| the game's thread: the recorder | 0.81-0.83 ms | 1.01 ms | 1.06-1.12 ms |
| the walker: the bridge (of it waiting for the native side) | 0.91-1.30 (0.64-0.88) ms | 2.91-2.96 (2.41-2.44) ms | 3.25-3.29 (2.76-2.84) ms |
| the native thread (recording) | 0.40 ms | 3.55-3.60 ms | 3.56-3.59 ms |
| the native GPU | 0.55-0.58 ms | 1.99-2.01 ms | 2.01 ms |

Uncapped, throughput (two runs each; the frame trace, tests/walker_breakdown.py):

| Arm | fps | interval p50 / p99 | walker over a frame: decoding / at the present (of it waiting for the previous batch) / asleep | the game's thread busy (idle) |
|---|---|---|---|---|
| reference | 137.0, 136.2 | 7.0 / 11.0-11.5 | 5.44 / 0.10 / 1.63 ms | 6.2 (1.02) ms |
| the development build, native paths off | 134.4, 128.4 | 7.0-7.4 / 12.1-12.2 | 5.51-5.88 / 0.10 / 1.72 ms | 6.6-6.7 (0.78-1.01) ms |
| R4 | 90.6, 94.8 | 10.5-10.8 / 14.5-20.2 | 6.36 / 2.46 (2.40) / 1.61 ms | 8.1 (2.32) ms |
| R5, stage 1 | 100.3, 100.2 | 9.4 / 15.5-16.0 | 2.29 / 1.52 (0.02) / 5.90 ms | 9.7 (0.09) ms |
| R5, stage 2 | 100.8, 101.1 | 9.3-9.4 / 15.7-15.9 | 1.80 / 1.90 (0) / 6.00 ms | 9.7 (0.08) ms |

**What the numbers say [M, I].** R5 is faster than R4 (100 fps against
91-95 uncapped) and as smooth capped, and still slower than the production
build (136-137). The walker, R4's bottleneck, now decodes 2.3 ms a frame
instead of 6.4 (asp's commands are gone from the ring) and sleeps 5.9 ms;
its wait for the previous batch at the present is gone. The critical path
has moved to the game's thread, which never waits now and is busy 9.7 ms
a frame against the production build's 6.2: the development lift's
observer hooks cost ~0.5 ms (native paths off), R4's recording of PPFX
~1.5 ms more, R5's recording of asp ~1.6 ms more (the recorder's own timer
counts 1.0 ms of it; the rest is asp's ~4,200 observed calls a frame, each
entering and leaving an observer, the state store's mirror, and the
programs interned by a byte-wise hash). That is R5's next cost to cut --
the observers and the record's copies (8 KB of vertex constants a draw) --
and, in R6, asp's draw bodies themselves need not run once their commands
are never kept. The native thread (3.6 ms a frame) and the walker's wait
for it at the display buffer's handbacks (2.4 ms) are off the critical
path while the game thread is slower, and go with R6's native present.

## 9. What R5 leaves, and R6's work [M, C]

**The handoffs** (engine -> native copies) and **handbacks** (native ->
engine), per river frame (build/r4/perf5, perf6 reports):

| | R4 | R5 |
|---|---|---|
| handoffs: asp's scene target (0:01AE0000) | 1 a frame (6,655 a run) | 0-2 a run (its first use) |
| handoffs: the display buffers (0:00000000, 0:00398000) | 1 a frame | 1 a frame |
| handbacks | 2 a frame (kind "target": the engine about to draw into a surface the native side wrote) | 2 a frame (the display buffer: at the engine's clear and at the present) |
| command words the walker decodes for asp | all | stage 1: asp's draws' words taken back (37.6 M a river minute); stage 2 (river only): its program uploads too (65.6 M more) |

What remains each frame is the display buffer: the engine still draws
the HUD and the menus over the final composite and presents it. R6
removes it.

**R5's own open items** (none blocks R6):

- **Start-up frames**: the area's first frames differ by a few draws; 10
  draws a river run (records 1166-1175, the area's first frame) are not
  drawn ("asp: pipeline": their fragment program's buffered constants
  cannot be collected -- 48 bytes, the first word 1E7E7E00). Later frames
  match (frame 2500 draw by draw, section 6).
- **Occasional moat-band frames** in the river (1k-8k pixels of a frame,
  the water's reflection band) and steady small differences (tens of
  pixels): 171 of 3,348 targets differ, 0.011% of pixels.
- **Large textures** (over 256 KB) are read when the native thread draws,
  not at the walk: a texture the game rewrote after drawing with it in the
  same frame would be read late (seen only for the water's small one,
  which is taken at the walk).
- **The water's texture** makes a new image a frame (its snapshot, keyed by
  content); a pool of same-sized images would avoid the allocations.
- **Stage 2** (asp's program uploads) is valid in the river, not in the
  menus (section 7): it waits for the menus' draws to be native.
- **The walker still waits for the native side** at the display buffer's
  handbacks (section 8): the native thread's per-frame recording is on the
  critical path until R6 presents natively.

**R6** (docs/NATIVE_R4.md section 10b, with what R5 found):

1. **The game's libgcm draws**: the HUD, menus, dialogue boxes, map and
   editor screens (0x006A2164 from 0x00024720, and draw-arrays draws) --
   records, rendered natively, then bypassed. They inherit asp's render
   state and run asp's last-loaded programs (vertex slot 0, fragment slot
   01533D80): their records must carry that state explicitly. With them
   native, bypass stages 2 (programs) and 3 (all of asp's state) become
   valid everywhere.
2. **The present**: the native display image presented directly, with the
   same pacing (DOTHEROES_FPS, the vblank clock, the flip's labels and
   semaphores). This removes the last per-frame handoff and both handbacks.
3. **The snapshot** (cellJpgEncEncodePicture) reads the native display
   image.
4. **CPU-written surfaces** (movies, any guest write into a surface) read
   from guest memory into native images -- the write watch (section 8)
   can tell when.
5. **The occlusion query** (PPFX's effect object, 0x006A1D1C and report
   769; docs/NATIVE_R4.md section 1). The owner's decision (2026-11-05):
   correct native behaviour with real results. Today the engine writes a
   fixed "visible" count and never carries out the copy into the game's
   result buffer, so the poll finds the result pending forever and the
   query is issued once an area. R6 issues a Vulkan occlusion query around
   the counted draws in the native pass, writes the report with the real
   sample count (and its timer) into the result buffer when the GPU has
   it, and honours cellGcmSetRenderEnable natively (conditional rendering,
   or the previous frame's result). This changes what the effect shows --
   intended.
6. **R4's bridge** (markers, handbacks, the touch hooks) goes once nothing
   is left to order against; R7 (retirement) follows.

## Reproducing

```
tools/r1_dev_lift.sh                                   (the development lift: 119 observers)
tools/r4_scene.sh <tag> river_fight 100 DOTHEROES_ASP_RECORD=1 DOTHEROES_ASP_NATIVE=1 DOTHEROES_ASP_COMPARE=1
        (asp native beside the engine, each frame's scene compared; DOTHEROES_ASP_COMPARE_DRAWS=<n> draw by draw)
tools/r4_scene.sh <tag> dungeon1_approach 200 DOTHEROES_PPFX_NATIVE=check DOTHEROES_ASP_RECORD=1   (the cross-check)
tools/r4_scene.sh <tag> river_fight 90 DOTHEROES_PPFX_BYPASS=1 DOTHEROES_PPFX_COMPARE=0 DOTHEROES_PPFX_XCHECK=0         DOTHEROES_PPFX_HANDBACK_CHECK=0 DOTHEROES_ASP_RECORD=1 DOTHEROES_ASP_NATIVE=1 DOTHEROES_ASP_SHARE=1         DOTHEROES_ASP_BYPASS=1 DOTHEROES_ASP_PROFILE=1      (R5 with asp_draw's time by part)
TAG=s6 sh build/r4/r5_stab.sh <build> 1                (stage 1 through six scenarios; local script)
TAG=sc STAGES="1 2" sh build/r4/stage2_check.sh <build>  (stage 2 against stage 1, the engine's remaining draws' states)
PERF_DIR=perf6 sh build/r4/r5_perf.sh <build> 3        (the measurement; local script, its arms in section 8)
sh build/r4/r5_plain.sh <build> 2                      (the development build, native paths off)
```

Switches: DOTHEROES_ASP_RECORD=1, DOTHEROES_ASP_NATIVE=1, DOTHEROES_ASP_SHARE=1,
DOTHEROES_ASP_BYPASS=1|2|3, DOTHEROES_ASP_VCACHE=0, DOTHEROES_ASP_WWATCH=0,
DOTHEROES_ASP_WWATCH_US=<n>, DOTHEROES_ASP_PASS_MERGE=0, DOTHEROES_ASP_PROFILE=1,
DOTHEROES_ASP_COMPARE=1, DOTHEROES_ASP_COMPARE_DRAWS=<n>,
DOTHEROES_ASP_DESCRIBE_FRAME=<frame>, DOTHEROES_ASP_DESCRIBE=<seq>,
DOTHEROES_ASP_RACE_CHECK=1, DOTHEROES_ASP_FOREIGN_STATES=<file>,
DOTHEROES_ASP_DUMP_FROM=<frame>, DOTHEROES_ASP_CULL_FLIP=1.
