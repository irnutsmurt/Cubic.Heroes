/* memtest.cpp -- `dotheroes --memtest`: the bare guest loads' assumptions,
 * tested against the real guest space (memtest.h; docs/NATIVE_R8.md section
 * 5.3).
 *
 * Every check reads guest memory through both forms of the lifted code's
 * loads -- checked and bare, compiled from the lift's own ppu_recomp.h -- and
 * compares each with what the bytes are: the bytes this test wrote, and zero
 * wherever it wrote nothing (a page the guest never touched). It covers:
 *  - the null page: untouched, then written;
 *  - every size (1, 2, 4, 8) at every alignment;
 *  - an access across a 4 KB page and across the 64 KB unit the commit
 *    handler commits, with neither side, the first or the second side
 *    committed beforehand (the commit state is checked first, so the fault
 *    really happens inside the access);
 *  - an access across 4 GB, into the guard the reservation carries past it;
 *  - addresses with bits above 32 set (the guest's 64-bit effective address);
 *  - the special addresses the checked loads' slow path treats: the GCM poll
 *    word and the get word, a raw SPU's problem state and local store;
 *  - a load in a spin loop seeing another thread's store (not hoisted);
 *  - aligned loads never torn by a concurrent store, and what misaligned
 *    ones do (reported: not single-copy atomic on Cell either);
 *  - the reservation itself: one allocation from the base to the guard's end.
 * A watchdog ends the test if a stage stops making progress (a fault the
 * commit handler did not resolve would retry forever). */
#include "memtest.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

extern "C" uint8_t* vm_base;
extern "C" uint32_t ppu_hle_inject_base;

