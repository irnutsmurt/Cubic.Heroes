// R8: the game's Edge zlib inflate done in the calling thread, without SPURS
// (port/native/inflate_direct.cpp; docs/SPU_NON_AUDIO.md section 2).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The entry hook's guard (config/r8_hooks.json, func_005CF5BC): set at start-up
// unless DOTHEROES_INFLATE_DIRECT=0, or DOTHEROES_NATIVE_INFLATE is 0 or check
// (those need the SPU task).
extern int g_dotheroes_inflate_direct;

// func_005CF5BC(r3 the .gz image's EA, r4 its size, r5 the output's EA), with
// toc the r2 it was entered with: 1 when the pack was inflated here (the hook
// returns r3 = 0, as the game's function does), 0 to leave the call to the
// lifted body (the SPURS instance, taskset, event flag, queue and task).
int inflate_direct_call(uint32_t gz, uint32_t gz_size, uint32_t out, uint32_t toc);

// The counts line (also at exit): what was inflated here, what went through
// SPURS and why. A test's stop calls it (dev_state.cpp).
void inflate_direct_report(void);

#ifdef __cplusplus
}
#endif
