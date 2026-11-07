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
//
// R4 (docs/NATIVE_R4.md), DOTHEROES_PPFX_BYPASS (with DOTHEROES_PPFX_NATIVE=1):
// each entry point PPFX reaches has a class (r4_class): its commands are
// what a record carries and only PPFX's own draws read them (bypassed), or
// other work reads what it leaves (kept: the surface, the scissor and clear
// values a kept clear uses, depth and stencil clears, the vertex program and
// its constants, the occlusion query's calls), or it writes no command
// (runs). =1: a PPFX call to a bypassed entry point does not run -- the
// observer tells the lifted function to return at once (ps3recomp 0112) --
// once the native renderer is ready, from the next frame; a bypassed draw or
// colour clear becomes a record with a stream marker in its place
// (bridge_marker). Calls made inside a kept call run. =verify: nothing is
// bypassed; each PPFX call to a bypassed entry point runs with its results
// checked against what bypassing it would leave (r4_verify_exit: the return
// value, the registers the ABI keeps, every store its body makes).
// =alt:<n>: bypassed in alternate blocks of n frames (R3, R4, R3, ...).

#include "ppu_context.h"
#include "ppfx_native.h"
#include "rsx_dispatch.h"
#include "rsx_texture_layout.h"
#include "ps3emu/osd.h"          // ps3_osd_post: an unimplemented graphics call, on screen (R8)
#include "ps3emu/ps3_strict.h"   // ps3_strict_fail: the same, fatal under PS3_STRICT=1
#include <atomic>

#include <chrono>
#include <map>
#include <set>
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
extern "C" uint32_t cellGcm_title_context_pub(void);   // R6: the title's CellGcmContextData
extern "C" int cellGcm_display_buffer_pub(uint32_t id, uint32_t* offset, uint32_t* pitch, uint32_t* w, uint32_t* h);
extern "C" void cellGcm_set_flip_gate(int (*gate)(void));
extern "C" int g_dotheroes_native_present;   // main.cpp: the walker leaves the frames' presents to the native renderer
extern "C" uint32_t cellGcm_ea2io_pub(uint32_t ea);
extern "C" uint32_t cellGcmResolveLocated(int local, uint32_t offset);
extern "C" uint32_t rsx_fp_texture_units(const uint8_t* ucode, uint32_t max_bytes);
extern "C" uint32_t rsx_fp_program_size(const uint8_t* ucode, uint32_t max_bytes);
extern "C" uint32_t rsx_vp_program_size_instrs(const uint8_t* ucode, uint32_t max_bytes);
extern "C" void (*g_dotheroes_stop_hook)(void);   // dev_state.cpp: run before a test's stop
extern "C" void rsx_frame_trace_arm(void);   // cellGcmSys.c: RSX_FRAME_TRACE=arm:<frames>:<path>
extern "C" void ppfx_watch_add_ea(uint32_t ea, uint32_t n);   // ppfx_watch.cpp
extern "C" int g_ppu_obs_store;                                // r1_observe.cpp (ps3recomp 0112)
extern "C" void cellGcm_set_native_frontend(int on);           // cellGcmSys.c (R7): no walker, the host's front end
void func_006CAD4C(ppu_context* ctx);                          // the lift (C++ linkage): asp's fragment heap set-up (R7 calls it)
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);    // the lift's tail calls
extern "C" void cellGcm_set_guest_read_hook(int (*hook)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t));
extern "C" uint32_t cellGcm_native_fe_recycles(void);
extern "C" uint32_t cellGcm_native_fe_get_polls(void);

namespace ppfx {
namespace {
// R7 (the game thread's cost): switches read on every observed call, cached
// in plain variables behind always-inlined getters (R6's getters were real
// calls on that path: 0.7% of the game thread)
int r6_skip_read();
int asp_bypass_read();
int s_r6_skip_c = -1, s_asp_bypass_c = -1;
__attribute__((always_inline)) inline int r6_skip() { return s_r6_skip_c >= 0 ? s_r6_skip_c : (s_r6_skip_c = r6_skip_read()); }
__attribute__((always_inline)) inline int asp_bypass() { return s_asp_bypass_c >= 0 ? s_asp_bypass_c : (s_asp_bypass_c = asp_bypass_read()); }
}  // namespace

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
    // the rest of the reset render state (the GPU's, as libgcm documents it)
    Raster& r = s->raster;
    r.cull_face = 0x0405u;                // CELL_GCM_BACK
    r.depth_func = 0x0201u;               // CELL_GCM_LESS
    r.front_poly_mode = r.back_poly_mode = 0x1B02u;   // CELL_GCM_POLYGON_MODE_FILL
    r.blend_sf_rgb = r.blend_sf_a = 1;    // CELL_GCM_ONE
    r.blend_df_rgb = r.blend_df_a = 0;    // CELL_GCM_ZERO
    r.blend_eq_rgb = r.blend_eq_a = 0x8006u;   // CELL_GCM_FUNC_ADD
    r.alpha_func = r.stencil_func = 0x0207u;   // CELL_GCM_ALWAYS
    r.stencil_fail = r.stencil_zfail = r.stencil_zpass = 0x1E00u;   // CELL_GCM_KEEP
    r.stencil_func_mask = r.stencil_write_mask = 0xFF;
    r.line_width = 8;                     // 1.0 (6.3 fixed point)
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

// A counter only the producer's thread adds to: no locked add (the report
// thread reads it whenever)
inline void bump(std::atomic<uint64_t>& a, uint64_t d = 1) { a.store(a.load(std::memory_order_relaxed) + d, std::memory_order_relaxed); }

const uint32_t kLocalEa = 0xC0000000u;
const uint32_t kPpfxLo = 0x007497E0u, kPpfxHi = 0x007D3E38u;   // PPFX's code
const uint32_t kGcmLo = 0x0069C000u, kGcmHi = 0x006B0000u;     // libgcm's

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint32_t g32(uint32_t ea) { return be32(vm_base + ea); }
void p32(uint32_t ea, uint32_t v)
{
    vm_base[ea] = (uint8_t)(v >> 24); vm_base[ea + 1] = (uint8_t)(v >> 16); vm_base[ea + 2] = (uint8_t)(v >> 8); vm_base[ea + 3] = (uint8_t)v;
}
void p8(uint32_t ea, uint8_t v) { vm_base[ea] = v; }
void p16(uint32_t ea, uint16_t v) { vm_base[ea] = (uint8_t)(v >> 8); vm_base[ea + 1] = (uint8_t)v; }
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

// R7 (the game thread's cost): each word also carries the shader object
// whose upload or patch wrote it last (R6 kept that in an unordered_map per
// word: 1.5% of the game thread), and the page last looked up is kept.
struct ShadowPage { uint8_t b[4096]; uint64_t ep[1024]; uint32_t own[1024]; };
std::unordered_map<uint32_t, ShadowPage*> s_shadow;
uint64_t s_epoch;   // transfers so far
uint32_t s_shadow_last = ~0u;
ShadowPage* s_shadow_last_p;

ShadowPage* shadow_page(uint32_t off)
{
    if ((off >> 12) == s_shadow_last) return s_shadow_last_p;
    ShadowPage*& p = s_shadow[off >> 12];
    if (!p) { p = new ShadowPage; memset(p, 0, sizeof *p); }
    s_shadow_last = off >> 12; s_shadow_last_p = p;
    return p;
}

// the words [off, off + n) written: one page lookup a page (off word-aligned)
void shadow_write(uint32_t off, const uint8_t* src, uint32_t n, uint64_t ep)
{
    for (uint32_t i = 0; i + 4 <= n;) {
        const uint32_t a = off + i;
        ShadowPage* p = shadow_page(a);
        uint32_t take = 0x1000 - (a & 0xFFF);
        if (take > ((n - i) & ~3u)) take = (n - i) & ~3u;
        if (!take) break;
        memcpy(p->b + (a & 0xFFF), src + i, take);
        for (uint32_t w = (a & 0xFFF) >> 2, e = w + take / 4; w < e; w++) p->ep[w] = ep;
        i += take;
    }
}
// the shader object a word of program memory belongs to (0: none known)
void owner_set(uint32_t off, uint32_t n, uint32_t obj)
{
    for (uint32_t i = 0; i + 4 <= n; i += 4) shadow_page(off + i)->own[((off + i) & 0xFFF) >> 2] = obj;
}
uint32_t owner_get(uint32_t off) { return shadow_page(off)->own[(off & 0xFFF) >> 2]; }

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
    uint32_t vc_dirty[16];           // R6: constants set since the last record took them
    uint32_t vp[kVpWords];           // vertex program memory as the uploads made it
    BlobP fp;                        // the bound fragment program's bytes
    BlobP vp_blob; uint64_t vp_gen, vp_blob_gen; uint32_t vp_blob_start;
    uint32_t fp_units;               // texture units the bound fragment program samples
    std::unordered_set<uint64_t> ppfx_targets;   // (location << 32 | offset) a record has written
    uint32_t frame;
    uint32_t final_frame;            // R5: the frame whose final composite has been recorded (~0u none)
    uint32_t ring_ctx;               // the command buffer's CellGcmContextData (PPFX's calls' first argument)
    uint8_t by;                      // who makes the call being encoded (By)
    // the producer
    uint32_t tid;
    // R4
    bool r4_frame;                   // this frame's PPFX calls are bypassed
    bool skipping;                   // the call being encoded is (its record is bypassed)
    uint32_t kept_depth;             // inside a kept PPFX call: everything runs
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
    // R5: asp's fragment programs, uploaded into local memory (offset -> bytes)
    std::unordered_map<uint32_t, uint32_t> fp_bytes;
    uint64_t fp_odd_patches;         // constant patches of an odd word count (asp transfers one word more)
    uint8_t fp_bound;                // an asp program pair is bound: its draws take the program from the shadow
    uint32_t fp_bound_obj;           // the bound fragment shader object
    std::unordered_map<uint32_t, uint32_t> fp_last_obj;   // per offset: the object uploaded there last
    std::unordered_map<uint32_t, uint64_t> fp_slot_epoch; // per offset: the shadow's epoch when it last got a program
    // R6: per word of program memory, the shader object whose upload or
    // constant patch wrote it last -- a resident bind keeps only its own
    // object's patches (R5's start-up gap: another program's patches kept)
    // (the words' owners: in the shadow's pages, ShadowPage::own)
    uint64_t fp_foreign_patch_words;  // words a resident bind took back from another object's patch
    uint64_t fp_const_calls;          // R6: 0x006C9C74's fragment-parameter calls (not vertex constants)
    uint64_t fp_quiet_binds;          // R6: pair binds that emitted nothing (the program unchanged)
    uint64_t samp_bias_binds;         // R6: sampler binds whose filter's LOD bias would not be 0 (not modelled)
    uint64_t pass_colour;             // R6: colour passes started (depth off, a white clear)
    uint64_t fp_stale_draws;         // draws whose bound program is not the one last uploaded at its offset
    uint64_t fp_resident_binds;      // binds of a program asp did not upload first (taken from the object)
    // R5: asp's state store as its setters left it, in the explicit form
    // (every setter writes its item into the command buffer and into the
    // item's slot; asp re-emits ranges of the store), and the inputs a draw
    // unbinds after its draw command
    State stored;
    uint8_t stored_init;
    uint32_t stored_valid;           // the groups a setter has set (the rest: what asp's set-up put there, unknown)
    uint32_t unbind_after;
    // R5: what asp's sampler bind last wrote per unit (it writes an item only
    // when it differs from that), and whether the store's samplers are known
    // (its global state block, rebuilt by 0x006D0B98, sets them to the reset
    // values; binds then write what changes)
    Sampler samp_written[16];
    uint32_t samp_written_valid;
    uint8_t stored_samp_valid;
    uint64_t rebuilds, samp_restores;
    std::unordered_set<uint64_t> asp_targets;   // surfaces asp draws into (location << 32 | offset)
    Rec::AspClear clears[32];        // asp's clears waiting for its next draw
    uint32_t nclears;
    uint64_t clears_dropped, clears_merged;
} A;

// Calls on other threads than the producer's touch the shadow (an inline
// transfer by other code) and the frame count; the lock keeps them apart. It
// is released while the producer waits for room in the queue.
std::mutex s_lock;
std::unique_lock<std::mutex>* s_held;   // the producer's hold on it, during a call
std::mutex s_unm_lock;                   // s_unmodelled_by, which the report reads
// every address a record drew or cleared into (the report: what the native
// renderer holds an image of, against what PPFX's draws sample)
struct TargetSeen { uint64_t records; uint32_t w, h, format, last_frame; };
std::map<uint64_t, TargetSeen> s_targets_seen;
std::unordered_map<uint64_t, BlobP> s_blobs;   // programs by content

// a fast 64-bit hash (the shape of xxHash64's lanes; the renderer's fast_hash)
uint64_t blob_hash(const uint8_t* p, size_t n)
{
    const uint64_t P1 = 11400714785074694791ull, P2 = 14029467366897019727ull, seed = 0xB10Bull;
    auto rotl = [](uint64_t x, int r) { return (x << r) | (x >> (64 - r)); };
    uint64_t a = seed + P1 + P2, b = seed + P2, c = seed, d = seed - P1;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t w[4]; memcpy(w, p + i, 32);
        a = rotl(a + w[0] * P2, 31) * P1; b = rotl(b + w[1] * P2, 31) * P1;
        c = rotl(c + w[2] * P2, 31) * P1; d = rotl(d + w[3] * P2, 31) * P1;
    }
    uint64_t h = rotl(a, 1) + rotl(b, 7) + rotl(c, 12) + rotl(d, 18) + n;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, p + i, 8); h ^= rotl(w * P2, 31) * P1; h = rotl(h, 27) * P1 + P2; }
    for (; i < n; i++) { h ^= p[i] * P1; h = rotl(h, 11) * P2; }
    return h;
}

