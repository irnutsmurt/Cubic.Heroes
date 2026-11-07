// R8: the game's native physics tasks issued and completed directly, without
// SPURS (port/native/phys_direct.cpp; docs/NATIVE_R8.md section 6.1).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The guard of both entry hooks (config/r8_hooks.json): set by
// phys_direct_init when a native physics task is on and DOTHEROES_PHYS_DIRECT
// is not 0.
extern int g_phys_direct;

// Before the game runs (dotheroes_native_init).
void phys_direct_init(void);

// The counts line (also at exit): issued natively and lifted, run by the
// waiting thread, the waits. A test's stop calls it (dev_state.cpp).
void phys_direct_report(void);

// func_00955CF0(manager, slot, entry[4]), the game's issue of one physics task:
// 1 when its work went to a native job (the hook returns 0 for the game), 0
// to leave the call to the lifted body (CreateTask and the queue push).
int phys_direct_issue(uint32_t m, uint32_t slot, const uint32_t entry[4]);

// func_00955BD4(manager, &cursor, &w0..&w3), the game's wait for one task of
// the manager: the first slot from the cursor on with a completion -- a native
// job's, or an entry in its SPURS output queue for a lifted task -- written
// as the game's poll writes it. tid: the guest thread, for the frame trace.
// Returns r3.
int phys_direct_poll(uint32_t m, uint32_t cursor_ea, const uint32_t out_ea[4], uint32_t tid);

#ifdef __cplusplus
}
#endif
