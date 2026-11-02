# Native Bullet: what the game's physics tasks do (2026-10-31)

Replacing 3D Dot Game Heroes' two Bullet SPU tasks with native code, one at a
time. The collision task is done and experimental (`port/native/`, "The
native collision task" below). The solver is profiled, and a native task for it
is a candidate ("The solver: where a step's time goes",
"Candidate"); not started.
The layouts below were read from guest-memory snapshots of one physics step in
a fight (`SPURS_SNAP`, runtime patch; `tests/snap_read.py`) and live memory.
Offsets are bytes, guest values big-endian, pointers 32-bit. Nothing here is
game data.

## The tasks

| | BulletCD | BulletSolver |
|---|---|---|
| SPU image | spu_0003 (runtime image id 4) | spu_0004 (id 5) |
| CreateTask entry | 0xF63600 | 0xF8EC80 |
| ELF name | `PS3_SPURS_BulletCD_spu_release` | `PS3_SPURS_BulletSolver_spu_release` |
| Source | Bullet's SpuNarrowPhaseCollisionTask (asserts name SpuVoronoiSimplexSolver.cpp, btBoxShape.h) | Sony's libsync parallel solver |
| Per step | 2 runs, in parallel | ~71 runs in 5 commands, each sequenced by the PPU (below) |

Three slots each. A task is created per command, with a 16-byte argument
`[slot, shared block, input queue, output queue]`; the queues are 0x80 apart
per slot. Built with GCC 4.1.1 / SDK 3.00.

**Bullet version: 2.73.** The game's BVH records it (`m_bulletVersion` =
273), and its `btCollisionObject` lacks `m_anisotropicFriction`,
`m_hasAnisotropicFriction` and `m_contactProcessingThreshold`, which 2.74
added. Bullet 2.74 (zlib licence; a copy outside the
repository at `build/ref/bullet-2.74`) serves as the reference for the
algorithms, each layout checked against the snapshots; ported code keeps its
licence notice.

## Queues (SPURS-queue-like, 128 B)

`+0` and `+4` counters, `+8` entry size 0x10, `+0xC` depth 4, `+0x14` buffer,
`+0x1C` direction (2 PPU->SPU input, 1 SPU->PPU output), `+0x74` the SPURS
object. An entry is `[slot, command 1, task descriptor, slot]`; the task echoes
it into the output queue when done, which is what the PPU polls for
(`cellSpursQueuePopBody` from `func_00955BD4`).

## BulletCD inputs

`SpuGatherAndProcessPairsTaskDesc`, one per slot, fixed for the run (0x80 apart):
`+0` work-unit array, `+4` mutex (0), `+8` dispatcher (0), `+0xC` numOnLastPage,
`+0x10` u16 numPages, `+0x12` u16 taskId, `+0x14` u8 useEpa (1).

Work unit (16 B): `+0` u64 pair array, `+8` start index, `+0xC` end index
(starts step by 64). One unit per slot in the step looked at.

`btBroadphasePair` (16 B): proxy0, proxy1, algorithm, `internalTmpValue`
(2 = an SPU pair; algorithm 0 is skipped). Proxies are 0x40 apart.

`SpuContactManifoldCollisionAlgorithm`: `+0` vtable, `+4` dispatcher,
`+0x10` shape dimensions 0 (vec3), `+0x20` shape dimensions 1, `+0x30`
manifold, `+0x34` shape type 0, `+0x38` shape type 1, `+0x3C` margin 0,
`+0x40` margin 1, `+0x44` object 0, `+0x48` object 1.

`btCollisionObject`: `+0` vtable, `+0x10` world transform (3 basis rows of 16 B,
origin at `+0x40`), `+0x50` interpolation transform, `+0x90` interpolation
linear velocity, `+0xA0` angular, `+0xB0` broadphase proxy, `+0xB4` shape,
`+0xB8` root shape, `+0xBC` flags (1 static), `+0xC0` island tag, `+0xC4`
companion, `+0xC8` activation state, `+0xCC` deactivation time, `+0xD0`
friction, `+0xD4` restitution, `+0xD8` user pointer, `+0xDC` internal type
(2 rigid body), `+0xE0` hit fraction.

`btBoxShape`: `+4` shape type, `+0x10` local scaling, `+0x20` implicit half
extents, `+0x30` margin. `btBvhTriangleMeshShape` (type 19): `+0x10` local AABB
min, `+0x20` max, `+0x30` mesh interface, `+0x34` BVH, `+0x38` flag bytes.
`btCylinderShape` (type 13, the one live cylinder seen: Y up, implicit half
extents 0.66/0.96/0.66, margin 0.04): as the box to `+0x30`, then `+0x40`
local AABB min, `+0x50` max, `+0x60` AABB-valid flag (byte), `+0x68` up axis
(1). `+0x64` held 0 -- 2.74's layout would put the up axis there, so 2.73's
has a field more.

