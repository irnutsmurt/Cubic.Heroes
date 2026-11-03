/* Edge zlib's large inflate as native code.
 *
 * The game inflates each area's map pack (map/pac_data/*.ps3.pac.gz, 2-10 MB
 * of deflate to 13-43 MB) with Edge zlib's SPU task while its main thread
 * waits on an event flag. Lifted, the task inflates ~140 MB/s: the first
 * dungeon's room (13 MB) held one frame for 93-96 ms on every visit, an area
 * of the overworld ~300 ms inside its load. An entry hook
 * (config/spu_entry_hooks.json) runs inflate.c in place of the task's
 * edgeZlibFetchAndInflateLargeRawData, ~1.1 GB/s, between the same main-memory
 * buffers; the task's own queue, counters and event flag are untouched.
 *
 * DOTHEROES_NATIVE_INFLATE: on unless 0. =check inflates natively into a
 * buffer of its own, lets the lifted routine run as before, and compares the
 * two where the task goes next (the hook at 0x3258), logging each result.
 * DOTHEROES_INFLATE_LOG=1 logs every inflate with its start on the frame
 * trace's clock, for tests/inflate_frames.py (an area change's frames). A
 * cache of inflated packs was tried and dropped: on the same revisits its
 * copies (20-27 ms into guest memory) left the frames as long, and first
 * visits got slower (build/baselines/2026-11-03-pack-cache-ab). */
#include "inflate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "spu_context.h"
#include "spu_coherency.h"
#include "spu_dma.h"

extern void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void spu_lockline_lock(void);
void spu_lockline_unlock(void);

int g_dotheroes_native_inflate = 1;
int g_dotheroes_native_inflate_check = 0;
static int s_log_each;

static long s_native, s_fallback, s_checked, s_differ;
#define COUNT(x) __atomic_add_fetch(&(x), 1, __ATOMIC_RELAXED)

__attribute__((constructor)) static void edge_inflate_init(void)
{
    const char* e = getenv("DOTHEROES_NATIVE_INFLATE");
    if (e && !strcmp(e, "check")) {
        g_dotheroes_native_inflate_check = 1;
    } else if (e && *e && !atoi(e)) {
        g_dotheroes_native_inflate = 0;
    }
    const char* l = getenv("DOTHEROES_INFLATE_LOG");
    s_log_each = l && atoi(l);
}

static double now_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
#endif
}

/* What the check hook compares: this thread's last native result. */
static _Thread_local struct { uint8_t* buf; uint32_t ea, size; long got; double ms; } t_pending;

static int fall_back(const char* why, uint32_t out_ea, uint32_t out_size, uint32_t in_ea, uint32_t in_size)
{
    const long n = COUNT(s_fallback);
    if (n <= 16)
        fprintf(stderr, "[edge-inflate] lifted routine instead (%s): out 0x%08X %u B, in 0x%08X %u B\n", why, out_ea,
                out_size, in_ea, in_size);
    return 0;
}

/* edgeZlibFetchAndInflateLargeRawData: r3 the output's EA, r4 its size (the
 * stream must fill it exactly), r5 the deflate data's EA, r6 its size, r7 a DMA
 * tag, r8-r11 local-store buffers -> r3 0, or the lifted routine runs. */
int dotheroes_edge_inflate_large(spu_context* ctx)
{
    const uint32_t out_ea = ctx->gpr[3]._u32[0], out_size = ctx->gpr[4]._u32[0];
    const uint32_t in_ea = ctx->gpr[5]._u32[0], in_size = ctx->gpr[6]._u32[0];
    if (!out_size || !in_size || out_ea < 0x10000u || in_ea < 0x10000u ||
        !mfc_ea_range_committed(in_ea, in_size) || !mfc_ea_range_committed(out_ea, out_size))
        return fall_back("an address outside committed memory", out_ea, out_size, in_ea, in_size);
    const double t0 = now_ms();
    if (g_dotheroes_native_inflate_check) {
        free(t_pending.buf);
        t_pending.buf = (uint8_t*)malloc(out_size);
        t_pending.ea = out_ea;
        t_pending.size = out_size;
        t_pending.got = t_pending.buf ? dh_inflate_raw(vm_base + in_ea, in_size, t_pending.buf, out_size, NULL) : -9;
        t_pending.ms = now_ms() - t0;
        return 0;
    }
    /* Inflating in place (the deflate data inside the output buffer) would
     * overwrite input not yet read: through a buffer of its own instead. */
    const int overlap = (uint64_t)in_ea < (uint64_t)out_ea + out_size && (uint64_t)out_ea < (uint64_t)in_ea + in_size;
    uint8_t* tmp = overlap ? (uint8_t*)malloc(out_size) : NULL;
    if (overlap && !tmp) return fall_back("no memory", out_ea, out_size, in_ea, in_size);
    const long got = dh_inflate_raw(vm_base + in_ea, in_size, overlap ? tmp : vm_base + out_ea, out_size, NULL);
    if (got != (long)out_size) {
        free(tmp);
        char why[64];
        snprintf(why, sizeof why, "inflate gave %ld", got);
        return fall_back(why, out_ea, out_size, in_ea, in_size);   /* it reports its own error */
    }
    if (tmp) {
        memcpy(vm_base + out_ea, tmp, out_size);
        free(tmp);
    }
    /* As a DMA PUT: an SPU holding a reservation on a line written loses it. */
    const uint32_t a0 = out_ea & ~127u, a1 = (out_ea + out_size - 1) & ~127u;
    for (uint32_t a = a0;; a += 128u) {
        if (spu_coh_is_reserved(a)) {
            spu_lockline_lock();
            spu_coh_notify_write_except(a, ctx);
            spu_lockline_unlock();
        }
        if (a == a1) break;
    }
    if (g_spu_put_hook) g_spu_put_hook(out_ea, out_size);
    memset(&ctx->gpr[3], 0, sizeof ctx->gpr[3]);
    const long n = COUNT(s_native);
    if (n <= 16 || n % 100 == 0 || s_log_each)
        fprintf(stderr, "[edge-inflate] #%ld native: %u bytes from %u in %.1f ms%s at %.3f s\n", n, out_size,
                in_size, now_ms() - t0, overlap ? " (in place, through a buffer)" : "", t0 / 1e3);   /* the trace's clock */
    return 1;
}

/* DOTHEROES_NATIVE_INFLATE=check: the lifted routine has written the guest's
 * copy; compare it with this thread's native one. Always lets the task go on. */
int dotheroes_edge_inflate_check(spu_context* ctx)
{
    (void)ctx;
    if (!t_pending.buf) return 0;
    const long n = COUNT(s_checked);
    size_t at = 0;
    const int same = t_pending.got == (long)t_pending.size &&
                     !memcmp(vm_base + t_pending.ea, t_pending.buf, t_pending.size);
    if (!same) {
        COUNT(s_differ);
        while (at < t_pending.size && vm_base[t_pending.ea + at] == t_pending.buf[at]) at++;
    }
    fprintf(stderr, "[edge-inflate] check #%ld: %u bytes at 0x%08X %s (native %.1f ms, gave %ld)%s\n", n,
            t_pending.size, t_pending.ea, same ? "identical" : "DIFFER", t_pending.ms, t_pending.got,
            same ? "" : " -- first difference below");
    if (!same)
        fprintf(stderr, "[edge-inflate]   first difference at +%zu of %u (%ld differing checks so far)\n", at,
                t_pending.size, s_differ);
    free(t_pending.buf);
    t_pending.buf = NULL;
    return 0;
}
