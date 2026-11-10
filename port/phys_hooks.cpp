/* R8's entry hooks (config/r8_hooks.json), the guest registers in and out:
 * the game's physics task issue and wait, whose work is
 * native/phys_direct.cpp's (docs/NATIVE_R8.md section 6.1), and its map
 * pack inflate, native/inflate_direct.cpp's (docs/SPU_NON_AUDIO.md section 2). */
#include "ppu_recomp.h"   /* ppu_context */
#include "native/inflate_direct.h"
#include "native/water_task.h"
#include "native/phys_direct.h"

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
