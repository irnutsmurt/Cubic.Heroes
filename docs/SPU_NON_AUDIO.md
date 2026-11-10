# The remaining non-audio SPU workloads: what replacing each takes

Written 2026-11-07 (R8). This is a plan with its evidence, not a record of
work done. Nothing here is implemented, and nothing is validated.

Markers:
- [C] read in the code or the lift;
- [M] measured in a run;
- [H] a hypothesis still to be established;
- [I] inferred from measurements.

The audio workloads (FMOD's mixers, codecs and DSPs: images 6-26, an lv2
SPU thread group) belong to the audio milestone and are left out, except
where they decide what can be removed.

Sources:
- R8's inventory (docs/NATIVE_R8.md section 1; the static read in
  build/r8/inventory_spu.md, local);
- section 6.1 (the physics issued directly);
- docs/BULLET_NATIVE.md (the physics tasks' structures);
- port/native/edge_inflate.c;
- the lift (build/gen/spu).

## Where things stand

| Workload (runtime image) | Lifted image | Now | On the critical path? |
|---|---|---|---|
| Bullet solver, commands 1 and 2 (5) | spu_0004 | **lifted**, run through SPURS: phys_direct declines them [C] | **yes**, in heavy-debris frames: the game thread waits for each command [M, section 6.1] |
| Edge zlib's shell (3) | spu_0002 | native inflate inside the lifted task; the task's SPURS shell, its queue and its completion lifted [C] | in area loads (load time), not in frames [M, HANDOFF] |
| Water (1) | spu_0000 | **fully lifted**, a persistent SPURS task signalled every frame [C] | no: 9.4% of one pool thread in the river, 0 in the fields [M, section 1] |
| Waver (2) | spu_0001 | **native by default** (2026-11-10): reachable from the hero's update; ported bit-exact, checked offline and in the game (section 4) | -- |
| Bullet collision, pairs the native code does not cover (4) | spu_0003 | lifted, declined to SPURS by phys_direct [C] | how often: **not measured** |
| Bullet collision (covered pairs), solver commands 4-6 | -- | native, issued and completed directly (section 6.1) | done |

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

**Now** [C, M]:
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

**Now** [C, M]:
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

## What remains after all five

Kept for FMOD until the audio milestone:
- the SPU context, channels, mailboxes, MFC and DMA;
- the lock-line lock and reservations, the dispatcher;
- sys_event, the lifted registry with its overlays.

The SPURS instances (Water, PhysXManager, the inflate's) and their
emulation are no longer used by any workload: SPURS itself can go once
nothing creates an instance. That is not the case until each step above is
done and validated. Until then the SPURS path stays as each workload's
fallback switch.

## Queued (runtime evidence each needs; docs/NATIVE_R8.md section 9.1)

- The solver's set-up captures (section 1, step 1): a river fight with the
  extended snapshot.
- The water task's DMA and atomics over a few hundred river frames
  (section 3, step 1).
- The declined collision tasks counted (section 5) and the waver's
  dispatches (section 4): counters in the next build, read from the
  validation runs.
- The inflate's PPU side (section 2, step 1) is a static read of the lift,
  possible without a run.
