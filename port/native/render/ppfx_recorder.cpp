// The native PPFX recorder (docs/NATIVE_R3.md, docs/NATIVE_R3B.md): PPFX's
// libgcm calls, seen at their entry by the development lift's observers
// (r1_observe.cpp dispatches here), made into native records from the calls'
// own arguments -- no FIFO decoding, no reconstructed RSX state.
//
// The state. Each libgcm entry point PPFX reaches has an encoder here: from
// the call's arguments (and the structures they point to: a CellGcmSurface, a
// CellGcmTexture, a CgBinaryProgram) it sets the explicit state items that
// call means (ppfx_native.h State), in libgcm's own vocabulary. Nothing else
// sets them but the calls and asp's state API below; the existing renderer's
// registers are never read here (the bridge compares against them in
// development runs, as a check only).
//
// What PPFX inherits. Some of what PPFX's draws use, PPFX does not set: the
// game sets a few items at a frame's start through libgcm (observed here the
// same way), and asp, the engine, sets the colour mask and its texture
// units' sampler state through its own state API. That API is observed too:
// asp's colour mask setter (its four booleans), its sampler bind (the unit's
// address mode as asp's state store holds it once the bind has run), and
// asp's re-emission of its state store into the command buffer (the items
// the re-emitted range holds). docs/NATIVE_R3B.md section 1 has the evidence
// and the list.
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
extern "C" void (*g_dotheroes_stop_hook)(void);   // dev_state.cpp: run before a test's stop
extern "C" void rsx_frame_trace_arm(void);   // cellGcmSys.c: RSX_FRAME_TRACE=arm:<frames>:<path>
extern "C" void ppfx_watch_add_ea(uint32_t ea, uint32_t n);   // ppfx_watch.cpp

