// The game's water task run natively (water_task.h; docs/SPU_NON_AUDIO.md section 3): the host around the
// task's own code, which is generated on this machine from the player's lift (water_gen,
// port/native/spu_native.h). Checked offline against the lifted task (tests/native/water_task_check.cpp) and
// in the game (DOTHEROES_NATIVE_WATER=check).
#include "water_task.h"
#ifndef NOMINMAX
#define NOMINMAX   // ps3emu/thread_name.h brings windows.h: no min/max macros
#endif
#include "ps3emu/thread_name.h"
#include "ps3emu/census.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "spu_helpers.h"   // spu_pref_u32: a register with its preferred slot set

namespace water_task {
namespace {

// Where the task's main enters its two functions (tools/spu_native/water.py): the step's heights, then its
// normal map, each with r3 = the local-store address of the step's 24 bytes, which main keeps in its frame
// (r1 + 0x30; main's r1 is the entry's 0x3FFD0 less its 0xE0-byte frame). The links are main's own.
constexpr uint32_t kStepHeights = 0x3098, kStepNormals = 0x35E0;
constexpr uint32_t kLinkHeights = 0x3C80, kLinkNormals = 0x3C88;
constexpr uint32_t kMainStack = 0x3FFD0 - 0xE0, kParamLsa = kMainStack + 0x30;

inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// The task's block count: (W < 0 ? W + 7 : W) >> 3, C's division.
inline int32_t blocks(int32_t width) { return width / 8; }

void guest_get(void* user, void* dst, uint32_t ea, uint32_t bytes) { memcpy(dst, (uint8_t*)user + ea, bytes); }
void guest_put(void* user, uint32_t ea, const void* src, uint32_t bytes) { memcpy((uint8_t*)user + ea, src, bytes); }

bool call(Local& l, spu_native::Dma& d, uint32_t entry, uint32_t link)
{
    memset(l.c.gpr, 0, sizeof l.c.gpr);
    l.c.gpr[0] = spu_pref_u32(link);
    l.c.gpr[1] = spu_pref_u32(kMainStack);
    l.c.gpr[3] = spu_pref_u32(kParamLsa);
    spu_native::Outcome o;
    const spu_native::Result r = water_gen::call(l.c.ls, l.c.gpr, entry, d.host(), &o);
    if (r == spu_native::Result::handled && !d.trouble && o.ret_pc == link) return true;
    l.why = d.trouble ? d.trouble : r == spu_native::Result::halted ? "the task halted" : spu_native::result_name(r);
    return false;
}

}  // namespace

Params parse_params(const uint8_t b[kParamBytes])
{
    Params p;
    p.heights = be32(b + 0x00);
    p.texture = be32(b + 0x04);
    p.width = (int32_t)be32(b + 0x08);
    p.index = be32(b + 0x0C);
    p.damping = be32(b + 0x10);
    p.scale = be32(b + 0x14);
    return p;
}

Io guest_io(uint8_t* base) { return Io{ base, guest_get, guest_put }; }

const char* unsupported_reason(const Params& p, uint32_t param_ea)
{
    if (param_ea & 15) return "parameter block not 16-byte aligned (the task halts)";
    if (blocks(p.width) <= 0) return nullptr;   // the task does nothing
    const uint32_t W = (uint32_t)p.width;
    if (W & 3) return "width not a multiple of 4 (the task halts)";
    if (W > 408) return "width above 408";
    if (p.index > 1) return "layer index other than 0 or 1";
    if (p.heights & 15) return "heights not 16-byte aligned (the task halts)";
    if (p.texture & 15) return "texture not 16-byte aligned (the task halts)";
    return nullptr;
}

Status run_frame(uint32_t param_ea, const Io& io, Local& ls, Params* used)
{
    ls.why = nullptr;
    if (param_ea & 15) { ls.why = "parameter block not 16-byte aligned (the task halts)"; return Status::unsupported; }
    uint8_t b[kParamBytes];
    io.get(io.user, b, param_ea, kParamBytes);   // the task's GET of the 24 bytes, into main's frame
    const Params p = parse_params(b);
    if (used) *used = p;
    if (!ls.loaded) { water_gen::load(ls.c.ls); ls.loaded = true; }
    memcpy(ls.c.ls + kParamLsa, b, kParamBytes);
    spu_native::Dma d;
    d.user = io.user;
    d.get = io.get;
    d.put = io.put;
    d.ls = ls.c.ls;
    if (!call(ls, d, kStepHeights, kLinkHeights) || !call(ls, d, kStepNormals, kLinkNormals)) return Status::unsupported;
    return blocks(p.width) <= 0 ? Status::nothing : Status::done;
}

}  // namespace water_task

