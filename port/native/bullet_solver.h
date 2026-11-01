// The game's Bullet solver commands as native code, working on its own
// structures in guest memory. Every offset used here was checked against
// captured steps of the game (tests/solver_layout.py; docs/BULLET_NATIVE.md,
// "Solver structures, checked"); Bullet 2.74's SpuParallellSolverTask.cpp is
// the reference for the algorithm only -- the game's Bullet is 2.73, and its
// contact constraint already differs from 2.74's (the pointer back to the
// manifold point is at +0x78, not +0x70).
#pragma once
#include <cstdint>

namespace bullet_solver {

// How a task takes the hash's spin lock: in the game, a lock shared with the
// lifted SPU tasks of the same round (bullet_solver_task.cpp); offline, a
// plain counter pair.
struct Sync {
    void (*acquire)(uint8_t* base, uint32_t lock_ea);
    void (*release)(uint8_t* base, uint32_t lock_ea);
};

struct Stats {
    unsigned cells = 0;       // cells this task claimed
    unsigned contacts = 0;    // contacts it wrote back
    unsigned waits = 0;       // times it found no free cell yet not every cell processed
};

// Command 6, the warm-start write-back, for one task of a round: claim free
// cells as the SPU task does (under the spin lock: the first cell neither
// processed nor dependent on a cell another task holds; the task's mask row
// set to the cell's dependencies, the cell's processed bit set), and for each
// contact in the cell store min(3, applied impulse) at the contact's pointer
// back to its manifold point; until every cell is processed. The task's row is
// cleared before each claim and so on return. `task`: the descriptor's task
// id (+4), 0-2.
void writeback_task(uint8_t* base, uint32_t desc, const Sync& sync, Stats& st);

// The same write-back as one list, for checking: each contact's target and the
// 32 bits it gets. Returns the number of contacts.
struct Write { uint32_t ea, bits; };
unsigned writeback_expected(const uint8_t* base, uint32_t desc, Write* out, unsigned max);

}  // namespace bullet_solver
