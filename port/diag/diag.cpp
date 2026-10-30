/*
 * Development diagnostics: crash reports and hang watchdog. See diag.h.
 *
 * A report names, for the faulting (or every) thread:
 *   - the signal / exception, fault address, and whether it is a guest EA
 *   - the guest context: lr, ctr, r1-r3, cia, guest thread id
 *   - the last firmware (HLE) call
 *   - the guest stack walked by back chain, each frame symbolized to the
 *     lifted function that contains it (func_XXXXXXXX+off)
 *   - the host backtrace, with lifted host frames mapped back to guest
 * and is written to stderr and to DIAG_CRASH_DIR/<kind>-<time>-<pid>.txt.
 */
#include "diag.h"

#ifdef DOTHEROES_DEV_DIAGNOSTICS

#include "ppu_recomp.h"
#include "ppu_tls.h"   /* PPU_THREAD_LOCAL */

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifndef _WIN32
#  include <dirent.h>
#  include <dlfcn.h>
#  include <execinfo.h>
#  include <pthread.h>
#  include <signal.h>
#  include <sys/stat.h>
#  include <sys/syscall.h>
#  include <ucontext.h>
#  include <unistd.h>
#else
#  include <windows.h>
#  include <direct.h>
#endif

extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern "C" uint32_t    g_last_hle_nid;
extern "C" const char* g_last_hle_name;
extern "C" unsigned    cellGcm_flip_request_count(void);
extern "C" void        ppu_guest_caller(char* out, size_t n);

namespace {

/* ---- report sink: stderr plus a file ---------------------------------- */
FILE* g_report = nullptr;

void rp(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
#ifndef _WIN32
    /* Straight to fd 2: ps3recomp swaps stderr for a 64 KB buffer drained by a
     * flusher thread, and using that FILE from a signal handler faulted. */
    vdprintf(2, fmt, ap);
#else
    vfprintf(stderr, fmt, ap);
#endif
    va_end(ap);
    if (g_report) {
        va_start(ap, fmt);
        vfprintf(g_report, fmt, ap);
        va_end(ap);
    }
}

const char* report_dir()
{
    const char* d = getenv("DIAG_CRASH_DIR");
    return d && *d ? d : "diag";
}

void open_report(const char* kind)
{
    const char* dir = report_dir();
#ifdef _WIN32
    _mkdir(dir);
    int pid = (int)GetCurrentProcessId();
#else
    mkdir(dir, 0755);
    int pid = (int)getpid();
#endif
    char stamp[32];
    time_t t = time(nullptr);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&t));
    char path[1024];
    snprintf(path, sizeof path, "%s/%s-%s-%d.txt", dir, kind, stamp, pid);
    g_report = fopen(path, "w");
    rp("[diag] writing %s report to %s\n", kind, g_report ? path : "(stderr only)");
}

void close_report()
{
    if (g_report) { fclose(g_report); g_report = nullptr; }
}

/* ---- guest symbolization ---------------------------------------------- */
/* Nearest lifted function at or below a guest address. */
bool guest_symbol(uint32_t ea, uint32_t* fn)
{
    uint64_t best = 0;
    for (uint64_t i = 0; i < function_table_count; i++) {
        uint64_t a = function_table[i].addr;
        if (a <= ea && a > best) best = a;
    }
    if (!best || ea - best > 0x100000) return false;
    *fn = (uint32_t)best;
    return true;
}

/* Lifted host function containing a host PC, if any. */
bool host_to_guest(uintptr_t pc, uint32_t* guest, uintptr_t* off)
{
    uintptr_t bh = 0; uint64_t bg = 0;
    for (uint64_t i = 0; i < function_table_count; i++) {
        uintptr_t h = (uintptr_t)function_table[i].func;
        if (h <= pc && h > bh) { bh = h; bg = function_table[i].addr; }
    }
    if (!bg || pc - bh > 0x40000) return false;
    *guest = (uint32_t)bg; *off = pc - bh;
    return true;
}