BlobP blob_of(const uint8_t* p, size_t n)
{
    const uint64_t h = blob_hash(p, n) ^ (uint64_t)n;
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
    A.fp_bound = 0;   // R5: a libgcm bind: the program is this one, not asp's
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

// cellGcmSetVertexProgramParameter(ctx, CgBinaryParameter*, const float*):
// the parameter's registers from its values, word(i) the i-th value word, as
// the body (0x006A39EC; 030, its jump table at 0x006A3A90) decodes them. A
// parameter whose resource (+0x04) is CG_UNDEFINED (0xCB8), or whose type
// is not one of these, sets nothing. Vectors go to the parameter's resIndex
// (+0x0C; -1 none), the components it lacks 0 (CG_FLOAT1 0x443 as CG_FLOAT).
// Matrices go row by row: each row is a parameter of its own after it (+0x30
// apart: its resource, its resIndex), the values a row apart whether the row
// is loaded or not (3 words a row for 3x3 and 4x3, 4 for 3x4 and 4x4), a row
// CG_UNDEFINED or at -1 skipped, w 0 for three columns. A 4x4 whose rows
// are all defined is one load at the first row's register, nothing if any
// row's is -1. (R3-R6 modelled the four vectors and the 4x4 at the
// parameter's own resIndex, every other type as one float4: vp_new_cases
// counts every parameter where the two decodes differ: the run's report.)
template <class W> void vp_param(uint32_t par, W word)
{
    if (!ea_ok(par, 16)) return;
    const uint32_t type = g32(par + 0), resource = g32(par + 4), res = g32(par + 12);
    auto set = [](uint32_t reg, const uint32_t v[4]) {
        if (reg >= 512) return;
        for (int k = 0; k < 4; k++) S.vconst[reg][k] = v[k];
        S.vconst_written[reg >> 5] |= 1u << (reg & 31);
        S.vc_dirty[reg >> 5] |= 1u << (reg & 31);
    };
    uint32_t comps = 0, rows = 0, cols = 0;
    switch (type) {
    case 0x415: case 0x443: comps = 1; break;   // CG_FLOAT, CG_FLOAT1
    case 0x416: comps = 2; break;               // CG_FLOAT2
    case 0x417: comps = 3; break;               // CG_FLOAT3
    case 0x418: comps = 4; break;               // CG_FLOAT4
    case 0x423: rows = 3; cols = 3; break;      // CG_FLOAT3x3
    case 0x424: rows = 3; cols = 4; break;      // CG_FLOAT3x4
    case 0x427: rows = 4; cols = 3; break;      // CG_FLOAT4x3
    case 0x428: rows = 4; cols = 4; break;      // CG_FLOAT4x4
    default: break;
    }
    if (!comps && !rows) { stats().unmodelled++; stats().vp_new_cases++; return; }
    if (resource == 0xCB8u) { stats().vp_new_cases++; return; }
    uint32_t v[4];
    if (comps) {
        if (type == 0x443) stats().vp_new_cases++;
        for (uint32_t k = 0; k < 4; k++) v[k] = k < comps ? word(k) : 0;
        if (res != 0xFFFFFFFFu) set(res, v);
        return;
    }
    if (!ea_ok(par, 0x30 * (rows + 1))) return;
    bool all_defined = true;
    for (uint32_t r = 0; r < rows; r++) if (g32(par + 0x30 * (r + 1) + 4) == 0xCB8u) all_defined = false;
    if (rows == 4 && cols == 4 && all_defined) {
        for (uint32_t r = 0; r < 4; r++) if (g32(par + 0x30 * (r + 1) + 0x0C) == 0xFFFFFFFFu) { stats().vp_new_cases++; return; }
        const uint32_t base = g32(par + 0x3C);
        if (base != res) stats().vp_new_cases++;
        for (uint32_t r = 0; r < 4; r++) {
            for (uint32_t k = 0; k < 4; k++) v[k] = word(4 * r + k);
            if (base + r < 512) set(base + r, v);
        }
        return;
    }
    stats().vp_new_cases++;
    for (uint32_t r = 0; r < rows; r++) {
        const uint32_t row = par + 0x30 * (r + 1), at = g32(row + 0x0C);
        if (g32(row + 4) == 0xCB8u || at == 0xFFFFFFFFu) continue;
        for (uint32_t k = 0; k < 4; k++) v[k] = k < cols ? word(cols * r + k) : 0;
        set(at, v);
    }
}
void enc_vp_const(ppu_context* c)
{
    const uint32_t par = (uint32_t)c->gpr[4], val = (uint32_t)c->gpr[5];
    vp_param(par, [val](uint32_t i) { const uint32_t at = val + 4 * i; return ea_ok(at) ? g32(at) : 0u; });
}

// R6 (=all): cellGcmSetVertexProgram's body (0x006A4E50; 030, loc_006A514C)
// ends by setting each parameter's default. For every CgBinaryParameter
// (0x30 bytes; the program's +0x0C their count, +0x10 the array's offset)
// with a default value (+0x14, an offset in the program) whose variability
// (+0x08) is CG_UNIFORM or CG_CONSTANT (0x1006, 0x1007), it copies the
// default's first 16 bytes into its frame (+0x70) and calls
// cellGcmSetVertexProgramParameter(ctx, parameter, that copy) -- an observed
// call, which is how the recorder saw these constants while the body ran
// (R4 kept the body: "its default constants, read by others"). A matrix
// parameter reads 64 bytes from the copy: the frame's next 48 bytes are the
// caller's r24-r29 as the body saved them. With the body skipped (=all) the
// same constants are set here, in the same order, after the program.
struct VpDefault { uint32_t par; uint32_t w[16]; };
template <class F> void vp_defaults_each(ppu_context* c, F each)
{
    const uint32_t prog = (uint32_t)c->gpr[4];
    if (!ea_ok(prog, 0x14)) return;
    const int32_t n = (int32_t)g32(prog + 0x0C);
    const uint32_t arr = prog + g32(prog + 0x10);
    for (int32_t i = 0; i < n && i < 4096; i++) {
        const uint32_t p = arr + 0x30u * (uint32_t)i;
        if (!ea_ok(p, 0x30)) break;
        const uint32_t dv = g32(p + 0x14);
        if (!dv || g32(p + 0x08) - 0x1006u > 1u) continue;
        const uint32_t src = prog + dv;
        VpDefault d;
        d.par = p;
        for (int k = 0; k < 4; k++) d.w[k] = ea_ok(src + 4 * k, 4) ? g32(src + 4 * k) : 0;
        for (int k = 0; k < 6; k++) {
            const uint64_t v = c->gpr[24 + k];
            d.w[4 + 2 * k] = (uint32_t)(v >> 32); d.w[5 + 2 * k] = (uint32_t)v;
        }
        each(d);
    }
}
void vp_defaults(ppu_context* c)
{
    stats().vpd_programs++;
    vp_defaults_each(c, [](const VpDefault& d) {
        vp_param(d.par, [&d](uint32_t i) { return i < 16 ? d.w[i] : 0u; });
        stats().vpd_sets++;
        if (g32(d.par) == 0x428) stats().vpd_matrices++;
    });
}
// its verify (DOTHEROES_R6_VP_VERIFY=1, with DOTHEROES_R6_KEEP=6A4E50 so the
// body runs): the defaults predicted at the entry, each nested parameter
// call's parameter and value words compared with the next prediction
bool vp_verify()
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DOTHEROES_R6_VP_VERIFY"); on = e && *e == '1'; }
    return on;
}
std::vector<VpDefault> s_vpv;
size_t s_vpv_next = 0;
bool s_vpv_open = false;
void vp_verify_enter(ppu_context* c)
{
    s_vpv.clear(); s_vpv_next = 0; s_vpv_open = true;
    vp_defaults_each(c, [](const VpDefault& d) { s_vpv.push_back(d); });
    stats().vpdv_programs++;
}
void vp_verify_nested(ppu_context* c)   // 0x006A39EC called from 0x006A529C
{
    if (!s_vpv_open) return;
    const uint32_t par = (uint32_t)c->gpr[4], val = (uint32_t)c->gpr[5];
    if (s_vpv_next >= s_vpv.size()) { stats().vpdv_differ++; return; }
    const VpDefault& d = s_vpv[s_vpv_next++];
    bool same = d.par == par;
    const uint32_t regs = ea_ok(par, 4) && g32(par) == 0x428 ? 4 : 1;
    for (uint32_t i = 0; same && i < 4 * regs; i++) same = ea_ok(val + 4 * i, 4) && g32(val + 4 * i) == d.w[i];
    if (same) stats().vpdv_equal++; else stats().vpdv_differ++;
}
void vp_verify_exit()
{
    if (!s_vpv_open) return;
    if (s_vpv_next != s_vpv.size()) stats().vpdv_count_differ++;
    s_vpv_open = false;
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
    case 0x006C6F88: case 0x006CB070: case 0x006D3190: case 0x006D32D0: case 0x006C9C74:   // R5
    case 0x006CA3D0: case 0x006C99B0:
    case 0x006C048C: case 0x006C05AC: case 0x006C06CC: case 0x006C08F0: case 0x006C5170: case 0x006D11BC:   // R5, more
    case 0x006C0F60: case 0x006C2D3C: case 0x006C4284: case 0x006C4508: case 0x006C8A34: case 0x006CADE4:
    case 0x006C9624: case 0x006D0E84:
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

int asp_record_call(uint32_t addr, ppu_context* c);   // R5, below
bool asp_probe_enter(uint32_t addr, ppu_context* c);    // R5's setter probe, below
int asp_call_state(uint32_t addr, ppu_context* c);

int asp_records_on();
void asp_mirror(uint32_t addr);

bool asp_state_bypass_enter(uint32_t addr, ppu_context* c);

int asp_state_skip(uint32_t addr, ppu_context* c);   // R6, below
bool asp_state_verify_enter(uint32_t addr, ppu_context* c);
int r6_skip();
int asp_call(uint32_t addr, ppu_context* c)
{
    bump(stats().asp_calls);
    if (r6_skip() == 3) { const int k = asp_state_skip(addr, c); if (k) return k - 1; }   // R6: =all
    if (r6_skip() == 2 && asp_state_verify_enter(addr, c)) {   // R6's verify: the body runs, the R5 path records
        int r5 = asp_record_call(addr, c);
        if (!r5) asp_call_state(addr, c);
        if (asp_records_on()) asp_mirror(addr);
        (void)r5;
        return 2;
    }
    const bool probed = asp_probe_enter(addr, c) || asp_state_bypass_enter(addr, c);
    int ret;
    if (const int r5 = asp_record_call(addr, c)) ret = r5 - 1;
    else ret = asp_call_state(addr, c);
    if (asp_records_on()) asp_mirror(addr);
    return probed ? 2 : ret;
}

// ---- R5: asp's state store, as items. A group of items per slot of asp's
// render-state table (TOC -0x2D98: u16 offsets into the store), as the
// setters read it from their own code.
enum AspGroup { kGAlpha, kGBlend, kGCmask, kGCull, kGDepth, kGDmask, kGPmode, kGPoffset, kGScissor, kGStencil,
                kGSwmask, kGViewport, kGroups };
const uint16_t kGroupSlot[kGroups] = { 0x04, 0x10, 0x20, 0x28, 0x30, 0x34, 0x40, 0x48, 0x50, 0x54, 0x5C, 0x64 };

void asp_copy_group(int g, const State& from, State& to)
{
    const Raster& f = from.raster;
    Raster& t = to.raster;
    switch (g) {
    case kGAlpha: t.alpha_test = f.alpha_test; t.alpha_func = f.alpha_func; t.alpha_ref = f.alpha_ref; break;
    case kGBlend:
        t.blend = f.blend; t.blend_sf_rgb = f.blend_sf_rgb; t.blend_df_rgb = f.blend_df_rgb; t.blend_sf_a = f.blend_sf_a;
        t.blend_df_a = f.blend_df_a; t.blend_eq_rgb = f.blend_eq_rgb; t.blend_eq_a = f.blend_eq_a;
        break;
    case kGCmask: t.color_mask = f.color_mask; t.color_mask_by = kByAsp; break;
    case kGCull: t.cull = f.cull; t.cull_face = f.cull_face; t.front_face = f.front_face; break;
    case kGDepth: t.depth_test = f.depth_test; t.depth_func = f.depth_func; break;
    case kGDmask: t.depth_write = f.depth_write; break;
    case kGPmode: t.front_poly_mode = f.front_poly_mode; t.back_poly_mode = f.back_poly_mode; break;
    case kGPoffset: t.poly_offset_fill = f.poly_offset_fill; t.poly_offset_factor = f.poly_offset_factor; t.poly_offset_units = f.poly_offset_units; break;
    case kGScissor: to.scissor = from.scissor; break;
    case kGStencil:
        t.stencil_test = f.stencil_test; t.stencil_func = f.stencil_func; t.stencil_ref = f.stencil_ref;
        t.stencil_func_mask = f.stencil_func_mask; t.stencil_fail = f.stencil_fail; t.stencil_zfail = f.stencil_zfail;
        t.stencil_zpass = f.stencil_zpass;
        break;
    case kGSwmask: t.stencil_write_mask = f.stencil_write_mask; break;
    case kGViewport: to.viewport = from.viewport; break;
    default: break;
    }
}

// A setter has run: its item is in the store now, as it is in the state
void asp_mirror(uint32_t addr)
{
    if (!A.stored_init) { reset_state(&A.stored); A.stored_init = 1; }
    int g = -1;
    switch (addr) {
    case 0x006C05AC: case 0x006C4284: g = kGAlpha; break;
    case 0x006C048C: case 0x006C5298: g = kGBlend; break;
    case 0x006C02E8: g = kGCmask; break;
    case 0x006C4D1C: g = kGCull; break;
    case 0x006C49BC: g = kGDepth; break;
    case 0x006C01C4: g = kGDmask; break;
    case 0x006C06CC: g = kGPmode; break;
    case 0x006C4628: g = kGPoffset; break;
    case 0x006C08F0: case 0x006C5170: g = kGScissor; break;
    case 0x006C0A70: case 0x006C4508: g = kGStencil; break;
    case 0x006C0E38: g = kGSwmask; break;
    case 0x006C0F60: g = kGViewport; break;
    case 0x006D0B98: {   // the global state block rebuilt: the store's samplers at their reset values
        for (uint32_t k = 0; k < 16; k++) {
            Sampler& t = A.stored.samp[k];
            t.enable = 0; t.min_lod = t.max_lod = 0; t.max_aniso = 0;
            t.min_filter = t.mag_filter = 1; t.conv = 1; t.bias = 0; t.filter_signed = 0;   // nearest
            t.wrap_s = t.wrap_t = 1; t.wrap_r = 3; t.unsigned_remap = 0; t.gamma = 0; t.zfunc = 6;
            t.border = 0; t.address_by = kByAsp;
        }
        A.stored_samp_valid = 1;
        A.stored.scissor = { 0, 0, 0x0FFF, 0x0FFF };   // and its scissor off (the store's re-emission writes 0x0FFF0000)
        A.stored_valid |= 1u << kGScissor;
        A.rebuilds++;
        return;
    }
    default: return;
    }
    asp_copy_group(g, S.st, A.stored);
    A.stored_valid |= 1u << g;
}

// asp re-emits [src, src + 4 * words) of its store: the items whose slots lie there are the store's again
void asp_restore(uint32_t begin, uint32_t src, uint64_t hi)
{
    if (!A.stored_init) return;
    const uint32_t tbl = g32(A.toc - 0x2D98u);
    for (int g = 0; g < kGroups; g++) {
        if (!(A.stored_valid >> g & 1) || !ea_ok(tbl + kGroupSlot[g], 2)) continue;
        const uint32_t slot = begin + (g16(tbl + kGroupSlot[g]) & ~3u);
        if (slot >= src && slot < hi) { asp_copy_group(g, A.stored, S.st); stats().asp_group_restores++; }
    }
    // the units' samplers (their address mode's slot: TOC -0x2D54)
    if (A.stored_samp_valid)
        for (uint32_t u = 0; u < 16; u++) {
            const uint32_t slot = asp_address_slot(begin, u);
            if (slot && slot >= src && slot < hi) { S.st.samp[u] = A.stored.samp[u]; A.samp_restores++; }
        }
}

int asp_call_state(uint32_t addr, ppu_context* c)
{
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
        if (asp_records_on()) asp_restore(begin, src, hi);   // R5: every item, from the setters' own values
        if (r6_skip() == 3) break;   // R6: the store's contents are not maintained -- the model alone
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
    return 0;
}

// ---- R4: the bypass ---------------------------------------------------------------

// DOTHEROES_PPFX_BYPASS: 0 off, 1 bypass, 2 verify, 3 alternate blocks of s_alt_n frames
int s_bypass = -1;
uint32_t s_alt_n = 60;
int bypass_mode()
{
    if (s_bypass < 0) {
        const char* e = getenv("DOTHEROES_PPFX_BYPASS");
        s_bypass = !e || !*e || *e == '0' ? 0 : !strcmp(e, "verify") ? 2 : !strncmp(e, "alt:", 4) ? 3 : *e == '1' ? 1 : 0;
        if (s_bypass == 3) { s_alt_n = (uint32_t)atoi(e + 4); if (!s_alt_n) s_alt_n = 60; }
        if (s_bypass && mode() != 2 && s_bypass != 2) s_bypass = 0;   // bypassing needs the native renderer
    }
    return s_bypass;
}

enum Cls : uint8_t { kClsRun = 0, kClsBypass = 1, kClsKeep = 2 };

// What R4 does with a PPFX call to this entry point (docs/NATIVE_R4.md
// section 2) and, for a kept one, why.
Cls r4_class(uint32_t addr, const ppu_context* c, const char** why)
{
    *why = "";
    switch (addr) {
    // PPFX's own state and draws: the records carry them, only PPFX's draws read them
    case 0x0069E6E0: case 0x0069E7EC: case 0x0069E99C: case 0x0069F19C: case 0x0069F2B8: case 0x0069FC38:
    case 0x006A1E28: case 0x006A56BC: case 0x006A5EC0: case 0x006A5FE0: case 0x006A6070: case 0x006A6100:
    case 0x006A62B4: case 0x006A6344: case 0x006A650C: case 0x006A659C: case 0x006A662C: case 0x006A66BC:
    case 0x006A6810: case 0x006A68A0: case 0x006A6DA0: case 0x006A6E30: case 0x006A6F74: case 0x006A7004:
    case 0x006A7124: case 0x006A71B4: case 0x006A7244: case 0x006A7364: case 0x006A75D8: case 0x006A7674:
    case 0x006A7744: case 0x006A7834: case 0x006A7BA4: case 0x006A7CF8: case 0x006A7EB0: case 0x006A90EC:
    case 0x006A96E8: case 0x006A5C30:
        return kClsBypass;
    // a clear: colour only is a record; one that clears depth or stencil is
    // kept (asp's scene is drawn into the depth buffer PPFX clears)
    case 0x006A5CF4:
        if ((uint32_t)c->gpr[4] & 0x03u) { *why = "depth/stencil clear: other work's draws use it"; return kClsKeep; }
        return kClsBypass;
    case 0x0069F944: *why = "surface: asp's scene and the game's draws render into the target PPFX sets"; return kClsKeep;
    case 0x006A674C: *why = "scissor: a kept clear's extent"; return kClsKeep;
    case 0x006A5DA0: *why = "clear colour: a kept clear's value"; return kClsKeep;
    case 0x006A5E30: *why = "clear depth/stencil value: a kept clear's value"; return kClsKeep;
    case 0x006A39EC: *why = "vertex program constants: asp's and the game's programs read them"; return kClsKeep;
    case 0x006A4E50: *why = "vertex program: its default constants, read by others"; return kClsKeep;
    case 0x006A8020: case 0x006A817C: case 0x006A820C: case 0x006A829C: case 0x006A1D1C:
        *why = "occlusion query: kept as the port runs it today"; return kClsKeep;
    default:
        return kClsRun;   // no command (helpers, wrappers, the flush): the body runs
    }
}

struct BpCount {
    uint64_t ppfx = 0, skipped = 0, kept = 0, inside_kept = 0, before_ready = 0, verified = 0;
    // verify: what running the body left that bypassing it would not
    uint64_t r3_same = 0, r3_diff = 0, f1_diff = 0, nonvol_diff = 0, ctx_moved = 0, callbacks = 0;
    uint64_t st_cmd = 0, st_ctx = 0, st_stack = 0, st_caller_abi = 0, st_other = 0, words = 0;
    uint64_t vol_diff = 0;
    uint32_t last_r3_diff_lr = 0, last_other_ea = 0, last_other_lr = 0;
    const char* why = "";
};
std::map<uint32_t, BpCount> s_bp;
std::mutex s_bp_lock;   // the report reads it

// A call whose body runs with its return seen (ps3recomp 0112's 2): a kept
// call (inside it everything runs) or a verified one.
struct VEntry {
    uint32_t addr, lr, sp, ctx_ea, begin, end, cur;
    uint8_t kind;   // 0 kept, 1 verified, 2 asp's draw (R5), 3 R5's setter probe, 4 R5's state bypass
    uint32_t lap;
    uint64_t gpr[32];
    double fpr[32];
    uint8_t vr[12][16];   // v20..v31
    uint32_t cr;
    size_t store_from;
};
std::vector<VEntry> s_vstack;
std::vector<std::pair<uint64_t, uint32_t>> s_stores;   // (ea, bytes) of verified bodies' stores
uint32_t s_verifying;

void r4_ctx_fields(VEntry& v, uint32_t ctx_ea)
{
    v.ctx_ea = ctx_ea;
    v.begin = v.end = v.cur = 0;
    if (ea_ok(ctx_ea, 16)) { v.begin = g32(ctx_ea); v.end = g32(ctx_ea + 4); v.cur = g32(ctx_ea + 8); }
}

void r4_verify_enter(uint32_t addr, ppu_context* c)
{
    VEntry v;
    v.addr = addr; v.lr = (uint32_t)c->lr; v.sp = (uint32_t)c->gpr[1]; v.kind = 1;
    r4_ctx_fields(v, (uint32_t)c->gpr[3]);
    memcpy(v.gpr, c->gpr, sizeof v.gpr);
    memcpy(v.fpr, c->fpr, sizeof v.fpr);
    memcpy(v.vr, &c->vr[20], sizeof v.vr);
    v.cr = c->cr;
    v.store_from = s_stores.size();
    s_vstack.push_back(v);
    s_verifying++;
    g_ppu_obs_store = 1;
}

void r4_verify_exit(const VEntry& v, ppu_context* c)
{
    std::lock_guard<std::mutex> g(s_bp_lock);
    BpCount& k = s_bp[v.addr];
    k.verified++;
    // the return value: bypassing leaves r3 as it was
    if (c->gpr[3] == v.gpr[3]) k.r3_same++;
    else { k.r3_diff++; k.last_r3_diff_lr = v.lr; }
    if (memcmp(&c->fpr[1], &v.fpr[1], 8)) k.f1_diff++;
    // what the ABI keeps across a call: r1, r2, r13-r31, f14-f31, v20-v31, CR2-CR4
    bool nv = c->gpr[1] != v.gpr[1] || c->gpr[2] != v.gpr[2] || ((c->cr ^ v.cr) & 0x00FFF000u) != 0;
    for (int r = 13; r < 32 && !nv; r++) nv = c->gpr[r] != v.gpr[r];
    for (int f = 14; f < 32 && !nv; f++) nv = memcmp(&c->fpr[f], &v.fpr[f], 8) != 0;
    if (!nv) nv = memcmp(&c->vr[20], v.vr, sizeof v.vr) != 0;
    if (nv) k.nonvol_diff++;
    // the registers the ABI does not keep (r0, r4-r12, CTR, the other CR fields): changed (only counted)
    bool vol = false;
    for (int r = 4; r <= 12 && !vol; r++) vol = c->gpr[r] != v.gpr[r];
    if (vol) k.vol_diff++;
    // the command buffer: where the body left its context
    VEntry now;
    r4_ctx_fields(now, v.ctx_ea);
    const bool callback = now.begin != v.begin || now.end != v.end || now.cur < v.cur;
    if (callback) k.callbacks++;
    if (now.cur != v.cur) k.ctx_moved++;
    if (!callback && now.cur > v.cur) k.words += (now.cur - v.cur) / 4;
    // every store the body made
    for (size_t i = v.store_from; i < s_stores.size(); i++) {
        const uint64_t ea = s_stores[i].first;
        const uint32_t n = s_stores[i].second;
        const bool cmd = (ea >= v.cur && ea + n <= (callback ? (uint64_t)v.end + 16 : (uint64_t)now.cur)) ||
                         (callback && ea >= now.begin && ea + n <= now.cur);
        if (cmd) k.st_cmd++;
        else if (ea == (uint64_t)v.ctx_ea + 8 && n == 4) k.st_ctx++;
        else if (ea < v.sp && ea + 0x10000 >= v.sp) k.st_stack++;            // the body's own frame
        else if (ea >= v.sp && ea + n <= (uint64_t)v.sp + 112) k.st_caller_abi++;   // LR/CR save, parameter save area
        else { k.st_other++; k.last_other_ea = (uint32_t)ea; k.last_other_lr = v.lr; }
    }
}

// What this PPFX call's body does: 0 runs, 1 bypassed, 2 runs, its return seen.
int r4_decide(uint32_t addr, ppu_context* c)
{
    const char* why;
    const Cls cls = r4_class(addr, c, &why);
    if (cls == kClsRun) return 0;
    // R6: the entry's counters found without the map and its lock after the
    // first call (map nodes do not move; the report reads them under the lock)
    static uint32_t c_addr[256];
    static BpCount* c_bp[256];
    const uint32_t h = (addr >> 2) & 255;
    BpCount* kp = c_addr[h] == addr ? c_bp[h] : nullptr;
    if (!kp) { std::lock_guard<std::mutex> g(s_bp_lock); kp = &s_bp[addr]; c_addr[h] = addr; c_bp[h] = kp; }
    BpCount& k = *kp;
    k.ppfx++;
    if (S.kept_depth) { k.inside_kept++; return 0; }
    if (cls == kClsKeep) {
        k.kept++; k.why = why;
        stats().r4_kept++;
        VEntry v; memset(&v, 0, sizeof v); v.addr = addr; v.kind = 0;
        s_vstack.push_back(v);
        S.kept_depth++;
        return 2;
    }
    if (bypass_mode() == 2) return 3;   // verify: the caller snapshots (outside this lock)
    if (!S.r4_frame) { k.before_ready++; return 0; }
    k.skipped++;
    stats().r4_skipped++;
    S.skipping = true;
    return 1;
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

// ---- R7: no walker (docs/NATIVE_R7.md) ----------------------------------------------
// DOTHEROES_R7=1: the native renderer is the title's whole GPU. Every call
// that wrote command words is replaced at its entry, nothing reaches the
// ring, no FIFO walker runs (port/main.cpp), and the records are consumed
// once the game's flush (cellGcmFlush, 0x006A336C) would have submitted their
// commands -- the queue's `flushed`. No stream markers.
#ifdef DOTHEROES_NATIVE_RELEASE
int r7() { return 1; }   // the release: no walker, so R7 always
bool rmark(uint32_t, uint64_t) { return true; }
#else
int r7()
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DOTHEROES_R7"); on = e && *e == '1'; }
    return on;
}
bool rmark(uint32_t ctx, uint64_t seq)
{
    if (r7()) return true;   // nothing walks the ring: no marker
    return bridge_marker(ctx, seq);
}
#endif

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

// The queue full: the walker must reach the records' commands (their
// markers) to make room, and it reads the ring only up to the put register,
// which the game moves at its flushes. With asp's records (R5) the queue
// can fill between two of the game's flushes (an area's first frames): the
// commands already written are made visible, as cellGcmFlush would -- the
// put register (the runtime's control area, VM_HLE_INJECT_BASE + 0x2000)
// moved forward to the context's current position, within the lap only.
extern "C" uint32_t ppu_hle_inject_base;
void r7_publish(bool forced);
void flush_for_room()
{
    if (r7()) { r7_publish(true); return; }
    if (!S.ring_ctx || !ea_ok(S.ring_ctx, 16)) return;
    const uint32_t ctl = ppu_hle_inject_base + 0x2000u;
    const uint32_t cur = cellGcm_ea2io_pub(g32(S.ring_ctx + 8)), put = g32(ctl);
    if (cur > put && cur - put < (16u << 20)) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        p32(ctl, cur);
        stats().flushes_for_room++;
    }
}

// a reused record's buffer: emptied, and let go past 64 KB (acquire, below)
template <class V> inline void trim_buf(V& v)
{
    if (v.capacity() * sizeof(typename V::value_type) > (64u << 10)) V().swap(v);
}

Rec* acquire()
{
    Queue& q = queue();
    const uint64_t p = q.put.load(std::memory_order_relaxed);
    // R6: DOTHEROES_R6_QUEUE_CAP=<n> (the backpressure test): the queue full at n records
    static uint64_t cap = 0;
    if (!cap) { const char* e = getenv("DOTHEROES_R6_QUEUE_CAP"); cap = e && atoi(e) > 0 && atoi(e) <= Queue::kCap ? (uint64_t)atoi(e) : Queue::kCap; }
    if (p - q.done.load(std::memory_order_acquire) >= cap) {
        const uint64_t t0 = now_ns();
        q.full_waits++;
        flush_for_room();
        if (s_held) s_held->unlock();
        std::unique_lock<std::mutex> lk(q.m);
        uint64_t waited = 0;
        while (p - q.done.load(std::memory_order_acquire) >= cap) {
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
    // R7 (memory over area cycles): a slot's buffers keep their capacity
    // when cleared, so one large input -- a Bink frame's planes, a long
    // vertex stream -- stayed held by its slot for good, and as the title's
    // movie replayed into other slots the process grew by 100-220 MB a
    // cycle (4,096 slots). Past 64 KB a buffer is let go here, where the slot
    // is being reused and no one else holds it.
    trim_buf(r->vdata);
    trim_buf(r->tdata);
    trim_buf(r->cdelta);
    trim_buf(r->fhash);
    r->game_arrays = 0;
    r->deferred = 0;
    r->rep_loc = 0;
    r->fh = 0;
    r->tex_at_flush = 0;
    return r;
}

// R6: a record's vertex constants: the delta since the previous record that
// took them (the native renderer applies it to its own copy, in record
// order), or all of them where the walker's cross-check reads them
int vconst_full()
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("DOTHEROES_R6_VCONST_FULL");
        const char* x = getenv("DOTHEROES_PPFX_XCHECK");
        // the cross-check reads a matched record's constants on the walker's
        // thread: check mode, or records not bypassed with it on
        on = (e && *e == '1') || mode() == 1 || (!(x && *x == '0') && !bypass_mode());
    }
    return on;
}
void take_vconst(Rec* r)
{
    memcpy(r->vconst_written, S.vconst_written, sizeof r->vconst_written);
    r->cdelta.clear();
    if (vconst_full()) {
        r->vc_full = 1;
        memcpy(r->vconst, S.vconst, sizeof r->vconst);
        memset(S.vc_dirty, 0, sizeof S.vc_dirty);
        return;
    }
    r->vc_full = 0;
    for (uint32_t w = 0; w < 16; w++) {
        uint32_t m = S.vc_dirty[w];
        while (m) {
            const uint32_t b0 = (uint32_t)__builtin_ctz(m);
            uint32_t b1 = b0;
            while (b1 < 32 && (m >> b1 & 1)) b1++;
            const uint32_t start = w * 32 + b0, count = b1 - b0;
            r->cdelta.push_back(start << 16 | count);
            r->cdelta.insert(r->cdelta.end(), &S.vconst[start][0], &S.vconst[start][0] + 4 * count);
            m = b1 >= 32 ? 0 : m & ~((1u << b1) - 1u);
        }
        S.vc_dirty[w] = 0;
    }
}

int r6_producers();
void asp_pend_clear(const Rec::AspClear& k);
void fill_common(Rec* r, uint32_t kind, ppu_context* c)
{
    r->vc_full = 0; r->cdelta.clear();   // a record without constants (a clear) carries none
    Queue& q = queue();
    r->seq = q.put.load(std::memory_order_relaxed);
    r->kind = kind;
    r->frame = S.frame;
    r->why = kWhyNone;
    r->st = S.st;
    r->nvr = 0; r->vdata.clear(); r->nneed = 0; r->ntex = 0; r->tdata.clear();
    memset(&r->walk, 0, sizeof r->walk);
    for (int16_t& k : r->walk.release_sem) k = -1;
    r->bypassed = S.skipping ? 1 : 0;
    // R6: the clears waiting (asp's, the game's) are done before this record
    r->naclear = 0;
    if (r6_producers()) {
        r->naclear = A.nclears;
        for (uint32_t i = 0; i < A.nclears; i++) r->aclear[i] = A.clears[i];
        A.nclears = 0;
    }
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
    // R6: no notify -- nothing waits for `put` (the native renderer waits for
    // the walker, a handback or a readback; the walker polls): each one was a
    // kernel wake on the game's thread when the native thread slept
    q.put.store(r->seq + 1, std::memory_order_release);
}

// The vertex program from its start slot through its last instruction, as
// the uploads left vertex program memory
BlobP vp_blob_now()
{
    const uint32_t start = S.st.vp.start;
    if (S.vp_blob_gen != S.vp_gen || !S.vp_blob || start != S.vp_blob_start) {
        if (start < 544) {
            const uint32_t instrs = rsx_vp_program_size_instrs((const uint8_t*)(S.vp + start * 4), (544 - start) * 16);
            S.vp_blob = instrs ? blob_of((const uint8_t*)(S.vp + start * 4), instrs * 16) : nullptr;
        } else S.vp_blob = nullptr;
        S.vp_blob_gen = S.vp_gen;
        S.vp_blob_start = start;
    }
    return S.vp_blob;
}

// Local memory's bytes as the shadow has them (else as memory has them)
void shadow_read(uint32_t off, std::vector<uint8_t>& out)
{
    uint32_t page = ~0u;
    const ShadowPage* p = nullptr;
    for (uint32_t i = 0; i + 4 <= out.size(); i += 4) {
        if ((off + i) >> 12 != page) {   // one lookup a page, not a word
            page = (off + i) >> 12;
            auto it = s_shadow.find(page);
            p = it == s_shadow.end() ? nullptr : it->second;
        }
        const uint32_t w = (off + i) & 0xFFF;
        if (p && p->ep[w >> 2]) memcpy(&out[i], p->b + w, 4);
        else memcpy(&out[i], vm_base + kLocalEa + off + i, 4);
    }
}

// R5: the fragment program an asp draw runs -- the one asp uploaded at the
// bound offset, with the patches made since, through its last instruction
BlobP asp_fp_now()
{
    const uint32_t off = S.st.fp.offset;
    auto it = A.fp_bytes.find(off);
    if (it == A.fp_bytes.end() || !it->second) return nullptr;
    // R6: the same program as the previous draw's when no transfer (upload,
    // patch, copy) has written the shadow since: no read, no hash, no allocation
    static uint32_t c_off = ~0u, c_bytes = 0;
    static uint64_t c_epoch = ~0ull;
    static BlobP c_blob;
    if (c_blob && off == c_off && it->second == c_bytes && s_epoch == c_epoch) return c_blob;
    static std::vector<uint8_t> buf;
    buf.resize(it->second & ~3u);
    shadow_read(off, buf);
    const uint32_t size = rsx_fp_program_size(buf.data(), (uint32_t)buf.size());
    if (size && size < buf.size()) buf.resize(size);
    c_blob = blob_of(buf.data(), buf.size());
    c_off = off; c_bytes = it->second; c_epoch = s_epoch;
    return c_blob;
}

void add_target_need(Rec* r)
{
    const Surface& sf = S.st.surface;
    if (r->kind == kDraw && (sf.color_format & 0x1Fu) == 0x08 && sf.width >= 1280) S.final_frame = S.frame;   // the final composite
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
    std::lock_guard<std::mutex> u(s_unm_lock);
    TargetSeen& t = s_targets_seen[key];
    t.records++; t.w = n.w; t.h = n.h; t.format = n.format; t.last_frame = S.frame;
}

void build_draw(ppu_context* c)
{
    Rec* r = acquire();
    fill_common(r, kDraw, c);
    r->prim = (uint32_t)c->gpr[4];
    r->first = (uint32_t)c->gpr[5];
    r->count = (uint32_t)c->gpr[6];
    take_vconst(r);
    r->vp = vp_blob_now();
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
        if (r7()) continue;   // R7: taken when consumed (r->deferred)
        r->vdata.insert(r->vdata.end(), vm_base + ea, vm_base + ea + bytes);
    }
    r->deferred = r7() ? 1 : 0;
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
        n.drawn = n.ppfx_wrote;
        if (!n.ppfx_wrote) {
            // a texture no record wrote: its bytes now, if small (a static
            // texture; a surface other work renders is handed over instead) --
            // its whole mip chain, as the engine reads it
            rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
            const uint32_t nlv = t.cubemap ? 0 : rsx_texture_mip_chain(n.format, n.w, n.h, t.mipmap, n.pitch, lv);
            const uint32_t bytes = nlv ? lv[nlv - 1].offset + lv[nlv - 1].tl.face_bytes : 0;
            const uint32_t ea = cellGcmResolveLocated(n.location == 0, n.offset);
            // DOTHEROES_PPFX_GUEST_AT_WALK=1 (a check of the walker's own
            // snapshot, ppfx_bridge.cpp guest_texture): none taken here
            static int at_walk = -1;
            if (at_walk < 0) { const char* e = getenv("DOTHEROES_PPFX_GUEST_AT_WALK"); at_walk = e && *e == '1'; }
            if (!at_walk && bytes && bytes <= (256u << 10) && ea_ok(ea, bytes)) {
                VtxRange& tr = r->tex[r->ntex++];
                tr.location = u;   // here: the unit
                tr.offset = n.offset; tr.bytes = bytes; tr.at = (uint32_t)r->tdata.size();
                if (!r7()) r->tdata.insert(r->tdata.end(), vm_base + ea, vm_base + ea + bytes);
            }
        }
    }
    publish(r);
    stats().draws++;
    if (r->bypassed) { stats().r4_records++; rmark((uint32_t)c->gpr[3], r->seq); }
}

void build_clear(ppu_context* c)
{
    Rec* r = acquire();
    fill_common(r, kClear, c);
    r->clear_mask = (uint32_t)c->gpr[4];
    add_target_need(r);
    publish(r);
    stats().clears++;
    if (r->bypassed) { stats().r4_records++; rmark((uint32_t)c->gpr[3], r->seq); }
}

// ---- R5: asp's records (docs/NATIVE_R4.md section 10) ---------------------------
// DOTHEROES_ASP_RECORD=1: asp's draws become records too, described from
// asp's own methods (its texture apply, stream binds, constant uploads,
// besides its state API above and the game's libgcm calls), built when
// asp's draw returns, matched with the engine's draw inside its commands and
// cross-checked there. Not rendered: R5's first step.

int bypass_mode();
// R5's bypass of asp's command writers (DOTHEROES_ASP_BYPASS=1: its draws):
// only with asp drawn natively, PPFX's images shared and R4's bypass of PPFX
// (=1): then nothing the engine would draw needs asp's scene
int asp_bypass_read()
{
    int on = -1;
    {
        const char* e = getenv("DOTHEROES_ASP_BYPASS");
        const char* n = getenv("DOTHEROES_ASP_NATIVE");
        const char* sh = getenv("DOTHEROES_ASP_SHARE");
        on = e && *e ? atoi(e) : 0;
        if (on && (!(n && *n == '1') || !(sh && *sh == '1') || mode() != 2 || bypass_mode() != 1)) {
            fprintf(stderr, "[ppfx] DOTHEROES_ASP_BYPASS needs DOTHEROES_ASP_NATIVE=1, DOTHEROES_ASP_SHARE=1 and "
                            "DOTHEROES_PPFX_BYPASS=1 (with DOTHEROES_PPFX_NATIVE=1): off\n");
            on = 0;
        }
    }
    return on;
}

int asp_records_on()
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DOTHEROES_ASP_RECORD"); on = e && *e == '1'; }
    return on;
}

// R6 (docs/NATIVE_R6.md section 1, DOTHEROES_R6_PRODUCERS=1, with
// DOTHEROES_ASP_RECORD=1): the graphics producers R5 left to the engine
// become records -- every frame from the title's first (asp's and the game's
// draws before PPFX's first call: start-up, the title, the first load), the
// game's own clears (its frame start's, of the display buffer and its depth)
// and the game's own draw-arrays draws (Bink's movie frames, the mesh path's
// draws without indices). Each carries the state it draws with explicitly,
// whoever set it (asp's API, the game's libgcm calls); with R5's bypass
// (DOTHEROES_ASP_BYPASS>=1) their bodies do not run.
int r6_producers()
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DOTHEROES_R6_PRODUCERS"); on = e && *e == '1' && asp_records_on(); }
    return on;
}

// R6's native present (DOTHEROES_R6_PRESENT=1, with the producers and R5's
// bypass): each cellGcmSetFlip is a record (kFlip) the native renderer
// presents the named display buffer at -- its own image of it, which the
// engine never draws -- and a flip completes for the game only once the
// native renderer has presented it as well (the flip gate). The walker's
// own present of those frames does not run.
int asp_bypass();
int r6_present()
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("DOTHEROES_R6_PRESENT");
        on = e && *e == '1';
        if (on && (!r6_producers() || asp_bypass() < 1)) {
            fprintf(stderr, "[ppfx] DOTHEROES_R6_PRESENT needs DOTHEROES_R6_PRODUCERS=1 and DOTHEROES_ASP_BYPASS>=1: off\n");
            on = 0;
        }
    }
    return on;
}

