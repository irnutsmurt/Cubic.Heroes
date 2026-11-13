// R8: the game's Edge zlib inflate done in the calling thread, without SPURS
// (docs/SPU_NON_AUDIO.md section 2).
//
// What the game does, read from the lift (build/r8/gen8, the SPU shell in
// build/gen/spu/spu_0002_at_00F49F00):
//   func_000752A0 (the map pack loader) calls, through the TOC stub
//   func_009824B8, func_005CF5BC(r3 the .gz image's EA, r4 its size, r5 the
//   output's EA), and nothing else calls it. For ONE pack it:
//    - initializes a SPURS instance of its own (1 SPU) in a static block
//      (B = *(r2+0x1AF8): attribute at B+0x1988, instance at B+0x1C00);
//    - calls func_005CF228(spurs, gz, size, out), which reads the gzip header
//      (func_0052EE34: the deflate data's start; func_0052EFF8: its size, the
//      rest less the 8-byte trailer; func_0052EDF8: ISIZE, the trailer's last
//      word), creates a taskset at B, an event flag at B+0x1900 (auto clear,
//      SPU to PPU), Edge's LF queue (16 entries, memalign'ed) and Edge's
//      inflate task (image 3, context save memalign'ed), sets the work-to-do
//      counter B+0x1980 to 1, pushes ONE request (edgeZlibAddInflateQueueElement,
//      func_0069C888 -> func_0069C60C: _cellSpursLFQueuePushBody), waits on the
//      event flag for bit 1 (cellSpursEventFlagWait, AND), then frees, detaches,
//      shuts the taskset down and joins it;
//    - finalizes the instance; r3 = 0.
//   The task, per request (0x31A0..0x3598 in the shell): inflates the stream
//   into the output (exactly ISIZE bytes: the large path through
//   edgeZlibFetchAndInflateLargeRawData, already native in edge_inflate.c; the
//   small path, both sizes <= 64 KB, through local store), then decrements the
//   counter (cellAtomicDecr32; on an error it first ORs in 0x80000000) and, as
//   the counter leaves 1, sets the event flag's bit (cellSpursEventFlagSet).
//   No size word is written back: the output size is the request's.
//
// So the whole SPURS round trip -- instance, kernel and pool threads, taskset,
// event flag, queue, task, 2 ms polled wait, teardown -- is the envelope of
// one synchronous inflate the calling thread waits for. Here, through one
// entry hook on func_005CF5BC (config/r8_hooks.json, port/phys_hooks.cpp):
// the same header walk, the same inflate (inflate.c, as edge_inflate.c runs
// it for the task), in the calling thread, then the guest state the task
// leaves behind that anyone could read: the output bytes and the counter's
// final 0. None of the SPURS objects is created, so nothing is left to set,
// clear, free or join.
//
// The header is walked as the game's own walk (func_0052EE34) walks it
// (gz_walk.h, checked branch by branch in tests/native/gz_walk_check.cpp): every
// flag it takes is taken the same way, and the flags it refuses are a damaged
// pack. Compressed data bigger than its output is inflated, as Edge's push
// (func_0069C60C) only warns of it. M6 (docs/OPENAL_MIGRATION_PLAN.md section
// 10.2): the map-pack loader is this function's only caller and every pack on
// the disc has FNAME alone, so only a damaged installation reaches the rest.
// A damaged pack -- a header the game refuses, a name or comment running into
// the trailer, a stream that does not fill the output -- is handled natively
// as the game's SPU task ends one: the output as far as it was inflated, the
// counter left with its error bit (0x80000000; nothing in the game reads it),
// and a line saying which pack and why. Declined still (the lifted body runs
// the SPURS path): what the game's own code cannot produce -- another static
// block, a buffer on a stack or outside guest memory -- and a host allocation
// that fails.
//
// DOTHEROES_INFLATE_DIRECT=0 leaves every pack to SPURS. The direct path is
// also off with DOTHEROES_NATIVE_INFLATE=0 (the lifted SPU routine) or =check
// (which compares inside the task). DOTHEROES_INFLATE_LOG=1 logs every pack in
// edge_inflate.c's format (tests/inflate_frames.py). DOTHEROES_INFLATE_CRC=1
// checks every output against its .gz trailer's CRC-32 and logs it; the SPURS
// path logs the same line (edge_inflate.c), so two runs can be compared pack
// by pack.
#include "inflate_direct.h"

