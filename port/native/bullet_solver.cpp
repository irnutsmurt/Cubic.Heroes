/*
Bullet Continuous Collision Detection and Physics Library - Parallel solver
Copyright (c) 2007 Starbreeze Studios

This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from the use of this software.
Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it freely,
subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

Written by: Marten Svanfeldt
*/
/* Altered source (zlib licence, condition 2): this file is not Bullet's SpuParallellSolverTask.cpp but a
 * version of its algorithm altered for this project -- its DMA replaced by reads and writes of the game's
 * structures in guest memory, on the host. */
// The game's Bullet solver commands as native code (bullet_solver.h). The
// algorithm follows Bullet 2.74's SpuParallellSolverTask.cpp (zlib licence,
// Copyright (c) 2007 Starbreeze Studios, written by Marten Svanfeldt; altered:
// DMA replaced by reads and writes of the game's structures in guest memory);
// the layouts are the game's, checked against captured steps.
#include "bullet_solver.h"

#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>
#include <xmmintrin.h>

#include "spu_helpers.h"   // the runtime's exact SPU single-precision arithmetic (iterate)

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

namespace {

// The copy-back's layouts (checked: tests/solver_copyback_layout.py)
constexpr uint32_t kDescBodies = 0x14, kDescFirst = 0x30, kDescCount = 0x34, kDescRigidBodies = 0x38;
constexpr uint32_t kSolverBodyBytes = 0x40, kDeltaLinear = 0x00, kDeltaAngular = 0x10, kInvMass = 0x24;
constexpr uint32_t kRbLinearVelocity = 0x130, kRbAngularVelocity = 0x140, kRbCompanionId = 0xC4;

// The SPU's single precision: rounding toward zero, denormals as zero. The
// adds below use SSE with MXCSR set so; the compiler barriers keep them
// between the mode's set and its restore.
struct SpuRounding {
    unsigned csr = _mm_getcsr();
    SpuRounding() { _mm_setcsr(csr | 0x6000u | 0x8040u); __asm__ __volatile__("" ::: "memory"); }
    ~SpuRounding() { __asm__ __volatile__("" ::: "memory"); _mm_setcsr(csr); }
};

inline uint32_t add_bits(uint32_t a, uint32_t b)
{
    float x, y;
    memcpy(&x, &a, 4); memcpy(&y, &b, 4);
    const float z = _mm_cvtss_f32(_mm_add_ss(_mm_set_ss(x), _mm_set_ss(y)));
    uint32_t r;
    memcpy(&r, &z, 4);
    return r;
}

inline bool positive(uint32_t bits)   // > 0, denormals as zero
{
    float x;
    memcpy(&x, &bits, 4);
    return _mm_comigt_ss(_mm_set_ss(x), _mm_setzero_ps()) != 0;
}

template <class Out>
unsigned copyback_range(const uint8_t* base, uint32_t desc, Out&& out)
{
    const uint32_t sbl = rd32(base, desc + kDescBodies), first = rd32(base, desc + kDescFirst);
    const uint32_t n = rd32(base, desc + kDescCount), rbl = rd32(base, desc + kDescRigidBodies);
    SpuRounding mode;
    for (uint32_t i = first; i < first + n; i++) {
        const uint32_t rb = rd32(base, rbl + 4 * i), sb = sbl + kSolverBodyBytes * i;
        uint32_t w[9];
        for (uint32_t k = 0; k < 4; k++) {
            w[k] = rd32(base, rb + kRbLinearVelocity + 4 * k);
            w[4 + k] = rd32(base, rb + kRbAngularVelocity + 4 * k);
        }
        if (positive(rd32(base, sb + kInvMass)))
            for (uint32_t k = 0; k < 4; k++) {
                w[k] = add_bits(w[k], rd32(base, sb + kDeltaLinear + 4 * k));
                w[4 + k] = add_bits(w[4 + k], rd32(base, sb + kDeltaAngular + 4 * k));
            }
        w[8] = 0xFFFFFFFFu;
        out(rb, w);
    }
    return n;
}

}  // namespace

// ---- command 1, the body set-up ---------------------------------------------
// Bullet 2.74's CMD_SOLVER_SETUP_BODIES (setupSpuBody) as the game's task does
// it, checked on captured steps (tests/solver_setup_diff.py: c1r1 against
// c2r1): for each body of the task's range, in packages of 256, the solver body
// (0x40 bytes, the whole of it written: the task puts its local copy) -- the
// two velocity deltas zero (all four lanes), the angular factor (+0x20) and
// the inverse mass (+0x24) from the rigid body (+0x154, +0x150), +0x28 zero
// (2.74's friction: not set, a fresh local store's zero), +0x2C the address of
// the rigid body's copy in the task's local store (setupSpuBody's
// m_originalBody: 0xC800 + 0x1F0 x its place in the package), the rest zero --
// and the rigid body's companion id (+0xC4) set to its index. The task puts
// the whole rigid body back, unchanged but for the id; the PPU writes none of
// these while it waits for the command.
namespace {
constexpr uint32_t kDescSolverBodies = 0x14, kDescFirstBody = 0x30, kDescBodyCount = 0x34, kDescBodyList = 0x38;
constexpr uint32_t kRbInverseMass = 0x150, kRbAngularFactor = 0x154;
constexpr uint32_t kSetupPackage = 256, kSetupLsBodies = 0xC800, kRbSize = 0x1F0;
template <class F> unsigned setup_range(const uint8_t* base, uint32_t desc, F&& out)
{
    const uint32_t bodies = rd32(base, desc + kDescSolverBodies), first = rd32(base, desc + kDescFirstBody);
    const uint32_t count = rd32(base, desc + kDescBodyCount), list = rd32(base, desc + kDescBodyList);
    for (uint32_t k = 0; k < count; k++) {
        const uint32_t i = first + k, rb = rd32(base, list + 4 * i);
        uint32_t w[16] = {};
        w[8] = rd32(base, rb + kRbAngularFactor);
        w[9] = rd32(base, rb + kRbInverseMass);
        w[11] = kSetupLsBodies + kRbSize * (k % kSetupPackage);
        out(bodies + 0x40 * i, w, rb, i);
    }
    return count;
}
}  // namespace

