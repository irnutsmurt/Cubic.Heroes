/* The game's log: levels, the session's record, tagged events, the size cap (game_log.h). */
#include "game_log.h"
#include "ps3emu/guest_pause.h"   /* M10.2: time the game is held is no frame spike */

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#else
#include <signal.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <cpuid.h>
#endif

extern "C" unsigned cellGcm_flip_request_count(void);   /* the runtime: the guest's flips so far */

namespace {

std::atomic<int> g_active{0};
std::string g_path;
std::mutex g_settings_mu;
std::vector<std::pair<std::string, std::string>> g_settings;

int parse_level(const char* s)
{
    if (!s || !*s) return DH_LOG_INFO;
    if (*s >= '0' && *s <= '4' && !s[1]) return *s - '0';
    static const char* names[] = { "error", "warning", "info", "debug", "trace" };
    for (int i = 0; i < 5; i++)
        if (!strncmp(s, names[i], 4)) return i;   /* "warn" too */
    return DH_LOG_INFO;
}

const char* level_word(int l)
{
    return l == DH_LOG_ERROR ? "error: " : l == DH_LOG_WARNING ? "warning: " : l >= DH_LOG_DEBUG ? "debug: " : "";
}

double now_ms()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

/* The session's record: when, what build, where it runs, with what settings */
void write_record()
{
    char when[64] = "";
    const time_t t = time(nullptr);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &lt);
    const char* build = getenv("DOTHEROES_BUILD_ID");
    static const char* lv[] = { "error", "warning", "info", "debug", "trace" };
    fprintf(stderr, "[session] started %s; build %s; log level %s\n", when, build && *build ? build : "(not installed)",
            lv[dh_log_level()]);

    char cpu[64] = "";
    unsigned regs[12] = {};
#ifdef _WIN32
    int r[4];
    __cpuid(r, 0x80000000);
    if ((unsigned)r[0] >= 0x80000004u)
        for (int i = 0; i < 3; i++) __cpuid((int*)&regs[i * 4], 0x80000002 + i);
#else
    unsigned a, b, c, d;
    if (__get_cpuid(0x80000000, &a, &b, &c, &d) && a >= 0x80000004u)
        for (int i = 0; i < 3; i++) __get_cpuid(0x80000002 + i, &regs[i * 4], &regs[i * 4 + 1], &regs[i * 4 + 2], &regs[i * 4 + 3]);
#endif
    memcpy(cpu, regs, sizeof regs);
    cpu[48] = 0;
    const char* c0 = cpu;
    while (*c0 == ' ') c0++;
    for (size_t n = strlen(cpu); n && cpu[n - 1] == ' '; n--) cpu[n - 1] = 0;
    const unsigned threads = std::thread::hardware_concurrency();
#ifdef _WIN32
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    OSVERSIONINFOW os = { sizeof os };
    if (HMODULE nt = GetModuleHandleA("ntdll.dll"))
        if (auto f = (RtlGetVersionFn)(void*)GetProcAddress(nt, "RtlGetVersion")) f(&os);
    MEMORYSTATUSEX ms = { sizeof ms };
    GlobalMemoryStatusEx(&ms);
    fprintf(stderr, "[session] Windows %lu.%lu build %lu; CPU %s, %u threads; memory %.1f GB (%.1f GB free)\n",
            os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, *c0 ? c0 : "(unknown)", threads,
            ms.ullTotalPhys / 1073741824.0, ms.ullAvailPhys / 1073741824.0);
#else
    struct utsname u;
    uname(&u);
    const double total = (double)sysconf(_SC_PHYS_PAGES) * (double)sysconf(_SC_PAGESIZE);
    const double avail = (double)sysconf(_SC_AVPHYS_PAGES) * (double)sysconf(_SC_PAGESIZE);
    fprintf(stderr, "[session] %s %s; CPU %s, %u threads; memory %.1f GB (%.1f GB free)\n", u.sysname, u.release,
            *c0 ? c0 : "(unknown)", threads, total / 1073741824.0, avail / 1073741824.0);
#endif
    fprintf(stderr, "[session] the GPU and its driver: the [RSX vulkan] and [ppfx] lines below\n");
    std::lock_guard<std::mutex> g(g_settings_mu);
    if (g_settings.empty()) fprintf(stderr, "[settings] none set: the defaults\n");
    for (auto& s : g_settings) fprintf(stderr, "[settings] %s=%s\n", s.first.c_str(), s.second.c_str());
}