// ---- the game's side ---------------------------------------------------------
#ifndef WATER_TASK_NO_GAME
extern "C" uint8_t* vm_base;
extern "C" int g_water_hook = 0;

namespace {
using namespace water_task;
using Clock = std::chrono::steady_clock;

enum Mode { kLifted, kAsync, kSync, kCheck };
const char* const kModeName[] = { "lifted", "native (worker thread)", "native (signalling thread)",
                                  "check (native beside lifted)" };

inline uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
inline uint32_t gbe32(uint32_t ea) { const uint8_t* p = vm_base + ea; return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// A step's inputs, taken at its signal (check mode, capture), and a sparse
// space over them for run_frame: the 24 bytes, both layers, the texture
// (scratch). Transfers outside them read guest memory (none in the game's
// steps: W = 128) and count as `outside`.
struct Snapshot {
    bool valid = false;
    uint32_t step = 0, param_ea = 0;
    uint8_t params[kParamBytes] = {};
    Params p;
    std::vector<uint8_t> heights, texture;
    unsigned outside = 0;
};
uint32_t layers_bytes(const Params& p) { return 8u * (uint32_t)p.width * (uint32_t)p.width; }
uint32_t texture_bytes(const Params& p) { return 32u * (uint32_t)p.width * (uint32_t)std::max(0, p.width / 8); }
uint8_t* snap_at(Snapshot& s, uint32_t ea, uint32_t bytes)
{
    if (ea >= s.param_ea && ea + bytes <= s.param_ea + kParamBytes) return s.params + (ea - s.param_ea);
    if (ea >= s.p.heights && ea + bytes <= s.p.heights + (uint32_t)s.heights.size()) return s.heights.data() + (ea - s.p.heights);
    if (ea >= s.p.texture && ea + bytes <= s.p.texture + (uint32_t)s.texture.size()) return s.texture.data() + (ea - s.p.texture);
    s.outside++;
    return nullptr;
}
void snap_get(void* u, void* dst, uint32_t ea, uint32_t n)
{
    const uint8_t* src = snap_at(*(Snapshot*)u, ea, n);
    memcpy(dst, src ? src : vm_base + ea, n);
}
void snap_put(void* u, uint32_t ea, const void* src, uint32_t n)
{
    if (uint8_t* d = snap_at(*(Snapshot*)u, ea, n)) memcpy(d, src, n);
}

struct Runner {
    Mode mode = kLifted;
    // the native worker (kAsync)
    std::mutex m;
    std::condition_variable wake, idle;
    bool started = false, pending = false, running = false;
    uint32_t param_ea = 0;
    Local* local = nullptr;
    // counts
    std::atomic<uint64_t> signals{0}, steps{0}, nothing{0}, unsupported{0}, coalesced{0}, shape{0}, ns{0}, max_ns{0};
    // native scheduling: the set-up's parameter block, and the set-ups and shutdowns
    bool native_setup = false;
    std::atomic<uint32_t> native_params{0};
    std::atomic<uint64_t> setups{0}, shutdowns{0}, signals_after_stop{0};
    bool live = false;   // between a native set-up and its shutdown
    // DOTHEROES_WATER_LOG=1: set-ups, shutdowns, gaps in the signals (a pause) and a count every 5 s
    bool log = false;
    Clock::time_point t0, log_last, sig_prev;
    const char* last_reason = nullptr;
    Clock::time_point first_signal, last_signal;   // the step rate (the game's period: TOC constants, not read yet)
    // check mode
    Snapshot prev;
    Local* check_local = nullptr;
    uint64_t checked = 0, busy = 0, equal = 0, heights_bad = 0, texture_bad = 0;
    unsigned lines_left = 16;
    // capture: DOTHEROES_WATER_CAPTURE=<first>,<count>:<prefix>
    uint32_t cap_first = 0, cap_count = 0;
    std::string cap_prefix;
    FILE* cap_log = nullptr;
    Snapshot cap_prev;   // the step whose results the next signal takes
};
Runner* g = nullptr;

void timed_run(uint32_t param_ea, Local& ls)
{
    const auto t0 = Clock::now();
    Params p;
    const Status st = run_frame(param_ea, guest_io(vm_base), ls, &p);
    const uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
    if (st == Status::done) {
        g->steps++;
        g->ns += ns;
        uint64_t m = g->max_ns.load();
        while (ns > m && !g->max_ns.compare_exchange_weak(m, ns)) {}
    } else if (st == Status::nothing) {
        g->nothing++;
    } else {
        g->unsupported++;
        g->last_reason = ls.why;
    }
}

void worker_main()
{
    ps3_thread_name("water native");   // its line in a per-thread CPU table (tools/thread_cpu.py)
    std::unique_lock<std::mutex> lk(g->m);
    for (;;) {
        g->wake.wait(lk, [] { return g->pending; });
        const uint32_t ea = g->param_ea;
        g->pending = false;
        g->running = true;
        lk.unlock();
        timed_run(ea, *g->local);   // reads the 24 bytes now, as the task does after waking
        lk.lock();
        g->running = false;
        if (!g->pending) g->idle.notify_all();
    }
}

void wait_idle()
{
    std::unique_lock<std::mutex> lk(g->m);
    g->idle.wait(lk, [] { return !g->pending && !g->running; });
}

// The lifted task is back in its wait: its slot registered in the flag
// (spuTaskUsedWaitSlots, +0x08; the water's flag has one waiter), not yet
// handed events (+0x02), and no unconsumed bit 0 (+0x00).
bool task_waiting(uint32_t flag)
{
    const uint8_t* f = vm_base + flag;
    const uint16_t used = be16(f + 0x08), pend = be16(f + 0x02), events = be16(f + 0x00);
    return used && !(pend & used) && !(events & 1);
}

bool take_snapshot(Snapshot& s, uint32_t step, uint32_t param_ea)
{
    s.valid = false;
    s.step = step;
    s.param_ea = param_ea;
    s.outside = 0;
    memcpy(s.params, vm_base + param_ea, kParamBytes);
    s.p = parse_params(s.params);
    if (const char* why = unsupported_reason(s.p, param_ea)) {
        static int said = 0;
        if (said++ < 4) fprintf(stderr, "[water-check] step %u not snapshotted: %s (heights %08X texture %08X width %d index %d)\n",
                                step, why, s.p.heights, s.p.texture, (int)s.p.width, (int)s.p.index);
        return false;
    }
    if (blocks(s.p.width) <= 0) return false;
    s.heights.assign(vm_base + s.p.heights, vm_base + s.p.heights + layers_bytes(s.p));
    s.texture.assign(vm_base + s.p.texture, vm_base + s.p.texture + texture_bytes(s.p));
    s.valid = true;
    return true;
}

// One file of a snapshot's ranges, the solver snapshots' layout (u32 ea, u32
// bytes, the bytes; host order): what tests/native/water_task_check.cpp loads.
void write_space(const std::string& path, const Snapshot& s, const uint8_t* heights, const uint8_t* texture)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    auto range = [f](uint32_t ea, const uint8_t* b, uint32_t n) { fwrite(&ea, 4, 1, f); fwrite(&n, 4, 1, f); fwrite(b, 1, n, f); };
    range(s.param_ea, s.params, kParamBytes);
    range(s.p.heights, heights, layers_bytes(s.p));
    range(s.p.texture, texture, texture_bytes(s.p));
    fclose(f);
}

// Check mode: the previous step's native result from its inputs, against
// what the lifted task left (the layer it wrote, the texture).
void check_previous(uint32_t flag)
{
    Snapshot& s = g->prev;
    if (!s.valid) return;
    s.valid = false;
    if (!task_waiting(flag)) { g->busy++; return; }   // still running, or the signal not yet taken: no result to compare
    Snapshot n = s;                                    // the native step runs on a copy: its puts land in it
    const Io io{ &n, snap_get, snap_put };
    if (run_frame(n.param_ea, io, *g->check_local) != Status::done) return;
    g->checked++;
    const uint32_t W = (uint32_t)n.p.width, layer = 4u * W * W, rows = 8u * (uint32_t)(n.p.width / 8);
    const uint8_t* want_h = n.heights.data() + layer * n.p.index;
    const uint8_t* got_h = vm_base + n.p.heights + layer * n.p.index;
    const uint8_t* want_t = n.texture.data();
    const uint8_t* got_t = vm_base + n.p.texture;
    unsigned dh = 0, dt = 0;
    uint32_t first_h = ~0u, first_t = ~0u;
    for (uint32_t k = 0; k < rows * W; k++) {
        if (memcmp(want_h + 4 * k, got_h + 4 * k, 4)) { if (!dh++) first_h = k; }
        if (memcmp(want_t + 4 * k, got_t + 4 * k, 4)) { if (!dt++) first_t = k; }
    }
    if (!dh && !dt) { g->equal++; return; }
    g->heights_bad += dh != 0;
    g->texture_bad += dt != 0;
    if (g->lines_left) {
        g->lines_left--;
        fprintf(stderr, "[water-check] step %u (index %u): %u height(s) differ", n.step, n.p.index, dh);
        if (dh) fprintf(stderr, " (first row %u x %u: native %08X lifted %08X)", first_h / W, first_h % W,
                        be32(want_h + 4 * first_h), be32(got_h + 4 * first_h));
        fprintf(stderr, ", %u pixel(s) differ", dt);
        if (dt) fprintf(stderr, " (first row %u x %u: native %08X lifted %08X)", first_t / W, first_t % W,
                        be32(want_t + 4 * first_t), be32(got_t + 4 * first_t));
        fprintf(stderr, "%s\n", n.outside ? "; transfers outside the snapshot" : "");
        if (!g->cap_prefix.empty()) {   // the pair, for tests/native/water_task_check.cpp
            char name[64];
            snprintf(name, sizeof name, "wbad%u_in.bin", n.step);
            write_space(g->cap_prefix + name, s, s.heights.data(), s.texture.data());
            snprintf(name, sizeof name, "wbad%u_out.bin", n.step);
            write_space(g->cap_prefix + name, s, vm_base + n.p.heights, vm_base + n.p.texture);
        }
    }
}

// Capture: a step's inputs at its signal, its results at the next signal
// (when the lifted task is back in its wait), and a line each in water.txt.
void capture(uint32_t step, uint32_t param_ea, uint32_t flag)
{
    char name[64];
    Snapshot& c = g->cap_prev;
    if (c.valid) {
        c.valid = false;
        const bool waiting = task_waiting(flag);
        if (waiting) {
            snprintf(name, sizeof name, "w%u_out.bin", c.step);
            write_space(g->cap_prefix + name, c, vm_base + c.p.heights, vm_base + c.p.texture);
        }
        if (g->cap_log) fprintf(g->cap_log, "out %u %s\n", c.step, waiting ? "taken" : "not taken: the task was not back in its wait");
    }
    if (step < g->cap_first || step >= g->cap_first + g->cap_count) return;
    const uint8_t* f = vm_base + flag;
    const bool ok = take_snapshot(c, step, param_ea);
    if (g->cap_log)
        fprintf(g->cap_log, "in %u param %08X flag %08X events %04X pend %04X used %04X heights %08X texture %08X "
                "width %d index %u damping %08X scale %08X mode %s%s\n", step, param_ea, flag, be16(f), be16(f + 2),
                be16(f + 8), c.p.heights, c.p.texture, c.p.width, c.p.index, c.p.damping, c.p.scale, kModeName[g->mode],
                ok ? "" : " (not taken: unsupported)");
    if (!ok) return;
    snprintf(name, sizeof name, "w%u_in.bin", step);
    write_space(g->cap_prefix + name, c, c.heights.data(), c.texture.data());
    if (g->cap_log) fflush(g->cap_log);
}

double since_start() { return std::chrono::duration<double>(Clock::now() - g->t0).count(); }

// A signal's time for the log: a gap of over a second (the game paused, or no water) said once it ends.
void log_signal()
{
    const Clock::time_point now = Clock::now();
    if (g->sig_prev != Clock::time_point{}) {
        const double gap = std::chrono::duration<double>(now - g->sig_prev).count();
        if (gap > 1.0)
            fprintf(stderr, "[water-log] t=%.2f signals again after %.2f s without (signals %llu, steps %llu)\n",
                    since_start(), gap, (unsigned long long)g->signals.load(), (unsigned long long)g->steps.load());
    }
    g->sig_prev = now;
    if (std::chrono::duration<double>(now - g->log_last).count() >= 5.0) {
        g->log_last = now;
        fprintf(stderr, "[water-log] t=%.2f signals %llu, steps %llu\n", since_start(),
                (unsigned long long)g->signals.load(), (unsigned long long)g->steps.load());
    }
}

void report()
{
    if (!g) return;
    static std::atomic<int> once{ 0 };   // the test stop's call (dev_state.cpp) and atexit's: once
    if (once.exchange(1)) return;
    const uint64_t n = g->steps.load();
    fprintf(stderr, "[water] %s: %llu signal(s); %llu native step(s), mean %.1f us, max %.1f us; %llu coalesced; "
            "%llu with nothing to do; %llu unsupported%s%s; %llu block(s) not the game's shape\n",
            kModeName[g->mode], (unsigned long long)g->signals.load(), (unsigned long long)n,
            n ? g->ns.load() / 1e3 / (double)n : 0.0, g->max_ns.load() / 1e3, (unsigned long long)g->coalesced.load(),
            (unsigned long long)g->nothing.load(), (unsigned long long)g->unsupported.load(),
            g->last_reason ? ": " : "", g->last_reason ? g->last_reason : "", (unsigned long long)g->shape.load());
    if (g->native_setup)
        fprintf(stderr, "[water] native scheduling: %llu set-up(s), %llu shutdown(s), no SPURS instance or task; "
                "%llu signal(s) with no water set up\n", (unsigned long long)g->setups.load(),
                (unsigned long long)g->shutdowns.load(), (unsigned long long)g->signals_after_stop.load());
    const uint64_t s = g->signals.load();
    const double span = std::chrono::duration<double>(g->last_signal - g->first_signal).count();
    if (s > 1 && span > 0)
        fprintf(stderr, "[water] steps signalled over %.1f s: %.2f a second\n", span, (double)(s - 1) / span);
    if (g->mode == kCheck)
        fprintf(stderr, "[water-check] %llu step(s) compared: %llu equal, %llu with heights differing, %llu with the "
                "texture differing; %llu not compared (the lifted task not back in its wait at the next signal)\n",
                (unsigned long long)g->checked, (unsigned long long)g->equal, (unsigned long long)g->heights_bad,
                (unsigned long long)g->texture_bad, (unsigned long long)g->busy);
}

}  // namespace