// R6 (=all): libgcm calls whose bodies did not run, by entry point and caller
// (PPFX's kept ones, the game's own) -- the proof of which bodies stop
struct SkipCount { uint32_t addr; uint64_t ppfx, game; };
SkipCount s_r6sk[256];
void r6_libgcm_skipped(uint32_t addr, int who)
{
    uint32_t h = (addr >> 2) * 2654435761u >> 24;
    for (uint32_t k = 0; k < 256; k++, h = (h + 1) & 255) {
        if (!s_r6sk[h].addr) s_r6sk[h].addr = addr;
        if (s_r6sk[h].addr == addr) { if (who) s_r6sk[h].ppfx++; else s_r6sk[h].game++; return; }
    }
}

// R6's occlusion reports (DOTHEROES_R6_OCCLUSION=1, with the native
// present): the zpass counting, clear-report, report and report-transfer
// calls -- PPFX's luminance probe (report 769) and the game's own query
// (report 128) -- do not run; their operations become records the native
// renderer carries out with its own occlusion queries over the counted
// draws, writing each report and transfer into guest memory when its GPU has
// the counts (docs/NATIVE_R6.md section 5). Off: as before -- the engine
// writes a fixed count at the report and never carries out the transfer.
int r6_occlusion()
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("DOTHEROES_R6_OCCLUSION");
        on = e && *e == '1';
        if (on && !r6_present()) {
            fprintf(stderr, "[ppfx] DOTHEROES_R6_OCCLUSION needs DOTHEROES_R6_PRESENT=1: off\n");
            on = 0;
        }
    }
    return on;
}
uint32_t r7_report_main();
bool occ_fn(uint32_t a)
{
    return a == 0x006A817Cu || a == 0x006A820Cu || a == 0x006A829Cu || a == 0x006A1D1Cu || a == 0x006A8020u;
}
void build_report(ppu_context* c, uint32_t op)
{
    const uint32_t ctx = (uint32_t)c->gpr[3];
    Rec* r = acquire();
    Queue& q = queue();
    r->seq = q.put.load(std::memory_order_relaxed);
    r->kind = kReport;
    r->frame = S.frame;
    r->why = kWhyNone;
    r->nvr = 0; r->vdata.clear(); r->nneed = 0; r->ntex = 0; r->tdata.clear(); r->naclear = 0;
    r->vc_full = 0; r->cdelta.clear();
    memset(&r->walk, 0, sizeof r->walk);
    for (int16_t& k : r->walk.release_sem) k = -1;
    r->rep_op = op;
    r->rep_type = op == kRepTransfer ? 1u : (uint32_t)c->gpr[4];
    r->rep_index = op == kRepReport ? (uint32_t)c->gpr[5] : op == kRepTransfer ? (uint32_t)c->gpr[5] : 0u;
    r->rep_count = op == kRepTransfer ? (uint32_t)c->gpr[6] : 1u;
    r->rep_io = op == kRepTransfer ? (uint32_t)c->gpr[4] : 0u;
    r->rep_loc = r7_report_main();   // R7: cellGcmSetReportLocation's choice (the game selects local before these)
    r->lap = r->lap_end = cellGcm_title_laps_pub(nullptr);
    r->io = r->io_end = cellGcm_ea2io_pub(g32(ctx + 8));
    r->io_alt = cellGcm_ea2io_pub(g32(ctx));
    r->bypassed = 1;
    publish(r);
    stats().occ_ops++;
    if (!rmark(ctx, r->seq)) stats().asp_marker_fail++;
}

int native_flip_gate()
{
    Queue& q = queue();
    return q.flips_presented.load(std::memory_order_acquire) >= q.flips_recorded.load(std::memory_order_acquire);
}

// cellGcmSetFlip(ctx, id) on the title's context: the frame's end, a record
// with a marker where the flip's commands begin (the walker meets it before
// the flip's own word)
void build_flip(ppu_context* c)
{
    const uint32_t id = (uint32_t)c->gpr[4] & 7, ctx = (uint32_t)c->gpr[3];
    uint32_t off = 0, pitch = 0, w = 0, h = 0;
    if (!cellGcm_display_buffer_pub(id, &off, &pitch, &w, &h) || !ea_ok(ctx, 16)) return;
    Rec* r = acquire();
    Queue& q = queue();
    r->seq = q.put.load(std::memory_order_relaxed);
    r->kind = kFlip;
    r->frame = S.frame ? S.frame - 1 : 0;   // the frame it ends (the count moved on at this call)
    r->why = kWhyNone;
    r->nvr = 0; r->vdata.clear(); r->nneed = 0; r->ntex = 0; r->tdata.clear(); r->naclear = 0;
    memset(&r->walk, 0, sizeof r->walk);
    for (int16_t& k : r->walk.release_sem) k = -1;
    r->flip_id = id; r->flip_offset = off; r->flip_pitch = pitch; r->flip_w = w; r->flip_h = h;
    // the clears still waiting (the game's frame-start clear in a frame that
    // drew nothing after it -- loads, transitions): done before the present,
    // as the engine did them before its flip
    if (r6_producers()) {
        r->naclear = A.nclears;
        for (uint32_t i = 0; i < A.nclears; i++) {
            const Rec::AspClear& k = A.clears[i];
            r->aclear[i] = k;
            // which clears wait for a flip: depth or stencil only, a colour
            // clear of the buffer this flip shows, a colour clear of another
            Stats& st = stats();
            const bool col = (k.mask & 0xF0u) != 0;
            if (!col) st.fclr_depth++;
            else if (!k.surface.color_location[0] && k.surface.color_offset[0] == off) st.fclr_shown++;
            else st.fclr_other++;
            static int said = 0;
            if (said < 12) {
                said++;
                char line[200];
                snprintf(line, sizeof line, "recorder: clear %02X of colour %u:%08X depth %08X done at the flip of frame %u "
                         "(display buffer 0:%08X)", k.mask, k.surface.color_location[0], k.surface.color_offset[0],
                         k.surface.depth_offset, S.frame ? S.frame - 1 : 0, off);
                report_add(line);
            }
        }
        A.nclears = 0;
    }
    r->vc_full = 0; r->cdelta.clear();
    r->lap = r->lap_end = cellGcm_title_laps_pub(nullptr);
    r->io = r->io_end = cellGcm_ea2io_pub(g32(ctx + 8));
    r->io_alt = cellGcm_ea2io_pub(g32(ctx));
    r->bypassed = 1;
    q.flips_recorded.fetch_add(1, std::memory_order_release);
    publish(r);
    if (!rmark(ctx, r->seq)) stats().asp_marker_fail++;
    static bool on = false;
    if (!on) {
        on = true;
        if (!r7()) cellGcm_set_flip_gate(native_flip_gate);   // R7: the native renderer completes the flips itself
        g_dotheroes_native_present = 1;
        char line[160];
        snprintf(line, sizeof line, "recorder: the native present takes the frames over at flip %llu (frame %u)",
                 (unsigned long long)q.flips_recorded.load(), S.frame);
        report_add(line);
    }
}


// ---- R7: the calls that wrote what the walker read (docs/NATIVE_R7.md) -------------------
// With every draw, state and program call already replaced at its entry
// (R6's =all), what still wrote words into the title's ring were thirteen
// libgcm functions and the flip (docs/NATIVE_R6.md section 3): the game's
// frame-start reset of eight render-state items, the performance markers,
// the wait-flip acquire, the profiling meter's report requests and their
// location -- and, outside the river, the labels Bink fences on, the
// reference command cellGcmFinish waits on, the 2D transfers. Each is
// replaced here by what it means: state, a record for the native renderer,
// or nothing (the markers, a jump). The flush publishes what is recorded.
// A word that still reaches the ring is found at the next flush (the
// detector) and counted by method -- the proof that nothing generates
// commands is that count staying 0.
struct R7State {
    uint32_t cur = 0;                 // the title's ring position the detector last saw
    uint32_t report_main = 0;         // cellGcmSetReportLocation: 1 main memory, 0 local (the reset)
    std::map<uint32_t, uint64_t> words;      // (subchannel << 16 | method) -> words found
    std::map<uint64_t, uint64_t> calls;      // (addr << 32 | lr) -> calls of the functions watched for the inventory
    std::mutex lock;                          // the report reads the maps
} R7S;

Rec* r7_rec(uint32_t kind)
{
    Rec* r = acquire();
    Queue& q = queue();
    r->seq = q.put.load(std::memory_order_relaxed);
    r->kind = kind;
    r->frame = S.frame;
    r->why = kWhyNone;
    r->nvr = 0; r->vdata.clear(); r->nneed = 0; r->ntex = 0; r->tdata.clear(); r->naclear = 0;
    r->vc_full = 0; r->cdelta.clear();
    memset(&r->walk, 0, sizeof r->walk);
    for (int16_t& k : r->walk.release_sem) k = -1;
    r->lap = r->lap_end = r->io = r->io_end = r->io_alt = 0;
    r->bypassed = 1;
    r->lab_index = r->lab_value = 0; r->lab_stage = 0;
    return r;
}

// R7 (section 2 of the R7 document): a record's small uncompressed guest
// textures -- at most 256 KB, not DXT, not an address a record drew -- taken
// into the record at the game's flush, on the game's thread: the walker's
// rule (R5: the water's texture is right taken where the RSX's front end
// reads it, wrong at the call), at the moment the walker could first have
// read them. The capture check found the game rewriting the water's texture
// between its flush and the native thread's capture; everything else it
// found stable. DOTHEROES_R7_TEX_AT_FLUSH=0 (development): at consumption.
}  // namespace
bool r7_take_at_flush(Rec* r)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DOTHEROES_R7_TEX_AT_FLUSH"); on = !(e && *e == '0'); }
    if (!on || r->tex_at_flush || (r->kind != kAspDraw && r->kind != kDraw)) return false;
    for (uint32_t k = 0; k < r->nneed; k++) {
        const Need& n = r->need[k];
        if (n.unit >= 16 || n.drawn) continue;
        const Texture& t = r->st.tex[n.unit];
        const uint32_t base = t.format & 0x9Fu;
        if (t.cubemap || (base >= 0x86 && base <= 0x88)) continue;
        rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
        const uint32_t nlv = rsx_texture_mip_chain(t.format, t.width, t.height, t.mipmap, t.pitch, lv);
        const uint32_t bytes = nlv ? lv[nlv - 1].offset + lv[nlv - 1].tl.face_bytes : 0;
        const uint32_t ea = cellGcmResolveLocated(t.location == 0, t.offset);
        if (!bytes || bytes > (256u << 10) || !ea_ok(ea, bytes)) continue;
        VtxRange* tr = nullptr;
        for (uint32_t j = 0; j < r->ntex; j++) if (r->tex[j].location == n.unit) tr = &r->tex[j];
        if (!tr) { if (r->ntex >= 16) continue; tr = &r->tex[r->ntex++]; }
        tr->location = n.unit; tr->offset = t.offset; tr->bytes = bytes; tr->at = (uint32_t)r->tdata.size();
        r->tdata.insert(r->tdata.end(), vm_base + ea, vm_base + ea + bytes);
        stats().r7_flush_tex++;
        stats().r7_flush_tex_bytes += bytes;
    }
    r->tex_at_flush = 1;
    return true;
}
namespace {

// The records so far go to the native renderer: the game's flush (or, with
// the queue full, what libgcm's ring-full callback would do -- counted apart)
void r7_publish(bool forced)
{
    Queue& q = queue();
    const uint64_t p = q.put.load(std::memory_order_acquire);
    // the small uncompressed textures, now (section 2); then, in development,
    // every input as it is at the flush
    for (uint64_t i = q.flushed.load(std::memory_order_relaxed); i < p; i++) {
        Rec* r = q.at(i);
        r7_take_at_flush(r);
        if (r7_capture_check()) { r7_input_hashes(r, r->fhash, nullptr); r->fh = 1; }
    }
    if (q.flushed.load(std::memory_order_relaxed) != p) {
        { std::lock_guard<std::mutex> g(q.m); q.flushed.store(p, std::memory_order_release); }
        q.cv.notify_all();
    }
    if (forced) stats().r7_forced++; else stats().r7_flushes++;
}

// R8: graphics work nothing executes. Without a walker, a command word in the
// title's ring (the detector) and a call of a libgcm writer R7 did not replace
// (r7_watch) are work lost, not deferred. Neither has happened in any scenario
// (NATIVE_R7.md), and neither may pass unnoticed if one ever does: each new
// kind is named on stderr at once and on screen (once a run), all are counted
// in a line at exit, and under PS3_STRICT=1 (development builds, every test
// run) the first is fatal, with its name.
std::atomic<uint64_t> s_lost_events{ 0 };
void r7_lost_summary()   // once: at exit, or at a test's stop (which skips the exit handlers)
{
    static std::atomic<int> said{ 0 };
    const uint64_t n = s_lost_events.load();
    if (n && said.exchange(1) == 0) fprintf(stderr, "[graphics] %llu unimplemented graphics calls or command-word batches were not executed this "
                           "run (the first of each kind above; the report lists them)\n", (unsigned long long)n);
}
void r7_unimplemented(const char* what, uint32_t addr, uint32_t lr, bool first_of_kind)
{
    s_lost_events.fetch_add(1);
    if (!first_of_kind) return;
    fprintf(stderr, "[graphics] not executed: %s %08X (from %08X, frame %u) -- the native renderer has no "
                    "implementation, so its work is lost\n", what, addr, lr, S.frame);
    fflush(stderr);
    static std::atomic<int> shown{ 0 };
    if (shown.exchange(1) == 0) {
        char t[PS3_OSD_TEXT_MAX];
        snprintf(t, sizeof t, "GRAPHICS CALL NOT IMPLEMENTED: %08X", addr);
        ps3_osd_post(t, 10000);
    }
    if (ps3_strict())
        ps3_strict_fail("unimplemented-graphics", "%s %08X from %08X at frame %u: no native implementation, "
                        "its work would be lost", what, addr, lr, S.frame);
}

// Words in the title's ring since the detector last looked: decoded by
// method (a header's subchannel and method, its count of data words), each
// counted, the first few named in the report with the frame
void r7_detect(uint32_t ctx)
{
    if (!ea_ok(ctx, 16)) return;
    const uint32_t beg = g32(ctx), cur = g32(ctx + 8);
    if (!R7S.cur) R7S.cur = beg;   // from the ring's start: anything since the title's set-up counts
    if (cur == R7S.cur) return;
    uint32_t a = cur > R7S.cur ? R7S.cur : beg;   // the ring went back to its start meanwhile
    uint64_t n = 0;
    std::unique_lock<std::mutex> g(R7S.lock);
    uint32_t new_key = 0, new_at = 0;
    bool fresh = false;
    while (a < cur && n < (1u << 20)) {
        const uint32_t h = g32(a);
        uint32_t cnt = (h >> 18) & 0x7FFu, key;
        if ((h & 0xE0000003u) == 0x20000000u || (h & 3u) == 1u || (h & 3u) == 2u) { key = 0xFFFF0000u | (h & 3u); cnt = 0; }
        else key = ((h >> 13) & 7u) << 16 | (h & 0x1FFCu);
        if (R7S.words[key] == 0 && !fresh) { fresh = true; new_key = key; new_at = a; }
        R7S.words[key] += 1 + cnt;
        n += 1 + cnt;
        a += 4u * (1 + cnt);
    }
    g.unlock();
    if (n) {   // the method's subchannel << 16 | method (0xFFFF000x: a jump or call); the word's address as "from"
        char what[96];
        snprintf(what, sizeof what, "%llu command words in the ring, a new method", (unsigned long long)n);
        r7_unimplemented(what, new_key, new_at, fresh);
    }
    g.lock();
    stats().r7_words += n;
    stats().r7_word_flushes++;
    static int said = 0;
    if (said < 24) {
        said++;
        char line[200];
        snprintf(line, sizeof line, "r7: %llu command words in the title's ring at the flush of frame %u (first header %08X at %08X)",
                 (unsigned long long)n, S.frame, cur > R7S.cur ? g32(R7S.cur) : g32(beg), cur > R7S.cur ? R7S.cur : beg);
        report_add(line);
    }
    R7S.cur = cur;
}

// The frame-start reset's state (func_00029618): set as each call's words
// said, counted where it differs from the reset value the native renderer assumes
uint64_t s_r7_reset_by[8];   // per setter (below, in order): calls not setting the reset value
uint32_t s_r7_reset_last[8];  // and the last such value
void r7_reset_note(bool differs, int which = 0, uint32_t value = 0)
{
    stats().r7_reset_calls++;
    if (differs) { stats().r7_reset_nondefault++; s_r7_reset_by[which & 7]++; s_r7_reset_last[which & 7] = value; }
}

// a function watched for the inventory (its body still runs): its calls by return address
void r7_watch(uint32_t addr, ppu_context* c)
{
    stats().r7_unknown_calls++;
    std::unique_lock<std::mutex> g(R7S.lock);
    const uint64_t k = (uint64_t)addr << 32 | (uint32_t)c->lr;
    bool fn_new = true;
    for (auto it = R7S.calls.lower_bound((uint64_t)addr << 32); it != R7S.calls.end() && (it->first >> 32) == addr; ++it)
        if (it->second) { fn_new = false; break; }
    const bool site_new = R7S.calls[k]++ == 0;
    g.unlock();
    r7_unimplemented("libgcm writer", addr, (uint32_t)c->lr, fn_new);
    g.lock();
    if (site_new) {
        char line[220];
        snprintf(line, sizeof line, "r7: watched %08X called from %08X at frame %u (r4 %08X r5 %08X r6 %08X r7 %08X r8 %08X r9 %08X r10 %08X)",
                 addr, (uint32_t)c->lr, S.frame, (uint32_t)c->gpr[4], (uint32_t)c->gpr[5], (uint32_t)c->gpr[6],
                 (uint32_t)c->gpr[7], (uint32_t)c->gpr[8], (uint32_t)c->gpr[9], (uint32_t)c->gpr[10]);
        report_add(line);
    }
}

// R7's entry points on the title's context: what ppfx_rec_observe answers
// (0 the body runs, 1 it does not), or -1 when the address is not one of them
int r7_call(uint32_t addr, ppu_context* c)
{
    Stats& st = stats();
    Raster& ra = S.st.raster;
    const uint32_t a4 = (uint32_t)c->gpr[4], a5 = (uint32_t)c->gpr[5], a6 = (uint32_t)c->gpr[6];
    switch (addr) {
    case 0x006A336Cu:   // cellGcmFlush: the body runs (put = current: nothing moved it)
        {   // DOTHEROES_R8_LOST_TEST=<frame> (development): at that frame's flush, one NOP method (header and
            // its data word) put in the ring as an unreplaced writer would -- the diagnostic's own test
            static const unsigned plant = [] { const char* e = getenv("DOTHEROES_R8_LOST_TEST"); return e ? (unsigned)atoi(e) : 0u; }();
            static bool planted = false;
            const uint32_t ctx = (uint32_t)c->gpr[3];
            if (plant && !planted && S.frame >= plant && ea_ok(ctx, 16)) {
                const uint32_t cur = g32(ctx + 8), end = g32(ctx + 4);
                if (ea_ok(cur, 8) && cur + 8 <= end) {
                    const uint32_t w[2] = { __builtin_bswap32(0x00040100u), 0 };   // NV4097 NO_OPERATION, count 1
                    memcpy(vm_base + cur, w, 8);
                    const uint32_t nc = __builtin_bswap32(cur + 8);
                    memcpy(vm_base + ctx + 8, &nc, 4);
                    planted = true;
                    fprintf(stderr, "[graphics] test: a NOP method planted in the ring at %08X, frame %u\n", cur, S.frame);
                }
            }
        }
        r7_detect((uint32_t)c->gpr[3]);
        r7_publish(false);
        return 0;
    case 0x0069DF80u: case 0x0069E0E0u:   // the performance markers' push and pop: inert strings
        st.r7_markers++;
        return 1;
    case 0x0069E504u:   // cellGcmSetJumpCommand: no ring to steer
        st.r7_jumps++;
        return 1;
    case 0x006A6220u: case 0x006AA3A0u:   // cellGcmSetLineWidth(ctx, width) and its Unsafe form: state
        ra.line_width = (uint16_t)(a4 & 0x1FFu);
        st.r7_line_width_calls++;
        return 1;
    case 0x006A7548u:   // cellGcmSetInvalidateTextureCache(ctx, value): the native renderer has no
                        // texture cache to invalidate -- guest textures are checked by their bytes
        st.r7_invalidates++;
        return 1;
    case 0x0069E474u: {   // cellGcmSetReferenceCommand(ctx, value)
        Rec* r = r7_rec(kRef);
        r->lab_value = a4;
        publish(r);
        st.r7_refs++;
        return 1;
    }
    case 0x0069E58Cu: {   // cellGcmSetWaitLabel(ctx, index, value): the wait-flip acquire among them
        Rec* r = r7_rec(kWait);
        r->lab_index = a4 & 0xFFu; r->lab_value = a5;
        publish(r);
        st.r7_waits++;
        return 1;
    }
    case 0x006A577Cu: case 0x006A5830u: case 0x006A5900u: {   // command, back-end and texture-read labels (ctx, index, value)
        Rec* r = r7_rec(kLabel);
        r->lab_index = a4 & 0xFFu; r->lab_value = a5;
        r->lab_stage = addr == 0x006A577Cu ? 0 : 1;
        publish(r);
        st.r7_labels++;
        return 1;
    }
    case 0x006A5624u: {   // a report request, NV4097_GET_REPORT (ctx, index): type 1, where the location says
        Rec* r = r7_rec(kReport);
        r->rep_op = kRepReport; r->rep_type = 1; r->rep_index = a4; r->rep_count = 1; r->rep_io = 0;
        r->rep_loc = R7S.report_main;
        publish(r);
        st.occ_ops++; st.r7_reports++;
        return 1;
    }
    case 0x006A9358u: {   // cellGcmSetReportLocation(ctx, location): the DMA word from libgcm's table
        const uint32_t tab = g32((uint32_t)c->gpr[2] - 0x35E0u);
        const uint32_t v = ea_ok(tab + (a4 << 2), 4) ? g32(tab + (a4 << 2)) : 0x66626660u;
        R7S.report_main = (v == 0xBAD68000u || v == 0xFEED0001u) ? 1u : 0u;
        st.r7_reportloc++;
        return 1;
    }
    // the frame start's reset
    case 0x006A5B8Cu:   // blend equation (ctx, colour, alpha)
        r7_reset_note(a4 != 0x8006u || a5 != 0x8006u, 0, a4 | a5 << 16);
        ra.blend_eq_rgb = (uint16_t)a4; ra.blend_eq_a = (uint16_t)a5;
        return 1;
    case 0x006A5F50u:   // colour mask of the extra targets (0x0370)
        r7_reset_note(a4 != 0, 1, a4);
        ra.color_mask_mrt = a4;
        return 1;
    case 0x006A6190u:   // front face (also Bink's draw set-up)
        r7_reset_note(a4 != 0x0901u, 2, a4);
        ra.front_face = (uint16_t)a4;
        return 1;
    case 0x006A6CF0u: {  // z min/max control (cullNearFar, zclamp, cullIgnoreW)
        const uint32_t v = (a4 & 0xFu) | (a5 & 0xFu) << 4 | (a6 & 0xFu) << 8;
        r7_reset_note(v != 1u, 3, v);
        ra.zminmax = v;
        return 1;
    }
    case 0x006A6EC0u:   // blend enable of the extra targets
        r7_reset_note((a4 | a5 | a6) & 1u, 4, a4 | a5 << 1 | a6 << 2);
        ra.blend_mrt = (uint8_t)((a4 & 1u) | (a5 & 1u) << 1 | (a6 & 1u) << 2);
        return 1;
    case 0x006A7094u:   // depth bounds test enable
        r7_reset_note(a4 & 1u, 5, a4);
        ra.depth_bounds = (uint8_t)(a4 & 1u);
        return 1;
    case 0x006A72D4u:   // two-sided stencil enable
        r7_reset_note(a4 & 1u, 6, a4);
        ra.two_sided_stencil = (uint8_t)(a4 & 1u);
        return 1;
    case 0x006A7484u:   // point sprite control (ctx, enable, rmode, texcoord mask)
        r7_reset_note(a4 & 1u, 7, a4);
        ra.point_sprite = (uint8_t)(a4 & 1u);
        ra.point_sprite_ctl = a4 | a5 << 1 | a6;
        return 1;
    case 0x0069FDD0u: {   // cellGcmSetTransferImage(ctx, mode, dstOffset, dstPitch, dstX, dstY, srcOffset, srcPitch,
                          //   srcX, srcY, width, height, bytesPerPixel): the last five on the caller's stack
        const uint32_t sp = (uint32_t)c->gpr[1];
        if (!ea_ok(sp + 0x70, 0x28)) return -1;
        Rec* r = r7_rec(kTransfer);
        // CELL_GCM_TRANSFER_MAIN_TO_LOCAL 0, LOCAL_TO_MAIN 1, LOCAL_TO_LOCAL 2, MAIN_TO_MAIN 3 (location 1 main, 0 local)
        const uint32_t mode = a4 & 3u;
        r->xs_loc = (mode == 0 || mode == 3) ? 1u : 0u;
        r->xd_loc = (mode == 1 || mode == 3) ? 1u : 0u;
        r->xd_off = a5; r->xd_pitch = a6; r->xd_x = (uint32_t)c->gpr[7]; r->xd_y = (uint32_t)c->gpr[8];
        r->xs_off = (uint32_t)c->gpr[9]; r->xs_pitch = (uint32_t)c->gpr[10];
        r->xs_x = g32(sp + 0x74); r->xs_y = g32(sp + 0x7C); r->x_w = g32(sp + 0x84); r->x_h = g32(sp + 0x8C);
        r->x_bpp = g32(sp + 0x94);
        publish(r);
        S.ppfx_targets.insert((uint64_t)r->xd_loc << 32 | r->xd_off);   // a native image there from now on
        st.r7_xfer_calls++;
        static int said = 0;
        if (said++ < 16) {
            char line[220];
            snprintf(line, sizeof line, "r7: transfer image %u:%08X (pitch %u) +%u,%u -> %u:%08X (pitch %u) +%u,%u, %ux%u, %u bytes "
                     "a pixel, frame %u, from %08X", r->xs_loc, r->xs_off, r->xs_pitch, r->xs_x, r->xs_y, r->xd_loc, r->xd_off,
                     r->xd_pitch, r->xd_x, r->xd_y, r->x_w, r->x_h, r->x_bpp, S.frame, (uint32_t)c->lr);
            report_add(line);
        }
        return 1;
    }
    // watched for the inventory: the other 2D transfers, the label helpers and the
    // Unsafe forms (no static callers), the FIFO reset -- their bodies run
    // (any word they write is the detector's), every caller named
    case 0x006A0D60u: case 0x006A1444u: case 0x006A17C4u: case 0x006A1974u: case 0x006A1B48u:
    case 0x006ABE64u: case 0x006ABFBCu: case 0x006ACCA8u: case 0x006AD264u: case 0x006AD3E8u: case 0x006AD4F0u:
    case 0x006AD65Cu:
        st.r7_xfer_calls++;
        r7_watch(addr, c);
        return 0;
    case 0x0069EDB0u: case 0x0069EEA4u: case 0x006AA034u: case 0x006AA074u: case 0x006AA0B4u: case 0x006AA110u:
    case 0x006AB60Cu: case 0x006AB68Cu: case 0x006A32D8u: case 0x006AA8A8u:
        r7_watch(addr, c);
        return 0;
    default:
        return -1;
    }
}

uint32_t r7_report_main() { return R7S.report_main; }

// R7: asp's fragment heap set up by its own code (func_006CAD4C: an
// allocation and an address-to-offset, no command), called from the
// upload's entry with the volatile registers given back as they were
void r7_heap_setup(ppu_context* c)
{
    uint64_t g[13];
    memcpy(g, c->gpr, sizeof g);
    const uint64_t lr = c->lr, ctr = c->ctr;
    const uint32_t cr = c->cr;
    func_006CAD4C(c);
    while (g_trampoline_fn) { void (*f)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; f(c); }
    memcpy(c->gpr, g, sizeof g);
    c->lr = lr; c->ctr = ctr; c->cr = cr;
    stats().r7_heap_setups++;
}

// asp's command context: the one PPFX writes to (the title's one)
uint32_t asp_ctx() { return S.ring_ctx; }

void asp_draw_enter(ppu_context* c)
{
    VEntry v;
    memset(&v, 0, sizeof v);
    v.addr = 0x006C6F88u; v.kind = 2;
    r4_ctx_fields(v, asp_ctx());
    v.lap = cellGcm_title_laps_pub(nullptr);
    v.gpr[3] = c->gpr[3];   // the draw object
    s_vstack.push_back(v);
}

// R5: the game's own cellGcmSetDrawIndexArray(ctx, mode, count, type,
// location, offset): drawn inside asp's scene with asp's programs (the
// reflection's stencil mask, the water, scene parts); a record like asp's,
// built at its return
void game_draw_enter(ppu_context* c)
{
    VEntry v;
    memset(&v, 0, sizeof v);
    v.addr = 0x006A2164u; v.kind = 2;
    r4_ctx_fields(v, (uint32_t)c->gpr[3]);
    v.lap = cellGcm_title_laps_pub(nullptr);
    for (int i = 3; i <= 8; i++) v.gpr[i] = c->gpr[i];
    s_vstack.push_back(v);
}

int asp_probe2();
// R5's probe of asp's draw (DOTHEROES_ASP_PROBE=2): the draw object and the
// words the draw wrote, for up to 24 distinct (primitive, index type, counts' shape)
void asp_draw_probe(const VEntry& v)
{
    const uint32_t self = (uint32_t)v.gpr[3];
    if (!ea_ok(self, 0x28) || !ea_ok(v.ctx_ea, 16)) return;
    const uint32_t cur = g32(v.ctx_ea + 8);
    if (cur < v.cur || cur - v.cur > 4096) return;
    const uint32_t key[] = { g32(self + 8), g32(self + 0x24), g32(self + 0x0C) == g32(self + 0x10) };
    static std::set<uint64_t> seen;
    if (seen.size() >= 24 || !seen.insert(fnv(key, sizeof key)).second) return;
    char line[2000];
    int at = snprintf(line, sizeof line, "asp probe2 draw this %08X:", self);
    for (uint32_t i = 0; i < 0x28; i += 4) at += snprintf(line + at, sizeof line - at, " %08X", g32(self + i));
    at += snprintf(line + at, sizeof line - at, " | words %u:", (cur - v.cur) / 4);
    for (uint32_t w = v.cur; w < cur && at < (int)sizeof line - 16; w += 4) at += snprintf(line + at, sizeof line - at, " %08X", g32(w));
    report_add(line);
}

// R5: what an asp draw draws (its object, vtable 0xFEA9D0: +0x08 the
// primitive in libgcm's numbering, +0x0C the index count, +0x10 the vertex
// count, +0x18 the index buffer's address, +0x24 its type) and what it
// reads and writes: its colour and depth targets, the units its fragment
// program samples, its streams' ranges. No bytes: the native renderer reads
// guest memory itself (docs/NATIVE_R5.md section 4).
// its streams: [0, vcount) of each enabled input
void asp_draw_streams(Rec* r)
{
    r->nvr = 0;
    for (uint32_t i = 0; i < 16; i++) {
        const VertexInput& in = r->st.vin[i];
        if (!in.size || !r->vcount) continue;
        VtxRange& vr = r->vr[r->nvr++];
        vr.location = in.location; vr.offset = (r->st.vdata.base_offset + (in.offset & 0x7FFFFFFFu)) & 0x0FFFFFFFu;
        vr.bytes = (r->vcount - 1) * in.stride + elem_bytes(in.type, in.size);
        vr.at = 0;
    }
}

void asp_draw_operation(Rec* r, uint32_t self)
{
    r->prim = r->first = r->count = r->idx_ea = r->idx_type = r->vcount = 0;
    if (self) {
        if (!ea_ok(self, 0x28)) { r->why = kWhyVertex; return; }
        r->prim = g32(self + 0x08);
        const uint32_t ic = g32(self + 0x0C);
        r->vcount = g32(self + 0x10);
        r->idx_ea = ic ? g32(self + 0x18) : 0;
        r->idx_type = g32(self + 0x24);
        r->count = ic ? ic : r->vcount;
        asp_draw_streams(r);
    }
    // its targets
    const Surface& sf = r->st.surface;
    if (sf.color_target) {
        Need& n = r->need[r->nneed++];
        memset(&n, 0, sizeof n);
        n.location = sf.color_location[0] ? 1u : 0u; n.offset = sf.color_offset[0]; n.pitch = sf.color_pitch[0];
        n.w = sf.width; n.h = sf.height; n.format = sf.color_format; n.unit = 0xFF;
    }
    if (sf.depth_format && (r->st.raster.depth_test || r->st.raster.stencil_test)) {
        Need& n = r->need[r->nneed++];
        memset(&n, 0, sizeof n);
        n.location = sf.depth_location ? 1u : 0u; n.offset = sf.depth_offset; n.pitch = sf.depth_pitch;
        n.w = sf.width; n.h = sf.height; n.format = sf.depth_format; n.unit = 0xFE;
    }
    // the units its fragment program samples
    const uint32_t units = r->fp ? rsx_fp_texture_units(r->fp->bytes.data(), (uint32_t)r->fp->bytes.size()) : 0;
    r->ntex = 0; r->tdata.clear();
    for (uint32_t u = 0; u < 16 && r->nneed < kMaxNeeds; u++) {
        if (!(units >> u & 1) || !r->st.samp[u].enable) continue;
        const Texture& t = r->st.tex[u];
        Need& n = r->need[r->nneed++];
        memset(&n, 0, sizeof n);
        n.location = t.location ? 1u : 0u; n.offset = t.offset; n.format = t.format;
        n.w = t.width; n.h = t.height; n.pitch = t.pitch; n.unit = (uint8_t)u;
        // a small uncompressed texture in guest memory that is no surface's:
        // taken by the walker at the draw's place in the stream (the bridge's
        // guest_texture) -- the game rewrites some after issuing the draw (the
        // water's), and the GPU reads them when it executes it
        const uint32_t base = t.format & 0x9Fu;
        const uint64_t key = (uint64_t)n.location << 32 | n.offset;
        n.drawn = S.ppfx_targets.count(key) || A.asp_targets.count(key) ? 1 : 0;
        n.ppfx_wrote = (base >= 0x86 && base <= 0x88) || n.drawn ? 1 : 0;
    }
    if (sf.color_target) A.asp_targets.insert((uint64_t)(sf.color_location[0] ? 1u : 0u) << 32 | sf.color_offset[0]);
}

// DOTHEROES_ASP_RACE_CHECK=1 (a diagnostic): the record's vertex and index
// bytes hashed as the game drew it; the native renderer hashes them again
// when it draws (stats: asp_race_changed)
uint64_t asp_race_hash(const Rec* r)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DOTHEROES_ASP_RACE_CHECK"); on = e && *e == '1'; }
    if (!on) return 0;
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < r->nvr; i++) {
        const uint32_t ea = cellGcmResolveLocated(r->vr[i].location == 0, r->vr[i].offset);
        if (ea_ok(ea, r->vr[i].bytes)) h = fnv(vm_base + ea, r->vr[i].bytes, h);
    }
    if (r->idx_ea && ea_ok(r->idx_ea, r->count * (r->idx_type == 2 ? 2u : 4u))) h = fnv(vm_base + r->idx_ea, r->count * (r->idx_type == 2 ? 2u : 4u), h);
    return h | 1;
}

