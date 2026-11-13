# The remaining non-audio SPU workloads: what replacing each takes

Written 2026-11-07 (R8) as a plan with its evidence. **Status 2026-11-12: every workload in it is done** --
native by default, or generated on the player's machine from their own lift -- and validated; the table below is
current. Each section keeps the record of what was done first, then its original plan as written on 2026-11-07
(marked "The plan of 2026-11-07"), which describes the state before the work and is history, not the present.
What is left of the SPU machinery itself (the runtime's SPU environment, SPURS, the overlays) and its
retirement: docs/OPENAL_MIGRATION_PLAN.md section 10 (M6).

Markers:
- [C] read in the code or the lift;
- [M] measured in a run;
- [H] a hypothesis still to be established;
- [I] inferred from measurements.

The audio workloads (FMOD's mixers, codecs and DSPs: images 6-26, an lv2
SPU thread group) belonged to the audio milestone and are left out: since M5
(2026-11-12) FMOD's PPU code is excluded from the player's lift and its SPU
images are not lifted (docs/OPENAL_MIGRATION_PLAN.md).

Sources:
- R8's inventory (docs/NATIVE_R8.md section 1; the static read in
  build/r8/inventory_spu.md, local);
- section 6.1 (the physics issued directly);
- docs/BULLET_NATIVE.md (the physics tasks' structures);
- port/native/edge_inflate.c;
- the lift (build/gen/spu).

## Where things stand (2026-11-12)

| Workload (runtime image) | Lifted image | Now | Its fallback |
|---|---|---|---|
| Bullet solver, commands 1 and 2 (5) | spu_0004 | **native by default** (2026-11-10, section 1): checked in the game against the lifted task, 0 spans different | DOTHEROES_NATIVE_SOLVER without 1,2: the lifted task through SPURS |
| Edge zlib's shell (3) | spu_0002 | **native by default** (2026-11-10, section 2): the whole shell replaced at its one caller, no SPURS created; every disc pack's CRC checked | packs it declines (one disc pack: the shell's small path) and DOTHEROES_INFLATE_DIRECT=0: the lifted task |
| Water (1) | spu_0000 | **native by default, generated on the player's machine** (2026-11-11, section 3), its start-up and shutdown the port's; the SPU's float mode since rel37 | DOTHEROES_NATIVE_WATER=0: the lifted task |
| Waver (2) | spu_0001 | **native by default, generated on the player's machine** (2026-11-11, section 4); the SPU's float mode since rel37 | DOTHEROES_NATIVE_WAVER=0: the lifted task |
| Bullet collision, pairs the native code does not cover (4) | spu_0003 | counted (2026-11-10, section 5): **none declined** in any scene run | an uncovered pair type: the lifted task through SPURS (never seen) |
| Bullet collision (covered pairs), solver commands 4-6 | -- | native, issued and completed directly (section 6.1) | -- |

Every fallback above keeps the SPU environment, SPURS and the lifted images alive until M6's census shows
what the whole game reaches (the parts of the game no scene has run yet -- later areas, uncommon collision
pairs, typed constraints, unusual pack sizes -- cannot be ruled out by the scenes so far).

**Generated on the player's machine (2026-11-11, docs/OPENAL_MIGRATION_PLAN.md M1)** [C, M]. The owner's
requirement: no code transcribed from the game in the repository. The water's and the waver's tasks were
native through hand transcriptions of their SPU code (2026-11-10, sections 3 and 4); those are gone. Their
native code is now **generated from the player's own lift** -- tools/gen_spu_native.py (generic: it turns any
lifted SPU image into C++, every statement the lift's own) with tools/spu_native_task.cpp.in and a per-task
configuration holding only where things are in the image (tools/spu_native/water.py, waver.py: its span, its
entry, the functions the host calls, the image's SHA-256) -- by the setup (tools/setup_pipeline.py, the `spu`
step) or the developer build (port/CMakeLists.txt), never committed and never in the prebuilt library. What
stays in the repository is the host around it (port/native/spu_native.{h,cpp}, water_task.cpp, waver_task.cpp):
the task's start as its main does it (the parameters in local store, its functions called with their
registers), its transfers done at once on guest memory, and the game's side (the hooks, the worker threads,
the native set-up and shutdown, the check modes). No SPU thread, SPURS or channel machinery runs: the
generated code calls the host's DMA directly. A tree without the lift (or another executable's images, which
the configuration's hash refuses) builds a stub, and the lifted tasks run.
- **Bit-exact by construction, and checked**: offline against the lifted tasks (tests/native/water_task_check.cpp,
  waver_task_check.cpp; x86-64-v3 and v2): the water's 300-step evolution and its made-up cases all equal; the
  waver's 600-frame evolution and 1,049 runs all equal (1,046 done in the transfer order the task was read to
  make, 3 halting as the lifted task does). In the game, check mode (rel29, M1's gate): the water at the
  river, **2,475 of 2,475 steps equal** to the lifted task run beside it; the waver, 4 starts forced in the
  fields, **1,085 of 1,092 frames equal** -- the 7 others at the starts and ends, the game's own races on the
  block described in section 4 (5 equal the native frame on the block as it was at the wait, 1 with the other
  texture; the first frame of the first start equals neither, as the last frames of a start did with the hand
  port).
- **Cost** [M, offline, this PC]: the water's step at width 128: **2,336 us generated (x86-64-v3; 6,437 us at
  v2)** against **783 us for the hand transcription** in the game (rel27) and about 4 ms lifted on the SPU
  pool; the waver's frame: **6,176 us generated** against 8,892 us lifted (the check's stand-in runtime) and
  2.7 ms for the hand transcription in the game. -O3 changes nothing; the local store's byte order costs one
  shuffle a load. About 13% of the waver's time is the float helpers' exact slow path (12% of its float
  vectors -- an exponent gap over 29 in an add, a denormal or extended-range operand -- redone lane by lane;
  measured by stubbing it out: 5.4 ms); the rest is the SPU code's own form, which the hand transcription
  (the same helpers, far fewer calls a value) did not keep. The player's build compiles the generated code for
  the player's own CPU (zig_build host_cpu), so v3's figure applies on a machine like this one.
