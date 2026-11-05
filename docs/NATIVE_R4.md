# R4: PPFX's command generation bypassed (2026-11-05)

The owner reviewed the R3 follow-up (docs/NATIVE_R3B.md) and authorised R4
behind an opt-in switch, after two prerequisites, and then R5 (asp, the
engine's renderer) as one continuous phase with R4 as its first checkpoint.
This is the R4 checkpoint. The existing renderer stays the default and what
a normal run shows; everything here is a development build's, off unless
DOTHEROES_PPFX_BYPASS is set.

Labels as elsewhere: **[M]** measured, **[C]** from code, **[I]** inferred,
**[U]** unknown.

## Summary

- **R4 works, behind DOTHEROES_PPFX_BYPASS (development builds, off by
  default)** [M]: PPFX's 2,625 command-writing calls a river frame do not
  run; 367 that other work depends on are kept and counted; the engine draws
  none of PPFX's draws; a stream marker stands where each record's commands
  were; the native output is handed back to the engine's surface before the
  engine next uses it (sections 2, 3, 5).
- **Skipping is checked, not assumed** [M, C]: a verify mode runs every
  bypassed body and compares what it left with what a skip leaves -- no
  store outside the command buffer, the context's position, its own frame
  and the caller's save slots; the ABI-kept registers unchanged; the r3 four
  entry points leave is read by no caller (static check of all 63 call
  sites and the 48 callers r3 is passed up to) (section 4).
- **Prerequisites** [M]: SNAPSHOT's picture is the JPEG encoder's read of
  the engine's display surface (handed back first in R4; the album shows R4's
  pictures correctly); PPFX's occlusion query result is read only by PPFX's
  own poll and never arrives in the port (the copy is not carried out) --
  kept as it is in R4, tracked as a gap (section 1).
- **Correct in every scenario reachable without the owner** [M]: river,
  dungeon with three title cycles, menus, editor, snapshot and album, field
  14, the field after the dungeon, 30/60/72/144 Hz pacing (17 runs): 4.4
  million records, none missed; 184,378 handbacks, none failed, all 91,622
  checked byte-identical; synchronisation validation clean; R3 and R4
  frames of a still scene identical at all 53 switches (sections 6, 7).
- **Slower than the reference** [M]: 60 and 72 fps held, the 60 fps tail a
  little longer, throughput 95-99 fps against 137-141: the handbacks put the
  native renderer on the critical path (section 8).
- **R5 has started** [M]: asp's draws recorded from asp's own methods and
  cross-checked against the engine (2.0 million matched in the river); its
  textures, constants and streams are described, its programs, render state
  and samplers not yet (section 10).
- **Needs the owner**: the boss and a magic attack (owner_boss_r4.drive,
  ready), the areas past the first dungeon's surroundings, and the GitHub
  repository rename (the browser here is not signed in to GitHub).

## 1. The prerequisites

**How SNAPSHOT takes its picture [M]** (build/r3/sn3: the snapshot scenario
with the target watch, now naming each access's call stack, and a frame
trace started as SNAPSHOT opens). The game's save path
(func_0045DB34 ... func_00514AB0) calls `cellJpgEncEncodePicture`, whose HLE
reads the engine's display surface back from the GPU
(`cellGcm_guest_read_surface` -> `rsx_draw_engine_guest_read`, under the
walker's lock, after the engine's work so far) into the display buffer's
guest memory and encodes it there. That is the "host-side copy" R3B saw
writing the second display buffer: 765 page writes, all from
`rsx_draw_engine_guest_read`'s memcpy. No GPU transfer reads a surface for
it (the trace's 2D subchannels carry only inline transfers: asp's and
PPFX's fragment-constant patches), and no CPU code reads a PPFX target. So
the picture is whatever the engine's display surface holds when the encoder
runs: the final composite and the HUD over it. In R4 the native final
composite reaches that surface through a handback (section 3), and the
engine's guest read is one of the places that asks for one first
(rsx_engine_bridge.touch, kind 2).

**Who reads the occlusion report [M, C].** PPFX's occlusion query is an
object (vtable 0xFEC2B8; methods begin 0x007C2B74, end 0x007C2EF4, poll
0x007C2CDC, result 0x007C2C28, wait 0x007C2DC0). Its end requests report
769 (cellGcmSetReport), zeroes its result buffer with the last word set to
1 ("pending"), and asks for the report to be copied there
(cellGcmSetTransferReportData: IO 0x01A2C500, EA 0x12B2C500); the copy's
last word, 0, marks the result ready. Its poll reads that word every frame:
the watch (sn3) counts 185 reads of the buffer over 65 s, all from the poll,
called by PPFX's func_00779E80 (an effect object holding an array of
queries and computing intensities at +0x40..+0x50 from their results). No
other code reads the buffer or report 769 (the HLE's cellGcmGetReport is
never called for it). The port carries out the report request (the walker
writes report 769 with value 0xFFFF: "visible", sample counts are not
measured) but **not the copy** (a memory-to-memory transfer on subchannel 1,
which the engine leaves unhandled): the buffer's word stays 1, the poll
finds the result pending forever, and the query is issued once per area
load and never again. On a PS3 the result arrives a frame later and PPFX
goes on using it.

