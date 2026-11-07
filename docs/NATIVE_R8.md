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
| Inventory: threads, SPU workloads, where the time goes | done (section 1): the main PPU thread is the critical path, alone |
| The accepted baseline; config/insn_patches.json | done: the file is the committed one, byte for byte (section 2) |
| Frame-counted values | done: none is a clock or moves gameplay (section 3) |
| The shorter uncapped walk | done: the test harness's input timing; fixed (ps3recomp 0117) |
| Unimplemented graphics calls | done: named, on screen, counted, fatal under strict (section 4) |
| The lifted code: -O2 | measured: no gain, not adopted (section 5) |
| The lifted code: the loads' tests | **removed in the native release: the game thread 1.1-1.2 ms faster** (section 5) |
| The indirect calls | a call-site cache (ps3recomp 0118): +1.5-3% (section 5.1) |
| The R8 release | validated: 8 scenarios' pictures, sync validation, capture check, gameplay timing at 60 and 200 fps, normal damage (sections 5.2, 7) |
| Heavy debris | profiled: the game thread's own physics, the extra draws, the SPURS handshake (section 6) |
| Measurement against R7, R6, production | done (section 8): combat 187-190 fps, exploring 224-228; 144 sustained exploring (99.7% of frames), not yet in combat (94-98%) |
| What is left | section 9 |

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
any recorded run ([release-path] lines: none).

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
commands 1-2, the inflate's shell) can hold a reservation.

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

## 9. What R8 leaves, and the next steps

R8 so far: the inventory, the timer question settled, the harness's hold
fixed, unimplemented graphics made loud, and the two largest measured
costs on the critical path cut (the loads' tests, the indirect dispatch).
Not yet done, in the order the measurements point:

| Left | Measured | Next |
|---|---|---|
| **The stores' tests** | every guest store still tests the inline range and reads the SPU reservation bitmap (a 4 MB bitmap: a load per store); not measured alone | measure a release with bare range tests on stores; the bitmap can go only with the SPU reservations it serves (below) |
| **The registers in memory** | the lifted code keeps every guest register in the context structure (each written register a store, each read a load); the compiler cannot keep them in registers (-O2 and `__restrict` change nothing) | the lifter promoting registers to locals between calls -- a code-generation change across the whole lift, validated scenario by scenario |
| **The SPURS handshake for native physics** | ~0.4 ms of a heavy-debris frame on the game thread: cellSpursCreateTask for every task (its 174 KB ELF hashed on the PPU to validate a cache, the lock-line lock, the taskset), and the main thread polling the result queues | a native submission for the native workloads (no fingerprint, no taskset), then the polling |
| **The game's own physics on the PPU** | ~0.8 ms of a heavy-debris frame, lifted | the code-generation work above; Bullet's PPU side stays the game's |
| **Lifted SPU code left (non-audio)** | the water task (every frame, 9% of a pool thread, off the critical path), solver commands 1-2 (body and manifold set-up), the inflate's SPU shell | native versions, retiring their SPU environment use; then the reservation bitmap's last users are FMOD's |
| **The hook path** | the generic entry dispatch, a lock and counters on every one of ~2,050 observed calls a frame: 0.7 ms exploring (profiled) | direct per-entry hooks for the hottest entries |
| **Development code in the release** | R4-R6's verify modes and the capture check compiled in, inert | trim |
| **Audio** | FMOD's mixer, codecs and DSPs lifted SPU code: shares the SPU context, channels, DMA, reservations, sys_event | the audio milestone |
| **R7's open items** | the post-boss warp run (the owner's optional session), occlusion against RPCS3, the depth clamp, the dual run's stall | unchanged |

## 10. Reproducing

```
tools/r8_lift.sh                        (R8's lift: R7's hooks, R8's lifter -- the call-site cache -- into build/r8/gen)
tools/r8_release_build.sh build/r8/rel -DRECOMP_DIR="$(pwd)/build/r8/gen"    (the R8 release: bare loads, -O1)
tools/r8_release_build.sh <dir> [-DNAME=value ...]            (a variant, e.g. -DDOTHEROES_PPU_OPT=-O2, -DDOTHEROES_PPU_CHECKED_LOADS=ON)
tools/r8_scene.sh <tag> <scenario> <secs> [NAME=value ...]   (a run into build/r8/<tag>; R8_BIN picks the binary, default build/r8/base;
                                                              later NAME=value pairs win: DOTHEROES_DEV_CANT_DIE=0 for normal damage)
tools/r8_threads.py <profile.raw> [--fps F] [--thread NAME] [--under FUNCTION]   (per-thread work and waits, by category)
tools/r8_split.py <profile.raw> <spans> <ftrace.txt>          (the main thread in two sets of frames, e.g. heavy debris against the rest)
tools/r7_measure.py, tools/r7_grabs.py, tests/fps_check.py, tests/actor_steps.py   (as in R7)
```

The batches, kept in build/r8 with their runs: prof1 (the inventory's
profiles), ab1 (-O2), hw1/hw2 (the frame counters' readers), sweep1 (the
timing sweep, the debris profile, the lost-work tests), ab2 (bare loads,
the call-site cache), val1 and dmg2 (the release's validation), bench1
(the measurement).

Switches (development): DOTHEROES_DEV_HWWATCH=<ea>[:w][,...]@<start s>,<secs>
(up to four words watched with the debug registers; hwwatch.txt),
DOTHEROES_R8_LOST_TEST=<frame>; the build options DOTHEROES_PPU_OPT and
DOTHEROES_PPU_CHECKED_LOADS. Engine changes: patches/ps3recomp/0117
(PAD_FILE holds), 0118 (the call-site cache, PPU_BARE_LOADS).
