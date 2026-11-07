// Native replacements for the game's SPU tasks (port/native/bullet_cd_task.cpp).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Register them with the runtime per DOTHEROES_NATIVE_CD; before the game runs.
void dotheroes_native_init(void);

#ifdef __cplusplus
}

#include <stdint.h>
// R8 (phys_direct.cpp): a collision task issued directly, its queue entry
// [slot, command, descriptor, slot] in hand. accept: native collision is on
// and this entry's work is what bullet_cd does (no side effects: a declined
// entry runs lifted, counted as always). run: the task's work, on any thread.
bool bullet_cd_direct_accept(const uint32_t entry[4]);
void bullet_cd_direct_run(const uint32_t entry[4]);
#endif
