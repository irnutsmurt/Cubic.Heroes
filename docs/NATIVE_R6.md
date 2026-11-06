# R6: the remaining producers, the native present, and the command writers replaced

R6 follows R5 (docs/NATIVE_R5.md, accepted 2026-11-05 as an experimental
checkpoint with its open items carried forward). Its scope, as the owner set
it: native rendering for every remaining graphics producer; a native
present, snapshot readback and CPU-written surfaces; replacing command
generation instead of taking the commands back; the game thread's recording
cost; R5's correctness gaps; real occlusion results; synchronisation and
shutdown validated; an honest uncapped measurement; and the R7 retirement
checklist. Everything stays opt-in: the production renderer is the default
and the reference builds are kept. As in R4 and R5: measured [M], read from
code [C], inferred [I]; evidence under build/r6/ (local, game-derived).

R6 does **not** remove the PS3 graphics path. The FIFO walker, the ring, the
GCM command HLE and the bridge still run, with almost nothing left to do
(section 9); R7 retires them.

## Current status (2026-11-06)

| Item | State |
|---|---|
| Producers: start-up, title, loading, menus, Bink, the game's clears and draw-arrays | native (DOTHEROES_R6_PRODUCERS=1); with the bodies skipped too (DOTHEROES_R6_SKIP=all) the engine draws nothing in any scenario run (with the producers alone it still draws asp's draws whose commands wrapped the ring: 0-107 a run at 60 fps, 256 in u1river's 150 s uncapped -- section 1) |
| Present | native (DOTHEROES_R6_PRESENT=1); the walker does not present those frames; a flip completes once presented |
| Display-buffer handoffs and handbacks | 0 a frame (R5: 1 handoff and 2 handbacks a frame) |
| Snapshot | read from the native image (the JPEG and PNG encoders' guest read) |
| CPU-written surfaces | Bink's frames and the album's photos are guest textures, read from guest memory (Bink's whole at the walker: its label promises them read); the post-boss warp's 2D transfer goes through the engine (a handback) -- not exercised in R6 (section 2) |
| Command generation | asp's draws, program functions and state functions, the game's draws, clears and state calls, PPFX's kept calls: replaced at their entry, their bodies never run (DOTHEROES_R6_SKIP=all); asp's draws, program and state functions and cellGcmSetVertexProgram's default constants (PPFX's and the game's calls) checked against their bodies with 0 differences; the game's draws, clears and state calls and PPFX's other kept calls rest on R4's verify of the bypassed entry points and the review's reading; a defect in the last addition (cellGcmSetVertexProgram's default constants, the 3D scene black from b18) found in the final checks and fixed in bH -- section 3 |
| Game thread | the bottleneck still; busy 6.4-6.6 ms a frame in the river fight, uncapped, on the lean lift (production 5.9-6.1; R5 9.7) -- sections 4 and 8 |
| R5's gaps | start-up draws fixed; recorder mismatches fixed; scene differences 0.011% -> 0.004%; the water's images pooled; the moat band open (its cause not measured in R6; presumably the walker-timed texture capture [H]) -- section 5 |
| Occlusion | real counts and completion (DOTHEROES_R6_OCCLUSION=1, a behaviour change): PPFX's exposure probe now loops -- section 6 |
| Synchronisation and shutdown | validated under the Khronos layer with working negative controls; the shutdown cancels instead of submitting an invalid wait; an orderly native stop -- section 7 |
| Throughput (uncapped, benchmark mode) | the fight: R6 150.6-152.4 fps, production 145.5-148.1 (R6 3% ahead); exploring: R6 169.5-169.7, production 179-181 (6% behind); R6's p99.9 shorter in every run; walker 0.07-0.34 ms a frame (production 2.1-5.3), GPU 0.7-0.76 ms (production 1.7-2.3); the game thread the limit; 144 fps on average, not sustained; at 60 fps, gameplay timing as production's -- section 8 (batch 16; batch 7's R6 numbers withdrawn: the black-scene defect) |
| Not done | the walker, markers, `walked` gate, bridge, ring and HLE command processing (R7); 13 unobserved libgcm functions still write 92 command words a frame (section 3); the dual-run comparison with the producers stalls, isolated (section 7); vertex-arena retirement not exercised; the RPCS3 side-by-side for the exposure |

## 0. What the map of R6's work found [M, C]

R6 began with a read-only map of everything it touches (seven readers over
the recorder, the bridge, the native renderer, the engine and the lift). It
corrected R5's description of what was left:

- **The in-game HUD and menus were already asp's records.** Every
  cellGcmSetDrawIndexArray the game makes (0x006A2164 from 0x00024720) goes
  into asp's scene or reflection target, none into a display buffer (five
  attributed frame traces, build/r3/lt1-3, gt, build/r4/ft1); the HUD is
  asp's draws after PPFX's final composite. NATIVE_R5.md section 9 said the
  engine still drew the HUD and menus: it did not.
- **What the engine still drew** in R5 was everything before PPFX's first
  call -- the recorder took the title's command context from PPFX's first
  call, so asp's and the game's draws at start-up, at the title and at the
  first load were the engine's -- and the Bink movie frames (the game's own
  cellGcmSetDrawArrays), about 29,500 draws a run at 60 fps whatever its
  length (about 240,000 uncapped, in the benchmark mode).
- **The display buffer's two handbacks a frame** came from the game's own
  frame-start clear (func_00029618 calls cellGcmSetClearSurface on the
  display buffer and its depth, which the recorder left to the engine) and
  from the engine's present.
- **Two occlusion users, not one**, and conditional rendering is never used
  (section 6).
- **The production build's uncapped 137 fps is not the display's cap**: in
  the benchmark mode (no cap, a mailbox present) it measured the same 137.5
  fps in bq1-prod (section 4) and 145.5-148.1 in batch 16 (section 8), above
  the 144 Hz refresh; its walker decodes the FIFO 5.5 ms a frame in bq1-prod
  (5.0-5.3 in batch 16) and its game thread idles about 1 ms a frame (batch
  16: 0.75-0.83 ms) waiting on the engine's side [I: the walker as the
  bound, inferred from that idle time].

## 1. The remaining producers [M]

DOTHEROES_R6_PRODUCERS=1 (with R5's DOTHEROES_ASP_RECORD=1):

- the title's command context from its first call (cellGcm_title_context_pub,
  the HLE's own), so every frame's asp and game draws are records;
- the game's clears (cellGcmSetClearSurface on the title's context) are
  pending clears like asp's, done before the next draw or, when the frame
  draws nothing after them (loads, transitions), before its present (bI;
  until then a frame that drew nothing after the game's frame-start clear
  was presented with the buffer's older picture -- section 3's review);
  PPFX's depth and stencil clears too (R4 had left them to the engine: the
  native depth images never saw them);
- the game's cellGcmSetDrawArrays (Bink's frames; the mesh path's draws
  without indices) are records like the game's indexed draws, with the
  libgcm state Bink sets (its vertex program and constants, fragment program,
  textures, blend function -- now decoded) and what it inherits (surface,
  viewport, scissor) carried explicitly: every record copies the whole
  explicit state, whoever set each item.