`btPersistentManifold`: 4 `btManifoldPoint`s of 0xB0 bytes from `+0`, then
`+0x2C0` body 0, `+0x2C4` body 1, `+0x2C8` point count, `+0x2CC` contact
breaking threshold (0.0121 for a debris box on the world mesh).
`btManifoldPoint`: `+0` local point A, `+0x10` local point B, `+0x20` world
point on B, `+0x30` on A, `+0x40` normal on B, `+0x50` distance, `+0x54`
combined friction (0.5 x 240 clamped to 10), `+0x58` restitution, `+0x5C..0x68`
part and index ids, `+0x70` applied impulse (the solver's), `+0x80` lifetime.
`+0x84`, `+0x88` and `+0x90..0x98` change exactly when a point is added or
replaced: fields this Bullet has and 2.74's point layout does not explain yet.

`btOptimizedBvh` (2.73; `btAlignedObjectArray` is 20 bytes: allocator,
size, capacity, data, owns): `+0x10` AABB min, `+0x20` max, `+0x30`
quantization, `+0x40` version (273), `+0x44` current node, `+0x48` use
quantization, `+0x4C` leaf nodes, `+0x60` contiguous nodes, `+0x74`
quantized leaf nodes, `+0x88` quantized contiguous nodes (size `+0x8C`, data
`+0x94`; 17,194 for the field's world), `+0x9C` traversal mode (0,
stackless), `+0xA0` subtree headers (size `+0xA4`, data `+0xAC`; 183).
Quantized node (16 B): u16 AABB min[3], max[3], i32 escape index (negative,
internal) or part/triangle index. Subtree header (32 B): u16 AABB min[3],
max[3], root node, size, 3 pad.

`btTriangleIndexVertexArray`: `+0x10` scaling, `+0x20` indexed meshes
(size `+0x24`, data `+0x2C`; one). `btIndexedMesh` (28 B): triangles, index
base, index stride, vertices, vertex base, vertex stride, index type
(3 = 16-bit). The field's world mesh: 8,597 triangles, 17,493 vertices.

## What the task does per pair (Bullet's SpuGatheringCollisionTask)

Skip unless `internalTmpValue == 2` with an algorithm and both proxies, and
unless one of the two objects is active (activation state not 2 sleeping or
5 disabled). Box vs box: `btBoxBoxDetector` on boxes of shape dimensions +
margin (2.74 source; `USE_PE_BOX_BOX` off). Mesh vs convex: swap so the
convex shape is first, walk the BVH's subtree headers and nodes against the
convex shape's AABB in mesh space, and run GJK between it and each
overlapping triangle (mesh scaling applied, triangle margin = the mesh's).
Contacts (SpuContactResult): dropped beyond the manifold's breaking
threshold; a new point near a cached one changes nothing (unlike desktop
Bullet, which replaces it); otherwise `addManifoldPoint`. A new point's
part/index ids, padding (`+0x84..0x8C`) and friction directions are left
uninitialised. At the end of the pair, if it has points: refresh all of
them against the objects' own transforms, and write the manifold back.
EPA is compiled only with `ENABLE_EPA`, which 2.74 never defines: the
penetration solver is then the Minkowski sampler, whatever `useEpa` says.

## What one step did (fight, 58 pairs)

- Pairs: 50 world mesh (type 19) vs box, 8 box vs box, 8 skipped. Every box is
  the same debris cube: implicit half extent 0.03, margin 0.04.
- Every pair: existing points refreshed (world positions, distance,
  lifetime + 1) and dropped when they drift past the breaking threshold
  (`refreshContactPoints`).
- Mesh vs box: new points found and added or replaced (39 points' local
  positions changed; counts went 0->1, 1->2, 2->3, 4->3 and so on).

So a native BulletCD needs, for these shapes: the BVH walk over the world
mesh and its triangle fetch, convex-vs-triangle and box-vs-box contact
generation (GJK with the penetration solver `useEpa` selects, or the box-box
routine; to be checked), and the manifold cache logic (add, replace, sort,
remove, refresh). The solver's command protocol is still to map.

## The native collision task (experimental)

**Experimental: off by default.** The lifted SPU task stays the default
(`DOTHEROES_NATIVE_CD` unset or 0). Native collision is selectable for
testing and comparison; it is not bit-exact with the game's SPU task (see
"How close it is"), has been checked only in the scenes below, and has not
been played by a person at length.

`port/native/bullet_cd.cpp` does one task's work over the game's data in
guest memory: box/box (`btBoxBoxDetector`), box or cylinder against box or
cylinder (GJK, Minkowski penetration solver), box or cylinder against the
triangle mesh (BVH walk, GJK per triangle), and the manifold cache. Bullet
2.74's narrowphase sources are compiled for it (`tools/build_deps.py` fetches
them, hash-pinned; `port/CMakeLists.txt` builds library `dotheroes_native`).
`port/native/bullet_cd_task.cpp` hooks it in through the runtime's native task
registry (`spu_workload_native_register`, runtime patch 0057): the task's
command is peeked from its input queue, popped, done, and echoed to its
output queue as the SPU task would. `DOTHEROES_NATIVE_CD`:

- `0` (default): the lifted SPU task.
- `1`: native (experimental).
- `2`: check. Native works the result out without writing it, the lifted task
  does the work, and each manifold either wrote is classified (below): point
  count, local and world points, normal, distance, friction, restitution,
  lifetime (not the impulses, which the solver may already be writing).
  `DOTHEROES_NATIVE_CD_DUMP=n` prints the first n manifolds of each kind of
  difference in full.
