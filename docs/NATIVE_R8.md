# R8: the CPU side -- SPU workloads, physics, scheduling, the game thread

Started 2026-11-06, after the owner accepted R7 (c74a26a, NATIVE_R7.md) as
the graphics architecture checkpoint. R8 is the combined CPU/SPU, physics,
native scheduling and game-thread work: an inventory of the PS3 execution
machinery still running and what of it is on the critical path, then
replacements and optimisations one at a time, each validated before the
next. R7's open items stay open (section 9). Markers as in the earlier
records: [M] measured, [C] checked in code, [I] inferred.

The target: 144 fps sustained on this PC, ideally 200+, uncapped with the
presentation and vblank limits removed; averages, percentiles, spikes and
per-thread work reported apart. An average of 144 is not 144 sustained.

## Current status

| Item | State |
|---|---|
| Inventory: threads, SPU workloads, where the time goes | done (section 1); re-profiled after part 2 (section 8.3): the main PPU thread is still the critical path, alone |
| The accepted baseline; config/insn_patches.json | done: the file is the committed one, byte for byte (section 2); unchanged by part 2 (R8's hooks are config/r8_hooks.json) |
| Frame-counted values | the engine's: none is a clock or moves gameplay (section 3). **Corrected**: the first dungeon's boss does count frames (section 9.6) |
| **The first dungeon's boss above 30 fps** | **fixed**: its head and body segments update at 30 Hz at any frame rate; measured at 30, 60 and 144 fps; in rel8t, validated. **A full kill with the fix is unverified** (the owner will play it). rel8t is a validated development build, **not a stable release**: no tag (section 9.6) |
| **The save-data list** | **a list of the saves, on load and on save** (name, play time, location, when written; ps3recomp 0131); before, the first entry was taken, and the game's SAVE wrote to it whichever save CONTINUE had picked (section 9.6) |
| The shorter uncapped walk | done: the test harness's input timing; fixed (ps3recomp 0117) |
| Unimplemented graphics calls | done: named, on screen, counted, fatal under strict (section 4) |
| The lifted code: -O2 | measured: no gain, not adopted (section 5) |
| The lifted code: the loads' tests | removed in the native release: the game thread 1.1-1.2 ms faster (section 5) |
| **The bare loads' assumptions** | **reviewed, enforced and tested** (`dotheroes --memtest`, 614 checks), defined for any alignment, gated to Windows x86-64; a checked comparison build kept (section 5.3) |
| **The stores** | **priced** (the reservation test 0.4 ms, the range test 0.2 ms: measurement builds); one mov after the tests; **the raw stores' missing reservation test added** (section 5.4) |
| **The two known store gaps** | **fixed in source** (ps3recomp 0125): a misaligned store across a line now tells both lines; the raw stores test the scalar stores' range, so a raw store into the null page is dropped. Standalone test 52/52 in both variants (the old code fails 24); the `--memtest` store groups and the cost are **not yet built, run or measured** (sections 5.4.1, 9.1) |
| **The SPU interpreter and microstepper** | **gone from every build**; any way into them stops the program whatever PS3_STRICT says (section 5.5) |
| **Register promotion** | at every call: measured worse, not adopted; **in functions that call nothing: adopted**, 3.5% of the game thread exploring (section 5.6) |
| The indirect calls | a call-site cache (ps3recomp 0118): +1.5-3% (section 5.1) |
| **The native physics tasks** | **issued and completed directly, without SPURS** (no task, no image hash, no queue, no polling of SPURS), checked and measured: heavy frames 0.2-0.5 ms shorter (section 6.1) |
| The releases | R8's first (rel1/rel2) and part 2's (rel6, then rel7 with leaf promotion; rel8 its final tree) validated: 8 scenarios' pictures, sync validation, capture check, the stops, gameplay timing at 60 and 200 fps, normal damage; the production and player build paths built and run (sections 5.2, 7, 7.1) |
| **The player's build path** | **audited: it built the development configuration** (the old renderer, checked loads and stores, physics through SPURS). Changed to build the native release at all three layers (the lift, the prebuilt runtime, the install-time compile); 11 static checks pass. **Not yet built or run** (sections 7.2, 9.1) |
| Measurement | done (sections 8, 8.2): combat 194 fps at the median (production 148), exploring 235 (production 186) for rel6; 144 sustained exploring (99.8% of frames), not yet in combat (97.8%) |
| **Stopping a batch** | **fixed**: tools/owned_run.py runs a batch in a job object of its own; a cancel, the launching shell's end or the launcher's end stops every process it started and nothing else. 7 dummy-process tests and one stop of a real background task pass; not yet used for a game batch (section 9.2) |
| **The recorder's bookkeeping** | **investigated**: one producer thread (measured: no observed call on another thread in the kept fight reports); the lock is needed only against copies into RSX local memory and inline transfers made on other threads, which the native renderer never takes part in. A lock-free producer path, an indexed dispatch and direct handlers are prepared as three measured steps; two counters added to tell whether other threads write at all (compiled, not run) (section 8.4) |
| **The RPCS3 comparison** | traced to their causes against RPCS3's own target copies at matched cameras (section 9.4): **the sparkle** = the translator ignored NV40's source precision modifiers (ps3recomp 0130; NONNEG, the workaround, removed); **the castle floor** = the depth clamp the game enables, never modelled, which needed the guest's 24-bit depth surfaces to work (both now the default); **the reflection** = R5G6B5 targets held as RGBA8 (now 16-bit; also the king's window-light residual, section 9.5); **the king** = the tested close-up matches RPCS3; why the owner's earlier screenshot pair differed is not established. The edited hero draws (harness fixes, section 9.3) |
| **The editor's lines** | **drawn** (line lists, width 1): the block grid matches RPCS3's frame but for the blinking cursor; a fifth reference scene (section 9.7) |
| **The solver's set-up (commands 1 and 2)** | **native and the default**: byte for byte on seven captured steps and in the game over 180 checked steps (0 differ); the river fight 203.4-209.3 fps against 198.2-200.9, p99 6.57-6.82 ms against 7.24-7.44 (docs/SPU_NON_AUDIO.md section 1) |
| **The store fixes (ps3recomp 0125, 0133)** | **built, validated, free**: memtest 695/695; the line-crossing test folded into the reservation bitmap, rel10b as fast as the play build (section 9.8) |
| **The recorder's step 1** | **done**: no lock on the producer's path, other threads handed over; the fields +2.5% (section 8.4) |
| **The player's install** | **all stages pass** on the newest lift (gen8), the installed game's river fight included (section 9.8) |
| **The inflate and the water** | **native, the default** (rel13): the inflate's direct path replaces each map pack's whole SPURS instance (every pack's CRC as before); the water's step equal to the SPU task's over 3,194 checked steps; river 204.1-208.8 fps against rel11's 202.2-202.3 (docs/SPU_NON_AUDIO.md sections 2-3) |
| What is left | section 9 -- one decision for the owner there (the reservation test) |

## 1. Inventory: what runs, and what is on the critical path [M, C]

Whole-process profiles of the R7 release (build/r8/base = build/r7/rel5,
unchanged), uncapped, real audio: the river fight (build/r8/p1river,
DIAG_PROF 75-115 s, 128 fps while profiled) and the fields (p1fields,
65-105 s, 148 fps). The profiler samples every thread about once a
millisecond, blocked or not, so a thread's samples are its wall time and a
sample in a kernel wait is a wait (tools/r8_threads.py).

**The main PPU thread never waits.** Uncapped, its every sample is work:
it is the critical path, alone.

| The main thread, ms a frame (profiled) | River fight | Fields |
|---|---|---|
| the game's lifted code | 5.49 (70.3%) | 5.47 (80.6%) |
| the recorder (R7's entry hooks and records) | 1.12 (14.3%) | 0.51 (7.5%) |
| ps3_indirect_call (the lifted code's indirect-call dispatch) | 0.40 (5.1%) | 0.46 (6.7%) |
| the runtime's other PPU and memory helpers | 0.10 | 0.09 |
| the test harness's pad file (PAD_FILE, a file opened on every pad read) | 0.08 (1.1%) | 0.04 (0.6%) |
| the whole entry-hook path (everything under ppu_entry_observe) | - | 0.71 (10.5%) |

In the fields, R7's game thread is 0.4-0.5 ms a frame busier than
production's (NATIVE_R7.md section 8): the hook path's 0.7 ms, of which the
generic dispatch (ppfx_rec_observe's own time 1.8%), a lock taken on every
observed call, the recorder's own timing and R4's per-frame decision.

**The other threads** (work, % of the thread's wall time):

| Thread | River | Fields | What |
|---|---|---|---|
| ppfx native (the native renderer) | 59.6 | 45.6 | records consumed, Vulkan work; part of it a timed wait's spin |
| spu job (one of 7) | 9.4 | 0 | the water task (lifted SPU code, every frame) |
| present | 4.3 | 4.6 | vkQueuePresent |
| spu FMOD:0 | 3.3 | 3.7 | the FMOD mixer (lifted SPU code; audio milestone) |
| dev keys, state, stop (test harness) | 13 | 13 | development threads the test runs start; off in play |
| spurs kernel (2), spurs pm (4), 6 other spu job, FMOD PPU threads, vblank, file system | idle | idle | |

No physics ran in either window (no debris): the heavy-debris frames need
their own profile (section 6).

**The PS3 execution machinery still running** (the runtime's cellSpurs and
SPU environment; an agent's read of the code, file and line references kept
in build/r8/inventory_spu.md):

| Workload | How the PPU submits it | Where it runs | Completion | State |
|---|---|---|---|---|
| water (image 1) | a persistent SPURS task, signalled every frame by an event flag | a pool thread (spu job), lifted SPU code, its local store and DMA | none awaited | fully lifted |
| waver (image 2) | registered, never dispatched in any run | - | - | lifted |
| Edge zlib (image 3) | a SPURS task per area load, a lock-free queue push | a pool thread: the native inflate inside the SPU environment (its context and 256 KB local store); the lifted routine if it declines | an event flag set by lifted GETLLAR/PUTLLC; the PPU waits in a 2 ms timed poll | native body, emulated shell |
| Bullet collision (image 4) | a SPURS task per command, a queue per slot (3 slots) | a pool thread, native (DOTHEROES_NATIVE_CD=1), no local store | the task pushes the result queue; **the main thread busy-polls the queues** (non-blocking pops round the slots, in lifted game code) | native, SPURS bookkeeping kept |
| Bullet solver (image 5) | as collision | commands 4-6 native (DOTHEROES_NATIVE_SOLVER=4,5,6); commands 1 (body set-up) and 2 (manifold set-up) lifted | as collision | partly native |
| FMOD (images 6-26) | an lv2 SPU thread group | its own thread, lifted | event queues | audio milestone |

What every native physics task still passes through: CreateTask's
bookkeeping under the lock-line lock, a hash of the task's ELF on the PPU
for every task, the taskset lock's spin, the exit scan, queue operations
under the global lock-line lock (a bare test-and-set spin in this
toolchain's build), the pool's wake-up. The SPURS kernel threads wake every
millisecond with nothing to do. Shared with the audio path, so kept until
the audio milestone: the SPU context, channels, mailboxes, MFC and DMA, the
lock-line lock and reservations, the dispatcher, sys_event, the lifted
registry with FMOD's overlays.

The defaults: a player install (setup_app.py) turns both native physics
switches on; the executable alone runs them lifted. The native inflate is
on unless DOTHEROES_NATIVE_INFLATE=0. The SPU interpreter and the
microstepper are compiled into every build; strict mode is off in a player
package, so an interpreted SPU branch would not stop it -- none has run in
any recorded run ([release-path] lines: none). (Part 2 removed both from
every build: section 5.5.)

## 2. The accepted baseline and config/insn_patches.json [M]

`git status` has shown config/insn_patches.json modified since 2026-11-02.
Its bytes are the committed blob's: the working file hashes to 2caa8b4
(`git hash-object`, with and without filters), 2,866 bytes, LF endings, no
carriage return -- the blob of commit 0d87837 ("The hero walks at 9 units a
second at any frame rate"). The index still holds the stat data of an
earlier checkout with CRLF endings (2,927 bytes: 2,866 and 61 line ends,
cached 2026-11-02 06:46), and git reports a size mismatch as a modification
without reading the content (`git diff` shows nothing). So every lift since
then -- R6's, R7's, R7's release -- read the committed file, and the only
change the file ever had in the working tree was its line endings, which
the lifter's JSON reader does not see. R8 refreshes the index's stat data
(no content change).

R8's baseline is the R7 release as measured, kept unchanged:
build/r8/base (dotheroes.exe sha256 1274d538..., its PDB 1b5f7b0b...,
SDL2.dll 63dbe5be...; build/r8/base/SHA256SUMS), built from repo c74a26a
and ps3recomp 4c22c24 (the 116 patches in patches/ps3recomp), R7's lift of
build/EBOOT.elf with config/insn_patches.json, config/sjlj.json and
config/r7_native_hooks.json.

## 3. Frame-counted values and the uncapped walk [M]

R6 and R7 reported that "the game's frame counters run with the frame
rate" uncapped (x2.63-2.79) and that the hero walked 2.8% shorter
uncapped. Neither was explained.

**Which values follow frames.** tests/fps_check.py compares the static
memory below 0x03000000 between two snapshots 10 s apart, standing still:
seven words change in proportion to the frame rate (R7's batch 4, 60 fps
against uncapped). A hardware watch (DOTHEROES_DEV_HWWATCH, section 10:
the CPU's debug registers on every thread, each access counted by the
instruction and its caller) names every function that reads or writes them
(build/r8/hw1a, 70 s at the castle front: standing, walking, the grass cut):

| Word | A frame | Written by | Read by | What it is |
|---|---|---|---|---|
| 02CF3570 | +1 | func_0094F290 (its increment) | nobody else in the game (only R8's own test logger) | a frame count |
| 02B37CEC | +1 | func_00734A50 | func_0072CB4C, 0072D84C, 0072DA30, 00733A70 | asp's scene-graph change generation |
| 029BA1BC | +3 | func_0072CB4C | func_0072CB4C, 0072D84C, 0072DA30 (~116 reads a frame) | the same, another graph |
| 02B785EC | +3 | func_00734A50 | as 02B37CEC | the same |
| 02B38D0C | +19 | func_00734A50 | as 02B37CEC (~1.7 million reads in 70 s) | the same |
| 02C14C20 | +85 | func_007BC770 (+1 a call) | only its own increment | a call count |
| 02C14C24 | +374 | func_007BC770 (+ a size a call) | only its own increment | a running total of sizes |

(build/r8/hw2 for the last three.) The change generations: func_0072CB4C
marks a node changed and, when the node's stamp (+0xE8) equals the graph's
counter ((+0x30)+0x1C), bumps the counter; func_0072D84C compares a node's
stamp with the counter to decide whether its cached state is still valid.
Only equality is ever tested: the counters advance with the frames because
animated nodes change once a frame. func_007BC770 counts its own calls and
adds up a size argument -- statistics nothing else reads.

**So "the frame counters run x2.66" was a description, not a defect:** of
the seven words that follow the frame rate, one is a frame count no game
code reads, four are change generations compared only for equality, two
are statistics. None is a clock, and none moves gameplay. The time-based
clocks (a seconds float at 029A64CC and its seven mirrors, a millisecond
count at 02C69D50, the 60 Hz count at 02CF3544) run x1.00 at every rate.
The hardware watch covered 70 s of the castle front (standing, walking,
the grass cut); a reader elsewhere in the game would not show there, but
the readers it found are the engine's own, not a mode's.

**What moves at a different rate** (build/r8/t1-*: tests/fps_check.py on
the castle-front drive, twice at 60 fps and twice uncapped at 154-164;
a1-*: the river fight's characters, DOTHEROES_DEV_ACTOR_LOG):

| Observable | 60 fps | Uncapped | |
|---|---|---|---|
| the hero's walk, 1 s held (his own position, 4 ms samples) | 8.99, 9.00, 9.14, 9.00 units in 1000.1-1016.4 ms held | 9.00, 9.03, 9.17, 8.99 in 1000.7-1006.9 ms | 8.99-9.00 units a second held; 8.97-9.11 |
| the camera target's walk (R6/R7's measure) | 4.99, 5.14 | 5.00, 5.17 | equal |
| the enemies' wander legs (river) | 4 units a second (38 of 49 legs), 8 (6) | 4 (32 of 38), 8 (4) | the same discrete speeds |
| the sword's swing state (+0x51C = 4) | 457 ms median (12 swings) | 420 ms (12) | a frame or two of transition at 60 fps (production the same: 449 / 423 ms in R7's runs) |
| the walk cycle (+0x51C 0/1/2), the swing's phases | 101-116 ms, 314 ms | 96-137 ms, 304 ms | within a frame |
| physics in the grass | 360 steps in 6.0 s, 55 bodies | 359-361 steps in 6.0 s, 55 | fixed 60 steps a second |
| the time-based clocks over 10 s standing | x1.00 | x1.00 | |

**The shorter uncapped walk was the test's input, not the game.** A drive's
`hold <button> <s>` presses through the pad file for a span of wall time,
and cellPad measured that span with GetTickCount64, which advances in ~15.6
ms steps on Windows: a 1 s hold was anything from ~984 to ~1016 ms. R7's
runs, measured again by the hero's own position, moved for 977-997 ms and
walked 8.86-9.00 units at the same speed (8.9-9.0 a second) -- at 60 fps
as uncapped. The hold is now timed with the trace's clock (the performance
counter) and recorded (PS3_FT_PAD, and a log line with the reads it
covered): held 1000.1-1016.4 ms at 60 fps (whole frames: 60 or 61) and
1000.7-1006.9 ms uncapped, the hero walks 9.0 units a second held at both.
The camera target's 4.86 against 5.00 (R6, R7) was the same input jitter
seen through the camera's follow. A test-harness fix (libs/input/cellPad.c,
PAD_FILE only); play is untouched.

**What this settles for the benchmark mode:** uncapped, the game plays at
the same speed -- movement, enemies, physics, clocks -- with its actions'
state changes rounded to its own frames, which at 60 fps adds a frame or
two to a swing. The benchmark mode stays a measurement mode for other
reasons (no vblank pacing, a mailbox present); its frame counters are not
one of them.

**Corrected (2026-11-09).** "The game plays at the same speed" held for
what this section measured: the hero, the river's enemies, physics and the
engine's clocks. It was too broad. The first dungeon's boss counts its own
frames and ran up to 4.5 times too fast at 144 fps (section 9.6). The hero and
physics checks above do not reach a boss's own logic; section 9.6 has what
was and was not checked for the others.

## 4. Graphics work nothing executes: a diagnostic [C]

Without a walker, a command word in the title's ring and a call of a
libgcm writer R7 did not replace are lost work. R7 found neither in any
scenario, and named them only in the development report. Now
(port/native/render/ppfx_recorder.cpp, r7_unimplemented):
- each new kind is named on stderr at once: the function (or, for words
  found in the ring, the new method), its caller, the frame;
- the first is shown on screen for 10 s ("GRAPHICS CALL NOT IMPLEMENTED:
  <address>"), the overlay the release keeps;
- a line at exit counts all of them;
- under PS3_STRICT=1 -- development builds and every test run -- the first
  is fatal (ps3_strict_fail "unimplemented-graphics"), with its name.

DOTHEROES_R8_LOST_TEST=<frame> (development) plants one NOP method in the
ring at that frame's flush, as an unreplaced writer would: the
diagnostic's own test.

## 5. The lifted code [M]

**The optimisation level.** Every build so far compiled the lifted PPU code
and its scaffold at -O1 (port/CMakeLists.txt: a level chosen for the huge
files' compile time; the runtime library, the native physics and the SPU
images are -O2). DOTHEROES_PPU_OPT=-O2 compiles them at -O2 (a full build:
13 min). A/B on the same sources (build/r8/r1 at -O1, build/r8/o2x at -O2;
ab1, two rounds, uncapped, the order rotated):

| | River fight fps | p99 ms | game thread busy ms | Fields fps | p99 ms | busy ms |
|---|---|---|---|---|---|---|
| -O1 | 150.2, 155.8 | 11.00, 10.14 | 6.52, 6.25 | 171.8, 171.6 | 7.80, 7.66 | 5.69, 5.65 |
| -O2 | 154.8, 152.6 | 10.15, 9.40 | 6.28, 6.39 | 170.8, 173.3 | 7.80, 7.69 | 5.71, 5.63 |

No difference beyond the runs' spread. The reason is in the code: one of the
hottest functions (func_000A2834, 7% of the exploring game thread) compiles
to the same 3,273 instructions at -O1 and -O2 (and with `__restrict` on its
context: the same). About 15 host instructions per guest instruction: every
guest register lives in the context structure (each written register a
store, each read a load), and every guest access is tested inline -- a
32-bit load against the GCM poll word, every load and store against the
inline range, every store against the SPU reservation bitmap -- with an
out-of-line slow path behind each (103 calls in that function). With the
accessors reduced to a bare load or store and byte swap, the same function
is 1,757 instructions and no calls. Not adopted: -O2 costs build time for
nothing measurable.

**What the tests cost, and the indirect calls** (ab2: the R7 lift at -O1
against two variants on the same sources, two rounds each, uncapped):
- `bare`: the lifted code's loads without the poll-word, range and
  alignment tests (build/r8/genx: the generated header's four load
  accessors reduced to a relaxed atomic load and its byte swap; stores
  unchanged);
- `ic`: R8's lift with the indirect calls' call-site cache (section 5.1).

| | River fps | p99 ms | busy ms | Fields fps | p99 ms | busy ms |
|---|---|---|---|---|---|---|
| R7 lift (r1) | 154.2, 153.7 | 9.39, 9.34 | 6.35, 6.38 | 170.5, 166.5 | 7.85, 8.35 | 5.72, 5.76 |
| bare loads | **185.3, 186.0** | 8.88, 8.20 | **5.22, 5.27** | **212.6, 212.5** | 6.45, 6.52 | **4.55, 4.58** |
| call-site cache | 156.6, 155.7 | 10.12, 9.20 | 6.21, 6.31 | 175.5, 175.7 | 7.82, 7.74 | 5.57, 5.55 |

The loads' tests cost the game thread 1.1-1.2 ms a frame: a fifth of it.
Not the tests' few instructions alone -- each load carries an out-of-line
slow path, and the hot code is about twice the size with them (the
function above: 3,273 instructions against 1,757).

**Why the tests can go in a native release** [C]: on Windows the guest's
whole 4 GB is reserved at start and a page is committed, zero-filled, the
first time anything touches it (port/main.cpp, alloc_guest_vm and its
vectored handler; ppu_vm_size 0: "no OOB guard needed"). So a load the range
test would have sent to the slow path -- the null page, an address past
what the game mapped -- reads 0 there, as the slow path answered. What the
slow path did besides: report a null-page read (on by default, capped; none
in any scenario's run: build/r8/{p1,ab2,t1}*, build/r7/{cc1,i1}*), emulate
the raw SPU windows' registers (no raw SPU in this game), and the GCM
poll word's read-driven reference publication, which the native front end
turns off (cellGcm_ref_on_poll returns at once: the host writes the
reference itself). So a native release build compiles the lifted loads bare
(PPU_BARE_LOADS, the generated header; port/CMakeLists.txt defines it for
DOTHEROES_NATIVE_RELEASE, DOTHEROES_PPU_CHECKED_LOADS=ON keeps the tests).
Development builds keep them, and with them the read diagnostics (the null
read report, PPU_RWATCH and the like), which the release's lifted loads no
longer pass through. Stores keep their tests: the SPU reservation bitmap
must see every store while lifted SPU code (FMOD, the water task, solver
commands 1-2, the inflate's shell) can hold a reservation. Part 2 reviewed
these assumptions one by one and tests them (section 5.3).

### 5.1 The indirect calls' call-site cache

Every bctrl and bcctrl (10,634 sites in the lift) called ps3_indirect_call:
a hash lookup (37% of its own time) and a host call through one shared
instruction for every target. R8's lifter emits PPU_ICALL at those sites
(tools/ppu_lifter.py; the runtime exports the lookup and the dispatch's
thread-local state): a site keeps its last target and that target's host
function in one 64-bit word and calls it directly when the target repeats,
doing what the dispatcher's fast path did around the call (the active
context, the depth and its cap, the vcall stack, the trampolines);
anything else goes through ps3_indirect_call. Measured: +1.5% in the river
(busy 6.21-6.31 ms against 6.35-6.38), +3% in the fields (5.55-5.57 against
5.72-5.76). Small, and kept: it costs nothing where it misses.

### 5.2 The R8 release, validated [M]

build/r8/rel1 (dotheroes.exe sha256 4f219165...; build/r8/rel1/SHA256SUMS):
R8's lift (tools/r8_lift.sh: the call-site cache) built with
tools/r8_release_build.sh (-O1, PPU_BARE_LOADS). Its PDB has 0 command-path
functions (tools/r7_symbols.py), as R7's.

- **The eight scenarios** (build/r8/v1*: river, menus, fields, dungeon,
  snapshot, the field after the dungeon, the editor, the exit after the
  boss; window grabs every 5 s): every drive done or run to its time, no
  crash, hang, strict failure or lost graphics work. Grab for grab against
  R7's release runs (build/r7/r1*, tools/r7_grabs.py): the same scenes, the
  same brightness and coverage, differences where things move (0.0-3.2 on
  the editor's and the snapshot's static screens); dark grabs 4-8 a run in
  both (loads and fades), one of them at a different second in the dungeon
  -- R8's run 2 s longer, its last grab in the final load's fade, both
  drives ending in the same place.
- **Synchronisation validation** (vval1river): 0 errors, the same 10
  warnings as R7's runs.
- **The capture check** (vcc1river): 395,583 records, 0 inputs written
  between the flush and the capture.
- **Lost graphics work** (lost3, DOTHEROES_R8_LOST_TEST=600, strict off):
  the stderr line, and the notice on screen for its 10 s in the window
  grabs ("GRAPHICS CALL NOT IMPLEMENTED: ..."); lost2 with strict on: the
  named fatal failure. (The exit summary printed at every periodic report
  write in rel1; it now prints once, at exit or a test's stop.)
- **Gameplay timing** (t2-60, t2-bch: the castle-front timing drive): at 60
  fps the walk 9.00 and 8.99 units in 1000.6 and 1000.3 ms held, physics 360
  steps in 6.0 s with 55 bodies, the clocks x1.00, the swing 448 ms (R7 at
  60: 457); uncapped at **200-206 fps** (179 in the grass cut) the walk 9.02
  and 9.11 in 1002.8 and 1001.6 ms held (8.99-9.09 units a second), physics
  60 steps a second with 55 bodies, the time-based clocks x1.00, the swing
  417 ms.
- **Normal damage** (dmg-*, no can't-die, the river fight): see section 7.

### 5.3 The bare loads' assumptions, reviewed and tested [C, M]

Before anything more is taken off the guest accesses, a review of what the
bare loads rely on, each assumption with the place that holds it and a test
(`dotheroes --memtest`, port/native/memtest.cpp: run before the game is
loaded, in a guest space of its own, reserved and backed exactly as a game
run's; it reads every case through both forms of the lifted code's loads --
checked and bare, each compiled from the lift's own ppu_recomp.h -- and
compares each with what the bytes are).

| Assumption | Why it holds | Where it is enforced | Tested |
|---|---|---|---|
| A load of the null page reads what is there, as the checked slow path did | both forms read memory there; the runtime drops a guest's stores below 0x1000 (vm_null_store), so it stays zero | -- (the same value; the checked form's `[null-read]` report, the first 16, is what the release does not have) | untouched and written, every size, across 0x10000 |
| Any size at any alignment | x86-64 loads 1-8 bytes at any address; the accessor is a volatile load through an alignment-1 type: defined for a misaligned address (rel1's atomic builtin was not: undefined for one, though it compiled to the same mov), one instruction (LLVM does not split or merge a volatile access of a legal width), never hoisted out of a spin or dropped | ppu_recomp.h: `#error` unless x86-64 and clang or GCC | every size at offsets 0-23; a spin on each size sees another thread's store |
| An access over a page or commit-unit boundary | the guest space and a 64 KB guard are one reservation; the vectored handler commits each 64 KB unit on first touch, the second unit of a split access too (the CPU reports the page that faulted) | port/main.cpp's start check: one reservation from the base through the guard, the guard reads zero; ppu_mm_publish: ppu_vm_size 0 | across 4 KB pages; across 64 KB units with neither, the first or the second side committed beforehand (the state checked first, so the fault happens inside the access), 198 checks |
| An access across 4 GB | the 32-bit address plus at most 7 bytes stays inside the guard, which reads zero | as above | every size ending past 4 GB, untouched and written |
| Bits above 32 | both forms take the address modulo 4 GB | -- | three high patterns, every size |
| The GCM words | the native release has no command path: cellGcm_ref_on_poll is compiled empty; get = put (cellGcm_get_on_poll) only ever ran for out-of-line reads, in either form | the bare loads are defined only in a native release build | both words, both forms equal |
| A raw SPU's problem state (a mailbox read pops) | this game creates no raw SPU | sys_raw_spu_create is fatal in a build with bare loads | the window read through both forms, none created |
| A read diagnostic (PPU_RWATCH, PPU_FORCE_READ_ADDR and the rest) | the checked form turned every load out of line while one was set | fatal at the guest's start in a build with bare loads: run a checked build | -- |
| The base is never NULL | the model is published before the first lifted function runs (ppu_run); every other way into guest code starts later | a guest call (ppu_guest_call, ppu_guest_call_ct) before the publication is fatal in a build with bare loads | -- |
| Concurrency | an aligned load of 1-8 bytes is single-copy atomic on x86-64, as on Cell; a misaligned one is not, on either, nor was the checked form's memcpy | -- | 20 million aligned loads of each size against a writer flipping the word: none torn; misaligned inside a cache line none, across one 50-60% (reported, not required) |

**The platform gate.** CMake defines PPU_BARE_LOADS only for a native release
on Windows x86-64; anywhere else the loads stay checked, with a configure
message. port/main.cpp refuses to compile the bare loads off Windows: the
Linux build maps the guest space another way (an mmap with MAP_NORESERVE, no
handler; an untouched page reads zero through the kernel's zero page), which
is plausible but not tested here. The checked comparison build is kept:
build/r8/chk3 (DOTHEROES_PPU_CHECKED_LOADS=ON).

**Results.** rel3 (bare) and chk3 (checked): 614 checks each, 0 failed
(build/r8/rel3/memtest.txt, build/r8/chk3/memtest.txt). The volatile
alignment-1 form compiles the hot function of section 5 to 2,574
instructions, against 2,630 for rel1's atomic form (the byte-swapped loads
fold into movbe).

### 5.4 The stores [C, M]

**What a lifted guest store does, and which of it is required.** The inline
test (ppu_recomp.h's vm_mm_st_ok) sends a store out of line when:
- its address is outside [0x10000, 0xE0000000) -- out of line, a store into
  the first 4 KB is **dropped** (hardware would fault; vm_null_store), one
  into a raw SPU's problem state has its **side effect** (none here);
- a store diagnostic is armed (PPU_WVAL, PPU_WWATCH, PT, SPU_ATOM_EA,
  PS3_NULL_SWEEP, PS3_ATTRIB_ST: the span is 0 and every store goes out of
  line, where the **memory watches** see it);
- the line's bit is set in the **SPU reservation bitmap**: out of line, the
  store takes the lock-line lock, writes, and breaks the SPU's reservation
  (spu_coh_notify_write), so that SPU's PUTLLC fails as on hardware. Lifted
  SPU code takes reservations (FMOD, the water task, the inflate's shell,
  solver commands 1-2, SPURS's queues and event flags), so the test is
  required for as long as any of it runs.
All three are required behaviour and stay. What is not: after the tests, an
aligned store was an atomic store and a misaligned one a memcpy -- a branch
on every store whose two arms are the same single mov on x86-64.

**One change of meaning-free code, two measurements** (DOTHEROES_PPU_STORES,
ps3recomp 0120):
- `mov` -- the same tests, then one volatile alignment-1 store (as the
  loads): no change of meaning, a candidate;
- `nocoh-measure` -- without the reservation test: a change of
  synchronisation, wrong while any SPU code can hold a reservation on a line
  the PPU writes; built only to price the test;
- `bare-measure` -- without any test: prices the range test and the
  diagnostics' span with it.
The two measurement builds refuse to start unless DOTHEROES_MEASURE_ONLY=1
(`[fatal] measurement-build: ...`, build/r8/smoke-stX-nomeasure): they cannot
be mistaken for a release.

**Found in the review: stores that skipped the reservation test.** The raw
stores -- vector stores (stvx: 737 sites in the lift, stvebx/stvehx/stvewx,
stvlx/stvrx), the byte-reversed stores (stwbrx, sthbrx, stdbrx) and dcbz (a
128-byte memset) -- wrote guest memory directly in every build, without the
reservation test: an SPU holding a reservation on such a line would have its
PUTLLC succeed over the PPU's write. (Also: a misaligned scalar store across
a 128-byte line tests only its first line.) Fixed, as a change of its own
and measured as one (below): each raw store tests the bitmap for its line or
lines and, reserved, writes under the lock-line lock and breaks the
reservation (vm_write_block_coh, vm_zero_block_coh in ppu_loader.cpp);
PPU_RAW_ST_NO_COH keeps the old form for the measurement. In this game's
lift that is the 737 stvx sites and two byte-reversed stores (no dcbz, no
stvlx/stvrx). Measured twice: vc1 (rel5, the first form -- three tests a
stvx through a global the compiler reloads -- against rel5x, the same build
without the test) put it at 0.13 ms exploring (busy 4.31-4.37 against
4.20-4.22); after stvx was made one test through the function's snapshot
(rel6, ps3recomp 0122), vc2 ran the three builds side by side: fields busy
rel6 4.25-4.28, rel5 4.24-4.28, rel5x 4.21-4.29 ms; the fight's the same
within its debris. **No cost beyond the runs' spread**: rel5 and rel5x,
the same binaries in both batches, were 0.13 ms apart in vc1 and 0.00-0.03
in vc2. Two rounds an arm do not resolve a tenth of a millisecond; the
differences this section reports beyond that size (the reservation test's
0.4 ms, the range test's 0.2 ms) stand.

**Measured** (build/r8/ab3_run.sh: each arm on the lift of build/r8/gen2 --
rel2, the R8 release as measured, on gen's; uncapped, real audio, two rounds,
the order rotated; tools/r8_bench.py):

| Fields (exploring) | fps | p99 ms | frames over 5 ms | game thread busy ms |
|---|---|---|---|---|
| rel2 (R8's first release: atomic bare loads) | 229.2-229.8 | 5.79-6.00 | 4.5-5.2% | 4.32-4.33 |
| rel3 (volatile alignment-1 bare loads; stores as before) | 231.8-233.6 | 5.80-5.94 | 4.1-4.8% | 4.25-4.28 |
| stA (stores: the tests, then one mov) | 234.0-235.1 | 5.72-5.95 | 3.8-4.2% | 4.22-4.24 |
| stX, measurement only: no reservation test | 258.7-260.0 | 5.20-5.37 | 1.4-1.6% | 3.82-3.83 |
| stB, measurement only: no store test | 273.7-274.1 | 4.91-4.97 | 0.9-1.0% | 3.62 |

| River fight (combat) | fps | p99 ms | game thread busy ms |
|---|---|---|---|
| rel2 | 192.5-197.9 | 7.14-7.97 | 4.99-5.13 (neither round had heavy debris) |
| rel3 | 189.5-190.6 | 7.62-7.79 | 5.18-5.21 (both had) |
| stA | 195.0-200.9 | 6.95-7.51 | 4.91-5.06 |
| stX, measurement only | 214.3-216.9 | 6.92-6.96 | 4.55-4.61 |
| stB, measurement only | 221.2-224.2 | 6.60-6.81 | 4.40-4.46 |

(The fight's debris differs from run to run -- a run with none is faster --
so the fields are the cleaner comparison of code.)

- **The defined bare loads** cost nothing measurable (rel3 against rel2:
  busy 0.05 ms less exploring -- within what two rounds resolve).
- **The single-mov stores**: no change of meaning, nothing measurable lost
  (stA against rel3: 0.03 ms less exploring, within the spread; the fight
  confounded by its debris): **adopted** for the simpler code (one store
  where there were two arms of a branch), not for a measured gain
  (DOTHEROES_PPU_STORES=mov, the release's default on Windows x86-64).
- **The SPU reservation test costs 0.4 ms of the game thread** (busy 4.23 to
  3.83 exploring, ~4.98 to 4.58 in the fight: a tenth of it); the range test
  and the diagnostics' span 0.2 ms more. These are prices, not candidates:
  the reservation test is required while any SPU code takes reservations on
  lines the PPU writes. It can go only with a different synchronisation
  design (section 9: a tradeoff for the owner).

### 5.4.1 The two known store gaps, closed (2026-11-07) [C]

Reviewed and fixed in source, with focused tests (ps3recomp 0125). The
standalone test ran; the in-game test is built into `--memtest` but **has
not run**, and nothing is measured (section 9.1). Both gaps were found in
part 2's review and left (section 9).

**A misaligned store across a 128-byte line.** Two places tested and told
only the store's first line:
- the inline test (vm_mm_st_ok);
- the out-of-line commit (ppu_loader.cpp's VM_WRITE_COH: one
  spu_coh_line_reserved, one spu_coh_notify_write).

So a 2-, 4- or 8-byte store that ran past a line's end into a line an SPU
held a reservation on left that reservation standing, and that SPU's
PUTLLC could succeed over the PPU's write. The same went for the line
watches: spu_coh_watch, the host waits the bitmap also drives.

Fixed:
- The inline test sends a store whose bytes run past its line
  (`(ea & 127) > 128 - n`) out of line. No naturally aligned store
  crosses, and for a byte store the compiler drops the test.
- VM_WRITE_COH tests both lines and tells both.
- The raw stores already tested both lines (vm_coh_reserved) and told
  every line (vm_write_block_coh).

The new inline test is an and, a compare and a branch on each 2-, 4- and
8-byte store. Its cost is **not yet measured**. If it shows, the lifter can
leave it out where it proves the address aligned, such as stack slots off
r1 with aligned offsets.

**A raw store into the first page.** The raw stores wrote wherever they
pointed and never tested the scalar stores' range: the byte-reversed
stores, stvx, the element stores, stvlx/stvrx and dcbz. So:
- a raw store into the first 4 KB landed where a scalar store is dropped
  and reported (vm_null_store);
- a store diagnostic's span of 0 did not stop them, and a write watch never
  saw a vector store;
- a raw store past the mapping's end was not turned away.

Fixed: every raw store tests the inline store span, through the lifted
function's snapshot for stvx and g_ppu_mm for the rest, and goes out of
line otherwise. vm_raw_store_slow and vm_raw_zero_slow then do what the
scalar stores' out-of-line path does:
- the write and barrier watches (a word at a time);
- the mapping's end;
- a store starting in the first 4 KB dropped and reported, with the
  null-page sweep's containment;
- the write, under the reservation protocol on every line it touches;
- a word store to a raw SPU register reaching the register.

PPU_STORES_BARE_MEASURE leaves the new range test out, as it does the
scalar stores' tests.

**Tests.**
- **tests/native/store_paths.py**: the generated header's store helpers,
  compiled with a small test (tests/native/store_paths_test.cpp) once per
  store variant and run against a 512 KB test guest space. The out-of-line
  functions are stubs that record their calls. It has 52 checks:
  - every 2-, 4- and 8-byte store at the last offsets of a line;
  - a byte store at offset 127;
  - a reserved first line, a reserved second line, and a store ending
    exactly at a reserved line;
  - the range at 0, in the first page, across 0x10000, at 0x10000, at the
    space's end and past it, and with a diagnostic armed;
  - each raw store kind in and out of the range, reserved and not.

  Result: **checked 52/52, mov 52/52**. The same test on the lifter before
  the fix fails 24 checks in each variant: every crossing store, and every
  raw store in the first page, across 0x10000, past the end or under a
  diagnostic.
- **`dotheroes --memtest`**, two new groups (memtest_stores.cpp, the
  build's own store variant, the real runtime). **Built into the next
  build, not yet run.**
  - **stores: the first pages**: each of eleven store kinds into the null
    page (must not land, even a dcbz over nonzero bytes), at 0x4000 (lands,
    out of line) and in the inline span (lands).
  - **stores: line boundaries**: a line watch (spu_coh_watch) counts the
    stores that reach the next line. It is told exactly when a store runs
    into it: 21 cases over the kinds and offsets, with the bytes checked.
    With both lines watched, a crossing store tells both.

  Compile-checked against the new header in both variants, as was the
  changed ppu_loader.cpp.
- ps3recomp's lifter tests: test_lifter_header_name,
  test_ppu_lifter_insn_patches and test_ppu_lifter_chunks pass.
  test_ppu_lift fails the same way with and without this change: it
  compiles its conformance file with MSVC, which has no
  `__builtin_expect`.

**What changes for this game** [I, from the lift's counts]: its lift has
737 stvx sites and two byte-reversed stores, and no dcbz or stvlx/stvrx
(section 5.4). Whether any store of it crosses a line or touches the first
page in play is not known. The fixes make what such a store does correct,
not observed.

### 5.5 No SPU interpreter, no microstepper [C, M]

Every build of the port leaves out the SPU interpreter (spu_interp.c) and the
self-modifying-code microstepper (spu_channels.c's spu_smc_microstep):
ps3recomp's PS3RECOMP_SPU_INTERPRETER=OFF, which port/CMakeLists.txt sets
(DOTHEROES_SPU_INTERPRETER, default OFF; CMake refuses it for a player
package, DOTHEROES_DEV_DIAGNOSTICS=OFF, and for a native release). In their
place runtime/spu/spu_interp_absent.c: every way into interpretation -- a
branch to local store no lifted function covers (the microstepper's way in),
SPU_INTERP_UNLIFTED, an SPU image with no lift (spu_run_interp_job,
RD_SPU_INTERP), the dispatcher's miss -- stops the program with the image
and the address (`[fatal] spu-interpreter-absent: ...`, abort, the crash
handler's report), **whatever PS3_STRICT says**. Strict mode off no longer
lets anything be interpreted, in a player's build or any other.

Every SPU image this game runs is lifted or native: no `[release-path]`,
`[spu-smc]`, `BRANCH-TO-0`, "interpreting unlifted" or interpreter line in
any of the 376 runs' logs since R6 (build/r6, build/r7, build/r8,
build/test). The PDBs: rel3 has spu_interp_absent and none of the
interpreter (spu_smc_microstep, spu_dec_*, spu_trace_dump); rel2 had them.
DOTHEROES_R8_INTERP_TEST=1 asks for the interpreter at start, as unlifted
SPU code would: the stop's own test. With PS3_STRICT=0 the release stops at
once, `[fatal] spu-interpreter-absent: SPU image -1 reached local store
0x01234 (spu_interp_run) ...` (build/r8/interp1).

### 5.6 Register promotion: not at every call, but in the functions that make none [M]

The lifted code keeps every guest register in the context structure. With
the loads bare and the stores one mov, `__restrict` on the context still
changes nothing (the hot function of section 5: 2,502 instructions either
way): every guest store goes through a pointer the compiler cannot tell from
the context, so after each one it reloads whatever it needs from it. Only
the lifter can keep registers in locals.

The simple form of that, prototyped as a rewrite of one generated file
(ppu_recomp_002.cpp, 254 functions): each register used becomes a local,
loaded at entry; before every statement where the context escapes (a guest
call, the trampoline drain, an indirect call, a syscall, an HLE call, the
hooks) and every return, the registers written in the function are stored
back, and after the escape every register used is reloaded; functions with a
guest setjmp frame (sjlj: a host longjmp returns into them, and a local would
hold a value from before it) are left alone. In the lift every escape is one
of those statement shapes (158,396 `ctx->lr = N; F(ctx);
DRAIN_TRAMPOLINE(ctx);` lines, 10,536 indirect calls, 448 syscalls, the
jump tables' indirect default, the hooks), and no register is indexed by a
variable or has its address taken, so the rewrite is mechanical. Measured:
the hot function grows from 2,502 to 2,621 instructions and the file from
504,393 to 697,708 (+38%): the stores and reloads at every call outweigh
what the locals save between them. **Not adopted.** What promotion needs is
liveness at each call: store back only the registers a callee can read
(r1-r10, r13, and r14-r31 only where a callee may save them -- a setjmp, an
unwinder), reload only those it can change and that are still needed (r0,
r3-r12 by the ABI, if the lift trusts the ABI for r14-r31 -- which a guest
setjmp/longjmp, hand-written assembly or the trampoline's continuation
breaks), plus the condition and float registers. A dataflow pass over each
function's statements and gotos in the lifter, validated scenario by
scenario: a larger piece of work, kept for later (section 9).

**The leaf form: adopted** (the lifter's --promote-leaf, ps3recomp 0123).
The cost above is all at the calls, and a function that calls nothing has
none: where the context never escapes at all -- no guest call, trampoline
drain, indirect call, syscall, HLE call, hook, observer, sjlj frame; only
ctx->field accesses, no register indexed by a variable -- every register
the function uses (gpr, fpr, cr, ctr, xer) is a local, loaded at entry,
those it writes (or takes the address of) stored back at every return and
at its end. Calls, callbacks, non-local returns and the context's
visibility are each answered by the condition itself: there are no calls
or callbacks; no longjmp lands in such a function or passes through it (it
calls nothing, and functions with sjlj frames are excluded); and no other
thread reads a running thread's registers -- what still reads the context
mid-function is diagnostic (a crash report, a store diagnostic's stack
dump), which may show a register's value from the function's entry.
- 8,395 of the lift's 24,025 functions; a fifth of the game thread's time
  exploring, a sixth in the fight (rel6's profiles: 23.1% and 17.3% of the
  main thread's samples are in them; the hottest, func_000A2834, is one).
- The strided fill loop func_00A363DC (3.9% exploring): 26 instructions an
  iteration, six of them on the context (the loop counter a
  read-modify-write in memory, a dependency from one iteration to the
  next), against 21 and none.
- **Measured** (build/r8/lp1_run.sh: rel6 against rel7, the same build
  lifted with --promote-leaf; three rounds, rotated): the fields 236.8-244.0
  fps against 228.8-233.7, busy 4.07-4.19 ms against 4.25-4.34 (0.15 ms, 3.5%
  of the game thread), frames over 5 ms 2.8-4.3% against 4.5-5.4%; the
  fight 198.2-200.1 against 197.3-200.8 with heavy debris in all three of
  rel7's rounds and one of rel6's (heavy p50 6.55-6.69 ms against 7.90).
- Validated as rel6 was (section 7.1, val3).

## 6. Heavy debris [M]

A profile of the castle front's grass cut, uncapped (build/r8/dp1: the
timing drive, DIAG_PROF over 16 s with the frame trace; the heavy frames
are the trace's intervals around collision tasks with 40 or more awake
bodies: 5.9 s, 629 frames). tools/r8_split.py compares the main thread's
inclusive time in those frames with the window's other frames:

| | Heavy debris | Other frames |
|---|---|---|
| frames a second (profiled) | 107 (9.33 ms) | 132 (7.57 ms) |
| the main thread | busy every sample | busy every sample |

What grows in a heavy frame, ms (inclusive):

| | ms a frame | What |
|---|---|---|
| the game's physics on the PPU (lifted: func_0005D0EC; func_000D55C4, 0093D448; 0x000C-0x000D and 0x0094-0x0095 functions) | ~0.8 | Bullet's PPU side and the game's physics manager, lifted code |
| drawing the pieces | ~0.7 | the recorder +0.4 (more of asp's draws), asp's backend +0.4 |
| the SPURS handshake for physics tasks | ~0.4 | cellSpursCreateTask on the main thread for every task (its ELF fingerprint, the lock-line lock, the taskset), and the main thread polling the result queues (func_00955BD4: non-blocking pops that answer "again" while the workers compute) |

The physics tasks themselves -- native collision, native solver commands
4-6, lifted solver commands 1-2 -- take about 1% each of a handful of pool
threads: they are not the limit. The heavy frames' cost is on the main
thread: its own lifted physics, the extra draws, and the PS3 task
machinery it still drives.

### 6.1 The native physics tasks issued and completed directly [C, M]

**What the handshake was, measured** (tools/r8_phys.py on the frame trace;
the R8 release's benchmark runs, build/r8/b1u-river-r8-*: a frame that
carries a heavy step, 48 or more awake bodies): ~70 solver tasks and one or
two collision tasks a step, each created through cellSpursCreateTask on the
game thread (~4.5 us each: its bookkeeping, the content hash of the 174 KB
task image that validates the fingerprint cache, the pool's wake-up), each
waiting ~4 us to start, then waiting for its queue entry, which the game
pushes after creating the task; the native commands run 1-13 us; the game
thread waits for every task of a command before issuing the next, polling
the slots' output queues: **0.82 ms of polling in such a frame**, on top of
the creations. By command (build/r8/dp1, profiled): the lifted manifold
set-up (command 2, ~130-175 us a run, 8 a step) and body set-up (command 1)
are most of the waiting; the native iterate rounds the rest.

**The two functions.** Reading the lift: the game issues each physics task
in one function and waits in another, and nothing else creates these tasks
or touches their queues (the only callers of the import stubs of
cellSpursQueuePushBody and cellSpursQueuePopBody, and of cellSpursCreateTask
but for one other site, which creates the water and inflate tasks):
- func_00955CF0(M, slot, w0..w3): cellSpursCreateTask on the manager's
  taskset with the argument [slot, M+0x2300, M+0x1A00+slot*0x80 (input
  queue), M+0x1D00+slot*0x80 (output queue)], then a blocking
  cellSpursQueuePushBody of the entry [w0..w3] = [slot, command,
  descriptor, slot]; r3 = 0, -1 if the push failed;
- func_00955BD4(M, &cursor, &w0..&w3): from *cursor round the slots,
  a non-blocking pop of each slot's output queue until one gives an entry;
  the cursor left at that slot, the entry's words stored through the
  pointers; r3 = 0. The round goes over slots 0..*(M+0x1980), and that word
  is the slot count (3): the round visits a fourth "slot" whose address is
  no queue, which the game's pop answered with an error (it checks the
  queue's direction and alignment).
Both are reached through the two managers' virtual methods (collision: task
image 0xF63600; solver: 0xF8EC80; func_00955328/func_009552BC and
func_009557C4/func_00955758).

**The replacement** (port/native/phys_direct.cpp, port/phys_hooks.cpp,
config/r8_hooks.json): two direct entry hooks -- the lifter's per-function
hook, now able to decline (`if (guard && host(ctx)) return;`, ps3recomp
0120) -- in place of the generic observer dispatch.
- The issue: a task whose work is native (collision with
  DOTHEROES_NATIVE_CD=1 and pairs bullet_cd covers; solver commands in
  DOTHEROES_NATIVE_SOLVER) becomes a job in a small native pool (two
  threads that spin 100 us after a job, then sleep): no SPURS task, no
  queue push, no taskset slot, no image hash; the hook returns r3 = 0. Any
  other task (solver commands 1 and 2; collision work bullet_cd does not
  cover) is declined: the lifted body creates its SPURS task as always.
- The wait: round the same slots in the same order, taking for each a
  native job's completion or, while the manager has lifted tasks out, an
  entry from the slot's output queue (the runtime's host-side pop under the
  lock-line lock, with its coherence notice and wake-up, after the checks
  the game's pop made: the queue 128-byte aligned, SPU-to-PPU, 16-byte
  entries); the cursor and the entry written as the game's poll writes
  them. While nothing is complete, the waiting thread runs a job not yet
  started itself, then spins.
- What is kept: the order (a job is issued when the game issues it, jobs
  are taken oldest first, and the game still waits for every task of a
  command before the next); visibility (a job is published with release and
  taken with acquire, so its runner sees everything the game wrote before
  the issue; its completion is published with release and the waiting
  thread acquires it before the game reads the results); lifetimes (a job
  holds nothing of the game's past its completion: the descriptor and its
  data are the game's, untouched by it until it has seen the task done).
  The work is the native tasks' own code (bullet_cd_task.cpp,
  bullet_solver_task.cpp: the queue operations taken out of it), with their
  checks, captures and counts; the port's creation hook (captures, the
  solver's checks) is called at the issue as CreateTask called it.
- DOTHEROES_PHYS_DIRECT=0 leaves every task to SPURS, as before.

**A defect found on the way.** The first version popped every slot the
game's round visits without the game's pop's checks: the fourth "slot"
(no queue) was popped as if it were one -- a write of its "counter" into
whatever memory follows the third queue, and its "entry size" copied into a
16-byte buffer. One validation run of three died there (an access
violation at the first lifted solver task, build/r8/smoke-rel4, no crash
report); the other two passed, so the passes proved nothing. The checks are
in; the slot range is logged once a manager ("its wait goes round slots
0..3: q q q -").

**Checked** (build/r8/pv1_run.sh: rel4, the direct issue, against rel3, the
same build through SPURS): the solver's own checks
(DOTHEROES_NATIVE_SOLVER_CHECK: each step's copy-back and write-back worked
out before they run and compared, each iterate round run natively on a copy
and compared) in the river fight (600 steps) and the castle front's grass
cut (200 steps), both builds: copy-back 0 bodies wrong, write-back 0
contacts wrong, iterate 0 rounds different from the native model; the
gameplay timing drive at 60 fps: physics 361 steps in 6.0 s with up to 55
bodies, the walk 4.99 units a second either way, as before; normal damage
(life down to 5 half apples, the fight done); DOTHEROES_PHYS_DIRECT=0 runs
everything through SPURS again. The release adds one more guard: the
managers' table belongs to the one thread that issues and waits, and a
second thread at either stops the program (the game uses one).

**Measured** (build/r8/pb1_run.sh: rel3 against rel4, everything else
equal; two rounds each, the order rotated; frames by the most awake bodies
any collision task in their interval saw, heavy = 48 or more):

| | heavy frames a second | heavy p50 ms | heavy p99 ms | other frames p99 ms |
|---|---|---|---|---|
| castle grass cut, through SPURS (rel3) | 141-143 | 6.89-6.91 | 8.81-10.92 | 5.90-5.95 |
| castle grass cut, direct (rel4) | **146-149** | **6.60-6.70** | **8.47-8.94** | 5.82-5.97 |
| river fight, through SPURS | 118-127 | 7.85-8.35 | 8.67-13.09 | 6.92-7.17 |
| river fight, direct | **125-133** | **7.27-7.90** | 9.39-10.04 | 7.14-7.44 |

A heavy frame 0.2-0.5 ms shorter; the other frames as they were (no
physics, the pool asleep). The solver tasks start at once (dispatch to start
p50 0 us, against 4 us: the waiting thread or a spinning pool thread takes
them); about a third are run by the waiting thread itself. What is left of
the wait is the lifted commands 1 and 2 (~130 us a run, nine a step) and
the native commands' own run time: rel5's frame trace marks, in each wait,
the part the waiting thread spent running jobs, so tools/r8_phys.py
reports the polling apart from that work (section 8.2).

## 7. Normal damage [M]

The benchmark and most tests run with can't-die (lethal damage stopped by
topping life up). With normal damage (DOTHEROES_DEV_CANT_DIE=0,
DOTHEROES_DEV_LIFE_LOG=1), the river fight, R7's release and R8's
alternating (build/r8/dmg-*, dmg2-*):

| Run | Hits taken | Lowest life (half apples) | The fight |
|---|---|---|---|
| R7 (3 runs) | 1, 6, 6 | 9, 4, 6 | 82.6-83.9 s, done |
| R8 (3 runs) | 10, 2, 4 | 0 (one death at 108 s), 8, 6 | 82.8-83.0 s done in two; the death in the first |

The scripted fight does not aim: in the run that died, the hero stood at
the wall by the river while an enemy kept reaching him (the grabs show it).
Damage varies from run to run in every build -- the can't-die runs' top-ups
(each one a hit taken at 8 half apples) were 1-6 a run for R7, R8's
variants and production alike (build/r8/ab*, v1river; build/r7/m1u*) --
so three runs each show no difference between the builds, in either
direction. Damage, death and the game-over screen work in R8 as in R7.

### 7.1 R8 part 2's release, validated [M]

build/r8/rel6 (dotheroes.exe sha256 edc409bd...; build/r8/rel6/SHA256SUMS):
the lift of build/r8/gen5 (R8's hooks with config/r8_hooks.json, the
lifter of ps3recomp 0122) built by tools/r8_release_build.sh: bare loads,
single-mov stores with their tests, the raw stores' reservation test, the
physics tasks issued directly, no SPU interpreter. Its checked comparison
build: build/r8/chk6. build/r8/val2_run.sh (REL=rel6), every run strict:

- **Self-test**: `--memtest` 614 checks, 0 failed (rel6, chk6).
- **The eight scenarios** (v2*: river, menus, fields, dungeon, snapshot,
  the field after the dungeon, the editor, the exit after the boss; grabs
  every 5 s): every drive done or run to its time; no crash, hang, strict
  or fatal stop, lost graphics work or interpreter entry. Grab for grab
  against R7's release runs (tools/r7_grabs.py): the same brightness and
  coverage, differences where things move (0.0-3.3 on the editor's and the
  snapshot's static screens); dark grabs 4-7 a run (loads and fades), the
  dungeon's last two in its final load's fade past R7's last grab, as in
  R8's first validation.
- **Synchronisation validation** (vval2river): 0 errors, the same 10
  warnings as R7 and R8's first release.
- **The capture check** (vcc2river): 385,014 records, 0 inputs written
  between the flush and the capture.
- **The stops**: lost graphics work (lost5, strict off): the stderr line,
  the exit count, the notice on screen in the same grabs as R8's first
  release; the interpreter (interp2, strict off): `[fatal]
  spu-interpreter-absent`, exit 0xC0000409.
- **Gameplay timing** (t3-60, t3-bch, the castle-front timing drive): at 60
  fps the camera target 5.00 units a second each way, physics 360 steps in
  6.0 s with up to 55 bodies; uncapped at 207 fps (188 in the grass cut) the
  same: 4.99 units, 360 steps in 6.0 s, 55 bodies.
- **Normal damage** (dmg3-1..3, the river fight): 0, 4 and 11 hits; lowest
  life 10, 6 and 0 -- the third died, the game-over screen and the
  continue took it back to the castle with full life, and the drive walked
  on until its time: the same spread as R8's first release (10, 2, 4, one
  death) and R7's.
- **The physics** (section 6.1): the solver's own checks with the direct
  issue (pv1, rel4).

**rel7**, the same build lifted with --promote-leaf (build/r8/gen6; sha256
746da4b9...; its checked comparison build: tools/r8_release_build.sh with
-DDOTHEROES_PPU_CHECKED_LOADS=ON on gen6), validated the same way
(build/r8/val3_run.sh, v3*, vval3, vcc3, lost6, interp3, dmg4-*, t4-*):
`--memtest` 614 checks, 0 failed; the eight scenarios done or run to their
time, no stop of any kind; their grabs against R7's as rel6's were (static
screens 0.0, differences at moving scenes and fades, rel6's maximum 72.8 and
rel7's 34.5 on the editor's menu transitions); synchronisation validation 0
errors, 10 warnings; the capture check 395,267 records, 0 written between
the flush and the capture; both stops; the timing drive at 60 fps (5.00
units a second, 359 steps in 6.0 s with 54 bodies) and uncapped at 203 fps
(5.13, within the 4.99-5.17 the holds give; 360 steps, 55 bodies); normal
damage 4, 3, 2 hits, lowest life 6-8. **rel7 is R8 part 2's release.**

**rel8** is rel7's tree with the last source changes, both outside the release's measured paths: the physics pool's
threads start with the first native task instead of at start (a lift
without the hooks -- the production lift, a player's -- never starts them),
and memtest_bare.cpp's fallback where the bare loads are not established.
Checked: `--memtest` 614/0; the river fight with the physics counted
(build/r8/smoke-rel8: the full time, no stop, 23,091 native tasks issued
directly, the pool started with the first).

**The other configurations, built and run.** The production development
build (build/port-win: the production lift, the RSX command path, strict)
and **the player's own build path** (tools/zig_build.py as setup_app.py
runs it: the prebuilt runtime and native libraries, then the install-time
compile and link of the lift -- build/r8/player_build.py): both built, both
pass `--memtest`, both ran the river fight to its time
(build/r8/smoke-prodcfg, smoke-player), physics through SPURS there (no
hooks in that lift), no SPU interpreter in either. **A defect found on the
way, older than R8:** the player build compiles the port from a fixed list
(zig_build.py's PORT_SOURCES), and since 2026-11-03 it lacked cpu_place.cpp,
which main.cpp calls: a player's install would have failed to link (the
player libraries were last built on 2026-11-02, so nothing showed it). The
list now has it, and R8's new port sources (phys_hooks.cpp, the memtest's
three).

**A second defect, older than R8 (from R7):** the native release drew only
with DOTHEROES_PPFX_NATIVE=1, which every test run sets
(tools/r8_scene.sh) and nothing else does. Started any other way it showed
a black window (build/r8/smoke-rel8-noenv: 8 of 8 grabs black, one flip in
40 s): with no walker and no command path, the recorder (ppfx::mode) and the
Vulkan backend's second queue for the native renderer both waited for the
switch. A native release now defaults both to on when the switch is unset
(ppfx_recorder.cpp; ps3recomp's rsx_vulkan_backend.c). The recorder's half
alone was not enough (smoke-rel8-noenv2: still black); with the backend's
half (ps3recomp 0124) the release draws started without the switch
(smoke-rel8-noenv3: 2,274 frames in 40 s, the scene in its grabs). rel8 is
rebuilt with both (sha256 f02ff92a...).

### 7.2 The player's build path, audited (2026-11-07) [C]

Done while the owner's PC was in use: reviewed and changed in source,
checked by static tests; **not yet built and not yet run** (section 9.1
queues both). The question: does a player's install (setup_app.py ->
tools/setup_pipeline.py -> tools/zig_build.py) produce the native release
that R8 builds, validates and measures (tools/r8_lift.sh, then
tools/r8_release_build.sh)?

**It did not** [C]. Three layers each built the development configuration:

| Layer | The measured release (rel7/rel8) | The player's path, until now |
|---|---|---|
| The lift (setup_pipeline.recomp) | config/sjlj.json, insn_patches.json, **r8_hooks.json** (the physics issued directly), **r7_native_hooks.json** (the entry hooks into the native renderer), **--promote-leaf**, 48 chunks | sjlj.json and insn_patches.json only |
| The prebuilt runtime and native library (zig_build.build_prebuilt) | **DOTHEROES_NATIVE_RELEASE=ON**: no RSX command path, no R4 bridge, the NID table without the walker's functions | the development configuration: the RSX command path and the bridge |
| What the install compiles (zig_build.game_jobs: the port, the PPU scaffold, the NID table, the lift, the SPU images) | DOTHEROES_NATIVE_RELEASE=1, PPU_BARE_LOADS=1, PPU_STORE_MOV=1 and the runtime's public PS3RECOMP_RSX_COMMAND_PATH=0, PS3RECOMP_RSX_VULKAN, PS3RECOMP_SPU_INTERPRETER=0 | PS3RECOMP_RSX_VULKAN only |

So part 2's player build (build/r8/player, smoke-player) was a working game,
but the old renderer, the checked loads and stores, and physics through
SPURS -- not the release. The layers must change together. A release
runtime under development-compiled sources does not link (main.cpp's
present path calls the walker's functions). Sources compiled without
PS3RECOMP_SPU_INTERPRETER=0 take spu_interp.h's default of 1 against a
runtime built without the interpreter. A release runtime without the R7
hooks in the lift has nothing that draws.

**Changed** (implemented; static tests only):
- setup_pipeline.py: the lift's options are LIFT_CONFIG and LIFT_FLAGS,
  tools/r8_lift.sh's.
- zig_build.build_prebuilt: configures the native release by default
  (RELEASE_CMAKE; `release=False` keeps the development configuration). It
  records the configuration and the definitions CMake gives the game
  executable's sources (read from its build.ninja, less main.cpp's
  development diagnostics) in build/lib/<platform>/game_build.json, which
  the bundle ships with the libraries.
- zig_build.game_jobs: adds those definitions to every source the install
  compiles. They are in each object's fingerprint, so an install built in
  another configuration recompiles everything; nothing is reused across
  configurations. A prebuilt without the record is an older, development
  one, and gets no extra definitions, as before.

**Tests.**
- tests/test_player_pipeline.py: 11 checks, no compiling, run and passing:
  - the lift's options against tools/r8_lift.sh;
  - the configuration files committed;
  - PORT_SOURCES against port/CMakeLists.txt's executable;
  - the build.ninja parser, on a sample and on rel7's own build tree (the
    six definitions above, exactly);
  - every kind of install-time source getting them;
  - an older prebuilt getting none;
  - no object reused across configurations;
  - build_prebuilt's CMake arguments.

  Both old defects fail it: the lift without the R7/R8 configuration, and
  the source list without cpu_place.cpp.
- tests/player_install_test.py, **prepared, not run**: a fresh install from
  the disc image and build/EBOOT.elf into F:, then:
  - **check**: the stamps, the settings file, and the lift (leaf promotion,
    the R7 and R8 hooks, file for file against the developers' lift
    build/r8/gen6);
  - **update**: nothing changed, so every step but the build is skipped,
    every object is reused, and the game is relinked;
  - **relift**: the lift's stamp cleared, so the same code is lifted again
    and every object is reused;
  - **settings**: a player's own dotheroes.cfg values kept, and a missing
    performance switch added back;
  - **game**, only with --run-game: `--memtest`, then a river fight
    started as a player starts the install.

**Differences that remain between a player's build and the measured
release** (reviewed; each to be measured or decided, not assumed harmless):

| Difference | Kind | What to do |
|---|---|---|
| Development diagnostics off | deliberate | no crash reports or PDB; not strict by default |
| The prebuilt runtime and native library at x86-64-v2 | performance | the developers' builds run at this PC's v3, and the native library holds Bullet's narrowphase, the solver commands, the recorder and the renderer. To be measured (section 9.1) |
| Symbols stripped (-g0, -s) | deliberate | -- |
| GCM_WRAP_LAP=1 and DOTHEROES_PRESENT_ONCE=1 in dotheroes.cfg | inert | they act on the FIFO walker, which the release does not have [C]; the other two performance switches are the native physics, which the release needs to match R8's measurements |
| DOTHEROES_FPS=60 by default | for the settings | docs/PC_EXPERIENCE.md replaces it |
| Linux | untested | the release configuration has never been built there (loads and stores stay checked there by CMake) |

An existing install's update relifts, because setup_pipeline.py is in the
tools' fingerprint. It then recompiles everything, because the definitions
changed: about the time of a fresh build. Its dotheroes.cfg is kept.

## 8. Measurement [M]

build/r8/bench1_run.sh: four arms under R7's batch-4 conditions -- the
production build (build/r6/prodF), R6 on its lean lift (build/r6/nI), the
R7 release as measured (build/r8/base = build/r7/rel5) and the R8 release
(build/r8/rel1). Real audio, the scenario's own shader cache, can't-die,
normal placement, comparisons off, the physics load counted in every arm.
Uncapped = the benchmark mode (no game or vblank ceiling, flips complete
when executed, a mailbox present) with the frame trace and GPU timestamps;
three rounds, the arms' order rotated; nothing else ran meanwhile. Frame
times are the guest's flips (tools/r7_measure.py); busy is the game
thread's time a frame less its waits. rel2, the release committed, is rel1
with the lost-work summary printed once (exit path only).

**Combat: the river fight, uncapped** (60 s of trace in the fight):

| Arm (3 runs) | fps | p50 ms | p90 | p99 | p99.9 | frames over 6.94 ms | over 5.00 ms | game thread busy ms | GPU ms |
|---|---|---|---|---|---|---|---|---|---|
| production | 137.6-142.6 | 6.86-7.11 | 7.74-8.05 | 10.30-11.01 | 16.06-16.94 | 42-61% | 100% | 5.98-6.11 (+ its walker) | 2.38-2.47 |
| R6 | 147.5-150.0 | 6.40-6.48 | 7.53-7.96 | 9.83-10.59 | 11.93-12.43 | 13-23% | 100% | 6.47-6.65 | 0.69-0.74 |
| R7 | 152.2-153.4 | 6.23-6.33 | 7.46-7.54 | 9.35-10.12 | 11.49-11.80 | 9-15% | 100% | 6.27-6.44 | 0.44 |
| **R8** | **187.2-190.2** | **5.04-5.09** | **5.93-6.24** | **7.81-8.70** | **9.49-10.23** | **1.9-5.7%** | 54-62% | **5.09-5.25** | 0.40 |

**Heavy debris** (the fight's intervals with 48 or more awake bodies):

| Arm | heavy frames: rate | p50 ms | p99 | max | other frames: rate | p99 |
|---|---|---|---|---|---|---|
| production | 117-118/s | 8.03-8.09 | 16.20-16.63 | 16.5-19.1 | 139-145/s | 10.10-10.39 |
| R6 | 104-110/s | 8.87-9.50 | 11.09-12.37 | 11.1-14.6 | 150-153/s | 8.87-9.64 |
| R7 | 107-118/s | 8.37-9.28 | 10.00-12.17 | 10.2-15.7 | 154-158/s | 8.43-9.09 |
| **R8** | **119-139/s** | **7.04-8.40** | **9.17-9.58** | **9.2-11.0** | **191-194/s** | **7.34-7.49** |

**Exploration: the fields, uncapped:**

| Arm (3 runs) | fps | p50 ms | p90 | p99 | p99.9 | max | over 6.94 ms | over 5.00 ms | busy ms |
|---|---|---|---|---|---|---|---|---|---|
| production | 178.6-181.3 | 5.39-5.44 | 6.10-6.21 | 7.82-8.19 | 11.52-15.44 | 19.8-107.1 | 3.0-4.4% | 80-85% | 5.34-5.45 |
| R6 | 165.7-167.9 | 5.88-5.96 | 6.49-6.66 | 7.90-8.18 | 10.33-10.83 | 92-99 | 3.5-4.6% | 95-97% | 5.77-5.89 |
| R7 | 168.7-173.7 | 5.68-5.89 | 6.25-6.51 | 7.65-7.97 | 9.57-10.26 | 12.6-43.5 | 2.7-3.2% | 93-96% | 5.64-5.69 |
| **R8** | **224.1-227.6** | **4.33-4.40** | **4.72-4.83** | **6.02-6.10** | **7.87-8.09** | 10.7-50.8 | **0.3%** | **4.6-5.4%** | **4.30-4.34** |

**Transitions** (the walk from the castle across the area boundary, its
load included, 23 s): R8 257/s, p99 6.46-6.69 ms, the load's hitch 64 ms
(one interval over 50 ms); R7 216/s, p99 7.73-8.26, 92 ms; R6 172-177/s, 92-95
ms; production 164-173/s, 90-93 ms.

**Capped (60 fps), the river fight:** every arm 60.0 flips a second; the
window's presents p99 / p99.9 / over 25 ms: production 22.37 / 24.94 / 4,
R7 18.83 / 20.51 / 0, R8 18.69 / 20.65 / 0.

**What it shows:**
- The game thread is still R8's limit everywhere (busy 5.09-5.25 ms a
  combat frame, 4.30-4.34 exploring), 1.1-1.3 ms less than R7's: the loads'
  tests gone and the call-site cache. The native renderer's thread (1.4 ms
  a frame) and the GPU (0.4 ms) have room.
- **Exploration's regression against production is gone**: R8 explores at
  224-228 fps against production's 179-181 (25% faster), its p99.9 7.9-8.1
  ms against 11.5-15.4.
- **144 fps sustained**: exploring, 99.7% of frames are within 6.94 ms (p99
  6.0-6.1 ms) -- the 0.3% over it are the area's streaming and the like; in
  combat 94-98% are (p90 5.9-6.2, p99 7.8-8.7): **not yet sustained in
  combat**, where the heavy debris frames (7.0-8.4 ms at p50) and the
  fight's busiest frames are over.
- **200 fps**: exploring, 95% of frames within 5 ms (p50 4.3-4.4); in
  combat, 38-46%. Not sustained.
- Gameplay timing holds at 60 and at 200 (section 5.2).

### 8.2 R8 part 2's release measured [M]

build/r8/bench3_run.sh (and bench3b): bench1's conditions, four arms --
production (build/r6/prodF), the R7 release (build/r8/base), R8's first
release as measured (r8a: build/r8/rel1) and part 2's (r8b: build/r8/rel6).
Three rounds for production and R7, five for the two R8 arms (round 3 of the
river ran slow for every arm, r8b's from start to end -- 163-177 fps against
194-199 in its other rounds, a state of the machine, not the game -- so the
R8 arms got two more), the order rotated; tools/r8_bench.py. Medians in
brackets.

**Combat, the river fight, uncapped:**

| Arm | fps | p50 ms | p90 | p99 | p99.9 | over 6.94 ms | over 5.00 ms | busy ms |
|---|---|---|---|---|---|---|---|---|
| production | 139.9-149.7 | 6.56-7.02 | 7.29-7.87 | 9.87-10.34 | 11.88-16.31 | 23-56% | 100% | 5.86-5.97 |
| R7 | 149.3-157.3 | 6.21-6.37 | 6.83-8.16 | 8.55-10.13 | 10.99-12.07 | 8-23% | 100% | 6.27-6.62 |
| R8a | 188.2-194.7 [192.1] | 5.04-5.18 | 5.51-5.97 | 7.01-7.76 [7.32] | 8.98-9.47 | 1.1-4.7% [1.6] | 56-80% [65] | 5.07-5.25 [5.14] |
| **R8b** | 171.6-197.5 [**194.4**] | 4.94-5.65 [4.96] | 5.48-6.52 [5.66] | 6.87-8.31 [**7.29**] | 8.38-10.76 [8.63] | 0.9-6.7% [**2.2**] | 41-100% [**43**] | 5.00-5.76 [**5.08**] |

Heavy debris in the fight (rounds with such frames): R8a p50 7.14-8.31 ms,
p99 9.55-11.21; R8b p50 6.89-7.52, p99 8.43-11.14. **The castle front's
grass cut** (the heaviest debris, two rounds): heavy frames R8a 137-139 a
second, p50 7.06-7.11 ms; R8b 136-146, p50 6.73-6.81. The game thread's
polling in such a frame (tools/r8_phys.py): R8a 0.53-0.61 ms; R8b 0.39-0.42
ms, plus 0.13-0.20 ms of physics jobs it runs itself while it waits --
useful work, now told apart from the polling.

**Exploration, the fields, uncapped:**

| Arm | fps | p50 ms | p90 | p99 | p99.9 | over 6.94 ms | over 5.00 ms | busy ms |
|---|---|---|---|---|---|---|---|---|
| production | 185.1-187.3 | 5.26-5.30 | 5.87-5.98 | 7.53-7.69 | 10.43-10.96 | 2.5-3.2% | 79-83% | 5.30-5.37 |
| R7 | 174.4-175.0 | 5.69-5.72 | 6.23-6.24 | 7.55-7.79 | 10.13-10.99 | 2.5-2.6% | 92-95% | 5.68-5.69 |
| R8a | 209.4-231.6 [229.0] | 4.27-4.48 | 4.70-5.14 | 5.86-7.13 [5.90] | 7.79-9.40 [8.21] | 0.2-1.1% [0.3] | 4.5-13% [5.0] | 4.29-4.59 [4.34] |
| **R8b** | 231.4-235.1 [**234.7**] | 4.22-4.32 [4.23] | 4.62-4.70 | 5.68-5.91 [**5.78**] | 7.43-8.81 [7.56] | 0.2-0.3% [**0.2**] | 3.8-4.2% [**3.9**] | 4.22-4.29 [**4.23**] |

**Capped (60 fps), R8b:** 60.0 flips a second, the presents' p99 18.76 ms,
p99.9 19.99, none over 25 ms.

**What it shows:**
- Part 2 is mostly correctness and architecture -- the loads' assumptions
  tested and gated, no interpreter, the physics tasks out of SPURS, the raw
  stores' reservation gap closed -- at no measured cost and some gain: the
  game thread 0.1 ms less a frame exploring (busy 4.23 against 4.34), 0.06
  ms less in the fight, the heavy-debris frames 0.3 ms shorter.
- **144 fps sustained: exploring yes** (99.8% of frames within 6.94 ms, p99
  5.8 ms); **in combat not yet**: 97.8% of frames within 6.94 ms at the
  median (p99 7.3 ms) -- the heavy-debris frames (p50 6.9-7.5 ms) and the
  fight's busiest are over.
- **200 fps**: exploring, 96% of frames within 5 ms; combat 57% (p50 4.96
  ms): not sustained.
- Against production: combat +31% (194 against 148 fps at the median),
  exploring +26% (235 against 186), p99.9 7.6 against 10.4-11.0 ms.
- **rel7** (rel6 with leaf promotion, section 5.6), measured against rel6
  in lp1 (three rounds, rotated): exploring 236.8-244.0 fps against
  228.8-233.7, busy 4.07-4.19 ms, frames within 5 ms 95.7-97.2%; the
  castle's grass cut heavy frames 152-156 a second at p50 6.32-6.49 ms
  against 147-150 at 6.57-6.63; the fight as rel6 with more of its debris.

### 8.3 Where the game thread's time goes now [M]

Profiles of rel6 as the inventory's were made (build/r8/prof2_run.sh:
p2river, p2fields; DIAG_PROF over the same windows, uncapped; the frame rate
over each window from its frame log), against the R7 release's
(p1river, p1fields; section 1):

| The main thread, ms a frame (profiled) | River R7 | River rel6 | Fields R7 | Fields rel6 |
|---|---|---|---|---|
| frames a second, profiled | 128 | 173 | 147 | 198 |
| the game's lifted code | 5.49 | 3.98 (68.7%) | 5.47 | 4.12 (81.5%) |
| the recorder | 1.12 | 1.03 (17.8%) | 0.51 | 0.52 (10.2%) |
| ps3_indirect_call | 0.40 | 0.03 | 0.46 | 0.06 |
| the runtime's other helpers | 0.10 | 0.12 | 0.09 | 0.10 |

- The lifted code is 1.4-1.5 ms a frame less (the loads' tests, R8's first
  part) and the indirect dispatch is gone (the call-site cache).
- **The recorder is now the second cost**, 17.8% of the fight's game thread:
  within it its lock (ppfx::acquire, 2.7%), the generic observer dispatch
  (ppfx_rec_observe, 2.6%), mutex calls (about 1.2%) -- about 0.35 ms a
  frame of bookkeeping round the records themselves (asp_record_call_inner
  1.7%, the copies).
- The hottest lifted functions exploring: func_000A7C78 (8.3%, not a leaf),
  func_000A2834 (7.0%, a leaf), func_00A363DC (3.9%, a leaf: a strided
  fill). Leaf functions: 23.1% of the samples exploring, 17.3% in the fight
  (section 5.6).
- The other threads: the native renderer's thread 65% busy in the fight (the
  recorder's consumption 29%, Vulkan work 13%, the rest a timed wait), the
  water task 9.5% of one pool thread, FMOD's mixer 2%; the physics pool
  threads idle outside physics steps. The main PPU thread is still the
  critical path alone.

### 8.4 The recorder's bookkeeping: what it must keep, and a direct path (2026-11-07) [C, M]

Investigated from the source and the kept runs' reports. Prepared, not
implemented: the only code change is two report counters, compile-checked,
not yet run.

**What it costs** [M, section 8.3]: the recorder is 1.03 ms of a fight
frame's game thread and 0.52 ms exploring. About 0.35 ms of the fight's is
bookkeeping around the records:
- the lock, under ppfx::acquire: 2.7%;
- the generic dispatch, ppfx_rec_observe: 2.6%;
- mutex calls: about 1.2%.

**How many calls it sees** [M, v3river's report: rel7's validation fight,
5,909 frames]:
- 35.8 million observed calls, about 6,070 a frame, not counting the game's
  memcpy, which returns before the count;
- asp's, 22.4 million: 62%;
- `other_threads 0`, `producer_threads 0`;
- `local_copies 8,157`.

The calls by address were last counted in R6, before the bodies were
skipped (u1river, a flip). The constants (0x006C9C74) 1,271, the stream
unbind (0x006D32D0) 1,235 and bind (0x006D3190) 673, the game's vertex
arrays (0x0069F19C) 612 and the fragment patch (0x006C9624) 476 made about
70% of 6,400. The current release's counts are not measured (section 9.1).

**One call's path** [C]:
1. The lifted wrapper tests g_ppu_entry_observe and calls
   ppu_entry_observe (r1_observe.cpp).
2. That calls ppfx_rec_observe, then r1_log: one address test and one
   atomic load.
3. ppfx_rec_observe makes, in order:
   - the start-up test, the call-count test, the memcpy test;
   - a 1-in-16 time sample and the call counter;
   - GetCurrentThreadId, then **the lock (s_lock), taken on every call**;
   - the flip and inline-transfer tests;
   - the producer-thread test and the title's context;
   - R7's calls (r7_call), asp's state (asp_resolve) and asp's calls
     (asp_fn, asp_call);
   - the caller's attribution, then the occlusion, R6 and R4 decisions.

   Each call walks this chain of tests before its own work.

**The producer-thread requirement, established.**
- [C] The command stream has one producer: the thread of the first
  observed call (S.tid). A PPFX call on any other thread is fatal. Other
  code's calls on other threads are counted and left out of the state.
- [M] In both kept fight reports (v3river; R6's u1river, 29,921 frames)
  every observed call but the memcpy came from that one thread.

**The lock's job, established** [C]. s_lock keeps the producer apart from
two writers that may run on other threads:
- the game's memcpy into RSX local memory, which updates the recorder's
  copy of program memory (shadow_cpu_write: the shadow's pages and its
  epoch counter);
- the inline transfer entry, 0x0069FC38 (shadow_write), handled before the
  thread test.

Nothing else needs it:
- the native renderer's thread never takes it. It reads the record queue
  (put/flushed/done atomics; q.m and q.cv only for the full-queue wait),
  the records, the program blobs (immutable once made) and guest memory;
- the report uses its own locks and the counters' atomics;
- other threads' other calls touch no state, but take the lock today
  because it comes before the thread test.

acquire() lets the lock go while it waits for room in the queue
(queue_full_waits: 0 in u1river), so those writers never wait behind a full
queue. **Whether either writer actually runs on another thread in play is
not known**: the report counted local copies but not their thread. Two
counters now say: `local_copies_other_threads` and
`transfers_other_threads` on the report's `calls` line (ppfx_native.h,
ppfx_recorder.cpp; compiled in both configurations, not yet run).

**Order requirements a faster path must keep** [C]:
- records are published in call order (seq = the queue's put), with the
  producer's state (S, A) as the calls left it;
- a record sees program memory as the copies and transfers before its call
  left it: a copy on another thread that the game ordered before the call
  must be in the shadow when the call reads it;
- each entry returns what it returns today: 0 the body runs, 1 it does not,
  2 it runs and its exit is observed, with the exit observation in order.

**The direct path, prepared** (each step a change of its own, validated
and measured before the next):

| Step | What | Correctness test | Measure |
|---|---|---|---|
| 0 | the current release's calls by address (DOTHEROES_R6_CALL_COUNTS=1) and the two new counters, fight and fields | -- | which entries to take first; whether other threads write at all |
| 1 | **no lock on the producer's path**. Other threads' copies and transfers go onto a short hand-off list (its own small lock and an atomic flag); the producer applies the list at the top of its next call (one relaxed load when empty) -- the same order as the lock gives today: whichever enters first. S.tid set once by compare-exchange; other threads' other calls return without the lock; acquire() no longer lets go of anything | the eight scenarios against rel8, sync validation, the capture check; the development configuration's cross-check (DOTHEROES_PPFX_XCHECK against the engine's own commands: 0 mismatches); a forced test of the hand-off (a development switch that makes the memcpy observer queue every copy) | A/B against rel8, uncapped, real audio: busy, the recorder's sampled time |
| 2 | **an indexed dispatch**: the lifter passes each observed entry's index (`ppu_entry_observe_ix(i, addr, ctx)`; the entry list is fixed at lift time) and the recorder keeps a table of handler classes -- memcpy, flip, transfer, R7's calls, asp's calls, the rest -- so a call goes to its class without the chain of tests; the start-up, call-count and timing tests leave the per-call path (counting becomes a build option) | as step 1, plus the calls-by-address report equal in shape to step 0's | as step 1 |
| 3 | **direct handlers for the hottest entries** (from step 0; in R6 asp's constants, stream bind/unbind, fragment patch, pair bind and draw, and the game's vertex arrays): each does what the generic path does for its address, in the same order (asp_resolve first, then the call), with its preconditions checked once instead of per call (the producer thread, the title's context known, not one of R7's calls) | as step 2, per handler: its records' counts and contents against the generic path's on the same drive | per handler: the recorder's time at that entry |

**Step 0, run** (2026-11-09, rel9's cc9river): 157,765,392 calls a run; other_threads 0,
local_copies 8,168, local_copies_other_threads 0, transfers_other_threads 0 -- in play every call
is the producer's. **Step 1, done** (rel11; port/native/render/ppfx_recorder.cpp): the producer's
calls take no lock; another thread's copies and transfers go onto a hand-off list (its own lock and
an atomic flag), which the producer applies at the top of its next call, at the top of its next
return (asp_draw_exit built a record there under the lock too) and before its own copies; another
thread's PPFX call is fatal, its other calls only counted. Corrections to the plan above: the lock
also covered asp_draw_exit's record; the list has to be applied before the producer's own copies;
and the lock never made shadow access exclusive (it was let go during a full-queue wait).
DOTHEROES_RECORDER_LOCKED=1 keeps the old path; DOTHEROES_RECORDER_HANDOFF_TEST=1 sends the producer's
own copies and transfers through the list. Checks: the five reference scenes within the accepted
figures on the new path and under the hand-off test (723,670 entries handed over and applied); the
capture check clean on both (392,152 and 397,815 records, nothing written between the flush and the
capture); menus, editor, snapshot, dungeon; `shadow_handoffs_applied 0` in play. Measured (build/r8/rel11_run.sh,
uncapped, real audio, two rounds rotated): the fields 248.1-250.6 fps, busy 3.96-4.00 ms, against the
locked path's 242.8 (4.09) and rel10b's 243.4 (4.08; its other round 227.1, an outlier); the river
fight 203.8-205.3, busy 4.81-4.84, against 201.7-204.4 (4.83-4.90) and 202.3-203.1 (4.86-4.88) --
within the spread; its heavy-debris frames too few in these runs to compare (p99 10.27-10.73 against
9.62-9.64 in one round each). Adopted.

The ceiling of steps 1-2 is the measured bookkeeping, about 0.35 ms of a
fight frame [I]; step 3 trims the handlers' own work and is estimated per
entry once step 0 gives the counts. Register-level hooks (the physics'
declining entry hooks) do not fit the observed entries as they are: those
can replace a body or decline, but not run it and observe its return
(code 2), which several entries need. So the index goes through the
observer instead.

## 9. What R8 leaves, and the next steps

R8 so far: the inventory; the timer question settled; the harness's hold
fixed; unimplemented graphics and SPU interpretation made loud; the loads'
tests and the indirect dispatch cut; the bare loads reviewed, tested and
gated; the stores priced and their reservation gap closed; the native
physics tasks out of SPURS; registers in locals where nothing can see them.
Left, in the order the measurements point:

| Left | Measured | Next |
|---|---|---|
| **The lifted code** | 68-82% of the game thread; registers in memory across every call | promotion across calls with liveness (section 5.6): store back only what a callee can read, reload only what it can change and is still needed -- a dataflow pass in the lifter, validated scenario by scenario. The hottest non-leaf function (func_000A7C78, 8% exploring) first |
| **The recorder's bookkeeping** | 1.03 ms a fight frame, 0.35 ms of it the lock, the generic dispatch and mutex calls round the records | requirements established and the direct path prepared in three steps (section 8.4): no lock on the producer's path (other threads' copies handed off), an indexed dispatch through the observer, direct handlers for the hottest entries. Step 0 (the current counts) first |
| **The SPU reservation test on every store** | 0.4 ms of the game thread (10%) -- priced by a measurement build, not removable as things stand | **a decision for the owner**: (a) keep it until no lifted SPU code takes reservations (the audio milestone and the non-audio SPU code below); or (b) change the synchronisation model: an SPU's PUTLLC checks the 128-byte line against what its GETLLAR read (as ppu_stwcx32 already does for the PPU) instead of every PPU store breaking reservations -- which loses PUTLLC's detection of a store that rewrote the same bytes (ABA) and needs another wake-up for SPU code waiting on a lost reservation (the SPURS queues and event flags, FMOD) |
| **Heavy-debris frames** | p50 6.9-7.5 ms in the fight (over 144's 6.94): the game's lifted physics on the PPU (~0.8 ms), the lifted solver commands 1-2 (~130 us a run, nine a step, waited for in turn) | native commands 1-2: a port of Sony's set-up code bit-exact with the SPU (its truncating single precision, its estimates), as iterate was -- days of work, worth ~0.3 ms of a heavy frame |
| **The stores' range test and the diagnostics' span** | 0.2 ms more (measurement build) | required (the first page's stores dropped, the store diagnostics); left |
| **Lifted SPU code left (non-audio)** | the water task (9.5% of a pool thread, off the critical path), solver commands 1-2, the inflate's SPU shell; the waver (never dispatched yet) and collision pairs the native code declines (not counted) | each documented with its inputs, outputs, dependencies, synchronisation, steps and tests in docs/SPU_NON_AUDIO.md: solver commands 1-2 first (the only ones the game thread waits for), then the inflate's shell (load time), then the water (architecture); with FMOD they are what holds the reservation test |
| **Development code in the release** | the test harness's threads (dev keys, state, stop) and R4-R7's verify modes compiled in, inert in play | trim |
| **Known gaps, found and left** | the bare loads on Linux not established (checked there). The two store gaps (a misaligned store across a line; a raw store into the first page) are fixed in source, not yet built or run (section 5.4.1) | a change of its own if a title needs it |
| **Audio** | FMOD's mixer, codecs and DSPs lifted SPU code: the SPU context, channels, DMA, reservations, sys_event | the audio milestone |
| **R7's open items** | the post-boss warp run (the owner's optional session), occlusion against RPCS3, the depth clamp, the dual run's stall | unchanged |

### 9.0 Register promotion across calls: what the profile says (2026-11-10) [M]

rel13 profiled as rel6 was (build/r8/prof13_run.sh: DIAG_PROF, uncapped; tests/prof_report.py). The
main thread's own time is spread thin: in the fields func_000A7C78 9.3%, func_000A2834 5.8%, then
twelve functions of 1.4-3.2%; in the river fight none above 3.3% (func_000A7C78 and the recorder's
acquire 3.3% each, ppfx_rec_observe 2.8%). The recorder's lock is gone from the profile (step 1).
Promotion at every call measured worse before (section 5.6: +38% code); a liveness-based promotion
would have to apply across many functions to pay, not one hot spot. Next, as an experiment of its own:
promotion with call-site liveness for the ten hottest non-leaf functions of these two profiles (about
a quarter of the thread's own time in the fields), measured against rel13 in both scenes; adopted only
if it is faster beyond the runs' spread. Not started.

### 9.1 Queued until testing resumes

Work done on 2026-11-07 while the owner's PC was in use for a meeting: no
game started, no window, no benchmark, no large build. What it still needs
is below, in the order to run it. **Nothing in sections 5.4.1, 7.2, 8.4 or
9.2 counts as validated before its row here has run.** The batch scripts
are in build/r8 (local). Each runs through tools/owned_run.py, so that
stopping it stops all of it (section 9.2).

| # | Run | What it does | For | Needs |
|---|---|---|---|---|
| 0 | a 30 s river smoke through `tools/owned_run.py`, stopped on purpose mid-run with the task's stop, then started again | the first game batch under the job object: the game ends with the stop, nothing is left running, the restart is clean | 9.2 | testing resumed |
| 1 | `owned_run.py --name resume1 -- sh build/r8/resume1_build.sh` | the lift build/r8/gen7 (ps3recomp 0125); the release build/r8/rel9 and its comparison build chk9 (checked loads, **checked stores**); `--memtest` on both (the two new store groups and the 614 load checks); the player's prebuilt parts in the release configuration; the static pipeline tests again | 5.4.1, 7.2 | heavy: the lift and three builds |
| 2 | `owned_run.py --name resume2 -- sh build/r8/resume2_install.sh` | tests/player_install_test.py: a fresh install from the disc image into F:, its checks (the lift file for file against gen7), an update with nothing changed, a relift, the settings pass; then `--memtest` and a river fight from the install as a player starts it | 7.2 | heavy (a full lift and compile); 1 |
| 3 | `owned_run.py --name val4 -- sh build/r8/val4_run.sh` | rel9 validated as rel7 was (the eight scenarios, synchronisation validation, the capture check, both stops, timing at 60 fps and uncapped, normal damage three times); then the recorder's step 0 (calls by address a flip; the other-thread counters) in the fight and the fields | 5.4.1, 8.4 | window, real audio; 1 |
| 4 | `owned_run.py --name ab4 -- sh build/r8/ab4_run.sh` | uncapped, real audio, two rounds rotated: rel8 against rel9 (the cost of 0125's two tests) against the player's own build (the x86-64-v2 prebuilt, diagnostics off) | 5.4.1, 7.2 | window, real audio; 1, 2 |
| 5 | not yet scripted: code first | the SPU captures docs/SPU_NON_AUDIO.md lists: the solver's set-up snapshots (DOTHEROES_SOLVER_SNAP extended), the water task's DMA and atomics, counters for the collision tasks phys_direct declines and for the waver's dispatches | SPU_NON_AUDIO.md | the counters and snapshot code written and built |

**Reconciled with what has run** (2026-11-09, night):

| # | State | Evidence |
|---|---|---|
| 0 | **done** in use: every game batch since 11-09 ran through owned_run, and three were stopped mid-run (gate1, gate2: "all 12 stopped"; the RPCS3 boot rpe: "all 4 stopped") with nothing left behind | build/r8/gate1.out, gate2.out, rpe.out |
| 1 | **done**: build/r8/gen7 (the lift with every hook of config/r8_hooks.json, the boss's included, and 0125's lifter half), rel9 and chk9 built and rebuilt with the night's sources; `--memtest` **695 checks, 0 failed on both** (rel8t, on gen6's header, failed the 12 raw-store checks); the player's prebuilt release parts; tests/test_player_pipeline.py 11/11 | build/r8/resume1.out, rel9b.out, rel9/memtest.txt, chk9/memtest.txt |
| 2 | running (queue9): the fresh install and the update | section 9.8 when done |
| 3 | running (queue9): val9 = val4 on rel9, plus the five reference scenes, the save list's two drives and the body set-up's check | section 9.8 when done |
| 4 | running (queue9): ab9 = ab4 with rel8u (the play build) for rel8 | section 9.8 when done |
| 5 | the solver's set-up: four steps were already captured (F:/3d dot game heroes/archive/build/snap, sv3_1/2 and sv4_150/300: c1r1, c2r1, c4r1 each), enough to begin; command 1 native, matching all four byte for byte (docs/SPU_NON_AUDIO.md); command 2 mapped, not written; the water's and the inflate's captures not started | tests/native/bullet_solver_snapcheck.cpp setup-bodies |

After 1-4 the record gets each result in its section. rel9 replaces rel8 as
R8's release only if 3 passes and 4 shows no cost beyond the runs' spread.
Otherwise 0125's inline test gets the lifter's alignment exemption (section
5.4.1) and is measured again. Pushing the commits made during the meeting
waits on the owner. The archived builds on F: stay until the current build
is confirmed to work well (the owner's rule).

### 9.2 Stopping a batch stops what it started (2026-11-07) [C, M]

**The defect.** Stopping a background batch, as part 2 had to once, ended
only the outermost Git Bash. Its inner shells, `timeout`,
tests/boot_smoke.py, tests/drive.py and the game ran on. One batch carried
on into its next stages beside two builds, and its runs landed in
directories the next batch reused. Cleaning up meant finding the processes
by command line and stopping them by hand.

**The fix: tools/owned_run.py** (implemented and tested with dummy
processes; no game run uses it yet). It runs a batch inside a Windows job
object of its own. Every process the batch starts belongs to the job from
its first instruction: the launcher joins the job before it starts the
command. So which processes the batch owns is the operating system's
record, not a search by name. The batch ends, with every process in the job,
when:
- `owned_run.py --cancel NAME` is run from any shell (the job is named);
- any shell or Python process the launcher was started under goes away, up
  to the first other process (all of them are watched);
- the launcher itself is killed (the job kills its processes when the
  launcher's handle closes).

When the command ends by itself, whatever it left running in the job is
stopped and named, and the launcher exits with the command's code. A process
outside the job is never touched: the owner's game, another batch, anything
started another way. `--list` shows each batch's processes and clears the
records of launchers that were killed.

**Tests** (tests/test_owned_run.py with tests/owned_dummy.py: dummy shells,
`sleep`, Python processes and their children; 7 tests, 11.5 s, all pass):
- the exit code passed on;
- leftovers stopped when the command ends;
- `--cancel` ends a tree of 5 or more processes, and a process started
  beside it survives;
- the launching bash killed, both with the launcher as one of its commands
  and as its only command;
- the launcher killed;
- a second batch of the same name refused, the first undisturbed.

**One end-to-end check with this harness's own stop**: a dummy batch of 8
processes (shells, `sleep`, Python and its child) run as a background task,
and the task stopped. All 8 ended at once.

**Not yet used for a game batch**: the first batch after testing resumes
runs through it (section 9.1). To launch one:
`build/venv/Scripts/python.exe tools/owned_run.py --name <batch> -- sh build/r8/<batch>_run.sh`.

### 9.3 The owner's play session and the RPCS3 comparison (2026-11-07, 11-09) [C, M]

After the meeting the owner played rel8b (section 9.1's queue still waits).
Three things came of it.

**The edited hero (the Link model).** The owner's downloaded edit saves
(three edited characters made on a real PS3: GreenHero, BlueRoyal, RedMage)
were decrypted with tools/edit_import.py and added to the play saves. The
game lists them and loads GreenHero, and it draws in full: in the first
dungeon from a SAVES run, and in the owner's own 07-area7 checkpoint
[M: runs ed3, ow7]. Two harness defects had made it look broken:
- a run's SAVES copy took only the BLUS30490 folders, but the game names edit
  data with the Japanese release's ID (BLJM60180...) in every region, so a
  test found no edit data at all;
- a play session's checkpoint copied only the save that changed, so a
  checkpoint whose hero is an edited one loaded with no body (only the
  shield).

Both now carry the BLJM60180 folders (tests/boot_smoke.py, tests/drive.py);
owner-play-r8's checkpoints 05-08 had them added. **Left:** this port has no
save-data list dialog: with several edits the list call loads the first
(GreenHero), where the PS3 asks. The PC-experience work needs one
(docs/PC_EXPERIENCE.md section 7a).

**The RPCS3 comparison.** The owner compared three scenes with RPCS3: the
king's close-up looked sharper in ours; the castle hall's floor reflection
lacked the ceiling's tiles; in the first dungeon the hero "sparkled like a
disco ball". RPCS3 was then driven through the same save for a reference at
the same spot. That needed a scratch copy of RPCS3 on F: with the keyboard as
its pad (the owner's per-game input profile is the Xbox pad; their config and
saves were not edited) and a game window smaller than the screen: a window
the size of the screen is presented full screen, and grabs of it show an old
frame (tests/rpcs3_capture.py).

*The sparkle* (first dungeon, second room, lamp-lit). The same save, the
Assassin preset, the room's south door [M: RPCS3 burst; ours sp8]:

| | the hood (median RGB) | stars |
|---|---|---|
| RPCS3 | 16, 12, 12 | none (one pink frame: the hit flash) |
| ours | 42, 23, 43 | in every one of 16 grabs: red, blue, white |
| ours, DOTHEROES_PPFX_NONNEG=1 | 19, 16, 18 | none in 16; none next to the lamp in 7 |

How it was found, each step a run:
- the first room matches RPCS3 exactly (the hero, its dotted bevel pattern);
- the frame described (DOTHEROES_ASP_DESCRIBE_FRAME=trigger): the hero is one
  non-indexed draw of 5,424 vertices whose lighting program adds two lamps
  (colour 100, 24.6, 0 over 5.62 x distance + 4, flickering) and a
  directional light, with a bevel normal map;
- outputs over 60000 or not finite, marked: none. The specular term
  removed, the lamps' flame sprites skipped: the stars stay. The colour
  texture shown raw: correct;
- every scene output clamped at 0: the stars go. Outputs below -0.25, marked:
  none. So small negative values, from each lamp's unclamped N.L on faces
  turned from it, reach YEBIS's bright pass, which divides colour by
  luminance: where the channels nearly cancel, the quotient runs to the
  pass's 64000 cap and the glare draws stars from it.

**Why RPCS3 has no such values is not established** (since established:
section 9.4, the sources' precision modifiers). Checked equal to what
the game gives: the program and its inline constants, the vertex constants,
the colour texture and the normal map (decoded from guest memory), the
vertex buffer (every frame vector an exact unit axis, the uvs 0 and 1), the
precision bits against RPCS3's rules, the post-effects' control words (they
export h0, as written). RPCS3 clamps neither fragment outputs nor sampled
texels. Ruled out on the way: the LOD bias (the game binds none), anisotropy
(1 to 1 on the hero's units), the placeholder for disabled units (no such
draw), the fragment precision clamp (added anyway, ps3recomp 0128).

**What changed** [C, M]: DOTHEROES_PPFX_NONNEG=1 writes an unblended scene
draw's negative colour components into a floating-point target as 0
(blended draws untouched: one may subtract on purpose). Off by default; the
owner's play session (rel8g) turns it on. Run only in the first dungeon so
far. Diagnostics added for the next comparison (off unless asked): the frame
description on a trigger with programs, constants, textures and vertices
written out; skipping draws by program; marking hot outputs; showing a
unit's texture; substituting shader text.

*The castle floor.* The owner's view is the throne-room corridor (area 82),
not the bright entrance hall: the owner's session record (11:10) gives the
way in -- the entrance hall's altar at x 40.2, north -- and the walk up the
corridor at x 12-15. Standing at (12.7, -38.3) gives the owner's camera (the
wall-tops at the same height) [M: runs c82f0, c82f1]:
- with the depth clamp off, ours is close to RPCS3 (mean difference 5.8 a
  channel). The owner's point holds there: left of the carpet RPCS3's floor
  shows a darker, teal-tinted band with fine tile lines (the mirrored
  ceiling) where ours is plain lighter grey;
- with DOTHEROES_PPFX_DEPTH_CLAMP=1 the band is still missing and the
  reflection gains light stepped blocks at the upper right that RPCS3 does
  not show (difference 13.6). **The depth clamp is not the fix**; it stays
  off. (Superseded, section 9.4: this camera did not match RPCS3's, and the
  blocks came from a second problem the clamp exposed, the float depth
  surfaces' polygon offset. At RPCS3's own camera the clamp is the fix.)

No draw was refused in the scene (the run's 7 unsupported records are all
in frame 0), so the band is a difference in how a draw renders, not a missing
draw. **Open:** the reflection pass's draws (the 1024x512 R5G6B5 target the
floors sample) described at this spot, next to RPCS3's frame from the same
camera.

*The king.* The dialogue close-up reproduces exactly: talking to the king
at (12.0, -19.9) gives the owner's RPCS3 frame ("King: That is...", the same
scripted camera) [M: run king2]. RPCS3 shows a soft glow round the light
cubes, spilling into the red behind them; ours has crisp edges and little
glow (edge energy on the king 18.1 against 13.4 -- biased, since the RPCS3
image went through the owner's window scaling and WebP; the glow is not an
artefact of that). So "sharper" is **the bloom weaker in ours**, not texture
filtering. **Open:** YEBIS's bloom levels compared with a frame of RPCS3's own
at this moment (the scratch RPCS3 can be driven there from castle-front).
(Superseded, section 9.4: at the PS3's 720p RPCS3's frame and ours are the
same at the tested close-up, glow included; what made the owner's screenshot
pair differ is not established.)

**What is reviewed, implemented, built and runtime-tested**:
- the harness fixes and the GreenHero edit: implemented, run (ed3, ow7);
- NONNEG: implemented, built (rel8g), run in the first dungeon (door, lamp,
  two rooms); the owner's play session uses it; not yet run elsewhere;
- the fragment precision clamp (0128): built, in about twenty comparison
  runs with no visible change; no validation of its own;
- the depth clamp and anisotropy switches: built; the depth clamp run in
  the corridor and rejected there; anisotropy not exercised (the scenes seen
  ask 1 to 1);
- rel8g: built and run in the dungeon and the castle; the section 9.1 queue
  (resume1-4) still not run.

A scratch RPCS3 (F:/3d dot game heroes/rpcs3-scratch: the keyboard as its
pad, a 1280x720 window, copies of the test saves) and its key sender and
fast grabber (build/compare/rpcs3_keys.py, rpcs3_burst.py) are local. One
control boot of the owner's own RPCS3 (the scratch config passed with
--config) replaced its current log file (RPCS3 rotates it each boot); its
configs and saves were not touched.

### 9.4 The three differences traced to their causes (2026-11-09) [C, M]

The owner's instruction: fix the causes, not the look; trace where our
rendering first diverges from RPCS3 at matched scenes; check each fix against
the other scenes. Section 9.3's "not established", "ruled out" and "weaker
bloom" are corrected here.

**Method.** RPCS3 (the scratch copy, scale 100, Write Color Buffers and Write
Depth Buffer on) is driven to the scene with posted keys; its GDB server then
reads the render targets from RSX local memory (0xC0000000 + the offset;
fp16 big-endian, ARGB8, R5G6B5, D24S8), the camera target (0x029B0E18,
0x029B0E20) and the area. One read a session: the server pauses the game and
does not resume it. Ours: the frame description on a trigger now also copies
every colour and depth target at the frame's end (tgt_..., dep_...), so each
target compares directly (build/compare's f16cmp.py, depcmp.py, reflcmp.py,
finalcmp.py, local). Where walking stalls short of RPCS3's camera, a drive
script places the hero (`place x z`: DOTHEROES_DEV_STATE's pos.trigger). Each
draw's z min/max control and viewport depth range are in the description.

**The sparkle: the sources' precision modifiers** [C, M]. NV40 gives each
source a precision modifier (SRC1 bits 19-27: half, fixed 12, fixed 9,
saturate); the shared translator ignored them. The first dungeon's lamp
shader reads its N.L sources saturated: unclamped, faces turned from a lamp
went slightly negative, and the bright pass's division by luminance made the
glare's stars of them. ps3recomp 0130 applies them as RPCS3's GetSRC does
(saturate before abs, the other clamps after it, negation last; a half
modifier on a half register and any modifier on COL0, COL1, the facing
register ignored), with src2's abs bit, NaN-safe precision clamps, fp16
rounding of half-precision results and NRM as RPCS3 writes it. Our scene
buffer then has no negative component, as RPCS3's has none, and the hero no
star. Every YEBIS pass computes in half precision and the scene's lighting
programs carry source modifiers, so the fix reaches the whole frame: the
castle's combined buffer is within 0.0007 of RPCS3's (below).
DOTHEROES_PPFX_NONNEG, the workaround that clamped scene colour at 0, is
removed.

**The castle floor: the depth clamp, and the depth format it needs** [C, M].
Every draw is made with z min/max control 0x11 (logged per draw: 129 of 129
in the corridor's frame), near/far culling and zclamp; RPCS3 clamps the depth
for it (VKGSRender's decode_rsx_state: clamped where zclamp is set or culling
is off). At RPCS3's own camera in the corridor's entry hall (14.4, -47.2):

| ours against RPCS3 (mean difference, of 255) | the floor reflection | the final frame |
|---|---|---|
| clipped (before) | 4.41 | 1.84 |
| clamped | 1.73 | 0.97 |

The stair walls RPCS3's floor mirrors are back. Section 9.3 rejected the
clamp at a camera that was not RPCS3's, and in the corridor the clamp also
lost the pillars' shadow. That second effect is ours alone: RPCS3's shadow
map (D24, read at the king) equals our clipped one row for row, while ours
clamped pushed the floor away (0.546 -> 0.994 at one texel). The cause is
the host depth format. This renderer kept the old engine's D32_SFLOAT_S8;
Vulkan's constant depth bias on a float surface is r = 2^(e - 23), e the
largest depth exponent in the primitive, and the shadow pass's floor
triangles, no longer clipped at the near plane, reach depths far past 1, so
their polygon offset grew to push them behind the walls. With
D24_UNORM_S8_UINT, the guest's own Z24S8 (one 2^-24 step a unit, as the RSX
and RPCS3), the clamped shadow map matches RPCS3's within 6e-7 (10 steps of
2^-24, the same offset everywhere) and the pillars keep their shadow. Both are
now the default (DOTHEROES_PPFX_DEPTH_CLAMP=0 clips as before, =2 clamps every
draw; DOTHEROES_PPFX_DEPTH_D32=1 the float surfaces; a device without D24 or
depthClamp falls back and says so in the report). Checked in the other scenes:

| ours (D24) against RPCS3, same camera | clipped | clamped |
|---|---|---|
| corridor (14.4, -29.52), hero placed: final / reflection | 1.12 / 2.5 | 1.19 / 2.6 |
| first dungeon's second room (89.6, -104.0): combined fp16 / final | 0.035 / 2.9 | 0.025 / 2.2 |
| the king's close-up: combined fp16 | 0.0007 | 0.0007 |

The corridor is the same either way (its residual: the reflection target is
R5G6B5 in RPCS3 and RGBA8 here, below); the dungeon's enemies move between
runs.

**The king: the tested close-up matches; the earlier screenshot's
difference is unexplained** [M]. RPCS3's own frame at the close-up (scale
100): its bright pass, bloom, glare and star targets are all but empty (max
0.03) and equal ours exactly; its combined buffer was within 0.0007 of ours
(the difference on the far floor's window-light patches, 1-2%; section 9.5
finds their cause). Its presented frame against our final (rel8n, 144 fps,
the owner's settings): a mean difference of 0.47 of 255, 0.09 on the king --
the king's gesture and the dialogue's typing are the rest. The soft glow
round the crown is in both. Edge energy on the king: RPCS3 at 720p 17.78,
ours 17.78. The owner's RPCS3 screenshot (13.4) came from RPCS3 at
Resolution Scale 150 in a 1913x1066 window, and the "ours" it was set
against was a screen grab of a rel8g-era run (18.1). **What made that pair
differ is not established**: RPCS3 at 150% in a 1280x720 window, grabbed 10
s into the dialogue, looked like it does at 100% (mean difference 2.2 of
255), so the scaling test did not reproduce the softer look; the owner's
window size, the moment of the screenshot, WebP compression and its resize
are all untested candidates, and none is claimed. Ruled out for the
close-up tested here: 0130 (rel8x, rel8n without it: the same glow, the
crown's difference the king's pose); exposure adaptation (the bright pass's
exposure an inline 3.0 in every frame seen; a series 1-11 s into the
dialogue at 30 and 60 fps shows no transient).

**Left at rel8o** [M] (all three traced in section 9.5): the floor's
window-light patches at the king (1-2% of their value); the reflection
target, R5G6B5 on the RSX, held as RGBA8 (surface_vkformat), so RPCS3's
reflection is quantised where ours was not (mean 1.9 of 255 after quantising
ours); the D24 shadow map's constant 10-step offset against RPCS3's.

**Builds and checks.** rel8o (build/r8/rel8o_build.sh): 0130, the D24
surfaces, the clamp by default, NONNEG removed, the placement trigger, and
the z min/max control's reset value 0x1 (RPCS3's; it was 0, which would
clamp the frames before the game's first reset). rel8n is the same without
that reset value; the diagnostic builds rel8h-rel8m and rel8x (rel8n's tree
without 0130) are local.

*Validation* [M] (build/r8/val5_run.sh, rel8o): the eight scenarios with
grabs (river, menus, fields, dungeon, snapshot, after the dungeon, editor,
the exit after the boss) all completed (exit 0, no drive failure but the
river's timed end), each report "render: depth surfaces as D24_UNORM_S8" and
the 7 unsupported records of frame 0 as before; Khronos validation with
synchronisation validation in the river fight and the dungeon approach: no
error, no hazard (only the known warning for fragment outputs no attachment
takes); the capture check and a normal-damage river fight completed. With
no switch set, rel8o at RPCS3's entry-hall camera gives the clamped figures
above (reflection 1.73, frame 0.97). The CPU-side checks (lost work, the
interpreter stops, gameplay timing) were not repeated: nothing on the CPU
changed. The screen grabs carry a Windows Firewall prompt left open over
their centre (the Nsight capture tool's, not answered: a security setting);
the target dumps above are unaffected.

*Performance* [M] (build/r8/bench4_run.sh, bench4b_run.sh; tools/r8_bench.py):
rel8o against rel8g (the owner's play build before it, NONNEG on) and rel8o
with its depth changes switched back (DOTHEROES_PPFX_DEPTH_D32=1
DOTHEROES_PPFX_DEPTH_CLAMP=0), uncapped, real audio, rotated order; four
river rounds for rel8o and rel8g, two for the third arm, two fields rounds
each.

| Arm | river fps | p99 ms | busy ms | fields fps | p99 ms | busy ms | GPU time, river run |
|---|---|---|---|---|---|---|---|
| rel8o | 197.2-203.9 | 6.84-7.37 | 4.84-5.01 | 240.2-242.5 | 5.59-5.74 | 4.09-4.13 | 25.6-25.8 s |
| rel8g | 193.5-204.7 | 7.12-7.25 | 4.83-5.10 | 242.0-245.1 | 5.53-5.73 | 4.05-4.10 | 26.1-26.2 s |
| rel8o, D32, clipped | 197.7-199.5 | 7.16-7.25 | 4.95-5.00 | 239.8-242.6 | 5.68-5.76 | 4.09-4.14 | 26.3-26.4 s |

The river fight is two workloads: a run issues about 23,000 debris tasks or
about 44,700 (the fight's own randomness), and rel8o drew the light one in
three rounds of four. Matched by it: light rounds rel8o 200.6-203.9 fps
against rel8g's one 204.7; heavy rounds rel8o's one 197.2 against
193.5-200.2. The heavy-debris frames' p50 follows the workload, not the
build (7.3-7.7 ms in light rounds for both, 6.6 in heavy ones): no
measurable cost; the D24 surfaces take about 2% less GPU time in the fight.
144 fps sustained exploring as before (99.8% of frames within 6.94 ms); in
combat 97.9-99.2% (rel8g 98.1-98.7%).

### 9.5 The residuals: the reflection's 16 bits, the window light, the bias unit; reference scenes (2026-11-09) [C, M]

**The reflection target at the guest's 16 bits** [C, M]. The castle's floor
reflection is an R5G6B5 surface; this renderer held every colour format but
the floating-point ones as RGBA8 (surface_vkformat), keeping 8 bits a channel
and the alpha the shader wrote, where the RSX (and RPCS3, which keeps
R5G6B5_UNORM_PACK16) stores 5/6/5 bits and reads alpha as 1. Now an R5G6B5
surface is R5G6B5_UNORM_PACK16 (DOTHEROES_PPFX_TARGET_565=0 for RGBA8; a
device lacking the format's attachment, blend and sampling features keeps
RGBA8 and says so); main-memory and transfer write-backs carry the 16-bit
words, big-endian. At RPCS3's cameras, the reflection target itself and the
frame (mean difference of 255):

| | reflection, RGBA8 | reflection, R5G6B5 | frame, RGBA8 | frame, R5G6B5 |
|---|---|---|---|---|
| entry hall (14.4, -47.2) | 1.73, 0.88, 1.89 | 0.18, 0.16, 0.33 | 0.97 (2.4% > 8) | 0.45 (0.8%) |
| corridor (14.4, -29.52), placed | 2.56, 1.18, 2.83 | 0.14, 0.14, 0.14 | 1.19 (4.6%) | 0.33 (1.0%) |
| king's close-up | 2.24, 0.96, 2.18 | 0.005, 0.023, 0.011 | -- | -- |

In the corridor the scene buffer's pixels more than 0.004 from RPCS3's fall
from 30% to 2.5% (the floors add the reflection). What is left at the two
castle cameras is the hero (RPCS3's walked there, ours is placed or walked:
its stance differs) and the guards walking.

**The king's window-light floor: the same cause** [M]. The residual was in
the scene before any post-effect (the first downsample showed it as the
combined buffer did), only on the lit floor patches, and its shape was a
quantiser's: RPCS3 lower in red and blue (ratio 0.94-0.96 at the median),
green often equal -- 5 bits against 6. With the 16-bit target the combined
buffer's pixels more than 0.004 from RPCS3's fall from 47,520 to 1,056,
isolated single pixels (0.1%; values at a rounding boundary of the 5/6/5
steps -- a hypothesis, not traced further), the mean difference from 0.0007
to 0.00002.

**The shadow map's 10 steps: NVIDIA's bias unit** [C, M]. The D24 shadow map
sat a constant 10 steps of 2^-24 deeper than RPCS3's. RPCS3 halves the
constant depth bias on an NVIDIA D24 surface (VKDraw.cpp: NVIDIA's unit,
derived on hardware, is twice the RSX's); this renderer now does the same
(DOTHEROES_PPFX_BIAS_NV=0: not), and the map equals RPCS3's: every texel of
the rows RPCS3's copy holds within two steps, the mean difference 0. Other
vendors keep the specification's unit (not measured: no such device here).

**The first dungeon's door: time, not rendering** [M]. The combined buffer
there differs from RPCS3's by 0.026, 0.013, 0.002 (41% of pixels more than
0.004 apart), red-weighted like the room's lamps (colour 100, 24.6, 0). Two
of our own runs at the same spot differ from each other more (0.036, 0.016,
0.005; 50%) than either from RPCS3: the lamps flicker and the enemies move.
The sparkle's measures hold: no negative component in our scene, the bright
pass as empty as RPCS3's.

**Unobstructed pictures** [C, M]. The validation runs had only screen grabs,
which a window over the game's covers (a firewall prompt sat over every grab
this session; it was not answered: a security setting). The harness's shots
(tests/boot_smoke.py, SHOT_EVERY: shots/NNNN.png) come from the Vulkan
backend's frame dump (PS3RECOMP_VK_DUMP, the presented picture, every 15th
present), which native runs write too; the scenarios turn it off for timing
(DUMP=0 SHOT_EVERY=0 in their headers), so validation now passes DUMP=1
SHOT_EVERY=5. A second dump added to the native present on the belief that
it had none was a mistake and is removed (e43c618). The backend writes the
dump in place, and a copy taken mid-write had loaded as a truncated image
with black rows (4 of 19 shots in one river run); the harness now keeps a
shot only when the copied file holds every pixel, and copies again shortly
when not.

**Reference scenes** [C, M]. tests/rpcs3_reference.py and
tests/rpcs3_reference.json keep the four scenes -- the dungeon's door, the
entry hall, the corridor, the king -- with their drives
(tests/scenarios/rpcs3ref_*.drive), RPCS3's cameras and the targets it wrote
back. `pack` keeps RPCS3's targets (gzipped, 35 MB in all) and the scene's
save in build/reference/rpcs3/ (game output: local, not in git); `run` plays
a scene with a build and checks it; `check` compares a dump with RPCS3's and
with the figures rel8r reached (the 'accepted' figures), failing on anything
worse than 1.5 times them (plus a floor for noise). The builds before the
depth clamp (the entry hall), before the 16-bit reflection (the corridor) and
before the bias unit (the king's shadow map) each fail it.

**Remaining differences.** Confirmed causes, fixed: the sources' precision
modifiers (the sparkle), the depth clamp and the 24-bit depth surfaces (the
reflected stair walls, the pillars' shadow), the R5G6B5 target (the
reflection's quantisation, the king's window-light floor), NVIDIA's bias unit
(the shadow map). Measured and explained, not rendering: the dungeon's
flicker and enemies; the hero's stance and the walking guards at the castle
cameras. Hypotheses, not traced: the king's 1,056 isolated pixels (rounding
at the 5/6/5 boundaries); why the owner's earlier king screenshot pair
differed (section 9.4: not reproduced).

**Builds.** rel8r (build/r8/rel8r_build.sh): rel8o + the R5G6B5 surfaces,
the bias unit and the redundant present dump; **rel8s**, the final build:
rel8r without that dump. The owner's play scenario runs rel8s.

*rel8s against the references* [M] (tests/rpcs3_reference.py run all, each
camera RPCS3's to the hundredth): all four within the accepted figures --
the entry hall's reflection 0.18, 0.16, 0.33 and frame 0.45, 0.43, 0.63 (of
255); the corridor's reflection 0.08 and frame 0.25-0.27; the king's
combined buffer 0.00002, its bloom chain and shadow map equal to RPCS3's;
the dungeon's door with no negative component and the bright pass as empty
as RPCS3's (its frame 2.1, the lamps' flicker).

*Validation* [M] (build/r8/val6_run.sh, rel8s): the eight scenarios (river,
menus, fields, dungeon, snapshot, after the dungeon, editor, the exit after
the boss) completed, exit 0, each with the backend's frame dump every 5 s
(shots/: 19-38 a run, no torn frame; looked through: the field, the river
fight, the menus, the dungeon's rooms, the snapshot camera, the editor, the
boss's exit -- nothing wrong to the eye) beside the screen grabs; their only
drive "failures" the timed ends (river, after the dungeon), as in val5. Each
report: "depth surfaces as D24_UNORM_S8" and frame 0's 7 unsupported
records; the editor 1,858 -- line primitives the renderer does not rebuild,
1,852 on rel8o too: not this work's. Khronos validation with synchronisation
validation (the river fight, the dungeon approach): no error, no hazard, the
known unconsumed-output warning only. The capture check (records 393,374;
none written between the flush and the capture) and a normal-damage river
fight (life 10 -> 7) completed.

*Performance* [M] (val6_run.sh's bench5, tools/r8_bench.py): rel8s against
rel8g (the owner's build before this work, NONNEG on) and rel8o, uncapped
(benchmark pacing), real audio, no frame dump, three rounds each in rotated
order.

| Arm | river fps | p99 ms | over 6.94 ms | busy ms | fields fps | p99 ms | busy ms | GPU, river run |
|---|---|---|---|---|---|---|---|---|
| **rel8s** | 202.3-203.3 | 6.90-7.00 | 0.9-1.1% | 4.85-4.88 | 237.8-243.5 | 5.55-5.74 | 4.08-4.17 | 25.7-25.8 s |
| rel8g | 199.8-204.5 | 6.90-7.36 | 0.9-1.9% | 4.83-4.94 | 240.1-243.2 | 5.64-5.75 | 4.08-4.13 | 26.1-26.3 s |
| rel8o | 196.4-202.7 | 6.74-7.21 | 0.6-1.6% | 4.87-5.03 | 239.9-242.6 | 5.76-6.00 | 4.09-4.14 | 25.7-25.8 s |

rel8s's three fights all drew the light debris workload (23,000-27,000
tasks; rel8g's two of three the heavy one, 41,000-45,000): matched, the
light rounds give rel8s 202.3-203.3 fps against rel8g's 204.5 and rel8o's
200.6-202.7 -- within the rounds' spread. No measurable cost; the fight's GPU
time about 2% lower than rel8g's (the D24 surfaces). 144 fps sustained
exploring (99.8-99.9% of frames within 6.94 ms) and 98.9-99.1% in the
fight.

### 9.6 The first dungeon's boss above 30 fps; the save-data list (2026-11-09) [M]

The owner's report from a session at 144 fps: the boss was too fast.

**How it was measured.** A new log (DOTHEROES_DEV_FIELD_LOG, development
builds: chosen words of every object of chosen classes, on change, 2 ms
scan) and tests/boss_timing.py, in real seconds. The fight from the owner's
save, 90 s or more, the hero standing at the door pressing CROSS (the
companion's dialogues stop the fight until they are answered), at 30 fps
(DOTHEROES_FPS=30, the original's pace), 60, and 144 (the owner's setting,
vsynced), with real audio.

**What the boss is.** A head (class 0x00FE4690) and six body segments
(0x00FE4760); the shots are 0x00FE4500 objects. Two things are frame-counted:
- The head's update (func_001E47C4) ignores the frame time it is given and
  calls the actors' common update with a constant 1/30 s. A static scan of
  every class's update found no other that does.
- The segments' update (func_001F2DC0) follows the head's trail through a
  ring of positions a step a frame, counts frames (+0x75C) and compares them
  with (+0x778 + 1) x period for its turns and attacks, and uses +0x75C's low
  bit for an every-other-frame branch. Only its wobble (+0x730) scales by dt.

| | 30 fps | 60 fps | 144 fps |
|---|---|---|---|
| Before: the frame counter (+0x75C) | 29.2/s | 55.5/s | 133.5/s |
| Before: the segments' speed in 0.25 s windows, median (all of the fight's hit levels mixed), units/s | 4.53 | 7.52 | 20.10 |
| Before: shots seen in the fight | 28 | 70 | 70 |
| After: the frame counter | 29.3/s | 29.2/s | 29.4/s |
| After: the update's spacing, median | 33.3 ms | 33.3 ms | 34.2 ms (whole frames of a 144 Hz display) |
| After: the segments' step an update (no hits yet), median | 0.141 | 0.141 | 0.141 |
| After: the animation's toggle (+0x514) | 3.9/s | 3.9/s | 3.9/s |

The speed depends on how often the boss has been hit: each hit adds 0.38 to
its speed multiplier (+0x5C4, from 1.00), so whole runs cannot be compared
(a hit is a matter of where the boss wandered). Per multiplier, two runs
each, the hero on one half apple (no sword beam), the head's speed in 0.25 s windows,
median, units/s:

| Multiplier | 30 fps | 60 fps | 144 fps |
|---|---|---|---|
| 1.00 | 2.80 | 2.80 | 2.80 |
| 1.38 | 4.64 | 3.86 | 3.86 |
| 1.76 | 5.91 | 5.57 | 5.91 |
| 2.14 | 6.54 | 7.15 | 6.54 |
| 2.52 | 7.06 | 8.06 | 7.26 |
| 2.90 | 8.35 | not reached | 9.08 |
| 3.28 | 11.02 | not reached | 10.43 |

(Above 1.00 a multiplier lasted 4-60 s of a run, so its median comes from few windows.)
The turns' spacing at 1.00: 4.86 s (12 turns), 5.10 (15), 7.04 (6); it is
random. Shots in volleys: 7.0, 4.3 and 5.9 volleys a minute of fight; too few, and too
dependent on the hit level, to compare closely, with no trend with the frame rate.

**The fix.** An entry hook on each of the two updates
(config/r8_hooks.json, port/game_patches.cpp tick30_gate): the frame's time
accumulates per object and the update runs only when a 1/30 s step is due,
told that step as its dt. A first version gated the segments alone:
the segments' frame counter was right and the boss was still fast (0.224 a
step, not 0.141), because the ungated head filled their trail; the
measurement caught it, and it led to the head's constant dt.
DOTHEROES_TICK30=0 turns it off, and at DOTHEROES_FPS=30 it is off. Above
30 fps the boss now moves in 30 Hz steps, as on the PS3: smooth interpolation
between them is not done.

**The others** [C for the code, M for the river's enemies].
- Static, every object class's update (slot 8 of 309 vtable-like runs, 206
  distinct functions): only the head passes a constant for the time; only
  the segments' update steps a frame counter on every call. Reading each of
  the other updates and helpers that step a field by one, among them the
  later bosses' classes (0x00FE45C0-0x00FE5940: +0x544, +0x53C, +0x5BC,
  +0x7C8), each step is taken when a float timer expires, or counts an event
  (a hit, a hop): not one a frame. The scan cannot see a float moved by a
  constant each frame.
- In play: the river fight's 30 enemies (class 0x00FE4500, the common
  enemies' class, tests/fps_fields.py) at 30, 60 and 144 fps: speed in 0.25 s windows
  (median) 3.56, 2.67, 3.05 units a second, no trend; the logic fields' change
  rates equal (+0x5C0: 0.26, 0.30, 0.25 a second).
- **Not run**: the later dungeons' bosses (no save reaches them). The code
  says they are time-based; only a play session will show it.

**The save-data list** [M]. cellSaveDataListLoad2 and ListSave2 took the
first entry the game offered. So with two saves CONTINUE could only load the
first, and -- found by this work's own test -- the game's SAVE overwrote the
first entry, whichever save had been loaded: the game asks for the list's head
as the focus and expects the player to choose.
Now both show the system's list as a panel over the game (ps3recomp 0131):
- each entry's PARAM.SFO DETAIL (the game writes the hero's name, the play
  time and the location there) and when it was last written; edit data shows
  its name; the game's sort and focus position honoured (RPCS3's rules);
- Up/Down, Cross (Z, Enter) to confirm, Circle (X) to go back: CELL_CANCEL to
  the game, which goes back to where it was (the title on CONTINUE);
- the save side has the game's New Save Data row, and asks before it
  overwrites a save;
- the game's pad reads released meanwhile, so its own screen does not move.
PS3_SAVEDATA_PICKER=0 restores the first entry (the test harness's default,
so that scripted routes stay as they were; tests/scenarios/save_picker*.drive
turn it on). The panel's font is the on-screen readout's, ASCII upper case;
the entries' ICON0.PNG is not drawn.

Tests on copies of the owner's saves (tests/scenarios/save_picker.drive,
save_picker_edit.drive): two saves, the second picked, and loaded (the save
named by the game's log); saved from the system menu into the same slot, the
other save's file byte-identical; to the title, a cancel, back at PRESS START; the first
picked and loaded. On the character select, the third of three edits picked
and read (its DATA.EDT, the character on the next screen). Saves are read as
before and written only when the player chooses to.

**The build** [M]: build/r8/rel8t (dotheroes.exe sha256 5702298d...; rel8s's
tree with the two entry hooks in the lift's chunk 008 and ps3recomp 0131;
the owner's play session, tests/scenarios/owner_play_r8.drive, runs it with
the list on). Validated as rel8s was (build/r8/val7_run.sh): the two list
drives and the eight scenarios exit 0; Vulkan sync validation (river,
dungeon) and the capture check clean; normal damage; the four RPCS3
reference scenes within the accepted figures (tests/rpcs3_reference.py).
The river fight uncapped with real audio, two rounds each: rel8t
205.5-208.1 fps, rel8s 200.1-202.5 (the fight's usual spread; the gate
runs only in the boss's room). The boss at 144 fps for ten minutes, the hero
in the room's middle: the six segments destroyed (at 42-274 s), the head
then alone at its top multiplier (3.28, 10.4 units a second) and not hit by
a hero standing still -- **a full kill with the gate on is not yet seen**; a
play session will show it.

**One clock, and its checks** (2026-11-09, later) [M]. The gate's time was
per object; now one 30 Hz clock serves every gated object: the frame's time
is added once a game frame (the game's own frame count, 0x02CF3570, stepped
at the frame's start by func_0094F290), and in a frame where a step is due
every gated object takes it, once (a second call in the same frame does
not step again). The head and the segments that follow its trail step in the
same frames whenever each was made, as in the original's single frame; an
object made at an address an old one used inherits nothing. The clock's time
passes only in frames with a gated call. DOTHEROES_TICK30_LOG=1 logs it.
Checks at 144 fps unless said, the hero on one half apple
(build/r8/gatechk_run.sh, build/compare/boss_*.drive):

| Check | What happened |
|---|---|
| Two pauses (the system menu, 13.5 and 12.5 s) | the segments' frame counter stood still through each (one change, at the end); after each, at most one step a frame -- no burst of the time spent paused |
| Room reload (the title, CONTINUE, the boss room again) | new objects at new addresses; the clock picked them up with its 0.003 s carry; 29.97 steps a second after |
| Frame-rate changes (DIAG_VBLANK_SCHEDULE: 30, 144, 60, 100, 40 Hz, 6 s each, round and round, 100 s) | 29.93-30.15 steps a second in every 5 s window; two frames of 34.1-34.6 ms took two steps; the head 2.80 units a second at multiplier 1.00, as at 30 fps |
| The boss's intro | its dialogue stops the walk: 239-345 game frames without a gated call, then steps resume one at a time |
| Speed per multiplier, these runs | 1.00: 2.80 in every run; the higher levels within the spread of section 9.6's table |

DIAG_VBLANK_SCHEDULE=<ms>:<s>,... (port/main.cpp) changes the vblank clock's
period as the game runs, for such tests.

**Status**: pushed as validated development work, not a stable release (the
tags stable-2026-11-02, checkpoint-2026-11-04, r4-checkpoint-2026-11-05 are
unchanged, and none is added). A full boss kill with the fix is unverified
until the owner plays it. Follow-up, not started: smooth visual interpolation
between the boss's 30 Hz steps above 30 fps (the gameplay timing stays as
corrected; interpolation would change only what is drawn between steps).

### 9.7 The character editor's lines (2026-11-09) [M]

Every validation since R7 reported the editor's records the native renderer
skipped: 1,845 a session "asp: a primitive the engine does not rebuild".
They are the block grid's lines -- the editor's three walls and floor, cell by
cell (NV40 LINES and LINE_STRIP, width 1). The renderer rebuilds every
primitive as a triangle list and had no case for lines, so the editor showed
its blocks floating with no grid.

**Now** (port/native/render/ppfx_render.cpp, the scene path): LINES,
LINE_STRIP and LINE_LOOP are drawn as a Vulkan line list (the strip and the
loop unrolled into pairs), width 1 -- the width the game sets (R7 counted
1,854 calls, all width 1; a wider one is still counted as unmodelled). The
pipeline key carries the topology. POINTS stay unsupported, with their own
reason in the report ("asp: points"); none has been seen. The report counts
the line draws ("native asp line draws").

**Against RPCS3** (the scratch copy, scale 100, display buffer 00000000 read
over GDB; ours dumped at the same screen by the same button presses): the
frame's mean difference 0.012-0.018 of 255 and 378 pixels over 8, all of them
the block cursor's blinking dashed outline and its shadow; the grid's line
pixels (26,159 in RPCS3's frame) present in ours at the same places, none
extra. Without lines (rel8t) the same frame differed in 25,804 pixels. The
editor is now a fifth reference scene (tests/rpcs3_reference.json "editor",
tests/scenarios/rpcs3ref_editor.drive; RPCS3's frame packed locally in
build/reference/rpcs3/editor): accepted from bt8, and rel8t fails it.


### 9.8 The store fixes built, priced and made cheap; the player's install (2026-11-09) [M]

**Built and checked.** build/r8/gen7 (r8_lift.sh: every hook in config/r8_hooks.json, the boss's
included) with 0125's lifter half; rel9 (release) and chk9 (checked loads and stores): `--memtest`
695 checks, 0 failed on both -- the raw-store groups that failed on gen6's header now pass. rel9
validated as rel7 was (build/r8/val9_run.sh): the eight scenarios, sync validation, the capture
check, the lost-work and interpreter stops (the interpreter's: "spu-interpreter-absent", fatal as
designed), three normal-damage fights, the five reference scenes, the save list's two drives.

**Priced** (uncapped, real audio, two rounds rotated; tools/r8_bench.py). rel9 against the play
build rel8u (gen6's lift, otherwise the same code): the fields 228.4-230.3 fps against 247.6-248.7,
the game thread's busy time 4.31-4.35 ms against 3.99-4.01; the river fight 178-195 against 201-207.
The player's own build of rel9 measured as rel9 (227.3-229.7 in the fields). Two measurement lifts
told the parts apart (build/r8/m9_run.sh, the fields):

| Build | fps | busy ms |
|---|---|---|
| rel8u (no new test) | 243.1-247.7 | 4.01-4.08 |
| rel9 (both new tests) | 228.6-233.2 | 4.26-4.34 |
| without the inline line-crossing test | 245.9-249.2 | 3.98-4.04 |
| without the raw stores' range test | 226.1-231.0 | 4.30-4.39 |

The whole cost was the inline test of a store's end -- an and, a compare and a branch on every 2-,
4- and 8-byte store. The raw stores' range test costs nothing measurable.

**Made cheap, still correct** (ps3recomp spu_coherency.c, ppu_lifter.py). The reservation bitmap's
bits are only ever set; now a reservation also sets the bit of the line before it. The inline store
tests its first line's bit alone, as before 0125, and a misaligned store that runs into a reserved
line finds its first line's bit set and goes out of line, where VM_WRITE_COH tests and tells both
lines. The bitmap is a superset: a store to the line before a reserved one takes the out-of-line path
when it need not -- slower, never wrong. tests/native/store_paths.py (70 checks a variant, both pass):
nothing reserved, every store at a line's end inline; the next line reserved, every store starting in
the line out of line. rel10 (lift build/r8/gen8): `--memtest` 695, 0 failed ("stores at a line's end:
the next line told exactly when the store reaches it"); the fields 237.0-239.5 fps, busy 4.14-4.19 ms
(rel8u 243.1-245.1, 4.05-4.08; rel9 229.1-233.5, 4.25-4.33); the river fight 203.4-205.0 against
rel8u's 200.8-205.7, heavy-debris frames p50 7.56 against 6.66-7.66 -- within the spread; six
scenarios pass. What is left (~0.1 ms in the fields) is the out-of-line path taken for lines before
reserved ones; rel10b keeps an exact bitmap beside the wide one for the out-of-line test, so those
stores take no lock. Measured again in one session (build/r8/rel10b_run.sh, the fields, two rounds
rotated): rel10b 244.0-244.6 fps, busy 4.06-4.07 ms; rel8u 244.3-244.6, 4.06; rel10 243.0-245.6,
4.04-4.08 -- all three within one spread, so rel10's lower figure above was the session's, not the
build's. **The store fixes cost nothing measurable now** (ps3recomp 0133); rel10b keeps the exact
bitmap (it spares those stores the lock-line lock), `--memtest` 695, 0 failed.

**The player's install** (tests/player_install_test.py, build/r8/resume2_install.sh, into
F:/3d dot game heroes/install-test): a fresh install from the disc image -- its prebuilt parts the
native release, nothing reused, the lift's stamps, leaf promotion (8,395 functions), R7's 162 entry
hooks and R8's physics hooks, **the lift equal to the developers' build/r8/gen7 file for file**, the
settings file with the performance switches; an update (only the build ran, every object reused); a
re-lift (the SPU step skipped, every object reused); the settings (the player's values kept, a missing
performance switch added back); the installed game's `--memtest` exit 0. The installed executable
carries the save list ("list dialog"), the boss gate ("frame-counted boss updates"), the editor's lines
and the SPU census. The last stage, a river fight from the install, failed in the harness, not the game:
boot_smoke.py took the install's path relative to the repository, which Windows cannot do across drives
(fixed, 8f5b02d); that stage runs again with the next build. The player's saves and edited characters
are not touched by an install or an update (the install writes its own directory; the game data root
is the player's, %LOCALAPPDATA%\dotheroes, never written by the tests).


## 10. Reproducing

```
tools/r8_lift.sh                        (R8's lift: R7's hooks and R8's (config/r8_hooks.json), R8's lifter, into build/r8/gen;
                                         R8_GEN=<dir> elsewhere: part 2's are build/r8/gen2..gen5)
tools/r8_release_build.sh build/r8/rel7 -DRECOMP_DIR="$(pwd)/build/r8/gen6"   (R8 part 2's release: bare loads, mov stores, leaf
                                                              promotion (tools/r8_lift.sh's default since gen6), -O1)
tools/r8_release_build.sh <dir> [-DNAME=value ...]            (a variant: -DDOTHEROES_PPU_CHECKED_LOADS=ON (the comparison build),
                                                              -DDOTHEROES_PPU_STORES=checked|mov|nocoh-measure|bare-measure,
                                                              -DCMAKE_CXX_FLAGS=-DPPU_RAW_ST_NO_COH, -DDOTHEROES_PPU_OPT=-O2);
                                                              R8_LIFT_FLAGS= tools/r8_lift.sh lifts without leaf promotion
build/r8/<dir>/dotheroes.exe --memtest                       (the bare loads' assumptions, tested: section 5.3)
tools/r8_scene.sh <tag> <scenario> <secs> [NAME=value ...]   (a run into build/r8/<tag>; R8_BIN picks the binary, default build/r8/base;
                                                              later NAME=value pairs win: DOTHEROES_DEV_CANT_DIE=0 for normal damage)
tools/r8_bench.py <prefix> [--arms a,b] [--bodies N]         (a batch's runs by arm: frame times, 144/200 fps budgets, busy, heavy debris)
tools/r8_phys.py <run dir> ...                               (the physics tasks a frame: latency, run time, waits -- polling apart from
                                                              the jobs the waiting thread runs)
tools/r8_threads.py <profile.raw> [--fps F] [--thread NAME] [--under FUNCTION]   (per-thread work and waits, by category)
tools/r8_split.py <profile.raw> <spans> <ftrace.txt>          (the main thread in two sets of frames, e.g. heavy debris against the rest)
tools/r7_measure.py, tools/r7_grabs.py, tests/fps_check.py, tests/actor_steps.py   (as in R7)
tools/owned_run.py --name <batch> -- sh build/r8/<batch>.sh   (a batch in a job object of its own: --cancel <batch>,
                                                              --list; section 9.2)
build/venv/Scripts/python.exe tests/test_player_pipeline.py    (the player's build path against the release's: section 7.2)
build/venv/Scripts/python.exe tests/player_install_test.py     (a fresh install and its updates into F:; `game --run-game`
                                                              starts the game: section 7.2)
build/venv/Scripts/python.exe tests/native/store_paths.py      (the generated stores, outside the game: section 5.4.1)
build/venv/Scripts/python.exe tests/test_owned_run.py          (batch cancellation with dummy processes: section 9.2)
```

The batches, kept in build/r8 with their runs: prof1 (the inventory's
profiles), ab1 (-O2), hw1/hw2 (the frame counters' readers), sweep1 (the
timing sweep, the debris profile, the lost-work tests), ab2 (bare loads,
the call-site cache), val1 and dmg2 (the release's validation), bench1
(the measurement); part 2: ab3 (the stores), pv1 (the direct physics
issue's validation), pb1 (its measurement), vc1/vc2 (the raw stores'
reservation test), val2 (rel6's validation), bench3/bench3b (the
measurement), prof2 (the profiles), lp1 (leaf promotion), val3 (rel7's
validation). On 2026-11-07, with the system drive full, the runs, the
variant builds and the earlier lifts went to an archive on another drive
(and the window grabs were deleted); build/r8 keeps base, rel1, rel2, rel6,
rel7 (with its build tree), rel8, chk6 (binaries) and gen5, gen6 (the lifts). Each binary measured is kept beside its
build with SHA256SUMS (build/r8/rel2..rel8, chk3/chk5/chk6, stA, stX, stB,
rel5x; the variants archived).

Switches (development): DOTHEROES_DEV_HWWATCH=<ea>[:w][,...]@<start s>,<secs>
(up to four words watched with the debug registers; hwwatch.txt),
DOTHEROES_R8_LOST_TEST=<frame>, DOTHEROES_R8_INTERP_TEST=1,
DOTHEROES_MEASURE_ONLY=1 (a measurement build's permission to start),
DOTHEROES_PHYS_DIRECT=0 (the physics tasks through SPURS),
DOTHEROES_PHYS_DIRECT_WORKERS=<n>; the build options DOTHEROES_PPU_OPT,
DOTHEROES_PPU_CHECKED_LOADS, DOTHEROES_PPU_STORES,
DOTHEROES_SPU_INTERPRETER (development only). Engine changes:
patches/ps3recomp/0117 (PAD_FILE holds), 0118 (the call-site cache,
PPU_BARE_LOADS), 0119 (no SPU interpreter; ps3_fatal), 0120 (the bare loads
defined and gated, the stores' variants, entry hooks that may decline), 0121
and 0122 (the raw stores' reservation test), 0123 (--promote-leaf), 0124 (the release draws without DOTHEROES_PPFX_NATIVE), 0125 (a misaligned store tells both lines; the raw stores keep the scalar stores' range).