void game_draw_operation(Rec* r, const VEntry& v)
{
    r->prim = (uint32_t)v.gpr[4] & 0xFF; r->count = (uint32_t)v.gpr[5]; r->first = 0;
    const uint32_t type = (uint32_t)v.gpr[6] & 0xF, loc = (uint32_t)v.gpr[7] & 1, off = (uint32_t)v.gpr[8];
    r->idx_type = type == 1 ? 2u : 4u;   // CELL_GCM_DRAW_INDEX_ARRAY_TYPE_16 -> asp's 2 (16-bit), else 32-bit
    r->idx_ea = cellGcmResolveLocated(loc == 0, off);
    r->vcount = 0;
    const uint32_t isz = r->idx_type == 2 ? 2u : 4u;
    if (!r->count || r->count > (1u << 22) || !ea_ok(r->idx_ea, r->count * isz)) { r->why = kWhyVertex; return; }
    // the vertices it reads: [0, the largest index]
    uint32_t mx = 0;
    for (uint32_t i = 0; i < r->count; i++) {
        const uint32_t x = isz == 2 ? g16(r->idx_ea + 2 * i) : g32(r->idx_ea + 4 * i);
        if (x > mx) mx = x;
    }
    r->vcount = mx + 1;
    stats().game_draw_records++;
}

// R6 (DOTHEROES_R6_SKIP=1, with the bypass and the producers): asp's draw
// (0x006C6F88) and the game's indexed draw (0x006A2164) replaced at their
// entry -- the body never runs, no command word is written, nothing is taken
// back -- by what the body does besides its words (docs/NATIVE_R6.md section
// 3; the draw's specification, build/r6): the frame counter at P+0x13DC, the
// stream objects' +0/+4 its binds store, the stream-set cache left clear, r3
// 0; the binds and the unbinds after the draw command as the record's state.
// =verify: the body runs, and what the replacement would have done is
// compared with what it did (stats skv_*).
int r6_skip_read()
{
    int on = -1;
    {
        const char* e = getenv("DOTHEROES_R6_SKIP");
        // 1: draws and programs; all (3): asp's state functions too; verify (2)
        on = !e || !*e ? 0 : !strcmp(e, "verify") ? 2 : !strcmp(e, "all") ? 3 : *e == '1' ? 1 : 0;
        if (on && (!r6_producers() || asp_bypass() < 1)) {
            fprintf(stderr, "[ppfx] DOTHEROES_R6_SKIP needs DOTHEROES_R6_PRODUCERS=1 and DOTHEROES_ASP_BYPASS>=1: off\n");
            on = 0;
        }
    }
    return on;
}
// DOTHEROES_R6_KEEP=<hex>,<hex>,... (a development switch): libgcm entry
// points whose bodies run under =all all the same -- to find which skipped
// body a difference depends on
bool r6_keep(uint32_t addr)
{
    static uint32_t keep[64];
    static int n = -1;
    if (n < 0) {
        n = 0;
        const char* e = getenv("DOTHEROES_R6_KEEP");
        while (e && *e && n < 64) {
            char* end = nullptr;
            const unsigned long v = strtoul(e, &end, 16);
            if (end == e) break;
            keep[n++] = (uint32_t)v;
            e = *end == ',' ? end + 1 : end;
        }
        if (n) fprintf(stderr, "[ppfx] DOTHEROES_R6_KEEP: %d libgcm entry points run under =all\n", n);
    }
    for (int i = 0; i < n; i++) if (keep[i] == addr) return true;
    return false;
}
const uint32_t kCurCtx = 0x02C11BACu, kStreamCache = 0x02C12E10u, kCurPair = 0x02C12CE0u, kSent = 0x02C12AF0u;
const uint32_t kCounterP = 0x02C12B1Cu, kStreamSetVt = 0x00FEABF0u;

// The binds asp's draw would make (0x006D3930 -> 0x006D3428 -> 0x006D3190),
// in its order: (stream object, table entry). false: none to decide here
struct Bind { uint32_t st, e; };
bool asp_draw_binds(const uint32_t* sets, std::vector<Bind>& out, bool* hit)
{
    out.clear();
    *hit = g32(kStreamCache) == sets[0] && g32(kStreamCache + 4) == sets[1] && g32(kStreamCache + 8) == sets[2] &&
           g32(kStreamCache + 12) == sets[3];
    if (*hit) return true;
    for (int k = 0; k < 4; k++) {                      // a set not ready: nothing bound
        if (!sets[k]) break;
        if (!ea_ok(sets[k], 0x50)) return false;
        if (!g32(sets[k] + 8)) return true;
    }
    const uint32_t G = g32(kCurPair), SENT = g32(kSent);
    if (!G) return true;                               // no pair since the frame's end: nothing bound
    if (!ea_ok(G + 0x20, 0xC8)) return false;
    const uint32_t T = G + 0x20;
    for (int k = 0; k < 4 && sets[k] && k < (int32_t)g32(T); k++) {   // the count re-read each time, as the body
        const uint32_t sk = sets[k];
        auto t = [&](uint32_t e, uint32_t sl) {
            if (!ea_ok(e, 8) || g32(e) == SENT) return;
            if (const uint32_t st = g32(sk + sl)) out.push_back({ st, e });
        };
        if (k == 0) { t(T + 0x90, 0x18); t(T + 0x98, 0x1C); t(T + 0xA0, 0x20); t(T + 0xA8, 0x24); t(T + 0xB8, 0x30); t(T + 0xC0, 0x34); }
        const uint32_t o = 8u * k;
        t(T + o + 0x04, 0x10); t(T + o + 0x28, 0x14); t(T + o + 0x4C, 0x28); t(T + o + 0x70, 0x2C);
    }
    return true;
}

// a stream object bound to the input its table entry names: the state as
// the bind's FORMAT and OFFSET words give it
void stream_state_bind(uint32_t st, uint32_t e)
{
    if ((g32(e) & 0x7FFFu) >= 16) return;
    VertexInput& v = S.st.vin[g32(e) & 15];
    v.stride = (uint8_t)g8(st + 8); v.size = (uint8_t)(g8(st + 9) & 0xF); v.type = (uint8_t)(g8(st + 10) & 0xF);
    v.location = (uint8_t)(g8(st + 11) & 1); v.frequency = (uint16_t)g16(st + 12); v.offset = g32(st + 16);
}

void asp_record_publish(Rec* r, uint32_t ctx, bool marker);
Rec* asp_record_begin();

// asp's draw at its entry. false: left to its body (counted)
bool asp_draw_skip(ppu_context* c)
{
    const uint32_t self = (uint32_t)c->gpr[3];
    const uint32_t a[4] = { self, (uint32_t)c->gpr[4], (uint32_t)c->gpr[5], (uint32_t)c->gpr[6] };
    // a third extra set: the body's r3 is then its unbind's incidental value -- the body's
    if (a[3] && ea_ok(a[3], 0x18) && g32(a[3] + 0x14)) { stats().skip_fallback_args++; return false; }
    if (!ea_ok(self, 0x28) || g32(kCurCtx) != S.ring_ctx || !ea_ok(S.ring_ctx, 16)) { stats().skip_fallback_ctx++; return false; }
    uint32_t sets[4] = { g32(self + 0x14), 0, 0, 0 };
    for (int k = 1; k < 4; k++) sets[k] = a[k] && ea_ok(a[k], 0x18) ? g32(a[k] + 0x14) : 0;
    const uint32_t s0 = sets[0];
    static std::vector<Bind> b;
    bool hit = false;
    if (!ea_ok(s0, 0x50) || g32(s0) != kStreamSetVt || !asp_draw_binds(sets, b, &hit)) { stats().skip_fallback_set++; return false; }
    if (a[1] || a[2]) stats().skip_extra_sets++;
    const uint32_t r7 = (uint32_t)c->gpr[7];
    if (hit) stats().skip_cache_hits++;
    // what the body stores besides its words
    const uint32_t P = g32(kCounterP);
    if (ea_ok(P + 0x13DC, 4)) p32(P + 0x13DC, g32(P + 0x13DC) + 1);
    for (const Bind& k : b) { p32(k.st, g32(k.e)); p32(k.st + 4, g32(k.e + 4)); stream_state_bind(k.st, k.e); }
    for (uint32_t i = 0; i < 4; i++) p32(kStreamCache + 4 * i, 0);
    c->gpr[3] = 0;
    // the record: the operation from the object (+0x1C and +0x20 the index
    // buffer, as the body takes it)
    const int32_t ic = (int32_t)g32(self + 0x0C);
    if (ic >= 0) {
        Rec* r = asp_record_begin();
        r->prim = g32(self + 0x08) & 0xFF;
        r->vcount = g32(self + 0x10);
        if (ic > 0) {
            const uint32_t off = g32(self + 0x1C) & 0x1FFFFFFFu, loc = (g8(self + 0x20) ^ 1u) & 1u;
            r->idx_ea = cellGcmResolveLocated(loc == 0, off);
            r->idx_type = g32(self + 0x24);
            r->count = (uint32_t)ic;
        } else {   // draw arrays: r7, when set, the vertex count (0x006C6F88's override)
            r->idx_ea = 0; r->idx_type = 0;
            if (r7) { r->vcount = r7; stats().skip_count_override++; }
            r->count = r->vcount;
        }
        r->first = 0;
        asp_draw_streams(r);
        asp_record_publish(r, S.ring_ctx, true);
    } else stats().skip_no_draw++;
    // after the draw command: each set's stream objects unbound (by the index
    // each last received, even those this draw did not bind)
    const uint32_t SENT = g32(kSent);
    for (int k = 0; k < 4; k++) {
        if (!sets[k] || !ea_ok(sets[k], 0x50)) continue;
        for (uint32_t i = 0; i < 16; i++) {
            const uint32_t st = g32(sets[k] + 0x10 + 4 * i);
            if (!st || !ea_ok(st, 0x14) || g32(st) == SENT || (g32(st) & 0x7FFFu) >= 16) continue;
            VertexInput& v = S.st.vin[g32(st) & 15];
            v.size = 0; v.type = 2; v.stride = (uint8_t)g8(st + 8); v.frequency = (uint16_t)g16(st + 12);
            v.location = (uint8_t)(g8(st + 11) & 1); v.offset = g32(st + 16);
        }
    }
    stats().skip_asp_draws++;
    return true;
}

// R6's verify (DOTHEROES_R6_SKIP=verify): the replacement's prediction at
// the draw's entry, the body's binds as they happen, compared at its return
struct SkipVerify { bool active; uint32_t P, p0; std::vector<Bind> want, got; } s_skv;
void skv_enter(ppu_context* c)
{
    s_skv.active = false;
    const uint32_t self = (uint32_t)c->gpr[3];
    const uint32_t a[4] = { self, (uint32_t)c->gpr[4], (uint32_t)c->gpr[5], (uint32_t)c->gpr[6] };
    if ((a[3] && ea_ok(a[3], 0x18) && g32(a[3] + 0x14)) || !ea_ok(self, 0x28) || g32(kCurCtx) != S.ring_ctx) return;
    uint32_t sets[4] = { g32(self + 0x14), 0, 0, 0 };
    for (int k = 1; k < 4; k++) sets[k] = a[k] && ea_ok(a[k], 0x18) ? g32(a[k] + 0x14) : 0;
    const uint32_t s0 = sets[0];
    bool hit = false;
    if (!ea_ok(s0, 0x50) || g32(s0) != kStreamSetVt || !asp_draw_binds(sets, s_skv.want, &hit)) return;
    s_skv.P = g32(kCounterP);
    s_skv.p0 = ea_ok(s_skv.P + 0x13DC, 4) ? g32(s_skv.P + 0x13DC) : 0;
    s_skv.got.clear();
    s_skv.active = true;
}
void skv_bind(uint32_t st, uint32_t e) { if (s_skv.active) s_skv.got.push_back({ st, e }); }
void skv_exit(const ppu_context* c)
{
    if (!s_skv.active) return;
    s_skv.active = false;
    Stats& st = stats();
    st.skv_draws++;
    bool same = s_skv.want.size() == s_skv.got.size();
    for (size_t i = 0; same && i < s_skv.want.size(); i++)
        same = s_skv.want[i].st == s_skv.got[i].st && s_skv.want[i].e == s_skv.got[i].e;
    if (same) st.skv_binds_equal++; else st.skv_binds_differ++;
    for (const Bind& k : s_skv.want)
        if (g32(k.st) != g32(k.e) || g32(k.st + 4) != g32(k.e + 4)) { st.skv_stream_words_bad++; break; }
    if (ea_ok(s_skv.P + 0x13DC, 4) && g32(s_skv.P + 0x13DC) != s_skv.p0 + 1) st.skv_counter_bad++;
    if (g32(kStreamCache) || g32(kStreamCache + 4) || g32(kStreamCache + 8) || g32(kStreamCache + 12)) st.skv_cache_bad++;
    if ((uint32_t)c->gpr[3] != 0) st.skv_r3_bad++;
}

// R6: the game's own cellGcmSetDrawIndexArray at its entry
void game_draw_skip(ppu_context* c)
{
    Rec* r = asp_record_begin();
    VEntry v;
    memset(&v, 0, sizeof v);
    v.addr = 0x006A2164u;
    for (int i = 3; i <= 8; i++) v.gpr[i] = c->gpr[i];
    game_draw_operation(r, v);
    asp_draw_streams(r);
    asp_record_publish(r, (uint32_t)c->gpr[3], true);
    stats().skip_game_draws++;
}

// R6: the common part of an asp-style record (asp's draws, the game's draws
// in asp's scene): its state, constants and programs now, its targets and units
Rec* asp_record_begin()
{
    Rec* r = acquire();
    Queue& q = queue();
    r->seq = q.put.load(std::memory_order_relaxed);
    r->kind = kAspDraw;
    r->frame = S.frame;
    r->why = kWhyNone;
    r->st = S.st;
    r->nvr = 0; r->vdata.clear(); r->nneed = 0; r->ntex = 0; r->tdata.clear();
    memset(&r->walk, 0, sizeof r->walk);
    for (int16_t& k : r->walk.release_sem) k = -1;
    r->bypassed = 0;
    take_vconst(r);
    r->vp = vp_blob_now();
    r->fp = A.fp_bound ? asp_fp_now() : S.fp;
    asp_draw_operation(r, 0);   // its targets and units (the operation is the caller's)
    r->why = kWhyNone;
    return r;
}

void asp_record_publish(Rec* r, uint32_t ctx, bool marker)
{
    if (!r->vp) r->why = kWhyNoVp; else if (!r->fp) r->why = kWhyNoFp;
    r->src_hash = asp_race_hash(r);
    r->naclear = A.nclears;
    for (uint32_t i = 0; i < A.nclears; i++) r->aclear[i] = A.clears[i];
    A.nclears = 0;
    r->lap = r->lap_end = cellGcm_title_laps_pub(nullptr);
    r->io = r->io_end = cellGcm_ea2io_pub(g32(ctx + 8));
    r->io_alt = cellGcm_ea2io_pub(g32(ctx));
    r->bypassed = marker ? 1 : 0;
    publish(r);
    stats().asp_records++;
    if (marker && !rmark(ctx, r->seq)) stats().asp_marker_fail++;
}

// R6: the game's own clear, cellGcmSetClearSurface(ctx, mask): a pending
// clear like asp's (Rec::AspClear) on the surface, colour and depth/stencil
// values the game's calls set
void game_clear(uint32_t mask)
{
    Rec::AspClear k;
    memset(&k, 0, sizeof k);
    k.mask = mask & 0xF3u;   // CELL_GCM_CLEAR_Z 1, _S 2, _R/_G/_B/_A 0x10-0x80
    k.argb = S.st.clear_color;
    const uint32_t ds = S.st.clear_depth;   // cellGcmSetClearDepthStencil: Z24S8 depth << 8 | stencil; Z16 the depth
    if (S.st.surface.depth_format == 1) { k.depth = (uint32_t)((uint64_t)(ds & 0xFFFFu) * 0xFFFFFFu / 0xFFFFu); k.stencil = 0; }
    else { k.depth = ds >> 8; k.stencil = ds & 0xFFu; }
    for (int j = 0; j < 4; j++) k.rgba[j] = (float)(k.argb >> (j == 3 ? 24 : 16 - 8 * j) & 0xFF) / 255.0f;
    k.surface = S.st.surface;
    // the engine clears the scissor's extent: a clear narrower than its
    // surface is counted (the native clear takes the whole surface)
    const Scissor& sc = S.st.scissor;
    if (sc.x || sc.y || sc.w < k.surface.width || sc.h < k.surface.height) {
        stats().r6_clears_scissored++;
        static int said = 0;
        if (said++ < 8) {
            char line[200];
            snprintf(line, sizeof line, "r6: game clear %02X of %u:%08X (%ux%u, depth %08X) with scissor %u,%u %ux%u at frame %u",
                     mask, k.surface.color_location[0], k.surface.color_offset[0], k.surface.width, k.surface.height,
                     k.surface.depth_offset, sc.x, sc.y, sc.w, sc.h, S.frame);
            report_add(line);
        }
    }
    asp_pend_clear(k);
    stats().r6_game_clears++;
}

// R6: the game's own cellGcmSetDrawArrays(ctx, mode, first, count), on the
// title's context: a record like the game's indexed draws (R5), without
// indices -- the vertices [first, first + count) of each enabled stream, the
// streams' offsets moved on by `first` -- at the call: with the bypass its
// body does not run (a marker in its place)
void game_arrays_record(ppu_context* c, bool skip)
{
    Rec* r = acquire();
    Queue& q = queue();
    r->seq = q.put.load(std::memory_order_relaxed);
    r->kind = kAspDraw;
    r->frame = S.frame;
    r->why = kWhyNone;
    r->st = S.st;
    r->nvr = 0; r->vdata.clear(); r->nneed = 0; r->ntex = 0; r->tdata.clear();
    memset(&r->walk, 0, sizeof r->walk);
    for (int16_t& k : r->walk.release_sem) k = -1;
    take_vconst(r);
    r->vp = vp_blob_now();
    r->fp = A.fp_bound ? asp_fp_now() : S.fp;
    const uint32_t first = (uint32_t)c->gpr[5];
    for (uint32_t i = 0; i < 16; i++)
        if (r->st.vin[i].size && r->st.vin[i].frequency <= 1) r->st.vin[i].offset += first * r->st.vin[i].stride;
    asp_draw_operation(r, 0);
    r->why = kWhyNone;
    r->prim = (uint32_t)c->gpr[4] & 0xFF; r->first = 0; r->count = (uint32_t)c->gpr[6];
    r->idx_ea = 0; r->idx_type = 0; r->vcount = r->count;
    if (!r->count || r->count > (1u << 22)) r->why = kWhyVertex;
    asp_draw_streams(r);
    if (!r->vp) r->why = kWhyNoVp; else if (!r->fp) r->why = kWhyNoFp;
    r->src_hash = asp_race_hash(r);
    r->naclear = A.nclears;
    for (uint32_t i = 0; i < A.nclears; i++) r->aclear[i] = A.clears[i];
    A.nclears = 0;
    const uint32_t ctx = (uint32_t)c->gpr[3];
    r->lap = r->lap_end = cellGcm_title_laps_pub(nullptr);
    r->io = r->io_end = cellGcm_ea2io_pub(g32(ctx + 8));
    r->io_alt = cellGcm_ea2io_pub(g32(ctx));
    r->bypassed = skip ? 1 : 0;
    r->game_arrays = 1;
    publish(r);
    stats().asp_records++;
    stats().r6_game_arrays++;
    if (skip) {
        stats().r6_game_skipped++;
        if (!rmark(ctx, r->seq)) stats().asp_marker_fail++;
    }
}

