// R3's check that no CPU code reads PPFX's targets (docs/NATIVE_R3.md).
// Development only, off unless DOTHEROES_PPFX_TARGET_WATCH is set.
//
// DOTHEROES_PPFX_TARGET_WATCH=<lo>-<hi>[,<lo>-<hi>...] (hex RSX local-memory
// offsets): the guest pages of those ranges (EA 0xC0000000 + offset) are made
// inaccessible; an access by any thread -- the game's, the walker's, a
// library's -- faults into a vectored handler that counts it by page, kind
// (read, write), thread and host return address, opens the page again and
// lets the access proceed. A watcher thread closes every page again every
// 250 ms, so a page touched every frame is counted a few times a second, not
// once. Every 10 s and at exit the counts go to
// DOTHEROES_PPFX_TARGET_WATCH_OUT (default ppfx_target_watch.txt), with each
// return address named from the executable's symbols (a lifted function is
// func_<guest address>). The renderer keeps its targets in GPU images, so a
// count here is code reading or writing the guest memory behind a PPFX
// target itself.
//
// Each access is counted with its call stack (the faulting frame and up to
// 11 callers, unwound from the fault's context): a lifted function calls the
// next directly, so the stack is the guest's call chain too, and a host
// function (a library's memcpy) shows who called it.
//
// Main-memory ranges (R4's checks, docs/NATIVE_R4.md): ppfx_watch_add_ea(ea,
// n) adds guest effective addresses [ea, ea + n) at run time (the occlusion
// query's result buffer, found at PPFX's call); only accesses inside the
// range are counted, the rest of its page passes. DOTHEROES_PPFX_EA_WATCH=
// <lo>-<hi>[,...] (hex effective addresses) adds them at start.

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>   // types only: dbghelp.dll is loaded at run time
#endif
#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>

extern "C" uint8_t* vm_base;

namespace {

#ifdef _WIN32
const uint32_t kLocalEa = 0xC0000000u;
// a range: guest effective addresses [lo, hi); `exact` counts only accesses
// inside it (a main-memory range shares its pages), else any access to its pages
struct Range { uint64_t lo, hi; bool exact; };
Range s_ranges[64];
std::atomic<int> s_nr{0};
enum { kDepth = 12 };
// one entry per accessing instruction, thread and kind: the pages it touched (lowest, highest), when
struct Hit { uint64_t rip; uint32_t page, page_hi, tid; uint8_t kind; std::atomic<uint64_t> n; uint64_t first_ms, last_ms;
             uint64_t stack[kDepth]; };
uint64_t s_t0;
enum { kHits = 4096 };
Hit s_hits[kHits];
std::atomic<int> s_nhits{0};
std::atomic<uint64_t> s_lost{0};
std::atomic_flag s_lock = ATOMIC_FLAG_INIT;
char s_out[512];

// 1: an access to count, 2: a page of a watched range (an access outside an
// exact range: let it pass), 0: not watched. *page: the access's guest EA page.
int watched(uintptr_t a, uint32_t* page)
{
    const uintptr_t base = (uintptr_t)vm_base;
    if (a < base || a - base >= 0x100000000ull) return 0;
    const uint64_t ea = a - base;
    int r = 0;
    const int n = s_nr.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++) {
        const Range& g = s_ranges[i];
        if (ea >= (g.lo & ~0xFFFull) && ea < ((g.hi + 0xFFF) & ~0xFFFull)) {
            *page = (uint32_t)(ea & ~0xFFFull);
            if (!g.exact || (ea >= g.lo && ea < g.hi)) return 1;
            r = 2;
        }
    }
    return r;
}

// The stack from the fault's context: the faulting frame, then its callers.
void unwind(const CONTEXT* from, uint64_t out[kDepth])
{
    CONTEXT c = *from;
    memset(out, 0, kDepth * 8);
    for (int i = 0; i < kDepth && c.Rip; i++) {
        out[i] = c.Rip;
        DWORD64 image = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &image, NULL);
        if (!f) {   // a leaf: the return address is on top of the stack
            if (!c.Rsp) break;
            c.Rip = *(DWORD64*)c.Rsp;
            c.Rsp += 8;
        } else {
            PVOID hd = NULL;
            DWORD64 ef = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, f, &c, &hd, &ef, NULL);
        }
    }
}

