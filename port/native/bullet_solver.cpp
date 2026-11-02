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

}  // namespace bullet_solver
