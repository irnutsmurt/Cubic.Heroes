# R3: native PPFX in the running game, dual run (2026-11-05)

Milestone R3 of NATIVE_PORT.md, approved by the owner on 2026-11-04 after
R2. **R3 removes nothing**: the existing renderer still renders PPFX and
stays what the player sees. Beside it, PPFX's own libgcm calls are turned
into native records, a native renderer on a thread and Vulkan queue of its
own renders them into images of its own, and the two outputs are compared
from the same frames. Development builds only (the entry observers of
tools/r1_dev_lift.sh), off unless asked for; R4 does not start before the
owner has reviewed these results.

Labels as elsewhere: **[M]** measured, **[C]** from code, **[I]** inferred,
**[U]** unknown.

## Results

> **Follow-up (docs/NATIVE_R3B.md).** The first-load differences below
> are explained and gone: one item was the existing renderer misreading a
> memory-copy command, the other is asp's state, which the recorder now
> follows. Records are explicit descriptions, not a register block; the
> native renderer's barriers are per image.

**Every PPFX output compared is bit-exact, except in an area's first
frames, where PPFX draws with state that other code left and that no
libgcm call carries (section 4).** No tolerance is used or set; outputs are
compared byte for byte (half floats by their 16 bits, unorm8 by their 8).

The runs (tools/r3_scene.sh; RTX 3070) [M]:

| Scene | Frames | Records matched / missed | Outputs compared | Identical | Differing (all in first-load frames) | Final composites identical |
|---|---|---|---|---|---|---|
| River combat (rv2) | 8,853 | 311,746 / 0 | 20,380 | 20,362 | 17 | 6,632 / 6,635 |
| Exploration (fd2) | 16,633 | 657,149 / 0 | 83,318 | 83,286 | 31 | 13,981 / 13,984 |
| Field 14, from the owner's save (f14) | 8,879 | 316,822 / 0 | 33,249 | 33,242 | 7 | 6,741 / 6,743 |
| First dungeon, 3 title cycles (dg4) | 22,548 | 831,446 / 0 | 88,822 | 88,737 | 85 | 16,259 / 16,260 |
| First dungeon, 10 title cycles (st1) | 75,486 | 2,924,531 / 0 | 368,310 | 367,908 | 402 | 61,045 / 61,068 |
| First dungeon, 3 title cycles, differences attributed (dg5) | 23,080 | 839,637 / 0 | 106,236 | 106,092 | 144 (144 in frames the cross-check flagged, **0 elsewhere**) | 17,383 / 17,391 |
| **all** | **155,479** | **5,881,331 / 0** | **700,315** | **699,627** | **686** | **122,041 / 122,081** |

Outputs compared but neither identical nor differing (2 in all) were in
flight when their run ended; comparisons the pools had no buffer for are
counted apart in each report (dg4 15,232 and st1 3,335 before the pools
were reworked, dg5 1,122). The runs' builds differ only in the
measurement plumbing (comparison pools, counters); recording, matching
and rendering are the same in all of them (since commit 07d848d).