/* ---- the crash record ---- */

void flush_both()
{
    fflush(stdout);
    fflush(stderr);
}

std::terminate_handler g_prev_terminate;

void on_terminate()
{
    const char* what = "(not a standard exception)";
    try {
        if (auto e = std::current_exception()) std::rethrow_exception(e);
        what = "(no exception: std::terminate called)";
    } catch (const std::exception& e) {
        fprintf(stderr, "[crash] the game stopped: an unexpected exception: %s\n", e.what());
        what = nullptr;
    } catch (...) {
    }
    if (what) fprintf(stderr, "[crash] the game stopped: an unexpected exception %s\n", what);
    flush_both();
    if (g_prev_terminate) g_prev_terminate();
    abort();
}

#ifdef _WIN32
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter;

const char* exception_name(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case 0xC0000409: return "stack buffer overrun / fast fail";
    case 0xE06D7363: return "C++ exception";
    default: return "exception";
    }
}

LONG WINAPI on_crash(EXCEPTION_POINTERS* ep)
{
    static std::atomic<int> once{0};
    if (!once.exchange(1) && ep && ep->ExceptionRecord) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        const void* at = er->ExceptionAddress;
        char mod[MAX_PATH] = "?";
        uintptr_t off = (uintptr_t)at;
        HMODULE h = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)at, &h) && h) {
            GetModuleFileNameA(h, mod, sizeof mod);
            off -= (uintptr_t)h;
        }
        const char* base = strrchr(mod, '\\');
        base = base ? base + 1 : mod;
        fprintf(stderr, "[crash] the game stopped: %s (code %08lX) at %s+0x%llX, thread %lu\n",
                exception_name(er->ExceptionCode), er->ExceptionCode, base, (unsigned long long)off,
                GetCurrentThreadId());
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
            fprintf(stderr, "[crash] %s address %016llX\n",
                    er->ExceptionInformation[0] == 0 ? "reading" : er->ExceptionInformation[0] == 1 ? "writing"
                                                                                                     : "executing",
                    (unsigned long long)er->ExceptionInformation[1]);
        flush_both();
    }
    return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}
#endif

/* ---- the watch: frame-time spikes, the size cap ---- */

struct Spikes {
    unsigned frames = 0, over50 = 0, over100 = 0, written = 0;
    double worst = 0, line_at = -1e9;
};
Spikes g_sp;
std::mutex g_sp_mu;
double g_started_ms;
#ifdef _WIN32
HANDLE g_watch_thread;   /* its CPU time, in the session's summary: what the watch costs */
#endif

