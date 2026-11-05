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
struct Range { uint32_t lo, hi; };
Range s_ranges[64];
int s_nr;
struct Hit { uint64_t rip; uint32_t page, tid; uint8_t kind; std::atomic<uint64_t> n; uint64_t first_ms, last_ms; };
uint64_t s_t0;
enum { kHits = 4096 };
Hit s_hits[kHits];
std::atomic<int> s_nhits{0};
std::atomic<uint64_t> s_lost{0};
std::atomic_flag s_lock = ATOMIC_FLAG_INIT;
char s_out[512];

bool watched(uintptr_t a, uint32_t* page)
{
    const uintptr_t base = (uintptr_t)vm_base + kLocalEa;
    if (a < base) return false;
    const uint64_t off = a - base;
    for (int i = 0; i < s_nr; i++)
        if (off >= s_ranges[i].lo && off < s_ranges[i].hi) { *page = (uint32_t)(off & ~0xFFFull); return true; }
    return false;
}

void count(uint64_t rip, uint32_t page, uint32_t tid, uint8_t kind)
{
    while (s_lock.test_and_set(std::memory_order_acquire)) {}
    const int n = s_nhits.load(std::memory_order_relaxed);
    int i = 0;
    for (; i < n; i++)
        if (s_hits[i].rip == rip && s_hits[i].page == page && s_hits[i].tid == tid && s_hits[i].kind == kind) break;
    if (i == n) {
        if (n == kHits) { s_lost++; s_lock.clear(std::memory_order_release); return; }
        s_hits[n].rip = rip; s_hits[n].page = page; s_hits[n].tid = tid; s_hits[n].kind = kind;
        s_hits[n].first_ms = GetTickCount64() - s_t0;
        s_nhits.store(n + 1, std::memory_order_relaxed);
    }
    s_hits[i].last_ms = GetTickCount64() - s_t0;
    s_hits[i].n++;
    s_lock.clear(std::memory_order_release);
}

LONG CALLBACK on_fault(EXCEPTION_POINTERS* e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t a = (uintptr_t)e->ExceptionRecord->ExceptionInformation[1];
    uint32_t page;
    if (!watched(a, &page)) return EXCEPTION_CONTINUE_SEARCH;
    count(e->ContextRecord->Rip, page, GetCurrentThreadId(), (uint8_t)e->ExceptionRecord->ExceptionInformation[0]);
    // A page never touched is only reserved: the runtime's own handler
    // commits guest pages on first access, so it is passed on to it. A
    // committed page was closed here and is opened again.
    void* p = (void*)((uintptr_t)vm_base + kLocalEa + page);
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return EXCEPTION_CONTINUE_SEARCH;
    DWORD old;
    VirtualProtect(p, 0x1000, PAGE_READWRITE, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}

std::atomic<uint64_t> s_closed{0}, s_fail{0};

// Every committed page of the ranges closed (protection applies to committed
// pages only; the others fault anyway, into the runtime's commit).
void close_all()
{
    uint64_t closed = 0;
    for (int i = 0; i < s_nr; i++)
        for (uint32_t pg = s_ranges[i].lo; pg < s_ranges[i].hi; pg += 0x1000) {
            void* p = vm_base + kLocalEa + pg;
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
               "%llu committed pages closed at the last pass, %llu protection failures\n", s_nr, n,
            (unsigned long long)s_lost.load(), (unsigned long long)s_closed.load(), (unsigned long long)s_fail.load());
    fprintf(f, "# count kind page(offset) thread [name] address module+offset symbol\n");
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
        fprintf(f, "%llu %s %08X %u [%s] %llX %s+0x%llX %s\n", (unsigned long long)s_hits[i].n.load(),
                s_hits[i].kind == 1 ? "write" : s_hits[i].kind == 8 ? "exec" : "read", s_hits[i].page, s_hits[i].tid,
                tname, (unsigned long long)s_hits[i].rip, mod, modoff, name);
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
        if (!e || !*e) return;
        s_t0 = GetTickCount64();
        for (const char* p = e; *p && s_nr < 64;) {
            char* q;
            const uint32_t lo = (uint32_t)strtoul(p, &q, 16);
            if (*q != '-') break;
            const uint32_t hi = (uint32_t)strtoul(q + 1, &q, 16);
            s_ranges[s_nr++] = { lo & ~0xFFFu, (hi + 0xFFFu) & ~0xFFFu };
            p = *q == ',' ? q + 1 : q;
        }
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
                for (uint32_t pg = s_ranges[i].lo; pg < s_ranges[i].hi; pg += 0x1000) {
                    MEMORY_BASIC_INFORMATION mi;
                    if (VirtualQuery(vm_base + kLocalEa + pg, &mi, sizeof mi) && mi.State == MEM_COMMIT) {
                        probe = (uint8_t)(1 + vm_base[kLocalEa + pg]);
                        break;
                    }
                }
            fprintf(stderr, "[ppfx-watch] %d ranges of RSX local memory watched, %llu committed pages closed, "
                            "self-test %s (%d accesses counted)\n", s_nr, (unsigned long long)s_closed.load(),
                    probe ? "read a closed page" : "found no committed page", s_nhits.load());
            watcher();
        }).detach();
    }
#endif
}
