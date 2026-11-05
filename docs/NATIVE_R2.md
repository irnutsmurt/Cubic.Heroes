# R2: the whole PPFX chain, rendered natively (2026-11-04)

Milestone R2 of NATIVE_PORT.md, approved by the owner on 2026-11-04 after
R1. **R2 removes nothing from the game's active graphics path**: the game
renders exactly as before. R2 proves that PPFX's complete per-frame chain,
captured from the running game, runs through the standalone native Vulkan
executor -- every intermediate image produced by the executor's own earlier
passes, only the chain's external inputs seeded -- frame after frame, with
every pass's output bit-exact against the existing renderer's.

Labels as elsewhere: **[M]** measured, **[C]** from code, **[I]** inferred,
**[U]** unknown.

## The chain

PPFX draws **47 times a frame**, as the evidence stage counted -- not the
46 from pass 3 to pass 48 that R2's first capture window covered. Pass 1 is
fragment program 0x018F0000 drawn into the scene target 0x01AE0000, with no
textures, before asp renders the scene over it (asp's pass 2: 193-202 draws
a frame in the river fight, 138 in exploration). Every frame of every
capture has exactly 47 draws through PPFX's draw path (0x006A1E28 from
0x007B6C38) and 47 fragment-program binds [M, boundary logs]. The 19
programs R1's trace counted are these 47 draws' programs: pass 1's
0x018F0000 is the 19th.