void count(const uint64_t stack[kDepth], uint32_t page, uint32_t tid, uint8_t kind)
{
    const uint64_t rip = stack[0];
    while (s_lock.test_and_set(std::memory_order_acquire)) {}
    const int n = s_nhits.load(std::memory_order_relaxed);
    int i = 0;
    for (; i < n; i++)
        if (s_hits[i].rip == rip && s_hits[i].tid == tid && s_hits[i].kind == kind &&
            !memcmp(s_hits[i].stack + 1, stack + 1, 4 * 8))
            break;
    if (i == n) {
        if (n == kHits) { s_lost++; s_lock.clear(std::memory_order_release); return; }
        s_hits[n].rip = rip; s_hits[n].page = s_hits[n].page_hi = page; s_hits[n].tid = tid; s_hits[n].kind = kind;
        memcpy(s_hits[n].stack, stack, kDepth * 8);
        s_hits[n].first_ms = GetTickCount64() - s_t0;
        s_nhits.store(n + 1, std::memory_order_relaxed);
    }
    s_hits[i].last_ms = GetTickCount64() - s_t0;
    if (page < s_hits[i].page) s_hits[i].page = page;
    if (page > s_hits[i].page_hi) s_hits[i].page_hi = page;
    s_hits[i].n++;
    s_lock.clear(std::memory_order_release);
}

LONG CALLBACK on_fault(EXCEPTION_POINTERS* e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t a = (uintptr_t)e->ExceptionRecord->ExceptionInformation[1];
    uint32_t page;
    const int w = watched(a, &page);
    if (!w) return EXCEPTION_CONTINUE_SEARCH;
    if (w == 1) {
        uint64_t stack[kDepth];
        unwind(e->ContextRecord, stack);
        count(stack, page, GetCurrentThreadId(), (uint8_t)e->ExceptionRecord->ExceptionInformation[0]);
    }
    // A page never touched is only reserved: the runtime's own handler
    // commits guest pages on first access, so it is passed on to it. A
    // committed page was closed here and is opened again.
    void* p = (void*)((uintptr_t)vm_base + page);
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return EXCEPTION_CONTINUE_SEARCH;
    DWORD old;
    VirtualProtect(p, 0x1000, PAGE_READWRITE, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}

std::atomic<uint64_t> s_closed{0}, s_fail{0};
bool s_on;   // the watch was started (a range list given)

// Every committed page of the ranges closed (protection applies to committed
// pages only; the others fault anyway, into the runtime's commit).
void close_all()
{
    uint64_t closed = 0;
    const int n = s_nr.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        for (uint64_t pg = s_ranges[i].lo & ~0xFFFull; pg < s_ranges[i].hi; pg += 0x1000) {
            void* p = vm_base + pg;
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) continue;
            if (mi.Protect == PAGE_NOACCESS) { closed++; continue; }
            DWORD old;
            if (VirtualProtect(p, 0x1000, PAGE_NOACCESS, &old)) closed++;
            else s_fail++;
        }
    s_closed = closed;
}