uint32_t guest_r32(uint32_t ea)
{
    if (!vm_base || ea == 0 || ea > 0xFFFFFFF0u) return 0;
    const uint8_t* p = vm_base + ea;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

bool guest_ea_of(uintptr_t host, uint32_t* ea)
{
    if (!vm_base) return false;
    uintptr_t b = (uintptr_t)vm_base;
    if (host < b || host - b >= 0x100000000ull) return false;
    *ea = (uint32_t)(host - b);
    return true;
}

void dump_guest(ppu_context* ctx)
{
    if (!ctx) { rp("  guest: (no PPU context on this thread)\n"); return; }
    rp("  guest: tid=%llu cia=0x%08X lr=0x%08X ctr=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X\n",
       (unsigned long long)ctx->thread_id, (uint32_t)ctx->cia, (uint32_t)ctx->lr,
       (uint32_t)ctx->ctr, (uint32_t)ctx->gpr[1], (uint32_t)ctx->gpr[2], (uint32_t)ctx->gpr[3]);
    rp("  gpr:");
    for (int i = 0; i < 32; i++) rp("%s r%d=%08X", i % 8 ? "" : "\n   ", i, (uint32_t)ctx->gpr[i]);
    rp("\n  guest stack (back chain; LR save at sp+16):\n");
    uint32_t fn;
    uint32_t lr = (uint32_t)ctx->lr;
    if (guest_symbol(lr, &fn)) rp("    #0 lr 0x%08X  func_%08X+0x%X\n", lr, fn, lr - fn);
    uint32_t sp = (uint32_t)ctx->gpr[1];
    for (int depth = 1; depth < 48 && sp; depth++) {
        uint32_t next = guest_r32(sp + 4);          /* 64-bit back chain, low word */
        if (!next || next <= sp || next - sp > 0x100000) break;
        uint32_t ret = guest_r32(next + 16 + 4);    /* caller's saved LR, low word */
        if (ret && guest_symbol(ret, &fn))
            rp("    #%d sp 0x%08X ret 0x%08X  func_%08X+0x%X\n", depth, next, ret, fn, ret - fn);
        else
            rp("    #%d sp 0x%08X ret 0x%08X\n", depth, next, ret);
        sp = next;
    }
}

void dump_host_backtrace()
{
#ifndef _WIN32
    void* bt[48];
    int n = backtrace(bt, 48);
    rp("  host backtrace:\n");
    for (int i = 0; i < n; i++) {
        uintptr_t pc = (uintptr_t)bt[i];
        uint32_t g; uintptr_t off;
        Dl_info di;
        const char* sym = (dladdr(bt[i], &di) && di.dli_sname) ? di.dli_sname : nullptr;
        if (host_to_guest(pc, &g, &off))
            rp("    %2d %p  [lifted func_%08X +host 0x%lX]\n", i, bt[i], g, (unsigned long)off);
        else
            rp("    %2d %p  %s\n", i, bt[i], sym ? sym : "?");
    }
#endif
}

void dump_this_thread(const char* why)
{
#ifndef _WIN32
    long tid = syscall(SYS_gettid);
#else
    long tid = (long)GetCurrentThreadId();
#endif
    char who[64] = "";
    ppu_guest_caller(who, sizeof who);
    rp("--- host thread %ld (%s)%s%s\n", tid, why, who[0] ? " in " : "", who);
    rp("  last HLE call: 0x%08X %s\n", g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
    dump_guest(g_active_ctx);
    dump_host_backtrace();
}

#ifndef _WIN32
/* ---- POSIX crash handler ---------------------------------------------- */
std::atomic<int> g_in_crash{0};
const int k_crash_signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };
const int k_dump_signal_off = 5;   /* SIGRTMIN+5: SIGRTMIN+4 is ps3recomp's suspend */

void crash_handler(int sig, siginfo_t* si, void* uc_)
{
    if (g_in_crash.exchange(1)) {          /* fault while reporting: give up */
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    open_report("crash");
    uintptr_t host_pc = 0;
#if defined(__x86_64__)
    host_pc = (uintptr_t)((ucontext_t*)uc_)->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    host_pc = (uintptr_t)((ucontext_t*)uc_)->uc_mcontext.pc;
#endif
    /* si_addr is meaningful only for hardware faults, not for abort(). */
    bool has_addr = si && (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE);
    rp("=== CRASH: signal %d (%s) code %d", sig, strsignal(sig), si ? si->si_code : 0);
    if (has_addr) rp(", fault address %p", si->si_addr);
    rp("\n");
    uint32_t ea;
    if (has_addr && guest_ea_of((uintptr_t)si->si_addr, &ea))
        rp("  fault address is guest EA 0x%08X\n", ea);
    uint32_t g; uintptr_t off;
    if (host_pc && host_to_guest(host_pc, &g, &off))
        rp("  faulting host pc %p is in lifted func_%08X (+host 0x%lX)\n", (void*)host_pc, g,
           (unsigned long)off);
    else if (host_pc) {
        Dl_info di;
        rp("  faulting host pc %p (%s)\n", (void*)host_pc,
           dladdr((void*)host_pc, &di) && di.dli_sname ? di.dli_sname : "?");
    }
    rp("  frames presented (guest flips): %u\n", cellGcm_flip_request_count());
    dump_this_thread("faulting");
    rp("=== end of crash report\n");
    close_report();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ---- all-thread dump via a real-time signal --------------------------- */
std::atomic<int> g_dump_pending{0};
pthread_mutex_t g_dump_lock = PTHREAD_MUTEX_INITIALIZER;

void dump_signal_handler(int, siginfo_t*, void*)
{
    pthread_mutex_lock(&g_dump_lock);       /* not async-signal-safe; dev only */
    dump_this_thread("snapshot");
    pthread_mutex_unlock(&g_dump_lock);
    g_dump_pending.fetch_sub(1);
}

void* watchdog_main(void*)
{
    pthread_setname_np(pthread_self(), "diag-watchdog");
    const char* e;
    unsigned hang_secs = (e = getenv("DIAG_HANG_SECS")) ? (unsigned)atoi(e) : 10;
    unsigned boot_secs = (e = getenv("DIAG_BOOT_SECS")) ? (unsigned)atoi(e) : 120;
    bool hang_exit = (e = getenv("DIAG_HANG_EXIT")) && *e == '1';
    /* DIAG_DUMP_AT=<s>[,<s>...]: also dump every thread at these seconds after
     * start, for a game that keeps flipping but stops making progress. */
    unsigned dump_at[16]; int n_dump = 0;
    if ((e = getenv("DIAG_DUMP_AT")))
        for (char* p = (char*)e; *p && n_dump < 16; ) {
            dump_at[n_dump++] = (unsigned)strtoul(p, &p, 10);
            while (*p == ',') p++;
        }
    unsigned elapsed = 0;
    unsigned last = cellGcm_flip_request_count();
    unsigned still = 0;
    bool reported = false;
    const bool fps_log = (e = getenv("DIAG_FPS")) && *e == '1';
    unsigned fps_last = last;
    struct timespec w0; clock_gettime(CLOCK_MONOTONIC, &w0);
    for (;;) {
        /* By the clock: signals (DIAG_PROF samples this thread too) end
         * sleep() early, and counting wake-ups ran this clock 1000x fast. */
        struct timespec wn;
        do { usleep(100000); clock_gettime(CLOCK_MONOTONIC, &wn); }
        while ((unsigned)(wn.tv_sec - w0.tv_sec) <= elapsed);
        const unsigned target = (unsigned)(wn.tv_sec - w0.tv_sec);
        if (target > elapsed + 1) still += target - elapsed - 1;
        elapsed = target - 1;
        elapsed++;
        /* DIAG_FPS=1: guest flips per second, every 10 s. */
        if (fps_log && elapsed % 10 == 0) {
            const unsigned f = cellGcm_flip_request_count();
            fprintf(stderr, "[diag] %u s: %.1f flips/s (%u total)\n", elapsed, (f - fps_last) / 10.0, f);
            fps_last = f;
        }
        for (int i = 0; i < n_dump; i++)
            if (dump_at[i] == elapsed) {
                char why[64];
                snprintf(why, sizeof why, "requested at %u s (DIAG_DUMP_AT)", elapsed);
                diag_dump_all_threads(why);
            }
        unsigned now = cellGcm_flip_request_count();
        if (now != last) { last = now; still = 0; reported = false; continue; }
        still++;
        unsigned limit = now ? hang_secs : boot_secs;
        if (still >= limit && !reported) {
            char why[96];
            snprintf(why, sizeof why, "no guest flip for %u s (%u frames so far)", still, now);
            diag_dump_all_threads(why);
            reported = true;
            if (hang_exit) { fprintf(stderr, "[diag] DIAG_HANG_EXIT: exit(4)\n"); _exit(4); }
        }
    }
    return nullptr;
}
#endif /* !_WIN32 */

#ifdef _WIN32
LONG WINAPI win_crash_filter(EXCEPTION_POINTERS* ep)
{
    open_report("crash");
    rp("=== CRASH: exception 0x%08lX at %p\n", ep->ExceptionRecord->ExceptionCode,
       ep->ExceptionRecord->ExceptionAddress);
    uint32_t g; uintptr_t off;
    if (host_to_guest((uintptr_t)ep->ExceptionRecord->ExceptionAddress, &g, &off))
        rp("  in lifted func_%08X (+host 0x%lX)\n", g, (unsigned long)off);
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uint32_t ea;
        uintptr_t a = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        rp("  access %s %p%s\n", ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
           (void*)a, guest_ea_of(a, &ea) ? " (guest EA)" : "");
    }
    dump_this_thread("faulting");
    rp("=== end of crash report\n");
    close_report();
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

} // namespace

extern "C" void diag_thread_init(void)
{
#ifndef _WIN32
    stack_t ss;
    ss.ss_size = 256 * 1024;
    ss.ss_sp = malloc(ss.ss_size);
    ss.ss_flags = 0;
    if (ss.ss_sp) sigaltstack(&ss, nullptr);
#endif
}

extern "C" void diag_install_crash_handler(void)
{
#ifndef _WIN32
    diag_thread_init();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int s : k_crash_signals) sigaction(s, &sa, nullptr);

    struct sigaction ds;
    memset(&ds, 0, sizeof ds);
    ds.sa_sigaction = dump_signal_handler;
    ds.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&ds.sa_mask);
    sigaction(SIGRTMIN + k_dump_signal_off, &ds, nullptr);
#else
    SetUnhandledExceptionFilter(win_crash_filter);
#endif
    fprintf(stderr, "[diag] crash handler installed (reports in %s/)\n", report_dir());
}