namespace ppfx {

// ---- shared definitions -----------------------------------------------------

// The GPU's reset state: everything off or zero, but all four colour
// channels written, counter-clockwise front faces, a depth/stencil clear
// value of depth 1.0 and stencil 0, and (0, 0, 0, 1) for a vertex input no
// array feeds (the RSX's documented reset values, as the toolkit's
// rsx_dispatch_init seeds them).
void reset_state(State* s)
{
    memset(s, 0, sizeof *s);
    s->raster.color_mask = 0x01010101u;   // CELL_GCM_COLOR_MASK_{R,G,B,A}
    s->raster.front_face = 0x0901u;       // CELL_GCM_CCW
    s->clear_depth = 0xFFFFFF00u;
    for (auto& v : s->vdata.current) { v[0] = v[1] = v[2] = 0.0f; v[3] = 1.0f; }
}

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

// A copy the game makes into local memory (its memcpy, observed): on pages
// the shadow keeps (program memory), the bytes it writes, in call order --
// a program uploaded again, after the title, at an offset that held other
// programs' patched words before.
void shadow_cpu_write(uint32_t off, const uint8_t* src, uint32_t n)
{
    const uint64_t ep = ++s_epoch;
    for (uint32_t pg = off & ~0xFFFu; pg < off + n; pg += 0x1000) {
        auto it = s_shadow.find(pg >> 12);
        if (it == s_shadow.end()) continue;
        ShadowPage* p = it->second;
        const uint32_t lo = pg < off ? off : pg, hi = pg + 0x1000 < off + n ? pg + 0x1000 : off + n;
        for (uint32_t w = lo & ~3u; w < hi; w += 4) {
            uint8_t* d = p->b + (w & 0xFFF);
            uint64_t& wep = p->ep[(w & 0xFFF) >> 2];
            if (!wep) memcpy(d, vm_base + kLocalEa + w, 4);   // a word copied in part: the rest as memory has it
            for (uint32_t b = 0; b < 4; b++)
                if (w + b >= lo && w + b < hi) d[b] = src[w + b - off];
            wep = ep;
        }
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

struct Rstate {
    State st;                        // the explicit state, as the calls set it
    uint32_t vconst[512][4];
    uint32_t vconst_written[16];
    uint32_t vp[kVpWords];           // vertex program memory as the uploads made it
    BlobP fp;                        // the bound fragment program's bytes
    BlobP vp_blob; uint64_t vp_gen, vp_blob_gen; uint32_t vp_blob_start;
    uint32_t fp_units;               // texture units the bound fragment program samples
    std::unordered_set<uint64_t> ppfx_targets;   // (location << 32 | offset) a record has written
    uint32_t frame;
    uint32_t ring_ctx;               // the command buffer's CellGcmContextData (PPFX's calls' first argument)
    uint8_t by;                      // who makes the call being encoded (By)
    // the producer
    uint32_t tid;
} S;

// asp's state API (docs/NATIVE_R3B.md section 1). asp keeps its render state
// in a state store: a command block, one slot per state item, at the
// CellGcmContextData its device holds at +0x124; its setters write an item
// both into the command buffer and into the item's slot, and it re-emits
// ranges of the store into the command buffer (its block append,
// 0x006D0BE0). The slots' offsets are asp's own tables, through its TOC.
struct Asp {
    uint32_t device, toc;            // asp's device and TOC, from its calls
    uint32_t pending_units;          // sampler binds that ran: the unit's address mode is in the store now
    uint8_t cmask_unknown;           // a pass apply set the colour mask in a way the model does not follow
    uint64_t unknown_draws;          // PPFX draws while it was
} A;

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

float gfloat(uint32_t ea) { const uint32_t u = g32(ea); float f; memcpy(&f, &u, 4); return f; }

// ---- encoders: what each call means ----------------------------------------------

// cellGcmSetSurface(ctx, const CellGcmSurface*)
void enc_surface(uint32_t s)
{
    if (!ea_ok(s, 60)) return;
    Surface& o = S.st.surface;
    o.type = (uint8_t)g8(s + 0); o.antialias = (uint8_t)g8(s + 1);
    o.color_format = (uint8_t)g8(s + 2); o.color_target = (uint8_t)g8(s + 3);
    for (int i = 0; i < 4; i++) {
        o.color_location[i] = (uint8_t)g8(s + 4 + i);
        o.color_offset[i] = g32(s + 8 + 4 * i);
        o.color_pitch[i] = g32(s + 24 + 4 * i);
    }
    o.depth_format = (uint8_t)g8(s + 40); o.depth_location = (uint8_t)g8(s + 41);
    o.depth_offset = g32(s + 44); o.depth_pitch = g32(s + 48);
    o.width = (uint16_t)g16(s + 52); o.height = (uint16_t)g16(s + 54);
    o.x = (uint16_t)g16(s + 56); o.y = (uint16_t)g16(s + 58);
}

// cellGcmSetViewport(ctx, x, y, w, h, min, max, scale[4], offset[4]): the
// last pointer is the ninth argument, in the caller's parameter save area.
void enc_viewport(ppu_context* c)
{
    Viewport& v = S.st.viewport;
    v.x = (uint16_t)c->gpr[4]; v.y = (uint16_t)c->gpr[5]; v.w = (uint16_t)c->gpr[6]; v.h = (uint16_t)c->gpr[7];
    v.zmin = (float)c->fpr[1]; v.zmax = (float)c->fpr[2];
    const uint32_t scale = (uint32_t)c->gpr[10];
    const uint32_t sp = (uint32_t)c->gpr[1];
    const uint32_t offset = ea_ok(sp + 112, 8) ? g32(sp + 112 + 4) : 0;
    for (int i = 0; i < 4; i++) {
        if (ea_ok(offset, 16)) v.offset[i] = gfloat(offset + 4 * i);
        if (ea_ok(scale, 16)) v.scale[i] = gfloat(scale + 4 * i);
    }
}

// The fragment program bind cellGcmSetFragmentProgram makes:
// (ctx, CgBinaryProgram*, offset, location)
void enc_fp_bind(ppu_context* c, uint32_t flips)
{
    const uint32_t prog = (uint32_t)c->gpr[4], off = (uint32_t)c->gpr[5], loc = (uint32_t)c->gpr[6] & 1;
    if (!ea_ok(prog, 32)) return;
    FragmentProgram& f = S.st.fp;
    f.offset = off; f.location = (uint8_t)loc;
    const uint32_t fpd = prog + g32(prog + 20);   // CgBinaryFragmentProgram
    if (ea_ok(fpd, 24)) {
        f.input_mask = g32(fpd + 4);
        f.tex_mask = (uint16_t)g16(fpd + 12); f.tex_2d = (uint16_t)g16(fpd + 14); f.tex_centroid = (uint16_t)g16(fpd + 16);
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
    S.fp_units = S.fp ? rsx_fp_texture_units(S.fp->bytes.data(), (uint32_t)S.fp->bytes.size()) : 0;
    if (s_pw && loc == 0) progwatch_bind(prog, off, bytes, known, flips);
}

// The shader control the fragment-program functions set: (ctx, prog, controlTxp, ...):
// the CgBinaryFragmentProgram's registerCount, outputFromH0, depthReplace, pixelKill
void enc_shader_control(ppu_context* c)
{
    const uint32_t prog = (uint32_t)c->gpr[4];
    if (!ea_ok(prog, 32)) return;
    const uint32_t fpd = prog + g32(prog + 20);
    if (!ea_ok(fpd, 24)) return;
    FragmentProgram& f = S.st.fp;
    f.reg_count = (uint8_t)g8(fpd + 18); f.output_from_h0 = (uint8_t)g8(fpd + 19);
    f.depth_replace = (uint8_t)g8(fpd + 20); f.pixel_kill = (uint8_t)g8(fpd + 21);
    f.txp = (uint8_t)((uint32_t)c->gpr[5] & 1);
}

// cellGcmSetVertexProgram(ctx, CgBinaryProgram*, ucode)
void enc_vp(ppu_context* c)
{
    const uint32_t prog = (uint32_t)c->gpr[4], uc = (uint32_t)c->gpr[5];
    if (!ea_ok(prog, 32)) return;
    const uint32_t vpd = prog + g32(prog + 20);   // CgBinaryVertexProgram
    if (!ea_ok(vpd, 24)) return;
    const uint32_t n = g32(vpd + 0), slot = g32(vpd + 4), rc = g32(vpd + 8), in = g32(vpd + 12);
    if (ea_ok(uc, n * 16))
        for (uint32_t i = 0; i < n * 4 && slot * 4 + i < kVpWords; i++) S.vp[slot * 4 + i] = g32(uc + 4 * i);
    VertexProgram& v = S.st.vp;
    v.start = (uint16_t)slot; v.input_mask = in; v.regs_le32 = rc <= 32;
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
    VertexInput& v = S.st.vin[(uint32_t)c->gpr[4] & 15];
    v.frequency = (uint16_t)c->gpr[5]; v.stride = (uint8_t)c->gpr[6];
    v.size = (uint8_t)((uint32_t)c->gpr[7] & 0xF); v.type = (uint8_t)((uint32_t)c->gpr[8] & 0xF);
    v.location = (uint8_t)((uint32_t)c->gpr[9] & 1); v.offset = (uint32_t)c->gpr[10];
}

// cellGcmSetTexture(ctx, index, const CellGcmTexture*)
void enc_texture(ppu_context* c)
{
    const uint32_t t = (uint32_t)c->gpr[5];
    if (!ea_ok(t, 24)) return;
    Texture& o = S.st.tex[(uint32_t)c->gpr[4] & 15];
    o.format = (uint8_t)g8(t + 0); o.mipmap = (uint8_t)g8(t + 1); o.dimension = (uint8_t)g8(t + 2);
    o.cubemap = (uint8_t)g8(t + 3); o.remap = g32(t + 4);
    o.width = (uint16_t)g16(t + 8); o.height = (uint16_t)g16(t + 10); o.depth = (uint16_t)g16(t + 12);
    o.location = (uint8_t)g8(t + 14); o.pitch = g32(t + 16); o.offset = g32(t + 20);
}

void enc_tex_filter(ppu_context* c)    // (ctx, index, bias, min, mag, conv)
{
    Sampler& s = S.st.samp[(uint32_t)c->gpr[4] & 15];
    s.bias = (uint16_t)((uint32_t)c->gpr[5] & 0x1FFF);
    s.min_filter = (uint8_t)((uint32_t)c->gpr[6] & 7); s.mag_filter = (uint8_t)((uint32_t)c->gpr[7] & 7);
    s.conv = (uint8_t)((uint32_t)c->gpr[8] & 7);
    s.filter_signed = 0;
}

void enc_tex_address(ppu_context* c)   // (ctx, index, wraps, wrapt, wrapr, unsignedRemap, zfunc, gamma)
{
    Sampler& s = S.st.samp[(uint32_t)c->gpr[4] & 15];
    s.wrap_s = (uint8_t)((uint32_t)c->gpr[5] & 0xF); s.wrap_t = (uint8_t)((uint32_t)c->gpr[6] & 0xF);
    s.wrap_r = (uint8_t)((uint32_t)c->gpr[7] & 0xF); s.unsigned_remap = (uint8_t)((uint32_t)c->gpr[8] & 0xF);
    s.zfunc = (uint8_t)((uint32_t)c->gpr[9] & 0xF); s.gamma = (uint8_t)((uint32_t)c->gpr[10] & 0xF);
    s.address_by = S.by;
}

void enc_tex_control0(ppu_context* c)  // (ctx, index, enable, minlod, maxlod, maxaniso)
{
    Sampler& s = S.st.samp[(uint32_t)c->gpr[4] & 15];
    s.enable = (uint8_t)((uint32_t)c->gpr[5] & 1);
    s.min_lod = (uint16_t)((uint32_t)c->gpr[6] & 0xFFF); s.max_lod = (uint16_t)((uint32_t)c->gpr[7] & 0xFFF);
    s.max_aniso = (uint8_t)((uint32_t)c->gpr[8] & 7);
}

// The calls that take one value: what it sets. 0 when the call is not one of them.
bool enc_simple(uint32_t addr, uint32_t a)
{
    State& t = S.st;
    Raster& r = t.raster;
    switch (addr) {
    case 0x006A5DA0: t.clear_color = a; break;                         // cellGcmSetClearColor
    case 0x006A5E30: t.clear_depth = a; break;                         // cellGcmSetClearDepthStencil
    case 0x006A5EC0: r.color_mask = a; r.color_mask_by = S.by; A.cmask_unknown = 0; break;   // cellGcmSetColorMask
    case 0x006A5FE0: r.cull_face = (uint16_t)a; break;                 // cellGcmSetCullFace
    case 0x006A6070: r.depth_func = (uint16_t)a; break;                // cellGcmSetDepthFunc
    case 0x006A6100: r.depth_write = (uint8_t)a; break;                // cellGcmSetDepthMask
    case 0x006A62B4: r.line_smooth = (uint8_t)a; break;                // cellGcmSetLineSmoothEnable
    case 0x006A6344: r.line_stipple = (uint8_t)a; break;               // cellGcmSetLineStippleEnable
    case 0x006A650C: r.poly_smooth = (uint8_t)a; break;                // cellGcmSetPolySmoothEnable
    case 0x006A659C: r.poly_stipple = (uint8_t)a; break;               // cellGcmSetPolygonStippleEnable
    case 0x006A662C: r.front_poly_mode = (uint16_t)a; break;           // cellGcmSetFrontPolygonMode
    case 0x006A66BC: r.back_poly_mode = (uint16_t)a; break;            // cellGcmSetBackPolygonMode
    case 0x006A6810: r.shade_model = (uint16_t)a; break;               // cellGcmSetShadeMode
    case 0x006A68A0: r.two_side_light = (uint8_t)a; break;             // cellGcmSetTwoSideLightEnable
    case 0x006A6DA0: r.alpha_test = (uint8_t)a; break;                 // cellGcmSetAlphaTestEnable
    case 0x006A6E30: r.blend = (uint8_t)a; break;                      // cellGcmSetBlendEnable
    case 0x006A6F74: r.logic_op = (uint8_t)a; break;                   // cellGcmSetLogicOpEnable
    case 0x006A7004: r.cull = (uint8_t)a; break;                       // cellGcmSetCullFaceEnable
    case 0x006A7124: r.depth_test = (uint8_t)a; break;                 // cellGcmSetDepthTestEnable
    case 0x006A71B4: r.dither = (uint8_t)a; break;                     // cellGcmSetDitherEnable
    case 0x006A7244: r.stencil_test = (uint8_t)a; break;               // cellGcmSetStencilTestEnable
    case 0x006A7364: r.poly_offset_fill = (uint8_t)a; break;           // cellGcmSetPolygonOffsetFillEnable
    case 0x006A7CF8: t.vdata.divider_op = (uint16_t)a; break;          // cellGcmSetFrequencyDividerOperation
    case 0x006A7EB0: t.fp.gamma = (uint8_t)a; break;                   // cellGcmSetFragmentProgramGammaEnable
    // occlusion queries: no rendering state; R4 has to keep their effects (docs/NATIVE_R3B.md)
    case 0x006A8020: r.render_enable_mode = (uint8_t)a; break;         // cellGcmSetRenderEnable
    case 0x006A817C: r.zpass_count = (uint8_t)a; break;                // cellGcmSetZpassPixelCountEnable
    case 0x006A820C: break;                                            // cellGcmSetClearReport
    case 0x006A829C: if (S.by == kByPpfx) stats().reports++; break;    // cellGcmSetReport
    default: return false;
    }
    return true;
}

// Calls PPFX makes that set no state the records use: wrappers whose work
// the calls they make are observed for, the vertex cache invalidate, and the
// functions that write no command at all -- and the report copy
// (0x006A1D1C, a transfer on the memory-to-memory object: a side effect for
// R4, no rendering state).
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

// ---- asp's state API ---------------------------------------------------------------

const uint32_t kAspColorMask = 0x006C02E8u, kAspSampler = 0x006C37A0u, kAspAppend = 0x006D0BE0u;
const uint32_t kAspPassApply = 0x006D1764u;

bool asp_fn(uint32_t a)
{
    switch (a) {
    case 0x006C02E8: case 0x006C37A0: case 0x006D0BE0: case 0x006D1764: case 0x006CD014: case 0x006D0B98:
    case 0x006C01C4: case 0x006C4628: case 0x006C49BC: case 0x006C4D1C: case 0x006C5298: case 0x006C0A70:
    case 0x006C0E38: case 0x006C19A4:
        return true;
    default:
        return false;
    }
}

// The store's bounds (its CellGcmContextData's begin and end).
bool asp_store(uint32_t* begin, uint32_t* end)
{
    if (!A.device) return false;
    const uint32_t ctx = g32(A.device + 0x124);
    if (!ea_ok(ctx, 16)) return false;
    *begin = g32(ctx + 0); *end = g32(ctx + 4);
    return ea_ok(*begin, 8) && *end > *begin;
}

// An item's slot in the store: asp's slot table (a TOC entry) gives its
// offset; the slot is the command that sets the item, its value the word
// after the method header.
uint32_t asp_slot(uint32_t begin, int32_t toc_entry, uint32_t at)
{
    const uint32_t tbl = g32(A.toc + (uint32_t)toc_entry);
    if (!ea_ok(tbl + at, 2)) return 0;
    return begin + (g16(tbl + at) & ~3u);
}
uint32_t asp_color_mask_slot(uint32_t begin) { return asp_slot(begin, -0x2D98, 0x20); }
uint32_t asp_address_slot(uint32_t begin, uint32_t unit) { return asp_slot(begin, -0x2D54, 4 * unit); }

// The value a slot holds, when it holds the item it should (the method
// header checks the table and the layout; a mismatch is counted).
bool asp_read(uint32_t slot, uint32_t method, uint32_t* v)
{
    if (!slot || !ea_ok(slot, 8) || g32(slot) != (0x00040000u | method)) { stats().asp_store_mismatch++; return false; }
    *v = g32(slot + 4);
    return true;
}

// asp's stored address mode (the value cellGcmSetTextureAddress would take, packed as asp stores it)
void asp_set_address(uint32_t unit, uint32_t w)
{
    Sampler& s = S.st.samp[unit];
    s.wrap_s = w & 0xF; s.wrap_t = w >> 8 & 0xF; s.unsigned_remap = w >> 12 & 0xF;
    s.wrap_r = w >> 16 & 0xF; s.gamma = w >> 20 & 0xF; s.zfunc = w >> 28 & 0xF;
    s.address_by = kByAsp;
}

void asp_set_color_mask(uint32_t m)
{
    S.st.raster.color_mask = m; S.st.raster.color_mask_by = kByAsp; A.cmask_unknown = 0;
}

// Sampler binds that have run since the last call: the store has the units' address modes now.
void asp_resolve()
{
    if (!A.pending_units) return;
    uint32_t begin, end;
    if (asp_store(&begin, &end))
        for (uint32_t u = 0; u < 16; u++) {
            uint32_t v;
            if ((A.pending_units >> u & 1) && asp_read(asp_address_slot(begin, u), 0x1A08 + 0x20 * u, &v))
                asp_set_address(u, v);
        }
    A.pending_units = 0;
}

void asp_call(uint32_t addr, ppu_context* c)
{
    stats().asp_calls++;
    if (addr == kAspColorMask || addr == kAspSampler) { A.device = (uint32_t)c->gpr[3]; A.toc = (uint32_t)c->gpr[2]; }
    switch (addr) {
    case kAspColorMask: {   // (device, const u8 rgba[4]): asp writes R, G and A as on/off, B's byte as it is
        const uint32_t p = (uint32_t)c->gpr[4];
        if (!ea_ok(p, 4)) break;
        asp_set_color_mask((g8(p) ? 0x00010000u : 0) | (g8(p + 1) ? 0x00000100u : 0) | g8(p + 2) |
                           (g8(p + 3) ? 0x01000000u : 0));
        break;
    }
    case kAspSampler: {     // (device, unit, enable): an enabled bind sets the unit's address mode from asp's sampler table
        const uint32_t u = (uint32_t)c->gpr[4] & 0xFF;
        if (u < 16 && ((uint32_t)c->gpr[5] & 0xFF)) A.pending_units |= 1u << u;
        break;
    }
    case kAspAppend: {      // (context, source, words): a re-emission of the store's range into the command buffer
        const uint32_t dst = (uint32_t)c->gpr[3], src = (uint32_t)c->gpr[4], words = (uint32_t)c->gpr[5];
        uint32_t begin, end, v;
        if (dst != S.ring_ctx || !asp_store(&begin, &end) || src < begin || src >= end) break;
        stats().asp_restores++;
        const uint64_t hi = (uint64_t)src + 4ull * words;
        const uint32_t cm = asp_color_mask_slot(begin);
        if (cm >= src && cm < hi && asp_read(cm, 0x0324, &v)) asp_set_color_mask(v);
        for (uint32_t u = 0; u < 16; u++) {
            const uint32_t s = asp_address_slot(begin, u);
            if (s >= src && s < hi && asp_read(s, 0x1A08 + 0x20 * u, &v)) asp_set_address(u, v);
        }
        break;
    }
    case kAspPassApply:     // a pass's own colour mask, depth test and mask: not followed here
        A.cmask_unknown = 1;
        break;
    default:                // asp's other setters: state PPFX sets itself (docs/NATIVE_R3B.md)
        break;
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
    r->st = S.st;
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
    const Surface& sf = S.st.surface;
    Need& n = r->need[r->nneed++];
    memset(&n, 0, sizeof n);
    n.location = sf.color_location[0] ? 1u : 0u;   // CELL_GCM_LOCATION_MAIN : _LOCAL
    n.offset = sf.color_offset[0];
    n.pitch = sf.color_pitch[0];
    n.w = sf.width;
    n.h = sf.height;
    n.format = sf.color_format;
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
    const uint32_t start = S.st.vp.start;
    if (S.vp_blob_gen != S.vp_gen || !S.vp_blob || start != S.vp_blob_start) {
        if (start < 544) {
            const uint32_t instrs = rsx_vp_program_size_instrs((const uint8_t*)(S.vp + start * 4), (544 - start) * 16);
            S.vp_blob = instrs ? blob_of((const uint8_t*)(S.vp + start * 4), instrs * 16) : nullptr;
        } else S.vp_blob = nullptr;
        S.vp_blob_gen = S.vp_gen;
        S.vp_blob_start = start;
    }
    r->vp = S.vp_blob;
    r->fp = S.fp;
    if (!r->vp) r->why = kWhyNoVp;
    else if (!r->fp) r->why = kWhyNoFp;
    if (A.cmask_unknown) A.unknown_draws++;
    // the vertex inputs' bytes this draw reads
    const uint32_t base = S.st.vdata.base_offset;
    for (uint32_t i = 0; i < 16; i++) {
        const VertexInput& in = S.st.vin[i];
        if (!in.size || !r->count) continue;
        const uint32_t loc = in.location;   // CELL_GCM_LOCATION_LOCAL 0, _MAIN 1
        const uint32_t off = (base + (in.offset & 0x7FFFFFFFu)) & 0x0FFFFFFFu;
        const uint32_t eb = elem_bytes(in.type, in.size);
        const uint32_t lo = off + r->first * in.stride;
        const uint32_t bytes = (r->count - 1) * in.stride + eb;
        const uint32_t ea = cellGcmResolveLocated(loc == 0, lo);
        if (!ea_ok(ea, bytes) || bytes > (1u << 20)) { r->why = kWhyVertex; continue; }
        VtxRange& v = r->vr[r->nvr++];
        v.location = loc; v.offset = lo; v.bytes = bytes; v.at = (uint32_t)r->vdata.size();
        r->vdata.insert(r->vdata.end(), vm_base + ea, vm_base + ea + bytes);
    }
    // what it reads and writes: its colour target, the units its program samples
    add_target_need(r);
    for (uint32_t u = 0; u < 16; u++) {
        if (!(S.fp_units >> u & 1) || !S.st.samp[u].enable) continue;
        const Texture& t = S.st.tex[u];
        Need& n = r->need[r->nneed++];
        memset(&n, 0, sizeof n);
        n.location = t.location ? 1u : 0u;
        n.offset = t.offset;
        n.format = t.format;
        n.w = t.width; n.h = t.height;
        n.pitch = t.pitch;
        n.unit = (uint8_t)u;
        n.ppfx_wrote = S.ppfx_targets.count((uint64_t)n.location << 32 | n.offset) ? 1 : 0;
        if (!n.ppfx_wrote) {
            // a texture no record wrote: its bytes now, if small (a static
            // texture; a surface other work renders is handed over instead) --
            // its whole mip chain, as the engine reads it
            rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
            const uint32_t nlv = t.cubemap ? 0 : rsx_texture_mip_chain(n.format, n.w, n.h, t.mipmap, n.pitch, lv);
            const uint32_t bytes = nlv ? lv[nlv - 1].offset + lv[nlv - 1].tl.face_bytes : 0;
            const uint32_t ea = cellGcmResolveLocated(n.location == 0, n.offset);
            if (bytes && bytes <= (256u << 10) && ea_ok(ea, bytes)) {
                VtxRange& tr = r->tex[r->ntex++];
                tr.location = u;   // here: the unit
                tr.offset = n.offset; tr.bytes = bytes; tr.at = (uint32_t)r->tdata.size();
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
               "producer_threads %llu local_copies %llu\n",
            (unsigned long long)s.calls, (unsigned long long)s.ppfx_calls, (unsigned long long)s.nested_ppfx,
            (unsigned long long)s.foreign_state_calls, (unsigned long long)s.other_thread_calls,
            (unsigned long long)s.unmodelled, (unsigned long long)s.producer_threads,
            (unsigned long long)s.local_copies);
    fprintf(f, "records draws %llu clears %llu (depth-only clears %llu) frames %llu put %llu walked %llu done %llu\n",
            (unsigned long long)s.draws, (unsigned long long)s.clears, (unsigned long long)s.depth_clears,
            (unsigned long long)s.frames,
            (unsigned long long)q.put, (unsigned long long)q.walked, (unsigned long long)q.done);
    fprintf(f, "wraps_in_call %llu ends_at_ring_end %llu\n", (unsigned long long)s.wraps_in_call,
            (unsigned long long)s.ends_at_ring_end);
    fprintf(f, "recorder_ms %.3f queue_full_waits %llu queue_full_wait_ms %.3f\n", s.rec_ns / 1e6,
            (unsigned long long)q.full_waits, q.full_wait_us / 1e3);
    fprintf(f, "walker matched %llu missed %llu foreign_draws %llu foreign_clears %llu lap_delta_changes %llu\n",
            (unsigned long long)s.matched, (unsigned long long)s.missed, (unsigned long long)s.foreign_draws,
            (unsigned long long)s.foreign_clears, (unsigned long long)s.lap_delta_changes);
    fprintf(f, "asp calls %llu store_restores %llu store_mismatches %llu ppfx_draws_after_untracked_pass_apply %llu\n",
            (unsigned long long)s.asp_calls, (unsigned long long)s.asp_restores,
            (unsigned long long)s.asp_store_mismatch, (unsigned long long)A.unknown_draws);
    fprintf(f, "side_effects reports %llu report_copies %llu\n", (unsigned long long)s.reports,
            (unsigned long long)s.transfers);
    fprintf(f, "crosscheck draws %llu state %llu consts %llu vp %llu fp %llu vtx %llu\n",
            (unsigned long long)s.xc_draws, (unsigned long long)s.xc_regs, (unsigned long long)s.xc_consts,
            (unsigned long long)s.xc_vp, (unsigned long long)s.xc_fp, (unsigned long long)s.xc_vtx,
            (unsigned long long)s.xc_tex);
    fprintf(f, "handoffs %llu failed %llu\n", (unsigned long long)s.handoffs, (unsigned long long)s.handoff_fail);
    fprintf(f, "native frames_with_records %llu\n", (unsigned long long)s.n_frames);
    fprintf(f, "native draws %llu clears %llu unsupported %llu batches %llu pipelines %llu images %llu (%.1f MB) "
               "static_textures %llu own_target_snapshots %llu images_replaced %llu\n",
            (unsigned long long)s.n_draws, (unsigned long long)s.n_clears, (unsigned long long)s.n_unsupported,
            (unsigned long long)s.n_batches, (unsigned long long)s.n_pipelines, (unsigned long long)s.n_images,
            s.n_image_bytes / 1048576.0, (unsigned long long)s.n_static_textures, (unsigned long long)s.n_feedback,
            (unsigned long long)s.n_retired);
    fprintf(f, "native_ms record_cpu %.1f gpu %.1f wait_walker %.1f wait_signal %.1f compare %.1f\n",
            s.cpu_record_ns / 1e6, s.gpu_ns / 1e6, s.wait_walker_us / 1e3, s.wait_signal_us / 1e3, s.compare_ns / 1e6);
    fprintf(f, "native_gpu_ms draws %.1f copies %.1f readbacks %.1f other %.1f | barriers %llu (full %llu) "
               "handoff_copies %llu snapshot_copies %llu\n",
            s.gpu_draw_ns / 1e6, s.gpu_copy_ns / 1e6, s.gpu_readback_ns / 1e6, s.gpu_other_ns / 1e6,
            (unsigned long long)s.n_barriers, (unsigned long long)s.n_barriers_full,
            (unsigned long long)s.n_handoff_copies, (unsigned long long)s.n_snapshot_copies);
    fprintf(f, "walker_ms bridge %.1f of which crosscheck %.1f\n", s.bridge_ns / 1e6, s.xcheck_ns / 1e6);
    fprintf(f, "compare differ_in_frames_the_crosscheck_flagged %llu differ_elsewhere %llu\n",
            (unsigned long long)s.cmp_differ_inherited,
            (unsigned long long)(s.cmp_differ - s.cmp_differ_inherited));
    // every comparison asked for ends as exactly one of these
    fprintf(f, "compare wanted %llu equal %llu differ %llu skipped_no_engine_copy %llu skipped_no_readback %llu "
               "skipped_extent %llu skipped_not_drawn %llu unfinished %llu | of them after a structure change: "
               "wanted %llu compared %llu | finals %llu finals_equal %llu structure_triggers %llu\n",
            (unsigned long long)s.cmp_wanted, (unsigned long long)s.cmp_equal, (unsigned long long)s.cmp_differ,
            (unsigned long long)s.cmp_skip_no_ref, (unsigned long long)s.cmp_skip_no_readback,
            (unsigned long long)s.cmp_skip_extent, (unsigned long long)s.cmp_skip_not_drawn,
            (unsigned long long)s.cmp_unfinished, (unsigned long long)s.cmp_change_wanted,
            (unsigned long long)s.cmp_change_done, (unsigned long long)s.cmp_finals,
            (unsigned long long)s.cmp_finals_equal, (unsigned long long)s.cmp_triggers);
    fprintf(f, "compare_queue waits %llu wait_ms %.1f | handoff no_slot %llu copy_failed %llu "
               "slot_waits %llu slot_wait_ms %.1f\n",
            (unsigned long long)s.cmp_queue_waits, s.cmp_queue_wait_ns / 1e6,
            (unsigned long long)s.hand_no_slot, (unsigned long long)s.hand_copy_fail,
            (unsigned long long)s.hand_slot_waits, s.hand_slot_wait_ns / 1e6);
    {
        std::lock_guard<std::mutex> g(s_unm_lock);
        for (auto& kv : s_unmodelled_by) fprintf(f, "unmodelled %08X %llu\n", kv.first, (unsigned long long)kv.second);
    }
    {
        std::vector<std::string> xs;
        bridge_report(xs);
        if (mode() == 2) render_report(xs);
        for (auto& l : xs) fprintf(f, "%s\n", l.c_str());
    }
    std::lock_guard<std::mutex> g(s_rep_lock);
    for (auto& l : s_rep_lines) fprintf(f, "%s\n", l.c_str());
    fclose(f);
}

}  // namespace

void report_add(const char* line)
{
    std::lock_guard<std::mutex> g(s_rep_lock);
    if (s_rep_lines.size() < 20000) s_rep_lines.push_back(line);
}

void start()
{
    reset_state(&S.st);
    read_env();
    const char* rp = getenv("DOTHEROES_PPFX_REPORT");
    snprintf(s_rep_path, sizeof s_rep_path, "%s", rp && *rp ? rp : "ppfx_report.txt");
    if (!mode()) return;
    atexit(write_report);
    // runs first (atexit is last in, first out): the native renderer finishes
    // what was walked and the comparisons in flight, so the report counts
    // what is left as unfinished
    if (mode() == 2) atexit([] { render_drain(15000); report_add("report: final, after the native renderer's drain"); });
    // a test's stop ends the process without exit handlers: the same there
    g_dotheroes_stop_hook = [] {
        if (mode() == 2) { render_drain(15000); report_add("report: final, after the native renderer's drain (test stop)"); }
        write_report();
    };
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
    if (addr == 0x00963EBCu) {   // the game's memcpy(dst, src, n): only copies into local memory matter
        const uint32_t dst = (uint32_t)ctx->gpr[3], src = (uint32_t)ctx->gpr[4], n = (uint32_t)ctx->gpr[5];
        if (dst >= kLocalEa && dst - kLocalEa < 0x10000000u && n && n <= (64u << 20) && ea_ok(src, n)) {
            std::lock_guard<std::mutex> g(s_lock);
            shadow_cpu_write(dst - kLocalEa, vm_base + src, n);
            stats().local_copies++;
        }
        // a diagnostic: the game's copies into RSX local memory below the
        // first PPFX intermediate (the display buffers among them) and from
        // there, by caller -- what a snapshot does with the frame
        const bool to_low = dst >= kLocalEa && dst - kLocalEa < 0x01AE0000u;
        const bool from_low = src >= kLocalEa && src - kLocalEa < 0x01AE0000u;
        if ((to_low || from_low) && n) {
            static std::map<uint64_t, uint64_t>* seen = new std::map<uint64_t, uint64_t>;
            static std::mutex m;
            std::lock_guard<std::mutex> g(m);
            const uint64_t key = (uint64_t)(uint32_t)ctx->lr << 32 | (to_low ? 1u : 0u) | (from_low ? 2u : 0u);
            if ((*seen)[key]++ == 0 && seen->size() < 64) {
                char line[200];
                snprintf(line, sizeof line, "memcpy %s local memory below the targets: lr %08X dst %08X src %08X bytes %u "
                         "frame %u at %.3f s", to_low && from_low ? "within" : to_low ? "into" : "out of",
                         (uint32_t)ctx->lr, dst, src, n, S.frame, now_s());
                report_add(line);
            }
        }
        return;
    }
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
    // asp's sampler binds that ran since the last call: their units' address
    // modes are in asp's state store now
    asp_resolve();
    if (asp_fn(addr)) {
        if (mode() || s_pw) asp_call(addr, ctx);
        st.rec_ns += now_ns() - t0;
        return;
    }
    if (who) { st.ppfx_calls++; if (who == 2) st.nested_ppfx++; }
    else st.foreign_state_calls++;
    S.by = who ? kByPpfx : kByGame;
    if (who && !S.ring_ctx) S.ring_ctx = (uint32_t)ctx->gpr[3];
    // DOTHEROES_PPFX_TRACE_LOAD=1 (a diagnostic, with RSX_FRAME_TRACE=arm:..):
    // the frame trace starts at the walker's next flip after PPFX's first call
    // following 30 frames without one -- an area's first frames, state-triggered
    if (who) {
        static int tl = -1;
        static uint32_t last_ppfx_frame = 0;
        if (tl < 0) { const char* e = getenv("DOTHEROES_PPFX_TRACE_LOAD"); tl = e && *e == '1'; }
        if (tl == 1 && S.frame >= last_ppfx_frame + 30) {
            rsx_frame_trace_arm();
            tl = 2;
            char line[160];
            snprintf(line, sizeof line, "recorder: frame trace armed at frame %u (the first PPFX call after %u frames "
                     "without one), record %llu next", S.frame, S.frame - last_ppfx_frame,
                     (unsigned long long)queue().put.load());
            report_add(line);
        }
        last_ppfx_frame = S.frame;
    }
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
    case 0x006A75D8:   // cellGcmSetTextureBorderColor (ctx, index, color)
        S.st.samp[(uint32_t)ctx->gpr[4] & 15].border = (uint32_t)ctx->gpr[5];
        break;
    case 0x006A674C: {  // cellGcmSetScissor (ctx, x, y, w, h)
        Scissor& sc = S.st.scissor;
        sc.x = (uint16_t)ctx->gpr[4]; sc.y = (uint16_t)ctx->gpr[5]; sc.w = (uint16_t)ctx->gpr[6]; sc.h = (uint16_t)ctx->gpr[7];
        break;
    }
    case 0x006A56BC: {  // cellGcmSetAntiAliasingControl (ctx, enable, alphaToCoverage, alphaToOne, sampleMask)
        Raster& r = S.st.raster;
        r.aa_enable = (uint8_t)((uint32_t)ctx->gpr[4] & 1); r.alpha_to_coverage = (uint8_t)((uint32_t)ctx->gpr[5] & 1);
        r.alpha_to_one = (uint8_t)((uint32_t)ctx->gpr[6] & 1); r.sample_mask = (uint16_t)ctx->gpr[7];
        break;
    }
    case 0x006A90EC:   // cellGcmSetVertexDataBase (ctx, baseOffset, baseIndex)
        S.st.vdata.base_offset = (uint32_t)ctx->gpr[4]; S.st.vdata.base_index = (uint32_t)ctx->gpr[5];
        break;
    case 0x006A96E8:   // cellGcmSetPolygonOffset (ctx, factor, units)
        S.st.raster.poly_offset_factor = (float)ctx->fpr[1]; S.st.raster.poly_offset_units = (float)ctx->fpr[2];
        break;
    case 0x006A1D1C:   // the report copy (a memory-to-memory transfer): a side effect, no rendering state
        if (who) {
            st.transfers++;
            // PPFX's occlusion query's end (func_007C2EF4, docs/NATIVE_R4.md):
            // cellGcmSetTransferReportData(ctx, io offset, index) copies report
            // `index` into the query's result buffer (its EA at +0x4C of the
            // query object, which the caller holds in r31), whose last word
            // the caller has just set to 1 (pending) -- the copy clears it.
            // Who reads the buffer and the report: the watch says (it has to
            // run: DOTHEROES_PPFX_TARGET_WATCH or DOTHEROES_PPFX_EA_WATCH).
            const uint32_t obj = (uint32_t)ctx->gpr[31];
            const uint32_t buf = (uint32_t)ctx->lr == 0x007C2F94u && ea_ok(obj + 0x4C) ? g32(obj + 0x4C) : 0;
            const uint32_t idx = (uint32_t)ctx->gpr[5];
            if (ea_ok(buf, 16)) ppfx_watch_add_ea(buf, 16);
            static int said = 0;
            if (said++ < 8) {
                char line[220];
                snprintf(line, sizeof line, "report copy: lr %08X query %08X result buffer %08X (%08X %08X %08X %08X) "
                         "io %08X report index %u frame %u", (uint32_t)ctx->lr, obj, buf,
                         ea_ok(buf, 16) ? g32(buf) : 0, ea_ok(buf, 16) ? g32(buf + 4) : 0,
                         ea_ok(buf, 16) ? g32(buf + 8) : 0, ea_ok(buf, 16) ? g32(buf + 12) : 0,
                         (uint32_t)ctx->gpr[4], idx, S.frame);
                report_add(line);
            }
        }
        break;
    case 0x006A1E28: if (mode() && who) build_draw(ctx); break;
    case 0x006A5CF4:   // a clear: a record when it clears colour (depth and stencil are not PPFX's output)
        if (!who) break;
        if (mode() && ((uint32_t)ctx->gpr[4] & 0xF0u)) build_clear(ctx);
        else st.depth_clears++;
        break;
    case 0x0069FC38: case 0x0069DD60: break;
    default: {
        const bool done = enc_simple(addr, (uint32_t)ctx->gpr[4]);
        if (!done && !no_state(addr) && who) { st.unmodelled++; std::lock_guard<std::mutex> u(s_unm_lock); s_unmodelled_by[addr]++; }
        break;
    }
    }
    st.rec_ns += now_ns() - t0;
}