namespace {

uint64_t g_vm_size;
int g_checks, g_fail, g_group_fail;
std::atomic<const char*> g_stage{"start"};
std::atomic<int> g_done{0};

struct Written { uint32_t ea; std::vector<uint8_t> b; };
std::vector<Written> g_written;

void fail(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("[memtest]   FAIL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    g_fail++; g_group_fail++;
}

void group(const char* name) { g_stage.store(name); g_group_fail = 0; }
void group_end(const char* name, int n) { printf("[memtest] %s %s (%d checks)\n", g_group_fail ? "FAIL" : "pass", name, n); }

/* What a guest byte is: the last value this test wrote there, else 0. Guest
 * offsets are 32-bit; the 64 KB past 4 GB is the guard, never written. */
uint8_t exp_byte(uint64_t at)
{
    for (size_t i = g_written.size(); i-- > 0;) {
        const Written& w = g_written[i];
        if (at >= w.ea && at < (uint64_t)w.ea + w.b.size()) return w.b[at - w.ea];
    }
    return 0;
}
uint64_t expect(int size, uint64_t ea)
{
    const uint64_t a = (uint32_t)ea;
    uint64_t v = 0;
    for (int i = 0; i < size; i++) v = (v << 8) | exp_byte(a + i);
    return v;
}
void put(uint32_t ea, const uint8_t* b, size_t n)
{
    memcpy(vm_base + ea, b, n);
    g_written.push_back({ ea, std::vector<uint8_t>(b, b + n) });
}
void put_pattern(uint32_t ea, size_t n, unsigned seed)
{
    std::vector<uint8_t> b(n);
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)((i + seed) * 37 + 11) | 1;   /* never 0 */
    put(ea, b.data(), n);
}

#ifdef _WIN32
bool committed(uint64_t off)
{
    MEMORY_BASIC_INFORMATION mi{};
    return VirtualQuery(vm_base + off, &mi, sizeof mi) && mi.State == MEM_COMMIT;
}
#else
bool committed(uint64_t) { return false; }
#endif

using reader = uint64_t (*)(int, uint64_t);
const char* name_of(reader r) { return r == mt_bare_read ? "bare" : "checked"; }

int check_read(reader r, int size, uint64_t ea, const char* what)
{
    g_checks++;
    const uint64_t got = r(size, ea), want = expect(size, ea);
    if (got != want)
        fail("%s: %s read%d at 0x%llX = 0x%llX, the bytes are 0x%llX", what, name_of(r), size * 8,
             (unsigned long long)ea, (unsigned long long)got, (unsigned long long)want);
    return 1;
}
int check_both(int size, uint64_t ea, const char* what)
{
    check_read(mt_checked_read, size, ea, what);
    check_read(mt_bare_read, size, ea, what);
    return 2;
}

const int kSizes[] = { 1, 2, 4, 8 };

/* ---- the null page */
void t_null()
{
    group("null page");
    int n = 0;
    g_checks++; n++;
    if (committed(0)) fail("the null page is committed before the test touched it");
    /* untouched: the first access commits it; both forms read 0 */
    n += check_read(mt_bare_read, 8, 0, "untouched null page");
    n += check_read(mt_checked_read, 8, 0, "untouched null page");
    for (int s : kSizes)
        for (uint32_t off : { 0u, 1u, 3u, 7u, 0x100u, 0xFFF8u, 0xFFFCu })
            n += check_both(s, off, "untouched null page");
    /* anything there (the runtime drops a guest's stores below 0x1000; this
     * writes from the host) reads back the same through both */
    put_pattern(0x100, 16, 1);
    put_pattern(0xFFF0, 16, 2);
    for (int s : kSizes)
        for (uint32_t off : { 0x100u, 0x101u, 0x103u, 0x107u, 0xFFF8u, 0xFFFCu, 0xFFFDu })
            n += check_both(s, off, "written null page");
    /* across 0x10000, into the first user page */
    for (int s : { 2, 4, 8 })
        n += check_both(s, 0x10000u - 1, "the null page's end");
    group_end("null page: untouched and written, every size", n);
}

/* ---- sizes and alignments, a 4 KB page boundary */
const uint32_t A = 0x10000000u;
void t_align()
{
    group("alignment");
    int n = 0;
    put_pattern(A, 64, 3);
    for (int s : kSizes)
        for (uint32_t off = 0; off < 24; off++)
            n += check_both(s, A + off, "alignment");
    group_end("every size at every alignment", n);

    group("page");
    n = 0;
    put_pattern(A + 0xFF0, 32, 4);
    for (int s : { 2, 4, 8 })
        for (int k = 1; k < s; k++)
            n += check_both(s, A + 0x1000 - k, "across a 4 KB page");
    group_end("across a 4 KB page", n);
}

/* ---- across the 64 KB commit unit, the fault inside the access */
void t_commit()
{
    group("commit unit");
    int n = 0, idx = 0;
    const char* states[] = { "neither side committed", "the first side committed", "the second side committed" };
    for (reader r : { mt_bare_read, mt_checked_read })
        for (int st = 0; st < 3; st++)
            for (int s : { 2, 4, 8 })
                for (int k = 1; k < s; k++) {
                    const uint32_t base = 0x30000000u + (uint32_t)idx++ * 0x20000u;
                    const uint32_t x = base + 0x10000u;
                    if (st == 1) put_pattern(x - 8, 8, (unsigned)idx);
                    if (st == 2) put_pattern(x, 8, (unsigned)idx);
                    g_checks++; n++;
                    if (committed(base) != (st == 1) || committed(x) != (st == 2))
                        fail("commit state before the access is not %s (0x%08X)", states[st], base);
                    char what[96];
                    snprintf(what, sizeof what, "across a commit unit, %s", states[st]);
                    n += check_read(r, s, x - k, what);
                    g_checks++; n++;
                    if (!committed(base) || !committed(x))
                        fail("the access across 0x%08X left a side uncommitted", x);
                }
    group_end("across the 64 KB commit unit (neither, first or second side committed)", n);
}

/* ---- across 4 GB: the guard */
void t_4gb()
{
    group("4 GB");
    int n = 0;
    g_checks++; n++;
    if (committed(0xFFFF0000ull) || committed(0x100000000ull))
        fail("the last unit or the guard is committed before the test touched it");
    n += check_read(mt_bare_read, 8, 0xFFFFFFFCull, "across 4 GB, untouched");
    n += check_read(mt_checked_read, 8, 0xFFFFFFFAull, "across 4 GB, untouched");
    put_pattern(0xFFFFFFF0u, 16, 5);
    for (int s : kSizes)
        for (int k = 1; k <= s; k++)
            n += check_both(s, 0x100000000ull - k, "at the end of 4 GB");
    group_end("across 4 GB into the guard", n);

    group("high bits");
    n = 0;
    for (int s : kSizes)
        for (uint64_t hi : { 0x100000000ull, 0xFFFFFFFF00000000ull, 0x8000000000000000ull })
            n += check_both(s, hi | (A + 8), "an address with bits above 32");
    group_end("addresses with bits above 32 (taken modulo 4 GB)", n);
}

/* ---- the slow path's special addresses */
void t_special()
{
    group("special");
    int n = 0;
    const uint32_t get = ppu_hle_inject_base + 0x2004u, ref = ppu_hle_inject_base + 0x2008u;
    const uint8_t gv[4] = { 0x12, 0x34, 0x56, 0x78 }, rv[4] = { 0x9A, 0xBC, 0xDE, 0xF1 };
    put(get, gv, 4);
    put(ref, rv, 4);
    /* in a build with the RSX command path, the checked read of the poll word
     * publishes fences (cellGcm_ref_on_poll): the bare loads are for a build
     * without it, where both forms read the word as it is */
    const bool bare_build = mt_build_bare() != 0;
    for (uint32_t ea : { get, ref }) {
        g_checks += 2; n += 2;
        const uint64_t c = mt_checked_read(4, ea), b = mt_bare_read(4, ea), w = expect(4, ea);
        if (b != w) fail("the GCM word 0x%08X: bare 0x%llX, the bytes 0x%llX", ea, (unsigned long long)b, (unsigned long long)w);
        if (c != w) {
            if (bare_build) fail("the GCM word 0x%08X: checked 0x%llX, the bytes 0x%llX", ea, (unsigned long long)c, (unsigned long long)w);
            else printf("[memtest]   note: this build's checked read of 0x%08X has a side effect (0x%llX): the RSX command path\n",
                        ea, (unsigned long long)c);
        }
    }
    /* a raw SPU's window, none created: plain memory to both */
    for (uint32_t off : { 0x100u, 0x44004u, 0x4400Cu, 0x44014u, 0x5C00Cu }) {
        put_pattern(0xE0000000u + off, 4, off);
        n += check_both(4, 0xE0000000u + off, "a raw SPU's window (none created)");
    }
    group_end("the GCM get and poll words, a raw SPU's window", n);
}

/* ---- a spin on a guest word sees another thread's store */
void t_spin()
{
    group("spin");
    int n = 0;
    for (int s : kSizes) {
        const uint32_t w = A + 0x200u + (uint32_t)s * 16u;
        const uint8_t z[8] = {};
        put(w, z, 8);
        std::thread setter([w, s] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            __atomic_store_n(vm_base + w + (s - 1), (uint8_t)1, __ATOMIC_RELEASE);
        });
        const auto t0 = std::chrono::steady_clock::now();
        const int seen = mt_bare_spin(s, w, 4000000000ull);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        setter.join();
        g_checks++; n++;
        if (!seen) fail("a %d-byte spin never saw the store (%.0f ms): the load was hoisted", s, ms);
        uint8_t one[8] = {}; one[s - 1] = 1;
        g_written.push_back({ w, std::vector<uint8_t>(one, one + 8) });
    }
    group_end("a load in a spin loop sees another thread's store", n);
}

