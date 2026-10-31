// Native BulletCD against the SPU task, offline: the task's inputs from a
// SPURS_SNAP "pre" snapshot, its outputs from the "post" one
// (docs/BULLET_NATIVE.md). Runs port/native/bullet_cd on each descriptor,
// without writing, and compares every manifold it would write with what the
// SPU task left -- point count, local and world points, normal, distance,
// friction, restitution, impulse, lifetime -- and lists manifolds the task
// changed that native did not write.
//
//   bullet_cd_snapcheck <pre.bin> <post.bin> <descriptor ea> [...]
//
// SNAPCHECK_VERBOSE=1 lists each differing point. The descriptors (one per
// slot, 0x80 apart) are the third word of the slots' queue entries. Built
// with the bundled Zig, flags and Bullet 2.74 sources as port/CMakeLists.txt's
// dotheroes_native (B = build/deps/src/bullet-2.74/src):
//   zig c++ -target x86_64-windows-gnu -mcpu=x86_64_v3 -O2 -w -DUSE_ADDR64
//     -DBT_NO_PROFILE -D__PHYSICS_COMMON_H__ -include stdint.h -I $B
//     -I $B/BulletMultiThreaded -I port/native tests/native/bullet_cd_snapcheck.cpp
//     port/native/bullet_cd.cpp port/native/bt_host_alloc.cpp <its ten Bullet .cpp>
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "bullet_cd.h"

static uint8_t* load_space(const char* path)   // a snapshot laid out at its guest addresses
{
    uint8_t* base = (uint8_t*)VirtualAlloc(nullptr, 0x20000000u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    FILE* f = fopen(path, "rb");
    if (!base || !f) { fprintf(stderr, "cannot load %s\n", path); exit(2); }
    uint32_t hdr[2];
    while (fread(hdr, 4, 2, f) == 2) {
        if ((uint64_t)hdr[0] + hdr[1] > 0x20000000u) { fseek(f, hdr[1], SEEK_CUR); continue; }
        if (fread(base + hdr[0], 1, hdr[1], f) != hdr[1]) break;
    }
    fclose(f);
    return base;
}

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static float bef(const uint8_t* p) { uint32_t u = be32(p); float f; memcpy(&f, &u, 4); return f; }

struct Diff { double max = 0; int n = 0; void add(double d) { if (d > max) max = d; n++; } };

int main(int argc, char** argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s <pre.bin> <post.bin> <descriptor ea> [...]\n", argv[0]); return 2; }
    uint8_t* pre = load_space(argv[1]);
    uint8_t* post = load_space(argv[2]);
    bullet_cd::Mem m = { pre };

    int manifolds = 0, count_mismatch = 0, missed = 0, untouched_changed = 0;
    std::map<const char*, Diff> diff;
    const struct { const char* name; uint32_t off; int vec; } fields[] = {
        { "local A", 0x00, 1 }, { "local B", 0x10, 1 }, { "world B", 0x20, 1 }, { "world A", 0x30, 1 },
        { "normal", 0x40, 1 }, { "distance", 0x50, 0 }, { "friction", 0x54, 0 }, { "restitution", 0x58, 0 },
        { "impulse", 0x70, 0 },
    };
    int life_mismatch = 0;
    for (int a = 3; a < argc; a++) {
        const uint32_t desc = (uint32_t)strtoul(argv[a], nullptr, 16);
        char why[160];
        if (!bullet_cd::supported(m, desc, why, sizeof why)) { printf("descriptor %08X: unsupported (%s)\n", desc, why); continue; }
        bullet_cd::Result r;
        bullet_cd::run(m, desc, r, false);
        printf("descriptor %08X: %d pairs, %d active, %d triangles tested, %d contacts reported, %zu manifolds written\n",
               desc, r.pairs, r.active, r.triangles, r.contacts, r.manifolds.size());
        std::set<uint32_t> written;
        for (const auto& o : r.manifolds) {
            written.insert(o.ea);
            manifolds++;
            const uint8_t* want = post + o.ea;
            const int nc = (int)be32(o.bytes + 0x2C8), wc = (int)be32(want + 0x2C8);
            if (nc != wc) {
                count_mismatch++;
                printf("  manifold %08X: %d points native, %d SPU (before: %u)\n", o.ea, nc, wc, be32(pre + o.ea + 0x2C8));
                continue;
            }
            for (int i = 0; i < nc; i++) {
                const uint8_t* np = o.bytes + i * 0xB0;
                const uint8_t* wp = want + i * 0xB0;
                for (const auto& fd : fields) {
                    double d = 0;
                    for (int k = 0; k < (fd.vec ? 3 : 1); k++)
                        d = fmax(d, fabs((double)bef(np + fd.off + 4 * k) - bef(wp + fd.off + 4 * k)));
                    diff[fd.name].add(d);
                }
                if (be32(np + 0x80) != be32(wp + 0x80)) life_mismatch++;
                const double dA = fabs((double)bef(np) - bef(wp)) + fabs((double)bef(np + 4) - bef(wp + 4)) +
                                  fabs((double)bef(np + 8) - bef(wp + 8));
                if (getenv("SNAPCHECK_VERBOSE") && (dA > 1e-3 || be32(np + 0x80) != be32(wp + 0x80)))
                    printf("  manifold %08X point %d/%d: local A native (%.4f %.4f %.4f) SPU (%.4f %.4f %.4f) | dist %.5f %.5f"
                           " | life %u %u | impulse %.3f %.3f | before: count %u, this slot A (%.4f %.4f %.4f) life %u\n",
                           o.ea, i, nc, bef(np), bef(np + 4), bef(np + 8), bef(wp), bef(wp + 4), bef(wp + 8),
                           bef(np + 0x50), bef(wp + 0x50), be32(np + 0x80), be32(wp + 0x80), bef(np + 0x70), bef(wp + 0x70),
                           be32(pre + o.ea + 0x2C8), bef(pre + o.ea + i * 0xB0), bef(pre + o.ea + i * 0xB0 + 4),
                           bef(pre + o.ea + i * 0xB0 + 8), be32(pre + o.ea + i * 0xB0 + 0x80));
            }
        }
        // what the SPU task changed and native did not write
        uint32_t in = be32(pre + desc), pages = (uint32_t)(pre[desc + 0x10] << 8 | pre[desc + 0x11]);
        for (uint32_t pg = 0; pg < pages; pg++, in += 16) {
            const uint32_t pairs = be32(pre + in + 4);
            const int s = (int)be32(pre + in + 8), e = (int)be32(pre + in + 12);
            for (int p = s; p < e; p++) {
                const uint8_t* pe = pre + pairs + p * 16u;
                const uint32_t alg = be32(pe + 8);
                if (!alg) continue;
                const uint32_t mf = be32(pre + alg + 0x30);
                const bool changed = memcmp(pre + mf, post + mf, bullet_cd::kManifoldBytes) != 0;
                if (changed && !written.count(mf)) { missed++; printf("  manifold %08X: changed by the SPU, not written natively\n", mf); }
                if (!changed && written.count(mf)) untouched_changed++;
            }
        }
    }
    printf("\n%d manifolds compared: %d point-count mismatches, %d missed, %d written natively but unchanged by the SPU,"
           " %d lifetime mismatches\n", manifolds, count_mismatch, missed, untouched_changed, life_mismatch);
    for (const auto& fd : fields)
        printf("  %-12s max |diff| %.3g over %d points\n", fd.name, diff[fd.name].max, diff[fd.name].n);
    return 0;
}