// The report from the test stop (dev_state.cpp: atexit does not run past TerminateProcess).
extern "C" void water_task_report(void) { report(); }

extern "C" void water_task_init(void)
{
    // Native on a worker thread by default (the task's code generated from the player's lift: equal to the
    // lifted task's by construction, checked offline and in the game); DOTHEROES_NATIVE_WATER=0 keeps the SPU task.
    const char* e = getenv("DOTHEROES_NATIVE_WATER");
    Mode mode = kAsync;
    if (!water_gen::has_entry(kStepHeights) || !water_gen::has_entry(kStepNormals)) {
        // a build without the task's generated code (no lift of its image: spu_native_absent.cpp)
        if (!(e && !strcmp(e, "0"))) fprintf(stderr, "[water] no native code generated for the task in this build: the lifted task\n");
        e = "0";
    }
    if (e && !strcmp(e, "0")) mode = kLifted;
    else if (e && !strcmp(e, "1")) mode = kAsync;
    else if (e && !strcmp(e, "sync")) mode = kSync;
    else if (e && !strcmp(e, "check")) mode = kCheck;
    const char* cap = getenv("DOTHEROES_WATER_CAPTURE");
    if (mode == kLifted && !(cap && *cap)) return;
    g = new Runner;   // never destroyed: the worker thread is detached
    g->mode = mode;
    // The set-up and shutdown native too (water_task.h), unless the SPU task is wanted (lifted, check) or
    // the steps are captured (the capture reads the task's event flag); DOTHEROES_WATER_SPURS=1 keeps them.
    const char* lg = getenv("DOTHEROES_WATER_LOG");
    g->log = lg && !strcmp(lg, "1");
    g->t0 = g->log_last = Clock::now();
    const char* sp = getenv("DOTHEROES_WATER_SPURS");
    g->native_setup = (mode == kAsync || mode == kSync) && !(cap && *cap) && !(sp && !strcmp(sp, "1"));
    if (mode == kAsync || mode == kSync) g->local = new Local;
    if (mode == kCheck) g->check_local = new Local;
    if (cap && *cap) {
        const char* colon = strchr(cap, ':');
        unsigned first = 0, count = 0;
        if (colon && sscanf(cap, "%u,%u", &first, &count) == 2 && count) {
            g->cap_first = first;
            g->cap_count = count;
            g->cap_prefix = colon + 1;
            g->cap_log = fopen((g->cap_prefix + "water.txt").c_str(), "w");
        } else {
            fprintf(stderr, "[water] DOTHEROES_WATER_CAPTURE=<first step>,<count>:<path prefix> (ignored: %s)\n", cap);
        }
    }
    g_water_hook = 1;
    atexit(report);
    fprintf(stderr, "[water] the water task: %s%s%s\n", kModeName[mode],
            g->cap_count ? "; steps captured (DOTHEROES_WATER_CAPTURE)" : "",
            g->native_setup ? "; set-up and shutdown native (no SPURS)" : "; the game's SPURS set-up");
}