- **In the game** [M, rel29, uncapped]: the water's step **2.85 ms** on its worker thread (rel27: 0.78 ms),
  22.7 steps a second at the river: about 6.5% of one core, off the critical path. The waver's frame **4.58 ms**
  on its thread (rel27: 2.63 ms), max 9.61 ms; the game's frame waits for the previous frame's task, **0.90 ms
  a frame on average while a ripple runs** (rel27: 0.005 ms). Against rel27 (docs/OPENAL_MIGRATION_PLAN.md
  section 4.2): with 4 ripples forced in the fields, 3% fewer frames and the p99 7.4 ms against 5.4 (frames
  over 6.94 ms 2.8% against 0.2%); at the river within the scene's spread. Always far above 60 fps; the
  headroom above 144 fps while a ripple runs comes back with a faster generator (a follow-up).
- **The SPU's float mode** (2026-11-12, rel37; the follow-up above, task A). What the generated code spent its
  time on [M, offline, the helper calls counted per block]: about 5.2 million helper calls a waver frame, 1 ns
  each; with the single-precision helpers replaced by the host's plain operations (not exact: a ceiling) the
  waver's frame fell from 5,279 to 2,055 us and the water's step from 2,410 to 823 us -- **60-65% of their time
  was spu_helpers.h's exact float forms** (on x86 each widens the four lanes to double and checks them, ~25
  instructions), not the SPU code's form. So the generator now runs an image's code in the SPU's own float
  mode when the image allows it: MXCSR at round toward zero, flush-to-zero, denormals-are-zero while the
  generated code runs (the template's call(); every call into the host has the host's mode back), where the
  host's single-precision operations truncate like the SPU and read and write denormals as zero, so fa, fs, fm,
  fma, fms and fnms are one host operation each (the FMA where the build has one, else in double) with integer
  checks for the +0 rule, the SPU's zeros and the range; a vector with an extended-range operand or a result at
  or above FLT_MAX is redone whole in double built from the words' bits (port/native/spu_native_float.h). The
  generator turns the mode on only when the image calls no helper that needs the host's own mode (the
  double-precision ones, the FPSCR's): the water and the waver call none; every other helper they use is
  integer work on the bit fields. ps3recomp's NEON helpers already did the same under FPCR. Generic: it reads
  which helpers an image's code calls, nothing of what it computes.
  - **Exact** [M]: tests/native/spu_float_rz_check.cpp (no game data) against spu_helpers.h's exact per-lane
    forms, words from every class the SPU distinguishes (its zeros, denormal patterns, extended range, FLT_MAX,
    the smallest normals, exponent gaps 0-64, every sign), four mixed lanes a vector: 216 million lanes
    per operation and as many again through the wide form alone, x86-64-v3 and v2, **0 different**. The tasks'
    own checks: the water's 300 steps and edge cases, the waver's 249 runs, both levels, all equal. In the game,
    check mode: the water at the river **2,475 of 2,475 steps equal**; the waver, four starts in the fields, in
    four runs 1,162 of 1,169, 989 of 997, 1,090 of 1,097 and 1,076 of 1,083 frames equal (rel35, the generator
    before, in the same check: 1,196 of 1,204). The differing frames are at a ripple's starts and ends (one run
    had one in a ripple's middle), where the game rewrites the block around the SPU task's read; fewer of them
    than with rel35 matched the native frame on the block as it was at the wait, so the inputs of each were
    dumped (DOTHEROES_WAVER_CHECK_DUMP) and replayed off-line (waver_task_check replay) in the last two runs: on
    every one's inputs -- 14 frames, the block at the creation and at the wait, 28 replays -- **the native frame
    equals the lifted task byte for byte**; the game's own result, at the check, matched neither replay in most,
    so what the SPU task read or what was left in the texture had moved around it. The generated code is exact
    on the game's inputs; the check's mismatches are its timing against the game (as M1's and the hand port's).
  - **Cost** [M, offline, this PC]: the waver's frame **2,621 us** (v3; 3,709 us at v2) against 5,279 (10,067)
    before; the water's step **1,306 us** (1,944 at v2) against 2,410 (6,450). The waver is at the hand
    transcription's speed; the water's step is the SPU code's own form now (its pixel loop is scalar: one value
    a vector, where the hand transcription did four) -- generic loop work, not float semantics, is what is left.
  - **In the game** [M, uncapped, real audio, three rounds rotated per arm; rel35 = M4's play build, rel37 = it
    with the float mode; both native audio]:

    | | rel27 (hand ports) | rel35 | rel37 |
    |---|---|---|---|
    | fields, 4 ripples forced: fps | 242.6-247.2 | 238.7-239.7 | 243.8-246.1 |
    | fields: mean / p95 / p99 / p99.9 | 4.04-4.12 / 4.68-4.80 / 5.59-5.74 / 7.38-8.30 ms | 4.17-4.19 / 5.65-5.81 / 7.39-7.49 / 8.87-9.04 ms | 4.06-4.10 / 4.74-4.79 / 5.58-5.67 / 7.41-8.00 ms |
    | fields: frames over 6.94 ms | 0.2% | 2.8-3.0% | 0.1-0.2% |
    | fields: the waver's frame (its thread) / the game's waits for it, a run | 2.62-2.63 ms / 14 ms | 4.42-4.59 ms / 1.92-2.21 s | 2.84-3.02 ms / 34-39 ms |
    | river fight: fps | 206.7-210.3 | 205.9-211.2 | 207.6-208.7 |
    | river: mean / p95 / p99 / p99.9 | 4.76-4.84 / 5.46-5.83 / 6.58-6.76 / 7.93-8.38 ms | 4.73-4.86 / 5.44-5.83 / 6.56-6.83 / 8.03-8.83 ms | 4.79-4.82 / 5.42-5.56 / 6.59-6.77 / 7.92-8.53 ms |
    | river: the water's step (its worker thread) | 0.78 ms | 2.84 ms | 1.60 ms |

    M1's loss in the fields is recovered: rel37 is within rel27's range in every figure. At the river all
    three are within one another's spread (the game does not wait for the water); the water's worker takes
    1.60 ms a step against the hand port's 0.78 -- about 3.6% of a core at 22.7 steps a second.
  - **The mode's switches, checked** [M, 2026-11-12]: MXCSR is each thread's own (the system keeps it with the
    thread), and the template sets and restores it in scoped guards -- call() saves the caller's and gives it
    back on every way out (each return, an unwinding exception); each call into the host (a channel, a stop, a
    branch it takes, an instruction the lift could not translate) runs in the caller's mode and puts the SPU's
    back after; a host that leaves by longjmp leaves from the host's mode. Measured
    (tests/native/water_task_check.cpp `mxcsr`, both levels): every transfer the generated water asked of the
    host ran in its caller's MXCSR, and the caller had its MXCSR back after every step (0 wrong); four threads at
    once in four modes (round down, up, toward zero; FTZ with DAZ) each gave 40 steps byte for byte as in the
    default mode; a whole step run inside another step's transfer (a nested call) left both right. The helpers
    the generated code still calls in the SPU's mode -- the compares, frest, frsqest, fi, cflts, cfltu, csflt,
    cuflt -- are integer work or conversions exact in double: spu_float_rz_check gives each the same words in
    both modes (16 million lanes each, both levels, 0 different), so a compiler moving one of them across a
    switch changes nothing; the float operations that depend on the mode are spu_native_float.h's, in volatile
    asm that does not move.
  - **Watched**: the one waver mismatch in a ripple's middle (the first two check runs, before the dump existed)
    was not replayed -- it is the only differing frame not shown to be the game's timing. The check mode with the
    dump (DOTHEROES_WAVER_CHECK_DUMP, then waver_task_check replay) stays in the validation of each M6 retirement
    that touches the SPU tasks; a recurrence is replayed before anything else.
  - **Follow-ups, not scheduled** (the owner, 2026-11-12: about 0.8 ms on a worker the game does not wait for):
    the water's pixel loop -- generic peepholes in the generator (the mpyh/mpyh/mpyu multiply idiom, rotate and
    shift pairs) and loop vectorization.