unsigned setup_bodies_task(uint8_t* base, uint32_t desc)
{
    return setup_range(base, desc, [base](uint32_t sb, const uint32_t* w, uint32_t rb, uint32_t i) {
        for (uint32_t k = 0; k < 16; k++) wr32(base, sb + 4 * k, w[k]);
        wr32(base, rb + kRbCompanionId, i);
    });
}

unsigned setup_bodies_expected(const uint8_t* base, uint32_t desc, SetupBody* out, unsigned max)
{
    unsigned n = 0;
    setup_range(base, desc, [&](uint32_t sb, const uint32_t* w, uint32_t rb, uint32_t i) {
        if (n < max) { out[n].sb = sb; memcpy(out[n].w, w, sizeof out[n].w); out[n].rb = rb; out[n].index = i; }
        n++;
    });
    return n;
}

unsigned copyback_task(uint8_t* base, uint32_t desc)
{
    return copyback_range(base, desc, [base](uint32_t rb, const uint32_t* w) {
        for (uint32_t k = 0; k < 4; k++) {
            wr32(base, rb + kRbLinearVelocity + 4 * k, w[k]);
            wr32(base, rb + kRbAngularVelocity + 4 * k, w[4 + k]);
        }
        wr32(base, rb + kRbCompanionId, w[8]);
    });
}

unsigned copyback_expected(const uint8_t* base, uint32_t desc, BodyWords* out, unsigned max)
{
    unsigned n = 0;
    copyback_range(base, desc, [&](uint32_t rb, const uint32_t* w) {
        if (n < max) { out[n].rb = rb; memcpy(out[n].w, w, sizeof out[n].w); }
        n++;
    });
    return n;
}

namespace {

// Iterate's layouts (checked: tests/solver_iterate_layout.py). The descriptor's
// index list of each cell's bodies; a cell's count of them and where its list
// starts (u16, +0 and +2).
constexpr uint32_t kDescBodyOffsets = 0x20;
constexpr uint32_t kCellBodies = 0, kCellBodyList = 2;
// a constraint row: what the SPU task's row solve reads and writes
constexpr uint32_t kRowRelpos1 = 0x00, kRowNormal = 0x10, kRowRelpos2 = 0x20, kRowAngularA = 0x30, kRowAngularB = 0x40;
constexpr uint32_t kRowBodyA = 0x58, kRowBodyB = 0x5C, kRowFriction = 0x60, kRowJacDiagInv = 0x68;
constexpr uint32_t kRowRhs = 0x7C, kRowCfm = 0x80, kRowLower = 0x84, kRowUpper = 0x88;
constexpr uint32_t kAngularFactor = 0x20;
// the SPU task's temporary storage: a cell's index list (rounded to 16 bytes)
// and bodies come out of it, and a packet of contacts gets what is left
constexpr uint32_t kTempStorage = 0x25800, kContactBytes = 3 * kConstraintBytes;
constexpr uint32_t kSign = 0x80000000u;

// The SPU's single-precision operations, on the float's bits.
inline uint32_t sp_fa(uint32_t x, uint32_t y) { return spu__fa_any(x, y); }
inline uint32_t sp_fs(uint32_t x, uint32_t y) { return spu__fa_any(x, y ^ kSign); }
inline uint32_t sp_fm(uint32_t x, uint32_t y) { return spu__fm_any(x, y); }
inline uint32_t sp_fma(uint32_t x, uint32_t y, uint32_t z) { return spu__fma_any(x, y, z, 0, 0); }        // x*y + z
inline uint32_t sp_fnms(uint32_t x, uint32_t y, uint32_t z) { return spu__fma_any(x, y, z, kSign, 0); }   // z - x*y
inline bool sp_fcgt(uint32_t x, uint32_t y) { return spu__sf_key(x) > spu__sf_key(y); }
inline bool sp_zero(uint32_t x) { return spu__sf_key(x) == 0; }   // fceq(x, 0)

// The task's dot product, in its order: x*x + y*y first, then z*z, each step rounded.
inline uint32_t dot3(const uint32_t* a, const uint32_t* b) { return sp_fma(a[2], b[2], sp_fma(a[0], b[0], sp_fm(a[1], b[1]))); }

// A solver body as the task holds it in local store: its 16 words.
struct LocalBody { uint32_t w[16]; };

// btSolverBody's internalApplyImpulse as the task does it: nothing for a body
// of zero inverse mass; x, y, z of each delta (w kept).
inline void apply_impulse(uint32_t* b, const uint32_t* normal, const uint32_t* angular, uint32_t impulse)
{
    const uint32_t inv_mass = b[kInvMass / 4];
    if (sp_zero(inv_mass)) return;
    const uint32_t t = sp_fm(impulse, b[kAngularFactor / 4]);
    for (int k = 0; k < 3; k++) {
        b[kDeltaLinear / 4 + k] = sp_fa(b[kDeltaLinear / 4 + k], sp_fm(impulse, sp_fm(inv_mass, normal[k])));
        b[kDeltaAngular / 4 + k] = sp_fa(b[kDeltaAngular / 4 + k], sp_fm(t, angular[k]));
    }
}

// SpuResolveSingleConstraintRowGeneric as the game's task computes it: `r` the
// row's 36 words. Changes the row's applied impulse (+0x54) and the two bodies.
inline void resolve_row(uint32_t* r, uint32_t* b1, uint32_t* b2)
{
    const uint32_t* n = r + kRowNormal / 4;
    const uint32_t applied = r[kAppliedImpulse / 4], jac = r[kRowJacDiagInv / 4];
    const uint32_t dv1 = sp_fa(dot3(n, b1 + kDeltaLinear / 4), dot3(r + kRowRelpos1 / 4, b1 + kDeltaAngular / 4));
    const uint32_t dv2 = sp_fa(dot3(n, b2 + kDeltaLinear / 4), dot3(r + kRowRelpos2 / 4, b2 + kDeltaAngular / 4));
    uint32_t impulse = sp_fnms(applied, r[kRowCfm / 4], r[kRowRhs / 4]);
    impulse = sp_fnms(dv1, jac, impulse);
    impulse = sp_fma(dv2, jac, impulse);
    const uint32_t sum = sp_fa(applied, impulse), lower = r[kRowLower / 4], upper = r[kRowUpper / 4];
    if (sp_fcgt(lower, sum)) {
        impulse = sp_fs(lower, applied);
        r[kAppliedImpulse / 4] = lower;
    } else if (sp_fcgt(sum, upper)) {
        impulse = sp_fs(upper, applied);
        r[kAppliedImpulse / 4] = upper;
    } else {
        r[kAppliedImpulse / 4] = sum;
    }
    apply_impulse(b1, n, r + kRowAngularA / 4, impulse);
    apply_impulse(b2, n, r + kRowAngularB / 4, impulse ^ kSign);
}

inline void load_words(const uint8_t* base, uint32_t ea, uint32_t* w, unsigned n)
{
    for (unsigned k = 0; k < n; k++) w[k] = rd32(base, ea + 4 * k);
}
inline void store_words(uint8_t* base, uint32_t ea, const uint32_t* w, unsigned n)
{
    for (unsigned k = 0; k < n; k++) wr32(base, ea + 4 * k, w[k]);
}

}  // namespace