// R6 (DOTHEROES_R6_SKIP=all): asp's state functions replaced at their entry
// (the state's specification, build/r6). Their words would only reach the
// engine, which draws nothing any more; their store items are read by asp's
// re-emissions alone, which are skipped too and applied from the recorder's
// own model of the store (A.stored). What other code reads is kept: the
// sampler table's dirty bytes, the device's surface-format byte (+0x12C),
// the pass's +0x418 byte, the viewport cache 0x006D4678 keeps, and the
// returns callers read (texture apply, pass start, render target set: 1).
// verify (DOTHEROES_R6_SKIP=verify): the replacement runs dry -- its guest
// stores are listed, not made -- and is checked against the body
bool s_dry = false;
struct GW { uint32_t ea, v; uint8_t n; };
std::vector<GW> s_dry_w;
void gw8(uint32_t ea, uint8_t v) { if (s_dry) s_dry_w.push_back({ ea, v, 1 }); else p8(ea, v); }
void gw32(uint32_t ea, uint32_t v) { if (s_dry) s_dry_w.push_back({ ea, v, 4 }); else p32(ea, v); }

void enc_asp_viewport(int32_t x, int32_t y, int32_t w, int32_t h, float zmin, float zmax)
{
    Viewport& v = S.st.viewport;
    v.x = (uint16_t)x; v.y = (uint16_t)y; v.w = (uint16_t)w; v.h = (uint16_t)h;
    v.zmin = zmin; v.zmax = zmax;
    v.scale[0] = w * 0.5f; v.scale[1] = h * -0.5f; v.scale[2] = zmax - zmin; v.scale[3] = 0.0f;
    v.offset[0] = x + w * 0.5f; v.offset[1] = y + h * 0.5f; v.offset[2] = 0.0f; v.offset[3] = 0.0f;
}

// 0x006D4678(x, y, w, h; zmin, zmax): the device's viewport setter, then its caches
void asp_viewport_cached(uint32_t toc, int32_t x, int32_t y, int32_t w, int32_t h, float zmin, float zmax)
{
    enc_asp_viewport(x, y, w, h, zmin, zmax);
    asp_mirror(0x006C0F60u);   // the setter writes the store's item too
    const uint32_t devp = g32(toc - 0x2AFC);
    const uint32_t dev = ea_ok(devp, 4) ? g32(devp) : 0;
    if (ea_ok(dev + 0x10C, 4) && ea_ok(g32(dev + 0x10C) + 0x88, 1)) gw8(g32(dev + 0x10C) + 0x88, 1);
    const uint32_t cp = g32(toc - 0x2AF8);
    const uint32_t cache = ea_ok(cp, 4) ? g32(cp) : 0;
    if (ea_ok(cache + 0x1390, 16)) {
        gw32(cache + 0x1390, (uint32_t)x); gw32(cache + 0x1394, (uint32_t)y);
        gw32(cache + 0x1398, (uint32_t)w); gw32(cache + 0x139C, (uint32_t)h);
    }
}

// a re-emission of the store item a slot-table entry names (0x006D0C4C),
// as the model has it
void asp_reemit_entry(uint32_t toc, uint32_t entry_ea)
{
    uint32_t begin, end;
    if (!ea_ok(entry_ea, 4) || !asp_store(&begin, &end)) return;
    const uint32_t off = g16(entry_ea) & ~3u, size = g16(entry_ea + 2) & ~3u;
    asp_restore(begin, begin + off, (uint64_t)begin + off + size);
    stats().skip_restores++;
    (void)toc;
}

// the sampler bind's own stores: the entry's dirty bytes cleared (all three
// for an enabled bind, +0x2C alone for a disabled one)
void asp_sampler_clear(uint32_t toc, uint32_t unit, uint32_t en)
{
    const uint32_t e = g32(toc - 0x2D88u) + 0x0Cu + 48u * unit;
    if (!ea_ok(e, 48)) return;
    gw8(e + 0x2C, 0);
    if (en) { gw8(e + 0x2D, 0); gw8(e + 0x2E, 0); }
}

int asp_record_call_inner(uint32_t addr, ppu_context* c);
int asp_state_skip(uint32_t addr, ppu_context* c)
{
    switch (addr) {
    case 0x006C01C4: case 0x006C49BC: case 0x006C4D1C: case 0x006C048C: case 0x006C5298: case 0x006C05AC:
    case 0x006C4284: case 0x006C4508: case 0x006C0A70: case 0x006C0E38: case 0x006C4628: case 0x006C06CC:
    case 0x006C19A4: case 0x006C02E8: case 0x006C0F60: case 0x006C08F0: case 0x006C5170: case 0x006C2D3C:
    case 0x006D11BC: case 0x006C8A34: case 0x006D1764: case 0x006C37A0: case 0x006CB070: case 0x006D0E84:
        break;
    case 0x006D0BE0:   // the store's re-emission (dst, src, words): into the ring only
        if ((uint32_t)c->gpr[3] != S.ring_ctx || !asp_records_on()) return 0;
        break;
    default:
        return 0;
    }
    if (!asp_records_on() || !S.ring_ctx || g32(kCurCtx) != S.ring_ctx) { stats().skip_state_left++; return 0; }
    const uint32_t toc = (uint32_t)c->gpr[2];
    if (addr == kAspColorMask || addr == kAspSampler) { A.device = (uint32_t)c->gpr[3]; A.toc = toc; }
    switch (addr) {
    case 0x006C02E8: { // the colour mask: the R3 encoder (its arguments); r3 the store context
        asp_call_state(addr, c);
        const uint32_t dev = (uint32_t)c->gpr[3];
        if (ea_ok(dev + 0x124, 4)) c->gpr[3] = g32(dev + 0x124);
        break;
    }
    case 0x006C2D3C: { // the clear: its record, then its two re-emissions (+0x00 alpha, +0x18 blend equation)
        asp_record_call_inner(addr, c);
        const uint32_t tbl = g32(toc - 0x2D98u);
        asp_reemit_entry(toc, tbl + 0x00); asp_reemit_entry(toc, tbl + 0x18);
        break;
    }
    case 0x006D11BC: { // the pass end: the colour-mask item re-emitted, the scissor off (not stored)
        const uint32_t pr = g32(toc - 0x2B6C);
        if (ea_ok(pr, 4)) asp_reemit_entry(toc, pr);
        S.st.scissor = { 0, 0, 0x0FFF, 0x0FFF };
        c->gpr[3] = S.ring_ctx;   // passed up by 0x006D89A8 (the ring context's address, as the body leaves it)
        break;
    }
    case 0x006D0BE0: { // a re-emission into the ring: the model's restore; r3 the copy's destination, as memcpy returns
        const uint32_t dst_cur = g32(S.ring_ctx + 8);
        asp_call_state(addr, c);
        c->gpr[3] = dst_cur;
        stats().skip_restores++;
        break;
    }
    case 0x006C8A34: { // the render target set
        const uint32_t rt = (uint32_t)c->gpr[3];
        const uint32_t sf = rt + 0x40u + 0x3Cu * ((uint32_t)c->gpr[4] & 0xFF);
        if (!ea_ok(rt, 0x18) || !ea_ok(sf, 0x3C) || g32(sf + 8) == 0xFFFFFFFFu) { c->gpr[3] = 0; break; }
        enc_surface(sf);
        const uint32_t dp = g32(toc - 0x2DA0);
        if (ea_ok(dp, 4) && ea_ok(g32(dp) + 0x12C, 1)) gw8(g32(dp) + 0x12C, (uint8_t)g8(sf + 2));   // 0x006C1994: the format
        if (g32(rt + 0xC) == 0xFFFFFFFDu) { const uint32_t pr = g32(toc - 0x2C84); if (ea_ok(pr, 4)) asp_reemit_entry(toc, pr); }
        asp_viewport_cached(toc, 0, 0, (int32_t)g32(rt + 0x10), (int32_t)g32(rt + 0x14), gfloat(toc - 0x2C7C), gfloat(toc - 0x2C80));
        c->gpr[3] = 1;
        break;
    }
    case 0x006D1764: { // the pass start
        const uint32_t rt = (uint32_t)c->gpr[3];
        if (!ea_ok(rt, 0x80) || g32(rt + 0x28) == 0xFFFFFFFFu || g32(rt + 0x58) == 0xFFFFFFFFu) { c->gpr[3] = 0; break; }
        const uint32_t bp = g32(toc - 0x2034);   // 0x007067A8: the +0x418 byte
        if (ea_ok(bp, 4) && ea_ok(g32(bp) + 0x418, 1)) gw8(g32(bp) + 0x418, (uint8_t)g8(rt + 0xC));
        const uint32_t dp = g32(toc - 0x2DA0);
        if (ea_ok(dp, 4) && ea_ok(g32(dp) + 0x12C, 1)) gw8(g32(dp) + 0x12C, (uint8_t)g8(rt + 0x2E));
        asp_record_call_inner(addr, c);   // its surface, state and clear (the encoder)
        asp_viewport_cached(toc, 0, 0, (int32_t)g32(rt + 4), (int32_t)g32(rt + 8), gfloat(toc - 0x2B58), gfloat(toc - 0x2B5C));
        if ((uint32_t)c->gpr[5] && g32(rt + 4) > 2 && g32(rt + 8) > 2)   // the inset scissor after the viewport's
            S.st.scissor = { 1, 1, (uint16_t)(g32(rt + 4) - 2), (uint16_t)(g32(rt + 8) - 2) };
        c->gpr[3] = 1;
        break;
    }
    case 0x006C37A0: { // the sampler bind: the record (dirty bytes read first), then the clears
        const uint32_t u = (uint32_t)c->gpr[4] & 0xFF, en = (uint32_t)c->gpr[5] & 0xFF;
        asp_record_call_inner(addr, c);
        if (u < 16) asp_sampler_clear(toc, u, en);
        break;
    }
    case 0x006CB070: { // the texture apply: the texture, then its unit's sampler bind (enabled), r3 1
        const uint32_t u = (uint32_t)(c->gpr[4] >> 32) & 0xFF;
        asp_record_call_inner(addr, c);
        if (u < 16) {
            const uint32_t dp = g32(toc - 0x2C10);
            const uint32_t saved3 = (uint32_t)c->gpr[3], saved4 = (uint32_t)c->gpr[4], saved5 = (uint32_t)c->gpr[5];
            c->gpr[3] = ea_ok(dp, 4) ? g32(dp) : 0; c->gpr[4] = u; c->gpr[5] = 1;
            A.device = (uint32_t)c->gpr[3]; A.toc = toc;
            asp_record_call_inner(0x006C37A0u, c);
            asp_mirror(0x006C37A0u);
            asp_sampler_clear(toc, u, 1);
            c->gpr[3] = saved3; c->gpr[4] = saved4; c->gpr[5] = saved5;
        }
        c->gpr[3] = 1;
        break;
    }
    case 0x006D0E84:   // the shadow map: ring words only
        asp_record_call_inner(addr, c);
        c->gpr[3] = 1;
        break;
    default: {         // the setters: their records; r3 the store context, as the body leaves it
        asp_record_call_inner(addr, c);
        const uint32_t dev = (uint32_t)c->gpr[3];
        if (ea_ok(dev + 0x124, 4)) c->gpr[3] = g32(dev + 0x124);
        break;
    }
    }
    asp_mirror(addr);
    stats().skip_state++;
    return 2;   // the body does not run
}

// R6's verify of a state function: the replacement run dry at its entry on
// the recorder's own state, its result kept and the state put back; the body
// then runs with the R5 path recording it (its nested calls observed); at its
// return the two states are compared, and the stores the replacement listed
// with guest memory, and r3 where callers read it
bool r6_takeback_fn(uint32_t addr);
void asp_state_bypass_exit(const VEntry& v);
struct StateVerify { uint32_t addr; State st; std::vector<GW> w; uint32_t r3; bool r3_read; };
std::vector<StateVerify> s_sksv;
bool asp_state_verify_enter(uint32_t addr, ppu_context* c)
{
    switch (addr) {
    case 0x006C01C4: case 0x006C49BC: case 0x006C4D1C: case 0x006C048C: case 0x006C5298: case 0x006C05AC:
    case 0x006C4284: case 0x006C4508: case 0x006C0A70: case 0x006C0E38: case 0x006C4628: case 0x006C06CC:
    case 0x006C19A4: case 0x006C02E8: case 0x006C0F60: case 0x006C08F0: case 0x006C5170: case 0x006C2D3C:
    case 0x006D11BC: case 0x006C8A34: case 0x006D1764: case 0x006C37A0: case 0x006CB070: case 0x006D0E84:
        break;
    case 0x006D0BE0:
        if ((uint32_t)c->gpr[3] != S.ring_ctx) return false;
        break;
    default:
        return false;
    }
    if (!asp_records_on() || !S.ring_ctx || g32(kCurCtx) != S.ring_ctx) return false;
    for (const VEntry& e : s_vstack) if (e.kind == 2 || e.kind == 6) return false;   // only the outermost
    const State st0 = S.st, stored0 = A.stored;
    const uint32_t valid0 = A.stored_valid, sv0 = A.stored_samp_valid, nclears0 = A.nclears;
    Rec::AspClear clears0[32];
    memcpy(clears0, A.clears, sizeof clears0);
    const uint64_t r3_0 = c->gpr[3], r4_0 = c->gpr[4], r5_0 = c->gpr[5];
    s_dry = true; s_dry_w.clear();
    asp_state_skip(addr, c);
    s_dry = false;
    StateVerify v;
    v.addr = addr; v.st = S.st; v.w = s_dry_w; v.r3 = (uint32_t)c->gpr[3];
    v.r3_read = addr == 0x006CB070u || addr == 0x006D1764u || addr == 0x006C8A34u;
    s_sksv.push_back(v);
    S.st = st0; A.stored = stored0; A.stored_valid = valid0; A.stored_samp_valid = sv0; A.nclears = nclears0;
    memcpy(A.clears, clears0, sizeof clears0);
    c->gpr[3] = r3_0; c->gpr[4] = r4_0; c->gpr[5] = r5_0;
    stats().skip_state--;   // the dry run is not a skip
    VEntry e;
    memset(&e, 0, sizeof e);
    e.addr = addr; e.kind = 6;
    if (r6_takeback_fn(addr)) {   // the clear's and pass start's words still taken back at the return
        r4_ctx_fields(e, S.ring_ctx);
        e.lap = cellGcm_title_laps_pub(nullptr);
        e.store_from = (size_t)stats().markers_written.load();
        e.gpr[0] = 1;
    }
    s_vstack.push_back(e);
    return true;
}
void asp_state_verify_exit(const VEntry& e, const ppu_context* c)
{
    if (s_sksv.empty()) return;
    const StateVerify v = s_sksv.back();
    s_sksv.pop_back();
    Stats& st = stats();
    st.sksv_calls++;
    if (!memcmp(&v.st, &S.st, sizeof S.st)) st.sksv_state_equal++;
    else {
        st.sksv_state_differ++;
        const State& a = v.st; const State& b = S.st;
        const bool parts[8] = { memcmp(&a.surface, &b.surface, sizeof a.surface) != 0,
                                memcmp(&a.viewport, &b.viewport, sizeof a.viewport) != 0,
                                memcmp(&a.scissor, &b.scissor, sizeof a.scissor) != 0,
                                memcmp(&a.raster, &b.raster, sizeof a.raster) != 0,
                                memcmp(a.samp, b.samp, sizeof a.samp) != 0, memcmp(a.tex, b.tex, sizeof a.tex) != 0,
                                memcmp(a.vin, b.vin, sizeof a.vin) != 0, memcmp(&a.fp, &b.fp, sizeof a.fp) != 0 };
        for (int i = 0; i < 8; i++) if (parts[i]) st.sksv_differ_by[i]++;
        static int said = 0;
        if (said++ < 12) {
            char line[200];
            snprintf(line, sizeof line, "r6 skip verify: state after %08X differs: surface %d viewport %d scissor %d raster %d "
                     "samplers %d textures %d inputs %d fp %d", e.addr, parts[0], parts[1], parts[2], parts[3], parts[4],
                     parts[5], parts[6], parts[7]);
            report_add(line);
        }
    }
    bool stores = true;
    for (const GW& w : v.w) stores &= w.n == 1 ? g8(w.ea) == (w.v & 0xFF) : g32(w.ea) == w.v;
    if (!stores) st.sksv_stores_bad++;
    if (v.r3_read && (uint32_t)c->gpr[3] != v.r3) st.sksv_r3_bad++;
    if (e.gpr[0] == 1) asp_state_bypass_exit(e);
}

void asp_draw_exit(const VEntry& v)
{
    if (asp_probe2()) asp_draw_probe(v);
    if (!ea_ok(v.ctx_ea, 16)) return;
    // R5's bypass, stage 1 (DOTHEROES_ASP_BYPASS=1): the draw's commands --
    // its own and those of the calls inside it -- taken back, before anything
    // can make them visible to the walker (a flush for room in the queue),
    // and a marker in their place once the record is published; the native
    // renderer alone draws it. Not when its commands wrapped the ring
    // (counted): the engine draws that one.
    bool taken = false;
    if (asp_bypass() >= 1) {
        const uint32_t now = g32(v.ctx_ea + 8);
        if (cellGcm_title_laps_pub(nullptr) == v.lap && now >= v.cur && now - v.cur < (4u << 20)) {
            p32(v.ctx_ea + 8, v.cur);
            taken = true;
            stats().asp_bypassed++;
            stats().asp_bypassed_words += (now - v.cur) / 4;
        } else stats().asp_not_bypassed++;
    }
    std::unique_lock<std::mutex> g(s_lock);
    struct Held { Held(std::unique_lock<std::mutex>* h) { s_held = h; } ~Held() { s_held = nullptr; } } held(&g);
    Rec* r = acquire();
    Queue& q = queue();
    r->seq = q.put.load(std::memory_order_relaxed);
    r->kind = kAspDraw;
    r->frame = S.frame;
    r->why = kWhyNone;
    r->st = S.st;
    r->nvr = 0; r->vdata.clear(); r->nneed = 0; r->ntex = 0; r->tdata.clear();
    memset(&r->walk, 0, sizeof r->walk);
    for (int16_t& k : r->walk.release_sem) k = -1;
    r->bypassed = 0;
    for (uint32_t i = 0; i < 16; i++) if (A.unbind_after >> i & 1) S.st.vin[i].size = 0;   // after its state was taken
    A.unbind_after = 0;
    take_vconst(r);
    // its programs: the vertex program as vertex program memory holds it,
    // the fragment program asp uploaded at the bound offset
    r->vp = vp_blob_now();
    r->fp = A.fp_bound ? asp_fp_now() : S.fp;
    if (v.addr == 0x006A2164u) {   // the game's draw: its operation from the call; its streams, targets, units as asp's
        asp_draw_operation(r, 0);
        r->why = kWhyNone;
        if (!r->vp) r->why = kWhyNoVp; else if (!r->fp) r->why = kWhyNoFp;
        game_draw_operation(r, v);
        asp_draw_streams(r);
    } else asp_draw_operation(r, (uint32_t)v.gpr[3]);
    r->src_hash = asp_race_hash(r);
    r->naclear = A.nclears;
    for (uint32_t i = 0; i < A.nclears; i++) r->aclear[i] = A.clears[i];
    A.nclears = 0;
    if (A.fp_bound) {
        auto lo = A.fp_last_obj.find(S.st.fp.offset);
        if (lo == A.fp_last_obj.end() || lo->second != A.fp_bound_obj) {
            if (A.fp_stale_draws++ < 12) {
                char line[200];
                snprintf(line, sizeof line, "asp fp stale seq %llu frame %u: bound %08X, last uploaded at %08X: %08X",
                         (unsigned long long)r->seq, S.frame, A.fp_bound_obj, S.st.fp.offset,
                         lo == A.fp_last_obj.end() ? 0u : lo->second);
                report_add(line);
            }
        }
    }
    if (!r->vp) r->why = kWhyNoVp;
    else if (!r->fp) r->why = kWhyNoFp;
    // its commands: from the context's position at the call to where it is now
    const uint32_t cur = g32(v.ctx_ea + 8), begin = g32(v.ctx_ea);
    r->lap = v.lap;
    r->io = cellGcm_ea2io_pub(v.cur);
    r->io_alt = cellGcm_ea2io_pub(begin);
    r->lap_end = cellGcm_title_laps_pub(nullptr);
    r->io_end = cellGcm_ea2io_pub(cur);
    r->bypassed = taken ? 1 : 0;
    publish(r);
    stats().asp_records++;
    if (r->bypassed && !rmark(v.ctx_ea, r->seq)) stats().asp_marker_fail++;
}

// asp's methods R5 describes: 1 + what ppfx_rec_observe answers, or 0 (not one of them)
// asp's clears wait for its next draw (Rec::aclear). A clear that covers an
// earlier pending one -- the same surface, every aspect the earlier one
// cleared -- replaces it; past 32 the oldest goes (counted)
void asp_pend_clear(const Rec::AspClear& k)
{
    for (uint32_t i = 0; i < A.nclears;) {
        const Rec::AspClear& o = A.clears[i];
        const bool same = o.surface.color_offset[0] == k.surface.color_offset[0] && o.surface.depth_offset == k.surface.depth_offset &&
                          o.surface.color_target == k.surface.color_target && (o.mask & ~k.mask) == 0;
        if (same) { memmove(&A.clears[i], &A.clears[i + 1], (A.nclears - i - 1) * sizeof k); A.nclears--; A.clears_merged++; }
        else i++;
    }
    if (A.nclears < 32) A.clears[A.nclears++] = k;
    else { memmove(&A.clears[0], &A.clears[1], 31 * sizeof k); A.clears[31] = k; A.clears_dropped++; }
}

// A unit's texture from a CellGcmTexture (asp's objects hold libgcm's own)
void enc_texture_at(uint32_t unit, uint32_t t)
{
    if (!ea_ok(t, 24)) return;
    Texture& o = S.st.tex[unit & 15];
    o.format = (uint8_t)g8(t + 0); o.mipmap = (uint8_t)g8(t + 1); o.dimension = (uint8_t)g8(t + 2);
    o.cubemap = (uint8_t)g8(t + 3); o.remap = g32(t + 4);
    o.width = (uint16_t)g16(t + 8); o.height = (uint16_t)g16(t + 10); o.depth = (uint16_t)g16(t + 12);
    o.location = (uint8_t)g8(t + 14); o.pitch = g32(t + 16); o.offset = g32(t + 20);
}

int asp_record_call_inner(uint32_t addr, ppu_context* c);
int asp_prog_r6(uint32_t addr, ppu_context* c);

// A fragment constant patch, as 0x006C9624(fo, param, count, values) makes it
// (030:62577-62782): the parameter's entry (+0x70: 20 bytes each) lists up
// to 8 offsets into the program (u16) and their count (+0x10); each gets the
// values with their 16-bit halves swapped, as the GPU reads constants. The
// entry count (0: the body stores nothing at all).
uint32_t fp_patch_record(uint32_t self, uint32_t par, uint32_t cnt, uint32_t val)
{
    if (!ea_ok(self, 0x74)) return 0;
    const uint32_t e = g32(self + 0x70) + 20u * par, base = g32(self + 0x54);
    if (!ea_ok(e, 20)) return 0;
    const uint32_t n = g32(e + 0x10);
    if (!cnt || cnt > 64 || !ea_ok(val, 4 * cnt)) return n;
    uint8_t buf[256];
    for (uint32_t i = 0; i < cnt; i++) {
        const uint32_t w = g32(val + 4 * i), sw = w << 16 | w >> 16;
        buf[4 * i] = (uint8_t)(sw >> 24); buf[4 * i + 1] = (uint8_t)(sw >> 16); buf[4 * i + 2] = (uint8_t)(sw >> 8); buf[4 * i + 3] = (uint8_t)sw;
    }
    const uint64_t ep = ++s_epoch;
    for (uint32_t k = 0; k < n && k < 8; k++) {
        shadow_write(base + g16(e + 2 * k), buf, 4 * cnt, ep);
        owner_set(base + g16(e + 2 * k), 4 * cnt, self);
    }
    if (cnt & 1) A.fp_odd_patches++;
    return n;
}


// R6: asp's program functions at their entry (the programs' specification,
// build/r6): what each records, and -- skipped -- the object fields its body
// stores besides the ring words, and its r3. =verify: the body runs and the
// replacement's predictions are checked at its return (VEntry kind 5).
const uint32_t kFpHeap = 0x02C124F0u;   // the fragment heap: +0x10 set up, +0x1C the program slot
int asp_prog_r6(uint32_t addr, ppu_context* c)
{
    const bool verify = r6_skip() == 2;
    const uint32_t self = (uint32_t)c->gpr[3];
    VEntry v;
    memset(&v, 0, sizeof v);
    v.addr = addr; v.kind = 5;
    switch (addr) {
    case 0x006C9C74: {
        const uint32_t h = (uint32_t)(c->gpr[4] >> 32);
        if ((int32_t)h < 0) {   // vertex constants: ring words only
            asp_record_call_inner(addr, c);
            if (verify) { v.gpr[10] = 1; s_vstack.push_back(v); return 3; }
            c->gpr[3] = 1; stats().skip_prog++;
            return 2;
        }
        // a fragment parameter: the patch of the pair's fragment object
        const uint32_t fo = ea_ok(self, 0xF4) ? g32(self + 0xF0) : 0;
        if (!ea_ok(fo, 0x74)) return asp_record_call_inner(addr, c);
        if (verify) {   // the nested patch records it; predicted: +0x50 = 1 when the entry has offsets
            const uint32_t e = g32(fo + 0x70) + 20u * ((h >> 15) & 0x7FFFu);
            v.gpr[4] = fo; v.gpr[5] = ea_ok(e, 20) && g32(e + 0x10) ? 1 : 0x100; v.gpr[10] = 1;
            s_vstack.push_back(v);
            return 3;
        }
        if (fp_patch_record(fo, (h >> 15) & 0x7FFFu, (uint32_t)c->gpr[6], (uint32_t)c->gpr[5])) p8(fo + 0x50, 1);
        c->gpr[3] = 1; stats().skip_prog++;
        return 2;
    }
    case 0x006C9624: {
        const uint32_t n = verify ? 0 : fp_patch_record(self, (uint32_t)c->gpr[4], (uint32_t)c->gpr[5], (uint32_t)c->gpr[6]);
        if (verify) {
            fp_patch_record(self, (uint32_t)c->gpr[4], (uint32_t)c->gpr[5], (uint32_t)c->gpr[6]);
            const uint32_t e = g32(self + 0x70) + 20u * (uint32_t)c->gpr[4];
            v.gpr[4] = self; v.gpr[5] = ea_ok(e, 20) && g32(e + 0x10) ? 1 : 0x100; v.gpr[10] = 0x100;
            s_vstack.push_back(v);
            return 3;
        }
        if (n && ea_ok(self, 0x54)) p8(self + 0x50, 1);
        stats().skip_prog++;
        return 2;
    }
    case 0x006CA3D0:   // the vertex program: ring words only
        asp_record_call_inner(addr, c);
        if (verify) { v.gpr[10] = 0x100; s_vstack.push_back(v); return 3; }
        stats().skip_prog++;
        return 2;
    case 0x006CADE4: {
        if (!ea_ok(self, 0x70)) return asp_record_call_inner(addr, c);
        if (g32(self + 0x4C) != 0xFFFFFFFFu) {   // marked dirty, nothing uploaded
            if (verify) { v.gpr[4] = self; v.gpr[5] = 1; v.gpr[10] = 0x100; s_vstack.push_back(v); return 3; }
            p8(self + 0x50, 1); stats().skip_prog++;
            return 2;
        }
        if (!g8(kFpHeap + 0x10)) {   // the heap's first set-up (a byte flag, 031:1872)
            if (!r7()) { stats().skip_prog_heap++; return asp_record_call_inner(addr, c); }   // R6: the body
            // R7: the body's first part as guest code -- func_006CAD4C, asp's
            // allocation of the heap (16 KB of local memory, its offset; no
            // command) -- and the rest replaced as below: the body's upload
            // is an inline transfer asp writes itself (24 words: nothing
            // would execute them)
            r7_heap_setup(c);
            if (!g8(kFpHeap + 0x10)) { stats().skip_prog_heap++; return asp_record_call_inner(addr, c); }
        }
        const uint32_t bin = g32(self + 0x18), off = g32(kFpHeap + 0x1C);
        if (!ea_ok(bin, 0x20)) return asp_record_call_inner(addr, c);
        if (verify) {   // predicted: +0x54 = the slot, +0x50 = 1, the configuration from the binary
            v.gpr[4] = self; v.gpr[5] = 1; v.gpr[6] = off; v.gpr[7] = g8(bin + 0xA) == 1 ? g32(bin + 0xC) : g32(self + 0x58);
            v.gpr[8] = g8(bin + 0xA) == 1 ? g32(bin + 0x18) : g32(self + 0x64); v.gpr[10] = 0x100;
            s_vstack.push_back(v);
            asp_record_call_inner(addr, c);   // the record, as the body leaves +0x54 (it is already the slot after the first)
            return 3;
        }
        if (g8(bin + 0xA) == 1) {   // 0x00956A4C: the configuration
            p32(self + 0x58, g32(bin + 0xC));
            p16(self + 0x5C, (uint16_t)g16(bin + 0x10)); p16(self + 0x5E, (uint16_t)g16(bin + 0x12));
            p16(self + 0x60, (uint16_t)g16(bin + 0x14));
            p32(self + 0x64, g32(bin + 0x18)); p8(self + 0x68, (uint8_t)g8(bin + 0x1C));
        }
        p32(self + 0x54, off);
        asp_record_call_inner(addr, c);   // the upload into the shadow, at the slot just set
        p8(self + 0x50, 1);
        stats().skip_prog++;
        return 2;
    }
    case 0x006C99B0: {
        const uint32_t fo = ea_ok(self, 0xF4) ? g32(self + 0xF0) : 0;
        if (!ea_ok(fo, 0x70)) return asp_record_call_inner(addr, c);
        asp_record_call_inner(addr, c);   // the bind (nothing when the object is not dirty)
        if (verify) { v.gpr[4] = fo; v.gpr[5] = 0; v.gpr[10] = 1; s_vstack.push_back(v); return 3; }
        p8(fo + 0x50, 0);
        c->gpr[3] = 1; stats().skip_prog++;
        return 2;
    }
    }
    return asp_record_call_inner(addr, c);
}

