// Native BulletCD: 3D Dot Game Heroes' Bullet narrowphase SPU task
// (PS3_SPURS_BulletCD, Bullet 2.73's SpuNarrowPhaseCollisionTask) as host
// code, over the game's own data in guest memory. docs/BULLET_NATIVE.md maps
// what the task reads and writes; the narrowphase itself is Bullet 2.74's
// (zlib licence), compiled for the host.
//
// One call does one task's work: the pairs of its descriptor's work units,
// each pair's contacts found and its persistent manifold updated the way the
// SPU task does it. Shapes it does not handle are refused before anything is
// written, so the caller can run the original task instead.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bullet_cd {

// Guest memory: big-endian, 32-bit addresses, `base` + ea on the host.
struct Mem {
    uint8_t* base;
};

constexpr uint32_t kManifoldBytes = 0x2D0;   // the points, bodies, count, breaking threshold

// A manifold as the task leaves it.
struct ManifoldOut {
    uint32_t ea;
    uint32_t types;   // the pair's shape types, type0 << 8 | type1
    uint8_t bytes[kManifoldBytes];
};

struct Result {
    int pairs = 0;          // SPU pairs looked at
    int active = 0;         // ... with an active object (processed)
    int triangles = 0;      // mesh triangles tested
    int contacts = 0;       // contact points reported by the narrowphase
    std::vector<ManifoldOut> manifolds;   // every manifold written
};

// 1 when every pair of the descriptor's work units is one this code handles
// (box/box, triangle mesh/box either way round, the BVH and mesh layouts it
// knows); 0 with the reason in `why` otherwise. Reads only.
int supported(const Mem& m, uint32_t desc_ea, char* why, size_t why_len);

// Do the task's work for descriptor `desc_ea`. With `write`, the manifolds are
// written to guest memory as the SPU task writes them; either way each is
// returned in `out.manifolds`.
void run(const Mem& m, uint32_t desc_ea, Result& out, bool write);

// The manifold of every SPU pair of the descriptor's work units as it stands
// now (to compare a run against). Reads only.
void manifolds(const Mem& m, uint32_t desc_ea, std::vector<ManifoldOut>& out);

// How much collision work a descriptor holds, whichever implementation does
// it: its SPU pairs, those the task processes (an object awake), by kind,
// the distinct awake bodies in them, and the contact points their manifolds
// hold as it is read. Reads only.
struct Census {
    int pairs = 0, active = 0;
    int mesh = 0, boxbox = 0, cylinder = 0;   // active pairs by kind
    int bodies = 0;                           // distinct awake objects in active pairs
    int points = 0;                           // manifold points in active pairs
};
void census(const Mem& m, uint32_t desc_ea, Census& out);

}  // namespace bullet_cd
