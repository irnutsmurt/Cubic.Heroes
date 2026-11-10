/* R8's entry hooks (config/r8_hooks.json), the guest registers in and out:
 * the game's physics task issue and wait, whose work is
 * native/phys_direct.cpp's (docs/NATIVE_R8.md section 6.1), and its map
 * pack inflate, native/inflate_direct.cpp's (docs/SPU_NON_AUDIO.md section 2). */
#include "ppu_recomp.h"   /* ppu_context */
#include "native/inflate_direct.h"
#include "native/water_task.h"
#include "native/waver_game.h"
#include "native/phys_direct.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);   // the lift's tail calls (as ppfx_recorder.cpp)

/* func_00955CF0(r3 manager, r4 slot, r5..r8 the queue entry): 1 when issued
 * natively, r3 = 0 as the game's function returns on success. */
extern "C" int dotheroes_phys_issue(ppu_context* ctx)
{
    const uint32_t entry[4] = { (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6], (uint32_t)ctx->gpr[7], (uint32_t)ctx->gpr[8] };
    if (!phys_direct_issue((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], entry)) return 0;
    ctx->gpr[3] = 0;
    return 1;
}

/* func_00955BD4(r3 manager, r4 &cursor, r5..r8 where the entry's words go):
 * always done here while the hooks are on. */
extern "C" int dotheroes_phys_poll(ppu_context* ctx)
{
    const uint32_t out[4] = { (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6], (uint32_t)ctx->gpr[7], (uint32_t)ctx->gpr[8] };
    ctx->gpr[3] = (uint64_t)(int64_t)phys_direct_poll((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], out,
                                                      (uint32_t)ctx->thread_id);
    return 1;
}

// The water's per-step signal to its SPU task (config/r8_hooks.json, port/native/water_task.cpp):
// handled natively, r3 = CELL_OK as cellSpursEventFlagSet's; declined, the lifted call runs.
extern "C" int dotheroes_water_signal(ppu_context* ctx)
{
    if (!water_task_signal((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4] & 0xFFFFu)) return 0;
    ctx->gpr[3] = 0;
    return 1;
}

namespace {
inline uint32_t g_rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline void g_wr32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
inline void g_wr64(uint32_t ea, uint64_t v) { g_wr32(ea, (uint32_t)(v >> 32)); g_wr32(ea + 4, (uint32_t)v); }
// A lifted call as a call site makes it: its return address in lr, then the trampoline's tail calls.
inline void guest_call(ppu_context* ctx, void (*fn)(ppu_context*), uint32_t ret)
{
    ctx->lr = ret;
    fn(ctx);
    while (g_trampoline_fn) { void (*f)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; f(ctx); }
}
}  // namespace

/* func_003C7BA8(r3 the water), the water's set-up (water_task.h). Native
 * scheduling: the game's own stores and its two texture calls, in its frame
 * (stdu r1,-0xA0: func_003A4740's r4 points into it), and the markers in
 * place of the SPURS instance, taskset and block. r3 = 0 as func_004736E0's
 * CELL_OK; r28 and r29, which the game's saves and restores, are not used. */
extern "C" int dotheroes_water_setup(ppu_context* ctx)
{
    if (!water_task_native_setup()) {
        water_task_note("set-up, the game's (SPURS)");
        return 0;
    }
    const uint64_t sp = ctx->gpr[1], lr = ctx->lr;
    const uint32_t toc = (uint32_t)ctx->gpr[2], water = (uint32_t)ctx->gpr[3];
    const uint32_t G = g_rd32(toc - 0x3774), tex = (water + 0x1D0) & 0xFFFFFFFFu;
    const uint32_t frame = (uint32_t)sp - 0xA0;
    g_wr64(frame, sp);                        // stdu r1,-0xA0(r1)
    g_wr64(frame + 0xB0, lr);                 // std r0,0xB0(r1) (mflr r0)
    g_wr64(frame + 0x88, ctx->gpr[29]);
    g_wr64(frame + 0x80, ctx->gpr[28]);
    ctx->gpr[1] = frame;
    g_wr32(G + 0x88, 0x80);                   // the width
    g_wr32(G + 0x90, g_rd32(toc - 0x3768));   // the damping (lfs, stfs: the same bits)
    g_wr32(G + 0x8C, g_rd32(water + 0x1EC));  // the layer index
    g_wr32(G + 0x80, g_rd32(water + 0x1E8));  // the heights
    ctx->gpr[3] = tex;
    ctx->gpr[4] = (uint64_t)frame + 0x70;
    guest_call(ctx, func_003A4740, 0x003C7BF4);
    g_wr32(G + 0x84, (uint32_t)ctx->gpr[3]);  // the normal map's texture
    ctx->gpr[3] = tex;
    guest_call(ctx, func_003A46E8, 0x003C7C04);
    g_wr32(G + 0x9C, water_task::kNativeInstance);
    g_wr32(G + 0x4, water_task::kNativeTaskset);
    g_wr32(G + 0x98, water_task::kNativeBlock);
    water_task_native_started(G + 0x80);
    ctx->gpr[1] = sp;
    ctx->lr = lr;
    ctx->gpr[3] = 0;
    return 1;
}