- `3`: self-check. As 2, but native rounding to nearest against native
  rounding toward zero (the SPU's mode).

A task with a pair it does not handle (another shape type, a BVH or mesh
layout it does not know) runs lifted in every mode, counted by reason in the
`[native-cd]` lines. In the fight and river scripts none does: every
collision task ran native (coverage 100%).

### How close it is

Unit: one manifold written by either implementation in one collision task,
per kind of pair. Classes, closest first (`bullet_cd_task.cpp`):
*identical* (every point in place within 1e-4 and 0.01 deg); *small
numerical* (in place within 2e-3 -- a sixth of the debris' 0.0121 contact
breaking threshold -- and 1 deg); *reordered* (the same points in other
slots); *other points* (different contact points, every normal within 5 deg
of one on the other side); *normal off* (a normal more than 5 deg from all of
the other side's); *point count* (with normals agreeing, or with a normal off);
*only one side wrote* the manifold (a contact one found and the other did
not, or one refreshed it and the other did not).

Check mode, one run each of the field and river scripts (`cls-*` in
build/baselines/2026-10-31-native-cd-classes), mesh/box (the bulk of the
pairs):

| mesh/box, % of manifolds | field native vs SPU | field self-check | river native vs SPU | river self-check |
|---|---|---|---|---|
| manifolds | 40,124 | 37,651 | 22,166 | 21,324 |
| identical | 90.91 | 91.38 | 85.49 | 86.21 |
| small numerical | 2.98 | 2.66 | 6.67 | 5.10 |
| reordered | 0 | 0 | 0.005 | 0 |
| other points, normals agree | 5.13 | 4.81 | 6.23 | 7.55 |
| normal >5 deg off | 0.42 | 0.58 | 0.72 | 0.35 |
| point count, normals agree | 0.38 | 0.44 | 0.75 | 0.71 |
| point count and normal off | 0.18 | 0.12 | 0.13 | 0.08 |
| only one side wrote | 0 | 0 | 0.005 (1) | 0 |

Box/box: 98-99.6% identical, 0.3-0.9% reordered, up to 1% other points, one
river manifold of 1,794 with a point count and a 7 deg normal off. Cylinder
pairs (few): 74-90% identical, the rest small numerical (up to 23% at the
river: GJK on a curved surface stops at slightly different points) or
other points (up to 2.9%); no normal off.

What the earlier figures were: "94-99% identical" was one field run, per
kind (box/box 98.7, mesh/box 94.4, cylinder 93.5 -- so 93.5-98.7), with
"identical" meaning every point within 1e-3 in place; "10.8-12.8%" was the
share of river mesh/box manifolds *not* within that tolerance, in two other
runs. Both lumped small numerical drift together with different contacts.
Separated, the genuinely different contacts (other points, normal off, point
count, one side only) are 6.1% of mesh/box manifolds in the field run and
7.8% at the river; materially different normals (normal off, with or without
a point count difference) 0.6% and 0.85%; manifolds one side wrote and the
other did not, 0 and 1 of 22,166.

The dumped differences are degenerate contacts: two identical, aligned
debris cubes give exactly equal penetration on both cubes' face axes, so
which cube's face carries the points (offset by the depth) is decided by the
last bit; a cube flat on the ground has no unique GJK witness point; the
Minkowski sampler picks between directions of equal depth, mirror images
(normal (+0.013, -0.9999) against (-0.013, -0.9999)) or opposites: a freshly
spawned cube whose centre lies exactly on the ground plane is half embedded
both ways, and the two sides can take opposite normals (native +y, SPU -y in
three river dumps); `sortCachedPoints` breaks area ties between symmetric
corner points. The game's SPU code is the same algorithm here (checked in the
lifted image: 42 sample directions plus the triangle's two face normals, none
for a box), so these are ties broken differently, not missing logic. Whether
the opposite normals bias debris: mesh/box contact points whose normal pushes
the box *down* into the ground are 66 of 13,540 (0.49%) of the SPU's and 79 of
13,564 (0.58%) of native's on the same inputs (check mode, river) -- within
counting noise -- and in native's own river run 1.0% while debris spawns,
falling to 0.28% over the run, not growing as sinking boxes would make it
(build/baselines/2026-10-31-native-cd-down); in another native river run
(the player build's) 1.0% over the whole run. **Unresolved:** similar
aggregate rates do not show equivalent behaviour. They say nothing about
which contacts flip, whether the same pieces of debris are affected, or
what a flipped contact does to that debris over the following steps; the
remaining contact differences (above) are not explained, only bounded.

The lifted task reproduces the SPU's arithmetic exactly (truncating, no
denormals, its estimate-and-refine reciprocal and square root); host Bullet
rounds to nearest. The self-check -- native against itself, only the
rounding changed -- differs in the same classes, about as often and by as
much (table). That **supports** rounding sensitivity in degenerate contacts
as the explanation of the native/SPU differences. It does **not prove** the
port free of bugs: a porting error that shows up rarely, or only in the
classes rounding also disturbs, would hide inside these rates (river normal
off is 0.72% against the SPU but 0.35% in the self-check -- small numbers,
one run each, unexplained). What would narrow it: replaying the kept
snapshots offline with native computing the SPU's way for the decisive
values (the triangle normal's normalisation, which settles the exactly
embedded ties above, can be reproduced bit for bit with the runtime's SPU
float helpers), and comparing decisions one by one. Running native toward
zero (`DOTHEROES_NATIVE_CD_FP=rz`) brings it closer to the SPU in an earlier
field run (mesh/box not within 1e-3: 3.7% against 5.6%) but not to it.

### What it gained (A/B, 2026-10-31)

Same build, save and drive scripts (`fight_field`, `fight_river`), frame
dumps off, lifted and native alternating, three runs each; per physics step
over the whole run (`tests/phys_breakdown.py`), frames by flip request
(`tests/frame_times.py`):

| per step, ms | field lifted | field native | river lifted | river native |
|---|---|---|---|---|
| collision exec | 1.25 | 0.07 | 2.74 | 0.13 |
| solver exec | 2.28 | 2.60 | 3.17 | 3.48 |
| step span | 3.60 | 2.78 | 6.02 | 3.76 |
| main thread waiting | 2.81 | 1.95 | 5.21 | 2.86 |

The collision task is ~95% cheaper and the main thread waits 30-45% less
per step; the solver is now 93% of the physics time. (Its means came out
higher native, but inside the run-to-run spread: lifted river runs ranged
2.59-3.87 ms a step, native 3.33-3.56.) Frame time moved
little: at the river, frames with a physics step averaged 24.2 ms (25.6
lifted), 95th percentile 34.2 (35.6); frames over 25 ms 121 (123), fps 57.3
(57.1). The field fights are within run-to-run noise (one native run had no
physics in the traced window). In the river's 25-34 ms frames the main
thread's physics wait fell from 6-12 ms to 3.6 ms; what holds those frames
now is the RSX walker, ~19 ms recording ~935 draws (~630 in an ordinary
frame) -- the debris' drawing.

Earlier estimate, for comparison: BulletCD 0.8-1.5 ms a run; the solver ~24
phases at ~70 us each; a physics step ~2.8 ms, and a frame that catches up
runs 2-3 steps.

## The solver: where a step's time goes (2026-10-31)

Three sets of river fights, kept in build/baselines/2026-10-31-solver-profile
(traces, profiles, the analysis scripts and their output in `analysis/`):
**A** two on-screen runs, native collision, each solver task marked with its
command (`DOTHEROES_PHYS_STATS=1`, frame-trace kind 20), 723 steps,
`tests/solver_breakdown.py`; **B** the on-screen campaign's river fights
(build/baselines/2026-10-31-onscreen, 6 lifted / 5 native runs, ~16k frames
each mode); **C** three runs with `DIAG_PROF` sampling the main thread and the
SPU workers only (`DIAG_PROF_THREADS`), sample times on the trace's clock
(`profile.times`). The sampler slows what it samples: C is for shares and
call paths, A and B for times. Frame times below are guest frames (flip
request to flip request) and present-call completion intervals, not display
timing.

**The PPU drives the solver the way Bullet 2.74's
`btParallelSequentialImpulseSolver::allSolved` does.** Same commands, same
order, and the PPU waits for every task of a command before the next:

| per step (A) | runs | span ms | main thread waiting ms |
|---|---|---|---|
| (PPU builds and sorts the hash) | | 0.12 before the first run | |
| 1 set up bodies | 1-2 | 0.09 | 0.07 |
| 2 manifold setup | 8, three slots pipelined | 0.47 | 0.39 |
| 4 iterate | 30: 10 rounds x 3 | 1.55 | 1.24 |
| 5 copy bodies back | 1-2 | 0.06 | 0.04 |
| 6 warm-start write-back | 30: 10 rounds x 3 | 0.97 | 0.66 |
| step (collision start to last solver end) | ~73 with collision | 3.37 | 2.51 (0.11 of it on collision) |

Frames with 121+ collision pairs are nearly the same per step (3.51 / 2.63
ms). The rest of the span, ~0.85 ms, is the main thread working: mostly
issuing ~71 tasks. An iterate round: the PPU creates the three tasks 10.7
and 20.2 us apart (median), a run takes 149 us, the round 171 us, and the
next round starts as the last run ends. Write-back: a run 70 us, a round
86 us. Bullet 2.74 issues the write-back once per solver iteration
(`numIterations` = 10), and each pass does the same thing: for every
contact, read the 16 bytes at its manifold point, store min(3,
appliedImpulse) in the first float, write them back. The impulses do not
change between passes, so passes 2-10 rewrite the values pass 1 wrote
(Bullet 2.74 source; the game's passes match it in count, the store not yet
checked in the lifted code).

**Inside the tasks (C, samples in spu_0004 by the command running):**

| command | lifted Bullet code | channels, DMA, copies | lock line, atomics, coherence |
|---|---|---|---|
| 4 iterate (995 samples) | 66% (10 of it float helpers) | 23% | 7% |
| 6 write-back (518) | 30% | 48% | 15% |
| 2 manifold setup (275) | 79% | 15% | 1% |

Iterate is mostly the lifted constraint math; write-back is mostly emulated
DMA (two 16-byte transfers and waits per contact) and the cell-claim spin
lock. The collision task's lifted runs fell ~20x when made native (2.74 to
0.13 ms a step, above), which is the only measured guide to what native
solver runs would take.

