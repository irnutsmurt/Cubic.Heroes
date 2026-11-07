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
| Frame-counted values | done: none is a clock or moves gameplay (section 3) |
| The shorter uncapped walk | done: the test harness's input timing; fixed (ps3recomp 0117) |
| Unimplemented graphics calls | done: named, on screen, counted, fatal under strict (section 4) |
| The lifted code: -O2 | measured: no gain, not adopted (section 5) |
| The lifted code: the loads' tests | removed in the native release: the game thread 1.1-1.2 ms faster (section 5) |
| **The bare loads' assumptions** | **reviewed, enforced and tested** (`dotheroes --memtest`, 614 checks), defined for any alignment, gated to Windows x86-64; a checked comparison build kept (section 5.3) |
| **The stores** | **priced** (the reservation test 0.4 ms, the range test 0.2 ms: measurement builds); one mov after the tests; **the raw stores' missing reservation test added** (section 5.4) |
| **The SPU interpreter and microstepper** | **gone from every build**; any way into them stops the program whatever PS3_STRICT says (section 5.5) |
| **Register promotion** | at every call: measured worse, not adopted; **in functions that call nothing: adopted**, 3.5% of the game thread exploring (section 5.6) |
| The indirect calls | a call-site cache (ps3recomp 0118): +1.5-3% (section 5.1) |
| **The native physics tasks** | **issued and completed directly, without SPURS** (no task, no image hash, no queue, no polling of SPURS), checked and measured: heavy frames 0.2-0.5 ms shorter (section 6.1) |
| The releases | R8's first (rel1/rel2) and part 2's (rel6, then rel7 with leaf promotion; rel8 its final tree) validated: 8 scenarios' pictures, sync validation, capture check, the stops, gameplay timing at 60 and 200 fps, normal damage; the production and player build paths built and run (sections 5.2, 7, 7.1) |
| Measurement | done (sections 8, 8.2): combat 194 fps at the median (production 148), exploring 235 (production 186) for rel6; 144 sustained exploring (99.8% of frames), not yet in combat (97.8%) |
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

**rel8** (sha256 873496ec...) is rel7's tree with the last two source
changes, both outside the release's measured paths: the physics pool's
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
| **The recorder's bookkeeping** | 1.03 ms a fight frame, 0.35 ms of it the lock, the generic dispatch and mutex calls round the records | direct per-entry hooks (the lifter's declining entry hooks, as the physics uses) for the hottest observed entries, and one producer thread without the lock |
| **The SPU reservation test on every store** | 0.4 ms of the game thread (10%) -- priced by a measurement build, not removable as things stand | **a decision for the owner**: (a) keep it until no lifted SPU code takes reservations (the audio milestone and the non-audio SPU code below); or (b) change the synchronisation model: an SPU's PUTLLC checks the 128-byte line against what its GETLLAR read (as ppu_stwcx32 already does for the PPU) instead of every PPU store breaking reservations -- which loses PUTLLC's detection of a store that rewrote the same bytes (ABA) and needs another wake-up for SPU code waiting on a lost reservation (the SPURS queues and event flags, FMOD) |
| **Heavy-debris frames** | p50 6.9-7.5 ms in the fight (over 144's 6.94): the game's lifted physics on the PPU (~0.8 ms), the lifted solver commands 1-2 (~130 us a run, nine a step, waited for in turn) | native commands 1-2: a port of Sony's set-up code bit-exact with the SPU (its truncating single precision, its estimates), as iterate was -- days of work, worth ~0.3 ms of a heavy frame |
| **The stores' range test and the diagnostics' span** | 0.2 ms more (measurement build) | required (the first page's stores dropped, the store diagnostics); left |
| **Lifted SPU code left (non-audio)** | the water task (9.5% of a pool thread, off the critical path), solver commands 1-2, the inflate's SPU shell | native versions as measured benefit allows; with FMOD they are what holds the reservation test |
| **Development code in the release** | the test harness's threads (dev keys, state, stop) and R4-R7's verify modes compiled in, inert in play | trim |
| **Known gaps, found and left** | a misaligned scalar store across a 128-byte line tests only its first line's reservation; a raw (vector) store into the first page lands where a scalar one is dropped; the bare loads on Linux not established (checked there) | each a change of its own if a title needs it |
| **Audio** | FMOD's mixer, codecs and DSPs lifted SPU code: the SPU context, channels, DMA, reservations, sys_event | the audio milestone |
| **R7's open items** | the post-boss warp run (the owner's optional session), occlusion against RPCS3, the depth clamp, the dual run's stall | unchanged |

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
```

The batches, kept in build/r8 with their runs: prof1 (the inventory's
profiles), ab1 (-O2), hw1/hw2 (the frame counters' readers), sweep1 (the
timing sweep, the debris profile, the lost-work tests), ab2 (bare loads,
the call-site cache), val1 and dmg2 (the release's validation), bench1
(the measurement); part 2: ab3 (the stores), pv1 (the direct physics
issue's validation), pb1 (its measurement), vc1/vc2 (the raw stores'
reservation test), val2 (rel6's validation), bench3/bench3b (the
measurement), prof2 (the profiles), lp1 (leaf promotion), val3 (rel7's
validation). Each binary measured is kept beside its
build with SHA256SUMS (build/r8/rel2..rel7, chk3/chk5/chk6, stA, stX, stB,
rel5x).

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
and 0122 (the raw stores' reservation test), 0123 (--promote-leaf).