**What R4 does with it:** keeps every call of the query (cellGcmSetReport,
SetZpassPixelCountEnable, SetClearReport, SetRenderEnable,
SetTransferReportData) running, so the port's behaviour -- report written,
copy not carried out -- is exactly what it was. The missing copy is tracked
separately (docs/HANDOFF.md, "Known gaps"): carrying it out would make the
result arrive, but with the fixed "visible" count, not the pixel count the
game measures; a faithful fix needs real occlusion counts (a behaviour
change for the owner to decide on).

## 2. What is bypassed, what is kept

Each libgcm entry point PPFX reaches has a class (ppfx_recorder.cpp,
r4_class) [C]. A PPFX call (its return address in PPFX's code, or a libgcm
function's called from one) to a **bypassed** entry point does not run: the
development lift's observer answers "replaced" (ps3recomp 0112) and the
lifted function returns at once. Its encoder has already taken what the
call means into the record's explicit state. A **kept** entry point runs,
and everything called inside it runs. Entry points that write no command
of their own (address helpers, wrappers, the flush) always run; a wrapper's
nested writers are classed one by one.

| Class | Entry points | Calls a river frame [M, rb1] | Why |
|---|---|---|---|
| bypassed | vertex data arrays (1,504), inline transfers (235: the fragment constants' patches), texture control 0, filter, address, border (116, 96, 37, 50), the frequency divider and vertex cache invalidate (94 each), textures (66), viewport (51), the fragment program bind and shader control, anti-aliasing control, draw arrays (47 each), blend (11), the other render-state calls (2-7 each), colour-only clears | **2,625** | the record carries the state; only PPFX's own draws read it (docs/NATIVE_R3B.md section 5) |
| kept | vertex program constants (307), scissor (54), vertex program (47), surface (47), render enable (4), clear depth/stencil value and depth/stencil clears (1 each), clear colour, the occlusion query's other calls (once a load) | **367** (and 94 calls made inside them) | other work reads what they leave: asp and the game draw into the surface PPFX sets, and into the depth buffer PPFX clears (with PPFX's scissor and clear value); asp's and the game's vertex programs read PPFX's constants (c457-c460 directly; indexed reads cannot be bounded, so every PPFX constant write is kept); the query as the port runs it today (section 1) |

A record of a bypassed draw or colour clear is marked `bypassed` and gets a
stream marker where its commands would have been (section 3).

The bypass starts at the first frame after the native renderer is ready;
the frames before it (the boot, the title) are R3's: the engine draws
PPFX's draws and the native renderer runs beside it. DOTHEROES_PPFX_BYPASS
=alt:<n> alternates R3 and R4 in blocks of n frames (section 7).

## 3. Markers and handbacks

All of R4's bridging is in port/native/render/ppfx_bridge.cpp [C].

**The marker.** At a bypassed draw or colour clear, after publishing its
record, the recorder writes four words at the command buffer's current
position -- an NV4097 method 0x0100 (no operation) header for three words,
not incrementing, then 0x52344D4B ("R4MK") and the record's 64-bit sequence
number -- reserving them as libgcm's own reserve does (the context's
callback first when they do not fit; this title's callback is the
runtime's, cellGcm_fifo_recycle_n). The engine treats 0x0100 as nothing and
hands each such word to the bridge (ps3recomp 0113). One marker a record:
47 a frame in the river, 16 bytes each, against the ~2,600 command headers
PPFX wrote before. One a record rather than one a run of records keeps the
walker's per-record work exactly R3's (the handoffs for that record), and
costs nothing measurable.

**Ordering.** The marker is written by the producer where the record's
commands would have gone, after the record was published: the walker meets
it after every command written before it -- other work's included -- and
before every later one, and the record is already in the queue. Matching is
by the sequence number in it, not by position: no lap arithmetic. A marker
naming a record behind the head counts the records in between as missed (0
in every run), one naming an unknown record is counted and ignored (0).
Kept clears still reach the walker as commands and are matched by position,
as in R3.

**At a marker** the walker does what it did at a PPFX draw in R3: copies the
engine's surfaces the record reads that other work wrote since (handoffs,
engine -> native, ordered by a semaphore the engine's submission signals).
Then the record's target is the native side's: the engine's copy of it is
stale.

**Handbacks (native -> engine).** Before the engine next reads or writes a
surface whose newest content is the native side's -- another draw renders
into it (the bridge's draw callback), it samples it, clears it, a 2D
transfer or a guest read or write of its memory touches it (the snapshot's
JPEG encoder), or presents it (the engine's touch callback) -- the walker:

1. submits the engine's batch so far, so that every engine signal the
   native side may be waiting for is on the queue;
2. asks the native thread for the image (handback_take): it records every
   walked record, puts the image in TRANSFER_SRC_OPTIMAL and submits with a
   binary semaphore to signal; the walker waits for that submission;
3. records the copy of the native image into the engine's surface in the
   engine's stream (rsx_vk_bridge_copy_into); the submission that carries
   it first waits on that semaphore. The engine's surface is the engine's
   again.
4. The next record walked waits for a signal of the engine's work after
   that copy before the native side touches the image again (write after
   read across the queues), and frees the semaphore once its own batch is
   done (the engine's wait has run by then).

No cycle: the engine's batch carrying the signals the native side waits for
is submitted before the walker waits for the native side (1); the native
submission the engine waits for is made before the engine's submission that
waits (2, 3); and a command buffer that waits on the native side never
carries a signal the native batch it waits for depends on. In the river,
2 handbacks a frame: the final composite into the display buffer before
the HUD draws over it, and the scene target before asp draws into it again.

**Checks (development).** DOTHEROES_PPFX_HANDBACK_CHECK (on unless
comparisons are off): every final composite's handback, and every 60th
other, reads both sides back after the copy and compares them byte for
byte -- an engine copy that ran before the native work completed would
differ. DOTHEROES_PPFX_PRESENT_HASH=<file>: every presented image hashed
(section 7).

## 4. Checking what a skipped call would have done

A skipped body must leave the CPU exactly as running it would, the
commands aside. Returning CELL_OK is not assumed; two checks:

**Dynamically [M]** (DOTHEROES_PPFX_BYPASS=verify: nothing is skipped; each
PPFX call to a bypassed entry point runs with its results checked against
what skipping it would leave -- build/r4/vf1, river, 100 s, the R3 dual run
otherwise). For each of the 35 bypassed entry points PPFX called (12.5
million calls):

| Check | Result |
|---|---|
| every store the body made (ps3recomp 0112's store probe) | only into the command buffer between its position before and after the call, the context's `current` word, the body's own stack frame, and the caller's ABI save slots (LR save, parameter save area): **0 stores anywhere else** |
| the registers the ABI keeps across a call (r1, r2, r13-r31, f14-f31, v20-v31, CR2-CR4) | **unchanged in every call** |
| r3 (and f1) | unchanged, as a skip leaves it, for 31 entry points; changed by the texture leaf (237,062 calls), draw arrays (168,817), the inline transfer (843,981: its destination offset rounded down) and, when its reserve called the context's callback, vertex data arrays (30) -- the static check shows nobody reads it |
| the context's callback (the ring wrapped inside the call) | 38 calls; a skipped call writes nothing, so needs no room |

The run's R3 comparisons stayed identical: 21,322 of 21,322, the 3,593
final composites and the 12,623 after a structure change among them.

**Statically [C]** (tools/r4_callsites.py over the lifted code, build/r4/
callsites.txt): every call of a bypassed entry point from PPFX's or
libgcm's code (63 sites, the two wrappers that end in a tail call of one
counted as calls of it): along every path after the call, up to the next
call or the return, the volatile registers the caller reads before writing
them. **None** reads one (r0, r4-r12, f0-f13, v0-v19, CTR, CR0/1/5-7). Ten
sites return with the bypassed call's r3 as their own result; following r3
up through those functions (0x006A97F8, 0x0077F264, 0x0077F468), none of
their 48 callers reads it.

Together: a skipped call differs from a run one only in the command words
it does not write, the context's `current` it does not advance, and the
reserve callbacks it does not need.

## 5. Proof that the bodies no longer run and PPFX's draws are gone

**Counters [M, rb1, river, 100 s].** Per entry point, PPFX's calls whose
body was skipped, kept (and why), run inside a kept call, or run before the
bypass began (report lines `r4 entry`): 9.12 million skipped, 1.27 million
kept; the walker matched **0** PPFX draws by their commands (the engine drew
none) and met all 163,253 records at their markers.

**The command stream [M]** (tools/r4_trace_audit.py: an attributed frame
trace's words by writer, PPFX's calls apart; 60 river frames each):

| | R3 (build/r3/lt3) | R4 (build/r4/rt1) |
|---|---|---|
| words of bypassed writers from PPFX's direct calls | 274,585 | **0** (120: the no-operation word a kept depth clear writes before its CLEAR_SURFACE) |
| ... from PPFX's calls through libgcm wrappers | 100,450 | **0** |
| ... from the game's own calls of the same functions | 22,124 | 44,420 (other frames of the river) |
| PPFX's draw-arrays words (0x006A1E28) | 12,620 (3,786 draw methods) | **0** |
| kept writers' words from PPFX's calls | 121,151 + 15,144 | 270,600 + 33,840 |
| stream markers | 0 | 2,820 (47.0 a frame) |

(The kept words are more in R4's 60 frames because those frames are later
in the fight: more of PPFX's constants a frame. asp's draws, 0x006C6F88,
and the game's libgcm draws are there in both.)

## 6. Coverage

R4's development build (build/r4/b1), the bypass on from the first frame
the native renderer was ready; every run with the handback check on (every
final composite's handback and every 60th other read back on both sides and
compared), real audio, the river, dungeon and field checkpoints as before
(tools/r4_scene.sh) [M]. "Records" counts every record; the few not met are
the ones in flight when the test stopped the game.

| Run | Scenario | Switches | Frames with records | Records (met at markers / matched by position / missed) | Calls bypassed / kept | Handbacks (checked: equal / differing) | R3 comparisons equal / differing | Ended |
|---|---|---|---|---|---|---|---|---|
| rb1 | river_fight | BYPASS=1 | 3474 | 163253 (163180 / 0 / 0) | 9.12M / 1.27M | 6945 (3530 / 0) | 0 / 0 | own stop |
| rmn | menus_r3 | BYPASS=1 | 7545 | 354567 (354520 / 0 / 0) | 19.80M / 2.77M | 15086 (7668 / 0) | 0 / 0 | own stop |
| ral | menus_r3 | BYPASS=alt:30 | 7765 | 364907 (182024 / 182836 / 0) | 10.17M / 2.85M | 12038 (4012 / 0) | 12631 / 0 | own stop |
| rdg3 | dungeon1_approach | BYPASS=1 | 16329 | 782682 (782635 / 0 / 0) | 43.53M / 6.30M | 32644 (16595 / 0) | 0 / 0 | own stop |
| rdg | dungeon1_approach | BYPASS=1 | 5837 | 279688 (279641 / 0 / 0) | 15.56M / 2.25M | 11668 (5932 / 0) | 0 / 0 | own stop |
| red | editor_r3 | BYPASS=1 | 1026 | 48217 (48170 / 0 / 0) | 2.69M / 0.38M | 2050 (1043 / 0) | 0 / 0 | own stop |
| rsn | snapshot_r3 | BYPASS=1 | 4194 | 197067 (197020 / 0 / 0) | 11.01M / 1.54M | 8384 (4262 / 0) | 0 / 0 | own stop |
| rfb | fields_beyond | BYPASS=1 | 7309 | 343518 (343471 / 0 / 0) | 19.19M / 2.68M | 14616 (7430 / 0) | 0 / 0 | own stop |
| rfs | field14_building | BYPASS=1 | 8021 | 376982 (376935 / 0 / 0) | 21.14M / 2.94M | 16040 (8154 / 0) | 0 / 0 | own stop |
| rfa | field_after_dungeon | BYPASS=1 | 14617 | 686994 (686947 / 0 / 0) | 38.43M / 5.36M | 29232 (14860 / 0) | 0 / 0 | own stop |
| rvv | river_fight | BYPASS=1 VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation | 881 | 41402 (41355 / 0 / 0) | 2.31M / 0.32M | 1760 (895 / 0) | 0 / 0 | own stop |
| rrl | river_fight | BYPASS=1 REALLOC_EVERY=60 | 3490 | 164025 (163932 / 0 / 0) | 9.16M / 1.28M | 6977 (3547 / 0) | 0 / 0 | own stop |
| rdl | river_fight | BYPASS=1 DELAY_MS=5 | 217 | 10194 (10147 / 0 / 0) | 0.57M / 0.08M | 432 (219 / 0) | 0 / 0 | own stop |
| rp30 | river_fight | BYPASS=1 FPS=30 | 2397 | 112654 (112607 / 0 / 0) | 6.29M / 0.88M | 4792 (2436 / 0) | 0 / 0 | own stop |
| rp72 | river_fight | BYPASS=1 FPS=display | 4270 | 200685 (200638 / 0 / 0) | 11.21M / 1.57M | 8538 (4341 / 0) | 0 / 0 | own stop |
| rp144 | river_fight | BYPASS=1 FPS=0 DIAG_VBLANK_MS=6.944 | 4839 | 227428 (227381 / 0 / 0) | 12.70M / 1.78M | 9676 (4919 / 0) | 0 / 0 | own stop |

What each adds:

- **rb1, rt1** the river fight; rt1 with an attributed frame trace
  (section 5).
- **rmn** menus over the game: the item menu, the system menu, the map,
  TITLE and CONTINUE between; 8 handbacks at a present (a menu frame whose
  final composite has nothing drawn over it). **ral** the same with R3 and
  R4 alternating every 30 frames (section 7).
- **rdg** the first dungeon, the castle route to it and back to the title
  and CONTINUE; **rdg3** the same three times (RUNS=3): three title/CONTINUE cycles, each area's and the dungeon's first-load frames three times.
- **red** the dot editor and its preview; **rsn** SNAPSHOT and the album:
  the pictures are taken in R4 frames (the JPEG encoder reads the engine's
  display surface after the final composite's handback) and the album shows
  them with PPFX's depth of field and bloom.
- **rfb, rfs** field 14: its edges, and the arrow shop's interior and back.
- **rfa** the field right after the first dungeon (checkpoint after-dungeon1): the route
  across it, stopped by the test's time limit.
- **First-load frames**: every area entry and CONTINUE in these runs is an
  R4 frame (the 38-, 49- and 51-draw first-load chains included), and the
  native renderer draws them from records; their handbacks are checked like
  every other.
- **rvv synchronisation validation** (Khronos layer, synchronisation
  validation with the shader-access heuristic), the engine's queue and the
  native queue together, 1,760 handbacks and their cross-queue waits:
  **0 errors**; the same 10 warnings as R2 and R3 (fragment programs writing
  a second colour output with one attachment bound).
- **rrl image lifetime**: every native image retired every 60 frames and
  destroyed once no batch in flight uses it: 2,091 images replaced in
  flight, every checked handback identical.
- **rdl a slow consumer** (the native side 5 ms slower a record): no
  deadlock, no failure, every check identical -- and the game at 4 fps: in
  R4 the native renderer is on the frame's critical path (the walker waits
  for it at each handback), where in R3 it only fell behind.
- **Pacing, requested against achieved** (these validation runs: the
  handback check waits for the GPU at every final composite, which costs
  frames; section 8 has the same paces without it):

| Requested | Achieved (with the per-frame check) | Interval p50 / p99 / max |
|---|---|---|
| 30 fps (DOTHEROES_FPS=30) | 30.0 fps (rp30) | 33.3 / 34.1 / 34.5 ms |
| 60 (the default) | 57.5 fps (rb1) | 16.7 / 33.5 / 92.3 ms |
| the display's pace, 72 (DOTHEROES_FPS=display) | 54.2 fps (rp72) | 14.0 / 28.1 / 29.5 ms |
| 144 (DOTHEROES_FPS=0, DIAG_VBLANK_MS=6.944) | 57.1 fps (rp144) | 20.4 / 25.7 / 30.8 ms |

**Not covered, needing the owner:** the boss and a magic attack
(tests/scenarios/owner_boss_r4.drive, ready to play), and the areas past
the first dungeon beyond the field right after it (the desert, the lava
dungeon, the volcano, the forest: no saves there).

## 7. Same-frame image comparisons

R4's displayed frames cannot be compared with R3's by frame number across
two play sessions: the runs differ. Two controlled comparisons instead,
both in the same run:

**At every handback [M].** The handback check reads the engine's surface
back after the copy and the native image at the same point and compares
them byte for byte. It tests exactly what R4 adds between the native
renderer and the rest of the frame -- the copy and its synchronisation (a
copy that ran before the native work completed, or a missing wait, would
differ). Every R4 run in section 6: every checked handback identical.

**R3 and R4 frames of one still scene [M]** (build/r4/ral: the menus
scenario, R3 and R4 alternating every 30 frames,
DOTHEROES_PPFX_BYPASS=alt:30, every presented image hashed after the HUD,
DOTHEROES_PPFX_PRESENT_HASH; tools/r4_present_hash.py). While a menu is
open the game stops and the same image is presented frame after frame. At
each switch between R3 (the engine draws PPFX's draws from PPFX's commands)
and R4 (the native renderer draws them from the records, handed back)
inside such a still stretch, the image after the switch must equal the one
before. With 8 presents a side (4 frames: the engine presents each frame
twice), **53 switches fell inside still stretches and all 53 show the
identical image; 0 differ**; 204 fell where the image moves (no conclusion
either way). The same run's R3 frames compared every native output with the
engine's: 12,631 of 12,631 identical (3,892 final composites); its R4 frames
made 12,038 handbacks, the 4,012 checked identical. The switches also
exercise R3 <-> R4 transitions: R4 -> R3 makes the engine draw PPFX again
from targets the native side last wrote (handed back on demand), 0
differences.

Together with R3's per-record comparisons (the native renderer bit-exact
against the engine on the same inputs, every scenario) and the reverse
dependencies (other work's draws read nothing a bypassed call wrote:
docs/NATIVE_R3B.md section 5, and section 5 above), the frames R4 shows are
the frames R3 showed.

## 8. Performance

**Setup [M]** (build/r4/perf, build/r4/perf1.sh): the river fight, real
audio, the same settings and checkpoint, arms alternated. **Reference**: the
corrected production build at R4's commit (the existing renderer with the
subchannel-1 routing fix; no observers; ps3recomp 0113's hooks compiled in,
inert). **R4**: the development build, DOTHEROES_PPFX_BYPASS=1, comparisons,
cross-check and handback checks off. **R3**: the same development build with
nothing bypassed (the native renderer beside the engine, comparisons off) --
what R4 replaces. Capped runs pace at 60 fps (the default) and measure
smoothness; uncapped runs take the 60 Hz vblank clock away
(DOTHEROES_FPS=0, DIAG_VBLANK_MS=0.5) and measure throughput, with the frame
trace (PS3_FTRACE, VK_GPU_TIME; tests/walker_breakdown.py). The Vulkan
present is FIFO, so the display's refresh (144 Hz) is the only cap left.

**Capped: the fight's guest frame intervals** (tools/r4_fight.py; three runs
each, ~5,000 frames a run):

| Arm | p50 | p99 | p99.9 | max | over 25 ms |
|---|---|---|---|---|---|
| reference | 16.7 | 17.2, 17.2, 17.2 | 17.8, 17.9, 18.2 | 20.8, 20.7, 22.7 | 0, 0, 0 |
| R4 | 16.7 | 17.4, 18.0, 19.2 | 22.0, 23.6, 23.2 | 28.8, 25.1, 26.5 | 2, 1, 1 |
| R3 (dual, no comparisons) | 16.7 | 17.3, 17.3, 17.2 | 17.8, 18.3, 17.8 | 21.4, 20.4, 18.8 | 0, 0, 0 |

All three hold 60.0 fps; R4's tail is longer (a few frames a run past 22 ms,
1-2 past 25 ms).

**R4's own costs, per frame with records** (capped, three runs):

| Where | Cost |
|---|---|
| the game's thread: the recorder (every observed call, encoders, records, markers) | 0.42-0.48 ms (R3: 0.34) |
| the walker: the bridge | 0.91-1.07 ms (R3: 0.10), of it the handbacks 0.78-0.93 ms, of that waiting for the native thread to finish recording and submit 0.65-0.81 ms |
| the native thread (recording) | 0.38-0.41 ms |
| the native GPU (its batches' elapsed time) | 0.53-0.56 ms (the engine drew the same draws in 0.86 ms, NATIVE_R2.md) |

The skipped libgcm bodies (2,625 calls a frame) are time the game's thread
no longer spends; the main thread's busy time below includes both.

**Uncapped: throughput** (two runs each; the reference reaches 137-141 fps
against the display's 144 Hz cap -- not held at 60 by the vblank clock):

| Arm | fps | interval p50 / p99 | walker over a frame: draining (of it handback waits in R4) / waiting for the previous batch at the present / asleep | main thread idle (waiting for the walker) |
|---|---|---|---|---|
| reference | 136.9, 140.6 | 7.0 / 11.0 | 5.54 / 0.09 / 1.63 ms | 0.96 ms |
| R4 | 94.9, 98.9 | 10.3 / 13.5 | 6.53 (~1.0) / 2.26 / 1.61 ms | 3.31 ms |

**Pacing without the checks, requested against achieved** (one run each):

| Requested | Reference | R4 |
|---|---|---|
| 60 (default) | 60.0 | 60.0 |
| 72 (the display's pace) | 72.0 (max 14.7 ms) | 72.0 (max 24.5 ms) |
| 144 (DOTHEROES_FPS=0, DIAG_VBLANK_MS=6.944) | 128.2 (p50 7.0 ms) | 89.1 (p50 13.6: two refreshes a frame) |
| 30 | -- | 30.0 (with the checks, section 6) |

**What the numbers say [M, I].** R4 is correct and holds 60 and 72 fps, but
it is slower than the reference in throughput, and its 60 fps tail is a
little longer. The cause is measured: the handbacks put the native renderer
on the frame's critical path. Before the HUD can be drawn, the walker waits
~1 ms for the native thread to record the frame's remaining PPFX records and
submit (the "waiting for the native side" above); and the engine's next
submission waits on the native queue's semaphore, so the scene, PPFX and the
HUD run one after another across two queues, and the present then waits
2.3 ms for the previous batch. In the reference the engine drew PPFX in
line, on one queue, with no hand-over. The native GPU work itself is
smaller than the engine's was (0.53 vs 0.86 ms). This is the cost of the
temporary bridge, not of native rendering: R5 removes the scene's
engine -> native handoff and the scene-target handback (asp drawn natively
into the same images), R6 the final composite's handback (the HUD native);
until then, reducing it means shortening the native thread's lag
(recording each record as it is walked, submitting the final composite's
batch at once) and letting the engine queue its wait before the native
submission (timeline semaphores), both measurable here.

## 9. Removing the bridge

Everything R4 adds to bridge the native renderer and the engine is in
ppfx_bridge.cpp, the R4 half of ps3recomp 0113, and the native thread's
handback service (ppfx_render.cpp, hb_service): the markers (their format,
their writing, their parsing), the handbacks and their semaphores, the
touch hooks' use, the kept-clear position matching. It exists because the
engine still renders what follows PPFX and what PPFX reads: asp's scene
(drawn into the target PPFX sets, read by PPFX through handoffs), the HUD
and menus (drawn over the final composite, after a handback), the present
and the snapshot's read of the display surface.

How it goes, as the producers become native:

- **asp native (R5):** asp's scene is drawn by the native renderer into the
  same native images PPFX reads: the scene-target handoff (engine -> native)
  and its handback (native -> engine) become ordering within the native
  queue. PPFX's kept writers whose state only asp reads (the surface it
  draws into, the depth clear, the vertex constants asp's programs read)
  become state the native asp records take from PPFX's records directly, and
  stop being kept.
- **The HUD, the menus and the game's own libgcm draws native (R6):** the
  final composite's handback goes; the native renderer presents (or hands
  the finished frame to the present) itself, and the snapshot reads the
  native display image.
- Then the markers have nothing left to order against: the recorder stops
  writing them, the engine's marker and touch hooks are unused, and
  ppfx_bridge.cpp, the queue's `walked` index and ps3recomp 0113's R4 half
  are deleted. What stays is the record format and the native renderer.

Until then the bridge is the only place R4 touches the engine, and turning
DOTHEROES_PPFX_BYPASS off returns exactly R3's behaviour (the same build,
the same records).

## 10. R5: asp

**Where R5 stands [M].** One step done: asp's draws are records, described
from asp's own methods and checked against the engine at each draw, in check
mode (DOTHEROES_ASP_RECORD=1; build/r4/as1, river, 100 s): 2,046,169 asp
records, 2,045,671 matched with the engine's draw inside their commands, 6
missed. The cross-check's per-item list (report lines `xcheck asp ...`) is
the roadmap: the textures on units 0 and 2 and the vertex constants asp
uploads agree; the vertex streams' fields agree but their offsets lack the
draw's own base (0x1C, 4 bytes: a base vertex the draw applies); asp's
programs, viewport, scissor, its own surfaces (the 1024x1024 shadow pass),
its render state and its sampler table are not described yet. Nothing of
asp is rendered natively, and nothing of asp is bypassed.

A second round (build/r4/as2, as3, with DOTHEROES_ASP_PROBE=1) found:

- asp's stream objects hold `0xFFFF8000 | index` while bound and
  0xFFFFFFFF while not; asp rebinds unbound ones too, which the first
  encoder took for input 15. Fixed (as3): no input-15 difference left.
- For some inputs (2, 3, and 0 or 1 in a few draws) the engine's stream
  has a smaller size (2 or 3) at an offset 4 bytes lower than the stream
  object's fields: asp writes those inputs from something other than the
  object's fields as read here -- the next encoder to settle.
- asp's draw object (vtable 0xFEA9D0): the primitive at +0x08, two counts
  at +0x0C and +0x10, the index buffer at +0x14 (its EA and IO offset at
  +0x18 and +0x1C), the index type at +0x24 (2 in every draw seen).
- A vertex shader object holds at +0x18 its container and at +0x1C its
  microcode; the container starts "CGB " (then 0, 0x00500003,
  0x00030100, 0x00004000, ...) with the microcode right after its 0x20-byte
  header. The fields giving the start slot, the instruction count and the
  input mask are to be matched against what the engine runs (the probe logs
  both).
- The fragment-program bind's object (vtable 0xFEAB48) holds the vertex
  shader object at +0x18 and, at +0xF0, what looks like the fragment
  shader's: the program pair asp applies together.

After as3 every asp draw still differs from the engine in some item
(programs, render state, samplers, viewport, surfaces are not described):
R5's encoders are the next work, one group at a time, until the asp
cross-check is clean; then native rendering of asp's records, then the
bypass of asp's writers.

**What asp is [M, C]** (build/r3/lt3: 60 river frames, every word
attributed; the lifted code). asp, the engine's renderer, draws about 230
draws a river frame (13,653 in 60 frames) through one draw method
(0x006C6F88); the game's own libgcm draws (0x006A2164, the HUD and menus)
add about 22. asp is a C++ renderer over libgcm's structures: its GPU
objects are classes whose virtual methods write the commands themselves
(vtables at 0xFEA9C8 -- the draw context -- and 0xFEAACC -- programs,
textures, streams), and its objects carry libgcm's own layouts (a texture
object holds a CellGcmTexture: format, mipmap, dimension, cubemap, remap,
width, height, depth, location, pitch, offset).

| Writer | What it writes [M: methods in the trace] | Words in 60 frames |
|---|---|---|
| 0x006C9624 | fragment-constant patches (inline transfers, NV3062/NV308A) | 574,766 |
| 0x006C9C74 | vertex constants (TRANSFORM_CONSTANT_LOAD) | 544,384 |
| 0x006CA3D0 | vertex program upload and bind (TRANSFORM_PROGRAM, its start, attribute mask) | 445,150 |
| 0x006CADE4 | fragment program upload (inline transfer) | 388,496 |
| 0x006C6F88 | the draw (index array, BEGIN/END, DRAW_INDEX) | 322,246 |
| 0x006D32D0, 0x006D3190 | vertex streams (VERTEX_DATA_ARRAY offset and format) | 269,628 + 155,420 |
| 0x006CB070 | textures (CellGcmTexture -> TEXTURE_* methods), then the sampler bind | 114,777 |
| 0x006C99B0 | fragment program bind (SHADER_PROGRAM, control, input mask, TEX_COORD_CONTROL) | 102,334 |
| 0x006D0E84 | vertex textures | 62,760 |
| the game's memcpy | asp's state store re-emitted (render state) | 171,013 |
| 0x006C37A0, setters | samplers, colour mask, depth, stencil, blend, cull, polygon offset, line width | ~50,000 |

**The plan, as R1-R4 did it for PPFX, now per group of asp's operations:**

1. *Record asp at its API.* Observers on asp's methods above; encoders that
   set the same explicit state (ppfx_native.h State) from their arguments
   and asp's objects -- the texture from its CellGcmTexture, a stream's
   size, type, stride, frequency, location and offset from its stream
   object, programs from the objects asp uploads (the program shadow PPFX's
   recorder keeps, fed by asp's own inline transfers), constants from the
   upload calls, render state from the setters' arguments (not from the
   store's command words). A record at each asp draw, with its index range.
2. *Check mode.* Cross-check each asp record against the engine's state at
   the same draw (matched by position, as R3 matched PPFX's) and compare
   outputs, group by group: the scene's opaque geometry, its alpha-tested
   and blended geometry, the shadow pass, the effects.
3. *Native rendering.* The native renderer draws asp's records into the same
   native images PPFX uses (depth attachments added), with the proven
   translators, caches, resource registry and per-image hazard tracking;
   asp's scene then reaches PPFX without a handoff, and PPFX's kept writers
   that only asp read become direct state.
4. *Bypass* asp's command writers group by group, behind their own
   switches, with the same verify mode, static call-site check, markers and
   handbacks (for what the engine still draws after: the HUD).

## 10b. R6, the concrete scope (not started; dependencies only)

R6 is the game's own libgcm users and the frame's end, the rest of what the
engine still draws once asp is native [M: build/r3/lt3, build/r4/rt1]:

- **The game's libgcm draws**: about 22 a river frame (0x006A2164, called
  from 0x00024720: the HUD, the menus, dialogue boxes, the map, the
  editor's screens, the title and loading screens). They go through the
  same libgcm entry points PPFX uses, already observed and encoded on the
  producer's thread (their calls are counted as the game's, kByGame): R6
  makes them records too, renders them natively over the final composite,
  and bypasses their writers with R4's verify mode and call-site check.
- **The present**: the native renderer presents its display image itself
  (its own swapchain, or the finished frame handed to the backend's present
  without the engine drawing anything), with the same pacing (DOTHEROES_FPS,
  the vblank clock, the flip's semaphores and labels the game waits on).
- **The snapshot**: cellJpgEncEncodePicture reads the native display image
  instead of the engine's surface.
- **Movies and other surfaces the game writes from the CPU** (any guest
  write into a surface the engine syncs, rsx_draw_engine_guest_wrote): read
  from guest memory into native images.

**Dependencies:** R5 complete (asp's scene native, so no engine surface is
left between PPFX and the HUD); the occlusion query decided (section 1);
then R4's bridge -- markers, handbacks, the touch hooks -- has nothing left
to order against and goes (section 9). **R7** (retirement) depends on R6:
the FIFO walker, the ring and its reserve callback, the engine, the
reports and labels as RSX memory, and the SPU environment as its workloads
go native.

## 11. Gaps

- **Needs the owner:** the boss and a magic attack
  (tests/scenarios/owner_boss_r4.drive); areas past the first dungeon's
  surroundings (no saves); the GitHub repository rename (no signed-in
  session here).
- **R4's cost**: throughput 95-99 fps against the reference's 137-141 and
  a longer 60 fps tail (section 8) -- the bridge's handbacks; not reduced in
  this checkpoint.
- **The occlusion query's result** never arrives in the port (section 1),
  in R3 and R4 alike: tracked in docs/HANDOFF.md; a faithful fix is a
  behaviour change for the owner to decide.
- **Kept writers** whose state other work only might read: every PPFX
  vertex constant write is kept because asp's and the game's indexed
  constant reads cannot be bounded statically (docs/NATIVE_R3B.md section
  5); they go when asp is native (R5).
- **asp (R5)**: its programs, render state, samplers, viewport and its own
  surfaces are not described yet; nothing of asp is rendered natively.

## Evidence kept

Local only (game-derived), under build/r4/ (and build/r3/sn3), each run's
`report.txt` and `run/` (run.json with the build's hash and every switch,
the drive log, frames.log, stderr, grabs):

| Tags | What |
|---|---|
| r3/sn3 | the snapshot with the target watch naming call stacks (`target_watch_named.txt`) and the 2D subchannels' methods (`subchannel_methods.txt`) |
| vf1, callsites.txt | the verify mode; the static call-site check |
| rb1, rt1 (`trace_audit.txt`; r3/lt3 `trace_audit.txt`: R3's same audit) | the river; the attributed trace's audit |
| ral (`run/present_hash.txt`) | R3 and R4 alternating, presented images hashed |
| rdg, rdg3, rmn, red, rsn, rfb, rfs, rfa | the scenarios in R4 |
| rvv (`run/validation.txt`), rrl, rdl, rp30, rp72, rp144 | synchronisation validation, image lifetime, a slow consumer, pacing |
| as1 | R5's first asp check |
| perf/ | the measurement: cap-ref1-3, cap-r4-1-3, cap-r3-1-3, unc-ref1-2, unc-r4-1-2 (with frame traces), pace72-*, pace144-* |
| b1, b2, prod, play | the development builds of the R4 runs (b1) and of as1 (b2), the production reference, the owner's session build; with their PDBs |

## Reproducing

```
tools/r1_dev_lift.sh                                   (the development lift: 104 observers)
tools/r4_scene.sh <tag> river_fight 100 DOTHEROES_PPFX_BYPASS=1          (R4; handback checks on)
tools/r4_scene.sh <tag> river_fight 100 DOTHEROES_PPFX_BYPASS=verify     (the verify mode)
tools/r4_scene.sh <tag> menus_r3 400 DOTHEROES_PPFX_BYPASS=alt:30 DOTHEROES_PPFX_PRESENT_HASH=present_hash.txt
tools/r4_present_hash.py build/r4/<tag>/run/present_hash.txt --k 8
tools/r4_scene.sh <tag> river_fight 100 DOTHEROES_PPFX_BYPASS=1 PS3_ATTRIB_SHADOW=1 PS3_ATTRIB_ST=11100000-13400000 RSX_FRAME_TRACE=75:60:frametrace.txt
tools/r4_trace_audit.py build/r4/<tag>/run/frametrace.txt
tools/r4_callsites.py build/r1/gen
tools/r4_runs.py <tags>;  tools/r4_pacing.py <run dirs>;  tools/r4_fight.py build/r4/perf/<tag>
sh build/r4/perf1.sh                                   (the measurement; local script, its arms in section 8)
tools/r4_scene.sh <tag> river_fight 100 DOTHEROES_PPFX_NATIVE=check DOTHEROES_ASP_RECORD=1   (R5's asp check)
```

New switches: DOTHEROES_PPFX_BYPASS=1|verify|alt:<n>,
DOTHEROES_PPFX_HANDBACK_CHECK=0|1|<n>, DOTHEROES_PPFX_PRESENT_HASH=<file>,
DOTHEROES_PPFX_EA_WATCH=<lo>-<hi>,..., DOTHEROES_ASP_RECORD=1,
DOTHEROES_ASP_PROBE=1; drive scripts' `touch trace.trigger`.
