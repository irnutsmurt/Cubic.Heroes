// The waver in the game (waver_game.h): the native frames' worker, and the
// check mode beside the SPU task.
#include "waver_game.h"
#include "waver_task.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

extern "C" uint8_t* vm_base;
extern "C" int g_waver_hook = 0;

namespace {
using namespace waver_task;
using Clock = std::chrono::steady_clock;

enum Mode { kLifted, kNative, kCheck };

inline uint32_t gbe32(uint32_t ea) { const uint8_t* p = vm_base + ea; return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// A lifted frame's inputs (check mode): the block as it was at the creation,
// and the field and texture the frame writes, both written whole by it.
struct Snap {
    bool valid = false;
    uint64_t n = 0;
    uint32_t param_ea = 0;
    Params p;
    std::vector<uint8_t> params, field, tex;
    unsigned outside = 0;
};

struct Runner {
    Mode mode = kLifted;
    std::mutex m;
    std::condition_variable wake, idle;
    bool started = false, pending = false, running = false;
    uint32_t param_ea = 0;
    Local* local = nullptr;
    std::atomic<uint64_t> setups{0}, stops{0}, frames{0}, unsupported{0}, ns{0}, max_ns{0}, waits{0}, wait_ns{0};
    const char* last_reason = nullptr;
    // check mode (the PPU thread's)
    Snap prev;
    Local* check_local = nullptr;
    uint64_t created = 0, checked = 0, equal = 0, field_bad = 0, tex_bad = 0, tex_other = 0, not_run = 0;
    unsigned lines = 12;
};
Runner* g;

void run_one(uint32_t ea)
{
    const auto t0 = Clock::now();
    Params p;
    const Status st = run_frame(ea, guest_io(vm_base), *g->local, &p);
    const uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
    if (st == Status::done) {
        g->frames++;
        g->ns += ns;
        uint64_t m = g->max_ns.load();
        while (ns > m && !g->max_ns.compare_exchange_weak(m, ns)) {}
    } else {
        g->unsupported++;
        g->last_reason = unsupported_reason(p, ea);
    }
}

void worker_main()
{
    std::unique_lock<std::mutex> lk(g->m);
    for (;;) {
        g->wake.wait(lk, [] { return g->pending; });
        const uint32_t ea = g->param_ea;
        g->pending = false;
        g->running = true;
        lk.unlock();
        run_one(ea);   // reads the block now, as the task does once started
        lk.lock();
        g->running = false;
        g->idle.notify_all();
    }
}

void wait_idle_locked(std::unique_lock<std::mutex>& lk)
{
    g->idle.wait(lk, [] { return !g->pending && !g->running; });
}

uint8_t* snap_at(Snap& s, uint32_t ea, uint32_t n)
{
    if (ea >= s.param_ea && ea + n <= s.param_ea + kParamBytes) return s.params.data() + (ea - s.param_ea);
    if (ea >= s.p.field && ea + n <= s.p.field + kFieldBytes) return s.field.data() + (ea - s.p.field);
    const uint32_t t = s.p.texture_ea();
    if (ea >= t && ea + n <= t + kTextureBytes) return s.tex.data() + (ea - t);
    s.outside++;
    return nullptr;
}
void snap_get(void* u, void* dst, uint32_t ea, uint32_t n)
{
    const uint8_t* src = snap_at(*(Snap*)u, ea, n);
    memcpy(dst, src ? src : vm_base + ea, n);
}
void snap_put(void* u, uint32_t ea, const void* src, uint32_t n)
{
    if (uint8_t* d = snap_at(*(Snap*)u, ea, n)) memcpy(d, src, n);
}

// The previous lifted frame (waited for by the game by now): the native frame
// on its inputs, against what the SPU task left in the field and the texture.
void check_previous()
{
    Snap& s = g->prev;
    if (!s.valid) return;
    s.valid = false;
    Snap n = s;
    const Io io{ &n, snap_get, snap_put };
    if (run_frame(n.param_ea, io, *g->check_local) != Status::done) { g->not_run++; return; }
    g->checked++;
    const uint8_t* got_f = vm_base + n.p.field;
    const uint8_t* got_t = vm_base + n.p.texture_ea();
    unsigned df = 0, dt = 0;
    uint32_t first_f = ~0u, first_t = ~0u;
    for (uint32_t k = 0; k < kFieldBytes / 4; k++) {
        if (memcmp(n.field.data() + 4 * k, got_f + 4 * k, 4) && !df++) first_f = k;
        if (memcmp(n.tex.data() + 4 * k, got_t + 4 * k, 4) && !dt++) first_t = k;
    }
    if (!df && !dt) { g->equal++; return; }
    g->field_bad += df != 0;
    g->tex_bad += dt != 0;
    // the texture the SPU task chose itself, if it read the select word later than the copy took it
    if (dt && !df && !memcmp(n.tex.data(), vm_base + n.p.texture[(n.p.select & 1) ^ 1], kTextureBytes)) g->tex_other++;
    if (g->lines) {
        g->lines--;
        fprintf(stderr, "[waver-check] frame %llu (select %u): %u field value(s) differ", (unsigned long long)n.n,
                n.p.select, df);
        if (df) {
            const uint8_t* q = n.field.data() + 4 * first_f;
            fprintf(stderr, " (first row %u x %u: native %08X lifted %08X)", first_f / 256, first_f % 256,
                    (unsigned)(q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3]), gbe32(n.p.field + 4 * first_f));
        }
        fprintf(stderr, ", %u pixel(s) differ%s\n", dt, n.outside ? "; transfers outside the snapshot" : "");
    }
}

void report()
{
    if (!g) return;
    static std::atomic<int> once{0};
    if (once.exchange(1)) return;
    if (g->mode == kNative) {
        const uint64_t n = g->frames.load(), w = g->waits.load();
        fprintf(stderr, "[waver] native: %llu set-up(s), %llu teardown(s), %llu frame(s), mean %.2f ms, max %.2f ms on "
                "its thread; %llu unsupported%s%s; the game's waits: %llu, %.3f ms in all; no SPURS instance or task\n",
                (unsigned long long)g->setups.load(), (unsigned long long)g->stops.load(), (unsigned long long)n,
                n ? g->ns.load() / 1e6 / (double)n : 0.0, g->max_ns.load() / 1e6,
                (unsigned long long)g->unsupported.load(), g->last_reason ? ": " : "",
                g->last_reason ? g->last_reason : "", (unsigned long long)w, g->wait_ns.load() / 1e6);
    } else if (g->mode == kCheck) {
        fprintf(stderr, "[waver-check] %llu lifted frame(s) created, %llu compared: %llu equal, %llu with the field "
                "differing, %llu with the texture differing (%llu of them equal to the other texture: the select "
                "word read after the copy took it); %llu the native frame did not run\n",
                (unsigned long long)g->created, (unsigned long long)g->checked, (unsigned long long)g->equal,
                (unsigned long long)g->field_bad, (unsigned long long)g->tex_bad, (unsigned long long)g->tex_other,
                (unsigned long long)g->not_run);
    }
}

}  // namespace