#include "gz_walk.h"
#include "inflate.h"
#include "ps3emu/ps3_ftrace.h"
#include "ps3emu/census.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" uint8_t* vm_base;                                     // the guest's memory (ppu_memory.h)
extern "C" uint32_t vm_read32(uint64_t a);
extern "C" void vm_write32(uint64_t a, uint32_t v);
extern "C" void spu_lockline_lock(void);                         // spu_coherency.h
extern "C" void spu_lockline_unlock(void);
extern "C" int spu_coh_is_reserved(uint32_t addr);
extern "C" void spu_coh_notify_write(uint32_t ea);
extern "C" void (*g_spu_put_hook)(uint32_t ea, uint32_t size);   // spu_channels.c (the RSX command path's)
extern "C" unsigned char g_vm_page_bitmap[65536 / 8];             // spu_vm_pagemap.c: committed 64 KB pages

extern "C" int g_dotheroes_inflate_direct = 0;

namespace {

constexpr int kImageEdgeZlib = 3;          // the runtime's image id of Edge zlib's task, for the frame trace
constexpr uint32_t kBlockAt = 0x1AF8;      // r2 + this: the static block B (func_005CF5BC, func_005CF228)
constexpr uint32_t kCounter = 0x1980;      // B + this: the request's work-to-do counter
constexpr uint32_t kSmall = 0x10000;       // the shell's local-store path: output and input both at most this

enum Why : int {
    kWhyBlock,    // r2+0x1AF8 is not the 128-aligned block the game's function uses
    kWhyGz,       // the image not in usable guest memory
    kWhyStack,    // an address in the stack area (Edge's sys_process_is_stack check prints one)
    kWhyMemory,   // a range outside usable guest memory
    kWhyNoTemp,   // in place, and no host buffer for it
    kWhyCount
};
const char* const kWhyText[kWhyCount] = { "static block", "image", "stack", "memory", "no buffer" };

int g_log_each, g_crc;
std::atomic<unsigned long long> g_n{ 0 }, g_small{ 0 }, g_in_place{ 0 }, g_in{ 0 }, g_out{ 0 }, g_ns{ 0 },
    g_inflate_ns{ 0 }, g_max_ns{ 0 }, g_crc_equal{ 0 }, g_crc_differ{ 0 }, g_damaged{ 0 }, g_bigger{ 0 },
    g_declined{ 0 }, g_why[kWhyCount];

inline uint32_t host_tid()
{
#ifdef _WIN32
    return (uint32_t)GetCurrentThreadId();
#else
    return 0;
#endif
}

int decline(Why why, uint32_t gz, uint32_t size, uint32_t out, const char* detail)
{
    ps3_census(PS3_CENSUS_NATIVE_DECLINE, 4, (unsigned)why);   // M6's census: a = 4 the inflate, b = why
    g_why[why].fetch_add(1, std::memory_order_relaxed);
    const unsigned long long n = g_declined.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16)
        fprintf(stderr, "[inflate-direct] through SPURS (%s%s%s): .gz 0x%08X %u B, output 0x%08X\n", kWhyText[why],
                detail ? ": " : "", detail ? detail : "", gz, size, out);
    return 0;
}

// All of [ea, ea+size) usable as the runtime's DMA check (spu_dma.h,
// mfc_ea_range_committed) finds its two end pages, here for the whole range:
// a 64 KB page the runtime's bitmap knows committed, else committed and
// accessible by VirtualQuery, or reserved guest space committed now (64 KB at
// a time) as an SPU DMA or a PPU store's first touch would. Free space, or
// memory without access (or, for the output, without write access), is not.
bool usable(uint32_t ea, uint32_t size, bool write)
{
    if (!vm_base || !size || ea < 0x10000u || (uint64_t)ea + size > 0x100000000ull) return false;
#ifdef _WIN32
    const uint64_t end = (uint64_t)ea + size;
    for (uint64_t a = ea; a < end;) {
        const uint32_t pg = (uint32_t)(a >> 16);
        if ((g_vm_page_bitmap[pg >> 3] >> (pg & 7)) & 1) {
            a = (uint64_t)(pg + 1) << 16;
            continue;
        }
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(vm_base + a, &mbi, sizeof mbi)) return false;
        if (mbi.State == MEM_RESERVE) {
            if (!VirtualAlloc(vm_base + ((uint64_t)pg << 16), 0x10000, MEM_COMMIT, PAGE_READWRITE)) return false;
            a = (uint64_t)(pg + 1) << 16;
            continue;
        }
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        if (write && !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) return false;
        a = (uint64_t)((const uint8_t*)mbi.BaseAddress - vm_base) + mbi.RegionSize;   // the region's end
    }
#else
    (void)write;
#endif
    return true;
}

// Edge's PPU side (func_0069C60C) prints an error for a buffer on a stack
// (sys_process_is_stack), and the runtime answers that for this range
// (ppu_sysprx.cpp).
inline bool on_stack(uint32_t ea) { return ea >= 0x0E000000u && ea < 0x10000000u; }