/* func_003C7AF4(), the water's shutdown: native when the set-up was (the
 * markers); the worker finishes its step first, then +0x4 and +0x9C cleared
 * as the game's does. Declined for a real taskset or instance (the lifted
 * path: quit bit, exit code, taskset and instance torn down). */
extern "C" int dotheroes_water_shutdown(ppu_context* ctx)
{
    const uint32_t G = g_rd32((uint32_t)ctx->gpr[2] - 0x3774);
    const uint32_t taskset = g_rd32(G + 0x4), inst = g_rd32(G + 0x9C);
    const bool ours = taskset == water_task::kNativeTaskset || inst == water_task::kNativeInstance;
    if (!ours) {
        water_task_note(taskset ? "shutdown, the game's (SPURS)" : "shutdown with nothing set up");
        return 0;
    }
    if ((taskset && taskset != water_task::kNativeTaskset) || (inst && inst != water_task::kNativeInstance)) return 0;
    if (taskset) {
        water_task_native_stopped();
        g_wr32(G + 0x4, 0);
    }
    g_wr32(G + 0x9C, 0);
    ctx->gpr[3] = 0;
    return 1;
}

/* DOTHEROES_DEV_WAVER=<s>[,<s>...] (a test switch; docs/SPU_NON_AUDIO.md section 4): the waver (SPU image 2)
 * started at the hero's position at these seconds after the frame function's first call, as the game starts
 * it (func_0018B708 -> the TOC stub func_009842F8 -> func_003CEB48 with r3 = &{x, y, z}); the game then runs
 * it every frame (func_000810F8 -> func_00982788 -> func_003CEEE8: the previous frame's SPU task awaited, a
 * new one created) until its duration ends and it tears itself down. Needs DOTHEROES_DEV_STATE=1 (the
 * player's object). Through an entry hook on func_000810F8, the frame function (r3 its object, f1 the frame
 * time): the waver's call between the game's frames, the frame function's registers given back as they
 * were; and the frame function timed, with the waver running and without (its SPU task's wait included). */
extern "C" uint32_t dev_state_player(void);
extern "C" int g_dev_waver = 0;
namespace {
struct DevWaver {
    std::vector<double> at;   // seconds, ascending
    size_t next = 0;
    bool t0_set = false, was_active = false;
    std::chrono::steady_clock::time_point t0;
    unsigned long long starts = 0, refused = 0, ends = 0, frames_on = 0, frames_off = 0;
    double ms_on = 0, ms_off = 0, max_on = 0, max_off = 0;
    unsigned long long frames_this = 0;
};
DevWaver* g_dw;
// The TOC the set-up's stub (func_009842F8: addis/addi +0x1F8C0, to the waver's 0x01057910) expects in its
// caller, func_0018B708's module. The frame function runs under another (its own stubs add 0x2F2B8), so the
// call is made under this one and the frame function's r2 given back after.
constexpr uint32_t kCallerToc = 0x01038050u;
constexpr uint32_t kWaverStateSlot = 0x01054804u;   // the waver's TOC slot r2-0x310C: its state block (+0 active)
}  // namespace

extern "C" void dev_waver_init(void)
{
    const char* e = getenv("DOTHEROES_DEV_WAVER");
    if (!e || !*e) return;
    g_dw = new DevWaver;
    for (const char* p = e; p && *p; ) {
        g_dw->at.push_back(atof(p));
        p = strchr(p, ',');
        if (p) p++;
    }
    std::sort(g_dw->at.begin(), g_dw->at.end());
    g_dev_waver = 1;
    fprintf(stderr, "[dev-waver] the waver started at the hero's position %zu time(s), the first at %.1f s\n",
            g_dw->at.size(), g_dw->at.empty() ? 0.0 : g_dw->at[0]);
}

extern "C" void dev_waver_report(void)
{
    static int once;
    if (!g_dw || once++) return;
    fprintf(stderr, "[dev-waver] %llu start(s), %llu refused, %llu end(s); the frame function: %llu frames with the waver "
            "running, mean %.3f ms, max %.3f ms; %llu without, mean %.3f ms, max %.3f ms\n", g_dw->starts, g_dw->refused,
            g_dw->ends, g_dw->frames_on, g_dw->frames_on ? g_dw->ms_on / g_dw->frames_on : 0.0, g_dw->max_on,
            g_dw->frames_off, g_dw->frames_off ? g_dw->ms_off / g_dw->frames_off : 0.0, g_dw->max_off);
}