**The main thread's wait (C):** in a step it is either creating tasks or
polling the slots' output queues (`cellSpursQueuePopBody`, non-blocking, round
the three slots, from `func_00955BD4`). The solver path is `func_00941668 ->
func_00952298` (issue `func_00955328`, poll `func_009552BC`); collision's is
`func_0095A73C` (sampled in the collision window only). Creating a task costs
~5.7 us in `cellSpursCreateTask`, about half of it a `printf` and a
`getenv("SPURS_TASKSET_TRACE")` on every call.

**The frame (B, river fights):**

| steps in the guest frame | frames lifted / native | guest frame ms | main thread busy | queue wait | present interval > 25 ms |
|---|---|---|---|---|---|
| 0 | 92.3% / 88.2% | 16.85 / 16.85 | 10.3 / 10.2 | 0 | 0.3% / 0.2% |
| 1 | 4.3% / 9.9% | 19.15 / 18.28 | 12.9 / 13.9 | 4.9 / 3.2 | 4.5% / 4.3% |
| 2 | 2.8% / 1.9% | 28.63 / 22.08 | 14.8 / 14.6 | 13.4 / 7.1 | 79.5% / 45.8% |
| 3+ | 0.6% / 0.0% (1 frame) | 47.7 / 33.9 | 17.5 / 18.7 | 29.6 / 14.8 | 100% |

"Busy" is the guest frame minus the waits the trace records (syscalls,
usleep, HLE waits, queue waits); it includes waits the trace does not, such
as the command-buffer wrap below. Busy p90 in one-step frames is 18.4 /
19.5 ms. Steps per frame follow the previous frame's length: after a guest
frame of 25 ms or more, 71% (lifted) / 50% (native) of frames with physics run
two or more steps; after one under 18 ms, 14% / 3%. The game catches up in
steps, so a slow physics frame tends to bring another.