**What decides the order.**
1. Solver commands 1 and 2 are the only remaining non-audio SPU work the
   game thread waits for. They come first.
2. Then the inflate's shell, for load time.
3. Then the water, for architecture: no frame time is expected from it.
4. The waver and the collision remainder need measurement before anything
   else.

**What their removal buys beyond time** [C]. Each of these runs through the
SPURS emulation:
- cellSpursCreateTask's bookkeeping under the lock-line lock;
- a content hash of the task image;
- the taskset, event-flag and lock-free-queue emulation;
- a kernel thread a SPURS instance that wakes every millisecond;
- the pool threads;
- the SPU context, its channels and DMA.

They also take **SPU reservations**:
- the water task: 4 atomic sites (GETLLAR/PUTLLC);
- the inflate shell: 14;
- the lifted solver tasks' libsync lock.

The PPU's store-side reservation test (0.4 ms of the game thread, the
owner's pending decision, docs/NATIVE_R8.md section 9) is required while
any SPU code takes reservations on lines the PPU writes. Replacing every
non-audio workload does not end that requirement: FMOD's code takes them
too. Only with the audio milestone, or with option (b) of that decision,
does the test become removable.

---

## 1. Bullet solver: body set-up (command 1) and manifold set-up (command 2)

**Progress (2026-11-09)** [M]:
- **Command 1, the body set-up: native and checked.** bullet_solver::setup_bodies_task: per body, the
  solver body's whole 0x40 bytes (deltas zero, angular factor from the rigid body's +0x154, inverse mass
  from +0x150, +0x2C the local-store address of the body's copy, 0xC800 + 0x1F0 x its place in the package
  of 256, the rest zero) and the companion id (+0xC4). Offline on four captured steps byte for byte
  (tests/native/bullet_solver_snapcheck.cpp setup-bodies); in the game, DOTHEROES_NATIVE_SOLVER_CHECK=1,
  200 steps of the river fight with the lifted task (20,950 bodies) and 200 with the native one (20,587):
  0 not as expected. Behind the switch: DOTHEROES_NATIVE_SOLVER=1,...; not yet the default.
