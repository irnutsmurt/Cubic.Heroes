/* Bullet's hashed overlapping pair cache, natively (issue #2, an experiment behind a switch).
 *
 * btHashedOverlappingPairCache's addOverlappingPair, removeOverlappingPair and processAllOverlappingPairs with
 * Bullet's remove and clean callbacks, as Bullet 2.74's BulletCollision/BroadphaseCollision/btOverlappingPairCache.cpp
 * and .h do them, run on the game's own pair cache in guest memory: the same pair array, hash and next tables, the
 * same order and the same counters, byte for byte. Entry hooks (config/r8_hooks.json) guarded by g_native_paircache:
 *
 *   DOTHEROES_NATIVE_PAIRCACHE=1      the three operations natively -- the default (port/main.cpp, and the setup's
 *                                     settings); =0 the lifted ones, for comparison
 *   DOTHEROES_NATIVE_PAIRCACHE=check  each call the native code would handle runs natively, its result is kept,
 *                                     the inputs are put back and the lifted function runs; the two results are
 *                                     compared (the pair array, the hash and next tables, the cache's fields, the
 *                                     counters and the return value) and the lifted one stays. [pair-cache] lines
 *                                     count them every 20000 calls and at the end.
 *   DOTHEROES_NATIVE_PAIRCACHE=lifted the lifted functions, called from the hooks (for timing them the same way)
 *   DOTHEROES_PAIRCACHE_TIME=<file>   each call's duration (the CPU's time-stamp counter), written at a test's stop
 *                                     and at exit: "us op ns native" (us on the frame trace's clock; op 0 add,
 *                                     1 remove, 2 scan; native 0 when the lifted function did it). With it, a call
 *                                     the native code declines runs the lifted function from the hook, timed too.
 *
 * What reaches beyond the cache stays with the game's code: a pair's collision algorithm is freed by the lifted
 * cleanOverlappingPair (the game's dispatcher), the native code doing the rest; an overlap filter or a ghost pair
 * callback, the array's growth (the game's allocator) and a scan with any other callback are declined before anything
 * is written. In check mode the native run calls no game code: a freed algorithm is what the cache sees of it (the
 * pair's m_algorithm cleared), and a call it could not run that way is put back and declined. Bullet 2.74 orders a
 * pair's proxies by unique id; the game's build compares the proxies' addresses: the same order for btAxisSweep3's
 * handles (one array, the id its index), and a call where the two would differ is declined (inside a scan, that pair
 * goes to the lifted removeOverlappingPair).
 *
 * Layouts, as Bullet 2.74 declares them for the PPU (32-bit): btHashedOverlappingPairCache -- vtable +0x00, the pair
 * array (size +0x08, capacity +0x0C, data +0x10), m_overlapFilterCallback +0x18, m_hashTable (size +0x24, capacity
 * +0x28, data +0x2C), m_next (size +0x38, capacity +0x3C, data +0x40), m_ghostPairCallback +0x48; btBroadphasePair (16
 * bytes) -- m_pProxy0, m_pProxy1, m_algorithm, m_internalInfo1; btBroadphaseProxy -- m_collisionFilterGroup +0x04,
 * m_collisionFilterMask +0x06 (shorts), m_uniqueId +0x0C; the callbacks -- RemovePairCallback {vtable, proxy},
 * CleanPairCallback {vtable, proxy, pair cache, dispatcher}. The game's build: Bullet's pair counters (gRemovePairs +0,
 * gOverlappingPairs +4, gAddedPairs +0xC) at the address in its TOC's +0x29B4; the two callbacks' vtables in its
 * TOC's +0x29CC (remove) and +0x29D0 (clean), processOverlap their +0x08; the cache's virtuals at +0x0C
 * (removeOverlappingPair), +0x20 (cleanOverlappingPair), +0x30 (processAllOverlappingPairs). */
#include "ppu_recomp.h"   /* ppu_context, the lifted functions */
#include "ps3emu/ps3_ftrace.h"   /* the frame trace's clock (DOTHEROES_PAIRCACHE_TIME) */

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);

extern "C" { int g_native_paircache = 0; }
extern "C" void (*g_dotheroes_paircache_report)(void);   // dev_state.cpp: called at a test's stop