**The heaviest frames (C):** physics frames whose busy time is 14 ms or more
(240 of 608): guest frame 23.5 ms, present interval 20.6. Sampled main-thread
time per frame: other game work 10.2 ms (7.8 in frames without physics), the
**command-buffer wrap wait 4.1** (`cellGcm_fifo_recycle`: the title's ring is
full and the runtime sleeps in 1 ms steps until the RSX walker has taken the
jump back; 0.2 ms in frames without physics), the solver's issuing and
polling 4.0, collision's 1.4 (both collision modes pooled). The 4.1 is
biased: "busy" counts the wrap wait, so picking frames by busy time picks
frames with one. Traced directly (docs/HANDOFF.md, "The command-buffer wrap
wait"), it is ~4.3-5.2 ms in guest frames of 17.5-33 ms that have one.

**Upper bound, not a forecast:** taking the main thread's whole physics wait
away -- 2.51 ms a step (A) -- would leave one-step frames at ~15.1 ms on
average with a busy p90 still ~19.5 ms, two-step frames at ~15.0 on average;
the heaviest frames (C), without the solver's and collision's issuing and
polling at all, at ~18 ms. Not even that clears 16.67 ms in the
debris-heavy frames: the wrap wait and the extra game work stay.

## Solver structures, checked (2026-11-01)

What the native solver code uses, each checked against the game's own memory,
not taken from Bullet 2.74: `DOTHEROES_SOLVER_SNAP` captures a step (guest
memory as the first task of each command is created, the tasks' descriptors,
and the spin lock and cell masks as each iterate and write-back task starts and
returns; port/native/bullet_solver_task.cpp), and tests/solver_layout.py checks
it. Captured river-fight steps (build/snap, game memory, local only): 150 and
300 of one run (the layouts; taken before the lock was read as tasks ran), 150
of a second (all 22 checks) and 1 and 2 of a third (1-2 cells in use, many
tasks finding nothing: all checks; one note, below). 71-302 contacts a step.

| structure | as the game has it | how it was checked |
|---|---|---|
| descriptor (0x90 a slot) | +0 command, +4 task id (= slot), +0x10 hash, +0x14 solver bodies, +0x18 contact constraints, +0x20 body offsets, +0x30 command data: bodies (+0x30 first, +0x34 count, +0x38 rigid bodies), manifold setup (+0x34.. counts, +0x40 manifold holders, +0x48.. solver info), iterate / write-back (+0x30 spin lock, 128-aligned; write-back also +0x40) | the words each command's tasks are created with |
| hash: cells | +0x870, 128 x 16 bytes: local bodies, body offset, manifolds, manifold offset, contacts, contact-constraint offset (x3), constraints, constraint offset (16 bits each) | the offsets are the PPU's running sums of the counts |
| hash: dependency matrix | +0, 128 rows of 4 words, symmetric, diagonal set | |
| hash: cell masks | +0x800, row 0 the processed cells (preset to the cells with nothing to do as each round starts), row 1 + task id what a task holds; rows 4-6 unused | at round starts, and as tasks start and return |
| contact constraint | 0x90 bytes, three a contact (the contact, two friction rows); applied impulse +0x54; pointer back to the manifold point **+0x78** (2.74: +0x70); friction rows' pointer 0 | the pointer and impulse fields that name and give exactly the floats write-back pass 1 changed |
| spin lock | libsync's ticket lock: +0 now serving, +2 next ticket, 16 bits each; 0/0 as a round starts, equal when released; one ticket a claim plus one last per task | read as tasks run (the lock is freed and its memory reused by the step's end) |
| write-back | for each contact, the first float of the 16 bytes at its pointer = min(3, applied impulse); the other 12 bytes untouched; every contact names its own 16 bytes | pass 1's changes in the snapshots; after pass 10 in the running game (`DOTHEROES_NATIVE_SOLVER_CHECK=1`, compared as the step's last write-back task finishes): 21,647 contacts in 200 steps all as pass 1 leaves them -- passes 2-10 rewrite the same values (redundant: shown, not relied on; all ten still run) |
| solver body (copy-back) | 0x40 bytes, as many as the descriptor's bodies: linear velocity delta +0x0, angular +0x10 (4 floats each), inverse mass +0x24 (2.74's layout) | the layout under which every changed velocity float is the old one plus the delta (tests/solver_copyback_layout.py) |
| rigid body (copy-back) | the game's 2.73 btRigidBody, by the descriptor's pointer list (+0x38): linear velocity +0x130, angular velocity +0x140 (4 floats each, the 4th added too), companion id +0xC4 (set to -1); nothing else in its first 0x200 bytes changes | the bytes that changed between the copy-back's start and the write-back's |
| copy-back arithmetic | velocity + delta in single precision **rounded toward zero** (the SPU's), only where the inverse mass is above zero; a body with deltas too small to change a sum so rounded stays as it was | 318 of 318 changed floats match toward zero, 244 to nearest (step 300); four steps |
| iterate: a cell's bodies | the cell's u32 index list at descriptor +0x20 + 4 x cell +2, cell +0 entries, each a solver body at +0x14 + 0x40 x index; copied in, solved, every one written back whole | tests/solver_iterate_layout.py: bodies no list names unchanged; only the deltas' x, y, z (+0x0..+0x8, +0x10..+0x18) change, never in a body of zero inverse mass (+0x24) |
| iterate: a row | relpos1 x normal +0x0, normal +0x10, relpos2 x normal +0x20, angular components A +0x30 and B +0x40, impulse +0x54, body ids (local to the cell's list) +0x58 / +0x5C (2.74: +0x68 / +0x6C), friction +0x60, 1 / jacobian diagonal +0x68, rhs +0x7C, cfm +0x80, limits +0x84 / +0x88; angular factor of a solver body +0x20 | the lifted row solve (SPU 0x3DB8-0x4258) read field by field; in the data: a round changes a normal row's impulse only, a friction row's impulse and limits only; the ids index the cell's list, the same in a contact's three rows |
| iterate: a round | per cell, in packets of (0x25800 - the index list rounded to 16 - 0x40 x bodies) / 0x1B0 contacts: every normal row, then each contact's two friction rows with limits friction x -/+ the contact's impulse; each row solved in the SPU's single precision (truncation, denormals as zero) in the task's own order of operations; typed constraints loaded and stored unchanged | friction limits after a round = friction x -/+ impulse truncated, every row; the native iterate on captured rounds = the game's, byte for byte (below) |

After the solver, before the next step's collision tasks, the game changes a
few of those 16-byte targets again (2 of 71 contacts in captured step 1; 116
contacts in 9 of 600 steps of one run): a manifold released and made again for
a new pair starts its points at zero impulse. A check of the write-back has to
look before that, as the step's last write-back task finishes.

The cell claim (as each task asks for a cell: its row cleared, then under the
lock the first cell neither processed nor dependent on a held one, the task's
row set to that cell's dependencies, the cell's processed bit set; a round ends
with every cell processed, every row clear and the lock released) matches
Bullet 2.74's getNextFreeCell on everything the captures show.

## Candidate: a native solver task (done: write-back, copy-back, iterate)

A candidate for the solver's share of slow frames, to be weighed against the
other contributors once they are measured -- not an established requirement.

**What.** Native code for the BulletSolver task (runtime image 5) through
the hook the collision task uses (`spu_workload_native_register`): peek the
slot's command, pop it, do the command's work on guest memory, push the
completion to the output queue, return 1. Port the command handlers of Bullet
2.74's `SpuParallellSolverTask.cpp` (zlib) -- body setup, manifold setup,
iterate, copy back, write-back -- with the DMA replaced by big-endian reads
and writes of the mapped structures, as `bullet_cd.cpp` does. The PPU side
stays lifted and unchanged; per command a task can still fall back to the
lifted image (return 0), so commands go native one at a time. Experimental and
opt-in (`DOTHEROES_NATIVE_SOLVER`), independent of the collision mode; native
collision stays experimental with its open contact differences, and its
snapshots and failure inputs stay as they are.

**Cells, ownership and completion.** Iterate and write-back share out the
hash's cells through `getNextFreeCell`: under the guest's spin lock (libsync's
`CellSyncMutex` at the descriptor's `m_spinLockVar`, which the PPU
initialises before each round) a task re-reads the masks, takes the first
cell that is neither processed (bit set in `m_currentMask[0]`, which the PPU
presets for empty cells) nor dependent on a cell another task holds (its row,
`m_currentMask[1 + task]`, against the dependency matrix), sets the cell's
processed bit and its own row, and unlocks; it clears its row before asking
for the next cell. A cell is therefore owned by the one task that set its
processed bit, and is never processed twice. A native task would claim cells
the same way -- the same mutex, the same bits, written to guest memory -- so a
lifted task running in the same round (a fallback) still sees every claim.
When the native task finishes before the other two take the lock, it has
claimed every cell in index order; the other two each pop their own command,
find `m_currentMask[0]` all ones, and push their own completion. Nothing
changes for the PPU: three tasks created per round, three completions, and its
wait for all three before it resets the masks for the next round (the
barrier). The order is one the original allows (one SPU ahead of the other
two) and repeatable; with a lifted task in the round it is whatever the race
gives, as now. What has to be shown on the game's own code, not only the 2.74
source: that the mutex is the one at `m_spinLockVar`, the mask layout, and
that the task leaves its row cleared and the processed bits set as the
lifted task does.

**Repeated write-back.** The ten write-back passes look redundant (each sets
the first float of every contact's 16 bytes to min(3, appliedImpulse) and
writes the other 12 back). Dropping passes 2-10 needs, first: the inputs
unchanged across the passes (each constraint's `m_appliedImpulse` and
`m_originalContactPoint`), and all 16 target bytes identical after every pass
-- checked per pass in a running game by hashing them -- so that nothing
else writes them in between (the read-modify-write would otherwise overwrite
it, and dropping a pass would change behaviour); and no consumer of the
intermediate state: nothing that reads those bytes, or the masks and lock the
passes use, between passes. If every pass leaves the same bytes, no reader
can tell the passes apart, but the PPU's own handling of the passes (masks,
lock init, its count of completions) stays as it is.

**Steps, each measured and checked before the next:**
1. Layouts of the game's 2.73 structures against the 2.74 source, from
   snapshots -- done for what write-back uses ("Solver structures, checked");
   still to do for `btSolverBody`, the rest of `btSolverConstraint`, the
   manifold and constraint cell holders, and the `btRigidBody` fields body
   setup and copy back touch.
2. Write-back natively -- done (below); copy-back done (below).
3. Iterate -- done (below); body setup turned out not to be needed first.
4. Manifold setup (deterministic per task), body setup.

**Done so far: write-back** (`DOTHEROES_NATIVE_SOLVER=6`, experimental, off by
default; port/native/bullet_solver.cpp). Each native task waits for its slot's
command, pops it, claims cells under the round's ticket lock and masks as the
lifted tasks do, writes min(3, impulse) for each contact of its cells, and
pushes its own completion; the PPU still creates 3 tasks a round for 10 rounds
and waits for each round. Checked: offline on captured inputs
(tests/native/bullet_solver_snapcheck.cpp: the native write-back on the
snapshot before pass 1 = the game's pass 1, 16 bytes per contact, 146 of 146;
the hash and lock left as a round leaves them); in the running game against
the lifted task (`DOTHEROES_NATIVE_SOLVER_CHECK=1` with the lifted write-back:
21,647 contacts, none different) and as itself (74,001 contacts in 600 steps,
none missed; 20,000 native tasks, none fell back to the lifted image).
Measured on screen (build/baselines/2026-11-01-native-writeback-ab; river
fights, lifted collision, the default ring, frame dumps off, five pairs; one
native run had no fight and is left out): the write-back phase 1.04 -> 0.21 ms a
step (0.60-1.39 -> 0.20-0.21 at every contact load), its tasks 89.9 -> 2.2 us
(median); the solver's phases together 0.5-1.6 ms a step shorter at matched
contact points, iterate unchanged. **Frames: no gain shown.** Present-call
completion intervals over 25 ms 2.90% -> 3.58%, over 33 ms 1.02% -> 0.86%, guest
frame p99 32.0 -> 30.8 ms. The native arm's fights carried 6-11 more awake
bodies at the same contact load; their lifted collision took 0.45-1.2 ms a step
longer -- as much as the write-back saved, or more -- and they ran more
catch-up steps (43% of physics frames with 2+ steps, against 38%). The
write-back cannot change the collision already done in the same step; it can
change later steps (what it writes is warm-start state) and the schedule (when
tasks finish and the PPU goes on), so the longer collision is not shown to be
fight variation alone. What it writes was identical to the lifted task's, which
leaves timing as the route. With that much difference between the arms, ~1 ms
a physics frame from the solver does not show through in five pairs.

**Again with native collision in both arms** (build/baselines/2026-11-01-native-
writeback-cd1-ab; on screen, default ring, only the write-back switched, all ten
rounds in both; four fight runs an arm, one run of each had no fight and is
left out). Steps at matched contact points 0.33-1.11 ms shorter in every band
(write-back 0.70-1.37 -> 0.21 ms). Physics frames at matched contact points in
the frame: guest frame 17.31 -> 17.22, 18.27 -> 17.60, 21.50 -> 20.18 ms (light,
medium, heavy), present-call completion intervals over 25 ms 3.8% -> 3.2%, 7.0%
-> 3.4%, 33.1% -> 15.1%, catch-up steps a physics frame 1.14 -> 1.07, 1.17 ->
1.12, 1.77 -> 1.38. Pooled over every frame the arms differ in workload (two
native-arm fights at 216 and 252 contact points a step, the other arm's at
78-166): over 25 ms 1.15% -> 0.98%, over 33 ms 0.18% -> 0.22%, guest frame
over 18 ms 5.58% -> 6.65%, frame start -> present median 26.99 -> 27.56 ms --
not comparable as they stand. A first batch of these runs is not counted: from
its second run on the game ran at ~35 fps in both arms behind another window
(foreground share 0); tests/boot_smoke.py's guard now also stops a run whose
window is covered, and TOPMOST=1 keeps it on top. Out of focus but uncovered,
runs keep their speed (58 fps).

**Does native collision make fights comparable?** Checked, not assumed
(tests/workload_variation.py over every fight run on the default ring: 16
lifted, 11 native). The workload varies as much either way -- between runs,
the coefficient of variation of awake bodies 0.38 / 0.35, pairs 0.39 / 0.34,
contact points 0.46 / 0.46 (lifted / native); a fight is light or heavy by what
happens in it. What native collision steadies is the cost and the catch-up:
collision 3.40 +- 0.92 ms a step against 0.14 +- 0.02, a step 6.67 +- 1.70
against 3.32 +- 0.57 (CV 0.26 / 0.17), steps a physics frame CV 0.21 / 0.05,
physics frames with two or more steps 37% / 17%. (The write-back-off runs alone
suggested the workload steadied too -- 0.41 / 0.19 for bodies -- but that sample
had three heavy lifted fights and no heavy native one.) So comparisons still
have to match the workload; native collision makes what is left of a step
mostly the solver, which lets a solver change show at matched load.

**Its cost on identical inputs** (DOTHEROES_SOLVER_BENCH, 547 steps of a run
with the lifted write-back: before the lifted tasks did each step, the native
write-back was timed on that step's own inputs and everything it changed put
back; with the write-back check on as well, the game's result was untouched
over 200 steps). Lifted, the phase 504 / 656 / 784 us at 1-50 / 51-100 /
101-200 contacts (round 1, which does the work, 53-81 us; the 30 tasks' CPU
1.36-2.22 ms); native, one task doing the same work 0.5 / 1.2 / 1.8 us. As the
game runs it, the native phase is ~0.21 ms a step whatever the load: the PPU
creating 30 tasks and their dispatch, which a native write-back does not
remove. **Gameplay benefit: unresolved.** At matched load physics frames were
0.1-1.3 ms shorter (the phase gives back 0.5-1.2 ms a step, 1.1-1.4 steps a
physics frame), in four fights an arm; over whole fights the difference is
within what one fight differs from the next.

**Copy-back** (`DOTHEROES_NATIVE_SOLVER=5`, with 6 as `5,6`; experimental, off
by default). Its cost before: 0.053 ms a step (1.2 tasks of up to 64 bodies;
the main thread waiting 0.043 of it) -- at most that to gain, below what a
frame comparison resolves; done for what it checks (the solver body and rigid
body layouts body setup and iterate need too) as much as for time. Each
native task takes its range of bodies (+0x30, +0x34), adds the solver body's
deltas to the rigid body's velocities rounded toward zero as the SPU does --
rounding to nearest would have matched only 65-77% of the changed floats --
and sets the companion id to -1. Checked: the layouts on four captured steps
(tests/solver_copyback_layout.py); offline on the same captured inputs, the
native copy-back = the game's byte for byte (56-64 bodies a step, the first
0x200 bytes of each; all four steps had one copy-back task); in the game, the
native model against the lifted copy-back (`DOTHEROES_NATIVE_SOLVER_CHECK=1`,
compared as the first write-back task is created: 32,005 bodies in 400 steps,
none different) and the native copy-back as itself (21,768 bodies, none).
Compute only, on identical inputs (`DOTHEROES_SOLVER_BENCH`, 698 tasks): the
native copy-back 0.2-0.9 us a task; the lifted task as dispatched 39-50 us,
nearly the same for 1 or 64 bodies -- its cost is the task's own overhead.
In the game, dispatched (build/baselines/2026-11-01-native-copyback; on screen,
native collision, default ring, native write-back with all ten rounds in both
arms, only copy-back switched, three fight runs an arm): the copy-back phase
0.06 -> 0.01 ms a step, its tasks 49.5 -> 3.3 us. Steps at matched contact
points differed by -0.13 to +0.35 ms (iterate differing with the fights; the
copy-back arm ran more catch-up steps, 18% of physics frames against 10%), and
physics frames at matched load by +0.05 to +1.06 ms the other way -- a change of
0.05 ms a step does not show through that; **gameplay effect not resolved**.