void report()
{
    const unsigned long long n = g_n.load(), d = g_declined.load();
    if (!g_dotheroes_inflate_direct && !n && !d) return;
    if (!n && !d) {
        fprintf(stderr, "[inflate-direct] on, but no pack came through the hook (a lift without it inflates "
                "through SPURS)\n");
        return;
    }
    fprintf(stderr, "[inflate-direct] %llu packs inflated in the calling thread (%.1f MB from %.1f MB; %.1f ms "
            "inflating, %.1f ms in all, the longest call %.1f ms; %llu small, %llu in place; %llu damaged), %llu "
            "through SPURS\n",
            n, g_out.load() / 1e6, g_in.load() / 1e6, g_inflate_ns.load() / 1e6, g_ns.load() / 1e6,
            g_max_ns.load() / 1e6, g_small.load(), g_in_place.load(), g_damaged.load(), d);
    if (d) {
        char b[256] = "";
        size_t at = 0;
        for (int w = 0; w < kWhyCount; w++) {
            const unsigned long long k = g_why[w].load();
            if (k && at < sizeof b)
                at += (size_t)snprintf(b + at, sizeof b - at, "%s %s %llu", at ? "," : "", kWhyText[w], k);
        }
        fprintf(stderr, "[inflate-direct] through SPURS by reason:%s\n", b);
    }
    if (g_crc)
        fprintf(stderr, "[inflate-direct] CRC-32 against the .gz trailers: %llu equal, %llu different\n",
                g_crc_equal.load(), g_crc_differ.load());
}

struct Init {
    Init()
    {
        const char* e = getenv("DOTHEROES_INFLATE_DIRECT");
        const char* ni = getenv("DOTHEROES_NATIVE_INFLATE");   // as edge_inflate.c reads it
        const bool native_off = ni && (!strcmp(ni, "check") || (*ni && !atoi(ni)));
        const char* l = getenv("DOTHEROES_INFLATE_LOG");
        const char* c = getenv("DOTHEROES_INFLATE_CRC");
        g_log_each = l && atoi(l);
        g_crc = c && atoi(c);
        if ((e && *e == '0') || native_off) return;
        g_dotheroes_inflate_direct = 1;   // the hook's guard: only a lift with the hook calls in
        atexit(report);
    }
} s_init;

}  // namespace

extern "C" void inflate_direct_report(void) { report(); }