- **Command 2, the manifold set-up: native, checked offline, not yet in the game.**
  bullet_solver::setup_manifolds_task, byte for byte on the four captured steps (71-201 points a step:
  rows, solver bodies, index lists, hash cells; nothing else changed). Read from the lifted image's
  disassembly: the command table at local store 0x43AC, command 2 at 0x53D4-0x8160. Its descriptor: +0x30
  the first cell, +0x34 the cell count, +0x38 the body count (this document said "+0x34.. counts"),
  +0x40 the manifold holders (16 bytes, the manifold at +4), the time step +0x54, erp +0x68, the warm-start
  factor +0x80; btPersistentManifold is 0x2E0 bytes. Cells with typed constraints are left to the image
  (setup_manifolds_supported); none has been seen.
- **A finding the native code has to keep**: the task reads a moving body's velocity through the solver
  body's +0x2C -- the local-store address command 1 left, which in command 2's own local store holds
  copies of manifolds, the index list or zeros. The native code models that region (0xBC60-0x10A00) as
  the task fills it and reads the same bytes; zeros there instead change the friction rows of 8 points in
  two of the four steps. It rests on each task starting with a fresh local store, as the runtime gives
  (unless LBP_PERSIST_LS). On the PS3 the same reads took whatever an earlier task left.
- **Checked in the game, measured, the default (2026-11-10).** Three more captured river steps (200, 400,
  600: build/r8/snap9_run.sh) pass both commands offline -- seven steps in all. In the game,
  DOTHEROES_NATIVE_SOLVER_CHECK=1 now also checks command 2: every 10th step guest memory is copied as its
  first task is created, the native set-up runs on the copy once the game's tasks are done, and every span
  is compared -- two river fights, 180 steps, 17,398 points, 41,572 spans: **0 differ**. Runs with
  DOTHEROES_NATIVE_SOLVER=1,2,4,5,6 (river, fields, dungeon, a normal-damage river fight): 81,654 contacts set
  up natively in a river run, 0 reads outside the modelled local store, 0 tasks with typed constraints, the
  runs ended at their time. Measured (build/r8/ab12_run.sh, rel11, the river fight uncapped with real audio,
  three rounds rotated): 203.4-209.3 fps against 198.2-200.9 with the lifted set-up; p99 6.57-6.82 ms
  against 7.24-7.44; frames over 6.94 ms 0.5-0.8% against 1.6-2.0%; the light frames' p99 6.53-6.80 against
  6.95-7.26; the heavy-debris frames (one native round had them) p50 5.97 and p99 8.42 against 6.49-6.69 and
  9.11-9.68. **Now the default**: setup_app.py's performance settings (an install whose value is still the
  old default "4,5,6" is brought up to date; a player's own value is kept), the scenarios, the reference
  scenes. No solver task goes through SPURS any more in the runs seen.

*The plan of 2026-11-07, as written (done: above).*

**Where** [C]:
- task image 5 (`PS3_SPURS_BulletSolver_spu_release`, entry 0xF8EC80);
- issued by func_00955CF0 and awaited in func_00955BD4, the two functions
  phys_direct hooks;
- today phys_direct declines these two commands, so the lifted body creates
  a SPURS task, and the wait takes its entry from the slot's output queue.

**Inputs** [C, docs/BULLET_NATIVE.md "Solver structures, checked"]: the
task descriptor (0x90 a slot):
- +0 command, +4 task id (= slot), +0x10 the hash, +0x14 the solver bodies,
  +0x18 the contact constraints, +0x20 the body offsets;
- command 1 (body set-up): +0x30 first body, +0x34 count, +0x38 the list of
  the game's (Bullet 2.73) btRigidBody pointers;
- command 2 (manifold set-up): +0x34.. counts, +0x40 the manifold holders,
  +0x48.. the solver info;
- the hash's cells (+0x870: per cell, the bodies', manifolds' and
  constraints' counts and offsets the PPU summed);
- the manifolds (btPersistentManifold points) and the rigid bodies.

**Outputs** [C for the layouts checked, H for the rest]:
- command 1: the solver bodies (0x40 each): velocity deltas +0x0/+0x10,
  angular factor +0x20, inverse mass +0x24;
- command 2: the contact constraints, three of 0x90 bytes a contact (the
  contact and two friction rows), with these fields:

  | Field | Offset |
  |---|---|
  | relpos1 x normal | +0x0 |
  | normal | +0x10 |
  | relpos2 x normal | +0x20 |
  | angular components | +0x30 and +0x40 |
  | warm-start impulse | +0x54 |
  | body ids | +0x58/+0x5C |
  | friction | +0x60 |
  | 1 / jacobian diagonal | +0x68 |
  | the manifold point's back-pointer | +0x78 |
  | rhs | +0x7C |
  | cfm | +0x80 |
  | limits | +0x84/+0x88 |

  Typed constraints' rows as well, if the game has any [H].