void iterate_task(uint8_t* base, uint32_t desc, const Sync& sync, Stats& st)
{
    const uint32_t task = rd32(base, desc + kDescTask), hash = rd32(base, desc + kDescHash);
    const uint32_t rows = rd32(base, desc + kDescContacts), lock = rd32(base, desc + kDescLock);
    const uint32_t bodies = rd32(base, desc + kDescBodies), offsets = rd32(base, desc + kDescBodyOffsets);
    std::vector<uint32_t> index;
    std::vector<LocalBody> local;
    for (;;) {
        const uint32_t cell = next_free_cell(base, hash, task, lock, sync, st);
        if (cell >= kCells) break;
        st.cells++;
        const uint32_t c = hash + kHashCells + 16 * cell;
        const uint32_t nb = rd16(base, c + kCellBodies), nc = rd16(base, c + kCellContacts);
        if (!nc && !rd16(base, c + kCellConstraints)) continue;
        // the cell's bodies, copied in as the task DMAs them (a body listed twice
        // gets two copies, the later written back last, as on the SPU)
        index.resize(nb);
        local.resize(nb);
        const uint32_t list = offsets + 4 * rd16(base, c + kCellBodyList);
        for (uint32_t b = 0; b < nb; b++) {
            index[b] = rd32(base, list + 4 * b);
            load_words(base, bodies + kSolverBodyBytes * index[b], local[b].w, 16);
        }
        // the typed constraints: loaded and stored back unchanged by the task
        if (nc) {
            const uint32_t used = ((4 * nb + 15) & ~15u) + kSolverBodyBytes * nb;
            uint32_t per = used < kTempStorage ? (kTempStorage - used) / kContactBytes : 0;
            if (!per) { st.bad++; per = 1; }   // more bodies than the SPU's local store holds: not seen
            const uint32_t first = rd16(base, c + kCellContactOffset);
            uint32_t row[36], fr[36];
            for (uint32_t done = 0; done < nc;) {
                const uint32_t n = nc - done < per ? nc - done : per;
                const uint32_t packet = rows + (first + 3 * done) * kConstraintBytes;
                for (uint32_t j = 0; j < n; j++) {   // the normal rows
                    const uint32_t ea = packet + j * kContactBytes;
                    load_words(base, ea, row, 36);
                    const uint32_t a = row[kRowBodyA / 4], b = row[kRowBodyB / 4];
                    if (a >= nb || b >= nb) { st.bad++; continue; }
                    resolve_row(row, local[a].w, local[b].w);
                    wr32(base, ea + kAppliedImpulse, row[kAppliedImpulse / 4]);
                    st.contacts++;
                }
                for (uint32_t j = 0; j < n; j++) {   // the friction rows, on the contact's bodies
                    const uint32_t ea = packet + j * kContactBytes;
                    const uint32_t a = rd32(base, ea + kRowBodyA), b = rd32(base, ea + kRowBodyB);
                    if (a >= nb || b >= nb) continue;
                    for (uint32_t k = 1; k <= 2; k++) {
                        const uint32_t fea = ea + k * kConstraintBytes;
                        const uint32_t imp = rd32(base, ea + kAppliedImpulse);
                        load_words(base, fea, fr, 36);
                        fr[kRowLower / 4] = sp_fm(fr[kRowFriction / 4], imp ^ kSign);
                        fr[kRowUpper / 4] = sp_fm(fr[kRowFriction / 4], imp);
                        resolve_row(fr, local[a].w, local[b].w);
                        wr32(base, fea + kAppliedImpulse, fr[kAppliedImpulse / 4]);
                        wr32(base, fea + kRowLower, fr[kRowLower / 4]);
                        wr32(base, fea + kRowUpper, fr[kRowUpper / 4]);
                    }
                }
                done += n;
            }
        }
        for (uint32_t b = 0; b < nb; b++) store_words(base, bodies + kSolverBodyBytes * index[b], local[b].w, 16);
    }
}