namespace paircache {   // a named namespace: perf names its samples

enum : uint32_t {
    kSize = 0x08, kCap = 0x0C, kData = 0x10, kFilter = 0x18,
    kHashSize = 0x24, kHashCap = 0x28, kHashData = 0x2C,
    kNextSize = 0x38, kNextCap = 0x3C, kNextData = 0x40, kGhost = 0x48, kCacheBytes = 0x4C,
    kPairBytes = 16, kPairP0 = 0, kPairP1 = 4, kPairAlg = 8, kPairInfo = 12,
    kProxyGroup = 4, kProxyMask = 6, kProxyUid = 0x0C,
    kRemovePairs = 0, kOverlappingPairs = 4, kAddedPairs = 0x0C,
    kTocCounters = 0x29B4, kTocRemoveCb = 0x29CC, kTocCleanCb = 0x29D0,
    kVRemove = 0x0C, kVClean = 0x20, kVProcessOverlap = 0x08,
    kFnRemove = 0x009144C8u, kFnClean = 0x00913D88u, kFnRemoveCb = 0x00913EA8u, kFnCleanCb = 0x00913E28u,
};
constexpr int32_t kNull = -1;   // BT_NULL_PAIR

inline uint32_t r32(uint32_t ea) { const uint8_t* p = vm_base + ea; return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline void w32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
inline int32_t s16(uint32_t ea) { const uint8_t* p = vm_base + ea; return (int16_t)(uint16_t)((uint16_t)p[0] << 8 | p[1]); }
inline uint32_t uid(uint32_t proxy) { return r32(proxy + kProxyUid); }
inline uint32_t code_of(uint32_t vtable, uint32_t slot) { const uint32_t opd = r32(vtable + slot); return opd ? r32(opd) : 0; }

// btHashedOverlappingPairCache::getHash (Thomas Wang's integer hash), on 32-bit ints with arithmetic right shifts
inline uint32_t bt_hash(uint32_t id1, uint32_t id2)
{
    uint32_t key = id1 | id2 << 16;
    key += ~(key << 15);
    key ^= (uint32_t)((int32_t)key >> 10);
    key += key << 3;
    key ^= (uint32_t)((int32_t)key >> 6);
    key += ~(key << 11);
    key ^= (uint32_t)((int32_t)key >> 16);
    return key;
}

struct Pair { uint32_t p0, p1; };
// internalAddPair / removeOverlappingPair's order: the proxy with the smaller unique id first; false where the
// game's build, which compares the proxies' addresses, would order them otherwise
inline bool ordered(uint32_t a, uint32_t b, Pair& p)
{
    const bool by_uid = uid(a) > uid(b);
    if (by_uid != (a > b)) return false;
    p = by_uid ? Pair{ b, a } : Pair{ a, b };
    return true;
}

// internalFindPair: the pair's index, or kNull
int32_t find(uint32_t self, uint32_t p0, uint32_t p1, uint32_t hash)
{
    const uint32_t id1 = uid(p0), id2 = uid(p1), data = r32(self + kData), next = r32(self + kNextData);
    int32_t index = (int32_t)r32(r32(self + kHashData) + 4 * hash);
    while (index != kNull) {
        const uint32_t e = data + kPairBytes * (uint32_t)index;
        if (uid(r32(e + kPairP0)) == id1 && uid(r32(e + kPairP1)) == id2) break;
        index = (int32_t)r32(next + 4 * (uint32_t)index);
    }
    return index;
}

void counter_add(uint32_t counters, uint32_t off, int32_t d) { w32(counters + off, r32(counters + off) + (uint32_t)d); }

// unlink `index` from the hash chain `hash` (removeOverlappingPair)
void unlink(uint32_t self, uint32_t hash, int32_t index)
{
    const uint32_t ht = r32(self + kHashData), next = r32(self + kNextData);
    int32_t i = (int32_t)r32(ht + 4 * hash), previous = kNull;
    while (i != index) { previous = i; i = (int32_t)r32(next + 4 * (uint32_t)i); }
    if (previous != kNull) w32(next + 4 * (uint32_t)previous, r32(next + 4 * (uint32_t)index));
    else w32(ht + 4 * hash, r32(next + 4 * (uint32_t)index));
}

// the rest of removeOverlappingPair once the pair is found and cleaned (no ghost callback): out of its hash chain, the
// last pair moved into its place, the array one shorter. Returns m_internalInfo1.
uint32_t remove_at(uint32_t self, uint32_t hash, int32_t index)
{
    const uint32_t data = r32(self + kData), cap = r32(self + kCap);
    const uint32_t user = r32(data + kPairBytes * (uint32_t)index + kPairInfo);
    unlink(self, hash, index);
    const uint32_t size = r32(self + kSize);
    const int32_t last = (int32_t)size - 1;
    if (last != index) {
        const uint32_t le = data + kPairBytes * (uint32_t)last;
        const uint32_t last_hash = bt_hash(uid(r32(le + kPairP0)), uid(r32(le + kPairP1))) & (cap - 1);   // no swap, as Bullet
        unlink(self, last_hash, last);
        memmove(vm_base + data + kPairBytes * (uint32_t)index, vm_base + le, kPairBytes);
        const uint32_t ht = r32(self + kHashData), next = r32(self + kNextData);
        w32(next + 4 * (uint32_t)index, r32(ht + 4 * last_hash));
        w32(ht + 4 * last_hash, (uint32_t)index);
    }
    w32(self + kSize, size - 1);   // pop_back
    return user;
}

// ---- the three operations: 1 handled (ctx->gpr[3] as the function returns it), 0 declined (nothing written),
// -1 (check mode's native run only) given up after writing: the caller puts the input back --------------------------
enum Why { kWhyFilter, kWhyOrder, kWhyGrowth, kWhyVirtual, kWhyCallback, kWhyCheck, kWhyCount };
const char* const kWhyNames[kWhyCount] = { "filter/ghost", "order", "growth", "virtual", "callback", "check" };
std::atomic<unsigned long long> g_why[3][kWhyCount];
inline int no(int op, Why why) { g_why[op][why]++; return 0; }

thread_local int t_inner;       // a lifted function called from here: its own hook declines
thread_local bool t_simulate;   // check mode's native run: no game code called

void call_lifted(ppu_context* ctx, void (*fn)(ppu_context*), uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6)
{
    const uint64_t lr = ctx->lr;
    ctx->gpr[3] = a3; ctx->gpr[4] = a4; ctx->gpr[5] = a5; ctx->gpr[6] = a6;
    t_inner++;
    fn(ctx);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf((void*)ctx); }
    t_inner--;
    ctx->lr = lr;
}

// cache->cleanOverlappingPair(pair, dispatcher) (the cache's virtual is checked to be Bullet's): a pair with a
// collision algorithm has it destroyed and freed by the game's dispatcher -- the lifted function; in check mode's native
// run only what the cache sees of that, m_algorithm cleared
void clean(ppu_context* ctx, uint32_t cache, uint32_t pair, uint32_t dispatcher)
{
    if (!r32(pair + kPairAlg)) return;
    if (t_simulate) { w32(pair + kPairAlg, 0); return; }
    call_lifted(ctx, func_00913D88, cache, pair, dispatcher, 0);
}

// removeOverlappingPair(a, b, dispatcher) on a cache whose ghost callback is null and whose cleanOverlappingPair is
// Bullet's, the proxies in an order both builds agree on: m_internalInfo1 of the pair removed, or 0
uint32_t remove_pair(ppu_context* ctx, uint32_t self, const Pair& p, uint32_t dispatcher, uint32_t counters)
{
    counter_add(counters, kRemovePairs, 1);
    const uint32_t hash = bt_hash(uid(p.p0), uid(p.p1)) & (r32(self + kCap) - 1);
    const int32_t index = find(self, p.p0, p.p1, hash);
    if (index == kNull) return 0;
    clean(ctx, self, r32(self + kData) + kPairBytes * (uint32_t)index, dispatcher);
    return remove_at(self, hash, index);
}

int add_native(ppu_context* ctx)
{
    const uint32_t self = (uint32_t)ctx->gpr[3], a = (uint32_t)ctx->gpr[4], b = (uint32_t)ctx->gpr[5];
    if (r32(self + kFilter) || r32(self + kGhost)) return no(0, kWhyFilter);
    const uint32_t counters = r32((uint32_t)ctx->gpr[2] + kTocCounters);
    // needsBroadphaseCollision with no filter: each proxy's group against the other's mask
    const bool needs = (s16(a + kProxyGroup) & s16(b + kProxyMask)) != 0 && (s16(b + kProxyGroup) & s16(a + kProxyMask)) != 0;
    if (!needs) { counter_add(counters, kAddedPairs, 1); ctx->gpr[3] = 0; return 1; }
    Pair p;
    if (!ordered(a, b, p)) return no(0, kWhyOrder);
    const uint32_t cap = r32(self + kCap), hash = bt_hash(uid(p.p0), uid(p.p1)) & (cap - 1);
    const int32_t found = find(self, p.p0, p.p1, hash);
    const uint32_t data = r32(self + kData);
    if (found != kNull) { counter_add(counters, kAddedPairs, 1); ctx->gpr[3] = data + kPairBytes * (uint32_t)found; return 1; }
    const uint32_t size = r32(self + kSize);
    if (size >= cap) return no(0, kWhyGrowth);   // expand() would grow the array (the game's allocator) and the tables
    counter_add(counters, kAddedPairs, 1);
    const uint32_t e = data + kPairBytes * size;
    w32(e + kPairP0, p.p0); w32(e + kPairP1, p.p1); w32(e + kPairAlg, 0); w32(e + kPairInfo, 0);
    w32(self + kSize, size + 1);
    const uint32_t ht = r32(self + kHashData), next = r32(self + kNextData);
    w32(next + 4 * size, r32(ht + 4 * hash));
    w32(ht + 4 * hash, size);
    ctx->gpr[3] = e;
    return 1;
}

int remove_native(ppu_context* ctx)
{
    const uint32_t self = (uint32_t)ctx->gpr[3], a = (uint32_t)ctx->gpr[4], b = (uint32_t)ctx->gpr[5];
    const uint32_t dispatcher = (uint32_t)ctx->gpr[6];
    if (r32(self + kGhost)) return no(1, kWhyFilter);
    if (code_of(r32(self), kVClean) != kFnClean) return no(1, kWhyVirtual);
    Pair p;
    if (!ordered(a, b, p)) return no(1, kWhyOrder);
    const uint32_t counters = r32((uint32_t)ctx->gpr[2] + kTocCounters);
    const uint32_t user = remove_pair(ctx, self, p, dispatcher, counters);
    ctx->gpr[3] = user;
    return 1;
}

// removeOverlappingPairsContainingProxy's scan (a RemovePairCallback for `proxy`) on a cache without a ghost
// callback whose removeOverlappingPair and cleanOverlappingPair are Bullet's: 1, or -1 in check mode's native run
// when a pair would need the lifted removeOverlappingPair
int remove_containing(ppu_context* ctx, uint32_t self, uint32_t proxy, uint32_t dispatcher, uint32_t counters)
{
    for (uint32_t i = 0; i < r32(self + kSize); ) {
        const uint32_t e = r32(self + kData) + kPairBytes * i, p0 = r32(e + kPairP0), p1 = r32(e + kPairP1);
        if (p0 != proxy && p1 != proxy) { i++; continue; }
        Pair p;
        if (ordered(p0, p1, p)) remove_pair(ctx, self, p, dispatcher, counters);
        else if (t_simulate) { g_why[2][kWhyCheck]++; return -1; }
        else call_lifted(ctx, func_009144C8, self, p0, p1, dispatcher);
        counter_add(counters, kOverlappingPairs, -1);
    }
    return 1;
}

int process_native(ppu_context* ctx)
{
    const uint32_t self = (uint32_t)ctx->gpr[3], cb = (uint32_t)ctx->gpr[4], dispatcher = (uint32_t)ctx->gpr[5];
    const uint32_t toc = (uint32_t)ctx->gpr[2], cbvt = r32(cb), vt = r32(self);
    const bool rem = cbvt == r32(toc + kTocRemoveCb) && code_of(cbvt, kVProcessOverlap) == kFnRemoveCb;
    const bool cln = cbvt == r32(toc + kTocCleanCb) && code_of(cbvt, kVProcessOverlap) == kFnCleanCb;
    if (!rem && !cln) return no(2, kWhyCallback);
    const uint32_t proxy = r32(cb + 4), counters = r32(toc + kTocCounters);
    if (rem) {   // RemovePairCallback: each pair with the proxy removed (this->removeOverlappingPair), gOverlappingPairs--
        if (r32(self + kGhost)) return no(2, kWhyFilter);
        if (code_of(vt, kVRemove) != kFnRemove || code_of(vt, kVClean) != kFnClean) return no(2, kWhyVirtual);
        return remove_containing(ctx, self, proxy, dispatcher, counters);
    }
    // CleanPairCallback: each pair with the proxy cleaned (its pair cache's cleanOverlappingPair); never removes
    const uint32_t cache2 = r32(cb + 8), dispatcher2 = r32(cb + 0x0C);
    if (code_of(r32(cache2), kVClean) != kFnClean) return no(2, kWhyVirtual);
    for (uint32_t i = 0; i < r32(self + kSize); i++) {
        const uint32_t e = r32(self + kData) + kPairBytes * i;
        if (r32(e + kPairP0) == proxy || r32(e + kPairP1) == proxy) clean(ctx, cache2, e, dispatcher2);
    }
    return 1;
}

// ---- check mode: the native result against the lifted one, from the same input -------------------------------
struct Snap {
    std::vector<uint8_t> cache, pairs, hash, next, counters;
    uint32_t pairs_ea = 0, hash_ea = 0, next_ea = 0;
};
Snap snap(uint32_t self, uint32_t counters)
{
    Snap s;
    s.cache.assign(vm_base + self, vm_base + self + kCacheBytes);
    s.pairs_ea = r32(self + kData); s.hash_ea = r32(self + kHashData); s.next_ea = r32(self + kNextData);
    s.pairs.assign(vm_base + s.pairs_ea, vm_base + s.pairs_ea + kPairBytes * r32(self + kCap));
    s.hash.assign(vm_base + s.hash_ea, vm_base + s.hash_ea + 4 * r32(self + kHashCap));
    s.next.assign(vm_base + s.next_ea, vm_base + s.next_ea + 4 * r32(self + kNextCap));
    s.counters.assign(vm_base + counters, vm_base + counters + 16);
    return s;
}
void restore(uint32_t self, uint32_t counters, const Snap& s)
{
    memcpy(vm_base + self, s.cache.data(), s.cache.size());
    memcpy(vm_base + s.pairs_ea, s.pairs.data(), s.pairs.size());
    memcpy(vm_base + s.hash_ea, s.hash.data(), s.hash.size());
    memcpy(vm_base + s.next_ea, s.next.data(), s.next.size());
    memcpy(vm_base + counters, s.counters.data(), 16);
}
bool same(const Snap& a, const Snap& b)
{
    return a.cache == b.cache && a.pairs == b.pairs && a.hash == b.hash && a.next == b.next && a.counters == b.counters &&
           a.pairs_ea == b.pairs_ea && a.hash_ea == b.hash_ea && a.next_ea == b.next_ea;
}

struct Stats { std::atomic<unsigned long long> handled{0}, declined{0}, compared{0}, differ{0}, inner{0}, alg{0}; };
Stats g_st[3];
const char* const kOpNames[3] = { "add", "remove", "scan" };
int g_mode;   // 1 native, 2 check, 3 lifted (from the hooks)

// DOTHEROES_PAIRCACHE_TIME: one record a call, the counter mapped to the trace's clock when written
struct TimeRec { uint64_t tsc; uint32_t cycles; uint16_t op, native; };
TimeRec* g_tbuf;
std::atomic<uint32_t> g_tn{0};
constexpr uint32_t kTimeCap = 1u << 21;
uint64_t g_tsc0, g_ns0;
const char* g_tpath;
void time_write()
{
    static std::atomic<int> once{0};
    if (!g_tbuf || once.exchange(1)) return;
    const uint64_t tsc1 = __rdtsc(), ns1 = ps3_ft_now();
    const double ns_per = (double)(ns1 - g_ns0) / (double)(tsc1 - g_tsc0);
    FILE* f = fopen(g_tpath, "w");
    if (!f) return;
    fprintf(f, "# PAIRCACHE_TIME: us on the frame trace's clock, op (0 add, 1 remove, 2 scan), ns, native (%s)\n",
            g_mode == 3 ? "the lifted functions from the hooks" : "native where handled");
    const uint32_t n = std::min(g_tn.load(), kTimeCap);
    for (uint32_t i = 0; i < n; i++) {
        const TimeRec& r = g_tbuf[i];
        const double ns = (double)g_ns0 + (double)(int64_t)(r.tsc - g_tsc0) * ns_per - (double)g_ps3_ft.base;
        fprintf(f, "%.1f %u %.0f %u\n", ns / 1e3, r.op, r.cycles * ns_per, r.native);
    }
    fclose(f);
}
std::atomic<unsigned long long> g_calls{0};

void report()
{
    char line[1024]; int n = 0;
    n += snprintf(line + n, sizeof line - n, "[pair-cache] %s:", g_mode == 2 ? "check" : g_mode == 3 ? "lifted" : "native");
    for (int k = 0; k < 3; k++) {
        n += snprintf(line + n, sizeof line - n, " %s %llu handled", kOpNames[k], g_st[k].handled.load());
        if (g_mode == 2) n += snprintf(line + n, sizeof line - n, " (%llu with an algorithm)", g_st[k].alg.load());
        n += snprintf(line + n, sizeof line - n, ", %llu declined [", g_st[k].declined.load());
        for (int w = 0; w < kWhyCount; w++)
            if (g_why[k][w].load()) n += snprintf(line + n, sizeof line - n, " %s %llu", kWhyNames[w], g_why[k][w].load());
        n += snprintf(line + n, sizeof line - n, " ], %llu nested;", g_st[k].inner.load());
    }
    if (g_mode == 2)
        for (int k = 0; k < 3; k++)
            n += snprintf(line + n, sizeof line - n, " %s compared %llu, differ %llu;", kOpNames[k], g_st[k].compared.load(), g_st[k].differ.load());
    fprintf(stderr, "%s\n", line);
    time_write();
}

// a pair of this call with a collision algorithm (counted: the calls that reach the game's dispatcher)
bool touches_alg(int op, ppu_context* ctx)
{
    const uint32_t self = (uint32_t)ctx->gpr[3];
    if (op == 0) return false;
    if (op == 1) {
        Pair p;
        if (!ordered((uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5], p)) return false;
        const int32_t i = find(self, p.p0, p.p1, bt_hash(uid(p.p0), uid(p.p1)) & (r32(self + kCap) - 1));
        return i != kNull && r32(r32(self + kData) + kPairBytes * (uint32_t)i + kPairAlg);
    }
    const uint32_t proxy = r32((uint32_t)ctx->gpr[4] + 4);
    for (uint32_t i = 0; i < r32(self + kSize); i++) {
        const uint32_t e = r32(self + kData) + kPairBytes * i;
        if ((r32(e + kPairP0) == proxy || r32(e + kPairP1) == proxy) && r32(e + kPairAlg)) return true;
    }
    return false;
}

int run_op(int op, ppu_context* ctx, int (*native)(ppu_context*), void (*lifted)(ppu_context*))
{
    if (t_inner) { g_st[op].inner++; return 0; }
    if (g_mode != 2 && !g_tbuf) {
        const int r = native(ctx);
        (r ? g_st[op].handled : g_st[op].declined)++;
        return r;
    }
    if (g_mode != 2) {   // timed: the call done here either way, the lifted function when the native code declines
        const uint64_t a3 = ctx->gpr[3], a4 = ctx->gpr[4], a5 = ctx->gpr[5], a6 = ctx->gpr[6];
        const uint64_t c0 = __rdtsc();
        const int r = g_mode == 3 ? 0 : native(ctx);
        (r ? g_st[op].handled : g_st[op].declined)++;
        if (!r) call_lifted(ctx, lifted, (uint32_t)a3, (uint32_t)a4, (uint32_t)a5, (uint32_t)a6);
        const uint64_t c1 = __rdtsc();
        const uint32_t i = g_tn.fetch_add(1, std::memory_order_relaxed);
        if (i < kTimeCap) g_tbuf[i] = { c0, (uint32_t)(c1 - c0), (uint16_t)op, (uint16_t)(r != 0) };
        return 1;
    }
    // check: native on the live cache (no game code called), its result kept; the input put back; the lifted
    // function; compared
    if (++g_calls % 20000 == 0) report();
    const uint32_t self = (uint32_t)ctx->gpr[3], counters = r32((uint32_t)ctx->gpr[2] + kTocCounters);
    const bool alg = touches_alg(op, ctx);
    const Snap in = snap(self, counters);
    const uint64_t a3 = ctx->gpr[3], a4 = ctx->gpr[4], a5 = ctx->gpr[5], a6 = ctx->gpr[6];
    t_simulate = true;
    const int r = native(ctx);
    t_simulate = false;
    if (r <= 0) {
        if (r < 0) restore(self, counters, in);
        g_st[op].declined++;
        ctx->gpr[3] = a3;   // the lifted body runs from the same arguments
        return 0;
    }
    g_st[op].handled++;
    if (alg) g_st[op].alg++;
    const uint32_t r_native = (uint32_t)ctx->gpr[3];
    const Snap out_native = snap(self, counters);
    restore(self, counters, in);
    call_lifted(ctx, lifted, (uint32_t)a3, (uint32_t)a4, (uint32_t)a5, (uint32_t)a6);
    const Snap out_lifted = snap(self, counters);
    g_st[op].compared++;
    if (!same(out_native, out_lifted) || (op != 2 && r_native != (uint32_t)ctx->gpr[3])) {
        static std::atomic<int> shown{0};
        if (g_st[op].differ++ < 1 || shown++ < 10)
            fprintf(stderr, "[pair-cache] check: %s differs%s (native r3 %08X, lifted %08X; cache %d pairs %d hash %d next %d counters %d)\n",
                    kOpNames[op], alg ? " (with an algorithm)" : "", r_native, (uint32_t)ctx->gpr[3], out_native.cache == out_lifted.cache,
                    out_native.pairs == out_lifted.pairs, out_native.hash == out_lifted.hash, out_native.next == out_lifted.next,
                    out_native.counters == out_lifted.counters);
    }
    return 1;   // the lifted function's result stands
}

/* DOTHEROES_NATIVE_PAIRCACHE, read once dotheroes.cfg is applied (port/main.cpp: default 1) */
void init()
{
    {
        const char* e = getenv("DOTHEROES_NATIVE_PAIRCACHE");
        g_mode = !e ? 0 : !strcmp(e, "check") ? 2 : !strcmp(e, "lifted") ? 3 : atoi(e) == 1 ? 1 : 0;
        g_native_paircache = g_mode != 0;
        if (!g_mode) return;
        const char* t = getenv("DOTHEROES_PAIRCACHE_TIME");
        if (t && *t && g_mode != 2 && (g_tbuf = (TimeRec*)malloc(sizeof(TimeRec) * kTimeCap)) != nullptr) {
            g_tpath = t;
            (void)PS3_FT_ON();   // the trace's clock base set, as for DIAG_FRAMES
            g_tsc0 = __rdtsc(); g_ns0 = ps3_ft_now();
            atexit(time_write);
        }
        if (g_mode == 3 && !g_tbuf) { g_mode = 0; g_native_paircache = 0; return; }   // lifted without timing: off
        atexit(report);
        g_dotheroes_paircache_report = report;
        fprintf(stderr, "[pair-cache] Bullet's hashed pair cache: %s (DOTHEROES_NATIVE_PAIRCACHE)%s\n",
                g_mode == 2 ? "native checked against the lifted functions, call by call" : g_mode == 3 ? "the lifted functions" : "native",
                g_tbuf ? ", each call timed" : "");
    }
}

}  // namespace paircache