**Iterate** (`DOTHEROES_NATIVE_SOLVER=4`, with the others as `4,5,6`;
experimental, off by default). What it is in the game, mapped from the lifted
task (the row solve at SPU 0x3DB8-0x4258, the loops at 0x8770 and 0x87D0, the
packet size at 0x85DC / 0x31F8) and checked in captured steps ("Solver
structures, checked"):
- *Rounds and completion.* Ten rounds a step, the PPU creating three tasks a
  round and waiting for all three before it resets the cell masks and the
  ticket lock for the next (the barrier). A native task keeps that protocol as
  the write-back's does: its slot's command popped, cells claimed under the
  same lock and masks, its own completion pushed; the ten rounds and the
  PPU's side are unchanged.
- *Inputs, outputs.* Per claimed cell: its index list (descriptor +0x20) into
  the solver bodies (+0x14), the bodies copied in and every one written back
  whole; its contact rows (+0x18, three a contact), of which a round writes
  the impulse (+0x54) of every row and the limits (+0x84, +0x88) of the
  friction rows; its typed constraints loaded and stored back unchanged.
  Nothing else.
- *Order.* In a cell, contacts in packets the local store allows; in a packet,
  every contact's normal row, then each contact's two friction rows with
  limits friction x -/+ the contact's impulse as the normal rows left it. Each
  row in the task's own order of operations in the SPU's single precision
  (the runtime's exact helpers, spu_helpers.h: truncation, denormals as zero,
  the SPU's compares) -- bit for bit what the lifted task computes. Between
  cells: two cells listing the same body depend on each other (the matrix
  keeps them apart in time), and which goes first decides the result; one task
  alone takes the cells in index order, a schedule the protocol allows. The
  lifted round races three tasks for the cells; `DOTHEROES_SOLVER_SERIAL=1`
  makes them take turns, which gives that index order for reference. In the
  game three native tasks race the same way (how the cells split between
  them is not measured; the check below finds the result unchanged).
- *Body setup* is not needed first: iterate reads the solver bodies and rows
  the lifted setup and manifold setup leave, as they are. Deferred.

Checked, the same as the game's, bit for bit:
- offline, on identical captured inputs (tests/native/bullet_solver_snapcheck.cpp
  iterate; build/baselines/2026-11-01-native-iterate/analysis): three steps of
  2-26 cells and 71-201 contacts (4-44 between two moving bodies), one round
  and all ten; two steps chosen for a body in two cells' lists
  (`DOTHEROES_SOLVER_SNAP_SHARED`, the lifted tasks taking turns; 2 and 4
  cells, 249 and 93 contacts, 226 and 68 between moving bodies, 7 and 3 shared
  bodies), each of the ten rounds from its own snapshot and all ten from the
  first: every solver body (0x40 bytes) and every contact row (0x90 bytes).
  Nine rounds against the ten-round result fails, as it must.
- in the game (`DOTHEROES_NATIVE_SOLVER_CHECK=1`: each round's result against
  the native iterate run on that round's inputs as it starts): the lifted
  tasks taking turns, 14,000 rounds (1,400 steps, two runs); as usual, 2,000
  rounds; none different, with 69 bodies in two cells' lists over 34 steps
  and one cell larger than a packet among them. The native iterate itself,
  6,000 rounds (600 steps): none different from the model (309 times a native
  task waited for a cell another held).
- over repeated steps: in the native run every round of every step matched the
  model on that step's own inputs, so step after step the native iterate does
  what one lifted task taking the cells in index order would, and no
  difference can build up from step to step. The run's workload stayed with
  the lifted check runs' (109 contacts a step against 92-108); window grabs
  of the A/B below show nothing out of place.

Compute only, on identical inputs (`DOTHEROES_SOLVER_BENCH`, 3,660 rounds):
the native iterate as one task 64 us a round (median; 70 us at 201-400
contacts a step); the lifted round as dispatched 252 us (275), its slowest
task about the whole round and the three tasks' CPU 716 us (the two without
work spin on the lock until every cell is processed).

In the game, dispatched (build/baselines/2026-11-01-native-iterate-ab; on
screen, river fights, native collision, native copy-back and write-back with
all ten rounds, default ring in both arms, only iterate switched; four fight
runs an arm, one run without a physics step and three that ran beside a trace
analysis set apart): at matched contact points the iterate phase went from
0.90, 1.27, 1.53, 1.98, 3.24 to 0.24, 0.35, 0.44, 0.53, 0.67 ms a step (1-50
to 251+ points), the whole step from 1.67-5.02 to 1.08-2.27 ms, the main
thread's wait from 1.06-4.23 to 0.43-1.58 ms; iterate's task runs 129 -> 28 us
(median). Catch-up about the same (22.7% of physics frames with 2+ steps
against 23.6%). The fights were not alike: the native arm's had 159 contact
points a step against 109 (78 awake bodies against 56; per run 103-216
against 97-119), and frames did not follow the steps. Physics frames at
matched points: 17.14, 17.30, 16.65 ms (native) against 16.92, 16.89, 17.56
(guest frames; 1-100, 101-250, 251+ points), present-call completion
intervals over 25 ms 1.5, 3.1, 3.7% against 0.5, 2.0, 10.3%; per run the two
arms overlap (physics frames 16.85-17.90 against 16.52-17.34 ms). The native
arm also waited longer for room in the command ring (20.3 s against 16.9 s in
all; 2.23 against 2.16 ms in a one-step physics frame): its heavier scenes,
or the time the solver no longer takes moving to that wait -- not separated.
**Steps faster, gameplay effect not resolved.** Not extended with more noisy
runs: the cost on identical inputs is the bench above.

**Validation** (the plan; as done: write-back and copy-back compared field
for field, iterate byte for byte, offline and in the game, above --
`DOTHEROES_SOLVER_SERIAL` is the switch that serializes the iterate tasks,
`DOTHEROES_NATIVE_SOLVER_CHECK` the check mode; the native results matched
exactly, so there were no numeric differences to bound).
- Captured inputs: `SPURS_SNAP` extended to take guest memory before a
  command's first task and after its last, in capture runs whose solver
  tasks run one at a time (a switch that serializes image 5, so the lifted
  task claims every cell in index order like the native one). An offline
  checker like `bullet_cd_snapcheck` runs the native command on "pre" and
  compares every field it writes with "post": structural mismatches (a body,
  constraint or manifold point written or not written) counted apart from
  numeric differences (max absolute and relative per field).
- A check mode in the running game, as `DOTHEROES_NATIVE_CD=2`: native
  computes into a copy, the lifted task does the real work, differences
  logged.
- Sustained heavy-debris play: on-screen river fights and grass-cutting
  debris (`grass_cut.drive`), both collision modes, frame dumps off, the
  intended-fight classification and workload counts as before. Behaviour
  checks beside timing: debris settling (awake bodies over time), no velocity
  blow-ups, window grabs reviewed.

**Speedup (written before any native solver command; kept for what it
weighed).** Since measured, per command, above: write-back 1.04 -> 0.21 ms a
step, copy-back 0.06 -> 0.01, iterate 0.90-3.24 -> 0.24-0.67 at matched
contact points, none with a gameplay effect shown. At the time no native
solver run had been timed. The only
guide was the collision task, whose runs fell ~20x when made native; the
solver's could fall much less. What is measured is what a native task cannot
remove: the PPU's issuing, ~20 us per round to create three tasks (~0.4 ms
a step for the 20 iterate and write-back rounds), ~0.75 ms a step for all
~71 tasks -- measured before task creation lost its per-task log line and
getenv (ps3recomp patch 0064), which halved the round's 20 us. *If* native runs fell as far as collision's, the solver's phases
would go from ~3.1 to ~0.9-1.3 ms a step (1.8-2.2 ms of the 2.51 ms upper
bound, at this load): one-step frames from 18.3 to ~16-16.5 ms on average,
p90 still over 16.67, the heaviest from 23.5 to ~20.5-21. That is a
what-if, not a forecast. The other contributors to the same frames, largest
first: the command-buffer wrap wait (with neither it nor the physics wait,
761 of 839 frames of 18 ms or more would be under 16.67 ms; without the
physics wait alone 358, without the wrap wait alone 450 -- upper bounds,
docs/HANDOFF.md; a lap ending at the walker at the wrap, `GCM_WRAP_LAP=1`,
removed 85-94% of that wait but did not shorten frames with lifted collision:
off, experimental); the extra game work in debris frames (~2.4 ms over frames without physics); the
per-task issuing, which only replacing the PPU's solve (`func_00952298`)
natively would remove; lifted collision, the default, ~1.7 ms a step more
than native.

**Alternatives weighed.** Doing the write-back once instead of ten times:
~0.6 ms a step if the conditions above hold. Faster runtime DMA and
lock-line emulation: helps every SPU task, but those are a quarter to a half
of the solver's run time.

## Method

`SPURS_SNAP=F63600:F8EC80:<n>:<prefix>` writes guest memory (committed pages
below 0x20000000, ~170 MB) when the n-th slot-0 collision task of a run is
created, and again when the next solver task is: before a step's collision and
after it. Snapshots are game memory: keep them out of git (`build/snap/`).
`tests/snap_read.py` reads them (`Snap(path).u32(ea)`, `.dump(ea, n)`).
A native task can be checked offline the same way: run it on the "pre"
snapshot and compare its manifolds with "post".
