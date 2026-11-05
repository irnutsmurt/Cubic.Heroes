// R3's recorder (docs/NATIVE_R3.md): PPFX's libgcm calls, seen at their
// entry by the development lift's observers (r1_observe.cpp dispatches here),
// made into native records from the calls' own arguments -- no FIFO
// decoding, no reconstructed RSX state.
//
// The state block. Each libgcm entry point PPFX reaches has an encoder here:
// from the call's arguments (and the structures they point to: a
// CellGcmSurface, a CellGcmTexture, a CgBinaryProgram) it writes the values
// that call means into the recorder's own block, laid out like the RSX
// register file because that is the layout the shader translators, the
// texture and vertex decoders and the render-state decode read. Nothing else
// writes the block; the existing renderer's registers are never read here
// (the bridge compares against them in development runs, as a check only).
// The encodings were taken from a frame trace of what each call writes
// (tools/r3_gt_encodings.py) and are checked at every PPFX draw by the
// bridge's cross-check.
//
// Which calls are PPFX's. A call whose return address lies in PPFX's code is;
// a call from inside libgcm (a libgcm function calling another, as
// cellGcmSetFragmentProgram does the bind and the shader control) is PPFX's
// when the first return address outside libgcm, up the guest's stack back
// chain, lies in PPFX's code.
//
// Program memory. The game uploads fragment programs into RSX local memory,
// and PPFX patches their constants, through inline transfers queued in the
// command stream (cellGcmSetInlineTransfer): the walker applies them when it
// reaches them, later than the call. The recorder applies them itself, in
// call order, to a shadow of local memory, and takes a program's base bytes
// from the CgBinaryProgram the bind names (its microcode in main memory: the
// bytes the game's memcpy put in local memory before the first bind,
// docs/NATIVE_R3.md). So a program's bytes at its bind are exactly what the
// GPU will consume there, whatever the walker has applied so far.
//
// Ordering. Each record carries a monotonic sequence number and where its
// commands went: the title's ring lap (cellGcm_title_laps_pub, counted where
// the runtime writes the jump back) and the IO offset of its first command.
// That pair does not repeat; the bridge matches it against the walker's own
// position and lap. A record is published at its call, before the call's
// commands exist, so the walker can never reach them first; a call whose
// reserve wraps the ring puts them at the start of the next lap instead, and
// the record names that position too.
//
// DOTHEROES_PPFX_NATIVE=check: records, the bridge's match and cross-check;
// =1: and the native renderer. DOTHEROES_PPFX_PROGWATCH=<file> (a
// diagnostic): every fragment-program bind's bytes three ways -- main memory,
// local memory now, the shadow.

#include "ppu_context.h"
#include "ppfx_native.h"
#include "rsx_dispatch.h"
#include "rsx_texture_layout.h"

#include <chrono>
#include <map>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" uint8_t* vm_base;
extern "C" uint32_t cellGcm_title_laps_pub(uint32_t* jump_io);
extern "C" uint32_t cellGcm_ea2io_pub(uint32_t ea);
extern "C" uint32_t cellGcmResolveLocated(int local, uint32_t offset);
extern "C" uint32_t rsx_fp_texture_units(const uint8_t* ucode, uint32_t max_bytes);
extern "C" uint32_t rsx_fp_program_size(const uint8_t* ucode, uint32_t max_bytes);
extern "C" uint32_t rsx_vp_program_size_instrs(const uint8_t* ucode, uint32_t max_bytes);