/* ---- concurrent stores: torn or not */
uint64_t torn_run(int size, uint32_t ea, uint64_t n, bool aligned_store)
{
    std::atomic<int> stop{0};
    const uint64_t a = 0x1111111111111111ull >> (64 - size * 8), b = 0xEEEEEEEEEEEEEEEEull >> (64 - size * 8);
    std::thread writer([&] {
        uint8_t* p = vm_base + ea;
        for (uint64_t i = 0; !stop.load(std::memory_order_relaxed); i++) {
            const uint64_t v = (i & 1) ? b : a;   /* byte-uniform: the same either endianness */
            if (aligned_store) {
                switch (size) {
                case 2: __atomic_store_n((uint16_t*)p, (uint16_t)v, __ATOMIC_RELAXED); break;
                case 4: __atomic_store_n((uint32_t*)p, (uint32_t)v, __ATOMIC_RELAXED); break;
                default: __atomic_store_n((uint64_t*)p, v, __ATOMIC_RELAXED); break;
                }
            } else {
                typedef uint16_t u16u __attribute__((aligned(1)));
                typedef uint32_t u32u __attribute__((aligned(1)));
                typedef uint64_t u64u __attribute__((aligned(1)));
                switch (size) {   /* one instruction each, as a lifted store is */
                case 2: *(volatile u16u*)p = (uint16_t)v; break;
                case 4: *(volatile u32u*)p = (uint32_t)v; break;
                default: *(volatile u64u*)p = v; break;
                }
            }
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const uint64_t t = mt_bare_torn(size, ea, a, b, n);
    stop.store(1);
    writer.join();
    uint8_t buf[8]; memcpy(buf, vm_base + ea, 8);
    g_written.push_back({ ea, std::vector<uint8_t>(buf, buf + 8) });
    return t;
}
void t_torn()
{
    group("torn");
    int n = 0;
    const uint64_t N = 20000000;
    const uint32_t line = A + 0x1000u;   /* 64-byte aligned */
    for (int s : { 2, 4, 8 }) {
        const uint64_t t = torn_run(s, line + 0x40u * (uint32_t)s, N, true);
        g_checks++; n++;
        if (t) fail("aligned %d-byte loads: %llu of %llu torn by a concurrent store", s, (unsigned long long)t, (unsigned long long)N);
    }
    group_end("aligned loads against a concurrent store: never torn", n);
    /* reported, not required: Cell gives a misaligned access no single-copy
     * atomicity either, and the checked loads did the same (a memcpy) */
    const uint64_t in_line = torn_run(8, line + 0x201u, N, false);
    const uint64_t across = torn_run(8, line + 0x23Cu, N, false);
    printf("[memtest] note: misaligned 8-byte loads torn by a concurrent store: %llu of %llu inside a cache line, "
           "%llu of %llu across one (not single-copy atomic on Cell either)\n",
           (unsigned long long)in_line, (unsigned long long)N, (unsigned long long)across, (unsigned long long)N);
}

/* ---- the reservation */
void t_reservation()
{
    group("reservation");
    int n = 0;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION a{}, b{};
    g_checks++; n++;
    if (!VirtualQuery(vm_base, &a, sizeof a) || !VirtualQuery(vm_base + g_vm_size - 1, &b, sizeof b) ||
        a.AllocationBase != vm_base || b.AllocationBase != vm_base)
        fail("the guest space and its guard are not one reservation from the base");
    g_checks++; n++;
    if (g_vm_size < 0x100000000ull + 8)
        fail("the reservation is 0x%llX bytes: no guard past 4 GB for an access's last 7 bytes", (unsigned long long)g_vm_size);
    g_checks++; n++;
    if (*(volatile const uint64_t*)(vm_base + g_vm_size - 8) != 0) fail("the guard's last bytes are not zero");
#else
    g_checks++; n++;
    fail("not established on this platform: the bare loads are a Windows build's");
#endif
    group_end("one reservation from the base through the guard", n);
}

} // namespace

extern "C" int dotheroes_memtest(uint64_t vm_size)
{
    g_vm_size = vm_size;
    printf("[memtest] the guest loads, checked and bare, against the guest space at %p (0x%llX bytes); "
           "this build's lifted code: %s\n", (void*)vm_base, (unsigned long long)vm_size,
           mt_build_bare() ? "bare (PPU_BARE_LOADS)" : "checked");
    std::thread watchdog([] {
        for (int i = 0; i < 600 && !g_done.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!g_done.load()) {
            printf("[memtest] FAIL: no progress in \"%s\" for 60 s (a fault the commit handler did not resolve?)\n",
                   g_stage.load());
            fflush(stdout);
            _Exit(3);
        }
    });
    t_null();
    t_align();
    t_commit();
    t_4gb();
    t_special();
    t_spin();
    t_torn();
    t_reservation();
    g_done.store(1);
    watchdog.join();
    printf("[memtest] %d checks, %d failed: %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    fflush(stdout);
    return g_fail ? 1 : 0;
}