void report()
{
    FILE* f = fopen(s_out, "w");
    if (!f) return;
    HANDLE proc = GetCurrentProcess();
    typedef BOOL(WINAPI * SymInit)(HANDLE, PCSTR, BOOL);
    typedef BOOL(WINAPI * SymFrom)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
    typedef DWORD(WINAPI * SymOpts)(DWORD);
    static SymFrom from = nullptr;
    static int tried = 0;
    if (!tried++) {
        HMODULE h = LoadLibraryA("dbghelp.dll");
        SymInit init = h ? (SymInit)(void*)GetProcAddress(h, "SymInitialize") : nullptr;
        SymOpts opts = h ? (SymOpts)(void*)GetProcAddress(h, "SymSetOptions") : nullptr;
        if (init && opts) {
            opts(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            if (init(proc, NULL, TRUE)) from = (SymFrom)(void*)GetProcAddress(h, "SymFromAddr");
        }
    }
    const bool sym = from != nullptr;
    const int n = s_nhits.load();
    fprintf(f, "# ppfx target watch: %d ranges, %d distinct accesses, %llu not counted (table full); "
               "%llu committed pages closed at the last pass, %llu protection failures\n", s_nr.load(), n,
            (unsigned long long)s_lost.load(), (unsigned long long)s_closed.load(), (unsigned long long)s_fail.load());
    fprintf(f, "# count kind lowest-highest page (guest EA) thread [name] address module+offset symbol first_s "
               "last_s (seconds since the watch was set up); one line per instruction, thread, kind and caller chain, "
               "the chain after it ('  < module+offset symbol')\n");
    const int nr = s_nr.load();
    for (int i = 0; i < nr; i++)
        fprintf(f, "# range %llX-%llX%s\n", (unsigned long long)s_ranges[i].lo, (unsigned long long)s_ranges[i].hi,
                s_ranges[i].exact ? " (exact)" : "");
    auto describe = [&](uint64_t a, char* out, size_t cap) {
        char nm[256] = "?";
        if (sym) {
            alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO* s = (SYMBOL_INFO*)buf;
            memset(buf, 0, sizeof buf);
            s->SizeOfStruct = sizeof(SYMBOL_INFO); s->MaxNameLen = 255;
            DWORD64 disp = 0;
            if (from(proc, a, &disp, s)) snprintf(nm, sizeof nm, "%s+0x%llx", s->Name, (unsigned long long)disp);
        }
        char md[MAX_PATH] = "?";
        unsigned long long mo = 0;
        HMODULE hm = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)a, &hm) && hm) {
            char path[MAX_PATH];
            if (GetModuleFileNameA(hm, path, sizeof path)) {
                const char* b = strrchr(path, '\\');
                snprintf(md, sizeof md, "%s", b ? b + 1 : path);
            }
            mo = a - (uintptr_t)hm;
        }
        snprintf(out, cap, "%s+0x%llX %s", md, mo, nm);
    };
    for (int i = 0; i < n; i++) {
        char name[256] = "?";
        if (sym) {
            alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO* s = (SYMBOL_INFO*)buf;
            memset(buf, 0, sizeof buf);
            s->SizeOfStruct = sizeof(SYMBOL_INFO); s->MaxNameLen = 255;
            DWORD64 disp = 0;
            if (from(proc, s_hits[i].rip, &disp, s)) snprintf(name, sizeof name, "%s+0x%llx", s->Name, (unsigned long long)disp);
        }
        // the module and offset (for a symboliser offline) and the thread's name
        char mod[MAX_PATH] = "?";
        unsigned long long modoff = 0;
        HMODULE hm = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)s_hits[i].rip, &hm) && hm) {
            char path[MAX_PATH];
            if (GetModuleFileNameA(hm, path, sizeof path)) {
                const char* b = strrchr(path, '\\');
                snprintf(mod, sizeof mod, "%s", b ? b + 1 : path);
            }
            modoff = s_hits[i].rip - (uintptr_t)hm;
        }
        char tname[128] = "?";
        HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, s_hits[i].tid);
        if (th) {
            PWSTR w = nullptr;
            if (SUCCEEDED(GetThreadDescription(th, &w)) && w) {
                WideCharToMultiByte(CP_UTF8, 0, w, -1, tname, sizeof tname, NULL, NULL);
                LocalFree(w);
            }
            CloseHandle(th);
        }
        fprintf(f, "%llu %s %08X-%08X %u [%s] %llX %s+0x%llX %s %.2f %.2f\n", (unsigned long long)s_hits[i].n.load(),
                s_hits[i].kind == 1 ? "write" : s_hits[i].kind == 8 ? "exec" : "read", s_hits[i].page,
                s_hits[i].page_hi, s_hits[i].tid, tname, (unsigned long long)s_hits[i].rip, mod, modoff, name,
                s_hits[i].first_ms / 1000.0, s_hits[i].last_ms / 1000.0);
        for (int k = 1; k < kDepth && s_hits[i].stack[k]; k++) {
            char d[400];
            describe(s_hits[i].stack[k], d, sizeof d);
            fprintf(f, "  < %s\n", d);
        }
    }
    fclose(f);
}