using namespace paircache;

/* For port/axis_sweep.cpp's check mode: one of the cache's operations done natively without calling game code (a freed
 * algorithm is its field cleared): op 0 addOverlappingPair(a, b), 1 removeOverlappingPair(a, b, dispatcher), 2
 * removeOverlappingPairsContainingProxy(a, dispatcher) -- the cache's virtuals checked to be the hashed cache's. 1 done
 * (*ret what the function returns), 0 not possible that way: nothing written, or (op 2) what it wrote is the caller's
 * to put back. ctx->gpr[3..6] are used. */
extern "C" int dotheroes_paircache_simulate(ppu_context* ctx, int op, uint32_t self, uint32_t a, uint32_t b, uint32_t dispatcher,
                                            uint32_t* ret)
{
    const uint32_t vt = r32(self);
    if (op == 0 && code_of(vt, 0x08) != 0x00A4CD24u) return 0;
    if (op >= 1 && code_of(vt, kVRemove) != kFnRemove) return 0;
    if (op == 2 && (code_of(vt, 0x10) != 0x0091555Cu || r32(self + kGhost) || code_of(vt, kVClean) != kFnClean)) return 0;
    t_simulate = true;
    int r;
    if (op == 2) {
        r = remove_containing(ctx, self, a, dispatcher, r32((uint32_t)ctx->gpr[2] + kTocCounters)) > 0;
        ctx->gpr[3] = 0;
    } else {
        ctx->gpr[3] = self; ctx->gpr[4] = a; ctx->gpr[5] = b; ctx->gpr[6] = dispatcher;
        r = (op == 0 ? add_native : remove_native)(ctx) > 0;
    }
    t_simulate = false;
    if (r && ret) *ret = (uint32_t)ctx->gpr[3];
    return r;
}