| | |
|---|---|
| Draws | pass 1 (1280x720, into the scene target, before asp); passes 3-48 (46 draws after asp's scene: passes 11 and 12 at 1280x720, 5 at 640x360, 32 at 320x180, 3 at 160x90, 3 at 80x45, and the final composite, 1280x720 into the display buffer, 0x0 and 0x398000 in turn) |
| Formats | R16G16B16A16_SFLOAT for every target but the display buffer (R8G8B8A8_UNORM); one R8_UNORM texture |
| External inputs | the scene target as asp leaves it (read by passes 3 and 11), and a 64x64 R8 texture (guest 0x01AD6200, format B8, read by pass 44, never written by the chain) |
| History | none: nothing the chain reads was last written by the chain in an earlier frame [M, every frame of every capture] |
| Feedback | pass 11 composites into the scene target while sampling it: the renderer copies it first (a snapshot image); the executor does the same |
| Blending | none: every PPFX blend enable is 0 |
| Resources | 38 images (40 in exploration, whose snapshot pool rotates over 4) |

## Result

| Scene | Frames (engine) | Consecutive | Outputs compared (47 passes + 1 copy a frame) | Bit-exact, carried state | Bit-exact, poisoned each frame | Programs remade identical | Validation errors / warnings | Boundary: passes matching |
|---|---|---|---|---|---|---|---|---|
| river combat | 12566-12568 | 3 (0.05 s) | 144 | 144 | 144 | 141 / 141 | 0 / 10 | 141 / 141 |
| river combat | 12623-12742 | **120 (2.0 s)** | 5,760 | **5,760** | **5,760** | 5,640 / 5,640 | 0 / 10 | 5,640 / 5,640 |
| exploration | 13634-13753 | **120 (2.0 s)** | 5,760 | **5,760** | **5,760** | 5,640 / 5,640 | 0 / 10 | 5,640 / 5,640 |
| exploration, 143 s | 17888-17890 | 3 | 144 | 144 | 144 | 141 / 141 | 0 / 10 | 141 / 141 |
| exploration, 187 s | 23662-23664 | 3 | 144 | 144 | 144 | 141 / 141 | 0 / 10 | 141 / 141 |
| exploration, 215 s: passes 11, 14, 48 = 0x018F2300, 0x018F24C0, 0x018F2740 | 27363-27365 | 3 | 144 | 144 | 144 | 141 / 141 | 0 / 10 | 141 / 141 |
| exploration, 250 s: passes 11, 14, 48 = 0x018F2100, 0x018F2C00, 0x018F2740 | 32003-32005 | 3 | 144 | 144 | 144 | 141 / 141 | 0 / 10 | 141 / 141 |
| dungeon (49 passes + 1 copy a frame) | 13022-13141 | **120 (2.0 s)** | 6,000 | **6,000** | **6,000** | 5,880 / 5,880 | 0 / 10 | 5,880 / 5,880 |
| dungeon, another run | 12143-12322 | **180 (3.0 s)** | 9,000 | **9,000** | **9,000** | 8,820 / 8,820 | 0 / 10 | 6,713 / 6,713 (137 frames; the last 43 past the log's one-minute window) |
| **all** | | **555 frames** | **27,240** | **27,240** | **27,240** | **26,685 / 26,685** | **0** errors | **24,578 / 24,578** |

[M; build/r1/captures/{rc4,rs120,fs120,fv/b0-b3,dg120,de180}/: compare_chain.json,
compare_chain_pe.json, replay_chain*.json, boundary_chain.json,
validation_chain.txt.] Every output is the target read back right after
its pass, before any later pass could overwrite it, compared raw with the
existing renderer's (tools/r2_compare.py, R1's measures: half floats by
their 16 bits, unorm8 by their 8). **No tolerance was needed, and none is
set.** The ten validation warnings are all one message, repeated: four
programs (passes 1, 11, 12 and 44) write a second colour output the pass
has no attachment for, and the write is unused -- the game binds one target
for those draws too (every pass's blend state has one attachment) [M].

**Carried and poisoned.** In the carried run every image keeps what the
executor left in it the frame before; in the poisoned run every image but
the frame's seeds starts each frame as 0xA5 bytes. Both are bit-exact over
all 120 and 180 frames, so no pass depends on anything an earlier frame left, and
every pass writes every pixel it is compared on. Pass 1 starts from poison
too (its target, the scene target, is seeded only later, where asp hands it
back).

## The dungeon's chain

Inside the first dungeon (dungeon1_approach, area 37) PPFX runs a different
chain [M, build/r1/captures/dg120]: **49 draws**, 15 programs, 44 images.
Pass 1 as elsewhere; a short downsample (640x360, 320x180, 320x180) and
the feedback composite into the scene target already at pass 6 (program
0x018F2300, via a snapshot); then a copy of the scene into a 1280x720
image, a pyramid from 640x360 (five passes, the first 0x018F2380, new) down
to 20x12 (three passes a level, 40x23 and 20x12 included), a combine of
seven of its levels into 640x360 (0x018F26C0, new), a 640x360 blur stage of
four 4-pass iterations, four more 640x360 passes (one samples the R8
texture), and the final composite 0x018F2840 (new) into the display
buffer. External inputs: the
scene target, a 64x64 R8 texture (guest 0x01AD6000, not the river's
0x01AD6200) and a 4x4 RGBA8 one (0x01AD5F00). No history, no blending.
Over its 120 frames (2.0 s, starting 5 s after the area switched) only one
constant of pass 6 changes, once; over another run's 180 frames (3.0 s,
from 3 s after the switch) the same constant changes for 32 frames and back.
Its snapshot pool rotates over up to six images.

## Temporal behaviour

**No GPU history.** In every frame of every capture, everything the chain
reads before writing it is either the scene target as asp leaves it or a
static texture (the 64x64 R8 one; the 4x4 RGBA8 one of the variant and
dungeon final composites); nothing the chain wrote in an earlier frame is
read in a later one [M, chain.json: history empty in all captures]. There
is no blending either, so no pass accumulates over its target. The carried
and poisoned runs being identical over 120 consecutive frames in three
scenes and 180 in a fourth run (2.0 and 3.0 s, at 60 frames a second)
confirms it from the output side.

**What does change over time is driven from the CPU.** PPFX patches its
programs' constants each frame (inline transfers queued just before the
bind), and in these sequences:
- river combat: one constant of passes 5, 7, 9 and 11 (a blur strength, by
  how it is used) steps at frames 40, 84 and 106 (3.91, 4.80, 3.76, 3.80 in
  pass 11; the other passes' values in fixed ratio to it);
- exploration: the same constant ramps smoothly over tens of frames (from
  4.8 to 16.0 and back within a second), the final composite's colour
  matrix (its constants 8, 9 and 11) animates with it, and in frames 41-57
  PPFX uploads a shorter variant of pass 11's program into the same place
  (its blend stages two instructions fewer).
Each frame's values and program bytes are in that frame's description, so
the executor reproduces them exactly [M]; a native PPFX (R3) gets them from
the same calls. So if the game adapts anything to the picture (exposure,
say), it does so in that CPU logic: no PPFX call reads a result back (none
is a report, a label or a wait), and this renderer writes no GPU result into
guest memory [C]. Whether the CPU reads a PPFX target from local memory is
not measured [U] (R3's first open point). Inside the dungeon, from 3 s
after entering, nothing ramps (above); its first 3 s were not captured.

## Programs and chain structures

**The inventory.** DOTHEROES_R2_INVENTORY logs every frame's PPFX draws --
fragment program, target, size -- over a whole run, and
tools/r2_inventory.py cuts the stream into chains at each pass 1. The
exploration route of field_after_dungeon, run with the inventory alone (no
capture, no observers; 280 s), showed 32,210 chains in 7 structures [M,
build/r1/inventory/fi1/inventory.txt; the capture run fs120 showed the same
7, later in time]:

| Chains | When (s) | Draws | How it differs from the river fight's chain |
|---|---|---|---|
| 17,225 | 37-142, 144-179 | 47 | none (the 19 programs) |
| 7,199 | 179-192, 243-280 | 47 | pass 11 0x018F2300 (river 0x018F1D80); pass 14 0x018F24C0 (0x018F03C0); pass 48 0x018F2740 (0x018F0FC0) |
| 6,646 | 192-241 | 47 | pass 14 0x018F2C00; pass 48 0x018F2740 |
| 609 | 203, 217, 226, 229, 241-243 | 47 | pass 11 0x018F2100; pass 14 0x018F2C00; pass 48 0x018F2740 |
| 529 | 55, 73, 122, 142-144, 167 | 47 | pass 11 0x018F2100 |
| 1 | 37, the first frame after loading | 38 | a shorter chain |
| 1 | 37, the next frame | 49 | two extra passes into 1x1 targets (program 0x018F1FC0) |

So the chain's shape is fixed, but three positions take other programs in
some areas or moments (25 programs over the route), and PPFX also rewrites
code in place (pass 11's two variants above).

**The variants, captured.** One run of the same route captured three frames
at each of four moments (tools/r2_capture.sh with several times: one
batch each, its own boundary log). Two caught the common structure (one
aimed at a two-second variant, one shifted by the slowdown an earlier
capture window causes), and two caught
variants, which together contain every program the route showed: at 215 s
passes 11, 14 and 48 ran 0x018F2300, 0x018F24C0 and 0x018F2740; at 250 s,
0x018F2100, 0x018F2C00 and 0x018F2740. All bit-exact, as the table above
shows. The variant final composite 0x018F2740 samples one more static input:
a 4x4 RGBA8 texture (guest 0x01AD6100, format A8R8G8B8), seeded like the R8
one. Not captured as whole structures: pass 11 = 0x018F2100 alone, and
0x018F2C00 with 0x018F2740 alone (both made only of captured programs); and
the two one-off chains at an area's first frame (38 and 49 draws, the 49
with two 1x1 passes and the 25th program, 0x018F1FC0).


## Validation, dependencies, repetition

**Synchronisation.** The executor tracks every image's layout, its last
write and the reads since, and gives each access exactly the barrier its
hazard needs: read after write (a pass's output sampled by a later pass),
write after read (a target drawn again after being sampled: the blur
passes alternate between two 320x180 images), write after
write, and the layout changes between colour attachment, shader read and
transfer. The feedback pass reads a copy, made into a separate image just
before it; the scene target is never sampled and written by one pass.
Run under the Khronos validation layer with synchronisation validation, the
whole chain gives **0 errors** in every frame of every capture; the only
warnings are the unused-output message above [M]. Negative control:
`--hazard-selftest` removes one dependency (the first read of a pass's
output keeps its layout transition but no longer waits for the colour
writes); the layer reports it -- one error, a write-after-write hazard on
that transition [M, rc4/validation_chain_hazard.txt].

**Independence.** tools/r1_check_deps.py on the executor with its chain
mode: its link inputs are its own two files, the program translators and
glslang; none of its objects references the FIFO walker, GCM HLE, RSX state,
the Vulkan backend, guest memory or the PPU/SPU runtimes; no runtime log
string is in the binary [M, build/r1/bld/r1_deps.json: no findings].

**Repetition.** The chain run frame after frame over 120 and 180 frames
(carried state) and again with every image poisoned each frame, all
bit-exact; and in each scene the timed frame run 2,200 times, then once
more with readbacks: still bit-exact [M].

**Unchanged game.** The capture is development-only (off unless its
switches are set). The main build at ps3recomp 0105-0106 passed a river
fight smoke (route, fight and menu segments, exit 0, no capture code
active) [M, build/r1/mainsmoke-r2.out].

## GPU timing

**Method.** In the executor (`r1_replay --chain ... --reps 2000 --warmup
200`): the timed frame's chain recorded with no readbacks, each segment --
pass 1; passes 3-48 -- between a TOP_OF_PIPE timestamp after a full barrier
and a BOTTOM_OF_PIPE one at its end. Inside a segment, everything the chain
needs is timed: every barrier and layout transition, the feedback copy, the
draws. Outside: the seeding (the scene target asp would have left, the R8
texture), capture, file I/O and readback. After the repetitions the chain
runs once more with readbacks: still bit-exact (47 / 47). In the game the
same brackets come from VK_GPU_PASSES over the evidence stage's steady
frames (build/ab/2026-11-04-evidence/runs/*-gpu; the dungeon from a run of
the main build with the same switches, build/r1/dg-gpu-run;
tools/r2_game_chain_time.py):
pass 1 is a render pass of its own just before asp's scene pass; passes 3-48
are measured from pass 3's start to pass 48's end, barriers, gaps and the
feedback copy included. RTX 3070, timestamp period 1 ns, steps of about
1.02 us.

| | Native median (mean, p90) | Existing path, median (range, frames) |
|---|---|---|
| river, pass 1 | 17.7 us (17.7, 18.2); again 17.7 | 21.5 us (15.4-29.7, 20) |
| river, passes 3-48 | 461.5 us (463.3, 464.2); again 462.1 | 843.8 us (677.9-4115.4, 25): 450.3 in passes, 396.4 between |
| river, the chain | 479.2 us (481.0, 482.1); again 479.8 | |
| exploration, pass 1 | 17.8 us (17.8, 18.2) | 15.4 us (14.3-34.8, 32) |
| exploration, passes 3-48 | 461.9 us (463.1, 463.6) | 680.9 us (662.6-1225.7, 39): 370.7 in passes, 313.2 between |
| dungeon, pass 1 | 17.8 us (17.8, 18.2) | 15.1 us (14.3-15.4, 5) |
| dungeon, passes 3-50 (48 draws and the copy) | 501.6 us (502.8, 503.4) | 709.6 us (707.6-712.7, 7): 395.3 in passes, 314.3 between |

The native chain after pass 1, with its barriers and copy, takes about as
long as the existing path's passes alone; the existing path's gaps between
passes (its full barrier and render-pass set-up before each, 7-8 us a pass,
NATIVE_EVIDENCE.md §4) are most of the difference [M, I]. This is a
standalone measurement of comparable work, not an in-game frame-rate claim.
R1's per-pass difference (native passes 3 and 48 1-7 us slower than the
game's in isolation) stays recorded and unexplained; it is not chased here.

## What was built

| Part | Where | Role |
|---|---|---|
| Chain capture | ps3recomp 0105-0107 (rsx_draw_engine.c, rsx_vulkan_backend.c); port/native/render/r1_observe.cpp; tools/r2_capture.sh | every PPFX draw of N consecutive frames, resources named across draws, each frame's operations in order with what work outside the chain wrote in between; inputs only where the chain reads before writing |
| Chain description | tools/r2_chain.py -> chain.json | resources with roles (external, history, snapshot, intermediate); per frame: copy, draw, outside and seed operations |
| Executor, chain mode | port/native/render/r1_replay.c (`--chain`) | the whole chain on its own images, barriers from the tracked hazards, the feedback copy, poison, per-pass readback, segment timing, validation |
| Checks | tools/r2_compare.py, r2_boundary_check.py, r1_check_deps.py, r2_game_chain_time.py | every output against its reference; every pass's boundary; independence; the game's own chain time |
| Inventory | rsx_draw_engine.c (DOTHEROES_R2_INVENTORY); tools/r2_inventory.sh, r2_inventory.py | every frame's chain structure over a whole run, at the cost of a hash per draw |
| Evidence | tools/r2_prune.py | a checked capture reduced to its descriptions, seeds and hashes |

## Evidence kept

build/r1/captures (local; game data, never in Git), 6.7 GB in all after
tools/r2_prune.py: per capture its chain.json, every frame's ops.jsonl and
draw descriptions with their programs, constants and geometry, the seeds
(what the chain reads from outside it), references.json (every reference
image's SHA-256), every result file (replay, compare, boundary, validation),
the gzipped boundary log, the run's logs and inventory, and each frame's
final_sheet.png (reference | native | difference). With them the executor
runs every chain again and the comparison checks it against the hashes. The
superseded first capture (rc3: a window from pass 3, the wrong swizzle on
uploaded textures) is deleted. Inventories: build/r1/inventory/{fi1,at2};
the smoke and dungeon timing runs: build/r1/mainsmoke-r2-run,
build/r1/dg-gpu-run.

## Reproducing

```
tools/r1_dev_lift.sh
tools/r2_capture.sh rs120 river_fight 100 120 200
tools/r2_capture.sh fv field_after_dungeon 143,187,215,250 3 320
build/venv/Scripts/python.exe tools/r2_chain.py build/r1/captures/rs120
build/r1/bld/r1_replay.exe --chain build/r1/captures/rs120 --validate     (and --poison-each-frame, --hazard-selftest)
build/venv/Scripts/python.exe tools/r2_compare.py build/r1/captures/rs120 --png --prune     (--variant pe)
build/venv/Scripts/python.exe tools/r2_boundary_check.py build/r1/captures/rs120
build/r1/bld/r1_replay.exe --chain build/r1/captures/rs120 --frames 0-0 --reps 2000 --warmup 200
build/venv/Scripts/python.exe tools/r2_game_chain_time.py build/r1/captures/rs120 <VK_GPU_PASSES passes.txt>
build/venv/Scripts/python.exe tools/r1_check_deps.py build/r1/bld
tools/r2_inventory.sh fi1 field_after_dungeon 280
build/venv/Scripts/python.exe tools/r2_prune.py build/r1/captures/rs120
```

## Coverage

**Tested** [M]: 555 captured frames, 26,685 passes and 555 feedback copies,
in three scenes and four chain structures -- river combat (3 frames, and
120 consecutive), exploration (120 consecutive, and 4 x 3 frames at moments
with program variants), the first dungeon's interior (120 consecutive from
5 s after entering, and 180 consecutive from 3 s after, in another run).
27 program offsets, 26 distinct programs; every one remade identically and
every pass bit-exact.

**Seen but not captured** (from the inventories, so their programs are not
checked):
- the one-off chains of an area's first frames (38, 49 and 51 draws; two
  1x1 passes, program 0x018F1FC0);
- the chain after returning to the title and continuing: the normal
  structure's sizes, but PPFX reloaded its programs at other offsets
  (0x018F00C0, 0x018F0180, 0x018F0280, 0x018F0480, 0x018F0B80, 0x018F0DC0,
  0x018F10C0 and others) -- most likely the same programs, unverified;
- the exploration route's two remaining combinations (pass 11 = 0x018F2100
  alone; 0x018F2C00 with 0x018F2740 alone), made only of captured programs.

**Not seen at all:**
- the first seconds inside the dungeon (both captures began 3 s or more
  after the area switched; the switch time varied by 5 s between runs);
- other areas' chains: the attract mode's demos (desert, lava, volcano,
  forest) are movies, not PPFX -- an inventory over every draw found one to
  19 draws a frame there (the video's quad), none of them PPFX's [M,
  build/r1/inventory/at2] -- so further areas need saves there;
- the boss fight (no automated route reaches it: dungeon1_boss stops at the
  key's chest), menus over the game, the dot editor;
- moments with magic attacks or cut-ins, if they change the chain.

## R3: the proposed scope (for review)

R3 brings PPFX into the game in **dual run**: PPFX's calls produce native
render records, rendered by a native render thread into images of their own,
compared frame by frame with the existing path's -- which still renders
PPFX and stays what the player sees. R3 removes nothing; R4 then stops
PPFX's command generation, ring traffic and decoding.

**Where the records come from: PPFX's libgcm calls.** R2 checked every one
of the 17,865 captured passes' boundaries (375 frames, three scenes)
against what the GPU consumed, and they agree in full: the arguments of the
libgcm calls PPFX makes -- surface, each unit's texture, the fragment
program's offset and every inline patch into it, each vertex parameter, the
viewport, the scissor, the state enables, the draw -- determine each draw.
And every render state a PPFX draw uses was last set by PPFX itself, with
the value the GPU consumed (colour mask all channels; blend, depth test,
cull, alpha test and stencil off; texture filter and address per unit) [M].
So a recorder at those 29 entry points, for calls from PPFX's code
(0x007497E0-0x007D3E38), can build each record from PPFX's own calls and a
small state block of its own -- no FIFO decoding and no reconstructed RSX
state. The lifter's entry-observer mechanism (ps3recomp 0101) already
reaches exactly these calls; R3 makes the observer call the recorder
(development switch, off by default), while the lifted libgcm bodies still
run so the reference path is unchanged.

**What R3 builds:**
1. *The recorder and PPFX's state block*: last surface, texture per unit and
   its sampler state, the bound programs (the vertex program from its
   upload call; the fragment program's bytes from local memory with PPFX's
   queued patches applied), vertex parameters, viewport, scissor, enables,
   vertex arrays. At each draw-arrays call, a record in R2's vocabulary:
   Vulkan formats and swizzles, samplers, programs by content.
2. *Programs by content*, through the existing translators and the shader
   and pipeline caches (asset conversion): PPFX rewrites program code in
   place at runtime (pass 11's two variants in one exploration second) and
   uses other programs at three positions in other areas (25 programs on
   the exploration route), so a program's identity is its bytes, never its
   offset.
3. *A resource registry shared with the existing backend*: guest location,
   offset, format, pitch and size to a native image. Shared: the scene
   target (PPFX's pass 1 writes it, asp renders over it, later passes read
   it and the feedback composite writes it again), the display buffers (the final composite writes them; HUD work
   draws on them afterwards) and the R8 texture. Owned by PPFX alone: the
   intermediates (34 in the river's chain, 38 in the dungeon's), which never
   need guest memory. The feedback copy is an explicit record.
4. *The render thread and record queue* (one producer, the main thread [M,
   NATIVE_EVIDENCE.md §1]), and *ordering against the FIFO stream*: pass 1
   must run before asp's scene and the rest after it, before the HUD.
   Each record carries the ring position its commands occupy; the walker,
   reaching it, lets the render thread run the records up to it. In dual
   run the records render into shadow images, compared with the existing
   path's outputs on sampled frames.
5. *Checks*: bit-exact counts per frame in development runs over the scenes
   R2 covered, validation, the recorder's main-thread cost, unchanged game
   output (frame grabs against the reference path).

**What R3 still leaves:** everything; the FIFO path renders PPFX as the
reference. R4 removes PPFX's ~6,900 command headers a frame (16%) with
their decoding and register state.

**Open points R3 starts with:** whether the fragment program's base bytes in
guest local memory at bind time equal what the GPU consumed (R2 verified
the patches, not the unpatched bytes) [U]; whether any CPU code reads a
PPFX target back from local memory (this renderer writes nothing back, and
none of PPFX's calls is a report or a wait) [U]; the one-off chains at an
area's first frames (38, 49 and 51 draws, with 1x1 targets) and the
programs PPFX reloads at other offsets after a return to the title [U, not
captured]; the boss fight and other areas, which need saves (below).

**What the owner could provide** (not needed for R2's result, useful for
R3's coverage): a copy of a save just before the first dungeon's boss, with
the instruction "fight it with a magic attack or two"; and saves in other
areas with their own looks (the desert, the lava dungeon, the volcano, the
forest -- the attract mode shows them only as movies). For each, a sentence
on where it is.