void spike_summary()
{
    std::lock_guard<std::mutex> g(g_sp_mu);
    if (!g_active.load() || !g_sp.frames) return;
    dh_log(DH_LOG_INFO, "frame", "the session: %u frames; %u over 50 ms, %u over 100 ms; the longest %.1f ms",
           g_sp.frames, g_sp.over50, g_sp.over100, g_sp.worst);
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (g_watch_thread && GetThreadTimes(g_watch_thread, &c, &e, &k, &u)) {
        const double cpu = ((double)(((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                            (double)(((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 1e4;
        dh_log(DH_LOG_INFO, "log", "the log's watch used %.0f ms of CPU time in %.0f s", cpu,
               (now_ms() - g_started_ms) / 1000.0);
    }
#endif
}

void watch(std::string log, double cap_mb)
{
#ifdef _WIN32
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_watch_thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
#endif
    unsigned last = cellGcm_flip_request_count();
    double last_at = now_ms(), size_at = last_at;
    uint64_t held_at = ps3_guest_paused_ns();
    bool capped = cap_mb <= 0 || log.empty();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        const double t = now_ms();
        const unsigned n = cellGcm_flip_request_count();
        if (n != last) {
            const uint64_t held = ps3_guest_paused_ns();   /* the time the game was held meanwhile: not a frame's */
            const double dt = t - last_at - (double)(held - held_at) / 1e6;
            held_at = held;
            std::lock_guard<std::mutex> g(g_sp_mu);
            /* the first flip's wait is the start-up, not a spike */
            if (last) {
                g_sp.frames += n - last;
                if (dt > g_sp.worst) g_sp.worst = dt;
                if (dt > 50) {
                    g_sp.over50++;
                    if (dt > 100) g_sp.over100++;
                    /* a line for a spike over 100 ms, at most one a second (the rest counted); Debug: every one
                     * over 50 ms */
                    if (dt > 100 && t - g_sp.line_at >= 1000) {
                        g_sp.line_at = t;
                        dh_log(DH_LOG_WARNING, "frame", "a %.0f ms frame (frame %u; %u over 100 ms so far)", dt, n,
                               g_sp.over100);
                    } else {
                        dh_log(DH_LOG_DEBUG, "frame", "a %.0f ms frame (frame %u)", dt, n);
                    }
                }
            }
            last = n;
            last_at = t;
        }
        if (!capped && t - size_at >= 2000) {
            size_at = t;
            std::error_code ec;
            const auto size = std::filesystem::file_size(log, ec);
            if (!ec && (double)size > cap_mb * 1048576.0) {
                capped = true;
                spike_summary();
                fprintf(stderr, "[log] this log reached its limit (%g MB): the rest of the session is not kept\n",
                        cap_mb);
                flush_both();
#ifdef _WIN32
                freopen("NUL", "w", stdout);
                freopen("NUL", "w", stderr);
#else
                freopen("/dev/null", "w", stdout);
                freopen("/dev/null", "w", stderr);
#endif
            }
        }
    }
}

}  // namespace

extern "C" int dh_log_level(void)
{
    static const int l = parse_level(getenv("DOTHEROES_LOG_LEVEL"));
    return l;
}

extern "C" void dh_log(int level, const char* tag, const char* fmt, ...)
{
    if (level > dh_log_level()) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[%s] %s%s\n", tag, level_word(level), msg);
}

extern "C" int dh_log_session_active(void) { return g_active.load(); }

extern "C" void dh_log_note_setting(const char* name, const char* value)
{
    if (!name || !*name) return;
    std::string n = name;
    for (const char* secret : { "KEY", "TOKEN", "PASS", "SECRET" })
        if (n.find(secret) != std::string::npos) return;   /* never in a log */
    std::lock_guard<std::mutex> g(g_settings_mu);
    for (auto& s : g_settings)
        if (s.first == n) return;   /* the first file read wins, as the settings do */
    g_settings.emplace_back(n, value ? value : "");
}

extern "C" void dh_log_session_start(const char* log_path)
{
    if (g_active.exchange(1)) return;
    g_path = log_path ? log_path : "";
    write_record();
    g_prev_terminate = std::set_terminate(on_terminate);
#ifdef _WIN32
    g_prev_filter = SetUnhandledExceptionFilter(on_crash);
#endif
    const char* e = getenv("DOTHEROES_LOG_CAP_MB");
    const double mb = e && *e ? atof(e) : 64.0;
    g_started_ms = now_ms();
    std::thread(watch, g_path, mb).detach();
    atexit(spike_summary);
    /* DOTHEROES_CRASH_TEST=<seconds>: a deliberate crash, for testing the crash record and the export after it */
    if (const char* c = getenv("DOTHEROES_CRASH_TEST"))
        if (atoi(c) > 0)
            std::thread([s = atoi(c)] {
                std::this_thread::sleep_for(std::chrono::seconds(s));
                dh_log(DH_LOG_WARNING, "crash", "DOTHEROES_CRASH_TEST: crashing now, on purpose");
                *(volatile int*)(uintptr_t)8 = 1;
            }).detach();
}
