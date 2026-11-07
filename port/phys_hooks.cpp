/* R8: the entry hooks of the game's physics task issue and wait
 * (config/r8_hooks.json), the guest registers in and out; the work is
 * native/phys_direct.cpp's (docs/NATIVE_R8.md section 6.1). */
#include "ppu_recomp.h"   /* ppu_context */
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
