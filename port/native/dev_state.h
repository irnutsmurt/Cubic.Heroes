// Development aids for benchmarks that need the game's own state (dev_state.cpp).
#pragma once

void dev_state_init();

// Physics steps so far: collision tasks begun (bullet_cd_task.cpp; counted
// when native collision, DOTHEROES_PHYS_STATS or DOTHEROES_DEV_STATE is on).
unsigned long long dotheroes_phys_steps();