extern "C" void diag_dump_all_threads(const char* reason)
{
#ifndef _WIN32
    pthread_mutex_lock(&g_dump_lock);
    open_report("hang");
    rp("=== THREAD DUMP: %s\n", reason);
    pthread_mutex_unlock(&g_dump_lock);
    /* Every host thread dumps itself from the signal handler. */
    long self = syscall(SYS_gettid);
    pid_t pid = getpid();
    DIR* d = opendir("/proc/self/task");
    int sent = 0;
    if (d) {
        while (dirent* de = readdir(d)) {
            long tid = atol(de->d_name);
            if (tid <= 0 || tid == self) continue;
            g_dump_pending.fetch_add(1);
            if (syscall(SYS_tgkill, pid, tid, SIGRTMIN + k_dump_signal_off) == 0) sent++;
            else g_dump_pending.fetch_sub(1);
        }
        closedir(d);
    }
    /* Threads blocked with the signal masked never answer; don't wait forever. */
    for (int i = 0; i < 300 && g_dump_pending.load() > 0; i++) usleep(10000);
    pthread_mutex_lock(&g_dump_lock);
    rp("=== end of thread dump (%d threads signalled, %d did not answer)\n", sent,
       g_dump_pending.load());
    close_report();
    pthread_mutex_unlock(&g_dump_lock);
#else
    (void)reason;
#endif
}

