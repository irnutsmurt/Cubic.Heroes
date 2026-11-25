/* Bullet's sweep and prune broadphase, natively (issue #2, an experiment behind a switch).
 *
 * btAxisSweep3Internal<unsigned short>'s addHandle, removeHandle and updateHandle with quantize and the four sorts, as
 * Bullet 2.74's BulletCollision/BroadphaseCollision/btAxisSweep3.h does them (USE_OVERLAP_TEST_ON_REMOVES, the new
 * clamping), run on the game's own broadphase in guest memory: the same handles and edge arrays, the same calls into
 * its pair cache in the same order. Entry hooks (config/r8_hooks.json) guarded by g_native_sweep:
 *
 *   DOTHEROES_NATIVE_SWEEP=1      the three functions natively -- the default (port/main.cpp, and the setup's
 *                                 settings); =0 the lifted ones, for comparison
 *   DOTHEROES_NATIVE_SWEEP=check  each call runs natively, its pair cache calls the game's way where they write only
 *                                 the cache (its virtuals), otherwise simulated without calling game code
 *                                 (port/pair_cache.cpp); the input is put back and the lifted function
 *                                 runs; the two results are compared byte for byte (the broadphase's fields, its handles
 *                                 and edge arrays in use, the pair cache's fields, pair array, hash and next tables,
 *                                 Bullet's pair counters, the returned handle), the lifted one kept. [sweep] lines
 *                                 count them every 20000 calls and at the end.
 *
 * The pair cache is reached through its virtual functions, as Bullet does (so DOTHEROES_NATIVE_PAIRCACHE applies to
 * those calls): addOverlappingPair (+0x08), removeOverlappingPair (+0x0C), removeOverlappingPairsContainingProxy
 * (+0x10), hasDeferredRemoval (+0x38). A broadphase with a user pair callback, and addHandle with no free handle, are
 * declined; in check mode, a call whose pair cache part could not be simulated is put back and declined.
 *
 * Layouts, as Bullet 2.74 declares them for the PPU (32-bit): btAxisSweep3Internal -- vtable +0x00, m_bpHandleMask
 * +0x04, m_handleSentinel +0x06 (shorts), m_worldAabbMin +0x10, m_worldAabbMax +0x20, m_quantize +0x30 (btVector3),
 * m_numHandles +0x40, m_maxHandles +0x42 (shorts), m_pHandles +0x44, m_firstFreeHandle +0x48 (short), m_pEdges[3]
 * +0x4C, m_pairCache +0x64, m_userPairCallback +0x68; a Handle (64 bytes) -- btBroadphaseProxy (m_clientObject +0x00,
 * m_collisionFilterGroup +0x04, m_collisionFilterMask +0x06, m_multiSapParentProxy +0x08, m_uniqueId +0x0C, the aabb
 * +0x10/+0x20), m_minEdges[3] +0x30, m_maxEdges[3] +0x36 (the next free handle in m_minEdges[0]); an Edge -- m_pos,
 * m_handle (shorts), a max edge's m_pos odd. */
#include "ppu_recomp.h"   /* ppu_context, the lifted functions */

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern "C" void ps3_indirect_call_to(ppu_context* ctx, uint32_t target);
extern "C" int dotheroes_paircache_simulate(ppu_context* ctx, int op, uint32_t self, uint32_t a, uint32_t b, uint32_t dispatcher,
                                            uint32_t* ret);   // port/pair_cache.cpp
extern "C" int dotheroes_paircache_pure(ppu_context* ctx, int op, uint32_t self, uint32_t a, uint32_t b);

extern "C" { int g_native_sweep = 0; }
extern "C" void (*g_dotheroes_sweep_report)(void);   // dev_state.cpp: called at a test's stop

