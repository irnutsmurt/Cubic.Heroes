// The game's Bullet solver commands as native code (bullet_solver.h). The
// algorithm follows Bullet 2.74's SpuParallellSolverTask.cpp (zlib licence,
// Copyright (c) 2007 Starbreeze Studios, written by Marten Svanfeldt; altered:
// DMA replaced by reads and writes of the game's structures in guest memory);
// the layouts are the game's, checked against captured steps.
#include "bullet_solver.h"

#include <cstring>
#include <thread>

namespace bullet_solver {
namespace {

// SpuSolverTaskDesc (checked: tests/solver_layout.py)
constexpr uint32_t kDescTask = 0x04, kDescHash = 0x10, kDescContacts = 0x18, kDescLock = 0x30;
// SpuSolverHash: dependency matrix at +0 (128 rows of 4 words), the cell masks
// at +0x800 (row 0 the processed cells, row 1 + task a task's held cells), the
// cells at +0x870 (16 bytes each)
constexpr uint32_t kHashMasks = 0x800, kHashCells = 0x870, kCells = 128, kWords = 4, kRows = 7;
constexpr uint32_t kCellContacts = 8, kCellContactOffset = 10, kCellConstraints = 12;
// the contact constraint record, three per contact (the contact, two friction rows)
constexpr uint32_t kConstraintBytes = 0x90, kAppliedImpulse = 0x54, kContactPoint = 0x78;

inline uint32_t rd32(const uint8_t* b, uint32_t ea)
{
    const uint8_t* p = b + ea;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
inline uint16_t rd16(const uint8_t* b, uint32_t ea) { return (uint16_t)(b[ea] << 8 | b[ea + 1]); }
inline void wr32(uint8_t* b, uint32_t ea, uint32_t v)
{
    uint8_t* p = b + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

// min(3, impulse) as the SPU task computes it, btMin(3, x) = 3 < x ? 3 : x,
// on the float's bits: the value stored is one of the two exactly.
inline uint32_t clamp_impulse(uint32_t bits)
{
    float x;
    memcpy(&x, &bits, 4);
    return 3.0f < x ? 0x40400000u : bits;
}

// getNextFreeCell: the next cell for `task`, kCells when every cell is processed.
uint32_t next_free_cell(uint8_t* base, uint32_t hash, uint32_t task, uint32_t lock, const Sync& sync, Stats& st)
{
    const uint32_t own = hash + kHashMasks + 16u * (1 + task);
    for (uint32_t k = 0; k < kWords; k++) wr32(base, own + 4 * k, 0);   // this task holds nothing now
    sync.acquire(base, lock);
    uint32_t cell = kCells;
    for (int spins = 0;; spins++) {
        uint32_t m[kRows][kWords], held[kWords] = { 0, 0, 0, 0 };
        for (uint32_t r = 0; r < kRows; r++)
            for (uint32_t k = 0; k < kWords; k++) m[r][k] = rd32(base, hash + kHashMasks + 16 * r + 4 * k);
        for (uint32_t r = 1; r < kRows; r++)
            for (uint32_t k = 0; k < kWords; k++) held[k] |= m[r][k];
        for (uint32_t i = 0; i < kCells; i++) {
            const uint32_t k = i >> 5, bit = 1u << (i & 31);
            if ((m[0][k] | held[k]) & bit) continue;                 // processed, or held
            bool clash = false;                                       // depends on a held cell
            for (uint32_t w = 0; w < kWords && !clash; w++) clash = (held[w] & rd32(base, hash + 16 * i + 4 * w)) != 0;
            if (clash) continue;
            cell = i;
            for (uint32_t w = 0; w < kWords; w++) wr32(base, own + 4 * w, rd32(base, hash + 16 * i + 4 * w));
            wr32(base, hash + kHashMasks + 4 * k, m[0][k] | bit);
            break;
        }
        if (cell != kCells) break;
        bool all = true;
        for (uint32_t k = 0; k < kWords; k++) all &= m[0][k] == 0xFFFFFFFFu;
        if (all) break;
        st.waits += spins == 0;                                       // another task holds what is left
        std::this_thread::yield();
    }
    sync.release(base, lock);
    return cell;
}

}  // namespace

void writeback_task(uint8_t* base, uint32_t desc, const Sync& sync, Stats& st)
{
    const uint32_t task = rd32(base, desc + kDescTask), hash = rd32(base, desc + kDescHash);
    const uint32_t contacts = rd32(base, desc + kDescContacts), lock = rd32(base, desc + kDescLock);
    for (;;) {
        const uint32_t cell = next_free_cell(base, hash, task, lock, sync, st);
        if (cell >= kCells) break;
        st.cells++;
        const uint32_t c = hash + kHashCells + 16 * cell;
        const uint32_t n = rd16(base, c + kCellContacts), first = rd16(base, c + kCellContactOffset);
        if (!n && !rd16(base, c + kCellConstraints)) continue;
        for (uint32_t j = 0; j < n; j++) {
            const uint32_t rec = contacts + (first + 3 * j) * kConstraintBytes;
            const uint32_t point = rd32(base, rec + kContactPoint);
            wr32(base, point, clamp_impulse(rd32(base, rec + kAppliedImpulse)));
            st.contacts++;
        }
    }
}

unsigned writeback_expected(const uint8_t* base, uint32_t desc, Write* out, unsigned max)
{
    const uint32_t hash = rd32(base, desc + kDescHash), contacts = rd32(base, desc + kDescContacts);
    unsigned n = 0;
    for (uint32_t i = 0; i < kCells; i++) {
        const uint32_t c = hash + kHashCells + 16 * i;
        const uint32_t nc = rd16(base, c + kCellContacts), first = rd16(base, c + kCellContactOffset);
        for (uint32_t j = 0; j < nc; j++) {
            const uint32_t rec = contacts + (first + 3 * j) * kConstraintBytes;
            if (n < max) out[n] = { rd32(base, rec + kContactPoint), clamp_impulse(rd32(base, rec + kAppliedImpulse)) };
            n++;
        }
    }
    return n;
}

}  // namespace bullet_solver
