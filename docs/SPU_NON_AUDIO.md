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
| Waver (2) | spu_0001 | lifted, registered, **never dispatched in any run so far** [M] | -- |
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
- **Still to do**: captures over more steps (build/r8/snap9_run.sh: five river steps, two in the fields),
  then the in-game runs with DOTHEROES_NATIVE_SOLVER=1,2,4,5,6 and the measurement.

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

**Step**: count its dispatches in the report and say so on stderr the first
time, so that a run reaching it shows it. Nothing to replace until then.

## 5. Bullet collision: the pairs the native code does not cover (image 4)

phys_direct declines a collision task whose pairs bullet_cd does not cover,
and the lifted task runs through SPURS [C]. How often that happens is
**not measured**.

**Step**: count the declined collision tasks and their pair types
(DOTHEROES_PHYS_STATS). Extend the native narrowphase only for pairs that
occur, each checked as the native collision was (HANDOFF's open problem 3,
its contact differences, still applies to the native collision as a whole).

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