void iterate_spans(const uint8_t* base, uint32_t desc, std::vector<Span>& body_spans, std::vector<Span>& row_spans,
                   IterateShape& shape)
{
    const uint32_t hash = rd32(base, desc + kDescHash), rows = rd32(base, desc + kDescContacts);
    const uint32_t bodies = rd32(base, desc + kDescBodies), offsets = rd32(base, desc + kDescBodyOffsets);
    std::vector<uint32_t> listed;
    body_spans.clear();
    row_spans.clear();
    shape = IterateShape();
    for (uint32_t i = 0; i < kCells; i++) {
        const uint32_t c = hash + kHashCells + 16 * i;
        const uint32_t nb = rd16(base, c + kCellBodies), nc = rd16(base, c + kCellContacts);
        if (!nc && !rd16(base, c + kCellConstraints)) continue;
        shape.cells++;
        shape.contacts += nc;
        const uint32_t list = offsets + 4 * rd16(base, c + kCellBodyList);
        for (uint32_t b = 0; b < nb; b++) listed.push_back(rd32(base, list + 4 * b));
        const uint32_t used = ((4 * nb + 15) & ~15u) + kSolverBodyBytes * nb;
        shape.multi_packet += used >= kTempStorage || nc > (kTempStorage - used) / kContactBytes;
        if (nc) row_spans.push_back({ rows + rd16(base, c + kCellContactOffset) * kConstraintBytes, nc * kContactBytes });
    }
    std::sort(listed.begin(), listed.end());
    for (size_t k = 0; k < listed.size(); k++) {
        if (k && listed[k] == listed[k - 1]) {
            shape.shared += k < 2 || listed[k - 2] != listed[k];   // count a body once however many lists
            continue;
        }
        body_spans.push_back({ bodies + kSolverBodyBytes * listed[k], kSolverBodyBytes });
    }
    shape.bodies = (unsigned)body_spans.size();
}

