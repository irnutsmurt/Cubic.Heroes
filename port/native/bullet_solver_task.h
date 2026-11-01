// The game's Bullet solver SPU task (runtime image 5): its commands marked in
// the frame trace, captures at its phase boundaries, and -- experimental, one
// command at a time -- native commands (port/native/bullet_solver_task.cpp).
#pragma once

// Before the game runs; phys_stats: DOTHEROES_PHYS_STATS=1.
void bullet_solver_init(bool phys_stats);
