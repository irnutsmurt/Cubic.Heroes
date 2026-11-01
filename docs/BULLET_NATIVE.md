# Native Bullet: what the game's physics tasks do (2026-10-31)

Replacing 3D Dot Game Heroes' two Bullet SPU tasks with native code, one at a
time. The collision task is done (`port/native/`, "The native collision task"
below); the solver is next, once the collision task's gain is measured.
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
| Per step | 2 runs, in parallel | ~24 phases of 3 tasks, each phase sequenced by the PPU |

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

## Method

`SPURS_SNAP=F63600:F8EC80:<n>:<prefix>` writes guest memory (committed pages
below 0x20000000, ~170 MB) when the n-th slot-0 collision task of a run is
created, and again when the next solver task is: before a step's collision and
after it. Snapshots are game memory: keep them out of git (`build/snap/`).
`tests/snap_read.py` reads them (`Snap(path).u32(ea)`, `.dump(ea, n)`).
A native task can be checked offline the same way: run it on the "pre"
snapshot and compare its manifolds with "post".