namespace ppfx {

// ---- shared definitions -----------------------------------------------------

const RegRange kRanges[] = {
    { 0x0180, 0x0400 },   // DMA objects, surface, the 0x03xx state (blend, alpha, stencil, masks)
    { 0x08C0, 0x0900 },   // scissor, shader program
    { 0x0900, 0x0980 },   // vertex texture units (never PPFX's; their enables shape the vertex program)
    { 0x0A00, 0x0B00 },   // viewport, depth, polygon offset
    { 0x0B40, 0x0B80 },   // texture coordinate control
    { 0x1400, 0x1500 },   // 0x142C, user clip planes 0x1478, 0x147C
    { 0x1680, 0x1880 },   // vertex arrays, base offset/index, polygon modes, cull, texture pitch/depth
    { 0x1A00, 0x1C00 },   // texture units
    { 0x1C00, 0x1D00 },   // vertex attribute values (what a disabled array reads)
    { 0x1D60, 0x1E00 },   // shader control, shader window, clear values, clear
    { 0x1E98, 0x1F00 },   // transform program load/start, timeout, constant load
    { 0x1FC0, 0x2000 },   // frequency divider, gamma, attribute masks, branch bits
};
const uint32_t kNRanges = sizeof kRanges / sizeof kRanges[0];

namespace {
uint16_t s_idx[0x2000 / 4];
uint16_t s_meth[kMaxRegs];
uint32_t s_nregs;
void init_index()
{
    if (s_nregs) return;
    for (auto& v : s_idx) v = 0xFFFF;
    for (uint32_t r = 0; r < kNRanges; r++)
        for (uint32_t m = kRanges[r].lo; m < kRanges[r].hi; m += 4) {
            s_idx[m >> 2] = (uint16_t)s_nregs;
            s_meth[s_nregs++] = (uint16_t)m;
        }
}
}  // namespace

uint32_t reg_count() { init_index(); return s_nregs; }
uint32_t reg_index(uint32_t m) { init_index(); return m < 0x2000 && s_idx[m >> 2] != 0xFFFF ? s_idx[m >> 2] : ~0u; }
uint32_t reg_method(uint32_t i) { init_index(); return i < s_nregs ? s_meth[i] : 0; }

uint64_t fnv(const void* p, size_t n, uint64_t h)
{
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

Queue& queue() { static Queue q; return q; }
Stats& stats() { static Stats s; return s; }

int mode()
{
    static int m = -1;
    if (m < 0) {
        const char* e = getenv("DOTHEROES_PPFX_NATIVE");
        m = !e || !*e ? 0 : !strcmp(e, "check") ? 1 : e[0] == '1' ? 2 : 0;
    }
    return m;
}

namespace {

const uint32_t kLocalEa = 0xC0000000u;
const uint32_t kPpfxLo = 0x007497E0u, kPpfxHi = 0x007D3E38u;   // PPFX's code
const uint32_t kGcmLo = 0x0069C000u, kGcmHi = 0x006B0000u;     // libgcm's

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint32_t g32(uint32_t ea) { return be32(vm_base + ea); }
uint32_t g16(uint32_t ea) { return (uint32_t)vm_base[ea] << 8 | vm_base[ea + 1]; }
uint32_t g8(uint32_t ea) { return vm_base[ea]; }
bool ea_ok(uint32_t ea, uint32_t n = 4) { return ea >= 0x10000u && (uint64_t)ea + n <= 0xE0000000ull; }
uint32_t fbits(double d) { float f = (float)d; uint32_t u; memcpy(&u, &f, 4); return u; }
uint32_t flog2(uint32_t v) { uint32_t r = 0; while (v > 1) { v >>= 1; r++; } return r; }

double now_s()
{
#ifdef _WIN32
    FILETIME c, e, k, u, t;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    GetSystemTimeAsFileTime(&t);
    return (double)((((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime) -
                    (((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime)) * 1e-7;
#else
    return 0;
#endif
}

uint64_t now_ns()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- the program shadow -----------------------------------------------------
// Local memory's bytes as the calls made them: per word, the bytes and the
// sequence number of the transfer that wrote it last (0: none).

struct ShadowPage { uint8_t b[4096]; uint64_t ep[1024]; };
std::unordered_map<uint32_t, ShadowPage*> s_shadow;
uint64_t s_epoch;   // transfers so far

ShadowPage* shadow_page(uint32_t off)
{
    ShadowPage*& p = s_shadow[off >> 12];
    if (!p) { p = new ShadowPage; memset(p, 0, sizeof *p); }
    return p;
}

void shadow_write(uint32_t off, const uint8_t* src, uint32_t n, uint64_t ep)
{
    for (uint32_t i = 0; i + 4 <= n; i += 4) {
        ShadowPage* p = shadow_page(off + i);
        memcpy(p->b + ((off + i) & 0xFFF), src + i, 4);
        p->ep[((off + i) & 0xFFF) >> 2] = ep;
    }
}

// Program memory at a bind. `since`: words a transfer wrote after it are the
// new program's own (patched just before this bind); with `fresh` the other
// words take the program's base bytes, else the shadow keeps what it has.
struct Bound { uint32_t prog; uint64_t epoch; };
std::unordered_map<uint32_t, Bound> s_bound;   // per local offset: the program bound there last

void program_bytes(uint32_t off, uint32_t prog, std::vector<uint8_t>& out, uint32_t* words_from_transfers)
{
    const uint32_t usize = g32(prog + 24), uoff = g32(prog + 28);
    Bound& b = s_bound[off];
    const bool fresh = b.prog != prog;   // a program the game put at this offset since (or the first bind)
    const uint64_t since = fresh ? b.epoch : 0;
    // The base: the microcode the CgBinaryProgram carries; the patched words
    // over it. Read to the program's end as the RSX finds it (the END bit),
    // at most its ucodeSize.
    std::vector<uint8_t> buf(usize ? usize : 16);
    uint32_t known = 0;
    for (uint32_t i = 0; i + 4 <= usize; i += 4) {
        ShadowPage* p = shadow_page(off + i);
        uint8_t* w = p->b + ((off + i) & 0xFFF);
        uint64_t& ep = p->ep[((off + i) & 0xFFF) >> 2];
        if (ep && (!fresh || ep > since)) { memcpy(&buf[i], w, 4); known++; }
        else {
            memcpy(&buf[i], vm_base + prog + uoff + i, 4);
            memcpy(w, &buf[i], 4);
            ep = 0;   // the base word: a later fresh bind takes its own base
        }
    }
    const uint32_t size = rsx_fp_program_size(buf.data(), (uint32_t)buf.size());
    buf.resize(size ? size : buf.size());
    out.swap(buf);
    b.prog = prog;
    b.epoch = s_epoch;
    if (words_from_transfers) *words_from_transfers = known;
}

// ---- the program watch (a diagnostic) ----------------------------------------

FILE* s_pw;
struct PwSeen { uint64_t main_h, local_h, shadow_h; uint32_t known, words; uint64_t binds; };
std::unordered_map<uint32_t, PwSeen> s_pw_seen;

void progwatch_bind(uint32_t prog, uint32_t off, const std::vector<uint8_t>& sh, uint32_t known, uint32_t flips)
{
    const uint32_t size = (uint32_t)sh.size(), uoff = g32(prog + 28);
    uint32_t d_ml = 0, d_ms = 0, d_ls = 0;
    const uint8_t* m = vm_base + prog + uoff;
    const uint8_t* l = vm_base + kLocalEa + off;
    for (uint32_t i = 0; i + 4 <= size; i += 4) {
        d_ml += memcmp(m + i, l + i, 4) != 0;
        d_ms += memcmp(m + i, &sh[i], 4) != 0;
        d_ls += memcmp(l + i, &sh[i], 4) != 0;
    }
    PwSeen now = { fnv(m, size), fnv(l, size), fnv(sh.data(), size), known, size / 4, 0 };
    PwSeen& was = s_pw_seen[off];
    const bool changed = was.binds == 0 || was.main_h != now.main_h || was.shadow_h != now.shadow_h ||
                         was.known != now.known;
    now.binds = was.binds + 1;
    was = now;
    if (changed)
        fprintf(s_pw, "B %u %.3f off=%08X prog=%08X words=%u known=%u main=%016llX local=%016llX shadow=%016llX "
                      "differ main/local=%u main/shadow=%u local/shadow=%u\n",
                flips, now_s(), off, prog, size / 4, known, (unsigned long long)now.main_h,
                (unsigned long long)now.local_h, (unsigned long long)now.shadow_h, d_ml, d_ms, d_ls);
}

// ---- the recorder's state ------------------------------------------------------

struct State {
    uint32_t regs[0x2000 / 4];
    uint8_t written[0x2000 / 4];     // the recorder has written this register
    uint32_t vconst[512][4];
    uint32_t vconst_written[16];
    uint32_t vp[kVpWords];
    uint32_t vp_load;                // 1E9C: the next upload's slot
    BlobP fp; uint32_t fp_off, fp_loc;
    BlobP vp_blob; uint64_t vp_gen, vp_blob_gen; uint32_t vp_blob_start;
    uint32_t fp_units;               // texture units the bound fragment program samples
    std::unordered_set<uint64_t> ppfx_targets;   // (location << 32 | offset) a record has written
    uint32_t frame;
    // the producer
    uint32_t tid;
} S;

// Calls on other threads than the producer's touch the shadow (an inline
// transfer by other code) and the frame count; the lock keeps them apart. It
// is released while the producer waits for room in the queue.
std::mutex s_lock;
std::unique_lock<std::mutex>* s_held;   // the producer's hold on it, during a call
std::mutex s_unm_lock;                   // s_unmodelled_by, which the report reads
std::unordered_map<uint64_t, BlobP> s_blobs;   // programs by content

BlobP blob_of(const uint8_t* p, size_t n)
{
    const uint64_t h = fnv(p, n) ^ (uint64_t)n;
    auto it = s_blobs.find(h);
    if (it != s_blobs.end() && it->second->bytes.size() == n && !memcmp(it->second->bytes.data(), p, n))
        return it->second;
    auto b = std::make_shared<Blob>();
    b->hash = h;
    b->bytes.assign(p, p + n);
    if (s_blobs.size() > 4096) s_blobs.clear();   // bounded: records keep theirs alive
    s_blobs[h] = b;
    return b;
}

void W(uint32_t m, uint32_t v)
{
    S.regs[m >> 2] = v;
    S.written[m >> 2] = 1;
}

// ---- encoders ----------------------------------------------------------------

// cellGcmSetSurface(ctx, const CellGcmSurface*)
void enc_surface(uint32_t s)
{
    if (!ea_ok(s, 60)) return;
    const uint32_t type = g8(s + 0), aa = g8(s + 1), cfmt = g8(s + 2), ctarget = g8(s + 3);
    uint32_t cloc[4], coff[4], cpitch[4];
    for (int i = 0; i < 4; i++) { cloc[i] = g8(s + 4 + i); coff[i] = g32(s + 8 + 4 * i); cpitch[i] = g32(s + 24 + 4 * i); }
    const uint32_t dfmt = g8(s + 40), dloc = g8(s + 41), doff = g32(s + 44), dpitch = g32(s + 48);
    const uint32_t w = g16(s + 52), h = g16(s + 54), x = g16(s + 56), y = g16(s + 58);
    W(0x0194, 0xFEED0000u + cloc[0]);
    W(0x018C, 0xFEED0000u + cloc[1]);
    W(0x01B4, 0xFEED0000u + cloc[2]);
    W(0x01B8, 0xFEED0000u + cloc[3]);
    W(0x0198, 0xFEED0000u + dloc);
    W(0x0208, cfmt | dfmt << 5 | type << 8 | aa << 12 | flog2(w) << 16 | flog2(h) << 24);
    W(0x020C, cpitch[0]); W(0x0210, coff[0]); W(0x0214, doff); W(0x0218, coff[1]); W(0x021C, cpitch[1]);
    W(0x022C, dpitch);
    W(0x0280, cpitch[2]); W(0x0284, cpitch[3]); W(0x0288, coff[2]); W(0x028C, coff[3]);
    W(0x0220, ctarget);
    W(0x02B8, y << 16 | x);
    W(0x0200, w << 16 | x);
    W(0x0204, h << 16 | y);
    W(0x1D88, h | 0x1000u);   // shader window: the height, origin bottom, centres at half
}

// cellGcmSetViewport(ctx, x, y, w, h, min, max, scale[4], offset[4]): the
// last pointer is the ninth argument, in the caller's parameter save area.
void enc_viewport(ppu_context* c)
{
    const uint32_t x = (uint32_t)c->gpr[4] & 0xFFFF, y = (uint32_t)c->gpr[5] & 0xFFFF;
    const uint32_t w = (uint32_t)c->gpr[6] & 0xFFFF, h = (uint32_t)c->gpr[7] & 0xFFFF;
    const uint32_t scale = (uint32_t)c->gpr[10];
    const uint32_t sp = (uint32_t)c->gpr[1];
    const uint32_t offset = ea_ok(sp + 112, 8) ? g32(sp + 112 + 4) : 0;
    W(0x0A00, w << 16 | x);
    W(0x0A04, h << 16 | y);
    W(0x0394, fbits(c->fpr[1]));
    W(0x0398, fbits(c->fpr[2]));
    for (int i = 0; i < 4; i++) {
        if (ea_ok(offset, 16)) W(0x0A20 + 4 * i, g32(offset + 4 * i));
        if (ea_ok(scale, 16)) W(0x0A30 + 4 * i, g32(scale + 4 * i));
    }
}

// cellGcmSetFragmentProgramOffset-like bind (the libgcm function
// cellGcmSetFragmentProgram calls): (ctx, CgBinaryProgram*, offset, location)
void enc_fp_bind(ppu_context* c, uint32_t flips)
{
    const uint32_t prog = (uint32_t)c->gpr[4], off = (uint32_t)c->gpr[5], loc = (uint32_t)c->gpr[6] & 1;
    if (!ea_ok(prog, 32)) return;
    W(0x08E4, off | (loc + 1));
    const uint32_t fpd = prog + g32(prog + 20);   // CgBinaryFragmentProgram
    if (ea_ok(fpd, 24)) {
        W(0x1FF4, g32(fpd + 4) | 0x20u);          // attributeInputMask, and bit 5 libgcm always sets
        const uint32_t tmask = g16(fpd + 12), t2d = g16(fpd + 14), tcen = g16(fpd + 16);
        for (uint32_t i = 0; i < 10; i++)
            if (tmask >> i & 1) W(0x0B40 + 4 * i, (t2d >> i & 1) | (tcen >> i & 1) << 4);
    }
    std::vector<uint8_t> bytes;
    uint32_t known = 0;
    if (loc == 0) program_bytes(off, prog, bytes, &known);
    else {   // a program in main memory: what is there
        const uint32_t usize = g32(prog + 24);
        const uint32_t ea = cellGcmResolveLocated(0, off);
        if (ea_ok(ea, usize)) bytes.assign(vm_base + ea, vm_base + ea + usize);
    }
    S.fp = bytes.empty() ? nullptr : blob_of(bytes.data(), bytes.size());
    S.fp_off = off; S.fp_loc = loc;
    S.fp_units = S.fp ? rsx_fp_texture_units(S.fp->bytes.data(), (uint32_t)S.fp->bytes.size()) : 0;
    if (s_pw && loc == 0) progwatch_bind(prog, off, bytes, known, flips);
}

// The shader control the fragment-program functions set: (ctx, prog, controlTxp, ...)
void enc_shader_control(ppu_context* c)
{
    const uint32_t prog = (uint32_t)c->gpr[4];
    if (!ea_ok(prog, 32)) return;
    const uint32_t fpd = prog + g32(prog + 20);
    if (!ea_ok(fpd, 24)) return;
    const uint32_t rc = g8(fpd + 18), h0 = g8(fpd + 19), depth = g8(fpd + 20), kill = g8(fpd + 21);
    const uint32_t txp = (uint32_t)c->gpr[5] & 1;
    W(0x1D60, (rc < 2 ? 2 : rc) << 24 | txp << 15 | 0x400u | (h0 ? 0 : 0x40u) | (depth ? 0xEu : 0) | (kill ? 0x80u : 0));
}

// cellGcmSetVertexProgram(ctx, CgBinaryProgram*, ucode)
void enc_vp(ppu_context* c)
{
    const uint32_t prog = (uint32_t)c->gpr[4], uc = (uint32_t)c->gpr[5];
    if (!ea_ok(prog, 32)) return;
    const uint32_t vpd = prog + g32(prog + 20);   // CgBinaryVertexProgram
    if (!ea_ok(vpd, 24)) return;
    const uint32_t n = g32(vpd + 0), slot = g32(vpd + 4), rc = g32(vpd + 8), in = g32(vpd + 12);
    W(0x1E9C, slot);
    W(0x1EA0, slot);
    S.vp_load = slot;
    if (ea_ok(uc, n * 16))
        for (uint32_t i = 0; i < n * 4 && slot * 4 + i < kVpWords; i++) S.vp[slot * 4 + i] = g32(uc + 4 * i);
    S.vp_load = slot + n;
    W(0x1FF0, in);
    W(0x1EF8, (rc <= 32 ? 0x20u : 0x30u) << 16 | 0xFFFFu);
    S.vp_gen++;
}

// cellGcmSetVertexProgramParameter(ctx, CgBinaryParameter*, const float*)
void enc_vp_const(ppu_context* c)
{
    const uint32_t par = (uint32_t)c->gpr[4], val = (uint32_t)c->gpr[5];
    if (!ea_ok(par, 16)) return;
    const uint32_t type = g32(par + 0), res = g32(par + 12);
    uint32_t regs = 1, comps = 4;
    switch (type) {
    case 0x415: comps = 1; break;        // CG_FLOAT
    case 0x416: comps = 2; break;        // CG_FLOAT2
    case 0x417: comps = 3; break;        // CG_FLOAT3
    case 0x418: comps = 4; break;        // CG_FLOAT4
    case 0x428: regs = 4; break;         // CG_FLOAT4x4
    default: stats().unmodelled++; break;
    }
    W(0x1EFC, res);
    for (uint32_t r = 0; r < regs && res + r < 512; r++)
        for (uint32_t k = 0; k < 4; k++) {
            const uint32_t at = val + 4 * (r * 4 + k);
            S.vconst[res + r][k] = k < comps && ea_ok(at) ? g32(at) : 0;
            S.vconst_written[(res + r) >> 5] |= 1u << ((res + r) & 31);
        }
}

// cellGcmSetVertexDataArray(ctx, index, frequency, stride, size, type, location, offset)
void enc_vertex_array(ppu_context* c)
{
    const uint32_t i = (uint32_t)c->gpr[4] & 15;
    const uint32_t freq = (uint32_t)c->gpr[5] & 0xFFFF, stride = (uint32_t)c->gpr[6] & 0xFF;
    const uint32_t size = (uint32_t)c->gpr[7] & 0xF, type = (uint32_t)c->gpr[8] & 0xF;
    const uint32_t loc = (uint32_t)c->gpr[9] & 1, off = (uint32_t)c->gpr[10];
    W(0x1740 + 4 * i, freq << 16 | stride << 8 | size << 4 | type);
    W(0x1680 + 4 * i, loc << 31 | (off & 0x7FFFFFFFu));
}

// cellGcmSetTexture(ctx, index, const CellGcmTexture*)
void enc_texture(ppu_context* c)
{
    const uint32_t u = (uint32_t)c->gpr[4] & 15, t = (uint32_t)c->gpr[5];
    if (!ea_ok(t, 24)) return;
    const uint32_t fmt = g8(t + 0), mip = g8(t + 1), dim = g8(t + 2), cube = g8(t + 3), remap = g32(t + 4);
    const uint32_t w = g16(t + 8), h = g16(t + 10), d = g16(t + 12), loc = g8(t + 14);
    const uint32_t pitch = g32(t + 16), off = g32(t + 20);
    const uint32_t b = 0x1A00 + 0x20 * u;
    W(b + 0x00, off);
    W(b + 0x04, (loc + 1) | cube << 2 | 0x8u | dim << 4 | fmt << 8 | mip << 16);
    W(b + 0x18, w << 16 | h);
    W(0x1840 + 4 * u, d << 20 | pitch);
    W(b + 0x10, remap);
}

void enc_tex_filter(ppu_context* c)    // (ctx, index, bias, min, mag, conv)
{
    const uint32_t u = (uint32_t)c->gpr[4] & 15;
    W(0x1A14 + 0x20 * u, ((uint32_t)c->gpr[7] & 7) << 24 | ((uint32_t)c->gpr[6] & 7) << 16 |
                         ((uint32_t)c->gpr[8] & 7) << 13 | ((uint32_t)c->gpr[5] & 0x1FFF));
}

void enc_tex_address(ppu_context* c)   // (ctx, index, wraps, wrapt, wrapr, unsignedRemap, zfunc, gamma)
{
    const uint32_t u = (uint32_t)c->gpr[4] & 15;
    W(0x1A08 + 0x20 * u, ((uint32_t)c->gpr[5] & 0xF) | ((uint32_t)c->gpr[6] & 0xF) << 8 |
                         ((uint32_t)c->gpr[8] & 0xF) << 12 | ((uint32_t)c->gpr[7] & 0xF) << 16 |
                         ((uint32_t)c->gpr[10] & 0xF) << 20 | ((uint32_t)c->gpr[9] & 0xF) << 28);
}

void enc_tex_control0(ppu_context* c)  // (ctx, index, enable, minlod, maxlod, maxaniso)
{
    const uint32_t u = (uint32_t)c->gpr[4] & 15;
    W(0x1A0C + 0x20 * u, ((uint32_t)c->gpr[5] & 1) << 31 | ((uint32_t)c->gpr[6] & 0xFFF) << 19 |
                         ((uint32_t)c->gpr[7] & 0xFFF) << 7 | ((uint32_t)c->gpr[8] & 7) << 4);
}

// The single-register calls: the method and the argument it takes.
struct Simple { uint32_t addr, method; int shift; };
const Simple kSimple[] = {
    { 0x006A5DA0, 0x1D90, 0 },   // clear colour
    { 0x006A5E30, 0x1D8C, 0 },   // clear depth/stencil value
    { 0x006A5EC0, 0x0324, 0 },   // colour mask
    { 0x006A5FE0, 0x1830, 0 },   // cull face
    { 0x006A6070, 0x0A6C, 0 },   // depth func
    { 0x006A6100, 0x0A70, 0 },   // depth mask
    { 0x006A62B4, 0x03BC, 0 },
    { 0x006A6344, 0x1DB4, 0 },
    { 0x006A650C, 0x1838, 0 },   // polygon smooth
    { 0x006A659C, 0x147C, 0 },
    { 0x006A662C, 0x1828, 0 },   // front polygon mode
    { 0x006A66BC, 0x182C, 0 },   // back polygon mode
    { 0x006A6810, 0x0368, 0 },   // shade mode
    { 0x006A68A0, 0x142C, 0 },
    { 0x006A6DA0, 0x0304, 0 },   // alpha test enable
    { 0x006A6E30, 0x0310, 0 },   // blend enable
    { 0x006A6F74, 0x0374, 0 },   // logic op enable
    { 0x006A7004, 0x183C, 0 },   // cull face enable
    { 0x006A7124, 0x0A74, 0 },   // depth test enable
    { 0x006A71B4, 0x0300, 0 },   // dither enable
    { 0x006A7244, 0x0328, 0 },   // stencil test enable
    { 0x006A7364, 0x0A68, 0 },   // polygon offset fill enable
    { 0x006A7CF8, 0x1FC0, 0 },   // frequency divider operation
    { 0x006A7EB0, 0x1FEC, 0 },   // fragment program gamma
    { 0x006A8020, 0x1E98, 24 },
    { 0x006A817C, 0x17CC, 0 },
    { 0x006A820C, 0x17C8, 0 },
    { 0x006A829C, 0x1800, 0 },
};

// Calls PPFX makes that write no state the records use: wrappers whose work
// the calls they make are observed for, the vertex cache invalidate, and the
// functions that write no command at all.
bool no_state(uint32_t a)
{
    switch (a) {
    case 0x0069F454: case 0x0069F814: case 0x0069FC2C: case 0x006A336C: case 0x006A97F8:
    case 0x006A7BA4: case 0x006A1D1C: case 0x006A4564: case 0x006A5A40: case 0x006A5ADC:
    case 0x006A9ED8: case 0x0069E474:
        return true;
    default:
        return a < 0x0069DD60u;   // 0x0069CAC4..0x0069D240: no command (address and memory helpers)
    }
}

// ---- attribution ---------------------------------------------------------------

bool in_ppfx(uint32_t a) { return a >= kPpfxLo && a < kPpfxHi; }
bool in_gcm(uint32_t a) { return a >= kGcmLo && a < kGcmHi; }

// 1 PPFX called it, 2 PPFX called the libgcm function that called it, 0 not PPFX.
int attribute(ppu_context* c)
{
    const uint32_t lr = (uint32_t)c->lr;
    if (in_ppfx(lr)) return 1;
    if (!in_gcm(lr)) return 0;
    uint32_t sp = (uint32_t)c->gpr[1];
    for (int d = 0; d < 6; d++) {
        if (!ea_ok(sp, 8)) return 0;
        const uint32_t back = g32(sp + 4);              // the back chain (a doubleword)
        if (!ea_ok(back + 16, 8) || back <= sp) return 0;
        const uint32_t saved = g32(back + 16 + 4);      // the caller's saved LR
        if (in_ppfx(saved)) return 2;
        if (!in_gcm(saved)) return 0;
        sp = back;
    }
    return 0;
}

// ---- records ---------------------------------------------------------------------

uint32_t elem_bytes(uint32_t type, uint32_t size)
{
    switch (type) {
    case 1: case 3: case 5: return 2 * size;   // snorm16, half, sint16
    case 2: return 4 * size;                    // float
    case 4: case 7: return size;                // unorm8, uint8
    case 6: return 4;                           // cmp32: one packed word
    default: return 4 * size;
    }
}

Rec* acquire()
{
    Queue& q = queue();
    const uint64_t p = q.put.load(std::memory_order_relaxed);
    if (p - q.done.load(std::memory_order_acquire) >= Queue::kCap) {
        const uint64_t t0 = now_ns();
        q.full_waits++;
        if (s_held) s_held->unlock();
        std::unique_lock<std::mutex> lk(q.m);
        uint64_t waited = 0;
        while (p - q.done.load(std::memory_order_acquire) >= Queue::kCap) {
            q.cv.wait_for(lk, std::chrono::milliseconds(100));
            if ((waited += 100) % 5000 == 0)
                fprintf(stderr, "[ppfx] the record queue has been full for %llu s (put %llu walked %llu done %llu)\n",
                        (unsigned long long)waited / 1000, (unsigned long long)p,
                        (unsigned long long)q.walked.load(), (unsigned long long)q.done.load());
        }
        lk.unlock();
        if (s_held) s_held->lock();
        q.full_wait_us += (now_ns() - t0) / 1000;
    }
    Rec*& r = q.slot[p % Queue::kCap];
    if (!r) r = new Rec();
    return r;
}

void fill_common(Rec* r, uint32_t kind, ppu_context* c)
{
    Queue& q = queue();
    r->seq = q.put.load(std::memory_order_relaxed);
    r->kind = kind;
    r->frame = S.frame;
    r->why = kWhyNone;
    const uint32_t n = reg_count();
    for (uint32_t i = 0; i < n; i++) r->regs[i] = S.regs[reg_method(i) >> 2];
    r->nvr = 0; r->vdata.clear(); r->nneed = 0; r->ntex = 0; r->tdata.clear();
    memset(&r->walk, 0, sizeof r->walk);
    // where its commands go: the title's lap and the context's current (or,
    // if this call's reserve wraps the ring, the next lap from its start)
    const uint32_t ctx = (uint32_t)c->gpr[3];
    r->lap = cellGcm_title_laps_pub(nullptr);
    r->io = ea_ok(ctx, 16) ? cellGcm_ea2io_pub(g32(ctx + 8)) : 0;
    r->io_alt = ea_ok(ctx, 16) ? cellGcm_ea2io_pub(g32(ctx + 0)) : 0;
}

void publish(Rec* r)
{
    Queue& q = queue();
    if (r->seq < 4) {   // the first records' positions, for the bridge's first match
        char line[200];
        snprintf(line, sizeof line, "recorder: record %llu kind %u frame %u lap %u io %08X alt %08X at %.3f s",
                 (unsigned long long)r->seq, r->kind, r->frame, r->lap, r->io, r->io_alt, now_s());
        report_add(line);
    }
    q.put.store(r->seq + 1, std::memory_order_release);
    q.cv.notify_all();
}

void add_target_need(Rec* r)
{
    const uint32_t* R = S.regs;
    const uint32_t dma = R[0x0194 >> 2];
    Need& n = r->need[r->nneed++];
    memset(&n, 0, sizeof n);
    n.location = dma == 0xFEED0001u ? 1u : 0u;
    n.offset = R[0x0210 >> 2];
    n.pitch = R[0x020C >> 2];
    n.w = R[0x0200 >> 2] >> 16;
    n.h = R[0x0204 >> 2] >> 16;
    n.format = R[0x0208 >> 2] & 0x1F;
    n.unit = 0xFF;
    const uint64_t key = (uint64_t)n.location << 32 | n.offset;
    n.ppfx_wrote = S.ppfx_targets.count(key) ? 1 : 0;
    S.ppfx_targets.insert(key);
}

void build_draw(ppu_context* c)
{
    Rec* r = acquire();
    fill_common(r, kDraw, c);
    r->prim = (uint32_t)c->gpr[4];
    r->first = (uint32_t)c->gpr[5];
    r->count = (uint32_t)c->gpr[6];
    memcpy(r->vconst, S.vconst, sizeof r->vconst);
    memcpy(r->vconst_written, S.vconst_written, sizeof r->vconst_written);
    // the vertex program from its start slot through its last instruction
    const uint32_t start = S.regs[0x1EA0 >> 2];
    r->vp_start = start;
    if (S.vp_blob_gen != S.vp_gen || !S.vp_blob || start != S.vp_blob_start) {
        if (start < 544) {
            const uint32_t instrs = rsx_vp_program_size_instrs((const uint8_t*)(S.vp + start * 4), (544 - start) * 16);
            S.vp_blob = instrs ? blob_of((const uint8_t*)(S.vp + start * 4), instrs * 16) : nullptr;
        } else S.vp_blob = nullptr;
        S.vp_blob_gen = S.vp_gen;
        S.vp_blob_start = start;
    }
    r->vp = S.vp_blob;
    r->fp = S.fp; r->fp_off = S.fp_off; r->fp_loc = S.fp_loc;
    if (!r->vp) r->why = kWhyNoVp;
    else if (!r->fp) r->why = kWhyNoFp;
    // the vertex arrays' bytes this draw reads
    const uint32_t base = S.regs[0x1738 >> 2];
    for (uint32_t i = 0; i < 16; i++) {
        const uint32_t f = S.regs[(0x1740 >> 2) + i], o = S.regs[(0x1680 >> 2) + i];
        const uint32_t size = f >> 4 & 0xF, type = f & 0xF, stride = f >> 8 & 0xFF;
        if (!size || !r->count) continue;
        const uint32_t loc = o >> 31 ? 1u : 0u;   // RSX_LOCATION_MAIN : _LOCAL
        const uint32_t off = (base + (o & 0x7FFFFFFFu)) & 0x0FFFFFFFu;
        const uint32_t eb = elem_bytes(type, size);
        const uint32_t lo = off + r->first * stride;
        const uint32_t bytes = (r->count - 1) * stride + eb;
        const uint32_t ea = cellGcmResolveLocated(loc == 0, lo);
        if (!ea_ok(ea, bytes) || bytes > (1u << 20)) { r->why = kWhyVertex; continue; }
        VtxRange& v = r->vr[r->nvr++];
        v.location = loc; v.offset = lo; v.bytes = bytes; v.at = (uint32_t)r->vdata.size();
        r->vdata.insert(r->vdata.end(), vm_base + ea, vm_base + ea + bytes);
    }
    // what it reads and writes: its colour target, the units its program samples
    add_target_need(r);
    for (uint32_t u = 0; u < 16; u++) {
        if (!(S.fp_units >> u & 1)) continue;
        const uint32_t b = (0x1A00 + 0x20 * u) >> 2;
        if (!(S.regs[b + 3] >> 31)) continue;     // control0's enable
        Need& n = r->need[r->nneed++];
        memset(&n, 0, sizeof n);
        const uint32_t fw = S.regs[b + 1];
        n.location = (fw & 3) == 2 ? 1u : 0u;
        n.offset = S.regs[b];
        n.format = fw >> 8 & 0xFF;
        n.w = S.regs[b + 6] >> 16; n.h = S.regs[b + 6] & 0xFFFF;
        n.pitch = S.regs[(0x1840 >> 2) + u] & 0xFFFFF;
        n.unit = (uint8_t)u;
        n.ppfx_wrote = S.ppfx_targets.count((uint64_t)n.location << 32 | n.offset) ? 1 : 0;
        if (!n.ppfx_wrote) {
            // a texture no record wrote: its bytes now, if small (a static
            // texture; a surface other work renders is handed over instead) --
            // its whole mip chain, as the engine reads it
            rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
            const uint32_t levels = fw >> 16 & 0xFF;
            const uint32_t nlv = (fw >> 2 & 1) ? 0 : rsx_texture_mip_chain(n.format, n.w, n.h, levels, n.pitch, lv);
            const uint32_t bytes = nlv ? lv[nlv - 1].offset + lv[nlv - 1].tl.face_bytes : 0;
            const uint32_t ea = cellGcmResolveLocated(n.location == 0, n.offset);
            if (bytes && bytes <= (256u << 10) && ea_ok(ea, bytes)) {
                VtxRange& t = r->tex[r->ntex++];
                t.location = u;   // here: the unit
                t.offset = n.offset; t.bytes = bytes; t.at = (uint32_t)r->tdata.size();
                r->tdata.insert(r->tdata.end(), vm_base + ea, vm_base + ea + bytes);
            }
        }
    }
    publish(r);
    stats().draws++;
}

void build_clear(ppu_context* c)
{
    Rec* r = acquire();
    fill_common(r, kClear, c);
    r->clear_mask = (uint32_t)c->gpr[4];
    W(0x1D94, r->clear_mask);
    r->regs[reg_index(0x1D94)] = r->clear_mask;
    add_target_need(r);
    publish(r);
    stats().clears++;
}

void read_env()
{
    const char* pw = getenv("DOTHEROES_PPFX_PROGWATCH");
    if (pw && *pw && (s_pw = fopen(pw, "w")))
        fprintf(s_pw, "# ppfx program watch: B <flips> <s> per program bind whose bytes changed; "
                      "T <flips> <s> per inline transfer of more than 4 words\n");
}

// ---- the report ----------------------------------------------------------------------

std::mutex s_rep_lock;
std::vector<std::string> s_rep_lines;
char s_rep_path[512];
std::map<uint32_t, uint64_t> s_unmodelled_by;   // function -> calls

void write_report()
{
    FILE* f = fopen(s_rep_path, "w");
    if (!f) return;
    Stats& s = stats();
    Queue& q = queue();
    fprintf(f, "# ppfx report at %.1f s (mode %d)\n", now_s(), mode());
    fprintf(f, "calls %llu ppfx %llu nested_ppfx %llu others_on_producer %llu other_threads %llu unmodelled %llu "
               "producer_threads %llu\n",
            (unsigned long long)s.calls, (unsigned long long)s.ppfx_calls, (unsigned long long)s.nested_ppfx,
            (unsigned long long)s.foreign_state_calls, (unsigned long long)s.other_thread_calls,
            (unsigned long long)s.unmodelled, (unsigned long long)s.producer_threads);
    fprintf(f, "records draws %llu clears %llu (depth-only clears %llu) frames %llu put %llu walked %llu done %llu\n",
            (unsigned long long)s.draws, (unsigned long long)s.clears, (unsigned long long)s.depth_clears,
            (unsigned long long)s.frames,
            (unsigned long long)q.put, (unsigned long long)q.walked, (unsigned long long)q.done);
    fprintf(f, "wraps_in_call %llu\n", (unsigned long long)s.wraps_in_call);
    fprintf(f, "recorder_ms %.3f queue_full_waits %llu queue_full_wait_ms %.3f\n", s.rec_ns / 1e6,
            (unsigned long long)q.full_waits, q.full_wait_us / 1e3);
    fprintf(f, "walker matched %llu missed %llu foreign_draws %llu foreign_clears %llu lap_delta_changes %llu\n",
            (unsigned long long)s.matched, (unsigned long long)s.missed, (unsigned long long)s.foreign_draws,
            (unsigned long long)s.foreign_clears, (unsigned long long)s.lap_delta_changes);
    fprintf(f, "crosscheck draws %llu regs %llu consts %llu vp %llu fp %llu vtx %llu\n",
            (unsigned long long)s.xc_draws, (unsigned long long)s.xc_regs, (unsigned long long)s.xc_consts,
            (unsigned long long)s.xc_vp, (unsigned long long)s.xc_fp, (unsigned long long)s.xc_vtx,
            (unsigned long long)s.xc_tex);
    fprintf(f, "handoffs %llu failed %llu\n", (unsigned long long)s.handoffs, (unsigned long long)s.handoff_fail);
    fprintf(f, "native draws %llu clears %llu unsupported %llu batches %llu pipelines %llu images %llu (%.1f MB) "
               "static_textures %llu own_target_snapshots %llu\n",
            (unsigned long long)s.n_draws, (unsigned long long)s.n_clears, (unsigned long long)s.n_unsupported,
            (unsigned long long)s.n_batches, (unsigned long long)s.n_pipelines, (unsigned long long)s.n_images,
            s.n_image_bytes / 1048576.0, (unsigned long long)s.n_static_textures, (unsigned long long)s.n_feedback);
    fprintf(f, "native_ms record_cpu %.1f gpu %.1f wait_walker %.1f wait_signal %.1f compare %.1f\n",
            s.cpu_record_ns / 1e6, s.gpu_ns / 1e6, s.wait_walker_us / 1e3, s.wait_signal_us / 1e3, s.compare_ns / 1e6);
    fprintf(f, "compare records %llu equal %llu differ %llu skipped %llu finals %llu finals_equal %llu\n",
            (unsigned long long)s.cmp_records, (unsigned long long)s.cmp_equal, (unsigned long long)s.cmp_differ,
            (unsigned long long)s.cmp_skipped, (unsigned long long)s.cmp_finals, (unsigned long long)s.cmp_finals_equal);
    {
        std::lock_guard<std::mutex> g(s_unm_lock);
        for (auto& kv : s_unmodelled_by) fprintf(f, "unmodelled %08X %llu\n", kv.first, (unsigned long long)kv.second);
    }
    {
        std::vector<std::string> xs;
        bridge_report(xs);
        for (auto& l : xs) fprintf(f, "%s\n", l.c_str());
    }
    std::lock_guard<std::mutex> g(s_rep_lock);
    for (auto& l : s_rep_lines) fprintf(f, "%s\n", l.c_str());
    fclose(f);
}

}  // namespace

bool reg_written(uint32_t m) { return m < 0x2000 && S.written[m >> 2]; }

void report_add(const char* line)
{
    std::lock_guard<std::mutex> g(s_rep_lock);
    if (s_rep_lines.size() < 20000) s_rep_lines.push_back(line);
}

void start()
{
    init_index();
    {   // the hardware's reset values, as the register file starts
        rsx_dispatch* t = (rsx_dispatch*)malloc(sizeof(rsx_dispatch));
        if (t) {
            rsx_dispatch_init(t, nullptr);
            memcpy(S.regs, t->regs, sizeof S.regs);
            free(t);
        }
    }
    read_env();
    const char* rp = getenv("DOTHEROES_PPFX_REPORT");
    snprintf(s_rep_path, sizeof s_rep_path, "%s", rp && *rp ? rp : "ppfx_report.txt");
    if (!mode()) return;
    atexit(write_report);
    std::thread([] {
#ifdef _WIN32
        SetThreadDescription(GetCurrentThread(), L"ppfx report");
#endif
        for (;;) { std::this_thread::sleep_for(std::chrono::seconds(10)); write_report(); }
    }).detach();
    bridge_install();
    if (mode() == 2) render_start();
}

}  // namespace ppfx

using namespace ppfx;

extern "C" int ppfx_rec_wanted(void)
{
    const char* pw = getenv("DOTHEROES_PPFX_PROGWATCH");
    if (pw && *pw) return 1;
    return ppfx::mode() != 0;
}

// Every observed call, from r1_observe.cpp's ppu_entry_observe.
extern "C" void ppfx_rec_observe(uint32_t addr, ppu_context* ctx)
{
    static int started = 0;
    if (!started) { started = 1; ppfx::start(); }
    const uint64_t t0 = now_ns();
    Stats& st = stats();
    st.calls++;
    const uint32_t tid =
#ifdef _WIN32
        GetCurrentThreadId();
#else
        0;
#endif
    const int who = attribute(ctx);
    std::unique_lock<std::mutex> g(s_lock);
    struct Held { Held(std::unique_lock<std::mutex>* h) { s_held = h; } ~Held() { s_held = nullptr; } } held(&g);
    if (addr == 0x0069DD60u) {   // cellGcmSetFlip: a frame
        if (tid == S.tid || !S.tid) { S.frame++; st.frames++; }
        if (s_pw && (S.frame & 255) == 0) fflush(s_pw);
    }
    // Inline transfers into the shadow, whoever makes them (the program
    // memory is the GPU's; PPFX's are the ones that matter, but a transfer
    // by other code lands in the same memory).
    if (addr == 0x0069FC38u) {
        const uint32_t dst = (uint32_t)ctx->gpr[4], src = (uint32_t)ctx->gpr[5], words = (uint32_t)ctx->gpr[6];
        if (words && words <= 0x10000u && ea_ok(src, words * 4)) {
            shadow_write(dst, vm_base + src, words * 4u, ++s_epoch);
            if (s_pw && words > 4)
                fprintf(s_pw, "T %u %.3f dst=%08X words=%u src=%08X lr=%08X\n", S.frame, now_s(), dst, words, src,
                        (uint32_t)ctx->lr);
        }
    }
    // The command stream has one producer: the thread of the first observed
    // call. Every PPFX call must be on it (fatal otherwise); calls by other
    // code on other threads are counted and left out of the state.
    if (!S.tid) S.tid = tid;
    if (tid != S.tid) {
        if (who) {
            st.producer_threads++;
            fprintf(stderr, "[ppfx] FATAL: a PPFX call (0x%08X) on thread %u, the command stream's producer is "
                            "thread %u: the single-producer assumption does not hold\n", addr, tid, S.tid);
            fflush(stderr);
            abort();
        }
        st.other_thread_calls++;
        st.rec_ns += now_ns() - t0;
        return;
    }
    // Other code's calls on the producer's thread set state too, at the same
    // entry points: PPFX's first frames after a load draw with some of it
    // (a texture address, the colour mask) -- the GPU's state as the calls
    // left it, in their order. Only PPFX's draws and clears become records.
    if (who) { st.ppfx_calls++; if (who == 2) st.nested_ppfx++; }
    else st.foreign_state_calls++;
    if (!mode() && !s_pw) return;
    switch (addr) {
    case 0x0069F944: enc_surface((uint32_t)ctx->gpr[4]); break;
    case 0x0069E7EC: enc_viewport(ctx); break;
    case 0x0069E99C: enc_fp_bind(ctx, S.frame); break;
    case 0x0069E6E0: enc_shader_control(ctx); break;
    case 0x006A4E50: enc_vp(ctx); break;
    case 0x006A39EC: enc_vp_const(ctx); break;
    case 0x0069F19C: enc_vertex_array(ctx); break;
    case 0x0069F2B8: enc_texture(ctx); break;
    case 0x006A7674: enc_tex_filter(ctx); break;
    case 0x006A7744: enc_tex_address(ctx); break;
    case 0x006A7834: enc_tex_control0(ctx); break;
    case 0x006A75D8: W(0x1A1C + 0x20 * ((uint32_t)ctx->gpr[4] & 15), (uint32_t)ctx->gpr[5]); break;
    case 0x006A674C:   // scissor (ctx, x, y, w, h)
        W(0x08C0, ((uint32_t)ctx->gpr[6] & 0xFFFF) << 16 | ((uint32_t)ctx->gpr[4] & 0xFFFF));
        W(0x08C4, ((uint32_t)ctx->gpr[7] & 0xFFFF) << 16 | ((uint32_t)ctx->gpr[5] & 0xFFFF));
        break;
    case 0x006A56BC:   // anti-aliasing control (ctx, enable, alphaToCoverage, alphaToOne, sampleMask)
        W(0x1D7C, ((uint32_t)ctx->gpr[7] & 0xFFFF) << 16 | ((uint32_t)ctx->gpr[6] & 1) << 8 |
                  ((uint32_t)ctx->gpr[5] & 1) << 4 | ((uint32_t)ctx->gpr[4] & 1));
        break;
    case 0x006A90EC: W(0x1738, (uint32_t)ctx->gpr[4]); W(0x173C, (uint32_t)ctx->gpr[5]); break;
    case 0x006A96E8: W(0x0A78, fbits(ctx->fpr[1])); W(0x0A7C, fbits(ctx->fpr[2])); break;
    case 0x006A1E28: if (mode() && who) build_draw(ctx); break;
    case 0x006A5CF4:   // a clear: a record when it clears colour (depth and stencil are not PPFX's output)
        if (!who) break;
        if (mode() && ((uint32_t)ctx->gpr[4] & 0xF0u)) build_clear(ctx);
        else st.depth_clears++;
        break;
    case 0x0069FC38: case 0x0069DD60: break;
    default: {
        bool done = false;
        for (const Simple& s : kSimple)
            if (s.addr == addr) { W(s.method, (uint32_t)ctx->gpr[4] << s.shift); done = true; break; }
        if (!done && !no_state(addr) && who) { st.unmodelled++; std::lock_guard<std::mutex> u(s_unm_lock); s_unmodelled_by[addr]++; }
        break;
    }
    }
    st.rec_ns += now_ns() - t0;
}
