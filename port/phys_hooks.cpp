/* R8's entry hooks (config/r8_hooks.json), the guest registers in and out:
 * the game's physics task issue and wait, whose work is
 * native/phys_direct.cpp's (docs/NATIVE_R8.md section 6.1), and its map
 * pack inflate, native/inflate_direct.cpp's (docs/SPU_NON_AUDIO.md section 2). */
#include "ppu_recomp.h"   /* ppu_context */
#include "native/inflate_direct.h"
#include "native/water_task.h"
#include "native/phys_direct.h"

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

/* func_005CF5BC(r3 the .gz image, r4 its size, r5 the output), the game's
 * inflate of one map pack through a SPURS instance of its own; r2 is its
 * TOC (the stub func_009824B8 set it). 1 when inflated natively, r3 = 0 as
 * the game's function returns. */
// The water's per-step signal to its SPU task (config/r8_hooks.json, port/native/water_task.cpp):
// handled natively, r3 = CELL_OK as cellSpursEventFlagSet's; declined, the lifted call runs.
extern "C" int dotheroes_water_signal(ppu_context* ctx)
{
    if (!water_task_signal((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4] & 0xFFFFu)) return 0;
    ctx->gpr[3] = 0;
    return 1;
}

extern "C" int dotheroes_inflate_direct(ppu_context* ctx)
{
    if (!inflate_direct_call((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5],
                             (uint32_t)ctx->gpr[2]))
        return 0;
    ctx->gpr[3] = 0;
    return 1;
}