// ---- command 2, the manifold set-up -----------------------------------------
// Bullet 2.74's CMD_SOLVER_MANIFOLD_SETUP as the game's task does it, read from
// the lifted image (spu_0004: the command table at LS 0x43AC, command 2 at
// 0x53D4-0x8160, computeJacobianInverse 0x3970, setupSpuBody 0x3300, the DMA
// helper 0xA160) and checked byte for byte on four captured steps (sv3_1,
// sv3_2, sv4_150, sv4_300: tests/native/bullet_solver_snapcheck.cpp,
// setup-manifolds; c2r1 against c4r1).
//
// Descriptor: +0x10 hash, +0x14 solver bodies, +0x18 contact rows, +0x20 body
// index lists, +0x30 first cell, +0x34 cells, +0x38 bodies (static solver
// bodies go past them), +0x3C (2.74: the manifold count; the contact path does
// not read it), +0x40 manifold holders (16 bytes
// each, the manifold's address at +4), +0x44 constraint holders, +0x48 the
// solver info (btContactSolverInfoData: +0x54 time step, +0x68 erp, +0x80
// warm-starting factor). Hash cell (16 bytes, u16 each): +0 local bodies, +2
// index list offset, +4 manifolds, +6 manifold offset, +10 contact-row offset
// (in rows), +12 typed constraints, +14 their offset. btPersistentManifold
// 0x2E0 bytes (points 0xB0 apart: +0x20 world point on B, +0x30 on A, +0x40
// normal on B (4 words), +0x50 distance, +0x54 friction, +0x58 restitution,
// +0x70 applied impulse; +0x2C0/+0x2C4 bodies, +0x2C8 points). Rigid body
// (0x1F0 bytes copied in): +0x40 origin, +0xC0 island tag, +0xC4 companion
// id, +0xDC internal type (2 rigid body), +0x100/+0x110/+0x120 the world
// inverse inertia tensor's rows, +0x130/+0x140 linear/angular velocity, +0x150
// inverse mass, +0x154 angular factor.
//
// Per cell of the task's range, in order: the manifolds in packages of 8
// (holders, then each manifold, copied into local store); per manifold with
// points, both rigid bodies copied in; a body with island tag >= 0 brings its
// solver body (companion id); otherwise setupSpuBody makes a static one (deltas
// 0, +0x20 angular factor, +0x24 inverse mass, +0x2C the body copy's local
// store address -- for a non-rigid object 1, 0, 0 -- +0x28 and +0x30.. left as
// the local copy had them) with the id bodies + the cell's manifold offset.
// Both ids go into the cell's index set (the u32 list in local store). Per
// point three rows of 0x90 (the contact, two friction rows) are built in one
// local buffer and put whole: fields no command-2 step writes (+0x50, +0x70,
// +0x74, +0x8C; +0x6C of the friction rows) keep what the buffer held -- zero,
// the task's local store being fresh. After the points, both solver bodies are
// put back (so the warm-start impulses of a manifold reach the next one's
// copy); after the cell, the index list (exactly 4 x bodies bytes) and the
// cell with +0 = the bodies and +12 = 0.
//
// The arithmetic, in the SPU's single precision (truncation, denormals as zero:
// the runtime's helpers) and in the task's order: dot(a, b) = a.z*b.z + (a.x*b.x
// + a.y*b.y) fused twice; cross(a, b).x = a.y*b.z - (a.z*b.y) fused, likewise y
// and z; a row of the tensor times a vector by dot; 1/x and a/x as frest, fi
// and one Newton step; sqrt as frsqest, fi, s = y*x, s + (1.0000001 - y*s)*s/2.
// The contact row: rel_pos = point - origin; normal the point's (all four
// words); relpos x normal; angular components = tensor rows . (relpos x
// normal); 1/jacobian = 1/(inverse mass + n . (angular x relpos), the mass added
// before the z term, for both bodies); the velocity at the point from the
// solver body as getVelocityInLocalPointObsolete has it (see below); rest =
// max(-(vel . n) * restitution, 0); impulse = point's impulse x warm-start
// factor, applied to both solver bodies (linear: impulse x (inverse mass x n),
// the rigid body's mass; angular: (impulse x angular factor) x angular
// component) unless zero; rhs = (-distance x erp / time step) x 1/jac + (rest -
// (u1 - u2)) x 1/jac, the first product fused, u = n . v + (relpos x n) . w
// with the rigid bodies' own velocities v, w; cfm 0, limits 0 and 1e10.
// Friction directions: t0 = vel - n x (vel . n) normalised and t1 = t0 x n
// normalised (x 1/sqrt), or btPlaneSpace1 when |t0|^2 <= FLT_EPSILON; their
// 1/jac from computeJacobianInverse (that one multiplies the vector by the
// tensor's columns), rhs = (u2 - u1) x 1/jac with t for n. Typed constraints
// (cell +12): not handled -- none in the steps seen; ManifoldSetupStats::typed
// counts them, and a caller has to leave such a task to the lifted image.
//
// getVelocityInLocalPointObsolete reads the rigid body through the solver
// body's +0x2C, a local-store address: for a static body the copy just made,
// but for a moving one the address command 1's task had its copy at (0xC800 +
// 0x1F0 x its place in the package) -- in this task that is the manifold
// copies, the index list, or zeroed store, so the "velocity" there is whatever
// those bytes are. It decides the friction directions (and the restitution
// term): zeroing those reads changes 8 points' friction rows in each of sv4_150
// and sv4_300. The native task keeps those local-store regions (0xBC60-0x10A00)
// as the task fills them, so it reads the same bytes; the rest is the zero of a
// fresh store (the runtime gives each SPURS task a calloc'd local store with the
// image loaded, spu_workload.c, unless LBP_PERSIST_LS is set).
namespace {
constexpr uint32_t kDescStartCell = 0x30, kDescNumCells = 0x34, kDescNumBodies = 0x38, kDescHolders = 0x40;
constexpr uint32_t kDescTimeStep = 0x54, kDescErp = 0x68, kDescWarmstart = 0x80;
constexpr uint32_t kCellManifolds = 4, kCellManifoldOffset = 6, kCellRowOffset = 10;
constexpr uint32_t kManifoldBytes = 0x2E0, kPointBytes = 0xB0, kManifoldsPerPackage = 8;
constexpr uint32_t kManifoldBody0 = 0x2C0, kManifoldBody1 = 0x2C4, kManifoldPoints = 0x2C8;
constexpr uint32_t kPtPosB = 0x20, kPtPosA = 0x30, kPtNormal = 0x40, kPtDistance = 0x50, kPtFriction = 0x54;
constexpr uint32_t kPtRestitution = 0x58, kPtImpulse = 0x70;
constexpr uint32_t kRbOrigin = 0x40, kRbIslandTag = 0xC0, kRbInternalType = 0xDC, kRbInvInertia = 0x100;
// the task's local store: the two rigid-body copies, the temporary storage
// (holders, manifolds, constraint holders and list, the index list)
constexpr uint32_t kLsRbA = 0xBC60, kLsRbB = 0xBE50, kLsHolders = 0xC400, kLsManifolds = 0xC480, kLsIndex = 0xFA00;
constexpr uint32_t kLsEnd = 0x10A00, kLsStack = 0x3E000, kLsTempUsed = 0x31C00;
constexpr uint32_t kOne = 0x3F800000u, kOneUp = 0x3F800001u, kHalf = 0x3F000000u, kBig = 0x501502F9u;   // 1e10
constexpr uint32_t kEpsilon = 0x34000000u, kSqrt12 = 0x3F3504F3u;
constexpr uint32_t kRowWords = kConstraintBytes / 4;

inline uint32_t sp_fms(uint32_t x, uint32_t y, uint32_t z) { return spu__fma_any(x, y, z, 0, kSign); }   // x*y - z
inline uint32_t lane0(u128 v) { return v._u32[0]; }
inline u128 quad(uint32_t x) { u128 v; v._u32[0] = x; return v; }
// a / b as the task divides: y = fi(b, frest(b)), q = a*y, q + (a - q*b)*y
inline uint32_t sp_div(uint32_t a, uint32_t b)
{
    const uint32_t y = lane0(spu_fi_ref(quad(b), spu_frest_ref(quad(b))));
    const uint32_t q = sp_fm(a, y);
    return sp_fma(sp_fnms(q, b, a), y, q);
}
// sqrt(x) as the task takes it: y = fi(x, frsqest(x)), s = y*x, s + (1.0000001 - y*s)*(s*0.5)
inline uint32_t sp_sqrt(uint32_t x)
{
    const uint32_t y = lane0(spu_fi_ref(quad(x), spu_frsqest_ref(quad(x))));
    const uint32_t s = sp_fm(y, x);
    return sp_fma(sp_fnms(y, s, kOneUp), sp_fm(s, kHalf), s);
}

struct V3 { uint32_t x, y, z; };
inline V3 cross(V3 a, V3 b)
{
    return { sp_fms(a.y, b.z, sp_fm(a.z, b.y)), sp_fms(a.z, b.x, sp_fm(a.x, b.z)), sp_fms(a.x, b.y, sp_fm(a.y, b.x)) };
}
inline uint32_t dot(V3 a, V3 b) { return sp_fma(a.z, b.z, sp_fma(a.x, b.x, sp_fm(a.y, b.y))); }
inline V3 v3_of(const uint32_t* w) { return { w[0], w[1], w[2] }; }
inline V3 sub(V3 a, V3 b) { return { sp_fs(a.x, b.x), sp_fs(a.y, b.y), sp_fs(a.z, b.z) }; }
inline V3 add(V3 a, V3 b) { return { sp_fa(a.x, b.x), sp_fa(a.y, b.y), sp_fa(a.z, b.z) }; }
inline V3 scale(V3 a, uint32_t s) { return { sp_fm(a.x, s), sp_fm(a.y, s), sp_fm(a.z, s) }; }
// inverse mass + n . w, the mass added before the z term (computeImpulseDenominator)
inline uint32_t denominator(V3 n, V3 w, uint32_t inv_mass)
{
    return sp_fma(n.z, w.z, sp_fa(sp_fma(n.x, w.x, sp_fm(n.y, w.y)), inv_mass));
}
inline void put3(uint32_t* w, V3 v, uint32_t fourth) { w[0] = v.x; w[1] = v.y; w[2] = v.z; w[3] = fourth; }

// The local store regions the task fills that a read through a solver body's
// +0x2C can reach, as big-endian bytes; everything above them up to the stack
// is the fresh store's zero.
struct SetupLs {
    uint8_t m[kLsEnd - kLsRbA];
    unsigned unmodeled = 0;
    void clear() { memset(m, 0, sizeof m); unmodeled = 0; }
    void put(uint32_t ls, const uint8_t* src, uint32_t n) { memcpy(m + (ls - kLsRbA), src, n); }
    uint32_t w(uint32_t ls) const { return rd32(m, ls - kLsRbA); }
    void set(uint32_t ls, uint32_t v) { wr32(m, ls - kLsRbA, v); }
    V3 v3(uint32_t ls) const { return { w(ls), w(ls + 4), w(ls + 8) }; }
    // lqd (ptr + off) then rotqby ptr: the three words a vector load through
    // a pointer gets
    V3 through(uint32_t ptr, uint32_t off)
    {
        const uint32_t q = (ptr + off) & 0x3FFF0u, r = ptr & 15u;
        uint8_t b[16] = {};
        if (q >= kLsRbA && q + 16 <= kLsEnd) memcpy(b, m + (q - kLsRbA), 16);
        else if (q < kLsEnd || q >= kLsStack || q == kLsTempUsed) unmodeled++;
        uint32_t o[3];
        for (uint32_t k = 0; k < 3; k++) {
            uint8_t x[4];
            for (uint32_t j = 0; j < 4; j++) x[j] = b[(r + 4 * k + j) & 15];
            o[k] = rd32(x, 0);
        }
        return { o[0], o[1], o[2] };
    }
};

// setupSpuBody on a body copied into local store at `rb`
void setup_static_body(uint32_t* b, const SetupLs& ls, uint32_t rb)
{
    for (int k = 0; k < 8; k++) b[k] = 0;
    if (ls.w(rb + kRbInternalType) == 2) {
        b[8] = ls.w(rb + kRbAngularFactor);
        b[9] = ls.w(rb + kRbInverseMass);
        b[11] = rb;
    } else {
        b[8] = kOne;
        b[9] = 0;
        b[11] = 0;
    }
}

// getVelocityInLocalPointObsolete: (v + dv) + (w + dw) x rel_pos, v and w read
// through the solver body's +0x2C in local store
V3 point_velocity(SetupLs& ls, const uint32_t* b, V3 rel_pos)
{
    const uint32_t ptr = b[11];
    if (!ptr) return { 0, 0, 0 };
    const V3 ang = add(ls.through(ptr, kRbAngularVelocity), v3_of(b + kDeltaAngular / 4));
    const V3 lin = add(ls.through(ptr, kRbLinearVelocity), v3_of(b + kDeltaLinear / 4));
    return add(lin, cross(ang, rel_pos));
}

struct BodyCopy {   // a rigid body as the task reads it from its copy
    V3 origin, rows[3], lin, ang;
    uint32_t inv_mass;
    void load(const SetupLs& ls, uint32_t rb)
    {
        origin = ls.v3(rb + kRbOrigin);
        for (int i = 0; i < 3; i++) rows[i] = ls.v3(rb + kRbInvInertia + 16 * i);
        lin = ls.v3(rb + kRbLinearVelocity);
        ang = ls.v3(rb + kRbAngularVelocity);
        inv_mass = ls.w(rb + kRbInverseMass);
    }
    V3 times_rows(V3 v) const { return { dot(rows[0], v), dot(rows[1], v), dot(rows[2], v) }; }
    V3 times_columns(V3 v) const
    {
        return { dot(v, { rows[0].x, rows[1].x, rows[2].x }), dot(v, { rows[0].y, rows[1].y, rows[2].y }),
                 dot(v, { rows[0].z, rows[1].z, rows[2].z }) };
    }
};

// computeJacobianInverse (SPU 0x3970)
uint32_t jacobian_inverse(const BodyCopy& b0, const BodyCopy& b1, V3 pos_a, V3 pos_b, V3 n)
{
    const V3 r0 = sub(pos_a, b0.origin), r1 = sub(pos_b, b1.origin);
    const V3 w0 = cross(b0.times_columns(cross(r0, n)), r0);
    const V3 w1 = cross(b1.times_columns(cross(r1, n)), r1);
    return sp_div(kOne, sp_fa(denominator(n, w0, b0.inv_mass), denominator(n, w1, b1.inv_mass)));
}

// btSolverBody::applyImpulse without the inverse-mass check, the linear part
// with the rigid body's inverse mass
void warm_start(uint32_t* b, V3 n, uint32_t inv_mass, const uint32_t* angular, uint32_t impulse)
{
    const V3 lc = scale(n, inv_mass);
    const uint32_t t = sp_fm(impulse, b[kAngularFactor / 4]);
    const uint32_t l[3] = { lc.x, lc.y, lc.z };
    for (int k = 0; k < 3; k++) {
        b[kDeltaLinear / 4 + k] = sp_fa(b[kDeltaLinear / 4 + k], sp_fm(impulse, l[k]));
        b[kDeltaAngular / 4 + k] = sp_fa(b[kDeltaAngular / 4 + k], sp_fm(t, angular[k]));
    }
}

// the two friction rows' common part
void friction_row(uint32_t* r, V3 t, uint32_t offs_a, uint32_t offs_b, uint32_t friction, const BodyCopy& b0,
                  const BodyCopy& b1, V3 pos_a, V3 pos_b, V3 rel1, V3 rel2)
{
    put3(r + kRowNormal / 4, t, 0);
    r[kRowBodyA / 4] = offs_a;
    r[kContactPoint / 4] = 0;
    r[kRowBodyB / 4] = offs_b;
    r[kRowFriction / 4] = friction;
    r[kAppliedImpulse / 4] = 0;
    const uint32_t jac = jacobian_inverse(b0, b1, pos_a, pos_b, t);
    r[kRowJacDiagInv / 4] = jac;
    const V3 c1 = cross(rel1, t), c2 = cross(rel2, t);
    put3(r + kRowRelpos1 / 4, c1, 0);
    put3(r + kRowAngularA / 4, b0.times_rows(c1), 0);
    put3(r + kRowRelpos2 / 4, c2, 0);
    put3(r + kRowAngularB / 4, b1.times_rows(c2), 0);
    r[0x64 / 4] = 0;
    const uint32_t v1 = sp_fa(dot(b0.lin, t), dot(c1, b0.ang)), v2 = sp_fa(dot(b1.lin, t), dot(c2, b1.ang));
    r[kRowRhs / 4] = sp_fm(sp_fs(v2, v1), jac);
    r[kRowCfm / 4] = 0;
    r[kRowLower / 4] = 0;
    r[kRowUpper / 4] = kBig;
}

}  // namespace

