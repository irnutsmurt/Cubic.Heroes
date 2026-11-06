# R5: asp's draws described, rendered and bypassed (begun 2026-11-05)

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
