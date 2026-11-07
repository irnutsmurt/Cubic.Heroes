// The game's Bullet solver SPU task (runtime image 5): its commands marked in
// the frame trace, captures at its phase boundaries, and -- experimental, one
// command at a time -- native commands (port/native/bullet_solver_task.cpp).
#pragma once

// Before the game runs; phys_stats: DOTHEROES_PHYS_STATS=1.
void bullet_solver_init(bool phys_stats);

#include <stdint.h>
// R8 (phys_direct.cpp): a solver task issued directly, its queue entry
// [slot, 1, descriptor, slot] in hand. accept: the descriptor's command is
// one done natively (DOTHEROES_NATIVE_SOLVER); *cmd gets it. run: the
// command's work for the slot, on any thread, as a native task does it.
bool bullet_solver_direct_accept(const uint32_t entry[4], uint32_t* cmd);
void bullet_solver_direct_run(uint32_t slot, uint32_t desc, uint32_t cmd);