extern "C" void waver_game_report(void) { report(); }

extern "C" void waver_game_init(void)
{
    const char* e = getenv("DOTHEROES_NATIVE_WAVER");
    Mode mode = kNative;   // checked offline bit-exact (tests/native/waver_task_check.cpp) and in the game (check mode)
    if (e && !strcmp(e, "0")) mode = kLifted;
    else if (e && !strcmp(e, "check")) mode = kCheck;
    if (mode == kLifted) return;
    g = new Runner;   // never destroyed: the worker thread is detached
    g->mode = mode;
    if (mode == kNative) g->local = new Local;
    if (mode == kCheck) g->check_local = new Local;
    g_waver_hook = 1;
    atexit(report);
    fprintf(stderr, "[waver] the waver (SPU image 2): %s\n", mode == kNative
            ? "native, set-up and frames (no SPURS)" : "check (the SPU task, the native frame beside it)");
}

extern "C" int waver_native(void) { return g && g->mode == kNative; }
extern "C" int waver_checking(void) { return g && g->mode == kCheck; }

extern "C" void waver_native_started(uint32_t w)
{
    std::lock_guard<std::mutex> lk(g->m);
    g->param_ea = w + 0x80;
    g->setups++;
    if (!g->started) { g->started = true; std::thread(worker_main).detach(); }
}

extern "C" void waver_native_create(void)
{
    std::unique_lock<std::mutex> lk(g->m);
    wait_idle_locked(lk);   // the game waits before each creation; this only keeps one frame at a time if it did not
    g->pending = true;
    g->wake.notify_one();
}

extern "C" void waver_native_wait(void)
{
    const auto t0 = Clock::now();
    std::unique_lock<std::mutex> lk(g->m);
    wait_idle_locked(lk);
    g->waits++;
    g->wait_ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
}

extern "C" void waver_native_stopped(void)
{
    {
        std::unique_lock<std::mutex> lk(g->m);
        wait_idle_locked(lk);
    }
    g->stops++;
}

extern "C" void waver_check_create(uint32_t w)
{
    check_previous();
    Snap& s = g->prev;
    s.n = ++g->created;
    s.param_ea = w + 0x80;
    s.outside = 0;
    s.params.assign(vm_base + s.param_ea, vm_base + s.param_ea + kParamBytes);
    s.p = parse_params(s.params.data());
    if (unsupported_reason(s.p, s.param_ea)) { s.valid = false; g->not_run++; return; }
    s.field.assign(vm_base + s.p.field, vm_base + s.p.field + kFieldBytes);
    s.tex.assign(vm_base + s.p.texture_ea(), vm_base + s.p.texture_ea() + kTextureBytes);
    s.valid = true;
}

extern "C" void waver_check_end(void) { check_previous(); }