#ifndef _WIN32
/* ---- Sampling profiler (DIAG_PROF=<start s>,<seconds>) ------------------------
 * Every thread is signalled about once a millisecond for <seconds>, starting
 * <start s> after launch; each records its raw return addresses into a
 * preallocated table. Afterwards they go to <DIAG_CRASH_DIR>/profile.raw with
 * the executable's load base, and tests/prof_report.py symbolizes them (lifted
 * functions included: they are ordinary symbols of the binary). The handler
 * only calls backtrace() (warmed up first) and gettid; SA_RESTART keeps
 * interrupted waits waiting. */
namespace {
constexpr int k_prof_signal_off = 6;
constexpr unsigned k_prof_max = 400000, k_prof_depth = 20;
void* (*g_prof_pc)[k_prof_depth];
int*   g_prof_tid;
unsigned char* g_prof_n;
std::atomic<unsigned> g_prof_count{0};
volatile int g_prof_on;

void prof_handler(int, siginfo_t*, void*)
{
    if (!g_prof_on) return;
    const unsigned i = g_prof_count.fetch_add(1, std::memory_order_relaxed);
    if (i >= k_prof_max) return;
    g_prof_tid[i] = (int)syscall(SYS_gettid);
    g_prof_n[i] = (unsigned char)backtrace(g_prof_pc[i], k_prof_depth);
}

void* prof_main(void* arg)
{
    const unsigned start = ((unsigned*)arg)[0], dur = ((unsigned*)arg)[1];
    pthread_setname_np(pthread_self(), "diag-prof");
    sleep(start);
    g_prof_pc  = (void* (*)[k_prof_depth])calloc(k_prof_max, sizeof *g_prof_pc);
    g_prof_tid = (int*)calloc(k_prof_max, sizeof(int));
    g_prof_n   = (unsigned char*)calloc(k_prof_max, 1);
    if (!g_prof_pc || !g_prof_tid || !g_prof_n) return nullptr;
    { void* warm[4]; backtrace(warm, 4); }
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prof_handler; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGRTMIN + k_prof_signal_off, &sa, nullptr);
    fprintf(stderr, "[diag] profiling every thread for %u s\n", dur);
    g_prof_on = 1;
    const long self = syscall(SYS_gettid); const pid_t pid = getpid();
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    long tids[256]; int nt = 0; unsigned tick = 0;
    for (;;) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - t0.tv_sec >= (long)dur) break;
        if (tick++ % 100 == 0) {            /* refresh the thread list */
            nt = 0;
            if (DIR* d = opendir("/proc/self/task")) {
                while (dirent* de = readdir(d)) {
                    long tid = atol(de->d_name);
                    if (tid > 0 && tid != self && nt < 256) tids[nt++] = tid;
                }
                closedir(d);
            }
        }
        for (int k = 0; k < nt; k++) syscall(SYS_tgkill, pid, tids[k], SIGRTMIN + k_prof_signal_off);
        usleep(1000);
    }
    g_prof_on = 0;
    usleep(20000);
    char path[600];
    snprintf(path, sizeof path, "%s/profile.raw", report_dir());
    mkdir(report_dir(), 0755);
    FILE* f = fopen(path, "w");
    if (f) {
        Dl_info di;
        if (dladdr((void*)&prof_main, &di)) fprintf(f, "base %p %s\n", di.dli_fbase, di.dli_fname);
        const unsigned n = g_prof_count.load() < k_prof_max ? g_prof_count.load() : k_prof_max;
        for (unsigned i = 0; i < n; i++) {
            fprintf(f, "%d", g_prof_tid[i]);
            for (unsigned j = 0; j < g_prof_n[i]; j++) fprintf(f, " %p", g_prof_pc[i][j]);
            fputc('\n', f);
        }
        fclose(f);
        fprintf(stderr, "[diag] profile: %u samples -> %s\n", n, path);
    }
    return nullptr;
}
} // namespace
#endif

extern "C" void diag_start_watchdog(void)
{
#ifndef _WIN32
    const char* e = getenv("DIAG_WATCHDOG");
    if (e && *e == '0') return;
    pthread_t t;
    pthread_create(&t, nullptr, watchdog_main, nullptr);
    pthread_detach(t);
    if (const char* p = getenv("DIAG_PROF")) {
        static unsigned arg[2] = { 0, 10 };
        sscanf(p, "%u,%u", &arg[0], &arg[1]);
        pthread_t pt;
        pthread_create(&pt, nullptr, prof_main, arg);
        pthread_detach(pt);
    }
#endif
}

#else /* !DOTHEROES_DEV_DIAGNOSTICS: release build, no diagnostics */

extern "C" void diag_install_crash_handler(void) {}
extern "C" void diag_thread_init(void) {}
extern "C" void diag_start_watchdog(void) {}
extern "C" void diag_dump_all_threads(const char*) {}

#endif