extern "C" void water_task_counts(WaterTaskCounts* out)
{
    *out = WaterTaskCounts{};
    if (!g) return;
    out->signals = g->signals.load();
    out->steps = g->steps.load();
    out->coalesced = g->coalesced.load();
    out->unsupported = g->unsupported.load();
    out->shape = g->shape.load();
    out->checked = g->checked;
    out->equal = g->equal;
    out->busy = g->busy;
    out->setups = g->setups.load();
    out->shutdowns = g->shutdowns.load();
}

extern "C" int water_task_native_setup(void) { return g && g->native_setup; }

extern "C" void water_task_native_started(uint32_t params)
{
    g->native_params = params;
    g->setups++;
    if (g->log)
        fprintf(stderr, "[water-log] t=%.2f set-up #%llu, native (parameters at %08X)\n", since_start(),
                (unsigned long long)g->setups.load(), params);
    std::lock_guard<std::mutex> lk(g->m);
    g->live = true;
}

extern "C" void water_task_native_stopped(void)
{
    {
        std::lock_guard<std::mutex> lk(g->m);
        g->live = false;   // signals from now on (none expected) do nothing
    }
    if (g->started) wait_idle();   // the step under way finishes before the game frees the heights and texture
    g->shutdowns++;
    if (g->log)
        fprintf(stderr, "[water-log] t=%.2f shutdown #%llu, native (signals %llu, steps %llu)\n", since_start(),
                (unsigned long long)g->shutdowns.load(), (unsigned long long)g->signals.load(),
                (unsigned long long)g->steps.load());
}