**Still unchecked** (BULLET_NATIVE.md's step 1):
- btSolverBody as a whole;
- the rest of btSolverConstraint;
- the manifold and constraint cell holders;
- the btRigidBody fields set-up reads.

**Dependencies.**
- [C] The native iterate (command 4) consumes these outputs as the lifted
  set-up leaves them; it is checked bit for bit against the lifted iterate
  on that input.
- [C] The SPU's single-precision arithmetic: truncation toward zero,
  denormals as zero (spu_helpers.h). The copy-back already had to round
  toward zero to match.
- [C] Bullet 2.74's SpuParallellSolverTask.cpp (zlib), as the reference for
  the algorithm. The game's code is the authority: layouts and order of
  operations are read from the lift and checked against snapshots.
- [C] phys_direct: issue, the pool, completion.

**Synchronisation.**
- [C] The PPU issues a command's tasks and waits for all of them before the
  next command. Body set-up runs 1-2 tasks a step; manifold set-up runs 8,
  pipelined over the three slots.
- [H] Unlike iterate, set-up hands each task a range of its own, with no
  cell claiming or lock (BULLET_NATIVE.md: "deterministic per task"). To be
  confirmed in the lifted task: if it takes the libsync lock or reservations,
  the native version must take them the same way, as the native iterate
  does.
- [C] Completion goes through phys_direct's job: published with release,
  taken with acquire by the waiting thread before the game reads the
  outputs.

**Implementation steps**, each checked before the next:
1. **Capture.** Extend DOTHEROES_SOLVER_SNAP to capture guest memory before
   a command-1 or command-2 task starts and after the command's last task
   returns, for a few hundred steps of the river fight. Check the unchecked
   layouts against them (tests/solver_layout.py).
2. **Body set-up, native**: offline on the captures, bit for bit
   (tests/native/bullet_solver_snapcheck.cpp, a `setup-bodies` mode).
3. **Manifold set-up, native**: the same, per task range.
4. **In the game, check mode**: DOTHEROES_NATIVE_SOLVER_CHECK runs the
   native command into scratch memory beside the lifted one and compares,
   over the river fight and a debris-heavy segment.
5. **On**: DOTHEROES_NATIVE_SOLVER=1,2,4,5,6; phys_direct accepts the two
   commands, so no solver task goes through SPURS.

**Correctness tests.**
- Steps 2-4: 0 differences over at least 600 steps from two runs, including
  steps with 120+ contacts.
- Then the physics drive at 60 fps and uncapped: bodies, steps, the camera
  target's distance.
- Normal damage, the eight validation scenarios, and the solver's existing
  checks with every command native.

**Expected benefit** [I]: the lifted manifold set-up runs 130-175 us a task,
8 a step, and was most of the game thread's 0.39 ms of waiting a step
[M, section 6.1 and BULLET_NATIVE.md]. Native set-up at iterate's speed-up
would take about 0.3 ms off a heavy-debris frame, where the median is
6.9-7.5 ms against 144 fps's 6.94. To be measured: heavy-frame p50/p99 and
waiting (tools/r8_phys.py), against the release before it.

## 2. Edge zlib's shell (image 3)

**Done (2026-11-10)** [C, M]: the direct path, on by default (port/native/inflate_direct.cpp;
DOTHEROES_INFLATE_DIRECT=0 keeps SPURS). What the reading found, against the plan below: every map pack
goes through one function, func_005CF5BC (r3 the .gz image, r4 its size, r5 the output; its only caller
func_000752A0), which builds **a whole SPURS instance** (1 SPU), a taskset, an event flag, an Edge LF
queue (16 entries of 32 bytes) and the task for that one pack, pushes **one** request, waits in
cellSpursEventFlagWait (AND), then frees and finalizes all of it. So one declining entry hook on
func_005CF5BC replaces the whole of it: the inflate runs in the calling thread (which waited anyway),
from the gzip header the game walks (FTEXT/FNAME only, as the game's packs all are), into the same
output; nothing of SPURS is created. Corrections to the plan: no size word (the request carries the
output size and the stream must fill it); the counter has an error bit (0x80000000), the flag's bit is
set as it goes from 1 to 0; packs whose output and input are 64 KB or less took the shell's small path,
lifted, not native (one disc pack does). Declined to the lifted path: anything else in the header,
odd sizes, memory it cannot use, an output not filled exactly, a CRC mismatch.

Checks: offline, all 167 disc packs (1,950 MB from 463 MB) through the direct path, each CRC-32 equal
to its trailer (~1.2 GB/s). In the game (rel13, DOTHEROES_INFLATE_CRC=1): river, dungeon, after the
dungeon, fields -- every pack's CRC, size and address the same as the SPURS path's in the same scene
("N equal, 0 different", "0 through SPURS"); the SPU census shows image 3 never started; the eight
scenarios, the reference scenes, sync validation and a normal-damage fight pass. A dungeon pack of
42-47 MB inflates in 48-66 ms in the calling thread.

*The plan of 2026-11-07, as written (done: above).*

**Before** [C, M]:
- Each area load creates a SPURS taskset, an event flag and a lock-free
  queue, pushes inflate requests, and creates the task.
- The task's body for large inflates is native (an SPU entry hook,
  config/spu_entry_hooks.json: edgeZlibFetchAndInflateLargeRawData ->
  port/native/inflate.c, ~1.1 GB/s, against ~140 MB/s lifted).
- Its shell stays lifted: the queue pop, the counters, 14 atomic sites and
  the event flag's set.
- The main thread waits in cellSpursEventFlagWait: a 2 ms timed wait,
  woken early by the PUTLLC commit hook.
- The task parks until cellSpursFinalize.

**Inputs** [H, to read from the lift]: the queue entry, Edge's inflate
request:
- the source address and compressed size;
- the destination and its buffer size;
- the address the inflated size is written to;
- the counter or event flag the request completes.

**Outputs**: the inflated bytes, the size word, the counter's decrement
and the event flag's bit [H].

**Dependencies.**
- [C] The SPURS emulation: taskset, event flag, LF queue.
- [C] The SPU environment for the shell: its context and 256 KB local
  store.
- [C] port/native/inflate.c.

**Synchronisation** [H]: the PPU pushes requests, then waits on the event
flag. The task decrements a shared counter atomically and sets the flag's
bit when a request completes. Read from the lifted shell and the game's
PPU side before replacing anything.

**Implementation steps.**
1. Read the PPU side in the lift. Find the game's (or Edge's) enqueue and
   wait functions, as section 6.1 did for the physics: who creates these
   tasks and queues, and nothing else touching them.