extern "C" int inflate_direct_call(uint32_t gz, uint32_t gz_size, uint32_t out, uint32_t toc)
{
    const uint64_t t0 = ps3_ft_now();

    // The block func_005CF228 keeps the counter in (and the taskset, the event
    // flag and the instance, which are not made here).
    const uint32_t block = vm_read32((uint64_t)toc + kBlockAt);
    if (!block || (block & 0x7Fu)) return decline(kWhyBlock, gz, gz_size, out, nullptr);

    if (!usable(gz, gz_size ? gz_size : 1, false)) return decline(kWhyGz, gz, gz_size, out, "not guest memory");
    if (on_stack(gz) || on_stack(out)) return decline(kWhyStack, gz, gz_size, out, nullptr);
    const uint8_t* const g = vm_base + gz;

    // func_0052EE34's walk (gz_walk.h): the deflate data's start, or why the pack is damaged (a header the game
    // refuses, or one its walk would run past the image with).
    const GzWalk w = gz_walk(g, gz_size);
    const char* const refused = w.refused;
    const uint32_t usize = w.usize, csize = w.csize, crc_want = w.crc;
    const uint32_t in_ea = gz + w.start;
    // Edge's push (func_0069C60C) only warns of compressed data bigger than its output ("Pointless?") and inflates
    // it as any other: so here.
    if (!refused && csize > usize && g_bigger.fetch_add(1, std::memory_order_relaxed) < 4)
        fprintf(stderr, "[inflate-direct] .gz 0x%08X: the compressed data is bigger than its output (%u > %u), as "
                "Edge's warning says; inflated all the same\n", gz, csize, usize);
    if (!refused && usize && !usable(out, usize, true)) return decline(kWhyMemory, gz, gz_size, out, "the output");

    // The inflate, as edge_inflate.c runs it for the task: in place (the
    // deflate data inside the output) through a buffer of its own.
    const bool overlap = usize && (uint64_t)in_ea < (uint64_t)out + usize && (uint64_t)out < (uint64_t)in_ea + csize;
    uint8_t* tmp = overlap && !refused ? (uint8_t*)malloc(usize) : nullptr;
    if (overlap && !refused && !tmp) return decline(kWhyNoTemp, gz, gz_size, out, nullptr);
    uint8_t* const dst = tmp ? tmp : vm_base + out;
    const uint64_t i0 = ps3_ft_now();
    const long got = refused ? 0 : dh_inflate_raw(vm_base + in_ea, csize, dst, usize, nullptr);
    const uint64_t i1 = ps3_ft_now();
    const bool damaged = refused || got != (long)usize;
    if (damaged) {
        g_damaged.fetch_add(1, std::memory_order_relaxed);
        char d[64];
        if (!refused) snprintf(d, sizeof d, "the stream gave %ld of %u bytes", got, usize);
        fprintf(stderr, "[inflate-direct] DAMAGED map pack (.gz 0x%08X, %u B): %s; the output left as inflated and "
                "the request's error bit set, as the game's SPU task leaves a failed inflate (the game file may need "
                "reinstalling)\n", gz, gz_size, refused ? refused : d);
    } else if (g_crc) {   // the task does not check the trailer's CRC; this diagnostic does
        const uint32_t crc = dh_crc32(0, dst, usize);
        if (crc != crc_want) {
            g_crc_differ.fetch_add(1, std::memory_order_relaxed);
            fprintf(stderr, "[inflate-direct] CRC-32 0x%08X, the trailer's 0x%08X: .gz 0x%08X %u B, output 0x%08X\n",
                    crc, crc_want, gz, gz_size, out);
        } else {
            g_crc_equal.fetch_add(1, std::memory_order_relaxed);
            fprintf(stderr, "[edge-inflate] crc 0x%08X of %u bytes from %u at 0x%08X (direct: the trailer's)\n", crc,
                    usize, csize, out);
        }
    }
    if (tmp) {
        if (got > 0) memcpy(vm_base + out, tmp, (size_t)got);   // in place: the bytes the stream gave
        free(tmp);
    }
    // As a store: an SPU holding a reservation on a line written loses it. The range the inflate may have
    // written: the whole output (a refused header writes nothing).
    const uint32_t wrote = refused ? 0 : usize;
    if (wrote) {
        const uint32_t a0 = out & ~127u, a1 = (out + wrote - 1) & ~127u;
        for (uint32_t a = a0;; a += 128u) {
            if (spu_coh_is_reserved(a)) {
                spu_lockline_lock();
                spu_coh_notify_write(a);
                spu_lockline_unlock();
            }
            if (a == a1) break;
        }
        if (g_spu_put_hook) g_spu_put_hook(out, wrote);   // what the task's write showed the RSX command path
    }

    // The task's completion: the counter func_005CF228 set to 1, decremented
    // to 0 -- with the error bit ORed in first on a failed inflate, as the task
    // does. The event flag it would then set, and the wait that would clear
    // it, belong to objects not made here.
    vm_write32((uint64_t)block + kCounter, damaged ? 0x80000000u : 0u);

    const uint64_t t1 = ps3_ft_now();
    if (PS3_FT_ON()) ps3_ft_span(PS3_FT_SPU_TASK, host_tid(), (uint32_t)kImageEdgeZlib, 0, i0, i1);
    const unsigned long long n = g_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (usize <= kSmall && csize <= kSmall) g_small.fetch_add(1, std::memory_order_relaxed);
    if (overlap) g_in_place.fetch_add(1, std::memory_order_relaxed);
    g_in.fetch_add(csize, std::memory_order_relaxed);
    g_out.fetch_add(usize, std::memory_order_relaxed);
    g_inflate_ns.fetch_add(i1 - i0, std::memory_order_relaxed);
    g_ns.fetch_add(t1 - t0, std::memory_order_relaxed);
    for (unsigned long long m = g_max_ns.load(std::memory_order_relaxed);
         t1 - t0 > m && !g_max_ns.compare_exchange_weak(m, t1 - t0, std::memory_order_relaxed);) {
    }
    if (n == 1)
        fprintf(stderr, "[inflate-direct] map packs inflated in the calling thread: no SPURS instance, taskset, "
                "event flag, queue or task (DOTHEROES_INFLATE_DIRECT=0: through SPURS)\n");
    // edge_inflate.c's line (tests/inflate_frames.py): the start on the frame trace's clock
    if (n <= 16 || n % 100 == 0 || g_log_each)
        fprintf(stderr, "[edge-inflate] #%llu native: %u bytes from %u in %.1f ms (direct%s, the call %.1f ms) at "
                "%.3f s\n", n, usize, csize, (i1 - i0) / 1e6, overlap ? ", in place through a buffer" : "",
                (t1 - t0) / 1e6, i0 / 1e9);
    return 1;
}