extern "C" void water_task_note(const char* what)
{
    if (g && g->log)
        fprintf(stderr, "[water-log] t=%.2f %s (signals %llu, steps %llu)\n", since_start(), what,
                (unsigned long long)g->signals.load(), (unsigned long long)g->steps.load());
}

extern "C" int water_task_signal(uint32_t block, uint32_t bits)
{
    if (!g) return 0;
    // The task's argument as the game builds it (func_003C7BA8): the 24
    // bytes' EA as a u64, then the flag. Anything else is not this task.
    if (block == kNativeBlock) {   // the native set-up's: always handled here (there is no flag to set)
        if (!(bits & 1)) return 1;
        std::lock_guard<std::mutex> lk(g->m);
        if (!g->live) { g->signals_after_stop++; return 1; }
        const uint32_t step = (uint32_t)++g->signals;
        g->last_signal = Clock::now();
        if (step == 1) g->first_signal = g->last_signal;
        if (g->log) log_signal();
        if (g->mode == kSync) {
            g->running = true;   // (the lock is held: the sync step runs here, on the signalling thread)
            timed_run(g->native_params, *g->local);
            g->running = false;
            return 1;
        }
        if (!g->started) { g->started = true; std::thread(worker_main).detach(); }
        if (g->pending) g->coalesced++;
        g->pending = true;
        g->param_ea = g->native_params;
        g->wake.notify_one();
        return 1;
    }
    const uint32_t arg = block + kBlockArgument, flag = block + kBlockFlag;
    const uint32_t param_ea = gbe32(arg + 4);
    if (gbe32(arg) != 0 || gbe32(arg + 8) != flag) {   // not the water's block: the lifted set (M6's census: a = 5)
        g->shape++;
        ps3_census(PS3_CENSUS_NATIVE_DECLINE, 5, block);
        return 0;
    }
    if (bits & 2) {   // the quit: to the lifted task, once nothing native touches the water's memory
        if (g->started) wait_idle();
        if (g->mode == kCheck) check_previous(flag);   // the last step's results, before the game frees them
        if (g->cap_count) capture(~0u, param_ea, flag);
        return 0;
    }
    if (!(bits & 1)) return 0;
    const uint32_t step = (uint32_t)++g->signals;
    g->last_signal = Clock::now();
    if (step == 1) g->first_signal = g->last_signal;
    if (g->log) log_signal();
    if (g->cap_count) capture(step, param_ea, flag);
    switch (g->mode) {
    case kLifted:
        return 0;
    case kCheck:
        check_previous(flag);
        take_snapshot(g->prev, step, param_ea);
        return 0;
    case kSync:
        timed_run(param_ea, *g->local);
        return 1;
    case kAsync: {
        std::lock_guard<std::mutex> lk(g->m);
        if (!g->started) { g->started = true; std::thread(worker_main).detach(); }
        if (g->pending) g->coalesced++;   // the flag's bits: at most one run owed, the parameters read when it starts
        g->pending = true;
        g->param_ea = param_ea;
        g->wake.notify_one();
        return 1;
    }
    }
    return 0;
}
#endif  // WATER_TASK_NO_GAME