2. A direct path: a declining entry hook on the enqueue. It runs the
   inflate on a native thread and completes the request exactly as the
   task does (the size word, the counter, the event flag set through the
   runtime's own API so the waiting thread wakes at once). It declines
   anything it does not recognise. The SPURS path stays behind a switch.
3. Then no inflate taskset, task or LF queue is created; the event flag
   stays if the game waits on it.

**Correctness tests.**
- DOTHEROES_NATIVE_INFLATE=check (the bytes, as now).
- Hashes of every inflated pack, equal to the current build's over the area
  loads of the eight scenarios.
- DOTHEROES_INFLATE_LOG timings: tests/inflate_frames.py, the dungeon
  entry, the overworld's areas.
- 20 area-reload cycles: memory bounded, no task, thread or queue left
  behind (the cycles of HANDOFF's long-session tests).

**Expected benefit** [I]: load latency (the poll's 2 ms granularity, the
task and context set-up), not frame time.

## 3. Water (image 1)

**Done (2026-11-10)** [C, M]: native on a worker thread by default (port/native/water_task.cpp;
DOTHEROES_NATIVE_WATER=0 keeps the SPU task, =check runs both and compares). What the reading found:
the PPU (func_003C7460) signals the task **once a simulation step** (22.8 a second in the river), not
every frame, through func_00473668 (cellSpursEventFlagSet(block + 0x3D400, 1)); the task GETs a 24-byte
block (two 128 x 128 float height layers in main memory, the normal map's texture in RSX local memory,
the width, the layer index, damping, scale), steps the heights
(cur = ((left + right + up + down) * 0.5 - cur) * damping, rows wrapping) and writes the normal map
(x, y from the slopes, normalized with the SPU's estimates, 0x00RRGGBB). Its four atomic sites are
the libspurs event-flag wait, not water data; the "mailbox calls" an spu_printf error path. The game's
own DMA order leaves the last row's "down" neighbour as row W-8, not row 0; the native code keeps it.
A declining hook on func_00473668 runs the step natively (the quit is declined to the lifted task).

Checks: offline against the lifted functions on synthetic inputs (tests/native/water_task_check.cpp,
widths 4-408, edge values, 300 evolution steps); in the game, check mode over two river fights: 705 and
2,489 steps compared, heights and texture **all equal**. Native: 22.8 steps a second, 0.78 ms each on
its worker thread. The census: image 1 still starts once (the task parks in its wait; the instance is
created by the game's set-up, which is not hooked).

**The start-up and the idle wait, native too (2026-11-10)** [C, M]: the water's set-up (func_003C7BA8, reached
from func_000221DC through the TOC stub func_009810A8) built a SPURS instance "Water", a taskset, a 0x3D600-byte
block, an event flag and the persistent task that parked in its wait; the shutdown (func_003C7AF4) set the quit
bit, waited for the exit code and tore it all down. Both are now the port's (port/phys_hooks.cpp,
water_task.h): the set-up does the game's own stores and its two texture calls in the game's frame and puts
markers (0xFFFFF001-3) where the instance, taskset and block went -- the update (func_003C7460) only tests the
taskset for non-zero and passes the block to the signal, and nothing else in the lift reads them -- and the
shutdown waits for the worker's step, then clears the globals as the game's does. DOTHEROES_NATIVE_WATER=0 or
check, a capture, or DOTHEROES_WATER_SPURS=1 keep the game's set-up.
- **When they run** (DOTHEROES_WATER_LOG=1, tests/scenarios/river_water.drive): the set-up once, at boot (0.1 s);
  the water is the game's for the whole session -- TITLE and CONTINUE does not shut it down; the shutdown only in
  the game's own exit path (the drive's new `quit`: the game asked to exit as closing the window asks it).
- **Pause, reload, shutdown** (rel15/rel16, the drive with native scheduling and with the SPURS set-up): the same
  gaps in the signals both ways -- 11.5 s over each 10 s pause, 24.7 s over TITLE and CONTINUE, 2.3 s over
  each area change -- and the same rate between; at the quit, "shutdown #1, native" after its last step, and
  the game's SPURS shutdown with the hook declining.
- **The census** (SPURS starts; FMOD's own lv2 SPU thread is not in it): with native scheduling **no SPU program starts through SPURS** in the river, the menus, the dungeon, after
  the dungeon, the boss's exit; with the SPURS set-up image 1 starts once, as before.
- The check mode, the hooks present: 2,495 river steps compared, all equal. rel15's validation scenes and the
  reference scenes pass.

*The plan of 2026-11-07, as written (done: above).*

**Before** [C, M]:
- A persistent SPURS task, its own instance "Water" (1 SPU), resumed from
  its signal wait each frame by cellSpursEventFlagSet.
- Fully lifted.
- 9.4% of one pool thread in the river fight, 0 in the fields.

**What the lift shows** [C, spu_0000_at_00F47380: 60 functions]:
- one DMA command site (a transfer helper, its opcode in a register);
- four atomic sites (GETLLAR/PUTLLC);
- the SPURS task's mailbox calls.

What it reads and writes is **not established**.

**Hypothesis** [H]: it writes the water's small A8R8G8B8 texture in RSX
local memory. R5-R7 found that texture rewritten every frame, the one
guest input rewritten inside the recorder's flush window (NATIVE_R7.md
section 2).

**Implementation steps.**
1. **Capture its traffic**: every DMA of image 1 for a few hundred frames
   (the runtime's MFC trace, filtered to the image: addresses, sizes,
   directions, the frame), and the lines its atomics reserve. Map each
   output to its reader: is the put target the texture the recorder
   captures?
2. **Read the algorithm** from the 60 lifted functions. Port it
   bit-exact, in the SPU's float semantics.
3. **Check offline** on captured inputs (the bytes it gets, its outputs),
   then in the game in a check mode: the native job into scratch memory
   beside the lifted task, compared each frame.
4. **Replace.** A declining entry hook where the PPU signals the task's
   event flag starts the native job. The instance then needs neither its
   task nor, if nothing else uses it, its SPURS instance and kernel thread.

**Synchronisation, to keep** [H]: the PPU awaits nothing; the frame's draw
reads the output. Today the order between the lifted task's write and the
draw's read is a race the game tolerates (R7: the texture changes inside the
flush window). The native job must keep whatever ordering the game relies
on, which step 1 establishes, and not tighten or loosen it unmeasured.