void setup_manifolds_task(uint8_t* base, uint32_t desc, ManifoldSetupStats& st, ManifoldSetupSpans* spans)
{
    const uint32_t hash = rd32(base, desc + kDescHash), sbl = rd32(base, desc + kDescBodies);
    const uint32_t rows = rd32(base, desc + kDescContacts), lists = rd32(base, desc + kDescBodyOffsets);
    const uint32_t first = rd32(base, desc + kDescStartCell), ncells = rd32(base, desc + kDescNumCells);
    const uint32_t nbodies = rd32(base, desc + kDescNumBodies), holders = rd32(base, desc + kDescHolders);
    const uint32_t time_step = rd32(base, desc + kDescTimeStep), erp = rd32(base, desc + kDescErp);
    const uint32_t warm = rd32(base, desc + kDescWarmstart);
    // the hash as the task copies it in at its start
    uint8_t cells[kCells * 16];
    memcpy(cells, base + hash + kHashCells, sizeof cells);
    thread_local SetupLs ls;
    ls.clear();
    uint32_t body[2][16] = {};          // the task's two solver-body buffers
    uint32_t row[3][kRowWords] = {};    // its three-row buffer
    for (uint32_t c = 0; c < ncells && first + c < kCells; c++) {
        const uint32_t ci = first + c;
        uint8_t* cell = cells + 16 * ci;
        const uint32_t nman = rd16(cell, kCellManifolds), moff = rd16(cell, kCellManifoldOffset);
        uint32_t rindex = rd16(cell, kCellRowOffset), nlocal = 0;
        st.cells++;
        for (uint32_t done = 0; done < nman;) {
            const uint32_t pkg = nman - done < kManifoldsPerPackage ? nman - done : kManifoldsPerPackage;
            ls.put(kLsHolders, base + holders + 16 * (moff + done), 16 * pkg);
            for (uint32_t m = 0; m < pkg; m++)
                ls.put(kLsManifolds + kManifoldBytes * m, base + ls.w(kLsHolders + 16 * m + 4), kManifoldBytes);
            for (uint32_t m = 0; m < pkg; m++) {
                const uint32_t mls = kLsManifolds + kManifoldBytes * m, mea = ls.w(kLsHolders + 16 * m + 4);
                const uint32_t np = ls.w(mls + kManifoldPoints);
                if (!np) continue;
                st.manifolds++;
                ls.put(kLsRbA, base + ls.w(mls + kManifoldBody0), kRbSize);
                ls.put(kLsRbB, base + ls.w(mls + kManifoldBody1), kRbSize);
                uint32_t id[2], offs[2];
                for (int k = 0; k < 2; k++) {
                    const uint32_t rb = k ? kLsRbB : kLsRbA;
                    if ((int32_t)ls.w(rb + kRbIslandTag) > -1) {
                        id[k] = ls.w(rb + kRbCompanionId);
                        load_words(base, sbl + kSolverBodyBytes * id[k], body[k], 16);
                    } else {
                        id[k] = nbodies + moff;
                        setup_static_body(body[k], ls, rb);
                        st.static_bodies++;
                    }
                }
                for (int k = 0; k < 2; k++) {   // SpuIndexSet::insert
                    uint32_t i = 0;
                    while (i < nlocal && ls.w(kLsIndex + 4 * i) != id[k]) i++;
                    if (i == nlocal) ls.set(kLsIndex + 4 * nlocal++, id[k]);
                    offs[k] = i;
                }
                BodyCopy b0, b1;
                b0.load(ls, kLsRbA);
                b1.load(ls, kLsRbB);
                for (uint32_t j = 0; j < np; j++) {
                    const uint32_t pt = mls + kPointBytes * j;
                    const V3 pos_a = ls.v3(pt + kPtPosA), pos_b = ls.v3(pt + kPtPosB), n = ls.v3(pt + kPtNormal);
                    const V3 rel1 = sub(pos_a, b0.origin), rel2 = sub(pos_b, b1.origin);
                    const uint32_t friction = ls.w(pt + kPtFriction);
                    uint32_t* r = row[0];
                    // the contact row
                    r[kRowBodyA / 4] = offs[0];
                    r[kRowBodyB / 4] = offs[1];
                    r[kContactPoint / 4] = mea + kPtImpulse + kPointBytes * j;
                    put3(r + kRowNormal / 4, n, ls.w(pt + kPtNormal + 12));
                    const V3 c1 = cross(rel1, n), c2 = cross(rel2, n);
                    put3(r + kRowRelpos1 / 4, c1, 0);
                    put3(r + kRowRelpos2 / 4, c2, 0);
                    const V3 ang_a = b0.times_rows(c1), ang_b = b1.times_rows(c2);
                    put3(r + kRowAngularA / 4, ang_a, 0);
                    put3(r + kRowAngularB / 4, ang_b, 0);
                    const uint32_t d0 = denominator(n, cross(ang_a, rel1), b0.inv_mass);
                    const uint32_t d1 = denominator(n, cross(ang_b, rel2), b1.inv_mass);
                    const uint32_t jac = sp_div(kOne, sp_fa(d0, d1));
                    r[kRowJacDiagInv / 4] = jac;
                    const V3 vel = sub(point_velocity(ls, body[0], rel1), point_velocity(ls, body[1], rel2));
                    const uint32_t distance = ls.w(pt + kPtDistance);
                    r[0x6C / 4] = distance;
                    r[kRowFriction / 4] = friction;
                    const uint32_t rel_vel = dot(vel, n);
                    uint32_t rest = sp_fm(rel_vel ^ kSign, ls.w(pt + kPtRestitution));
                    if (sp_fcgt(0, rest)) rest = 0;
                    r[0x64 / 4] = rest;
                    const uint32_t impulse = sp_fm(ls.w(pt + kPtImpulse), warm);
                    r[kAppliedImpulse / 4] = impulse;
                    if (!sp_zero(impulse)) {
                        warm_start(body[0], n, b0.inv_mass, r + kRowAngularA / 4, impulse);
                        warm_start(body[1], n, b1.inv_mass, r + kRowAngularB / 4, impulse ^ kSign);
                    }
                    const uint32_t v1 = sp_fa(dot(b0.lin, n), dot(c1, b0.ang)), v2 = sp_fa(dot(b1.lin, n), dot(c2, b1.ang));
                    const uint32_t positional = sp_div(sp_fm(erp, distance ^ kSign), time_step);
                    r[kRowRhs / 4] = sp_fma(positional, jac, sp_fm(sp_fs(rest, sp_fs(v1, v2)), jac));
                    r[kRowCfm / 4] = 0;
                    r[kRowLower / 4] = 0;
                    r[kRowUpper / 4] = kBig;
                    // the friction directions
                    V3 t0 = sub(vel, scale(n, rel_vel)), t1;
                    const uint32_t lat = dot(t0, t0);
                    if (sp_fcgt(lat, kEpsilon)) {
                        t0 = scale(t0, sp_div(kOne, sp_sqrt(lat)));
                        t1 = cross(t0, n);
                        t1 = scale(t1, sp_div(kOne, sp_sqrt(dot(t1, t1))));
                    } else if (sp_fcgt(n.z & ~kSign, kSqrt12)) {   // btPlaneSpace1
                        const uint32_t a = sp_fma(n.y, n.y, sp_fm(n.z, n.z)), k = sp_div(kOne, sp_sqrt(a));
                        t0 = { 0, sp_fm(k, n.z ^ kSign), sp_fm(k, n.y) };
                        t1 = { sp_fm(a, k), sp_fm(n.x ^ kSign, t0.z), sp_fm(n.x, t0.y) };
                        st.plane_space++;
                    } else {
                        const uint32_t a = sp_fma(n.x, n.x, sp_fm(n.y, n.y)), k = sp_div(kOne, sp_sqrt(a));
                        t0 = { sp_fm(k, n.y ^ kSign), sp_fm(k, n.x), 0 };
                        t1 = { sp_fm(n.z ^ kSign, t0.y), sp_fm(n.z, t0.x), sp_fm(a, k) };
                        st.plane_space++;
                    }
                    friction_row(row[1], t0, offs[0], offs[1], friction, b0, b1, pos_a, pos_b, rel1, rel2);
                    friction_row(row[2], t1, offs[0], offs[1], friction, b0, b1, pos_a, pos_b, rel1, rel2);
                    const uint32_t ea = rows + (rindex + 3 * j) * kConstraintBytes;
                    store_words(base, ea, row[0], 3 * kRowWords);
                    if (spans) spans->rows.push_back({ ea, 3 * kConstraintBytes });
                    st.contacts++;
                }
                rindex += 3 * np;
                for (int k = 0; k < 2; k++) {
                    store_words(base, sbl + kSolverBodyBytes * id[k], body[k], 16);
                    if (spans) spans->bodies.push_back({ sbl + kSolverBodyBytes * id[k], kSolverBodyBytes });
                }
            }
            done += pkg;
        }
        if (rd16(cell, kCellConstraints)) st.typed++;   // typed constraints: none in the game's steps seen
        if (nlocal) {
            const uint32_t list = lists + 4 * rd16(cell, kCellBodyList);
            memcpy(base + list, ls.m + (kLsIndex - kLsRbA), 4 * nlocal);
            cell[0] = (uint8_t)(nlocal >> 8); cell[1] = (uint8_t)nlocal;
            cell[12] = 0; cell[13] = 0;
            memcpy(base + hash + kHashCells + 16 * ci, cell, 16);
            if (spans) {
                spans->lists.push_back({ list, 4 * nlocal });
                spans->cells.push_back({ hash + kHashCells + 16 * ci, 16 });
            }
        }
    }
    st.unmodeled += ls.unmodeled;
}

bool setup_manifolds_supported(const uint8_t* base, uint32_t desc)
{
    const uint32_t hash = rd32(base, desc + 0x10), first = rd32(base, desc + 0x30), n = rd32(base, desc + 0x34);
    if (first >= 128 || n > 128 - first) return false;
    for (uint32_t c = first; c < first + n; c++)
        if (rd16(base, hash + 0x870 + 16 * c + 12)) return false;   // typed constraints
    return true;
}

}  // namespace bullet_solver