**By chain variant** (a frame's sequence of targets, and as a second level
of programs by structure) [M, the reports' variant lines]:

| Variant | Where | Frames | Draws compared | Differing |
|---|---|---|---|---|
| 47 draws (river, exploration, field 14), 1 to 5 program variants (pass 11's rewrites among them) | every scene but the dungeon | 6,622 + 13,971 + 6,730 + 9,086 + 32,850 | 20,238 + 83,222 + 33,153 + 61,188 + 259,976 | **0** |
| 49 draws, the dungeon's | the dungeon | 8,237 + 28,142 | 27,366 + 107,187 | **0** |
| 49 draws with two 1x1 passes (a first-load chain) | the first frame after each area load and CONTINUE | 1 a load | all | the inherited-state frames |
| 51 draws with two 1x1 passes (a first-load chain) | the dungeon's entries | 1 a load | all | likewise |
| 38 draws (a first-load chain) | 18 frames after a load | 1 a load | all | the colour-mask frame |

The comparison compares every frame's final composite, every PPFX draw of
every 30th frame, and every draw of a frame whose chain departs from the
frame before together with the 60 frames after it (state-triggered: the
first-load chains, the dungeon's first seconds, the chains after a
CONTINUE); outputs are read back right after their draw, before anything
can overwrite them.

**Controls** [M]: without handoffs (nh1) 12,291 of 12,691 compared outputs
and every final composite differ -- the comparison sees a missing input;
a deliberate hazard is reported by the validation layer (hz2); a native
side 5 ms a record slower (dl3) throttles the game through the full queue
and changes no result.

## Switches

| | |
|---|---|
| `DOTHEROES_PPFX_NATIVE=check` | records and the walker's match, with the cross-check; no native rendering |
| `DOTHEROES_PPFX_NATIVE=1` | also the native renderer and the comparison |
| `DOTHEROES_PPFX_COMPARE_EVERY=<n>` | every PPFX draw of every n-th frame compared (default 30); every frame's final composite always |
| `DOTHEROES_PPFX_COMPARE_AFTER_CHANGE=<n>` | after a frame whose chain departs from the frame before, every draw of the next n frames (default 60) |
| `DOTHEROES_PPFX_COMPARE=0` | no comparison at all (the native renderer's own cost) |
| `DOTHEROES_PPFX_XCHECK=0` | no cross-check |
| `DOTHEROES_PPFX_REPORT=<file>` | the report, every 10 s and at exit |
| `DOTHEROES_PPFX_DUMP=<dir>` | the first differing outputs, both sides, raw |
| `DOTHEROES_PPFX_DELAY_MS=<n>` | a test: the native renderer n ms slower a record |
| `DOTHEROES_PPFX_NO_HANDOFF=1` | a negative control: no handoffs |
| `DOTHEROES_PPFX_HAZARD_TEST=1` | a negative control: before one draw in 5000, two copies into one image with no barrier between them |
| `DOTHEROES_PPFX_REALLOC_EVERY=<n>` | a test of image lifetime: every n frames every native image retired and made again at its next use |
| `DOTHEROES_PPFX_TARGET_WATCH=<ranges>` | which code touches PPFX's targets in guest memory (ppfx_watch.cpp) |

## 1. The recorder: records from PPFX's own calls

**Where.** The development lift calls `ppu_entry_observe` at the entry of
83 functions (config/dev_capture_observers.json): every libgcm function
PPFX's code (0x007497E0-0x007D3E38) calls, directly or through another
libgcm function, from the lift's static call graph; the flip; and the
game's memcpy (below). r1_observe.cpp passes each call to the recorder
(port/native/render/ppfx_recorder.cpp) before the lifted body runs. The
bodies run unchanged in R3.

**Whose calls.** A call whose return address lies in PPFX's code is PPFX's.
A call made from inside libgcm (cellGcmSetFragmentProgram calling the bind
and the shader control, the vertex program's upload setting its constants)
is PPFX's when the first return address outside libgcm, up the guest
stack's back chain, lies in PPFX's code [C]. Calls by other code on the
same thread set state too and are tracked the same way (below); only
PPFX's draws and colour clears become records.

**From what.** Each entry point PPFX reaches has an encoder that writes
what the call means into the recorder's own state block, from the call's
arguments and the structures they point to (CellGcmSurface,
CellGcmTexture, CgBinaryProgram and its fragment/vertex program
descriptors, CgBinaryParameter, the viewport's scale and offset arrays --
the ninth argument from the caller's parameter save area). The block is
laid out like the RSX register file because that is what the existing
translators, texture and vertex decoders and render-state decode read; it
is written by the encoders alone. The encodings were read off a frame trace
of what each call writes (tools/r3_gt_encodings.py) and are checked at
every PPFX draw by the cross-check below. 46 entry points have an encoder;
the others write no command (address and memory helpers, wrappers whose
nested calls are encoded, the vertex cache invalidate). Every run reports
PPFX calls with commands and no encoder: **0** in every run [M].

**Not from the FIFO or the existing renderer [C].** Nothing in
ppfx_recorder.cpp reads the command ring, the walker's position, the
engine's register file or any of its objects: its inputs are the observed
call's guest registers, guest memory behind its arguments, and its own
state. The bridge (section 3) reads the engine's state in one place only,
`crosscheck()`, a development check whose results are counted and never
written back into a record. Records are published before the call's body
has written its commands at all.

**What a record holds.** A draw (draw arrays) or a colour clear: a
monotonic sequence number, its frame, where its commands go (section 3),
the state block's ranges (754 words), the vertex program's 512 constants,
both programs by content (immutable, shared, keyed by their bytes'
hash), the bytes of the vertex arrays the draw reads, the bytes of small
static textures no record wrote, and what it reads and writes (its colour
target, the units its fragment program samples). A record is
self-contained: the renderer needs nothing from the record before it.

**Snapshots.** Constants and vertex bytes are copied at the call: a later
frame's writes cannot reach a record. Programs are immutable blobs. Static
textures (the 64x64 R8 and 4x4 RGBA8 ones) are copied with their whole mip
chain. The cross-check compares each snapshot with what the engine reads
at the same draw: **vertex bytes, constants, both programs: 0
differences** in every run [M].

**One producer.** Every observed call on another thread than the command
stream's producer (the thread of the first observed call) is counted, and
a PPFX call there aborts the development run [C]: the single-producer
assumption is asserted, not assumed. **0 calls on other threads** in every
run [M].

## 2. Programs: where the bytes come from, by content

**Origin [M].** The game uploads PPFX's fragment programs into RSX local
memory with its own memcpy (guest 0x00963EBC, the main thread) from the
CgBinaryProgram's microcode in main memory (ppfx_watch: the only CPU
writes to the program pages; build/r3/e2/attrib_prog.txt). PPFX then
patches their constants every frame with inline transfers queued in the
command stream (cellGcmSetInlineTransfer, 0x0069FC38), which the walker
applies when it reaches them -- later than the calls.

**The shadow [C].** The recorder keeps program memory as the calls made
it: every inline transfer applied at its call, and every copy the game's
memcpy makes into local memory (observed; pages the shadow keeps) in call
order. At a bind, the program's bytes are the shadow's, with the
CgBinaryProgram's own microcode for words nothing observed has written; the
length is the RSX's (the END bit). So a record's program is exactly what
the GPU will consume at that draw, however far the walker has got.

**Proof [M].** The cross-check reads the fragment program from local
memory at the matched draw -- after every queued patch has landed -- and
compares it with the record's: **0 differences** over every matched draw
of every run, including three returns to the title per run, after which
PPFX reloads its programs at other offsets (0x018F00C0, 0x018F0180, ...).
Before the memcpy was observed, those reloads went wrong (4-12 words of
another program's old patches kept: 2,173 draws in one run); the fix is
what this check found.

**Identity.** Programs are keyed by content: the native pipeline cache's
key holds the vertex program's bytes and the fragment program's structural
hash (its code; the constants PPFX patches each frame go to the constant
buffer, as in the engine), the conversion parameters and the render state.
Code rewritten at the same offset (pass 11's variant) is another program;
the same program at another offset is the same one.

## 3. Ordering: the temporary bridge

port/native/render/ppfx_bridge.cpp, on the FIFO walker's thread, through
the engine's hooks (ps3recomp 0108: a draw about to be recorded; a draw or
colour clear written). It exists only while other work still renders
through the FIFO; it is a file of its own, and the queue's `walked` index
goes with it.

**Identity, not a ring offset.** A record names the title's ring lap and
the IO offset of its first command, taken at the call. The lap is the
runtime's own count of the jumps back to the ring's start it writes for
the title (`cellGcm_title_laps_pub`, counted on the title's thread where
each jump is written); the walker counts the back jumps it takes. The pair
(lap, offset) does not repeat. The walker takes a draw at its END, 0x20
bytes after its first command, and a clear at its method. When a call's
own reserve wraps the ring, its commands start the next lap: the record
names that position too (lap + 1 at the ring's start), and a draw whose
commands end exactly at the ring's end is reported by the walker at the
next lap's first byte, where no command of that lap can end.

**Matching [C].** PPFX's records are a subsequence of the walker's events,
in the same order: the head record either is the walker's event, lies
ahead of it (the event is other work's: its targets are marked dirty), or
lies behind it (the engine never drew it: counted as missed). The walker's
and the title's lap counts differ by a constant, fixed at the first match
and checked at every later one.

**Results [M].** Every record of every run matched, **0 missed** after one
fix (a draw ending exactly at the ring's end, found in the exploration run,
the third position above); laps' difference 0 and constant throughout;
wraps inside a call matched at the next lap's start 0-19 times a run;
commands ending at the ring's end 0-3 times a run. See the results.

**Delayed consumer [M].** With the native renderer slowed by 5 ms a record
(DOTHEROES_PPFX_DELAY_MS=5) the queue fills, the game's thread waits for
room (counted), and nothing is dropped (dl3, river, 100 s): 10,880 waits
for room, 58 s of them; 0 records missed; 564 handoffs, none dropped (with
every slot busy the walker submits the engine's work and waits for one);
4,005 of 4,009 compared outputs identical, the 4 others in the first-load
frames; comparisons without a free buffer skipped and counted (3,532).
Before the walker waited for slots, the same test dropped 213 handoffs
(dl2) -- which is why it waits.

## 4. Isolation and the shared boundaries

**Own images [C].** Every surface address PPFX renders to gets a native
image (the renderer's target registry); a native image is never given to
the engine, and the engine's images are never bound by the native
renderer. The only path between them is the bridge's copy, one way: the
engine's surface (source) into a native slot (destination), recorded in
the engine's own command stream (`rsx_vk_bridge_copy_image`), and from the
slot into the native image on the native queue. The engine's outputs for
the comparison go into native buffers the same way. Nothing goes back.

**What PPFX needs from other work.** The scene target as asp leaves it
(passes 3 and 11 sample it; pass 11 writes it again) and, at an address's
first use, the engine's surface as it was created there. The walker marks
an address dirty at every draw or clear of other work into it, and at a
PPFX record that reads or writes a dirty address -- or one no record has
written -- copies the engine's surface into a slot right there in the
stream: after every earlier write, before the PPFX draw.

**Why the boundaries stay comparable.** PPFX initialises the scene target
(pass 1), asp renders into it, and PPFX resumes from it. The handoff at the
resume carries asp's work over the *engine's* pass 1, not the native one.
That is the same input exactly when the two pass-1 outputs are identical,
because asp's rendering is a function of what it draws over -- and pass 1's
output is compared: identical in every compared frame outside the
first-load frames [M]. The handoff is therefore equivalent input wherever
the comparison holds, and is flagged by the comparison where it does not.

**Controls [M].** Without handoffs (DOTHEROES_PPFX_NO_HANDOFF=1, nh1,
river): 12,291 of 12,691 compared outputs differ and no final composite
matches (0 of 3,634) -- what the comparison sees when the native side lacks
asp's work; the 400 that still match are the passes that read nothing
asp drew. The cross-check finds the constants, both programs and the
vertex bytes of every record equal to what the engine reads at the same
draw (0 differences over 5.9 million draws).

> **Corrected (docs/NATIVE_R3B.md section 1).** Of the two items below,
> the colour mask was not PPFX's state at all: it was the existing
> renderer feeding PPFX's report copy, a memory-to-memory transfer on FIFO
> subchannel 1, into the 3D engine, whose colour mask took the transfer's
> FORMAT word (fixed: GCM_SUBCH1_2D=1). Unit 0's address mode is asp's
> and the recorder now follows it through asp's state API. The first-load
> frames match.

**Inherited state (a finding) [M].** In the first ~20 frames after an area
loads (the castle and dungeon entries, after each CONTINUE), 49 PPFX draws
use state that PPFX did not set and that came from other code: texture
unit 0's address mode in 3 draws, and the colour mask (G and B only) in one
whole frame of 46 draws. The values were written by other code copying
prebuilt command blocks into the ring with memcpy (lr 0x006D0C20) and by
asp's own command writers (0x006C37A0) -- not by libgcm calls (frame trace,
build/r3/c2). A recorder of calls cannot see them, so the native renderer
draws those 49 draws with PPFX's own state, and they differ from the
reference; nothing after them does (no PPFX output is read in a later
frame). This is the only difference found.

## 5. Lifetime and synchronisation

| Stage | What | Where |
|---|---|---|
| CPU publication | a record is complete before `put` moves past it (release); the walker and the renderer read it after an acquire | recorder |
| Command consumption | the walker reaches the record's draw, records its copies, and moves `walked` past it; the renderer takes only walked records | bridge |
| Engine's copies | ordered after every earlier write and before every later one in the engine's stream; a binary semaphore attached to the engine command buffer that holds them, signalled by its submission -- one per command buffer, shared by every record whose copies went into it | backend (0108) |
| Native submission | waits (CPU) until the engine has submitted that signal (`rsx_vk_bridge_submitted`), then submits with a wait on it, once, before the first record that needs it; the backend's queue lock around every native submit | renderer |
| GPU completion | each native batch's fence: then its slots go back to the bridge, its comparisons are queued, its readback buffers return to the pool | renderer |
| Image ownership | a slot belongs to the walker until it records the copy, to the renderer from the record until its batch completes; a native image to the renderer alone | both |
| Destruction | an image replaced (another size or format at an address) is retired and destroyed once no batch in flight can use it | renderer |

Command pools: the renderer's own, used by its thread only; the backend's
are untouched. Queues: a second queue of the graphics family (the backend
asks for it with DOTHEROES_PPFX_NATIVE=1), each submit under the backend's
queue lock (its device idle waits need every queue). Bounds: the record
queue is 1,024 records (the game's thread waits when it is full: counted,
**0 waits** in every normal run); slots 128 images, 512 buffers and 768 MB
(32 kept for final composites), 64 semaphores; four batches of 16 MB uploads and 1,024
descriptor sets; the comparison queue 512 jobs. Every one of these is
counted when it binds.

**A race found [M].** Two runs of about ten crashed at start-up (an access
violation reading 0x100): the native thread asked for the device as soon
as the backend had created it, and one time in several the backend had not
yet fetched its queues, so the first native submit went to a null queue.
The bridge now reports the device only after the queues are fetched
(0109); no crash since, in some twenty runs.

**Validation [M].** The Khronos validation layer with synchronisation
validation over dual runs (two queues, the cross-queue semaphores, the
copies in both streams, the native batches): **0 errors** -- vv1 (river,
100 s, 37,311 native draws, 9,564 comparisons) and vv2 (the same, with the
shader-access heuristic: 35,714 native draws). The only messages are R2's
ten warnings (four programs write a second colour output with one
attachment bound, unused). Negative control (hz2): two copies into one
image with no barrier between them, injected three times, reported as
three SYNC-HAZARD-WRITE-AFTER-WRITE errors. (A first control that only
dropped a barrier between render passes went unreported: the layer does
not follow sampled-image reads between passes without the heuristic; vv2
covers that case with it on.)

## 6. Coverage

| What | How it was reached | Result |
|---|---|---|
| River combat | river_fight | section "Results" |
| Exploration | field_after_dungeon (the route R2 captured) | section "Results" |
| Field 14 | fields_beyond, from the owner's save in field 14 (checkpoint field-area14): an area R2 never covered | section "Results" |
| The first dungeon | dungeon1_approach: its 49-draw chain | section "Results" |
| First-load chains (38, 49 and 51 draws, two 1x1 passes) | every area entry; compared in full by the structure trigger (every draw of the frame that departs from the frame before, and of the next 60) | the only differences: the inherited state of section 4 |
| The dungeon's first seconds | the entry's first-load chain and the 60 frames after it, triggered by the chain's own change -- no timed capture | identical but for the first-load frames |
| Return to the title and CONTINUE | dungeon1_approach with RUNS=3 and RUNS=10: each cycle ends at the title and continues | every cycle's frames identical but for its first-load frames |
| Programs reloaded at other offsets | after each CONTINUE (0x018F00C0, 0x018F0180, 0x018F0280, 0x018F0480, 0x018F0B80, 0x018F0DC0, ...) | the bytes exact (the cross-check) once the game's memcpy is observed |
| Wraps | natural ring wraps (laps 11 to 1,400+ a run); a reserve that wraps; commands ending at the ring's end | all matched |
| Delayed consumer | DOTHEROES_PPFX_DELAY_MS=5 | section 3 |
| Resource reuse | the record queue's 1,024 slots (5.9 million records through them), four batches (176,403 submissions in st1), the bridge's slots every frame; images replaced at an address: none in these scenes (each address keeps one size and format), so forced -- every native image retired every 60 frames (rl1, river, 100 s) | rl1: 2,199 images replaced while batches were in flight, live images 76 throughout, memory flat; 12,808 of 12,843 outputs identical, the 35 others all in frames with inherited state, 0 elsewhere |
| CPU reads of PPFX's targets | ppfx_watch over every PPFX target and the program pages (43 ranges), the river (tw3) and the dungeon with a title cycle (tw2) | below |
| One producer | every run | 0 calls on other threads |

> **Corrected (docs/NATIVE_R3B.md section 4).** The "SPU job" read below
> was the runtime's own diagnostic probe for SPU image 3, which follows
> every address-like word of the task's argument block: here a float,
> -116.0 (0xC2E80000), read as an address in RSX local memory. No SPU
> program and no game code read a PPFX target; the probe is off now
> (ps3recomp 0111), and the conclusion below ("a difference from the PS3")
> was wrong.

**Who touches PPFX's targets in guest memory** [M, tw3 and tw2; every page
closed, each access counted by instruction, thread and time, the symbols
from the build's PDB]: the game's clearing routine (func_009731A8, the main
thread) writes every target page once at each area load; the game's memcpy
(func_00963EBC) uploads programs into the program pages; the walker writes
the program pages (applying PPFX's queued patches) and the existing
renderer reads them (its program translation); the recorder reads them (a
development tool). **No game code reads a render target**, with one
exception: an SPU job (spu_async_run) read one page of the 640x360
intermediate at 0x02DD0000 once, 17 s into the river fight, never again.
Neither renderer writes anything back to guest memory, so that read sees
the game's own cleared memory with or without R3; on a PS3 it would have
seen PPFX's output. A one-off, so no frame depends on it -- but a
difference from the PS3 that predates R3, worth a look when the SPU jobs
go native.

**The attract mode** is not coverage of those areas' rendering: its demos
are movies (NATIVE_R2.md; corrected in HANDOFF.md, tests/scenarios/
README.md and attract.drive). The owner's manual sessions in the first
dungeon and against its boss (2026-11-02/03) stay recorded as they were:
gameplay coverage of the existing renderer, not of R3's native path.

## 7. Measurements

Correctness and cost are measured in different runs. A dual run renders
PPFX twice and reads outputs back: **its frame rate is not R4's**.

> **Corrected (docs/NATIVE_R3B.md section 3).** The per-frame figures in
> this section divide by every flip since start-up, about 2,200 of them
> before PPFX's first frame. Per frame with PPFX records (6,655 in ov2): the
> recorder 0.33 ms, the bridge 0.10 ms, the native thread 0.61 ms, the
> native GPU 1.69 ms.

**The native side's own cost** (ov2: river, 150 s, 8,863 frames, the
native renderer on, comparisons and cross-check off) [M]:

| Where | What | Per frame |
|---|---|---|
| the game's thread | the recorder: every observed call (PPFX's ~3,250 a frame, other code's, the memcpy filter), encoders, records, publication | 0.245 ms (2,168 ms in all) |
| the game's thread | waiting for room in the record queue | 0 (no wait) |
| the walker's thread | the bridge: matching, dirty tracking, recording the handoff copies | 0.078 ms (691 ms) |
| the native thread | turning records into Vulkan work (derivation, pipelines by key, uploads, descriptors) | 0.46 ms (4,087 ms) |
| the native queue | GPU time of the native batches (full barriers between operations, the handoff and snapshot copies) | 1.27 ms (11,272 ms) |
| the native thread | waiting for the engine's submissions of its signals (idle, not cost) | 1.2 ms (10,636 ms) |

The GPU figure is above R2's executor (0.48 ms for the river's chain with
per-image barriers): the native renderer uses the engine's full barrier
between every operation, and the handoff copies run every frame. The
existing path spends 0.86 ms in the game on the same chain (NATIVE_R2.md).

**What comparison and cross-check add** (the same scene with them on, rv2,
8,853 frames, 20,380 outputs compared) [M]: the walker's bridge 0.29 ms a
frame instead of 0.08 (the cross-check 0.19 of it, the engine's copies of
the compared outputs the rest); the native GPU +0.22 ms a frame (the
readback copies); the comparison thread 1.22 ms a frame of its own; the
native thread's own work unchanged (0.47 ms).

**Frame times** (river fight, 82 s each, dev builds) [M]:

| Run | p50 | p99 | p99.9 | max | frames over 25 ms |
|---|---|---|---|---|---|
| native off (of1) | 16.7 | 17.3 | 18.9 | 21.4 | 0 |
| records and matching only (ck1) | 16.7 | 17.2 | 17.6 | 19.3 | 0 |
| native renderer, no comparison (ov2) | 16.7 | 17.4 | 20.5 | 22.4 | 0 |
| dual run with comparisons (rv2) | 16.7 | 17.5 | 24.7 | 50.1 | 4 |
| dual run with comparisons, before the comparison thread and pools (rv1) | 16.7 | 33.4 | 33.8 | 36.8 | 159 |

The native renderer itself holds 60 (ov2). The comparisons cost frames
when they run on the renderer's thread with a buffer made for each
(rv1); on a thread of their own with pooled buffers in cached memory (rv2)
a few frames over 25 ms remain.

**Memory** [M]: native off 964-1,051 MB at the river; the native renderer
without comparisons 1,144-1,151 MB (its images 102 MB, its batches' upload
buffers 64 MB); with comparisons the reference and readback buffers come
on top (bounded since 2026-11-05 to 768 and 256 MB). Over ten title cycles
(st1, 1,270 s) the working set rose for the first two minutes as those
pools filled and then stayed flat (3,310 -> 3,471 MB from 120 s to 721 s
and to the end, before the bound in bytes); threads 52-58 and handles
850-870 throughout; native images 122 and pipelines 26 from the first
cycle on.

**The off switch** [M]: the production build (no observers, so none of
R3's code is linked; ps3recomp 0108-0109's hooks and locks are in, inert
without DOTHEROES_PPFX_NATIVE) against the preserved checkpoint build
(build/checkpoint/2026-11-04, tag checkpoint-2026-11-04), river fight,
real audio, alternated (build/r3/ab):

| Build | Runs | p50 | p99 | frames over 25 ms | memory at the end |
|---|---|---|---|---|---|
| checkpoint 2026-11-04 | 4 | 16.7 | 17.3, 17.5, 17.4, 17.5 | 1, 0, 1, 1 | 1,233, 968, 1,064, 1,064 MB |
| production with 0108 | 3 | 16.7 | 17.5, 17.3, 17.3 | 0, 1, 1 | 1,231, 1,066, 1,064 MB |
| production with 0108-0109 (final) | 2 | 16.7 | 17.5, 17.5 | 1, 1 | 1,227, 1,064 MB |

No regression: the same distribution in every arm. (One checkpoint run's
fight had no debris physics, which the report marks as an empty fight; its
frame times are listed anyway.)

## 8. Remaining gaps

- **The boss fight**, and anything a boss or a magic attack adds to the
  chain: the owner's manual session is the only route there (no automated
  route opens the special key's chest). tests/scenarios/owner_boss_r3.drive
  runs it on the dual-run build; not played yet.
- **Other areas** (desert, lava dungeon, volcano, forest; dungeons past the
  first): no saves there. The attract mode shows them as movies only.
- **The first-load frames** differ by design of a call recorder (section
  4): PPFX's inherited state comes from command blocks other code copies
  into the ring. R4 decides how to treat them (section 9).
- **Not reached by a dual run:** menus over the game (the system menu, the
  map) with PPFX drawing under them, the dot editor, the album; a 30 Hz or
  144 Hz pacing (all runs at 60). Their chains would show as new variants.
- **CPU reads of PPFX's targets** are answered for the river and the
  dungeon routes (section 6); a game path that reads a target back in
  another area would need the watch run there.
- **The native renderer's barriers** are full barriers between operations
  (as the engine's); R2's executor tracked hazards per image. Its GPU time
  is therefore above the executor's (section 7) -- an R4 matter.

## 9. R4: what it bypasses, and how counters prove it

R4 stops PPFX's command generation: for PPFX's calls, the bodies of the
libgcm functions that write commands do not run; the records (unchanged
from R3) and the native renderer produce PPFX's output, which then has to
reach the existing renderer at three points. Not started: this is the plan
for review.

**The bodies bypassed** (calls a river frame, from the boundary log of
build/r3/gt; direct = from PPFX's code, nested = from a libgcm function PPFX
called):

| Entry point | What it writes | Calls a frame |
|---|---|---|
| 0x0069F19C | vertex data array (1740+i, 1680+i) | 1,504 |
| 0x006A39EC | vertex program constants (1EFC) | 213 direct, 94 nested |
| 0x0069FC38 | inline transfer: the fragment programs' constant patches | 235 nested |
| 0x006A7834, 0x006A7674, 0x006A7744, 0x006A75D8 | texture control 0, filter, address, border colour | 116, 96, 37, 50 |
| 0x006A7BA4, 0x006A7CF8 | vertex cache invalidate, frequency divider | 94, 94 |
| 0x0069F2B8 | texture (offset, format, rect, pitch, remap) | 66 |
| 0x006A674C, 0x0069E7EC | scissor, viewport | 54, 51 |
| 0x0069F944, 0x0069E99C, 0x0069E6E0, 0x006A4E50 | surface, fragment program bind, shader control, vertex program | 47 each (the bind nested) |
| 0x006A56BC, 0x006A1E28 | anti-aliasing control, draw arrays | 47 each |
| 26 state setters (0x006A5CF4 the clear ... 0x006A96E8) | one or two registers each | 1-11 each, ~95 in all |

About 3,250 calls a frame. Not bypassed: the wrappers with no commands of
their own (0x0069F454, 0x0069F814, 0x0069FC2C -- their nested writers are
bypassed one by one), the address and memory helpers (0x0069CAC4 ...
0x0069D240, 0x006A97F8: their return values are used), the flip and the
game's memcpy (not PPFX's). The lifter's entry observer gains a result:
"the body is not wanted" makes the generated function return at once (its
r3 the body's normal result, CELL_OK).

**What must reach the existing renderer instead.** PPFX's pass 1 into the
scene target, before asp draws over it; the scene target as asp leaves it,
back to the native side (R3's handoff); the final composite into the
display buffer, before the HUD draws on it. With PPFX's commands gone, these
points need a mark in the stream: one marker method a point (three a frame,
in place of ~6,900 command headers), written where PPFX's first call of
each segment was, carrying the record's sequence number; the walker, at a
marker, makes the copy in that direction and orders it by semaphore as R3
does. The bridge stays temporary: with asp and the HUD native it goes.

**Counters that prove the bodies no longer execute:**
- per entry point, PPFX-attributed calls whose body ran and whose body was
  skipped (the generated code, both counted): ran = 0 for every bypassed
  one;
- the command words PPFX-attributed calls write a frame (PS3_ATTRIB_SHADOW
  in a frame trace): 0;
- the walker's draws a frame minus 47 (the engine's draw count), its
  decoded methods a frame minus PPFX's share (~6,900 headers in the river),
  and R3's bridge, still matching in a development run, finds 0 of PPFX's
  records in the stream;
- the walker's writes into PPFX's program pages (ppfx_watch: the inline
  transfers it applies today): 0.

> **Answered (docs/NATIVE_R3B.md sections 1, 5 and 9).** Other work does
> take state from PPFX (the colour target, vertex constants), and the
> first-load question no longer needs the owner: the plan is revised there.

**Before R4 can start, two questions:**
1. *Does other code inherit PPFX's state?* The reverse of the finding in
   section 4: if asp's first draws of a frame rely on state PPFX left
   (a texture unit, the surface, the viewport), removing PPFX's commands
   changes asp. A frame trace with writers (PS3_ATTRIB_SHADOW) answers it:
   for each draw of other work, the last writer of each register its
   derivation reads (the same effective set as R3's cross-check). Any that
   is PPFX's must be kept (a register write, not a command stream) or
   set by that code itself.
2. *The first-load frames.* There PPFX draws with state other code left (49
   draws after each load). A native PPFX that sets its own state renders
   those frames slightly differently (one frame's passes writing all four
   channels instead of the inherited G and B only; three draws clamping
   instead of the inherited wrap).
   Matching the original would need the inherited values from the FIFO
   path; not matching is a visible change of one frame after a load. The
   owner's call.

**Expected savings**, from earlier measurements, not from dual run:
PPFX's ~16% of the command stream's headers, their decode and register
state on the walker, and its 47 draws a frame through the engine
(NATIVE_EVIDENCE.md, NATIVE_R2.md: the existing path's chain 0.84-0.86 ms
of GPU in the river against the executor's 0.48 ms). The native side's own
cost is measured in section 7; R4 measures the difference in the game.

## Evidence kept

Local only (game-derived: frame data, the game's memory), under build/r3/,
outside the test runs' pruning: per run `<tag>/report.txt` (the recorder's,
bridge's and renderer's counters, the cross-check's and comparisons' detail
per variant), `<tag>/run/` (the run directory: run.json with the build's
hash and every switch, the drive log, frames.log, memory.csv, stderr) and
`<tag>/dump/` (the first differing outputs, both sides, raw).

| Tag | What |
|---|---|
| gt | ground truth for the encodings: boundary log, frame trace with writers, target watch (river) |
| e2 | the program pages' writer (attrib_prog.txt) |
| c2 | frame trace with writers over the first-load frames: who wrote the inherited state |
| rv2, fd2, f14, dg4, st1 | dual runs: river, exploration, field 14, dungeon x3 and x10 with title cycles |
| ov2, ck1, of1 | the native renderer without comparisons; records and matching only; native off |
| nh1, hz2 | negative controls: no handoffs; a hazard for the validation layer |
| vv1, vv2 | the validation layer with synchronisation validation |
| dl3 | the slow consumer |
| tw2, tw3 | the target watch with times: the dungeon with a title cycle, the river |
| dg5 | the dungeon x3 with differences attributed to frames with inherited state |
| rl1 | every native image retired every 60 frames |
| ab/ | the off-switch A/B against the preserved checkpoint build |

## Reproducing

```
tools/r1_dev_lift.sh                       (the development lift with the 83 observers)
tools/r3_scene.sh <tag> river_fight 150    (a dual run into build/r3/<tag>/; NAME=value switches after the length)
tools/r3_scene.sh <tag> dungeon1_approach 900 RUNS=3   (title cycles)
tools/r3_scene.sh <tag> river_fight 100 DOTHEROES_PPFX_NO_HANDOFF=1   (and the other controls, section 3-5)
SCENARIO=owner_boss_r3 build/venv/Scripts/python.exe tests/boot_smoke.py   (the boss, played)
```