| Run (build/r6) | engine-drawn draws (R5: ~29,500) | engine clears | display-buffer handoffs |
|---|---|---|---|
| p1river (producers, engine present) | 24 | 3,668 (asp's HUD clear) | 1 each (the first) |
| p1menus | 103 | 8,443 | 1 each |
| n1river (asp's clears taken back, native present) | 52 | 0 | 0 |
| n1fields | 0 | 0 | 0 |
| a1river, a1menus (every body skipped, section 3) | 0 | 0 | 0 |

The 52 draws left in n1river were asp's draws whose commands had wrapped
the ring (R5's "not bypassed": the take-back cannot take them back); they
went when the bodies stopped running. The menus run's grabs show the
publisher logos, the title, the loading bar, the pause and equipment menus,
the map and the exit prompt all drawn natively.

## 2. The native present, snapshot and readback [M]

DOTHEROES_R6_PRESENT=1: each cellGcmSetFlip on the title's context is a
record (kFlip); the native renderer blits its image of the display buffer the
flip names into the backend's present target on its own queue, and the
present's submission (on the backend's queue) waits for it
(rsx_vk_bridge_present_prepare / _submit: the previous present completed
first, the window present on the present thread as before). The walker's own
present of those frames does not run; it submits the engine's recorded work
at the flip instead. A flip completes for the game only once the native
renderer has presented it too (cellGcm_set_flip_gate): the game's own
one-flip-outstanding wait keeps it at most a frame ahead of the native side,
as it was of the walker.

One flip a run is not presented: the game's first (flip 0 of frame 0), made
before anything was drawn into its display buffer, so there is no native
image to show (`no native image 1` in every R6 run's report but two
lifetime-stress runs on builds before bI: lt1river (b18) 107 and val5river
(bF) 131 -- the first, then 106 and 130 flips of display buffers whose image
the stress had just retired, in frames that drew nothing after their
frame-start clear (val5river's first: buffer 1's flips at frames 1201-1209,
the load). Since bI those clears are
done at the flip, and the clear makes the image again (asp_clears'
target_image) [C, I]: val7river, the same stress on bI, 1). In some runs
one more flip is recorded than presented and missing together [H: the one
the game makes as it exits, after the native thread has stopped; not
examined].

Two synchronisation faults the native present exposed, fixed on the way:

- a signal the bridge attached to an engine command buffer with nothing
  recorded waited for an engine submission that never came once the engine
  drew nothing -- the native batch waited 10 s for it, the walker for the
  native side (the sync map's hazard 2, seen as three 10 s stalls). The
  backend now submits such a signal alone (a signal orders after everything
  submitted before it), and the WAR signal after a handback no longer opens
  an engine batch for itself;
- the engine's work was submitted only at its own present; with the native
  present the walker submits it at each flip.

| Run (b6, native present, capped) | flips presented | present/clear handbacks | fps | p99 |
|---|---|---|---|---|
| n1river | 5,870 | 0 | 59.9 | 18.0 ms |
| n1dungeon | 9,293 | 0 | 59.3 | 17.9 ms |
| n1fields | 9,479 | 0 | 59.7 | 17.4 ms |
| n1menus | 11,541 | 0 | 59.1 | 17.6 ms |
| n1snap | 6,737 | 0 | 58.5 | 20.7 ms |

**The snapshot** (cellJpgEncEncodePicture2 through cellGcm_guest_read_surface):
read from the native image (readback_take: everything published drawn, the
image copied to host memory in a batch of its own and waited for) and
written into the display buffer's guest memory as the engine's guest read
writes it; n1snap: 2 reads, the saved JPEG correct.

**CPU-written surfaces.** No CPU-written colour surface was found: Bink's
frames are CPU-written *textures* the game draws with cellGcmSetDrawArrays,
and the album's photos (cellPhotoDecodeFromFile) are textures too; both are
read from guest memory like any other texture (small ones at the walker,
larger ones when the native thread draws -- section 5). Bink's frames are
taken whole at the walker since bI: Bink decodes into a texture set again
once the back-end label after its draw (written by the walker) has passed
the set's fence (func_0045AC78), so its 1280x720 luma plane read later by
the native thread could be the next frame's (section 3's review). The
post-boss warp reads the display buffer with a 2D transfer the engine
carries out through a handback of the native image -- **not exercised in
any R6 run** (every report: transfer handbacks 0; the scenarios do not
reach the portal room after the boss), so inferred [I], not measured; R7's
item 4 replaces it with a native copy.

## 3. Replacing command generation [M, C]

R5 let asp's command writers run and took their words back at their return
(stage 1: its draws; stage 2: its program uploads). R6 replaces the bodies
at their entry instead (DOTHEROES_R6_SKIP=1: the draws and program
functions; =all: everything below): the body never runs, no command word is
written, nothing is taken back. What a body does besides its words is
reproduced by the replacement; what it wrote into the command buffer is
dropped, because no engine draw is left to read it (section 1).

Each replacement was specified from the lifted code by three readers (asp's
draw and stream code; its program functions; its state functions and state
store) and checked by two independent readers each, one for missed stores,
one for readers of what is stored (build/r6, local). What they established:

- **The draw** (0x006C6F88): besides its words it adds one to a frame
  counter (P+0x13DC, which a game function reads), stores each bound stream
  object's input words (+0/+4), and leaves the stream-set cache clear; r3 is
  0. The inputs it binds -- through up to four stream sets -- are decided at
  its entry from the sets, the current program pair's input table and the
  sentinel, so the record is built at the entry from them. A draw with a
  third extra stream set is left to its body (never seen).
- **The program functions** (vertex upload 0x006CA3D0, constants 0x006C9C74,
  fragment upload 0x006CADE4, patch 0x006C9624, pair bind 0x006C99B0): the
  fragment object's dirty byte (+0x50, set by an upload or patch, cleared by
  a bind), the upload's configuration fields (+0x54-+0x68), r3 1 where
  callers read it; the fragment heap's one-time set-up runs as guest code.
- **The state functions** (the setters, clears, pass start and end, render
  target set, sampler bind, texture apply, the shadow map, and the store's
  re-emission -- including the whole store's replay at every pass begin):
  asp's state store has no reader but its own re-emission, so with the
  re-emissions skipped too it need not be kept (the recorder keeps its own
  model of it, A.stored, and applies each re-emission from it); kept are the
  sampler table's dirty bytes, the device's surface-format byte (+0x12C), the
  pass's +0x418 byte, the viewport cache 0x006D4678 keeps (+0x88,
  +0x1390-0x139C), and the r3 values callers read or pass on (texture apply,
  pass start, render target set 1; pass end the ring context; setters the
  store context).
- **The game's and PPFX's remaining libgcm calls** on the title's context
  (=all): every entry point R4 classified as bypassed or kept -- PPFX's kept
  calls, the game's own state calls (its mesh path's vertex arrays, Bink's,
  the frame start's state reset's 15 observed setters and its clear) --
  does not run [M, C]. R4's verify showed that
  the bypassed entry points' bodies (on PPFX's calls) store nothing but
  their words; the kept ones and the game's own calls were outside it and
  rest on the review's reading (cellGcmSetVertexProgram's nested calls were
  the black-scene defect below). Left running: the flip, the flush,
  the helpers that write no command, labels, and the 13 libgcm functions
  outside the observed set (no entry hook; below): the frame start's other
  8 setters, the performance-marker push and pop, the wait-flip acquire, the
  profiling meter's timestamps, the report location (section 9) [M, C].