**Correctness tests.**
- Outputs byte for byte against the lifted task (steps 3-4).
- The river's and the moat's grabs against the current release.
- R7's mutable-input check (inputs hashed at the flush and at the capture:
  0 changed after the flush).
- The eight scenarios.

**Expected benefit**: none on the critical path [I]. Architectural: one
SPURS instance, its kernel thread, one pool thread's work, and its
reservations.

## 4. Waver (image 2)

Registered, never dispatched in any run [M]. Only the start of the game has
been played under test: nothing past the castle area, plus the owner's
sessions. So "never" means never yet.

**Traced, reachable, replaced (2026-11-10)** [C, M].

*What triggers it* [C, the lift and the ELF]: the waver's three entry points are reached only through 16-byte
TOC stubs (`std r2,0x28(r1); addis/addi r2; b`), which is why no descriptor or direct call named them:
- the set-up func_003CEB48 from func_0018B708 (stub func_009842F8), which centres it on the hero (the player's
  +0x428/+0x430, through func_00983238) and plays sound 0x55;
- func_0018B708 from func_0011E1A8 at 0x0011E5B4, a sub-state machine run from the hero's update
  (func_0012476C): in its state 2 (+0x500), once a timer (+0x6FC) runs out, kind 0x11 (+0x5E8) looks an object
  up (func_00983498) and starts the waver on it; kinds 0x0F, 0x10 and 0x12 take other branches (sounds 0x51,
  0x57). Which game action sets kind 0x11 is **not established** (+0x5E8 has over a hundred writers in the lift).
- each frame while it runs (+0 of its state W = 0x02AB2200 set), the frame function func_000810F8 calls its
  update func_003CEEE8 (stub func_00982788); the update tears it down after its duration (about 5 s, ~300
  frames); its shutdown func_003CEAC0 is also called by the effects' resets (func_003B64A8, func_003B72D4,
  func_003B7358).

*What it is* [C, port/native/waver_task.h]: an expanding ripple at the hero, one SPURS task **a frame**: the
update writes the parameters, waits for the previous frame's task and creates the next, the game thread
waiting on it. The task GETs a 0x10224-byte block (a 256 x 256 distance map, a 128-entry wave table, phase,
radius, frequency, amplitude, slope), writes a 256 x 256 float height field, reads it back and writes its
normal map into one of two 256 x 256 textures (alternating). No atomics, mailbox or event flag (the flag the
set-up makes is never read).