void watcher()
{
    SetThreadDescription(GetCurrentThread(), L"ppfx target watch");
    for (int k = 1;; k++) {
        Sleep(250);
        close_all();
        if (k % 40 == 0) report();
    }
}
#endif

}  // namespace

// Called by r1_observe.cpp's start-up (a static library's object is linked
// only when something references it).
extern "C" void ppfx_watch_start(void)
{
#ifdef _WIN32
    {
        const char* e = getenv("DOTHEROES_PPFX_TARGET_WATCH");
        const char* ea_list = getenv("DOTHEROES_PPFX_EA_WATCH");
        if ((!e || !*e) && (!ea_list || !*ea_list)) return;
        s_t0 = GetTickCount64();
        for (int pass = 0; pass < 2; pass++) {
            const char* l = pass == 0 ? e : ea_list;
            for (const char* p = l; p && *p && s_nr < 64;) {
                char* q;
                const uint64_t lo = strtoull(p, &q, 16);
                if (*q != '-') break;
                const uint64_t hi = strtoull(q + 1, &q, 16);
                const int k = s_nr.load();
                if (pass == 0) s_ranges[k] = { kLocalEa + (lo & ~0xFFFull), kLocalEa + ((hi + 0xFFFull) & ~0xFFFull), false };
                else s_ranges[k] = { lo, hi, true };
                s_nr.store(k + 1);
                p = *q == ',' ? q + 1 : q;
            }
        }
        s_on = true;
        const char* o = getenv("DOTHEROES_PPFX_TARGET_WATCH_OUT");
        snprintf(s_out, sizeof s_out, "%s", o && *o ? o : "ppfx_target_watch.txt");
        atexit(report);
        // the guest's memory is set up once the runtime starts: wait for it,
        // then put this handler first, ahead of the runtime's own
        std::thread([] {
            while (!vm_base) Sleep(10);
            Sleep(2000);
            AddVectoredExceptionHandler(1, on_fault);
            close_all();
            // the self-test: a read of a closed page from this thread is counted
            volatile uint8_t probe = 0;
            for (int i = 0; i < s_nr && !probe; i++)
                for (uint64_t pg = s_ranges[i].lo; pg < s_ranges[i].hi && !s_ranges[i].exact; pg += 0x1000) {
                    MEMORY_BASIC_INFORMATION mi;
                    if (VirtualQuery(vm_base + pg, &mi, sizeof mi) && mi.State == MEM_COMMIT) {
                        probe = (uint8_t)(1 + vm_base[pg]);
                        break;
                    }
                }
            fprintf(stderr, "[ppfx-watch] %d ranges watched, %llu committed pages closed, "
                            "self-test %s (%d accesses counted)\n", s_nr.load(), (unsigned long long)s_closed.load(),
                    probe ? "read a closed page" : "found no committed page", s_nhits.load());
            watcher();
        }).detach();
    }
#endif
}

// A main-memory range added at run time (the watch must have been started
// with DOTHEROES_PPFX_TARGET_WATCH or DOTHEROES_PPFX_EA_WATCH). Counted
// exactly: other accesses to its pages pass.
extern "C" void ppfx_watch_add_ea(uint32_t ea, uint32_t n)
{
#ifdef _WIN32
    if (!s_on || !n) return;
    while (s_lock.test_and_set(std::memory_order_acquire)) {}
    const int k = s_nr.load(std::memory_order_relaxed);
    bool have = false;
    for (int i = 0; i < k; i++) have |= s_ranges[i].lo == ea && s_ranges[i].hi == (uint64_t)ea + n;
    if (!have && k < 64) {
        s_ranges[k] = { ea, (uint64_t)ea + n, true };
        s_nr.store(k + 1, std::memory_order_release);
    }
    s_lock.clear(std::memory_order_release);
    if (!have) fprintf(stderr, "[ppfx-watch] main memory %08X-%08X watched\n", ea, ea + n);
#else
    (void)ea; (void)n;
#endif
}