extern "C" int dotheroes_dev_waver(ppu_context* ctx)
{
    static int inner;
    if (inner || !g_dw) return 0;
    DevWaver& d = *g_dw;
    const auto now = std::chrono::steady_clock::now();
    if (!d.t0_set) { d.t0_set = true; d.t0 = now; }
    const double t = std::chrono::duration<double>(now - d.t0).count();
    const uint32_t state = g_rd32(kWaverStateSlot);
    if (d.next < d.at.size() && t >= d.at[d.next]) {
        d.next++;
        const uint32_t hero = dev_state_player();
        if (!hero || vm_base[state]) {
            d.refused++;
            fprintf(stderr, "[dev-waver] not started at %.1f s: %s\n", t, !hero ? "the player's object not found yet"
                    : "already running");
        } else {
            // the frame function's registers, given back after the call (its inputs r3 and f1 among them)
            uint64_t g[14]; double f[14];
            memcpy(g, ctx->gpr, sizeof g); memcpy(f, ctx->fpr, sizeof f);
            const uint64_t lr = ctx->lr, ctr = ctx->ctr; const uint32_t cr = ctx->cr, xer = ctx->xer;
            const uint32_t sp = (uint32_t)ctx->gpr[1], frame = sp - 0x100;
            g_wr64(frame, sp);                                      // a frame of its own, as func_0018B708's
            for (int k = 0; k < 12; k += 4) g_wr32(frame + 0x70 + k, g_rd32(hero + 0x428 + k));   // x, y, z
            ctx->gpr[1] = frame;
            ctx->gpr[2] = kCallerToc;
            ctx->gpr[3] = frame + 0x70;
            guest_call(ctx, func_009842F8, 0x0018B774);
            memcpy(ctx->gpr, g, sizeof g); memcpy(ctx->fpr, f, sizeof f);
            ctx->lr = lr; ctx->ctr = ctr; ctx->cr = cr; ctx->xer = xer;
            d.starts++;
            uint32_t x = g_rd32(hero + 0x428), z = g_rd32(hero + 0x430);
            float fx, fz; memcpy(&fx, &x, 4); memcpy(&fz, &z, 4);
            fprintf(stderr, "[dev-waver] started at %.1f s at (%.1f, %.1f): %s\n", t, fx, fz,
                    vm_base[state] ? "running" : "did not start (the game's set-up declined)");
        }
    }
    const bool on = vm_base[state] != 0;
    if (d.was_active && !on) {
        d.ends++;
        fprintf(stderr, "[dev-waver] ended at %.1f s after %llu frames\n", t, d.frames_this);
    }
    if (on && !d.was_active) d.frames_this = 0;
    // the frame function, timed (its lifted body: the guard's second entry declines)
    inner = 1;
    const auto a = std::chrono::steady_clock::now();
    func_000810F8(ctx);
    while (g_trampoline_fn) { void (*fn)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; fn(ctx); }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
    inner = 0;
    if (on) { d.frames_on++; d.frames_this++; d.ms_on += ms; d.max_on = std::max(d.max_on, ms); }
    else { d.frames_off++; d.ms_off += ms; d.max_off = std::max(d.max_off, ms); }
    d.was_active = on;
    return 1;
}

/* The waver's set-up and frames native (port/native/waver_game.h). func_003CEB48 (r3 = &{x, y, z}; r2 the
 * waver's TOC 0x01057910, set by the stub): the game's own stores and calls in its frame (stdu r1,-0xB0;
 * func_003A4740's r4 points into it), the SPURS objects' places given the markers. r3 at the end as the
 * game's leaves it is unused by its callers; 0 here. */