*Test case*: DOTHEROES_DEV_WAVER=<s>[,...] (port/phys_hooks.cpp, a test switch through an entry hook on the
frame function) starts it at the hero's position at those times, exactly as func_0018B708 does, through the
game's own set-up -- the natural trigger aside.

*Native* (port/native/waver_task.cpp, waver_game.cpp; DOTHEROES_NATIVE_WAVER=0 the SPU task, =check both):
the set-up's own stores and texture calls with markers (0xFFFFF011-3) for its SPURS objects; the shared SPURS
wrappers (create, wait, free, destroy, finalize) act on the markers only, so a frame runs on a worker thread of
its own, the game's wait waits for it, the teardown does nothing; anything else goes to the game's code.
- Offline (tests/native/waver_task_check.cpp, the lifted image run whole): 249 of 249 runs equal byte for byte
  and in transfer order, at x86-64-v3 and v2, 1,649 more at v3; seven deliberate mutations of the port all
  caught.
- In the game, check mode, the fields and the river, 2 starts each: 1,183 frames compared, **the height field
  equal in all**, the texture in all but 8 -- the first and last frame of each start. The check then also
  ran the native frame on the block as it was when the game came to wait, and compared both textures
  (rel17, 4 starts, 1,204 frames): the first frame of a start equals the native frame on the block at the
  wait (the task read it late: the game rewrites the select word right after the creation), or the one on
  the creation's block with the other texture; **the last frame of each start equals neither** -- the game
  writes the next frame's values before it waits, and the task, reading the block while they are being
  written, gets some of each (field values unchanged, the select or the slope new): a read the two copies
  cannot reproduce. All are the game's own races on the block; the native frame reads it as the task does,
  at its start.
- Native: 4 starts, ~300 frames each, 2.7 ms a frame on its own thread; the frame function (the game thread)
  0.024-0.028 ms a frame with it running (0.016 ms with the SPU task; the first frame's two tasks back to back
  waited 3.3 ms once); no SPU program started through SPURS.
- Untested: the natural trigger (kind 0x11); the effects' resets stopping it midway.

## 5. Bullet collision: the pairs the native code does not cover (image 4)

phys_direct declines a collision task whose pairs bullet_cd does not cover,
and the lifted task runs through SPURS [C]. How often that happens is
**not measured**.

**Step**: count the declined collision tasks and their pair types
(DOTHEROES_PHYS_STATS). Extend the native narrowphase only for pairs that
occur, each checked as the native collision was (HANDOFF's open problem 3,
its contact differences, still applies to the native collision as a whole).

**Counted (2026-11-10)** [M]: phys_direct reports what goes through SPURS by kind (the solver's command, the
collision task's reason), and bullet_cd its unsupported pairs. Across rel13's and rel15's runs (the river, the
fields, the dungeon, after the dungeon, the menus, the editor, the save list, the boss's exit, normal damage):
**no collision pair the native code declines** ("unsupported: none") and no solver command declined -- with one
exception, which was not a pair type but a defect in phys_direct itself:

- **The managers' table filled for good.** phys_direct kept at most four managers, keyed by guest address, and
  never let one go. The game makes a new pair (collision, solver) for some area loads and frees the old one.
  From the third pair on, every physics task of the session went through SPURS: rel13's validation run after
  the dungeon (area 146, its third world) sent 720 tasks and then every task through SPURS, images 4 and 5
  started lifted 1,093 and 51,170 times, and the waiting thread polled the queues 6.4 million times. An address
  reused by a manager of the other kind also kept the kind it was first seen with: the "solver task through
  SPURS: command 371879296" was a collision task seen as a solver's, whose first word is an address. A
  player would have met this after a few area loads.
- **Fixed** (port/native/phys_direct.cpp): each lookup reads the manager's task image again (one load) and
  re-keys an entry whose address now holds the other kind (never one with work out); a full table gives up
  the least recently used entry with nothing out; eight entries; the report counts managers taken on,
  re-keyed, evicted, and refusals. rel15: after the dungeon for 420 s, **8 managers taken on, 0 tasks through
  SPURS** (157,754 native); three river fights with TITLE and CONTINUE between, 6 managers, 0 through SPURS.

Untested: a manager address reused by the other kind (counted, not yet seen), a table full of busy managers
(the game's protocol never has more than one pair busy).

## What remains after all five (2026-11-12)

All five are done; FMOD's SPU programs are gone from the player's build (M5). What is left is machinery, not
workloads: the SPU context, channels, mailboxes, MFC and DMA, the lock-line lock and the per-store
reservation test, the dispatcher, sys_event, the lifted registry and its overlays, SPU thread groups, and
SPURS (the physics manager's two instances, which the game's own set-up still creates though no task runs in
them; Water's and the inflate's are no longer
created). Each stays while a fallback above can reach it; their retirement, one at a time with the census and
validation after each, is M6 (docs/OPENAL_MIGRATION_PLAN.md section 10).

## Queued (2026-11-07; all done since -- sections 1-5)

- The solver's set-up captures (section 1, step 1): a river fight with the
  extended snapshot.
- The water task's DMA and atomics over a few hundred river frames
  (section 3, step 1).
- The declined collision tasks counted (section 5) and the waver's
  dispatches (section 4): counters in the next build, read from the
  validation runs.
- The inflate's PPU side (section 2, step 1) is a static read of the lift,
  possible without a run.