**Each replacement checked against its body** (DOTHEROES_R6_SKIP=verify: the
body runs; the replacement's prediction is made at its entry -- for state
functions, on a copy of the recorder's state with its guest stores listed,
not made -- and compared at the body's return):

| Run (b16, verify: v4river, v4menus) | calls compared | differences |
|---|---|---|
| river 100 s: asp's draws -- binds (stream object, table entry, in order), stream words, the counter, the cache, r3 | 2,043,700 | 0 |
| river: program functions -- object fields (+0x50, +0x54, +0x58, +0x64), r3 | 19,188,246 | 0 |
| river: state functions -- the recorded state (the whole State compared), the guest stores, r3 | 4,241,481 | 0 |
| menus, the scenario to its end, 198 s (the title, Bink, every menu): state functions | 9,982,310 | 0 |

**Which bodies no longer run** (=all, a1river, river 100 s): asp's draws
2,156,827 replaced at entry (none left to its body), the game's draws 171,394,
program functions 15.7 million, state functions 4.34 million (none left to
the body), store re-emissions applied from the model 168,184; the libgcm
calls whose body did not run are counted by entry point and caller in the
report (`r6 libgcm calls whose body did not run`).

**A defect the final checks found, fixed: the black scene.** From the
build that added the libgcm calls to `=all` (b18) until bH, every run with
`=all` drew the 3D scene black, with the 2D HUD intact: batch 6's runs with
`=all` (all but tr0river and c6river, which ran without it; tr0river's grab
drew the scene, 0.99 of pixels above black) and the R6 arms of the first
measurement batch (section 8 says what was re-run) [M].
Found in batch 7's grabs (a grab's share of pixels above black: 0.08
against 1.00), it was missed at first because the runs' counters were all
correct -- draws, reports, verify, validation -- and nothing compared the
pictures. A bisect on the final build (`DOTHEROES_R6_KEEP=<entry points>`,
a development switch that lets listed bodies run under `=all`) narrowed it
to one entry point: PPFX's cellGcmSetVertexProgram (0x006A4E50). Its body
ends by calling cellGcmSetVertexProgramParameter (0x006A39EC, observed) for
each parameter with a default value -- R4 had kept the body for "its
default constants, read by others" -- and the recorder had only ever learnt
those constants from the nested calls. With the body skipped they were never
set, and the scene's shaders read zeros. Of the 30 entry points `=all`
skips, it is the only one whose body calls another observed function (the
others' only call is the ring-full callback). The replacement now applies
the defaults itself, as the body does (030: loc_006A514C): each
CgBinaryParameter (0x30 bytes; count at the program's +0x0C, array at +0x10)
with a default (+0x14) and variability uniform or constant (0x1006/0x1007),
its first 16 bytes -- for a matrix, the frame's next 48 bytes, the caller's
saved r24-r29 (none seen) -- after the program. Checked against the body
(DOTHEROES_R6_VP_VERIFY=1 with the body run: the prediction at the entry
against each nested call's parameter and value words):

| Run (bH; vv1 bI) | programs | default constants compared | differences | count differences |
|---|---|---|---|---|
| z2river (verify), river 100 s | 171,945 | 343,890 | 0 | 0 |
| z5menus (verify), menus, 198 s | 374,523 | 749,046 | 0 | 0 |
| vv1river (bI, verify, the game's programs too), river 100 s | 172,735 (the game's 553) | 344,917 | 0 | 0 |
| vv1menus (bI, verify), menus, 198 s | 396,984 (the game's 1,166) | 792,802 | 0 | 0 |
| z1river, z3fields, z4menus (the fix) | 172,267 / 346,768 / 397,837 programs, their defaults set by the replacement | - | - | - |

With the fix, the fields' and menus' grabs follow R6's earlier runs without
`=all` grab for grab (mean brightness within 2 of n1fields' at 33 of 39
grabs and of n1menus' at 38 of 39; the largest differences at grabs just
after a fade, 21 at the fields' 36 s grab and 3.5 at the menus' 155 s; every
other grab within 3.7), and the river's grabs every 5 s match the same build with the
body run and with `=1` (z6, z7, z8river: 129-138 from 60 s on). The lesson
kept for R7: a replacement is checked against its body's whole observable
effect -- including the calls it makes -- and the pictures are compared,
not only counters.

**An independent review of `=all`** (a workflow: three reviewers -- the fix
against the lifted bodies; every skipped body's effects beyond its words;
every consumer of engine state on R6's path -- and one adversarial
check of each finding; 22 agents). It confirmed the replacement (the loop,
the filter, the default's offset, the 16-byte copy, the saved registers a
matrix would read, the order) and found, of 19 findings, 8 that survived
their check (the first row holds two of them; the last row, not changed, is
from one refuted):

| Finding | Disposition (bI) |
|---|---|
| The parameter model (R3's enc_vp_const, now under `=all` the constants' only source) is not 0x006A39EC's decode: CG_FLOAT1 (0x443), the 3x3, 3x4 and 4x3 matrices, a 4x4 at its first row's register, rows CG_UNDEFINED or at -1, a parameter CG_UNDEFINED | the model now follows the body's jump table; `vp_new_cases` counts every parameter the two decodes would treat differently: 0 in every bI and bJ run with `=all`, vv1river and vv1menus included |
| The verify covered PPFX's programs only, not the game's own (Bink's, the mesh path's) | the game's calls verified too: vv1river 553 and vv1menus 1,166 of the game's programs, 0 differences (`programs (the game's N)`) |
| A clear made after a frame's last draw waits for the next frame's first draw: a frame that draws nothing after the game's frame-start clear is presented with the buffer's older picture (the engine showed it cleared) | the flip record carries the pending clears; they are done before the present (`clears done at a flip`). Which clears they are (bJ, a classification added after the review; fc1river 100 s, fc1menus to its end, 198 s): of 3,848 and 4,507 done (the kinds are counted when the flip is recorded: 3,848 and 4,508 -- the menus run's last flip, one clear pending, was recorded but not done before the exit), depth or stencil only 3,690 and 4,335 (96%; not in the picture presented; those named in the report are mask 01 on the depth buffer the display buffers share, 0x00730000); colour of the buffer the flip shows 158 and 171 (the nine of each named in the report are mask F1, colour with depth, at consecutive flips of frames 1202-1210 and 1200-1208: frames that draw nothing after the frame-start clear; that the rest are such frames too -- the loads and transitions, the frames the fix is for -- is inferred [I]: the count does not record the mask); colour of another surface 0 and 2 |
| Bink's back-end label is written by the walker as it meets it; the native thread reads Bink's 900 KB luma plane later, and Bink decodes into the set again once the label has passed | Bink's records take every texture at the walker, before the label (`game draw-arrays records with every texture taken at the walker`) |
| Flip and report markers took a reused slot's need[0] into the bridge's table of what the native side holds | they leave it alone |
| PPFX's colour clears clear every channel even if the mask names some (the engine did the same) | counted and named when seen (`PPFX colour clears of some channels only`) |
| The post-boss warp's transfer through a handback was never exercised under R6 | marked inferred (section 2); R7 item 4 |
| (not changed) A matrix default reads the caller's saved registers beyond the 16-byte copy | as the body; 0 matrix defaults seen |

Among the refuted: the report location (the game selects the local report
area just before every cellGcmSetReport -- func_003AA9FC -- so the native
reports' fixed local area is right; main memory is selected only by the
profiling meter, for the timestamps the walker still writes), and the depth
clear value at a flip (the engine reset it to 0xFFFFFF00 each flip; PPFX
sets that same value every frame and the recorder starts with it).

**Which command words are no longer generated** (attributed frame traces,
PS3_ATTRIB_SHADOW=1 over the command buffer, 60 frames of the river fight,
tools/r4_trace_audit.py; the recorder's stream markers excluded):

| Run | command words written by guest code in 60 frames | a frame | writers |
|---|---|---|---|
| tr0river: R5's configuration (asp native, its draws taken back) | 7,859,232 (7.50 M by asp's and the game's own writers, 313 k by R4's kept entry points -- 271 k of them PPFX's calls --, 45 k by bypassed entry points the game calls, 5.5 k by libgcm's unobserved functions) | ~131,000 | 60+ functions |
| tr1river: R6 (producers, present, `=all`, occlusion) | 5,520 | 92 | 13 libgcm functions, all called by the game |

No asp, PPFX or game draw, state or program body writes a word in tr1; the
trace is 4 MB against tr0's 133 MB. The 13 writers that remain are libgcm
functions outside the observed set (no entry hook, so no replacement), each
called by the game:

| Writer | Method(s) | Words a frame | Caller | What it is |
|---|---|---|---|---|
| 0x0069DF80, 0x0069E0E0 | NV4097_NO_OPERATION carrying a string ("Shadow" ...) | 24 | the game's pass code (0x00084xxx) | performance-marker push and pop: inert |
| 0x0069E58C | NV406E semaphore offset and acquire (label 0x10 == 0) | 4 | 0x00029D04 | the wait-flip acquire |
| 0x006A5624 | NV4097_GET_REPORT, timestamps (offsets 0x4000-0x4270, 0x10 apart: 10 a frame, rotating through 40 slots every 4 frames) | 20 | 0x00474404 | the profiling meter's timestamps (its reports read by the game) |
| 0x006A9358 | NV4097_SET_CONTEXT_DMA_REPORT: 0xBAD68000 (main memory) 10 times a frame from 0x004743C0, 0x66626660 (local memory) once from 0x003AAA38 | 22 | 0x004743C0, 0x003AAA38 | the report location |
| 0x006A5B8C, 0x006A5F50, 0x006A6190, 0x006A6CF0, 0x006A6EC0, 0x006A7094, 0x006A72D4, 0x006A7484 | blend equation, 0x0370, front face, 0x1D78, 0x036C, depth bounds enable, two-sided stencil, 0x1EE4/0x1EE8 | 22 | 0x00029618-0x00029930 (front face also 0x0045BC28), once or twice a frame | the frame start's state reset (no engine draw reads it in R6) |

Removing them is R7's (section 9): the reset and the markers by entry
hooks that skip them; the acquire, the timestamps and the report location
by native replacements that keep what the game reads.

The specification work also found where R5's recorder described asp
differently from its code, fixed in R6 (section 5).

## 4. The game thread [M]

R5 moved the bottleneck to the game thread (uncapped: busy 9.7 ms a frame
against production's 6.2). R6's changes to it, in order:

- **cheap cuts in the recorder**: single-writer counters without locked
  adds, the recorder's own timer sampled (one call in 16), no wake-up on
  every record's publication (nothing waits for it), the fragment program
  blob reused while no transfer has written program memory (it was read word
  by word through a map and hashed byte by byte at every asp draw), the
  shadow read a page at a time, the vertex constants as a delta per record
  (the ranges set since the previous record; the native thread, the records'
  one in-order consumer, applies them to its own copy -- 8 KB copied per
  record before), R4's bypass decision's map and lock replaced by a cache;
- **the bodies skipped** (section 3): asp's draws no longer run their
  stream code (about 1,900 observed calls a flip of it went), the program
  and state functions no longer write the ring or the store;
- **a lean native-play lift** (tools/r6_native_lift.sh,
  config/r6_native_observers.json): the same API boundaries without the
  development lift's store probes (PPU_OBS_ST on every store of an observed
  body -- the game's memcpy among them).

Observed calls a flip (u1river, DOTHEROES_R6_CALL_COUNTS=1, before the
skips): asp's constants 0x006C9C74 1,271, its stream unbind 0x006D32D0 1,235
and bind 0x006D3190 673 (inside its draws), the game's vertex arrays
0x0069F19C 612, asp's fragment patch 0x006C9624 476, pair bind 249, draw 232,
sampler bind and texture apply 198 each, memcpy 164.

| Uncapped river fight (benchmark mode) | fps | game thread busy a frame | walker active |
|---|---|---|---|
| production (bq1-prod) | 137.5 | 6.03 ms | 5.55 ms |
| R6, producers and present, recorder cuts (u1, b9; call counting on) | 102.2 | ~9.4 ms | 1.61 ms |
| + draws and program functions skipped (u3, b13) | 143.2 | 6.94 ms | 0.57 ms |
| + state functions skipped (u4, b16) | 144.3 | 6.71 ms | 0.41 ms |

**Where the game thread's time goes with everything skipped** (pr1river:
the sampling profiler over the uncapped fight, the development build b18,
6,090 main-thread samples; b18 had the black-scene defect, section 3 -- the
GPU's constants wrong, the game thread's work the same but for the defaults'
replacement added since; the recorder's sampled time is the same before
and after the fix, 0.80 ms a frame in the fight, so the shares stand [I]):
the recorder -- everything under the entry observer -- is 23.5% of the game
thread's samples (about 1.8 ms of pr1river's 7.7 ms frames under the
profiler; about 1.6 ms of u4's 6.7 ms busy frame [I]); the rest is the
game's own code. Inside the recorder: asp's calls 14.9% (asp_record_call_inner
12.2%, the program functions' replacements outside it 2.2%, the state
functions' 0.6%; its draw's replacement 9.0%, of which building the record
7.0% -- the same record building in the game's draws another 1.1%, within
their 1.5%), the shader blobs' identity 3.7% (the fragment program's 2.3%,
the vertex program's 1.4%, inclusive; hashing the blobs, blob_of, 2.8% in
all), the game's draws 1.5%, the vertex-constant delta 1.3%, the program
shadow's page lookups 1.2% (64 of their 73 samples in writes to it: asp's
uploads and patches, the inline transfers), a map lookup
(unordered_map<u32,u32>) 1.1%, the observer's own dispatch 1.6%. What is left to cut is in the record's
construction and the program identity (R7: identity by the guest's own
upload, not by reading the program).

The final comparison, with the lean lift and the libgcm calls skipped too,
is section 8.

## 5. R5's correctness gaps [M, C]

- **The start-up draws** (records 1166-1175 never drawn: "asp: pipeline"):
  their fragment program's bytes were another program's. A program asp binds
  without uploading it again is refilled from its object, keeping the words
  patched since the slot last got a program -- and those included patches
  made for the *previous* program in the slot. Each word of program memory
  now has an owner (the object whose upload or patch wrote it); a resident
  bind keeps only its own object's patches. No R6 run since reports a
  pipeline failure (only the very first frame's 7 draws before any surface
  is set: "no clip").
- **Recorder mismatches with asp's code** (found by the specifications,
  section 3): 0x006C9C74 with its handle's bit 31 clear patches a fragment
  program parameter -- R5 wrote it into the vertex constants (the
  cross-check's c0 0x3F000000 against the engine's 0 from frame 3999); the
  pair bind emits nothing when its object's dirty byte is clear (the program
  stays as it was); the fragment object is the pair's +0xF0; the shader
  control is the register count (at least 2) over the whole +0x64 word; the
  vertex program's slot and register count come from its binary; its own
  constants' registers are a u16 table; an upload with +0x4C not -1 uploads
  nothing; the clear's stencil is r7 and its colour and depth are truncated;
  a pass start with +0x0C set is a colour pass (depth and stencil off, a
  white clear) and its inset scissor only with r5; the render target set
  takes its surface by index and nothing for an unset one; the shadow map's
  unit is r6's and its filter nearest unless r5; the anisotropy ratio 0 is 1
  and caps at 16; the viewport's depth offset is 0. The sampler's LOD bias
  is still recorded as 0: the binds that would carry one are counted, and
  none was seen.
- **The scene against the engine** (R3's arrangement, DOTHEROES_ASP_COMPARE,
  river 100 s): R5 (build/r4/fc1) 3,177 of 3,348 targets equal, 0.0111% of pixels
  differing; R6 3,116 of 3,272 (b10) and 3,250 of 3,412 (b13), 0.0036% and
  0.0043%; the area's first frames 35,128 and 87,802 differing pixels
  before, at most 47 after on b13 (b10: 11,133 in frame 2151); the worst
  later frame 55,628 before, 15,388 (b10) and 14,822 (b13) after. What still differs is the moat's band in some frames and tens of
  pixels in the area's first frames.
- **The water's texture** made a new image every frame: images the
  static-texture sweep drops go into a pool once the batches that may use
  them have completed, and a texture of the same shape takes one from there
  (a1river: 4,107 images reused, 13 live, 18 pooled).
- **Dynamic texture data, open.** Small uncompressed textures (up to
  256 KB) are still taken by the walker at the draw's place in the stream
  (73,920 a river run, 4.8 GB), larger ones and DXT when the native thread
  draws (the write watch telling which ranges changed). The moat band is
  presumably the water's texture rewritten between the game's draw and that
  capture [H: not measured in R6; R5 found the walker-timed capture right,
  and the engine's sparse texture hash explains part].
  The explicit rule replacing the walker's timing -- data read once its
  commands are flushed, later CPU writes detected by the write watch -- is
  R7's first item, with the walker's removal (section 9).

## 6. Occlusion results [M, C]

The map found two users of the RSX's zpass counter, and no conditional
rendering (every cellGcmSetRenderEnable is mode TRUE):

- **PPFX's luminance probe** (report 769): one full-viewport triangle into a
  1x1 target in main memory with zpass counting on, cellGcmSetReport(1,
  769), then cellGcmSetTransferReportData copies the report into the probe's
  result buffer, whose last word the CPU has just set to 1 ("pending"). The
  probe polls that word for 0, then reads the 1x1 pixel's bytes and turns
  them into a luminance (0.2220 R + 0.7067 G + 0.0713 B) for its exposure
  adaptation. Until R6 the copy was never carried out (the engine dropped
  it), so the poll never saw it ready and the probe was issued once an area.
- **The game's own query** (report 128): the draws it makes with counting
  on -- 10-11 a report in a river area's first frames after a load
  (build/r3/lt1-3), about 20 in the river fight (o1river: 72,313 counted
  draws over its 3,632 reports; 23 in each of the frame trace build/r3/gt's 4 reports, 22 in each of its
  boundary log's 12), about
  10 in the menus, about 6 in the dungeon -- read with cellGcmGetReport at
  once, and tested against 10. The engine wrote a
  fixed 0xFFFF at the report.

DOTHEROES_R6_OCCLUSION=1 (with the native present; a behaviour change, kept
apart from the rest): the counting, clear-report, report, report-transfer
and render-enable calls do not run; the clear-report, report and transfer
calls are records, and the counting switch is state that every draw record
carries. The native renderer
counts every draw made with zpass counting on with a precise occlusion
query (occlusionQueryPrecise enabled on the shared device), a
cellGcmSetClearReport starts a new count, and when the batch holding a
report completes -- the GPU has the counts -- it writes the report
{timer, count, 0} into the report area, the pad word last; a transfer, at
its batch's completion, first writes back the main-memory colour targets
the records drew into (the probe's 1x1 pixel, as the guest's A, R, G, B
bytes), then copies the reports, the pad word last. Batches now retire in
submission order, so a report's counts are always read before it is
written. A conditional render enable would be counted and named (none seen).

| Run (o1river, b9, river 100 s) | |
|---|---|
| zpass-counted draws, each with a query | 72,514 |
| reports written / transfers | 3,833 / 201 |
| PPFX's probe | issued and completed every 18 frames (frames 2160, 2178, 2196, ...): its adaptation loop runs |
| the game's report 128 as it reads it | 0 (before its first query), then the counts (87,814, 265,027) instead of a fixed 65,535 |

What the adaptation changes on screen is subtle in the river's daylight
(a grab at the same moment shows no visible difference); the comparison
that would settle it is the owner's side by side with RPCS3, which runs this
game with accurate, strictly synchronised ZCULL reports (the owner's
configuration: `Accurate ZCULL stats: true`). The game's own count is
above 10 either way in these runs, so its branch is unchanged.

## 7. Synchronisation and shutdown [M, C]

**The shutdown rule.** At exit R4 waited 10 s (its limit in play too) and
R5 100 ms for an engine signal a
batch depended on, then submitted the batch anyway -- with the wait on a
semaphore whose signal was never submitted (invalid:
VUID-vkQueueSubmit-pWaitSemaphores-03238), and its commands reading what the
engine's copies would have written. R6 cancels such a batch instead: it is
never submitted; its completion work runs once every batch before it has
completed; its slots are given back. In play, a wait that long is a bug: it
is reported every 10 s and the wait goes on -- never a submission that
waits on an unsubmitted signal. A failed vkQueueSubmit no longer leaves its
batch pending (retiring it waited forever on a fence never submitted).
**The native thread stops at exit**: after the drain it submits what is
open, waits for every batch and returns, so no native work is in flight when
the process ends (val1river: drain 77 ms, the thread stopped, 0 batches
cancelled). Batches retire in submission order.

**Validation** (the Khronos layer with synchronisation validation and its
shader-access heuristic, the whole R6 path: producers, native present,
every body skipped, occlusion). Batch 6 ran on b18, with the black-scene
defect (section 3): its synchronisation results stand for the work that
build did, and were repeated on the final development build (bI, with the
fixes) -- the first table is bI's:

| Run (bI, final) | errors | warnings and coverage |
|---|---|---|
| val6river, 150 s | 0 (no SYNC-HAZARD) | the known Undefined-Value-ShaderOutputNotConsumed (10 reported: the layer's duplicate-message limit); the fight reached |
| val7river, lifetime stress under validation, the drive to its end (198 s; limit 240 s): everything retired every 30 frames, two vertex arenas | 0 (no SYNC-HAZARD) | the known warnings (10 reported); 3 of 3 segments (to the river, the fight, back); 9,042 images retired and made again; 1,897,006 of 1,897,596 markers met; exit drain 5.3 ms, 0 cancelled |
| val8river, negative control: the present's wait dropped (DOTHEROES_R6_HAZARD=present) | SYNC-HAZARD-READ-RACING-WRITE (10 reported: the layer's duplicate-message limit), SYNC-HAZARD-WRITE-AFTER-WRITE x1: the layer still sees the hazard | the same known warnings |

| Run (b18, batch 6) | errors | warnings |
|---|---|---|
| val1river, 90 s | 0 (no SYNC-HAZARD) | the known Undefined-Value-ShaderOutputNotConsumed (10 reported: the layer's limit) |
| val2river, negative control: the present's wait dropped (DOTHEROES_R6_HAZARD=present) | SYNC-HAZARD-READ-RACING-WRITE (10 reported: the layer's limit; the present's copy reads the target the native blit wrote on the other queue), SYNC-HAZARD-WRITE-AFTER-WRITE | the same known warnings |
| val3river, negative control: R3's in-batch hazard (DOTHEROES_PPFX_HAZARD_TEST=1) | SYNC-HAZARD-WRITE-AFTER-WRITE x2 | the same known warnings |
| val4river, lifetime stress under validation, 60 s | 0 | 0 -- but under the layer the run never left the title (572 images replaced, no gameplay draw); val5river repeats it, the drive to its end (197 s; below) |
| val5river (bF), lifetime stress under validation, the drive to its end (197 s; limit 240 s): everything retired every 30 frames, two vertex arenas | 0 (no SYNC-HAZARD) | the known warnings (10 reported); the run reached and fought the river (3 of 3 segments), 8,953 images retired and made again, 1,913,767 of 1,914,357 markers met, exit drain 17.0 ms, 0 cancelled; 131 flips not presented (no native image: section 2, fixed in bI) |

**Backpressure, lifetime, cycles** (the whole R6 path; bI's runs first):

| Run | what was forced | result |
|---|---|---|
| bp2river (bI), 100 s | the 64-record queue and the slowed consumer, as bp1 | 1,352,207 waits for room (52.4 s of the run), each with a flush for room; 1,613,592 markers written, all met; no hang; the fight reached. Its exit drain reached its 15 s limit: the game kept making frames while it exited and the slowed consumer never caught up -- then the orderly stop: the native thread stopped with every batch it had submitted complete, 0 cancelled. The shutdown's limit path, exercised: a timeout ends the wait, not the GPU's work, which is waited for |
| cyc2dungeon (bI), 383 s | three title and area cycles | 9 of 9 segments; 0 foreign draws; 16,750 clears done at a flip (clears made after a frame's last draw; in bJ's river and menus runs 96% of them depth or stencil only, the rest the loading and transition frames' colour clears -- section 3's review); 9,232 static images reused from the pool; exit drain 11.0 ms, 0 cancelled; grabs every 10 s follow the earlier runs' |
| bp1river, 100 s | a 64-record queue (DOTHEROES_R6_QUEUE_CAP=64) and a consumer slowed by 40 us a record (DOTHEROES_PPFX_DELAY_US=40): the game waits for room | 341,564 waits for room (39.9 s of the run), each with a flush for room; 473,415 markers written, all met; no hang; the fight reached; exit drain 7.5 s (the game kept making frames while it exited: over 130,000 records put after the stop trigger, drawn by the slowed consumer; the 64-record queue itself is about 3.6 ms of its work), the thread stopped, 0 batches cancelled |
| lt1river, 100 s | every native image, depth target, guest and static texture retired every 30 frames (DOTHEROES_PPFX_REALLOC_EVERY=30); vertex arenas kept to two (DOTHEROES_R6_ARENAS=2) | 9,908 images retired (each destroyed once the batches in flight were done, except the static textures: those went into the static-texture pool, 0 destroyed past it) and made again, or taken from the pool (3,481 reused), at their next use; no device loss (the layer is off here; under it, val5river above); 2,202,947 of 2,203,623 markers met (the rest written after the walker's last pass at exit); exit drain 16.8 ms, 0 cancelled; 107 flips not presented (no native image: section 2, fixed in bI). Two arenas sufficed: **arena retirement was not exercised** (0 retired) |
| cyc1dungeon, 382 s | three cycles of title -> load -> castle -> dungeon entrance -> back to the title (RUNS=3) | 9 of 9 segments; 819,719 native draws; 0 foreign draws, 0 handbacks; 8,053 static images reused from the pool, 0 destroyed past it; exit drain 12.4 ms, 0 cancelled |

A larger queue only makes the full-queue path rarer; bp1 and bp2 force it
on 72% and 84% of records and show the path itself safe in play: the game
waits for room, the native thread drains, every marker is met, nothing
hangs. At exit the 64 records still queued in each run were walked but not
drawn (the orderly stop), and bp2's drain reached its 15 s limit.

**The dual-run comparison with the producers, open and isolated.** In R3's
arrangement (the engine drawing everything too: no share, no bypass) with
the producers on, the record queue fills at the title's transition and the
game stalls. Where (c6river, b19): the native thread is at record 7,543
(frame 1,222, an asp draw), inside its batch's submission, waiting for the
engine to submit a signal the batch waits on (position 3); the walker has
passed every record (walked = put = 11,639) [M]. The engine submits its open
batch only at its own frame points, which the game -- blocked on the full
queue -- no longer reaches: a cycle [I, from the code]. R6's configurations
do not meet it: the bypass leaves no engine work for a native batch to wait
on, and the engine's batch is flushed at every natively presented flip
(section 2). Without the producers the dual run runs (the present
comparison: 41 of 44 compared presents equal). A fix, if the dual run is
kept for R7's comparisons: the walker flushing its open batch whenever it is
idle and the native side waits. The producers' output is checked instead by
the scene comparison (section 5), the snapshot, and the grabs.

## 8. Measurement [M]

Batch 16 (build/r6/m2*, 2026-11-06), on the final builds (bI, nI): five
arms under the same conditions -- real audio, each scenario's own shader
cache (a copy, the same state for every arm), can't-die, normal CPU
placement, the window in front, every comparison and cross-check off, arms
rotated each round. It replaces batch 7 (build/r6/m1*): batch 7's R6 arms
ran with the black-scene defect (section 3), the 3D scene drawn black, so
their numbers are withdrawn, not merely superseded. Its production, R4 and
R5 arms were valid and agree with batch 16's (production's fight
135.6-152.2 fps there, 145.5-148.1 here).

| Arm | build | configuration |
|---|---|---|
| prod | the production build (no observers, no native path) | the production renderer |
| R4 | the final development build (bI) | PPFX native, its writers bypassed; asp on the engine |
| R5 | bI | asp native too, its images shared, its draws taken back |
| R6d | bI | R6: producers, native present, every body skipped (`=all`), occlusion |
| R6n | the lean native-play build (nI, section 4) | R6, the same switches |

R4 and R5 run on R6's development build, so they include R6's recorder cuts
(section 4); R5's own build measured 9.7 ms of game-thread time in the
fight. Frame times are measured only inside the drive's segments of the kind
named (the river fight's encounter; the fields' walks): a run that did not
reach them would be reported as not measured -- every run reached them, so
no run was dropped and none is pooled with another. Each run is listed
apart. Every run's grabs (the river's at 60 and 120 s, the fields' every
4 s) were checked against production's: the 3D scene in every R6 grab, its
brightness inside production's own spread (the river 136-148 against
136-148, and one production grab at 78), the fields grab by grab within a
few levels outside their transitions, which every arm, production's
included, catches at slightly different moments.

**The benchmark mode** (DOTHEROES_BENCH=1; never a play default): no frame
cap from the game (DOTHEROES_FPS=0) or the vblank clock (a flip completes as
soon as it has been executed, cellGcm_set_flip_immediate, so no fast-ticking
clock thread competes for a core), a mailbox present (PS3RECOMP_VK_PRESENT_MODE,
FIFO when the surface has none) so rendering is not held to the display's
refresh, each flip presented once, no per-frame copy to host memory. The
test harness's window finder now skips the NVIDIA driver's hidden helper
window a mailbox swapchain creates.

Columns: guest frames a second (flips) in the segment; flip-to-flip
intervals in ms at the 50th/90th/99th/99.9th percentile and the largest;
from the frame trace (60-70 s into the run, 60 s): the game thread's busy
time a guest frame (the frame less its idle time), the walker's active time,
the engine's GPU time (VK_GPU_TIME); from the native renderer's report: the
recorder's time on the game thread (sampled), the native recording thread's
CPU time and the native GPU time, a frame.

**Uncapped, the river fight** (debris-heavy combat, 150 s runs, three
rounds):

| Run | fps | p50 | p90 | p99 | p99.9 | max | game thread | walker | engine GPU | recorder | native thread | native GPU |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| prod-1 | 145.5 | 6.66 | 7.73 | 10.90 | 16.52 | 139.0 | 6.07 | 5.27 | 2.26 | - | - | - |
| prod-2 | 148.1 | 6.56 | 7.59 | 10.44 | 16.16 | 99.9 | 5.87 | 5.04 | 2.18 | - | - | - |
| prod-3 | 147.3 | 6.58 | 7.67 | 10.33 | 16.30 | 137.7 | 6.03 | 5.15 | 2.28 | - | - | - |
| R4-1 | 102.9 | 9.50 | 11.18 | 12.58 | 20.08 | 113.7 | 7.26 | 5.73 | 4.86 | 0.79 | 0.36 | 0.45 |
| R4-2 | 93.5 | 10.42 | 12.33 | 13.66 | 23.80 | 85.8 | 7.09 | 6.66 | 4.89 | 0.80 | 0.36 | 0.46 |
| R4-3 | 102.4 | 9.55 | 11.20 | 12.91 | 21.82 | 115.8 | 7.19 | 5.69 | 4.78 | 0.79 | 0.36 | 0.45 |
| R5-1 | 120.8 | 7.93 | 9.22 | 12.90 | 15.64 | 98.7 | 8.05 | 3.57 | 0.20 | 1.15 | 3.06 | 1.39 |
| R5-2 | 120.0 | 7.97 | 9.75 | 12.05 | 15.42 | 114.5 | 8.18 | 3.43 | 0.20 | 1.16 | 2.83 | 1.40 |
| R5-3 | 120.9 | 7.93 | 9.15 | 13.09 | 15.37 | 110.3 | 8.02 | 3.37 | 0.20 | 1.17 | 2.83 | 1.40 |
| R6d-1 | 147.2 | 6.47 | 8.11 | 9.99 | 11.90 | 128.3 | 6.70 | 0.33 | 0.00 | 0.82 | 1.61 | 0.74 |
| R6d-2 | 150.5 | 6.38 | 7.49 | 10.47 | 11.94 | 101.6 | 6.45 | 0.33 | 0.00 | 0.77 | 1.42 | 0.69 |
| R6d-3 | 149.8 | 6.41 | 7.72 | 9.54 | 12.04 | 124.1 | 6.55 | 0.34 | 0.00 | 0.81 | 1.49 | 0.74 |
| R6n-1 | 150.6 | 6.36 | 7.69 | 9.58 | 11.28 | 136.0 | 6.56 | 0.31 | 0.00 | 0.81 | 1.47 | 0.73 |
| R6n-2 | 152.4 | 6.31 | 7.38 | 10.17 | 11.40 | 17.3 | 6.38 | 0.31 | 0.00 | 0.81 | 1.46 | 0.72 |
| R6n-3 | 150.7 | 6.37 | 7.51 | 10.15 | 12.28 | 122.4 | 6.55 | 0.31 | 0.00 | 0.80 | 1.58 | 0.73 |

**Uncapped, the fields** (exploration, the drive to its end, 160 s; two
rounds):

| Run | fps | p50 | p90 | p99 | p99.9 | max | game thread | walker | engine GPU | recorder | native thread | native GPU |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| prod-1 | 179.2 | 5.46 | 6.12 | 8.40 | 11.03 | 116.1 | 5.41 | 2.13 | 1.75 | - | - | - |
| prod-2 | 181.4 | 5.39 | 6.08 | 8.16 | 11.94 | 109.1 | 5.29 | 2.10 | 1.72 | - | - | - |
| R4-1 | 164.0 | 6.01 | 6.64 | 8.02 | 11.70 | 115.5 | 5.92 | 2.63 | 0.77 | 0.48 | 0.32 | 0.50 |
| R4-2 | 163.1 | 6.04 | 6.68 | 8.14 | 11.31 | 115.3 | 5.93 | 2.53 | 0.76 | 0.49 | 0.33 | 0.51 |
| R5-1 | 149.0 | 6.63 | 7.34 | 8.92 | 11.54 | 113.4 | 6.48 | 1.36 | 0.16 | 0.66 | 1.23 | 1.26 |
| R5-2 | 147.8 | 6.69 | 7.45 | 9.10 | 11.49 | 111.4 | 6.53 | 1.37 | 0.16 | 0.67 | 1.22 | 1.26 |
| R6d-1 | 168.9 | 5.86 | 6.41 | 7.71 | 10.38 | 101.4 | 5.75 | 0.07 | 0.00 | 0.45 | 0.78 | 0.74 |
| R6d-2 | 169.7 | 5.83 | 6.37 | 7.77 | 9.78 | 94.1 | 5.72 | 0.08 | 0.00 | 0.47 | 0.79 | 0.76 |
| R6n-1 | 169.5 | 5.83 | 6.39 | 7.82 | 10.38 | 94.3 | 5.74 | 0.07 | 0.00 | 0.47 | 0.77 | 0.76 |
| R6n-2 | 169.7 | 5.83 | 6.38 | 7.69 | 10.30 | 90.2 | 5.73 | 0.07 | 0.00 | 0.47 | 0.77 | 0.76 |

Every run but two, production's included, has one interval over 25 ms in
its segment (86-139 ms): the same hitch in every arm, not R6's. R6n-2's
fight has none; R4-2's fight has two more besides it (28.2 and 25.8 ms,
about 3 s into the encounter).

What it shows:

- **Throughput: R6 a little ahead of production in the fight, behind it
  exploring.** The fight: R6n 150.6-152.4 fps, R6d 147.2-150.5, production
  145.5-148.1 -- R6n 3% ahead. The fields: R6n 169.5-169.7 against
  179.2-181.4, 6% fewer frames. R6 is bound by the game thread (busy to
  within 0.07 ms of the guest frame in both scenarios); so is production
  exploring (0.04 ms), while in the fight production's game thread idles
  0.75-0.83 ms a frame, waiting on the engine's side. R6's game thread is
  still slower than production's: 6.38-6.56 ms (R6n) against 5.87-6.07 in
  the fight, 5.73-5.74 against 5.29-5.41 exploring -- 0.5 and 0.4 ms more.
  The recorder's sampled 0.81 and 0.47 ms (R6n; its own time, the
  observers' dispatch not in it) is more than that difference; the rest
  (about 0.3 and 0.1 ms) is net of everything else that differs, the
  skipped bodies chief among them [I: not measured apart; section 4's
  profile puts the whole observer path at 23.5% of the game thread, about
  1.6-1.8 ms].
- **R6 against R4 and R5**: the fight 93.5-102.9 -> 120.0-120.9 ->
  150.6-152.4 fps, the fields 163.1-164.0 -> 147.8-149.0 -> 169.5-169.7 (R5
  on R6's build is the slowest exploring: its game thread is the limit,
  busy 6.48-6.53 ms against R4's 5.92-5.93, 0.18 ms of the difference the
  recorder; the rest not measured apart [I: its taken-back draws' work on
  the game thread -- its recording thread, 1.2 ms, runs on its own]). R5's game thread: 8.0-8.2 ms in the fight on this
  build, 9.7 on its own.
- **The lean lift: within the runs' spread.** 0.07 ms less game-thread time
  than the development lift in the fight on average (6.50 against 6.57;
  round by round 0.14, 0.07 and 0.00), none exploring (5.735 both). Batch
  7's 0.34 ms rested on one round's 0.88. With the bodies skipped, the
  development lift's store probes have little left to probe.
- **Walker and GPU**: the walker's active time falls from 5.0-5.3 ms a
  frame (production) to 0.31-0.34 in the fight, 2.1 to 0.07-0.08 exploring;
  the GPU's from 2.2-2.3 ms (the engine) to 0.69-0.74 (native, occlusion
  queries included) in the fight, 1.7 to 0.74-0.76 exploring. Batch 7's R6
  GPU times (0.49-0.71 ms) were the black scene's.
- **Frame-time tail**: R6's p99.9 is shorter than production's in every
  uncapped run: 11.3-12.3 ms against 16.2-16.5 in the fight, 9.8-10.4
  against 11.0-11.9 exploring. Its p99 is shorter exploring (7.7-7.8
  against 8.2-8.4) and, in the fight, for R6n (9.6-10.2 against
  10.3-10.9); R6d's 9.5-10.5 overlaps production's (R6d-2's 10.47 above
  prod-2's 10.44 and prod-3's 10.33).
- **The 144 fps goal**: averages above 144 in both scenarios, but not
  sustained: 144 fps needs every frame under 6.94 ms. R6n's p50 in the
  fight is 6.31-6.37 ms but its p90 7.38-7.69; exploring, p90 6.38-6.39 and
  p99 7.69-7.82. Production is no better (the fight's p90 7.59-7.73). The
  remaining limit is the game thread -- the game's own code (77% of it,
  section 4) and the recorder; 200+ needs both cut.
- **Not counted as a win**: R5's experimental stage 2 is not among the arms
  (incompatible with the menus).

**Capped at 60 fps, the river fight** (the play default; two rounds):

| Run | flips p50 | p99 | p99.9 | max | presents p90 | p99 | p99.9 | presents >25 ms | recorder | native thread | native GPU |
|---|---|---|---|---|---|---|---|---|---|---|---|
| prod-1 | 16.68 | 17.24 | 17.61 | 19.4 | 18.62 | 22.04 | 24.36 | 1 | - | - | - |
| prod-2 | 16.67 | 17.52 | 17.69 | 18.6 | 18.39 | 21.90 | 24.52 | 4 | - | - | - |
| R4-1 | 16.67 | 17.39 | 20.45 | 21.7 | 20.18 | 22.87 | 24.34 | 2 | 0.80 | 0.37 | 0.55 |
| R4-2 | 16.67 | 17.87 | 20.70 | 22.7 | 19.92 | 22.34 | 23.91 | 1 | 0.80 | 0.37 | 0.55 |
| R5-1 | 16.67 | 17.22 | 18.76 | 22.8 | 18.33 | 20.54 | 22.14 | 0 | 1.19 | 3.01 | 2.08 |
| R5-2 | 16.67 | 17.25 | 17.62 | 17.9 | 18.26 | 20.33 | 23.08 | 0 | 1.18 | 2.96 | 2.07 |
| R6d-1 | 16.67 | 17.44 | 17.61 | 17.8 | 17.97 | 19.35 | 20.63 | 0 | 1.30 | 2.43 | 3.22 |
| R6d-2 | 16.67 | 17.20 | 17.52 | 17.6 | 18.04 | 19.53 | 21.12 | 0 | 1.28 | 2.43 | 3.16 |
| R6n-1 | 16.67 | 17.48 | 17.58 | 17.8 | 18.13 | 19.63 | 20.98 | 0 | 1.28 | 2.41 | 3.22 |
| R6n-2 | 16.67 | 17.15 | 17.51 | 17.7 | 18.02 | 19.38 | 20.62 | 0 | 1.28 | 2.72 | 3.21 |

Every arm holds 60.0 frames a second. R6's guest frames are the steadiest
(the largest interval 17.6-17.8 ms against production's 18.6-19.4), and its
presents too (p99 19.4-19.6 ms against 21.9-22.0; no present over 25 ms in
R6's four runs, five in production's two). The presents' intervals (the
present call's return, FIFO with three images) spread around 16.7 ms --
81-83% within 15-19 ms in production, 91-93% in R6 -- with no clusters at
the 144 Hz display's 13.9 and 20.8 ms steps. At 60 fps the same native work measures more
time a frame than uncapped (recorder 1.28 against 0.80 ms, native thread
2.4-2.7 against 1.5, native GPU 3.2 against 0.7).

**The GPU's clocks** (nvidia-smi once a second over two R6n river runs,
build/r6/m2clk-c.csv at 60 and m2clk-u.csv uncapped, after the first 30 s):
at 60 the GPU spends 96 of 121 samples in P5 (graphics 1,050 MHz, memory
810 MHz), 14 in P3 (1,110 and 5,001 MHz), 9 in P8 (210 and 405) and 2 in P0;
uncapped, 120 of 122 in P3 (graphics 675 MHz, memory 5,001 MHz) and 2 in P0
(1,410 and 7,001). At 60 the driver runs the memory at a sixth of its
uncapped clock [M], presumably for the slack the cap leaves [I]; the native
GPU's 3.2 ms a
frame against 0.7 is that clock, not more work [I: the clocks are measured,
the attribution inferred -- the same scenario, the same records]. The CPU
times rising at 60 (the recorder, the native thread) are presumably the
CPU's own power management at a lighter load [H: not measured]. Neither
reaches the game: its frames keep their 16.7 ms.

**Gameplay timing** (tests/fps_check.py on build/r6/m2t-*: the
castle-front drive's same inputs -- standing, a 1 s walk down and back up,
the grass with its debris, settling -- at 60 fps and in the benchmark mode,
production and R6n):

| Run | frames a second (still, walk, grass, settle) | walk down, up (z) | physics steps a second | most bodies | the game's clocks over 10 s standing, against prod-60 |
|---|---|---|---|---|---|
| prod-60 | 60.0, 60.1, 60.0, 60.0 | -4.99, +4.99 | 60.2 | 54 | - |
| R6n-60 | 59.9, 60.0, 60.0, 60.0 | -4.99, +4.99 | 60.2 | 55 | frame counters x1.00, time-based x1.00 |
| R6n-bench | 157.6, 157.8, 142.2, 150.5 | -4.86, +4.86 | 60.0 | 55 | frame counters x2.63, time-based x1.00 |
| prod-bench | 160.3, 167.1, 152.7, 161.4 | -4.86, +4.86 | 60.0 | 55 | frame counters x2.67, time-based x1.00 |

R6 at 60 plays as production at 60: the same walk to the hundredth,
physics at 60.2 steps a second, the game's clocks x1.00. Uncapped, R6 and
production walk the same 4.86 -- 2.6% short of 4.99 -- with physics at 60.0
steps a second and the time-based clocks x1.00: the same in both arms, so
not R6's; whether it is uncapped play's or the spread between runs is not
settled (batch 7's production runs: 5.14 and 4.99 at 60, 5.02 uncapped --
the spread between runs is of that order). The
game's frame counters run with the frame rate (x2.63 and x2.67) in both arms, which is
why the benchmark mode stays a measurement mode, never a play default. The
castle front uncapped: R6 2-7% below production (142-158 fps against
153-167), as in the fields.

## 9. What R6 leaves, and the R7 retirement checklist

R6 does not remove the PS3 graphics path; it leaves it with almost nothing
to do. In the R6 configuration (producers, native present, every asp body
skipped, occlusion) a river minute measures: no engine draw, no display
handoff or handback, no asp body run on the title's context, the engine's
GPU time 0.00 ms a frame (no engine GPU work in the frame trace; about
0.01 before the libgcm calls were skipped, u2-u4). What still runs, and what R7 must replace
before any of it can be called gone:

| Still running in R6 | Why it is still there | R7: what replaces it |
|---|---|---|
| **The FIFO walker** (rsx_dispatch / rsx_commands decoding the ring each frame) | it still meets the flip word (its half of the flip's completion), the wait-flip semaphore acquire, labels (Bink's back-end label), the profiling meter's timestamp reports, the stream markers -- and any word nothing skips | the flip completed by the native present alone; labels, reports and timestamps written natively at batch completion; no markers; then no walker |
| **The records' `walked` gate and the stream markers** (one marker a record) | the native thread draws only what the walker has passed: small guest textures are taken at the walker (73,920 a river run, 4.8 GB), at the draw's place in the stream | an explicit capture rule: by-address data read once its commands are flushed (the put register), CPU writes after that detected by the write watch and counted; the queue consumed at publish |
| **The bridge** (ppfx_bridge.cpp: matching, handoffs, handbacks, touch hooks) | handoffs and handbacks are 0 a frame in R6, but the engine still calls the touch hooks and the bridge still serves the post-boss warp's 2D transfer from a display buffer (a handback) and the comparisons | the warp's transfer as a native copy record; the bridge gone with the walker; comparisons kept in development builds only |
| **The game's libgcm bodies that still run on the title's context** | the flip and the flush (observed, kept running); the helpers that write no command; and 13 functions outside the observed set that write 92 words a frame (section 3): the frame start's state reset (8 setters), the performance-marker push and pop, the wait-flip acquire, the profiling meter's timestamps, the report location | entry hooks on the 13: the reset and the markers skipped; the acquire, the timestamps and the report location native (what the game reads written at the batch's completion); the flip and the flush replaced keeping their CPU-side effects (flip status, the put register as the game reads it, GetLastFlipTime); then no command words at all |
| **The ring and the GCM command HLE** (cellGcmSys's context, put/get, the reserve callback, the report and label areas) | the game's libgcm still writes into a ring and polls its registers | once nothing writes commands: the ring kept only as the memory the game expects, its registers answered natively; the method decoder, the RSX state and the engine's draw path excluded from release builds |
| **The backend's present and window** (rsx_vulkan_backend: swapchain, the offscreen target, the overlays) | the native present blits into the backend's target and the backend presents it | a native present module (swapchain owned by the native renderer); the backend's draw path not built |
| **The development lift's observers** | normal native play still runs on the development lift (or the lean native-play lift, section 4) | a release build: the production lift with native entry hooks at the same boundaries (the lifter's `enter`/`around` forms), no observers, no recorder diagnostics |

The checklist, each item to be shown by a counter or a trace:

1. [ ] Records drawn at publish, by-address data captured at the flush with the write watch -- the `walked` gate and the markers removed; the moat band compared again (section 5).
2. [ ] The flip: its completion from the native present alone; cellGcmSetFlip's body replaced (flip status, GetLastFlipTime, the queued count), no flip word written.
3. [ ] The 13 remaining writers hooked (section 3): the frame start's reset and the performance markers skipped; the wait-flip acquire, labels, the report location and the profiling meter's timestamps native (written at the batch's completion, as R6's occlusion reports are).
4. [ ] The post-boss warp's 2D transfer and any CPU read of a surface as native copies (the snapshot already is).
5. [ ] An attributed frame trace with no command word written by any game, asp, PPFX or libgcm function (tools/r4_trace_audit.py) -- the proof that command generation is gone.
6. [ ] The walker thread not started; rsx_dispatch, rsx_commands, rsx_draw_engine and the backend's draw path out of the release build; the bridge removed.
7. [ ] A native present module owning the swapchain; the window, OSD and frame dump moved with it.
8. [ ] A release lift with native entry hooks; the production renderer kept only as a development reference.
9. [ ] Validation again: the Khronos layer with synchronisation validation and its negative controls, lifetime stress, repeated area and title cycles, the six scenarios plus the editor, capped pacing, the gameplay-timing check, the uncapped measurement against R6.
10. [ ] The owner's play on the R7 build, and the comparison with RPCS3 for the occlusion-driven exposure (section 6).

Carried with them (not retirement, but open after R6):

- the post-boss warp's 2D transfer, never exercised under R6 (section 2): a
  run through the portal room after the boss -- a short owner session or a
  save there -- before or with item 4;
- Bink's movies compared frame by frame against the reference (R6 has their
  grabs only; their frames are now taken whole at the walker);
- every replacement checked against its body's whole effect, its calls
  included, and the pictures compared, not only counters (the black-scene
  defect, section 3);
- the vertex-arena retirement path forced by a test (lt1river's two arenas
  never needed retiring);
- the dual-run stall with the producers (section 7), if the dual run stays
  the comparison tool: the walker flushing its open batch when idle;
- the recorder's remaining cost on the game thread (section 4): building
  asp's draw record (8% of the game thread) and the programs' identity
  (2.8%) -- identity from the guest's own upload rather than by reading the
  program;
- the throughput goal (144 fps sustained, 200+ in demanding play) is for
  the finished migration: section 8 is where R6 stands, not the end point.

## 10. Reproducing

```
tools/r1_dev_lift.sh build                      (the development build; copy build/r1/bld into build/r6/<b>)
tools/r6_native_lift.sh                         (the lean native-play build: build/r6/native/bld)
tools/r6_scene.sh <tag> <scenario> <secs> [NAME=value ...]     (a run into build/r6/<tag>; R6_BIN picks the binary)
  R6: DOTHEROES_PPFX_BYPASS=1 DOTHEROES_PPFX_COMPARE=0 DOTHEROES_PPFX_XCHECK=0 DOTHEROES_PPFX_HANDBACK_CHECK=0
      DOTHEROES_ASP_RECORD=1 DOTHEROES_ASP_NATIVE=1 DOTHEROES_ASP_SHARE=1 DOTHEROES_ASP_BYPASS=1
      DOTHEROES_R6_PRODUCERS=1 DOTHEROES_R6_PRESENT=1 DOTHEROES_R6_SKIP=all [DOTHEROES_R6_OCCLUSION=1]
  verify:   ... DOTHEROES_R6_SKIP=verify
  validate: VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true
            VK_KHRONOS_VALIDATION_SYNCVAL_SHADER_ACCESSES_HEURISTIC=true VK_KHRONOS_VALIDATION_LOG_FILENAME=validation.txt
            VK_KHRONOS_VALIDATION_DEBUG_ACTION=VK_DBG_LAYER_ACTION_LOG_MSG VK_KHRONOS_VALIDATION_REPORT_FLAGS=error,warn
  benchmark: DOTHEROES_BENCH=1 PS3_FTRACE=70:60:ftrace.txt VK_GPU_TIME=1
tools/r6_measure.py <fight|explore> <tag dir> ...   (frame times in the drive's segments; thread and GPU costs)
tools/symbolize.py <exe> 0x<rva> ...                (a crash report's host stack, from the PDB)
```

Switches (all development, all off by default): DOTHEROES_R6_PRODUCERS=1,
DOTHEROES_R6_PRESENT=1, DOTHEROES_R6_SKIP=1|all|verify,
DOTHEROES_R6_OCCLUSION=1, DOTHEROES_R6_PRESENT_COMPARE=<n>,
DOTHEROES_R6_VCONST_FULL=1, DOTHEROES_R6_CALL_COUNTS=1, DOTHEROES_BENCH=1,
DOTHEROES_R6_QUEUE_CAP=<n>, DOTHEROES_PPFX_DELAY_US=<n>, DOTHEROES_R6_ARENAS=<n>,
DOTHEROES_R6_HAZARD=present, DOTHEROES_R6_KEEP=<entry points>,
DOTHEROES_R6_VP_VERIFY=1; the engine's PS3RECOMP_VK_PRESENT_MODE and
PS3RECOMP_VK_READBACK=0.