// R6's verify of a program function: at its return, the fields and r3
// against what the replacement would have left (v.gpr: 4 the object, 5 its
// +0x50 or 0x100 none, 6 +0x54 or 0, 7 +0x58, 8 +0x64, 10 r3 or 0x100 any)
void prog_verify_exit(const VEntry& v, const ppu_context* c)
{
    Stats& st = stats();
    st.skpv_calls++;
    bool ok = true;
    if (v.gpr[4]) {
        const uint32_t o = (uint32_t)v.gpr[4];
        if (v.gpr[5] != 0x100 && g8(o + 0x50) != (uint32_t)v.gpr[5]) ok = false;
        if (v.gpr[6] && g32(o + 0x54) != (uint32_t)v.gpr[6]) ok = false;
        if (v.gpr[7] && g32(o + 0x58) != (uint32_t)v.gpr[7]) ok = false;
        if (v.gpr[8] && g32(o + 0x64) != (uint32_t)v.gpr[8]) ok = false;
    }
    if (!ok) {
        st.skpv_fields_bad++;
        static int said = 0;
        if (said++ < 8) {
            char line[200];
            snprintf(line, sizeof line, "r6 skip verify: %08X object %08X: +0x50 %02X (want %X) +0x54 %08X (want %08X)",
                     v.addr, (uint32_t)v.gpr[4], g8((uint32_t)v.gpr[4] + 0x50), (uint32_t)v.gpr[5],
                     g32((uint32_t)v.gpr[4] + 0x54), (uint32_t)v.gpr[6]);
            report_add(line);
        }
    }
    if (v.gpr[10] != 0x100 && (uint32_t)c->gpr[3] != (uint32_t)v.gpr[10]) st.skpv_r3_bad++;
}
int asp_record_call(uint32_t addr, ppu_context* c)
{
    if (!asp_records_on()) return 0;
    // R6 (DOTHEROES_R6_SKIP): asp's program functions replaced at their entry
    switch (addr) {
    case 0x006C9C74: case 0x006CA3D0: case 0x006CADE4: case 0x006C9624: case 0x006C99B0:
        if (r6_skip() && S.ring_ctx && g32(kCurCtx) == S.ring_ctx) return asp_prog_r6(addr, c);
        break;
    default:
        break;
    }
    return asp_record_call_inner(addr, c);
}

int asp_record_call_inner(uint32_t addr, ppu_context* c)
{
    switch (addr) {
    case 0x006C6F88:   // the draw: a record at its return
        if (!S.ring_ctx) return 1;
        if ((r6_skip() == 1 || r6_skip() == 3) && asp_draw_skip(c)) return 2;   // R6: replaced at its entry, the body not run
        if (r6_skip() == 2) skv_enter(c);
        {
            static int probe = -1, n = 0;
            if (probe < 0) { const char* e = getenv("DOTHEROES_ASP_PROBE"); probe = e && *e == '1'; }
            const uint32_t self = (uint32_t)c->gpr[3];
            if (probe && n < 48 && ea_ok(self, 0x28)) {
                n++;
                char line[400];
                int at = snprintf(line, sizeof line, "asp probe draw-call seq %llu r4 %08X r5 %08X r6 %08X r7 %08X | this:",
                                  (unsigned long long)queue().put.load(), (uint32_t)c->gpr[4], (uint32_t)c->gpr[5],
                                  (uint32_t)c->gpr[6], (uint32_t)c->gpr[7]);
                for (uint32_t i = 0; i < 0x28; i += 4) at += snprintf(line + at, sizeof line - at, " %08X", g32(self + i));
                report_add(line);
            }
        }
        asp_draw_enter(c);
        return 3;
    case 0x006CB070: { // texture apply (this, {unit, ..}, CellGcmTexture*)
        enc_texture_at((uint32_t)(c->gpr[4] >> 32) & 15, (uint32_t)c->gpr[5]);
        return 1;
    }
    case 0x006D3190: case 0x006D32D0: {   // a vertex stream's bind (stream object, {index, ..}) / unbind (stream object)
        const uint32_t s = (uint32_t)c->gpr[3];
        const uint32_t ix = addr == 0x006D3190u ? (uint32_t)c->gpr[4] : s;
        if (!ea_ok(s, 0x14) || !ea_ok(ix, 4)) return 1;
        {
            static int probe = -1, n = 0;
            if (probe < 0) { const char* e = getenv("DOTHEROES_ASP_PROBE"); probe = e && *e == '1'; }
            if (probe && n < 64) {
                n++;
                char line[300];
                snprintf(line, sizeof line, "asp probe stream %s seq %llu obj %08X: %08X %08X %08X %08X %08X | r4 %08X -> %08X %08X",
                         addr == 0x006D3190u ? "bind" : "rebind", (unsigned long long)queue().put.load(), s, g32(s), g32(s + 4),
                         g32(s + 8), g32(s + 12), g32(s + 16), (uint32_t)c->gpr[4],
                         ea_ok((uint32_t)c->gpr[4], 8) ? g32((uint32_t)c->gpr[4]) : 0,
                         ea_ok((uint32_t)c->gpr[4], 8) ? g32((uint32_t)c->gpr[4] + 4) : 0);
                report_add(line);
            }
        }
        if (addr == 0x006D3190u) skv_bind(s, ix);   // R6's verify: the body's binds, in order
        // a stream object not bound to an input holds 0xFFFFFFFF (bound: 0xFFFF8000 | index)
        if ((g32(ix) & 0x7FFFu) >= 16) return 1;
        VertexInput& v = S.st.vin[g32(ix) & 15];
        if (addr == 0x006D32D0u) {   // the unbind: the input off (its stride and type kept) ...
            bool in_draw = false;      // ... after the draw it is called from (its draw command already written)
            for (const VEntry& e : s_vstack) if (e.kind == 2) in_draw = true;
            if (in_draw) A.unbind_after |= 1u << (g32(ix) & 15);
            else v.size = 0;
            return 1;
        }
        v.stride = (uint8_t)g8(s + 8); v.size = (uint8_t)(g8(s + 9) & 0xF); v.type = (uint8_t)(g8(s + 10) & 0xF);
        v.location = (uint8_t)(g8(s + 11) & 1); v.frequency = (uint16_t)g16(s + 12); v.offset = g32(s + 16);
        return 1;
    }
    // ---- asp's render state setters (device, ...): their arguments, in asp's
    // own enumerations, mapped to the libgcm constants they write (the setter
    // probe, docs/NATIVE_R5.md section 2)
    case 0x006C01C4:   // depth write mask (device, enable)
        S.st.raster.depth_write = (uint32_t)c->gpr[4] ? 1 : 0;
        return 1;
    case 0x006C49BC: { // depth test (device, func 0..7): 7 (always) turns the test off
        const uint32_t f = (uint32_t)c->gpr[4];
        if (f >= 7) S.st.raster.depth_test = 0;
        else { S.st.raster.depth_test = 1; S.st.raster.depth_func = (uint16_t)(0x0200u + f); }
        return 1;
    }
    case 0x006C4D1C: { // cull (device, mode): 0 none, 1 front, 2 back; clockwise front faces
        const uint32_t m = (uint32_t)c->gpr[4];
        if (!m) S.st.raster.cull = 0;
        else {
            S.st.raster.cull = 1; S.st.raster.front_face = 0x0900u;
            S.st.raster.cull_face = (uint16_t)(m == 1 ? 0x0404u : m == 2 ? 0x0405u : 0x0408u);
        }
        return 1;
    }
    case 0x006C048C:   // blend off (device)
        S.st.raster.blend = 0;
        return 1;
    case 0x006C5298: { // blend on (device, src, dst, op, src alpha, dst alpha, op alpha)
        // asp's factors in GL's order: zero, one, the nine 0x0300-0x0308, then the constant colour's four
        auto factor = [](uint32_t i) -> uint16_t {
            return (uint16_t)(i < 2 ? i : i <= 10 ? 0x0300u + (i - 2) : i <= 14 ? 0x8001u + (i - 11) : 1u);
        };
        auto op = [](uint32_t i) -> uint16_t {
            static const uint16_t k[] = { 0x8006, 0x800A, 0x800B, 0x8007, 0x8008 };   // add, subtract, reverse subtract, min, max
            return i < 5 ? k[i] : 0x8006;
        };
        Raster& r = S.st.raster;
        r.blend = 1;
        r.blend_sf_rgb = factor((uint32_t)c->gpr[4]); r.blend_df_rgb = factor((uint32_t)c->gpr[5]);
        r.blend_eq_rgb = op((uint32_t)c->gpr[6]);
        r.blend_sf_a = factor((uint32_t)c->gpr[7]); r.blend_df_a = factor((uint32_t)c->gpr[8]);
        r.blend_eq_a = op((uint32_t)c->gpr[9]);
        return 1;
    }
    case 0x006C05AC:   // alpha test off (device)
        S.st.raster.alpha_test = 0;
        return 1;
    case 0x006C4284: { // alpha test on (device, func 0..7, reference 0..1)
        const double ref = c->fpr[1];
        S.st.raster.alpha_test = 1;
        S.st.raster.alpha_func = (uint16_t)(0x0200u + ((uint32_t)c->gpr[4] & 7));
        S.st.raster.alpha_ref = (uint8_t)(ref <= 0 ? 0 : ref >= 1 ? 255 : (uint32_t)(ref * 255.0));   // truncated, as asp does
        return 1;
    }
    case 0x006C4508:   // stencil test off (device)
        S.st.raster.stencil_test = 0;
        return 1;
    case 0x006C0A70: { // stencil test on (device, func 0..7, ref, mask, fail, zfail, zpass)
        auto sop = [](uint32_t i) -> uint16_t {
            static const uint16_t k[] = { 0x1E00, 0x0000, 0x1E01, 0x1E02, 0x1E03, 0x150A, 0x8507, 0x8508 };   // keep, zero, replace, incr, decr, invert, incr wrap, decr wrap
            return i < 8 ? k[i] : 0x1E00;
        };
        Raster& r = S.st.raster;
        r.stencil_test = 1;
        r.stencil_func = (uint16_t)(0x0200u + ((uint32_t)c->gpr[4] & 7));
        r.stencil_ref = (uint8_t)c->gpr[5]; r.stencil_func_mask = (uint8_t)c->gpr[6];
        r.stencil_fail = sop((uint32_t)c->gpr[7]); r.stencil_zfail = sop((uint32_t)c->gpr[8]); r.stencil_zpass = sop((uint32_t)c->gpr[9]);
        return 1;
    }
    case 0x006C0E38:   // stencil write mask (device, mask)
        S.st.raster.stencil_write_mask = (uint8_t)c->gpr[4];
        return 1;
    case 0x006C4628: { // polygon offset (device, factor, units): both 0 turns it off
        const float f = (float)c->fpr[1], u = (float)c->fpr[2];
        S.st.raster.poly_offset_fill = f != 0.0f || u != 0.0f;
        S.st.raster.poly_offset_factor = f; S.st.raster.poly_offset_units = u;
        return 1;
    }
    case 0x006C06CC: { // polygon mode (device, mode 0 point, 1 line, 2 fill): both faces
        const uint32_t m = (uint32_t)c->gpr[4];
        S.st.raster.front_poly_mode = S.st.raster.back_poly_mode = (uint16_t)(0x1B00u + (m < 3 ? m : 2));
        return 1;
    }
    case 0x006C0F60: { // viewport (device, x, y, w, h, zmin, zmax): asp's depth maps [zmin, zmax] from 0..1
        Viewport& v = S.st.viewport;
        const int32_t x = (int32_t)c->gpr[4], y = (int32_t)c->gpr[5], w = (int32_t)c->gpr[6], h = (int32_t)c->gpr[7];
        const float zmin = (float)c->fpr[1], zmax = (float)c->fpr[2];
        v.x = (uint16_t)x; v.y = (uint16_t)y; v.w = (uint16_t)w; v.h = (uint16_t)h;
        v.zmin = zmin; v.zmax = zmax;
        v.scale[0] = w * 0.5f; v.scale[1] = h * -0.5f; v.scale[2] = zmax - zmin; v.scale[3] = 0.0f;
        v.offset[0] = x + w * 0.5f; v.offset[1] = y + h * 0.5f; v.offset[2] = 0.0f; v.offset[3] = 0.0f;   // R6: z 0 (the body)
        return 1;
    }
    case 0x006C08F0:   // scissor (device, x, y, w, h)
        S.st.scissor = { (uint16_t)c->gpr[4], (uint16_t)c->gpr[5], (uint16_t)c->gpr[6], (uint16_t)c->gpr[7] };
        return 1;
    case 0x006C5170:   // scissor off (device, ...): the whole 4096 x 4096
        S.st.scissor = { 0, 0, 0x0FFF, 0x0FFF };
        return 1;
    case 0x006D11BC:   // a pass's end (render target): every colour channel written, no scissor
        S.st.scissor = { 0, 0, 0x0FFF, 0x0FFF };
        S.st.raster.color_mask = 0x01010101u; S.st.raster.color_mask_by = kByAsp;
        return 1;
    case 0x006C37A0: { // sampler bind (device, unit, enable): the unit's entry in asp's sampler table
        const uint32_t u = (uint32_t)c->gpr[4] & 0xFF, en = (uint32_t)c->gpr[5] & 0xFF;
        if (u >= 16) return 1;
        Sampler& m = S.st.samp[u];
        m.enable = en ? 1 : 0;
        if (!en) { A.stored.samp[u].enable = 0; return 1; }   // control 0 alone
        // the table (TOC -0x2D88): an entry of 48 bytes a unit from +0x0C
        const uint32_t e = g32((uint32_t)c->gpr[2] - 0x2D88u) + 0x0Cu + 48u * u;
        if (!ea_ok(e, 48)) return 1;
        m.min_lod = (uint16_t)(g8(e + 3) << 8); m.max_lod = 0x0C00;   // 4.8 fixed point: the first level, 12
        // R6 (030:55715-55770): the low byte of (int)ratio, 0 taken as 1, past 16 as 16
        const float an = gfloat(e + 0x10);
        uint32_t n = (uint32_t)(int32_t)an & 0xFF;
        if (!n) n = 1;
        if (n > 16) n = 16;
        // the filter's LOD bias comes from the float at +4 (not modelled: 0);
        // the binds that would give one are counted
        if ((g8(e + 0x2D) || g8(e + 0x2C)) && gfloat(e + 4) != 0.0f) A.samp_bias_binds++;
        const uint32_t at = g32((uint32_t)c->gpr[2] - 0x2D94u);   // anisotropy codes by ratio
        m.max_aniso = ea_ok(at + 0x40 + n - 1, 1) ? (uint8_t)(g8(at + 0x40 + n - 1) & 7) : 0;
        // the address mode with the border colour, and the filter: written
        // (into the command buffer and the store) only when the entry marks
        // them changed -- its dirty byte at +0x2E for the first, +0x2D or
        // +0x2C for the filter, which the bind clears (read here before it runs)
        Sampler nw = m;
        nw.min_filter = (uint8_t)(g32(e + 0x0C) + 1); nw.mag_filter = (uint8_t)(g32(e + 0x08) + 1);
        nw.conv = 1; nw.bias = 0; nw.filter_signed = 0;
        nw.wrap_s = (uint8_t)(g32(e + 0x14) + 1); nw.wrap_t = (uint8_t)(g32(e + 0x18) + 1); nw.wrap_r = 3;   // R clamped
        nw.unsigned_remap = 0; nw.gamma = 0; nw.zfunc = 6;   // the depth comparison GEQUAL
        auto ch = [](float f) -> uint32_t { return f <= 0 ? 0u : f >= 1 ? 255u : (uint32_t)(f * 255.0f + 0.5f); };
        nw.border = ch(gfloat(e + 0x28)) << 24 | ch(gfloat(e + 0x1C)) << 16 | ch(gfloat(e + 0x20)) << 8 | ch(gfloat(e + 0x24));
        Sampler& st = A.stored.samp[u];
        // control 0 (enable, levels, anisotropy): always written
        st.enable = m.enable; st.min_lod = m.min_lod; st.max_lod = m.max_lod; st.max_aniso = m.max_aniso;
        if (g8(e + 0x2E)) {   // the address mode and the border colour
            m.wrap_s = st.wrap_s = nw.wrap_s; m.wrap_t = st.wrap_t = nw.wrap_t; m.wrap_r = st.wrap_r = nw.wrap_r;
            m.zfunc = st.zfunc = nw.zfunc; m.gamma = st.gamma = nw.gamma; m.unsigned_remap = st.unsigned_remap = nw.unsigned_remap;
            m.border = st.border = nw.border;
        }
        if (g8(e + 0x2D) || g8(e + 0x2C)) {   // the filter
            m.min_filter = st.min_filter = nw.min_filter; m.mag_filter = st.mag_filter = nw.mag_filter;
            m.conv = st.conv = nw.conv; m.bias = st.bias = nw.bias; m.filter_signed = st.filter_signed = nw.filter_signed;
        }
        m.address_by = st.address_by = kByAsp;
        return 1;
    }
    case 0x006D0E84: { // the shadow map (render target, r5 linear, r6 the unit): its texture, a fixed sampler
        const uint32_t rt = (uint32_t)c->gpr[3];
        if (!ea_ok(rt + 0x14, 24)) return 1;
        // R6 (031:8912, 0x006C960C): the unit is (r6 >> 32) & 0x7FFF (4 in R5's
        // runs); the filters nearest when r5 is 0, linear otherwise
        const uint32_t unit = (uint32_t)(c->gpr[6] >> 32) & 0x7FFFu;
        if (unit >= 16) return 1;
        enc_texture_at(unit, rt + 0x14);
        Sampler& m = S.st.samp[unit];
        const uint8_t flt = (uint32_t)c->gpr[5] ? 2 : 1;
        m.enable = 1; m.min_lod = 0; m.max_lod = 0; m.max_aniso = 0;
        m.min_filter = flt; m.mag_filter = flt; m.conv = 1; m.bias = 0; m.filter_signed = 0;
        m.wrap_s = m.wrap_t = m.wrap_r = 3; m.unsigned_remap = 0; m.gamma = 0; m.zfunc = 6;   // clamped; GEQUAL
        m.address_by = kByAsp;
        return 1;
    }
    case 0x006C8A34: { // render target set (render target, index): its CellGcmSurface at +0x40 + 0x3C x index
        // R6 (030:61686-61723): r4 the index; a surface whose colour offset is -1 sets nothing
        const uint32_t sf = (uint32_t)c->gpr[3] + 0x40u + 0x3Cu * ((uint32_t)c->gpr[4] & 0xFF);
        if (!ea_ok(sf, 0x3C) || g32(sf + 8) == 0xFFFFFFFFu) return 1;
        enc_surface(sf);
        return 1;
    }
    case 0x006D1764: { // a pass's start (render target, ...): its CellGcmSurface at +0x2C, the pass's own state
        const uint32_t rt = (uint32_t)c->gpr[3];
        // R6 (031:9549-9817): nothing when +0x28 or +0x58 is -1 (R5 tested 0)
        if (!ea_ok(rt, 0x80) || g32(rt + 0x28) == 0xFFFFFFFFu || g32(rt + 0x58) == 0xFFFFFFFFu) return 1;
        enc_surface(rt + 0x2C);
        Raster& r = S.st.raster;
        const bool colour = g8(rt + 0x0C) != 0;
        Rec::AspClear k;
        memset(&k, 0, sizeof k);
        if (!colour) {   // a depth pass: colour off, depth test and write on, stencil off; depth 1.0, stencil 0xFF
            r.color_mask = 0; r.color_mask_by = kByAsp;
            r.depth_test = 1; r.depth_write = 1; r.stencil_test = 0;
            k.mask = 0x03u; k.depth = 0xFFFFFFu; k.stencil = 0xFFu;
        } else {         // a colour pass: depth test and stencil off (the colour mask as it was); colour white
            r.depth_test = 0; r.stencil_test = 0;
            k.mask = 0xF0u; k.argb = 0xFFFFFFFFu;
            for (int j = 0; j < 4; j++) k.rgba[j] = 1.0f;
            A.pass_colour++;
        }
        A.cmask_unknown = 0;
        k.surface = S.st.surface;
        asp_pend_clear(k);
        // a scissor one pixel inside the target's edges (+0x04, +0x08 its size),
        // only with r5 set -- into the command buffer alone, not the store
        const uint32_t w = g32(rt + 4), h = g32(rt + 8);
        if ((uint32_t)c->gpr[5] && w > 2 && h > 2) S.st.scissor = { 1, 1, (uint16_t)(w - 2), (uint16_t)(h - 2) };
        return 1;
    }
    case 0x006C2D3C: { // clear (device, flags 1 colour 2 depth 4 stencil, const float rgba[4]*, stencil; depth f1)
        const uint32_t fl = (uint32_t)c->gpr[4], cp = (uint32_t)c->gpr[5];
        Rec::AspClear k;
        memset(&k, 0, sizeof k);
        k.mask = (fl & 1 ? 0xF0u : 0) | (fl & 2 ? 0x01u : 0) | (fl & 4 ? 0x02u : 0);
        // R6 (030:54863): each channel the low byte of (int)(x * 255), the depth
        // (int)(d * 2^24 - 1); the stencil in r7 (R5 read r6 and rounded)
        if (fl & 1 && ea_ok(cp, 16)) {
            for (int i = 0; i < 4; i++) k.rgba[i] = gfloat(cp + 4 * i);
            auto ch = [](float f) -> uint32_t { return (uint32_t)(int32_t)(f * 255.0f) & 0xFFu; };
            k.argb = ch(k.rgba[3]) << 24 | ch(k.rgba[0]) << 16 | ch(k.rgba[1]) << 8 | ch(k.rgba[2]);
        }
        const double d = c->fpr[1];
        k.depth = (uint32_t)(int32_t)(d * 16777215.0) & 0xFFFFFFu;
        k.stencil = (uint32_t)c->gpr[7] & 0xFF;
        k.surface = S.st.surface;
        if (!k.mask) return 1;
        asp_pend_clear(k);
        return 1;
    }
    case 0x006C9C74: { // vertex constants (this, {start, ..}, const float*, count): whole vec4s
        // R6 (the body, 031:640-839): a handle with bit 31 clear is a fragment
        // program's parameter -- (h >> 15) & 0x7FFF -- patched through
        // 0x006C9624 (observed on its own while the body runs); R5 wrote it
        // into the vertex constants at h & 0x7FFF (the cross-check's c0
        // 3F000000 against the engine's 0)
        if ((int32_t)(uint32_t)(c->gpr[4] >> 32) >= 0) { A.fp_const_calls++; return 1; }
        const uint32_t start = (uint32_t)(c->gpr[4] >> 32) & 0x7FFFu, data = (uint32_t)c->gpr[5];
        const uint32_t n = ((uint32_t)c->gpr[6] + 3u) & ~3u;
        if (!n || !ea_ok(data, n * 4)) return 1;
        for (uint32_t i = 0; i < n && start + i / 4 < 512; i++) {
            S.vconst[start + i / 4][i % 4] = g32(data + 4 * i);
            S.vconst_written[(start + i / 4) >> 5] |= 1u << ((start + i / 4) & 31);
            S.vc_dirty[(start + i / 4) >> 5] |= 1u << ((start + i / 4) & 31);
        }
        return 1;
    }
    // ---- asp's programs. A shader object (vtables 0xFEAB48 vertex, 0xFEAB30
    // fragment) holds at +0x18 its container ("CGB": at +0x08 the microcode's
    // bytes in the high half, at +0x0C the input mask in the high half), at
    // +0x1C the microcode (the container's +0x20).
    case 0x006CA3D0: { // vertex program upload and bind (shader object): at slot 0, with its own constants
        const uint32_t self = (uint32_t)c->gpr[3];
        if (!ea_ok(self, 0x30)) return 1;
        const uint32_t cont = g32(self + 0x18), uc = g32(self + 0x1C);
        if (!ea_ok(cont, 0x20)) return 1;
        // R6 (0x009569FC): the slot at the binary's +0x18 (0 in every probe), the
        // register count at +0x0E
        const uint32_t bytes = g16(cont + 8), slot = g16(cont + 0x18) < 544 ? g16(cont + 0x18) : 0;
        if (!bytes || !ea_ok(uc, bytes)) return 1;
        for (uint32_t i = 0; i < bytes / 4 && slot * 4 + i < kVpWords; i++) S.vp[slot * 4 + i] = g32(uc + 4 * i);
        VertexProgram& v = S.st.vp;
        v.start = (uint16_t)slot; v.input_mask = g16(cont + 0x0C); v.regs_le32 = g8(cont + 0x0E) <= 32;
        S.vp_gen++;
        // its constants: +0x20 their count, +0x24 their registers (u16 each: R6,
        // 0x00956B14; R5 read 12-byte entries), +0x28 the values
        const uint32_t n = g32(self + 0x20), tbl = g32(self + 0x24), vals = g32(self + 0x28);
        for (uint32_t k = 0; k < n && k < 64 && ea_ok(tbl + 2 * k, 2) && ea_ok(vals + 16 * k, 16); k++) {
            const uint32_t reg = g16(tbl + 2 * k);
            if (reg >= 512) continue;
            for (uint32_t j = 0; j < 4; j++) S.vconst[reg][j] = g32(vals + 16 * k + 4 * j);
            S.vconst_written[reg >> 5] |= 1u << (reg & 31);
            S.vc_dirty[reg >> 5] |= 1u << (reg & 31);
        }
        return 1;
    }
    case 0x006CADE4: { // fragment program upload (shader object): its microcode into local memory at +0x54
        const uint32_t self = (uint32_t)c->gpr[3];
        if (!ea_ok(self, 0x70)) return 1;
        if (g32(self + 0x4C) != 0xFFFFFFFFu) return 1;   // R6 (031:1934): marked dirty only, nothing uploaded
        const uint32_t cont = g32(self + 0x18), uc = g32(self + 0x1C), off = g32(self + 0x54);
        if (!ea_ok(cont, 0x20)) return 1;
        const uint32_t bytes = g16(cont + 8);
        if (!bytes || !ea_ok(uc, bytes) || off >= 0x10000000u) return 1;
        shadow_write(off, vm_base + uc, bytes & ~3u, ++s_epoch);
        owner_set(off, bytes & ~3u, self);
        A.fp_bytes[off] = bytes;
        A.fp_last_obj[off] = self;
        A.fp_slot_epoch[off] = s_epoch;
        return 1;
    }
    case 0x006C9624:   // fragment constant patch (shader object, parameter, count, const float*)
        fp_patch_record((uint32_t)c->gpr[3], (uint32_t)c->gpr[4], (uint32_t)c->gpr[5], (uint32_t)c->gpr[6]);
        return 1;
    case 0x006C99B0: { // fragment program bind (program pair): the fragment shader object at +0xF0 (= +0x1C)
        const uint32_t pair = (uint32_t)c->gpr[3];
        if (!ea_ok(pair, 0xF4)) return 1;
        const uint32_t fo = g32(pair + 0xF0);
        if (!ea_ok(fo, 0x70)) return 1;
        // R6 (031:427-546): with the object's dirty byte (+0x50) clear the bind
        // emits nothing -- the GPU keeps the program it has -- and clears it
        if (!g8(fo + 0x50)) { A.fp_quiet_binds++; return 1; }
        FragmentProgram& f = S.st.fp;
        f.offset = g32(fo + 0x54); f.location = 0;   // local memory
        f.input_mask = g32(fo + 0x58);
        f.tex_mask = (uint16_t)g16(fo + 0x5C); f.tex_2d = (uint16_t)g16(fo + 0x5E); f.tex_centroid = (uint16_t)g16(fo + 0x60);
        // the shader control as the body writes it (031:508-540): the register
        // count, at least 2, over the whole word at +0x64
        const uint32_t rc = g8(fo + 0x68) < 2 ? 2u : g8(fo + 0x68);
        const uint32_t ctl = rc << 24 | g32(fo + 0x64);
        f.reg_count = (uint8_t)(ctl >> 24); f.txp = (uint8_t)(ctl >> 15 & 1); f.output_from_h0 = !(ctl & 0x40u);
        f.depth_replace = (ctl & 0xEu) != 0; f.pixel_kill = (uint8_t)(ctl >> 7 & 1);
        A.fp_bound = 1; A.fp_bound_obj = fo;
        // a program asp binds without uploading it is one it holds resident at
        // its slot: the slot's bytes are its microcode (asp's patches since apply on top)
        auto lo = A.fp_last_obj.find(f.offset);
        if (lo == A.fp_last_obj.end() || lo->second != fo) {
            const uint32_t cont = g32(fo + 0x18), uc = g32(fo + 0x1C);
            const uint32_t bytes = ea_ok(cont, 0x20) ? g16(cont + 8) : 0;
            if (bytes && ea_ok(uc, bytes) && f.offset < 0x10000000u) {
                // its words, but those asp patched since the slot last got a
                // program: the patches were this program's (made before the bind)
                const uint64_t since = A.fp_slot_epoch[f.offset], ep = ++s_epoch;
                for (uint32_t i = 0; i + 4 <= (bytes & ~3u); i += 4) {
                    ShadowPage* pg = shadow_page(f.offset + i);
                    if (pg->ep[((f.offset + i) & 0xFFF) >> 2] > since) {
                        // patched since: kept if it was this program's own patch
                        if (owner_get(f.offset + i) == fo) continue;
                        A.fp_foreign_patch_words++;
                    }
                    shadow_write(f.offset + i, vm_base + uc + i, 4, ep);
                    owner_set(f.offset + i, 4, fo);
                }
                A.fp_bytes[f.offset] = bytes;
                A.fp_last_obj[f.offset] = fo;
                A.fp_slot_epoch[f.offset] = ep;
                A.fp_resident_binds++;
            }
        }
        return 1;
    }
    default:
        return 0;
    }
}