/* For port/axis_sweep.cpp's check mode: 1 when the operation (op 0 addOverlappingPair(a, b), 1 removeOverlappingPair(a,
 * b), 2 removeOverlappingPairsContainingProxy(a)) would write nothing beyond the cache, its arrays and the counters --
 * the cache's virtuals Bullet's, no overlap filter or ghost callback, no growth, no algorithm to free -- so the check
 * can run it the way the game does, through the cache's own virtual. */
extern "C" int dotheroes_paircache_pure(ppu_context* ctx, int op, uint32_t self, uint32_t a, uint32_t b)
{
    (void)ctx;
    const uint32_t vt = r32(self);
    if (r32(self + kGhost)) return 0;
    if (op == 0 && (code_of(vt, 0x08) != 0x00A4CD24u || r32(self + kFilter))) return 0;
    if (op >= 1 && (code_of(vt, kVRemove) != kFnRemove || code_of(vt, kVClean) != kFnClean)) return 0;
    if (op == 2) {
        if (code_of(vt, 0x10) != 0x0091555Cu) return 0;
        for (uint32_t i = 0; i < r32(self + kSize); i++) {
            const uint32_t e = r32(self + kData) + kPairBytes * i;
            if ((r32(e + kPairP0) == a || r32(e + kPairP1) == a) && r32(e + kPairAlg)) return 0;
        }
        return 1;
    }
    Pair p;
    if (!ordered(a, b, p)) return 0;
    const int32_t i = find(self, p.p0, p.p1, bt_hash(uid(p.p0), uid(p.p1)) & (r32(self + kCap) - 1));
    if (op == 0) return i != kNull || r32(self + kSize) < r32(self + kCap);
    return i == kNull || !r32(r32(self + kData) + kPairBytes * (uint32_t)i + kPairAlg);
}

/* The entry hooks (config/r8_hooks.json): btHashedOverlappingPairCache::addOverlappingPair (0x00A4CD24),
 * removeOverlappingPair (0x009144C8) and processAllOverlappingPairs (0x00914054); 0 declines (the lifted body runs). */
extern "C" void dotheroes_paircache_init(void) { paircache::init(); }   // port/main.cpp, after the settings
extern "C" int dotheroes_paircache_add(ppu_context* ctx) { return run_op(0, ctx, add_native, func_00A4CD24); }
extern "C" int dotheroes_paircache_remove(ppu_context* ctx) { return run_op(1, ctx, remove_native, func_009144C8); }
extern "C" int dotheroes_paircache_process(ppu_context* ctx) { return run_op(2, ctx, process_native, func_00914054); }