extern "C" int dotheroes_waver_setup(ppu_context* ctx)
{
    if (!waver_native()) return 0;
    const uint64_t sp = ctx->gpr[1], lr = ctx->lr;
    const uint32_t toc = (uint32_t)ctx->gpr[2], pos = (uint32_t)ctx->gpr[3];
    const uint32_t frame = (uint32_t)sp - 0xB0;
    g_wr64(frame, sp);
    g_wr64(frame + 0xC0, lr);
    for (int r = 27; r <= 31; r++) g_wr64(frame + 0x88 + 8 * (r - 27), ctx->gpr[r]);
    ctx->gpr[1] = frame;
    guest_call(ctx, func_003B64A8, 0x003CEB70);   // the other screen effects stopped (r3 = pos, unused)
    const uint32_t W = g_rd32(toc - 0x310C);
    if (vm_base[W]) {   // running already: the game's does nothing more
        ctx->gpr[1] = sp; ctx->lr = lr; ctx->gpr[3] = (W + 0x28) & 0xFFFFFFFFu;
        return 1;
    }
    g_wr32(W + 0x64, g_rd32(pos + 0));   // x (lfs, stfs: the same bits)
    g_wr32(W + 0x6C, g_rd32(pos + 8));   // z
    vm_base[W] = 1;                      // running
    g_wr32(W + 0x8, 0);
    g_wr32(W + 0x68, 0);
    g_wr32(W + 0x74, 0);                 // the time so far
    g_wr32(W + 0x4, 0);                  // frames
    ctx->gpr[3] = W + 0x28; ctx->gpr[4] = (uint64_t)frame + 0x70;
    guest_call(ctx, func_003A4740, 0x003CEBD4);
    g_wr32(W + 0x84, (uint32_t)ctx->gpr[3]);   // the first texture
    ctx->gpr[3] = W + 0x40; ctx->gpr[4] = (uint64_t)frame + 0x70;
    guest_call(ctx, func_003A4740, 0x003CEBE8);
    g_wr32(W + 0x88, (uint32_t)ctx->gpr[3]);   // the second
    ctx->gpr[3] = W + 0x28;
    guest_call(ctx, func_003A46E8, 0x003CEBF8);
    ctx->gpr[3] = W + 0x40;
    guest_call(ctx, func_003A46E8, 0x003CEC04);
    g_wr32(W + 0x8C, 0);
    g_wr32(W + 0x80, g_rd32(W + 0x102A4));     // the height field
    g_wr32(W + 0x60, waver_task::kNativeInstance);
    g_wr32(W + 0x58, waver_task::kNativeTaskset);
    g_wr32(W + 0x5C, waver_task::kNativeBlock);
    waver_native_started(W);
    ctx->gpr[1] = sp;
    ctx->lr = lr;
    ctx->gpr[3] = 0;
    return 1;
}

/* The game's SPURS wrappers, shared by the water's and the waver's code (func_004736E0 create a task on
 * r3 taskset, r4 block, r5 image; func_004735DC wait for the block's task; func_00473510 free the block;
 * func_0047353C destroy the taskset; func_004734A0 finalize the instance): the waver's markers handled
 * here, r3 = 0 (CELL_OK); anything else declined. Check mode watches the lifted waver's creations. */
extern "C" int dotheroes_spurs_create_task(ppu_context* ctx)
{
    const uint32_t ts = (uint32_t)ctx->gpr[3];
    if (ts == waver_task::kNativeTaskset) { waver_native_create(); ctx->gpr[3] = 0; return 1; }
    if (waver_checking() && ts) {
        const uint32_t W = g_rd32(waver_task::kStateSlot);
        if (ts == g_rd32(W + 0x58)) waver_check_create(W);
    }
    return 0;
}
extern "C" int dotheroes_spurs_wait_task(ppu_context* ctx)
{
    const uint32_t b = (uint32_t)ctx->gpr[3];
    if (b != waver_task::kNativeBlock) {
        if (waver_checking() && b) {   // check mode: the block as the game left it before this wait
            const uint32_t W = g_rd32(waver_task::kStateSlot);
            if (b == g_rd32(W + 0x5C)) waver_check_wait(W);
        }
        return 0;
    }
    waver_native_wait();
    ctx->gpr[3] = 0;
    return 1;
}
extern "C" int dotheroes_spurs_free_block(ppu_context* ctx)
{
    const uint32_t b = (uint32_t)ctx->gpr[3];
    if (b == waver_task::kNativeBlock) { ctx->gpr[3] = 0; return 1; }
    if (waver_checking() && b && b == g_rd32(g_rd32(waver_task::kStateSlot) + 0x5C)) waver_check_end();
    return 0;
}
extern "C" int dotheroes_spurs_destroy_taskset(ppu_context* ctx)
{
    if ((uint32_t)ctx->gpr[3] != waver_task::kNativeTaskset) return 0;
    waver_native_stopped();
    ctx->gpr[3] = 0;
    return 1;
}
extern "C" int dotheroes_spurs_finalize(ppu_context* ctx)
{
    if ((uint32_t)ctx->gpr[3] != waver_task::kNativeInstance) return 0;
    ctx->gpr[3] = 0;
    return 1;
}

/* func_005CF5BC(r3 the .gz image, r4 its size, r5 the output), the game's
 * inflate of one map pack through a SPURS instance of its own; r2 is its
 * TOC (the stub func_009824B8 set it). 1 when inflated natively, r3 = 0 as
 * the game's function returns. */
extern "C" int dotheroes_inflate_direct(ppu_context* ctx)
{
    if (!inflate_direct_call((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5],
                             (uint32_t)ctx->gpr[2]))
        return 0;
    ctx->gpr[3] = 0;
    return 1;
}