// R5's setter probe (DOTHEROES_ASP_PROBE=2, development): for each of asp's
// observed methods but its draw, up to 16 distinct calls -- by the command
// words they wrote -- with their arguments (r3-r8, f1-f4), the first words of
// what r3-r5 point at, and the words written into the command context
// between the call and its return. What maps asp's arguments to the state
// they set (docs/NATIVE_R5.md).
int asp_probe2()
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DOTHEROES_ASP_PROBE"); on = e && *e == '2'; }
    return on;
}

std::map<uint32_t, uint32_t> s_probe_distinct;   // per function: distinct calls logged

// asp's command context: through its TOC (the setters' own load, TOC -0x2D9C)
uint32_t asp_cmd_ctx(const ppu_context* c)
{
    const uint32_t p = g32((uint32_t)c->gpr[2] - 0x2D9Cu);
    return ea_ok(p, 4) ? g32(p) : 0;
}

bool asp_probe_enter(uint32_t addr, ppu_context* c)
{
    if (!asp_probe2() || addr == 0x006C6F88u || addr == kAspAppend) return false;
    if (s_probe_distinct[addr] >= 24) return false;   // enough of each to see its distinct uses
    VEntry v;
    memset(&v, 0, sizeof v);
    v.addr = addr; v.kind = 3; v.lr = (uint32_t)c->lr;
    r4_ctx_fields(v, asp_cmd_ctx(c));
    memcpy(v.gpr, c->gpr, sizeof v.gpr);
    memcpy(v.fpr, c->fpr, sizeof v.fpr);
    s_vstack.push_back(v);
    return true;
}

void asp_probe_exit(const VEntry& v)
{
    if (!ea_ok(v.ctx_ea, 16)) return;
    const uint32_t cur = g32(v.ctx_ea + 8);
    if (cur < v.cur || cur - v.cur > 4096) return;   // wrapped, or not this context
    const uint64_t h = fnv(vm_base + v.cur, cur - v.cur, v.addr);
    static std::map<uint32_t, std::set<uint64_t>> seen;
    std::set<uint64_t>& m = seen[v.addr];
    if (m.size() >= 24 || !m.insert(h).second) return;
    char line[4000];
    int at = snprintf(line, sizeof line, "asp probe2 %08X lr %08X r3 %08X r4 %08X r5 %08X r6 %08X r7 %08X r8 %08X r9 %08X r10 %08X "
                      "f1 %g f2 %g f3 %g f4 %g |", v.addr, v.lr, (uint32_t)v.gpr[3], (uint32_t)v.gpr[4], (uint32_t)v.gpr[5],
                      (uint32_t)v.gpr[6], (uint32_t)v.gpr[7], (uint32_t)v.gpr[8], (uint32_t)v.gpr[9], (uint32_t)v.gpr[10],
                      v.fpr[1], v.fpr[2], v.fpr[3], v.fpr[4]);
    auto dump = [&](const char* name, uint32_t p, uint32_t n) {
        if (!ea_ok(p, n)) return;
        at += snprintf(line + at, sizeof line - at, " %s@%08X:", name, p);
        for (uint32_t i = 0; i < n && at < (int)sizeof line - 16; i += 4) at += snprintf(line + at, sizeof line - at, " %08X", g32(p + i));
    };
    dump("*r3", (uint32_t)v.gpr[3], 0x80);
    dump("*r4", (uint32_t)v.gpr[4], 0x40);
    dump("*r5", (uint32_t)v.gpr[5], 0x40);
    if (v.addr == 0x006CA3D0u || v.addr == 0x006CADE4u || v.addr == 0x006C9624u) {   // a shader object: its container
        const uint32_t self = (uint32_t)v.gpr[3];
        if (ea_ok(self + 0x18, 8)) dump("container", g32(self + 0x18), 0x60);
        if (ea_ok(self + 0x24, 12)) { dump("[24]", g32(self + 0x24), 0x30); dump("[28]", g32(self + 0x28), 0x30); dump("[30]", g32(self + 0x30), 0x30); }
    }
    if (v.addr == 0x006C37A0u) {   // the sampler bind: the unit's entry in asp's sampler table (TOC -0x2D88, 48 bytes a unit)
        const uint32_t tbl = g32((uint32_t)v.gpr[2] - 0x2D88u);
        dump("samplers", tbl + 48u * ((uint32_t)v.gpr[4] & 15), 48);
    }
    at += snprintf(line + at, sizeof line - at, " | words %u:", (cur - v.cur) / 4);
    for (uint32_t w = v.cur; w < cur && at < (int)sizeof line - 16; w += 4) at += snprintf(line + at, sizeof line - at, " %08X", g32(w));
    report_add(line);
    s_probe_distinct[v.addr] = (uint32_t)m.size();
}

// R5's bypass, stage 2 (DOTHEROES_ASP_BYPASS=2): asp's state functions and
// program uploads run, and the commands they wrote are taken back at their
// return (not when they wrapped the ring, nor when a marker was written
// inside them: counted). The native renderer has their state from the
// records; the engine, which draws none of asp's, does not need it.
int asp_bypass();
// =2: the program traffic alone -- vertex program uploads, fragment program
// uploads and their constant patches (most of asp's words); =3 every state
// function too, which the engine's own remaining draws (the HUD, the menus)
// inherit state from (blend, alpha test, depth, surface): not for use
// before R6 makes those draws native (docs/NATIVE_R5.md section 7)
bool asp_state_bypassed_fn(uint32_t addr)
{
    if (asp_bypass() >= 3) return true;
    return addr == 0x006CA3D0u || addr == 0x006CADE4u || addr == 0x006C9624u;
}

// R6 (DOTHEROES_R6_PRODUCERS=1 with the bypass): asp's clears and its
// passes' starts (which clear inline) touch the surfaces they clear -- the
// display buffer among them -- in the engine, which then wants the native
// image back (a handback) for nothing: with every draw native, their words
// are taken back at their return as stage 2 takes back its state calls'
int r6_producers();
bool r6_takeback_fn(uint32_t addr)
{
    return r6_producers() && asp_bypass() >= 1 && (addr == 0x006C2D3Cu || addr == 0x006D1764u);
}

bool asp_state_bypass_enter(uint32_t addr, ppu_context* c)
{
    const bool r6 = r6_takeback_fn(addr);
    if (!r6 && (asp_bypass() < 2 || addr == 0x006C6F88u || !asp_state_bypassed_fn(addr))) return false;
    // after the frame's final composite the engine draws the HUD and the menus
    // with asp's programs: from there on, asp's uploads are the engine's (kept)
    if (!r6 && S.final_frame == S.frame) return false;
    for (const VEntry& e : s_vstack) if (e.kind == 2 || e.kind == 4) return false;   // inside a draw or another: theirs
    VEntry v;
    memset(&v, 0, sizeof v);
    v.addr = addr; v.kind = 4;
    r4_ctx_fields(v, addr == kAspAppend ? (uint32_t)c->gpr[3] : asp_cmd_ctx(c));
    if (v.ctx_ea != S.ring_ctx) return false;   // another context's (asp's state store being built): left alone
    v.lap = cellGcm_title_laps_pub(nullptr);
    v.store_from = (size_t)stats().markers_written.load();
    s_vstack.push_back(v);
    return true;
}