namespace sweep {   // a named namespace: perf names its samples

enum : uint32_t {
    kMask = 0x04, kSentinel = 0x06, kWorldMin = 0x10, kQuantize = 0x30, kNumHandles = 0x40, kMaxHandles = 0x42,
    kHandles = 0x44, kFirstFree = 0x48, kEdges = 0x4C, kPairCache = 0x64, kUserCb = 0x68, kObjBytes = 0x80,
    kHandleBytes = 64, kHOwner = 0x00, kHGroup = 0x04, kHMask = 0x06, kHMultiSap = 0x08, kHUid = 0x0C,
    kHMinEdges = 0x30, kHMaxEdges = 0x36,
    kVAdd = 0x08, kVRemove = 0x0C, kVRemoveContaining = 0x10, kVDeferred = 0x38,
    kFnAdd = 0x00A4CD24u, kFnRemove = 0x009144C8u,
    // the pair cache, for check mode's snapshot (port/pair_cache.cpp)
    kPcSize = 0x08, kPcCap = 0x0C, kPcData = 0x10, kPcHashCap = 0x28, kPcHashData = 0x2C, kPcNextCap = 0x3C,
    kPcNextData = 0x40, kPcBytes = 0x4C, kTocCounters = 0x29B4,
};

inline uint32_t r32(uint32_t ea) { const uint8_t* p = vm_base + ea; return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline uint16_t r16(uint32_t ea) { const uint8_t* p = vm_base + ea; return (uint16_t)(p[0] << 8 | p[1]); }
inline void w16(uint32_t ea, uint16_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
inline void w32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
inline float rf(uint32_t ea) { const uint32_t u = r32(ea); float f; memcpy(&f, &u, 4); return f; }
inline uint32_t code_of(uint32_t vtable, uint32_t slot) { const uint32_t opd = r32(vtable + slot); return opd ? r32(opd) : 0; }

thread_local int t_inner;       // a lifted function called from here: its own hook declines
thread_local bool t_simulate;   // check mode's native run: the pair cache simulated, no game code called
thread_local bool t_failed;     // ... and a pair cache call it could not simulate

// one broadphase, its fields read once a call
struct Sweep {
    uint32_t self, handles, edges[3], cache, dispatcher;
    uint16_t mask, sentinel;
    uint32_t handle(uint16_t i) const { return handles + kHandleBytes * i; }
    uint32_t edge(int axis, uint32_t i) const { return edges[axis] + 4 * i; }
};
inline uint16_t epos(uint32_t e) { return r16(e); }
inline uint16_t ehandle(uint32_t e) { return r16(e + 2); }
inline bool is_max(uint32_t e) { return epos(e) & 1; }
inline void eswap(uint32_t a, uint32_t b) { uint32_t x, y; memcpy(&x, vm_base + a, 4); memcpy(&y, vm_base + b, 4); memcpy(vm_base + a, &y, 4); memcpy(vm_base + b, &x, 4); }
inline uint16_t hmin(uint32_t h, int axis) { return r16(h + kHMinEdges + 2 * axis); }
inline uint16_t hmax(uint32_t h, int axis) { return r16(h + kHMaxEdges + 2 * axis); }
inline void hmin_add(uint32_t h, int axis, int d) { w16(h + kHMinEdges + 2 * axis, (uint16_t)(hmin(h, axis) + d)); }
inline void hmax_add(uint32_t h, int axis, int d) { w16(h + kHMaxEdges + 2 * axis, (uint16_t)(hmax(h, axis) + d)); }

// a guest virtual call obj->slot(a4, a5, a6): the known hashed cache functions called directly (their hooks apply),
// anything else through the runtime's indirect call; returns r3
uint32_t vcall(ppu_context* ctx, uint32_t obj, uint32_t slot, uint32_t a4, uint32_t a5, uint32_t a6)
{
    const uint32_t opd = r32(r32(obj) + slot), code = r32(opd);
    const uint64_t r2 = ctx->gpr[2], lr = ctx->lr;
    ctx->gpr[3] = obj; ctx->gpr[4] = a4; ctx->gpr[5] = a5; ctx->gpr[6] = a6;
    ctx->gpr[2] = r32(opd + 4);
    if (code == kFnAdd) func_00A4CD24(ctx);
    else if (code == kFnRemove) func_009144C8(ctx);
    else ps3_indirect_call_to(ctx, code);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf((void*)ctx); }
    ctx->gpr[2] = r2; ctx->lr = lr;
    return (uint32_t)ctx->gpr[3];
}

// the pair cache's add and remove, as the sorts call them. In check mode's native run a call that writes only the
// cache goes the game's way too (the virtual, so the production path is what is checked); any other is simulated
void pc_add(ppu_context* ctx, const Sweep& s, uint32_t h0, uint32_t h1)
{
    if (t_simulate && !dotheroes_paircache_pure(ctx, 0, s.cache, h0, h1)) {
        if (!dotheroes_paircache_simulate(ctx, 0, s.cache, h0, h1, 0, nullptr)) t_failed = true;
        return;
    }
    vcall(ctx, s.cache, kVAdd, h0, h1, 0);
}
void pc_remove(ppu_context* ctx, const Sweep& s, uint32_t h0, uint32_t h1)
{
    if (t_simulate && !dotheroes_paircache_pure(ctx, 1, s.cache, h0, h1)) {
        if (!dotheroes_paircache_simulate(ctx, 1, s.cache, h0, h1, s.dispatcher, nullptr)) t_failed = true;
        return;
    }
    vcall(ctx, s.cache, kVRemove, h0, h1, s.dispatcher);
}

// testOverlap2D
inline bool overlap2d(uint32_t a, uint32_t b, int axis0, int axis1)
{
    return !(hmax(a, axis0) < hmin(b, axis0) || hmax(b, axis0) < hmin(a, axis0) ||
             hmax(a, axis1) < hmin(b, axis1) || hmax(b, axis1) < hmin(a, axis1));
}

void sort_min_down(ppu_context* ctx, const Sweep& s, int axis, uint16_t edge, bool update)
{
    uint32_t e = s.edge(axis, edge), prev = e - 4;
    const uint32_t he = s.handle(ehandle(e));
    while (epos(e) < epos(prev)) {
        const uint32_t hp = s.handle(ehandle(prev));
        if (is_max(prev)) {
            const int axis1 = (1 << axis) & 3, axis2 = (1 << axis1) & 3;
            if (update && overlap2d(he, hp, axis1, axis2)) pc_add(ctx, s, he, hp);
            hmax_add(hp, axis, 1);
        } else {
            hmin_add(hp, axis, 1);
        }
        hmin_add(he, axis, -1);
        eswap(e, prev);
        e -= 4; prev -= 4;
    }
}

void sort_min_up(ppu_context* ctx, const Sweep& s, int axis, uint16_t edge, bool update)
{
    uint32_t e = s.edge(axis, edge), next = e + 4;
    const uint32_t he = s.handle(ehandle(e));
    while (ehandle(next) && epos(e) >= epos(next)) {
        const uint32_t hn = s.handle(ehandle(next));
        if (is_max(next)) {
            const uint32_t h0 = s.handle(ehandle(e)), h1 = s.handle(ehandle(next));
            const int axis1 = (1 << axis) & 3, axis2 = (1 << axis1) & 3;
            if (update && overlap2d(h0, h1, axis1, axis2)) pc_remove(ctx, s, h0, h1);
            hmax_add(hn, axis, -1);
        } else {
            hmin_add(hn, axis, -1);
        }
        hmin_add(he, axis, 1);
        eswap(e, next);
        e += 4; next += 4;
    }
}

void sort_max_down(ppu_context* ctx, const Sweep& s, int axis, uint16_t edge, bool update)
{
    uint32_t e = s.edge(axis, edge), prev = e - 4;
    const uint32_t he = s.handle(ehandle(e));
    while (epos(e) < epos(prev)) {
        const uint32_t hp = s.handle(ehandle(prev));
        if (!is_max(prev)) {
            const uint32_t h0 = s.handle(ehandle(e)), h1 = s.handle(ehandle(prev));
            const int axis1 = (1 << axis) & 3, axis2 = (1 << axis1) & 3;
            if (update && overlap2d(h0, h1, axis1, axis2)) pc_remove(ctx, s, h0, h1);
            hmin_add(hp, axis, 1);
        } else {
            hmax_add(hp, axis, 1);
        }
        hmax_add(he, axis, -1);
        eswap(e, prev);
        e -= 4; prev -= 4;
    }
}

void sort_max_up(ppu_context* ctx, const Sweep& s, int axis, uint16_t edge, bool update)
{
    uint32_t e = s.edge(axis, edge), next = e + 4;
    const uint32_t he = s.handle(ehandle(e));
    while (ehandle(next) && epos(e) >= epos(next)) {
        const uint32_t hn = s.handle(ehandle(next));
        const int axis1 = (1 << axis) & 3, axis2 = (1 << axis1) & 3;
        if (!is_max(next)) {
            if (update && overlap2d(he, hn, axis1, axis2)) pc_add(ctx, s, s.handle(ehandle(e)), s.handle(ehandle(next)));
            hmin_add(hn, axis, -1);
        } else {
            hmax_add(hn, axis, -1);
        }
        hmax_add(he, axis, 1);
        eswap(e, next);
        e += 4; next += 4;
    }
}

// quantize: the point relative to the world's box, scaled to the handle range, clamped; a max edge odd
void quantize(const Sweep& s, uint16_t out[3], uint32_t point, int is_max)
{
    for (int i = 0; i < 3; i++) {
        const float v = (rf(point + 4 * i) - rf(s.self + kWorldMin + 4 * i)) * rf(s.self + kQuantize + 4 * i);
        out[i] = v <= 0 ? (uint16_t)is_max
               : v >= (float)s.sentinel ? (uint16_t)((s.sentinel & s.mask) | is_max)
               : (uint16_t)(((uint16_t)v & s.mask) | is_max);
    }
}

enum Why { kWhyUserCallback, kWhyNoHandle, kWhyCheck, kWhyCount };
const char* const kWhyNames[kWhyCount] = { "user callback", "no free handle", "check" };
std::atomic<unsigned long long> g_why[3][kWhyCount];
inline int no(int op, Why why) { g_why[op][why]++; return 0; }

bool load(Sweep& s, ppu_context* ctx, uint32_t self, uint32_t dispatcher)
{
    s.self = self; s.handles = r32(self + kHandles); s.cache = r32(self + kPairCache); s.dispatcher = dispatcher;
    for (int a = 0; a < 3; a++) s.edges[a] = r32(self + kEdges + 4 * a);
    s.mask = r16(self + kMask); s.sentinel = r16(self + kSentinel);
    (void)ctx;
    return !r32(self + kUserCb);
}

// updateHandle(this, handle, &aabbMin, &aabbMax, dispatcher)
int update_native(ppu_context* ctx)
{
    Sweep s;
    if (!load(s, ctx, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[7])) return no(0, kWhyUserCallback);
    const uint16_t handle = (uint16_t)ctx->gpr[4];
    const uint32_t h = s.handle(handle);
    uint16_t mn[3], mx[3];
    quantize(s, mn, (uint32_t)ctx->gpr[5], 0);
    quantize(s, mx, (uint32_t)ctx->gpr[6], 1);
    for (int axis = 0; axis < 3; axis++) {
        const uint16_t emin = hmin(h, axis), emax = hmax(h, axis);
        const int dmin = (int)mn[axis] - (int)epos(s.edge(axis, emin));
        const int dmax = (int)mx[axis] - (int)epos(s.edge(axis, emax));
        w16(s.edge(axis, emin), mn[axis]);
        w16(s.edge(axis, emax), mx[axis]);
        if (dmin < 0) sort_min_down(ctx, s, axis, emin, true);
        if (dmax > 0) sort_max_up(ctx, s, axis, emax, true);
        if (dmin > 0) sort_min_up(ctx, s, axis, emin, true);
        if (dmax < 0) sort_max_down(ctx, s, axis, emax, true);
        if (t_failed) return -1;
    }
    return 1;
}

// addHandle(this, &aabbMin, &aabbMax, owner, group, mask, dispatcher, multiSapProxy): the new handle in r3
int add_native(ppu_context* ctx)
{
    Sweep s;
    if (!load(s, ctx, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[9])) return no(1, kWhyUserCallback);
    const uint16_t handle = r16(s.self + kFirstFree);
    if (!handle) return no(1, kWhyNoHandle);
    uint16_t mn[3], mx[3];
    quantize(s, mn, (uint32_t)ctx->gpr[4], 0);
    quantize(s, mx, (uint32_t)ctx->gpr[5], 1);
    // allocHandle
    const uint32_t h = s.handle(handle);
    w16(s.self + kFirstFree, hmin(h, 0));   // GetNextFree
    const uint16_t num = (uint16_t)(r16(s.self + kNumHandles) + 1);
    w16(s.self + kNumHandles, num);
    w32(h + kHUid, handle);
    w32(h + kHOwner, (uint32_t)ctx->gpr[6]);
    w16(h + kHGroup, (uint16_t)ctx->gpr[7]);
    w16(h + kHMask, (uint16_t)ctx->gpr[8]);
    w32(h + kHMultiSap, (uint32_t)ctx->gpr[10]);
    const uint16_t limit = (uint16_t)(num * 2);
    for (int axis = 0; axis < 3; axis++) {
        hmax_add(s.handle(0), axis, 2);
        memcpy(vm_base + s.edge(axis, limit + 1), vm_base + s.edge(axis, limit - 1), 4);
        w16(s.edge(axis, limit - 1), mn[axis]); w16(s.edge(axis, limit - 1) + 2, handle);
        w16(s.edge(axis, limit), mx[axis]); w16(s.edge(axis, limit) + 2, handle);
        w16(h + kHMinEdges + 2 * axis, (uint16_t)(limit - 1));
        w16(h + kHMaxEdges + 2 * axis, limit);
    }
    sort_min_down(ctx, s, 0, hmin(h, 0), false);
    sort_max_down(ctx, s, 0, hmax(h, 0), false);
    sort_min_down(ctx, s, 1, hmin(h, 1), false);
    sort_max_down(ctx, s, 1, hmax(h, 1), false);
    sort_min_down(ctx, s, 2, hmin(h, 2), true);
    if (t_failed) return -1;
    sort_max_down(ctx, s, 2, hmax(h, 2), true);
    if (t_failed) return -1;
    ctx->gpr[3] = handle;
    return 1;
}

// removeHandle(this, handle, dispatcher)
int remove_native(ppu_context* ctx)
{
    Sweep s;
    if (!load(s, ctx, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[5])) return no(2, kWhyUserCallback);
    const uint16_t handle = (uint16_t)ctx->gpr[4];
    const uint32_t h = s.handle(handle);
    if (!(uint8_t)vcall(ctx, s.cache, kVDeferred, 0, 0, 0)) {   // hasDeferredRemoval (a bool): a const getter
        if (t_simulate && !dotheroes_paircache_pure(ctx, 2, s.cache, h, 0)) {
            if (!dotheroes_paircache_simulate(ctx, 2, s.cache, h, 0, s.dispatcher, nullptr)) return -1;
        } else {
            vcall(ctx, s.cache, kVRemoveContaining, h, s.dispatcher, 0);
        }
    }
    const int limit = (int)r16(s.self + kNumHandles) * 2;
    for (int axis = 0; axis < 3; axis++) hmax_add(s.handle(0), axis, -2);
    for (int axis = 0; axis < 3; axis++) {
        const uint16_t max = hmax(h, axis);
        w16(s.edge(axis, max), s.sentinel);
        sort_max_up(ctx, s, axis, max, false);
        const uint16_t i = hmin(h, axis);
        w16(s.edge(axis, i), s.sentinel);
        sort_min_up(ctx, s, axis, i, false);
        w16(s.edge(axis, limit - 1) + 2, 0);
        w16(s.edge(axis, limit - 1), s.sentinel);
    }
    // freeHandle
    w16(h + kHMinEdges, r16(s.self + kFirstFree));   // SetNextFree
    w16(s.self + kFirstFree, handle);
    w16(s.self + kNumHandles, (uint16_t)(r16(s.self + kNumHandles) - 1));
    return 1;
}

// ---- check mode -------------------------------------------------------------------------------------------------
struct Region { uint32_t ea; std::vector<uint8_t> b; };
struct Snap { std::vector<Region> r; };
Snap snap(ppu_context* ctx, uint32_t self, uint32_t extra_handle)
{
    Snap s;
    auto add = [&](uint32_t ea, uint32_t n) { if (n) s.r.push_back({ ea, std::vector<uint8_t>(vm_base + ea, vm_base + ea + n) }); };
    add(self, kObjBytes);
    const uint32_t num = r16(self + kNumHandles), maxh = r16(self + kMaxHandles), handles = r32(self + kHandles);
    // the edges in use (an added handle's two more and the end sentinel included) and the handles they and the
    // free list's head name
    const uint32_t nedges = std::min<uint32_t>(2 * maxh, 2 * num + 4);
    uint32_t top = std::max<uint32_t>(extra_handle, r16(self + kFirstFree));
    for (int a = 0; a < 3; a++) {
        const uint32_t ed = r32(self + kEdges + 4 * a);
        add(ed, 4 * nedges);
        for (uint32_t i = 0; i < nedges; i++) top = std::max<uint32_t>(top, r16(ed + 4 * i + 2));
    }
    add(handles, kHandleBytes * std::min<uint32_t>(maxh, top + 1));
    const uint32_t pc = r32(self + kPairCache);
    add(pc, kPcBytes);
    add(r32(pc + kPcData), 16 * r32(pc + kPcCap));
    add(r32(pc + kPcHashData), 4 * r32(pc + kPcHashCap));
    add(r32(pc + kPcNextData), 4 * r32(pc + kPcNextCap));
    add(r32((uint32_t)ctx->gpr[2] + kTocCounters), 16);
    return s;
}
void restore(const Snap& s) { for (const Region& g : s.r) memcpy(vm_base + g.ea, g.b.data(), g.b.size()); }
Snap reread(const Snap& s) { Snap o = s; for (Region& g : o.r) memcpy(g.b.data(), vm_base + g.ea, g.b.size()); return o; }
bool same(const Snap& a, const Snap& b, int* where)
{
    for (size_t i = 0; i < a.r.size(); i++) if (a.r[i].b != b.r[i].b) { *where = (int)i; return false; }
    return true;
}

struct Stats { std::atomic<unsigned long long> handled{0}, declined{0}, compared{0}, differ{0}, inner{0}; };
Stats g_st[3];
const char* const kOpNames[3] = { "update", "add", "remove" };
int g_mode;   // 1 native, 2 check
std::atomic<unsigned long long> g_calls{0};

void report()
{
    char line[1024]; int n = 0;
    n += snprintf(line + n, sizeof line - n, "[sweep] %s:", g_mode == 2 ? "check" : "native");
    for (int k = 0; k < 3; k++) {
        n += snprintf(line + n, sizeof line - n, " %s %llu handled, %llu declined [", kOpNames[k], g_st[k].handled.load(), g_st[k].declined.load());
        for (int w = 0; w < kWhyCount; w++)
            if (g_why[k][w].load()) n += snprintf(line + n, sizeof line - n, " %s %llu", kWhyNames[w], g_why[k][w].load());
        n += snprintf(line + n, sizeof line - n, " ], %llu nested;", g_st[k].inner.load());
    }
    if (g_mode == 2)
        for (int k = 0; k < 3; k++)
            n += snprintf(line + n, sizeof line - n, " %s compared %llu, differ %llu;", kOpNames[k], g_st[k].compared.load(), g_st[k].differ.load());
    fprintf(stderr, "%s\n", line);
}

void call_lifted(ppu_context* ctx, void (*fn)(ppu_context*), const uint64_t a[8])
{
    const uint64_t lr = ctx->lr;
    for (int i = 0; i < 8; i++) ctx->gpr[3 + i] = a[i];
    t_inner++;
    fn(ctx);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf((void*)ctx); }
    t_inner--;
    ctx->lr = lr;
}

int run_op(int op, ppu_context* ctx, int (*native)(ppu_context*), void (*lifted)(ppu_context*))
{
    if (t_inner) { g_st[op].inner++; return 0; }
    if (g_mode != 2) {
        const int r = native(ctx);
        (r ? g_st[op].handled : g_st[op].declined)++;
        return r;
    }
    if (++g_calls % 20000 == 0) report();
    uint64_t a[8];
    for (int i = 0; i < 8; i++) a[i] = ctx->gpr[3 + i];
    const uint32_t self = (uint32_t)a[0];
    const Snap in = snap(ctx, self, op == 0 || op == 2 ? (uint32_t)(uint16_t)a[1] : 0);
    t_simulate = true; t_failed = false;
    const int r = native(ctx);
    t_simulate = false;
    if (r <= 0 || t_failed) {
        if (r != 0 || t_failed) { restore(in); g_why[op][kWhyCheck]++; }
        g_st[op].declined++;
        for (int i = 0; i < 8; i++) ctx->gpr[3 + i] = a[i];   // the lifted body runs from the same arguments
        return 0;
    }
    g_st[op].handled++;
    const uint32_t r_native = (uint32_t)ctx->gpr[3];
    const Snap out_native = reread(in);
    restore(in);
    call_lifted(ctx, lifted, a);
    const Snap out_lifted = reread(in);
    g_st[op].compared++;
    int where = -1;
    if (!same(out_native, out_lifted, &where) || (op == 1 && (uint16_t)r_native != (uint16_t)ctx->gpr[3])) {
        static std::atomic<int> shown{0};
        if (g_st[op].differ++ < 1 || shown++ < 10)
            fprintf(stderr, "[sweep] check: %s differs (native r3 %08X, lifted %08X; region %d at %08X)\n", kOpNames[op], r_native,
                    (uint32_t)ctx->gpr[3], where, where >= 0 ? in.r[where].ea : 0);
    }
    return 1;   // the lifted function's result stands
}

/* DOTHEROES_NATIVE_SWEEP, read once dotheroes.cfg is applied (port/main.cpp: default 1) */
void init()
{
    {
        const char* e = getenv("DOTHEROES_NATIVE_SWEEP");
        g_mode = !e ? 0 : !strcmp(e, "check") ? 2 : atoi(e) == 1 ? 1 : 0;
        g_native_sweep = g_mode != 0;
        if (!g_mode) return;
        atexit(report);
        g_dotheroes_sweep_report = report;
        fprintf(stderr, "[sweep] Bullet's btAxisSweep3: %s (DOTHEROES_NATIVE_SWEEP)\n",
                g_mode == 2 ? "native checked against the lifted functions, call by call" : "native");
    }
}

}  // namespace sweep

/* The entry hooks (config/r8_hooks.json): btAxisSweep3Internal<unsigned short>::updateHandle (0x00A46DD8), addHandle
 * (0x00A47588) and removeHandle (0x00A456B0); 0 declines (the lifted body runs). */
using namespace sweep;
extern "C" void dotheroes_sweep_init(void) { sweep::init(); }   // port/main.cpp, after the settings
extern "C" int dotheroes_sweep_update(ppu_context* ctx) { return run_op(0, ctx, update_native, func_00A46DD8); }
extern "C" int dotheroes_sweep_add(ppu_context* ctx) { return run_op(1, ctx, add_native, func_00A47588); }
extern "C" int dotheroes_sweep_remove(ppu_context* ctx) { return run_op(2, ctx, remove_native, func_00A456B0); }
