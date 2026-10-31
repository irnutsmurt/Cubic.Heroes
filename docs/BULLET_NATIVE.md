# Native Bullet: what the game's physics tasks do (checkpoint, 2026-10-31)

Groundwork for replacing 3D Dot Game Heroes' two Bullet SPU tasks with native
code. Paused in favour of the debris rendering cost (HANDOFF item 9), which is
the larger part of a slow fight frame; this is where to pick it up again.
Everything below was read from guest-memory snapshots of one physics step in a
fight (`SPURS_SNAP`, runtime patch; `tests/snap_read.py`). Offsets are bytes,
guest values big-endian, pointers 32-bit. Nothing here is game data.

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

**Bullet version: provisional.** The game's `btCollisionObject` has no
`m_anisotropicFriction`, `m_hasAnisotropicFriction` or
`m_contactProcessingThreshold`, which Bullet 2.74 has: the game's Bullet is
older (2.70-2.73) or modified. Bullet 2.74 (zlib licence; a copy outside the
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

`btPersistentManifold`: 4 `btManifoldPoint`s of 0xB0 bytes from `+0`, then
`+0x2C0` body 0, `+0x2C4` body 1, `+0x2C8` point count, `+0x2CC` contact
breaking threshold (0.0121 for a debris box on the world mesh).
`btManifoldPoint`: `+0` local point A, `+0x10` local point B, `+0x20` world
point on B, `+0x30` on A, `+0x40` normal on B, `+0x50` distance, `+0x54`
combined friction (0.5 x 240 clamped to 10), `+0x58` restitution, `+0x5C..0x68`
part and index ids, `+0x70` applied impulse (the solver's), `+0x80` lifetime.
`+0x84`, `+0x88` and `+0x90..0x98` change exactly when a point is added or
replaced: fields this Bullet has and 2.74's point layout does not explain yet.

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

## Costs it would remove (after the v3 build and the MFC cache)

BulletCD 0.8-1.5 ms a run; the solver ~24 phases at ~70 us each; a physics
step ~2.8 ms, and a frame that catches up runs 2-3 steps. Native code is
estimated to cut a step to 1 ms or less.

## Method

`SPURS_SNAP=F63600:F8EC80:<n>:<prefix>` writes guest memory (committed pages
below 0x20000000, ~170 MB) when the n-th slot-0 collision task of a run is
created, and again when the next solver task is: before a step's collision and
after it. Snapshots are game memory: keep them out of git (`build/snap/`).
`tests/snap_read.py` reads them (`Snap(path).u32(ea)`, `.dump(ea, n)`).
A native task can be checked offline the same way: run it on the "pre"
snapshot and compare its manifolds with "post".