void asp_state_bypass_exit(const VEntry& v)
{
    if (!ea_ok(v.ctx_ea, 16)) return;
    const uint32_t now = g32(v.ctx_ea + 8);
    if ((size_t)stats().markers_written.load() != v.store_from) { stats().asp_state_kept++; return; }
    if (cellGcm_title_laps_pub(nullptr) != v.lap || now < v.cur || now - v.cur > (4u << 20)) { stats().asp_state_kept++; return; }
    p32(v.ctx_ea + 8, v.cur);
    stats().asp_state_bypassed++;
    stats().asp_state_words += (now - v.cur) / 4;
    if (r6_takeback_fn(v.addr)) { stats().r6_takeback++; stats().r6_takeback_words += (now - v.cur) / 4; }
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

int s_cc_on_fwd();
void write_call_counts(FILE* f);
void write_report()
{
    FILE* f = fopen(s_rep_path, "w");
    if (!f) return;
    Stats& s = stats();
    Queue& q = queue();
    fprintf(f, "# ppfx report at %.1f s (mode %d)\n", now_s(), mode());
    write_call_counts(f);
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
    fprintf(f, "recorder_ms %.3f (at the draws' returns %.3f) queue_full_waits %llu queue_full_wait_ms %.3f flushes_for_room %llu exit_signal_skips %llu\n",
            s.rec_ns / 1e6, s.rec_exit_ns / 1e6, (unsigned long long)q.full_waits, q.full_wait_us / 1e3, (unsigned long long)s.flushes_for_room,
            (unsigned long long)s.exit_signal_skips);
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
    fprintf(f, "handoffs %llu failed %llu shared %llu | textures from guest memory at the walker %llu (%.1f MB) failed %llu\n",
            (unsigned long long)s.handoffs, (unsigned long long)s.handoff_fail, (unsigned long long)s.handoffs_shared,
            (unsigned long long)s.guest_tex, s.guest_tex_bytes / 1048576.0, (unsigned long long)s.guest_tex_fail);
    if (bypass_mode()) {
        fprintf(f, "r4 mode %d frames %llu records %llu calls_bypassed %llu calls_kept %llu exit_mismatch %llu | "
                   "markers written %llu wraps %llu failed %llu met %llu unknown %llu | r3_matched_in_r4_frames %llu\n",
                bypass_mode(), (unsigned long long)s.r4_frames, (unsigned long long)s.r4_records,
                (unsigned long long)s.r4_skipped, (unsigned long long)s.r4_kept, (unsigned long long)s.r4_exit_mismatch,
                (unsigned long long)s.markers_written, (unsigned long long)s.marker_wraps,
                (unsigned long long)s.marker_fail, (unsigned long long)s.markers_met,
                (unsigned long long)s.marker_unknown, (unsigned long long)s.matched_r3_in_r4);
        fprintf(f, "r4 handbacks %llu failed %llu ms %.1f (waiting for the native side %.1f) | by kind: target %llu "
                   "texture %llu transfer %llu present %llu clear %llu | checked %llu equal %llu differ %llu | "
                   "present_hashes %llu\n",
                (unsigned long long)s.handbacks, (unsigned long long)s.handback_fail, s.handback_ns / 1e6,
                s.handback_wait_ns / 1e6, (unsigned long long)s.handbacks_by_kind[0].load(),
                (unsigned long long)s.handbacks_by_kind[1].load(), (unsigned long long)s.handbacks_by_kind[2].load(),
                (unsigned long long)s.handbacks_by_kind[3].load(), (unsigned long long)s.handbacks_by_kind[4].load(),
                (unsigned long long)s.hb_checked, (unsigned long long)s.hb_equal, (unsigned long long)s.hb_differ,
                (unsigned long long)s.present_hashes);
        std::lock_guard<std::mutex> g(s_bp_lock);
        for (auto& kv : s_bp) {
            const BpCount& k = kv.second;
            fprintf(f, "r4 entry %08X class %s ppfx_calls %llu bypassed %llu kept %llu inside_kept %llu "
                       "ran_before_ready %llu%s%s\n", kv.first,
                    k.why[0] ? "kept" : "bypassed", (unsigned long long)k.ppfx, (unsigned long long)k.skipped,
                    (unsigned long long)k.kept, (unsigned long long)k.inside_kept, (unsigned long long)k.before_ready,
                    k.why[0] ? " | " : "", k.why);
            if (k.verified)
                fprintf(f, "r4 verify %08X runs %llu r3_unchanged %llu r3_changed %llu (last caller %08X) f1_changed %llu "
                           "abi_kept_regs_changed %llu volatile_changed %llu | context moved %llu reserve_callbacks %llu "
                           "command_words %llu | stores: commands %llu context %llu own_frame %llu caller_abi_slots %llu "
                           "other %llu (last %08X from %08X)\n", kv.first,
                        (unsigned long long)k.verified, (unsigned long long)k.r3_same, (unsigned long long)k.r3_diff,
                        k.last_r3_diff_lr, (unsigned long long)k.f1_diff, (unsigned long long)k.nonvol_diff,
                        (unsigned long long)k.vol_diff, (unsigned long long)k.ctx_moved,
                        (unsigned long long)k.callbacks, (unsigned long long)k.words, (unsigned long long)k.st_cmd,
                        (unsigned long long)k.st_ctx, (unsigned long long)k.st_stack,
                        (unsigned long long)k.st_caller_abi, (unsigned long long)k.st_other, k.last_other_ea,
                        k.last_other_lr);
        }
    }
    fprintf(f, "native frames_with_records %llu\n", (unsigned long long)s.n_frames);
    if (asp_records_on())
        fprintf(f, "asp records %llu matched %llu missed %llu engine_draws_in_them %llu | crosscheck draws %llu "
                   "with_differences %llu\n", (unsigned long long)s.asp_records, (unsigned long long)s.asp_matched,
                (unsigned long long)s.asp_missed, (unsigned long long)s.asp_draw_events,
                (unsigned long long)s.asp_xc_draws, (unsigned long long)s.asp_xc_differ);
    if (asp_records_on())
        fprintf(f, "asp store_group_restores %llu fp_patches_of_odd_count %llu fp_programs_uploaded_at %zu "
                   "draws_with_a_program_not_uploaded_last %llu store_rebuilds %llu sampler_restores %llu "
                   "resident_binds_other_objects_patch_words %llu fragment_param_calls %llu quiet_binds %llu "
                   "sampler_binds_with_a_lod_bias %llu colour_passes %llu\n",
                (unsigned long long)s.asp_group_restores, (unsigned long long)A.fp_odd_patches, A.fp_bytes.size(),
                (unsigned long long)A.fp_stale_draws, (unsigned long long)A.rebuilds, (unsigned long long)A.samp_restores,
                (unsigned long long)A.fp_foreign_patch_words, (unsigned long long)A.fp_const_calls,
                (unsigned long long)A.fp_quiet_binds, (unsigned long long)A.samp_bias_binds,
                (unsigned long long)A.pass_colour);
    if (s.asp_state_bypassed || s.asp_state_kept)
        fprintf(f, "asp state calls bypassed %llu (%.1f M words taken back) kept %llu\n", (unsigned long long)s.asp_state_bypassed,
                s.asp_state_words / 1e6, (unsigned long long)s.asp_state_kept);
    if (s.asp_bypassed || s.asp_not_bypassed)
        fprintf(f, "asp bypassed draws %llu (%.1f M words taken back) not bypassed %llu (their commands wrapped the ring) "
                   "marker_fail %llu\n", (unsigned long long)s.asp_bypassed, s.asp_bypassed_words / 1e6,
                (unsigned long long)s.asp_not_bypassed, (unsigned long long)s.asp_marker_fail);
    if (s.asp_tex_snapshots)
        fprintf(f, "asp textures taken as the game drew them %llu\n", (unsigned long long)s.asp_tex_snapshots);
    if (s.game_draw_records)
        fprintf(f, "game draw records (cellGcmSetDrawIndexArray inside asp's scene) %llu\n", (unsigned long long)s.game_draw_records);
    if (r6_producers())
        fprintf(f, "r6 producers: game clears %llu (narrower than their surface %llu) game draw-arrays %llu | "
                   "game calls whose body did not run %llu\n", (unsigned long long)s.r6_game_clears,
                (unsigned long long)s.r6_clears_scissored, (unsigned long long)s.r6_game_arrays,
                (unsigned long long)s.r6_game_skipped);
    if (r6_skip())
        fprintf(f, "r6 skipped bodies: asp draws %llu game draws %llu | left to the body: arguments %llu context %llu "
                   "stream set %llu | cache hits %llu, no draw %llu, more stream sets %llu, count overrides %llu | verify: draws %llu binds equal %llu differ %llu, "
                   "stream words wrong %llu, counter wrong %llu, cache not clear %llu, r3 not 0 %llu\n",
                (unsigned long long)s.skip_asp_draws.load(), (unsigned long long)s.skip_game_draws.load(),
                (unsigned long long)s.skip_fallback_args.load(), (unsigned long long)s.skip_fallback_ctx.load(),
                (unsigned long long)s.skip_fallback_set.load(), (unsigned long long)s.skip_cache_hits.load(),
                (unsigned long long)s.skip_no_draw.load(), (unsigned long long)s.skip_extra_sets.load(),
                (unsigned long long)s.skip_count_override.load(), (unsigned long long)s.skv_draws.load(),
                (unsigned long long)s.skv_binds_equal.load(), (unsigned long long)s.skv_binds_differ.load(),
                (unsigned long long)s.skv_stream_words_bad.load(), (unsigned long long)s.skv_counter_bad.load(),
                (unsigned long long)s.skv_cache_bad.load(), (unsigned long long)s.skv_r3_bad.load());
    if (r6_skip() == 2)
        fprintf(f, "r6 state verify: calls %llu state equal %llu differ %llu (surface %llu viewport %llu scissor %llu "
                   "raster %llu samplers %llu textures %llu inputs %llu fp %llu) | guest stores wrong %llu, r3 wrong %llu\n",
                (unsigned long long)s.sksv_calls.load(), (unsigned long long)s.sksv_state_equal.load(),
                (unsigned long long)s.sksv_state_differ.load(), (unsigned long long)s.sksv_differ_by[0].load(),
                (unsigned long long)s.sksv_differ_by[1].load(), (unsigned long long)s.sksv_differ_by[2].load(),
                (unsigned long long)s.sksv_differ_by[3].load(), (unsigned long long)s.sksv_differ_by[4].load(),
                (unsigned long long)s.sksv_differ_by[5].load(), (unsigned long long)s.sksv_differ_by[6].load(),
                (unsigned long long)s.sksv_differ_by[7].load(), (unsigned long long)s.sksv_stores_bad.load(),
                (unsigned long long)s.sksv_r3_bad.load());
    if (r6_skip() == 3) {
        fprintf(f, "r6 libgcm calls whose body did not run (=all), by entry point (PPFX's kept/game's):");
        for (const SkipCount& k : s_r6sk)
            if (k.addr) fprintf(f, " %08X %llu/%llu", k.addr, (unsigned long long)k.ppfx, (unsigned long long)k.game);
        fprintf(f, "\n");
    }
    if (r6_skip() == 3)
        fprintf(f, "r6 skipped state functions %llu (left to the body: not the title's context %llu) | re-emissions "
                   "applied from the model %llu\n", (unsigned long long)s.skip_state.load(),
                (unsigned long long)s.skip_state_left.load(), (unsigned long long)s.skip_restores.load());
    if (r6_skip())
        fprintf(f, "r6 skipped program functions %llu (the heap's first set-up left to the body %llu) | verify: calls %llu "
                   "fields wrong %llu r3 wrong %llu\n", (unsigned long long)s.skip_prog.load(),
                (unsigned long long)s.skip_prog_heap.load(), (unsigned long long)s.skpv_calls.load(),
                (unsigned long long)s.skpv_fields_bad.load(), (unsigned long long)s.skpv_r3_bad.load());
    if (r6_producers())
        fprintf(f, "r6 asp clears and pass starts taken back %llu (%.2f M words)\n",
                (unsigned long long)s.r6_takeback.load(), s.r6_takeback_words.load() / 1e6);
    if (r6_occlusion())
        fprintf(f, "r6 occlusion: operations %llu | counted draws %llu, reports written %llu (other types %llu, "
                   "missing counts %llu, the last %llu), transfers %llu, surfaces written back %llu | conditional "
                   "render enables %llu\n", (unsigned long long)s.occ_ops.load(), (unsigned long long)s.occ_draws.load(),
                (unsigned long long)s.occ_reports.load(), (unsigned long long)s.occ_other_types.load(),
                (unsigned long long)s.occ_missing.load(), (unsigned long long)s.occ_sum_last.load(),
                (unsigned long long)s.occ_transfers.load(), (unsigned long long)s.occ_writebacks.load(),
                (unsigned long long)s.occ_cond_render.load());
    if (r6_skip() == 3 || s.vpdv_programs)
        fprintf(f, "r6 vertex program defaults (cellGcmSetVertexProgram's body skipped): programs %llu, constants set %llu "
                   "(matrices %llu) | verify (body run): programs %llu (the game's %llu), parameter calls equal %llu differ %llu, "
                   "count differs %llu | parameters decoded otherwise than R3-R6's model %llu\n",
                (unsigned long long)s.vpd_programs.load(), (unsigned long long)s.vpd_sets.load(),
                (unsigned long long)s.vpd_matrices.load(), (unsigned long long)s.vpdv_programs.load(),
                (unsigned long long)s.vpdv_game.load(), (unsigned long long)s.vpdv_equal.load(),
                (unsigned long long)s.vpdv_differ.load(), (unsigned long long)s.vpdv_count_differ.load(),
                (unsigned long long)s.vp_new_cases.load());
    if (r6_producers())
        fprintf(f, "r6 producers (late fixes): clears done at a flip %llu | game draw-arrays records with every "
                   "texture taken at the walker %llu | PPFX colour clears of some channels only %llu | clears at a flip: depth/stencil only %llu, colour of the buffer shown %llu, colour of another %llu\n",
                (unsigned long long)s.r6_flip_clears.load(), (unsigned long long)s.r6_game_arrays_walked.load(),
                (unsigned long long)s.clears_masked.load(), (unsigned long long)s.fclr_depth.load(),
                (unsigned long long)s.fclr_shown.load(), (unsigned long long)s.fclr_other.load());
    if (r7()) {
        fprintf(f, "r7 (no walker): flushes %llu (forced by a full queue %llu) | command words found in the title's ring %llu "
                   "(at %llu flushes) | ring recycles %u, get answered %u times | markers skipped %llu, jumps %llu, texture cache invalidates %llu | frame-start "
                   "reset calls %llu (not the reset value %llu) | report location calls %llu, report requests %llu\n",
                (unsigned long long)s.r7_flushes.load(), (unsigned long long)s.r7_forced.load(),
                (unsigned long long)s.r7_words.load(), (unsigned long long)s.r7_word_flushes.load(),
                cellGcm_native_fe_recycles(), cellGcm_native_fe_get_polls(), (unsigned long long)s.r7_markers.load(),
                (unsigned long long)s.r7_jumps.load(), (unsigned long long)s.r7_invalidates.load(), (unsigned long long)s.r7_reset_calls.load(),
                (unsigned long long)s.r7_reset_nondefault.load(), (unsigned long long)s.r7_reportloc.load(),
                (unsigned long long)s.r7_reports.load());
        fprintf(f, "r7 frame-start reset, calls not setting the reset value (the last value): blend equation %llu (%08X), "
                   "MRT colour mask %llu (%X), front face %llu (%X), z min/max control %llu (%X), MRT blend %llu (%X), depth "
                   "bounds %llu, two-sided stencil %llu, point sprite %llu\n",
                (unsigned long long)s_r7_reset_by[0], s_r7_reset_last[0], (unsigned long long)s_r7_reset_by[1], s_r7_reset_last[1],
                (unsigned long long)s_r7_reset_by[2], s_r7_reset_last[2], (unsigned long long)s_r7_reset_by[3], s_r7_reset_last[3],
                (unsigned long long)s_r7_reset_by[4], s_r7_reset_last[4], (unsigned long long)s_r7_reset_by[5],
                (unsigned long long)s_r7_reset_by[6], (unsigned long long)s_r7_reset_by[7]);
        fprintf(f, "r7 textures taken at the flush %llu (%.1f MB)\n", (unsigned long long)s.r7_flush_tex.load(),
                s.r7_flush_tex_bytes.load() / 1048576.0);
        fprintf(f, "r7 asp fragment heap set-ups by asp's own allocation (the upload replaced) %llu | line width calls %llu, "
                   "line draws wider than 1 (drawn 1 wide: not modelled) %llu\n",
                (unsigned long long)s.r7_heap_setups.load(), (unsigned long long)s.r7_line_width_calls.load(),
                (unsigned long long)s.r7_wide_line_draws.load());
        fprintf(f, "r7 synchronisation: labels %llu (written %llu) | references %llu (written %llu) | label waits %llu (spun %llu, "
                   "%.1f ms, timed out %llu) | flips executed natively %llu | legacy occlusion: reports %llu, copies dropped %llu | "
                   "PPFX draws whose inputs were taken at consumption %llu (%.1f MB) | draws with state the native renderer does "
                   "not model %llu | inventory: transfer calls %llu, watched calls %llu\n",
                (unsigned long long)s.r7_labels.load(), (unsigned long long)s.r7_labels_done.load(),
                (unsigned long long)s.r7_refs.load(), (unsigned long long)s.r7_refs_done.load(),
                (unsigned long long)s.r7_waits.load(), (unsigned long long)s.r7_wait_spins.load(), s.r7_wait_us.load() / 1e3,
                (unsigned long long)s.r7_wait_timeouts.load(), (unsigned long long)s.r7_flip_executed.load(),
                (unsigned long long)s.r7_legacy_reports.load(), (unsigned long long)s.r7_legacy_transfers_dropped.load(),
                (unsigned long long)s.r7_deferred.load(), s.r7_deferred_bytes.load() / 1048576.0,
                (unsigned long long)s.r7_unmodelled_draws.load(), (unsigned long long)s.r7_xfer_calls.load(),
                (unsigned long long)s.r7_unknown_calls.load());
        if (s.cc_records)
            fprintf(f, "r7 capture check: records %llu, input ranges %llu (%.1f MB hashed twice) | written between the flush and "
                       "the capture: records %llu, ranges %llu | rewritten after the flush but taken at it (not used) %llu\n",
                    (unsigned long long)s.cc_records.load(),
                    (unsigned long long)s.cc_ranges.load(), s.cc_bytes.load() / 2.0 / 1048576.0,
                    (unsigned long long)s.cc_changed_records.load(), (unsigned long long)s.cc_changed_ranges.load(),
                    (unsigned long long)s.cc_flush_taken_changed.load());
        std::lock_guard<std::mutex> g(R7S.lock);
        if (!R7S.words.empty()) {
            fprintf(f, "r7 command words by (subchannel:method):");
            for (auto& kv : R7S.words) fprintf(f, " %X:%04X %llu", kv.first >> 16, kv.first & 0xFFFFu, (unsigned long long)kv.second);
            fprintf(f, "\n");
        }
        for (auto& kv : R7S.calls)
            fprintf(f, "r7 watched call %08X from %08X: %llu\n", (uint32_t)(kv.first >> 32), (uint32_t)kv.first, (unsigned long long)kv.second);
    }
    if (r6_present())
        fprintf(f, "r6 present: flips recorded %llu presented %llu (no native image %llu) | present_ms %.1f\n",
                (unsigned long long)queue().flips_recorded.load(), (unsigned long long)s.r6_presents.load(),
                (unsigned long long)s.r6_present_missing.load(), s.r6_present_ns.load() / 1e6);
    if (s.asp_race_checked)
        fprintf(f, "asp race check: %llu draws checked, %llu whose vertex or index bytes changed between the record and the native draw\n",
                (unsigned long long)s.asp_race_checked, (unsigned long long)s.asp_race_changed);
    if (s.n_asp_draws)
        fprintf(f, "native asp draws %llu clears %llu vertex_mb %.1f | clears merged %llu dropped %llu | resident fp binds %llu\n",
                (unsigned long long)s.n_asp_draws, (unsigned long long)s.n_asp_clears, s.n_asp_vertex_bytes / 1048576.0,
                (unsigned long long)A.clears_merged, (unsigned long long)A.clears_dropped, (unsigned long long)A.fp_resident_binds);
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
    fprintf(f, "compare_queue waits %llu wait_ms %.1f | readback buffer waits %llu wait_ms %.1f skipped_for_none %llu "
               "| handoff no_slot %llu copy_failed %llu slot_waits %llu slot_wait_ms %.1f\n",
            (unsigned long long)s.cmp_queue_waits, s.cmp_queue_wait_ns / 1e6,
            (unsigned long long)s.cmp_slot_waits, s.cmp_slot_wait_ns / 1e6, (unsigned long long)s.cmp_slot_skips,
            (unsigned long long)s.hand_no_slot, (unsigned long long)s.hand_copy_fail,
            (unsigned long long)s.hand_slot_waits, s.hand_slot_wait_ns / 1e6);
    {
        std::lock_guard<std::mutex> g(s_unm_lock);
        for (auto& kv : s_unmodelled_by) fprintf(f, "unmodelled %08X %llu\n", kv.first, (unsigned long long)kv.second);
        int n = 0;
        for (auto& kv : s_targets_seen)
            if (n++ < 64)
                fprintf(f, "target %u:%08X %ux%u format %02X records %llu last_frame %u\n", (uint32_t)(kv.first >> 32),
                        (uint32_t)kv.first, kv.second.w, kv.second.h, kv.second.format,
                        (unsigned long long)kv.second.records, kv.second.last_frame);
    }
    {
        std::vector<std::string> xs;
#ifndef DOTHEROES_NATIVE_RELEASE
        bridge_report(xs);
#endif
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
    atexit(r7_lost_summary);
    // runs first (atexit is last in, first out): the native renderer finishes
    // what was walked and the comparisons in flight, so the report counts
    // what is left as unfinished
    if (mode() == 2) atexit([] { render_drain(15000); report_add("report: final, after the native renderer's drain"); });
    // a test's stop ends the process without exit handlers: the same there
    g_dotheroes_stop_hook = [] {
        if (mode() == 2) { render_drain(15000); report_add("report: final, after the native renderer's drain (test stop)"); }
        r7_lost_summary();
        write_report();
    };
    std::thread([] {
#ifdef _WIN32
        SetThreadDescription(GetCurrentThread(), L"ppfx report");
#endif
        for (;;) { std::this_thread::sleep_for(std::chrono::seconds(10)); write_report(); }
    }).detach();
#ifndef DOTHEROES_NATIVE_RELEASE
    if (!r7()) bridge_install();   // R7: no walker, no bridge
    else
#endif
    cellGcm_set_guest_read_hook(guest_read_native);   // the snapshot's read of a display buffer
    if (mode() == 2) render_start();
}

}  // namespace ppfx

using namespace ppfx;

// R7's switches: DOTHEROES_R7=1 is the whole native configuration (R4-R6's
// switches; each can still be set by hand), with the host's front end in
// cellGcmSys set before the title's first call. Called from
// r1_observe.cpp's static initialiser, before anything reads them.
static void r7_defaults()
{
#ifndef DOTHEROES_NATIVE_RELEASE
    const char* e = getenv("DOTHEROES_R7");
    if (!(e && *e == '1')) return;
#endif
    static const char* const kv[][2] = {
        { "DOTHEROES_PPFX_NATIVE", "1" }, { "DOTHEROES_PPFX_BYPASS", "1" }, { "DOTHEROES_PPFX_COMPARE", "0" },
        { "DOTHEROES_PPFX_XCHECK", "0" }, { "DOTHEROES_PPFX_HANDBACK_CHECK", "0" }, { "DOTHEROES_ASP_RECORD", "1" },
        { "DOTHEROES_ASP_NATIVE", "1" }, { "DOTHEROES_ASP_SHARE", "1" }, { "DOTHEROES_ASP_BYPASS", "1" },
        { "DOTHEROES_R6_PRODUCERS", "1" }, { "DOTHEROES_R6_PRESENT", "1" }, { "DOTHEROES_R6_SKIP", "all" } };
    for (auto& p : kv) if (!getenv(p[0])) _putenv_s(p[0], p[1]);
    // the occlusion queries' calls are always records here (nothing else
    // would write their reports); DOTHEROES_R7_OCCLUSION=legacy writes them
    // as the previous port did (the render report says which)
    _putenv_s("DOTHEROES_R6_OCCLUSION", "1");
    cellGcm_set_native_frontend(1);
}

extern "C" int ppfx_rec_wanted(void)
{
    r7_defaults();
    const char* pw = getenv("DOTHEROES_PPFX_PROGWATCH");
    if (pw && *pw) return 1;
    return ppfx::mode() != 0;
}

// Every observed call, from r1_observe.cpp's ppu_entry_observe: 0 the body
// runs, 1 it does not (R4's bypass), 2 it runs and ppfx_rec_exit sees its return.
namespace ppfx { namespace {
// R6 (DOTHEROES_R6_CALL_COUNTS=1, development): every observed call counted
// by address -- which entry points the game thread's observing pays for
int s_cc_on = -1;
struct CallCount { uint32_t addr; uint64_t n; };
CallCount s_cc[1024];
void call_count(uint32_t addr)
{
    uint32_t h = (addr >> 2) * 2654435761u >> 22;
    for (uint32_t k = 0; k < 1024; k++, h = (h + 1) & 1023) {
        if (s_cc[h].addr == addr) { s_cc[h].n++; return; }
        if (!s_cc[h].addr) { s_cc[h].addr = addr; s_cc[h].n = 1; return; }
    }
}
} }

extern "C" int ppfx_rec_observe(uint32_t addr, ppu_context* ctx)
{
    static int started = 0;
    if (!started) { started = 1; ppfx::start(); }
    if (ppfx::s_cc_on) {
        if (ppfx::s_cc_on < 0) { const char* e = getenv("DOTHEROES_R6_CALL_COUNTS"); ppfx::s_cc_on = e && *e == '1'; }
        if (ppfx::s_cc_on) ppfx::call_count(addr);
    }
    if (addr == 0x00963EBCu) {   // the game's memcpy(dst, src, n): only copies into local memory matter
        const uint32_t dst = (uint32_t)ctx->gpr[3], src = (uint32_t)ctx->gpr[4], n = (uint32_t)ctx->gpr[5];
        if (dst >= kLocalEa && dst - kLocalEa < 0x10000000u && n && n <= (64u << 20) && ea_ok(src, n)) {
            std::lock_guard<std::mutex> g(s_lock);
            shadow_cpu_write(dst - kLocalEa, vm_base + src, n);
            stats().local_copies++;
        }
        // a diagnostic: the game's copies into RSX local memory below the
        // first PPFX intermediate (the display buffers among them) and from
        // there, by caller -- what a snapshot does with the frame (R4: a
        // snapshot reads the engine's display surface instead,
        // cellJpgEncEncodePicture; docs/NATIVE_R4.md)
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
        return 0;
    }
    // R6: the recorder's own time sampled (one call in 16, scaled) and the hot
    // counters bumped without a locked add (the producer is their one writer)
    static uint32_t tick = 0;
    const bool timed = (++tick & 15) == 0;
    const uint64_t t0 = timed ? now_ns() : 0;
    Stats& st = stats();
    bump(st.calls);
    const uint32_t tid =
#ifdef _WIN32
        GetCurrentThreadId();
#else
        0;
#endif
    std::unique_lock<std::mutex> g(s_lock);
    struct Held { Held(std::unique_lock<std::mutex>* h) { s_held = h; } ~Held() { s_held = nullptr; } } held(&g);
    if (addr == 0x0069DD60u) {   // cellGcmSetFlip: a frame
        if (tid == S.tid || !S.tid) {
            S.frame++; st.frames++;
            // R4: whether the next frame's PPFX calls are bypassed -- once the
            // native renderer can take them; alternate blocks with =alt:<n>
            const int bm = bypass_mode();
            S.r4_frame = (bm == 1 || (bm == 3 && (S.frame / s_alt_n) % 2 == 1)) && (render_ready() || r7());
            if (S.r4_frame) st.r4_frames++;
        }
        if (s_pw && (S.frame & 255) == 0) fflush(s_pw);
        // R7 (memory over area cycles): what the recorder holds, every 1800
        // frames (DOTHEROES_R7_CENSUS=1, development)
        static int census = -1;
        if (census < 0) { const char* e = getenv("DOTHEROES_R7_CENSUS"); census = e && *e == '1'; }
        if (census && S.frame % 1800 == 0)
            fprintf(stderr, "[census] recorder frame %u shadow_pages %zu (%.1f MB) bound %zu blobs %zu fp_bytes %zu "
                            "fp_last_obj %zu fp_slot_epoch %zu targets_seen %zu pw_seen %zu ppfx_targets %zu asp_targets %zu\n",
                    S.frame, s_shadow.size(), s_shadow.size() * (double)sizeof(ShadowPage) / 1048576.0, s_bound.size(),
                    s_blobs.size(), A.fp_bytes.size(), A.fp_last_obj.size(), A.fp_slot_epoch.size(), s_targets_seen.size(),
                    s_pw_seen.size(), S.ppfx_targets.size(), A.asp_targets.size());
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
        if (attribute(ctx)) {
            st.producer_threads++;
            fprintf(stderr, "[ppfx] FATAL: a PPFX call (0x%08X) on thread %u, the command stream's producer is "
                            "thread %u: the single-producer assumption does not hold\n", addr, tid, S.tid);
            fflush(stderr);
            abort();
        }
        st.other_thread_calls++;
        if (timed) bump(st.rec_ns, 16 * (now_ns() - t0));
        return 0;
    }
    // R6: the title's context from its first call on, not from PPFX's first
    if (!S.ring_ctx && r6_producers()) {
        const uint32_t t = cellGcm_title_context_pub();
        if (ea_ok(t, 16)) {
            S.ring_ctx = t;
            char line[120];
            snprintf(line, sizeof line, "recorder: the title's context %08X taken at frame %u (R6)", t, S.frame);
            report_add(line);
        }
    }
    // R7: the remaining command writers, the flush, labels, reference, waits
    if (r7() && S.ring_ctx && (uint32_t)ctx->gpr[3] == S.ring_ctx) {
        const int a7 = r7_call(addr, ctx);
        if (a7 >= 0) { if (timed) bump(st.rec_ns, 16 * (now_ns() - t0)); return a7; }
    }
    // Other code's calls on the producer's thread set state too, at the same
    // entry points: PPFX's first frames after a load draw with some of it
    // (a texture address, the colour mask) -- the GPU's state as the calls
    // left it, in their order. Only PPFX's draws and clears become records.
    // asp's sampler binds that ran since the last call: their units' address
    // modes are in asp's state store now
    asp_resolve();
    if (asp_fn(addr)) {
        const int a = mode() || s_pw ? asp_call(addr, ctx) : 0;
        if (timed) bump(st.rec_ns, 16 * (now_ns() - t0));
        return a;
    }
    const int who = attribute(ctx);   // R7: computed here, past the calls that do not need it
    if (who) { st.ppfx_calls++; if (who == 2) st.nested_ppfx++; }
    else st.foreign_state_calls++;
    S.by = who ? kByPpfx : kByGame;
    if (who && !S.ring_ctx) S.ring_ctx = (uint32_t)ctx->gpr[3];
    // R4: what this call's body does (r4_decide); a bypassed call's record is too
    S.skipping = false;
    // R6: the occlusion reports' calls, whoever makes them, are records (their
    // bodies do not run); the zpass counting and render enable are state
    const bool occ = occ_fn(addr) && r6_occlusion() && (uint32_t)ctx->gpr[3] == S.ring_ctx;
    if (occ) {
        if (addr == 0x006A820Cu) build_report(ctx, kRepClear);
        else if (addr == 0x006A829Cu) build_report(ctx, kRepReport);
        else if (addr == 0x006A1D1Cu) build_report(ctx, kRepTransfer);
        else if (addr == 0x006A8020u && (uint32_t)ctx->gpr[4] == 2) {   // CELL_GCM_CONDITIONAL: never seen
            st.occ_cond_render++;
            static int said = 0;
            if (!said++) report_add("r6: cellGcmSetRenderEnable in the conditional mode -- not carried out natively");
        }
    }
    int act = occ ? 1 : 0;
    bool decided = occ;
    // R6 (=all): every libgcm call on the title's context whose body only
    // writes command words -- PPFX's kept calls, the game's own state calls
    // (its mesh path's vertex arrays, Bink's, the frame start's reset) -- does
    // not run either: no engine draw is left to read that state (R4's verify:
    // these bodies store nothing but their words). The flip, the flush, the
    // helpers and calls with no command keep running (class run); the draws
    // and clears are records (below).
    if (!occ && r6_skip() == 3 && (uint32_t)ctx->gpr[3] == S.ring_ctx && addr != 0x006A1E28u && addr != 0x006A2164u &&
        addr != 0x006A5CF4u && !occ_fn(addr) && !r6_keep(addr)) {
        const char* why;
        const Cls k = r4_class(addr, ctx, &why);
        if (k == kClsBypass && who && S.r4_frame && !S.kept_depth) { act = r4_decide(addr, ctx); decided = true; }
        else if (k != kClsRun && !(who && S.kept_depth)) { act = 1; decided = true; r6_libgcm_skipped(addr, who); }
    }
    if (!decided) act = who && bypass_mode() ? r4_decide(addr, ctx) : 0;
    // R6: the vertex program verify for the game's own cellGcmSetVertexProgram
    // calls too (Bink's, the mesh path's): their exit observed
    if (!decided && !who && act == 0 && addr == 0x006A4E50u && vp_verify() && (uint32_t)ctx->gpr[3] == S.ring_ctx) {
        VEntry v; memset(&v, 0, sizeof v); v.addr = addr; v.kind = 0;
        s_vstack.push_back(v);
        act = 2;
        stats().vpdv_game++;
    }
    if (act == 3) { r4_verify_enter(addr, ctx); act = 2; }
    // DOTHEROES_PPFX_TRACE_LOAD=1 (a diagnostic, with RSX_FRAME_TRACE=arm:..):
    // the frame trace starts at the walker's next flip after PPFX's first call
    // following 30 frames without one -- an area's first frames, state-triggered
    if (who) {
        static int tl = -1;
        static uint32_t last_ppfx_frame = 0;
        if (tl < 0) { const char* e = getenv("DOTHEROES_PPFX_TRACE_LOAD"); tl = e && *e == '1'; }
        if (tl == 1 && S.frame >= last_ppfx_frame + 30) {
#ifndef DOTHEROES_NATIVE_RELEASE
            rsx_frame_trace_arm();
#endif
            tl = 2;
            char line[160];
            snprintf(line, sizeof line, "recorder: frame trace armed at frame %u (the first PPFX call after %u frames "
                     "without one), record %llu next", S.frame, S.frame - last_ppfx_frame,
                     (unsigned long long)queue().put.load());
            report_add(line);
        }
        last_ppfx_frame = S.frame;
    }
    if (!mode() && !s_pw) return act;
    switch (addr) {
    case 0x0069F944: enc_surface((uint32_t)ctx->gpr[4]); break;
    case 0x0069E7EC: enc_viewport(ctx); break;
    case 0x0069E99C: enc_fp_bind(ctx, S.frame); break;
    case 0x0069E6E0: enc_shader_control(ctx); break;
    case 0x006A4E50:
        enc_vp(ctx);
        if (act == 1) vp_defaults(ctx);   // R6 (=all): the body's default constants, the body skipped
        else if (act == 2 && vp_verify()) vp_verify_enter(ctx);
        break;
    case 0x006A39EC:
        if (vp_verify() && (uint32_t)ctx->lr == 0x006A529Cu) vp_verify_nested(ctx);
        enc_vp_const(ctx);
        break;
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
    case 0x006A5C30: {  // cellGcmSetBlendFunc (ctx, sfcolor, dfcolor, sfalpha, dfalpha): R6 (Bink's blend)
        Raster& r = S.st.raster;
        r.blend_sf_rgb = (uint16_t)ctx->gpr[4]; r.blend_df_rgb = (uint16_t)ctx->gpr[5];
        r.blend_sf_a = (uint16_t)ctx->gpr[6]; r.blend_df_a = (uint16_t)ctx->gpr[7];
        break;
    }
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
    case 0x006A1E28:
        if (mode() && who) build_draw(ctx);
        else if (!who && r6_producers() && (uint32_t)ctx->gpr[3] == S.ring_ctx) {
            // R6: the game's own cellGcmSetDrawArrays(ctx, mode, first, count)
            // -- Bink's frames, the mesh path's -- a record like asp's
            const bool skip = asp_bypass() >= 1;
            game_arrays_record(ctx, skip);
            if (skip) act = 1;
        }
        break;
    case 0x006A2164:   // cellGcmSetDrawIndexArray by the game (inside asp's scene): R5's records, at its return
        if (!who && asp_records_on() && S.ring_ctx && (uint32_t)ctx->gpr[3] == S.ring_ctx) {
            if (r6_skip() == 1 || r6_skip() == 3) { game_draw_skip(ctx); act = 1; }   // R6: at its entry, the body not run
            else { game_draw_enter(ctx); act = 2; }
        }
        break;
    case 0x006A5CF4:   // a clear: a record when it clears colour (depth and stencil are not PPFX's output)
        if (!who) {
            // R6: the game's own clear (its frame start's: the display buffer
            // and its depth) -- done before the next record, as asp's are
            if (r6_producers() && (uint32_t)ctx->gpr[3] == S.ring_ctx) {
                game_clear((uint32_t)ctx->gpr[4]);
                if (asp_bypass() >= 1) { act = 1; st.r6_game_skipped++; }
            }
            break;
        }
        if (mode() && ((uint32_t)ctx->gpr[4] & 0xF0u)) {
            // the native clear clears every channel of the first target (as the
            // engine's reference did): a mask of some channels only is counted
            // and named, so the assumption is checked, not silent
            const uint32_t cm = (uint32_t)ctx->gpr[4] & 0xF0u;
            if (cm != 0xF0u) {
                if (!st.clears_masked++) {
                    char line[140];
                    snprintf(line, sizeof line, "recorder: PPFX colour clear of some channels only (mask %02X) at frame %u "
                             "-- cleared whole natively", (uint32_t)ctx->gpr[4] & 0xFFu, S.frame);
                    report_add(line);
                }
            }
            build_clear(ctx);
        }
        else st.depth_clears++;
        // R6: its depth and stencil part done natively too (R4 kept it for the
        // engine's depth buffer; the native depth images asp's scene uses
        // never saw it), before the next record
        if (r6_producers() && ((uint32_t)ctx->gpr[4] & 0x03u) && (uint32_t)ctx->gpr[3] == S.ring_ctx) {
            game_clear((uint32_t)ctx->gpr[4] & 0x03u);
            if (r6_skip() == 3 && act != 1 && !r6_keep(addr)) {   // kept by R4: with =all its body does not run
                if (act == 2 && !s_vstack.empty() && s_vstack.back().kind == 0) { s_vstack.pop_back(); if (S.kept_depth) S.kept_depth--; }
                act = 1; r6_libgcm_skipped(addr, who);
            }
        }
        break;
    case 0x0069DD60:   // cellGcmSetFlip: R6's native present (R7: from the first, the renderer ready or not)
        if (r6_present() && (render_ready() || r7()) && (uint32_t)ctx->gpr[3] == S.ring_ctx) build_flip(ctx);
        // R7: the RSX puts the depth/stencil clear value back to its reset at a
        // flip (the engine's gcm_fifo_flip_resets_state did it in the walker)
        if (r7() && (uint32_t)ctx->gpr[3] == S.ring_ctx) S.st.clear_depth = 0xFFFFFF00u;
        break;
    case 0x0069FC38: break;
    default: {
        const bool done = enc_simple(addr, (uint32_t)ctx->gpr[4]);
        if (!done && !no_state(addr) && who) { st.unmodelled++; std::lock_guard<std::mutex> u(s_unm_lock); s_unmodelled_by[addr]++; }
        break;
    }
    }
    S.skipping = false;
    if (timed) bump(st.rec_ns, 16 * (now_ns() - t0));
    return act;
}

// The return of a call ppfx_rec_observe answered 2 (the producer's thread):
// a kept call's end, or a verified call's results checked.
extern "C" void ppfx_rec_exit(uint32_t addr, ppu_context* ctx)
{
    if (s_vstack.empty()) { stats().r4_exit_mismatch++; return; }
    const VEntry v = s_vstack.back();
    s_vstack.pop_back();
    if (v.addr != addr) stats().r4_exit_mismatch++;
    if (v.kind == 0) { if (v.addr == 0x006A4E50u) vp_verify_exit(); if (S.kept_depth) S.kept_depth--; return; }
    if (v.kind == 2 && v.addr == 0x006C6F88u) skv_exit(ctx);   // R6's verify: before the take-back
    if (v.kind == 2) {   // R6: the record built at a draw's return, timed (sampled) apart from the entries
        static uint32_t tick = 0;
        const bool timed = (++tick & 15) == 0;
        const uint64_t t0 = timed ? now_ns() : 0;
        asp_draw_exit(v);
        if (timed) bump(stats().rec_exit_ns, 16 * (now_ns() - t0));
        return;
    }
    if (v.kind == 3) { asp_probe_exit(v); return; }
    if (v.kind == 4) { asp_state_bypass_exit(v); return; }
    if (v.kind == 5) { prog_verify_exit(v, ctx); return; }   // R6's verify of a program function
    if (v.kind == 6) { asp_state_verify_exit(v, ctx); return; }   // R6's verify of a state function
    r4_verify_exit(v, ctx);
    if (--s_verifying == 0) { g_ppu_obs_store = 0; s_stores.clear(); }
}

// A store a verified body makes (ps3recomp 0112's PPU_OBS_ST).
extern "C" void ppfx_rec_store(uint64_t ea, uint32_t bytes)
{
    if (!s_verifying) return;
#ifdef _WIN32
    if (GetCurrentThreadId() != S.tid) return;
#endif
    if (s_stores.size() < 4096) s_stores.emplace_back(ea, bytes);
}

namespace ppfx { namespace {
void write_call_counts(FILE* f)
{
    if (s_cc_on <= 0) return;
    std::vector<CallCount> v;
    for (const CallCount& c : s_cc) if (c.addr) v.push_back(c);
    std::sort(v.begin(), v.end(), [](const CallCount& a, const CallCount& b) { return a.n > b.n; });
    const double fl = stats().frames ? (double)stats().frames : 1.0;
    fprintf(f, "r6 observed calls by address (a flip):");
    for (size_t i = 0; i < v.size() && i < 40; i++) fprintf(f, " %08X %.0f", v[i].addr, v[i].n / fl);
    fprintf(f, "\n");
}
} }
