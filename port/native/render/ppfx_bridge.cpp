// R3's bridge between PPFX's native records and the FIFO path
// (docs/NATIVE_R3.md), on the FIFO walker's thread: the draw engine calls it
// at each draw it is about to record and after each draw and clear
// (rsx_engine_bridge, ps3recomp 0108). Temporary by design: it exists only
// while the existing renderer still draws everything else, and goes when the
// remaining graphics producers are native.
//
// Matching. Every record names where its commands go (ppfx_recorder.cpp): the
// title's ring lap and the IO offset of its first command, or lap + 1 at the
// ring's start when its own reserve wrapped. The walker knows its own lap
// (back jumps taken) and position, and takes a draw at its END, 0x20 bytes
// past the draw's first command (R1), a clear at its method (measured). The
// head record either matches the event the walker is at, lies ahead of it
// (the event is other work's), or lies behind it (the engine never drew it:
// counted as missed). PPFX's records are a subsequence of the walker's events
// in the same order, so comparing with the head alone decides. The walker's
// lap count and the title's differ by a constant when every back jump is a
// wrap the runtime wrote; the first match fixes it and every later match
// checks it.
//
// Cross-check (development, DOTHEROES_PPFX_XCHECK, on unless 0): at each
// matched draw, the record's state against the engine's -- registers,
// vertex-program constants and words, the fragment program's bytes in local
// memory now (after every queued patch landed), and the vertex and static
// texture bytes the engine is about to read. It only counts and reports; the
// records never take anything from the engine.

#include "ppfx_native.h"

#include <chrono>
#include <thread>
#include <map>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>

#include "rsx_dispatch.h"
#include "rsx_vulkan_bridge.h"
#include "rsx_draw_engine.h"
#include "rsx_fp_decompiler.h"
#include "rsx_vp_decompiler.h"

extern "C" uint8_t* vm_base;
extern "C" uint32_t cellGcmResolveLocated(int local, uint32_t offset);

namespace ppfx {

namespace {

const uint32_t kDrawDelta = 0x20, kClearDelta = 0;

uint64_t bnow()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct BState {
    uint64_t w;                 // the head record (== queue().walked)
    int have_delta;
    int64_t delta;              // walker laps - title laps
    int xcheck;
    std::unordered_map<uint64_t, uint64_t> dirty;   // (location << 32 | offset) -> foreign writes since
    // cross-check aggregates: per method, how many draws differed, last values
    struct XR { uint64_t n; uint32_t rec, eng; uint64_t seq0, seq1; uint32_t frame0, frame1; };
    const Rec* cur;              // the record being checked
    Rec* pending;                // a matched draw, finished after the engine's draw
    uint32_t copies;             // copies recorded for it so far
    long long last_ord = -1;     // the last signal this bridge attached ...
    int16_t last_slot = -1;      // ... and its semaphore slot
    std::map<std::string, XR> xr;
    std::mutex xr_lock;          // the report reads it
    uint64_t logged_miss;
} B;

uint64_t K(int64_t lap, uint32_t io) { return (uint64_t)lap << 32 | io; }

uint32_t delta_of(uint32_t kind) { return kind == kDraw ? kDrawDelta : kClearDelta; }

void mark_walked(Rec* r)
{
    Queue& q = queue();
    (void)r;
    B.w++;
    q.walked.store(B.w, std::memory_order_release);
    if (mode() == 1) q.done.store(B.w, std::memory_order_release);
    q.cv.notify_all();
}

void foreign(const uint32_t* locations, const uint32_t* offsets, uint32_t n, int draw)
{
    for (uint32_t i = 0; i < n; i++) B.dirty[(uint64_t)locations[i] << 32 | offsets[i]]++;
    if (draw) stats().foreign_draws++; else stats().foreign_clears++;
}

// The constants a vertex program reads (D1 const_src, for each source whose
// register type is a constant; a relative read may reach any of them).
void vp_const_mask(const Blob* vp, uint32_t mask[16])
{
    memset(mask, 0, 16 * 4);
    if (!vp) return;
    const uint32_t* w = (const uint32_t*)vp->bytes.data();
    const size_t words = vp->bytes.size() / 4;
    for (size_t i = 0; i + 4 <= words; i += 4) {
        const uint32_t d1 = w[i + 1], d2 = w[i + 2], d3 = w[i + 3];
        const uint32_t src[3] = { ((d2 >> 23) & 0x1FF) | ((d1 & 0xFF) << 9), (d2 >> 6) & 0x1FFFF,
                                  ((d3 >> 21) & 0x7FF) | ((d2 & 0x3F) << 11) };
        const uint32_t c = (d1 >> 12) & 0x1FF;
        for (int k = 0; k < 3; k++)
            if ((src[k] & 3) == 3) {
                if (d3 >> 1 & 1) { for (int j = 0; j < 16; j++) mask[j] = ~0u; return; }
                mask[c >> 5] |= 1u << (c & 31);
            }
    }
}

// One state item the record and the engine disagree on.
void xdiff(const char* what, uint32_t rec, uint32_t eng, uint32_t& n)
{
    n++;
    auto& x = B.xr[std::string(what)];
    if (!x.n) { x.seq0 = B.cur->seq; x.frame0 = B.cur->frame; }
    x.n++; x.rec = rec; x.eng = eng; x.seq1 = B.cur->seq; x.frame1 = B.cur->frame;
}

void xreg(const rsx_dispatch* E, const rsx_dispatch* R, uint32_t m, uint32_t& n)
{
    const uint32_t e = E->regs[m >> 2], v = R->regs[m >> 2];
    if (e == v) return;
    char name[48];
    snprintf(name, sizeof name, "reg %04X%s", m, reg_written(m) ? "" : " (never PPFX's)");
    xdiff(name, v, e, n);
}

rsx_dispatch* scratch()
{
    static rsx_dispatch* d = nullptr;
    if (!d) { d = (rsx_dispatch*)malloc(sizeof(rsx_dispatch)); rsx_dispatch_init(d, nullptr); }
    return d;
}

// The effective state of the matched draw, record against engine: what the
// engine's derivation reads for this draw -- the surface, viewport and
// scissor, the programs' registers, the units the fragment program samples,
// the arrays the vertex program reads, the render state (the fields that
// matter with the enables as they are), the constants the vertex program
// reads, the programs' bytes, and the vertex bytes.
void crosscheck(Rec* r)
{
    const rsx_dispatch* E = rsx_draw_engine_state();
    if (!E) return;
    Stats& st = stats();
    st.xc_draws++;
    rsx_dispatch* R = scratch();
    const uint32_t nr = reg_count();
    for (uint32_t i = 0; i < nr; i++) R->regs[reg_method(i) >> 2] = r->regs[i];
    std::lock_guard<std::mutex> g(B.xr_lock);
    B.cur = r;
    uint32_t dr = 0, dc = 0;
    static const uint16_t kAlways[] = {
        0x0194, 0x0208, 0x020C, 0x0210, 0x0200, 0x0204, 0x0220, 0x02B8, 0x1D88,          // surface
        0x0A00, 0x0A04, 0x0394, 0x0398, 0x0A20, 0x0A24, 0x0A28, 0x0A2C, 0x0A30, 0x0A34,   // viewport
        0x0A38, 0x0A3C, 0x08C0, 0x08C4,                                                    // and scissor
        0x08E4, 0x1D60, 0x1FF4, 0x1FF0, 0x1EA0, 0x1FEC, 0x1478, 0x1D7C,                    // programs
        0x1738, 0x173C, 0x1FC0, 0x0A68 };
    // (texture coordinate control, 0x0B40-0x0B64, is not read by the translators or the engine)
    for (uint16_t m : kAlways) xreg(E, R, m, dr);
    for (uint32_t u = 0; u < 4; u++) xreg(E, R, 0x090C + 0x20 * u, dr);   // vertex texture control0 (enable)
    // the units the fragment program samples
    const uint32_t units = r->fp ? rsx_fp_texture_units(r->fp->bytes.data(), (uint32_t)r->fp->bytes.size()) : 0;
    for (uint32_t u = 0; u < 16; u++) {
        if (!(units >> u & 1)) continue;
        for (uint32_t k = 0; k < 8; k++) xreg(E, R, 0x1A00 + 0x20 * u + 4 * k, dr);
        xreg(E, R, 0x1840 + 4 * u, dr);
    }
    // the arrays the vertex program reads
    rsx_vp_input_analysis an = { 0xFFFFu, 0 };
    if (r->vp) rsx_vp_analyze_inputs(r->vp->bytes.data(), (uint32_t)r->vp->bytes.size(), &an);
    for (uint32_t i = 0; i < 16; i++) {
        if (!(an.input_mask >> i & 1)) continue;
        xreg(E, R, 0x1740 + 4 * i, dr);
        if (R->regs[(0x1740 >> 2) + i] >> 4 & 0xF) xreg(E, R, 0x1680 + 4 * i, dr);
        else for (uint32_t k = 0; k < 4; k++) xreg(E, R, 0x1C00 + 16 * i + 4 * k, dr);
    }
    if (R->regs[0x0A68 >> 2] & 1) { xreg(E, R, 0x0A78, dr); xreg(E, R, 0x0A7C, dr); }
    // the render state, with what the enables make irrelevant left out
    rsx_be_render_state a, b;
    rsx_draw_engine_decode_render_state(R, &a);
    rsx_draw_engine_decode_render_state(E, &b);
#define RS(f) if (a.f != b.f) xdiff("rs " #f, a.f, b.f, dr)
    RS(alpha_test_enable); if (a.alpha_test_enable) { RS(alpha_func); RS(alpha_ref_raw); }
    RS(blend_enable); RS(blend_mrt);
    if (a.blend_enable || a.blend_mrt) { RS(sf_rgb); RS(df_rgb); RS(sf_a); RS(df_a); RS(eq_rgb); RS(eq_a); }
    RS(depth_test); if (a.depth_test) { RS(depth_write); RS(depth_func); }
    RS(cull_enable); if (a.cull_enable) { RS(cull_face); RS(front_face); }
    RS(color_mask); RS(color_mask_mrt); RS(rt_fp16);
    RS(stencil_enable);
    if (a.stencil_enable) {
        RS(stencil_two_sided); RS(s_func); RS(s_func_mask); RS(s_write_mask); RS(s_fail); RS(s_zfail); RS(s_zpass);
        RS(bs_func); RS(bs_fail); RS(bs_zfail); RS(bs_zpass);
    }
    RS(depth_bounds_test);
#undef RS
    // the constants the vertex program reads
    uint32_t cm[16];
    vp_const_mask(r->vp.get(), cm);
    for (uint32_t k = 0; k < 512; k++)
        if ((cm[k >> 5] >> (k & 31) & 1) && memcmp(E->constants[k], r->vconst[k], 16)) {
            char name[48];
            snprintf(name, sizeof name, "const c%u%s", k,
                     r->vconst_written[k >> 5] >> (k & 31) & 1 ? "" : " (never PPFX's)");
            xdiff(name, r->vconst[k][0], E->constants[k][0], dc);
        }
    r->walk.diff_regs = dr; r->walk.diff_consts = dc;
    if (dr || dc) frame_flag_inherited(r->frame);
    if (dr) st.xc_regs++;
    if (dc) st.xc_consts++;
    // the vertex program the engine will run
    const uint32_t start = rsx_dsp_vp_start(E);
    if (!r->vp || start != r->vp_start || start >= 544 || r->vp->bytes.size() > (544 - start) * 16 ||
        memcmp(E->vp + start * 4, r->vp->bytes.data(), r->vp->bytes.size())) {
        r->walk.diff_vp = 1; st.xc_vp++;
    }
    // the fragment program as local memory has it now
    uint32_t loc = 0;
    const uint32_t off = rsx_dsp_fragment_program(E, &loc);
    if (!r->fp || off != r->fp_off) { r->walk.diff_fp = 1; st.xc_fp++; }
    else {
        const uint32_t ea = cellGcmResolveLocated(loc == RSX_LOCATION_LOCAL, off);
        if (!ea || memcmp(vm_base + ea, r->fp->bytes.data(), r->fp->bytes.size())) {
            r->walk.diff_fp = 1; st.xc_fp++;
            static int said = 0;
            if (ea && said++ < 8) {
                uint32_t words = 0, first = ~0u;
                for (size_t i = 0; i + 4 <= r->fp->bytes.size(); i += 4)
                    if (memcmp(vm_base + ea + i, &r->fp->bytes[i], 4)) { words++; if (first == ~0u) first = (uint32_t)i; }
                char line[256];
                snprintf(line, sizeof line, "xfp seq %llu off %08X bytes %zu differing words %u first at +%u",
                         (unsigned long long)r->seq, off, r->fp->bytes.size(), words, first);
                report_add(line);
            }
        }
    }
    // vertex and static-texture bytes as the engine reads them now
    for (uint32_t k = 0; k < r->nvr; k++) {
        const VtxRange& v = r->vr[k];
        const uint32_t ea = cellGcmResolveLocated(v.location == 0, v.offset);
        if (!ea || memcmp(vm_base + ea, &r->vdata[v.at], v.bytes)) { r->walk.diff_vtx = 1; st.xc_vtx++; break; }
    }
    for (uint32_t k = 0; k < r->ntex; k++) {
        const VtxRange& t = r->tex[k];
        const Need* n = nullptr;
        for (uint32_t j = 0; j < r->nneed; j++) if (r->need[j].unit == t.location) n = &r->need[j];
        const uint32_t ea = n ? cellGcmResolveLocated(n->location == 0, n->offset) : 0;
        if (!ea || memcmp(vm_base + ea, &r->tdata[t.at], t.bytes)) { st.xc_tex++; break; }
    }
}

// The head record against the walker's event (kind, laps, io): 1 matched
// (the head is walked), 0 the event is other work's.
struct WEv { uint32_t kind, laps, io; };
WEv s_recent[8];
uint32_t s_nrecent;

int match(uint32_t kind, uint32_t laps, uint32_t io)
{
    Queue& q = queue();
    s_recent[s_nrecent++ % 8] = { kind, laps, io };
    for (;;) {
        const uint64_t put = q.put.load(std::memory_order_acquire);
        if (B.w >= put) return 0;
        Rec* r = q.at(B.w);
        const uint32_t d = delta_of(r->kind);
        if (!B.have_delta && B.logged_miss < 8) {   // until the first match: what was compared
            char line[200];
            snprintf(line, sizeof line, "bridge: unmatched walker event kind %u laps %u io %08X; head record %llu kind %u "
                     "lap %u io %08X alt %08X", kind, laps, io, (unsigned long long)r->seq, r->kind, r->lap, r->io,
                     r->io_alt);
            report_add(line);
            B.logged_miss++;
        }
        if (!B.have_delta) {
            // the first record: its position fixes the laps' difference
            if (r->kind == kind && io == r->io + d) { B.delta = (int64_t)laps - r->lap; B.have_delta = 1; }
            else if (r->kind == kind && io == r->io_alt + d) { B.delta = (int64_t)laps - (r->lap + 1); B.have_delta = 1; }
            else return 0;
            char line[160];
            snprintf(line, sizeof line, "bridge: first match at record %llu, walker laps - title laps = %lld",
                     (unsigned long long)r->seq, (long long)B.delta);
            report_add(line);
        }
        const uint64_t wk = K(laps, io);
        // k3: commands that end exactly at the ring's end, where the walker
        // has taken the jump back before it reports them (the next lap's
        // first byte: no command of that lap can end there)
        const uint64_t k1 = K(r->lap + B.delta, r->io + d), k2 = K(r->lap + 1 + B.delta, r->io_alt + d);
        const uint64_t k3 = K(r->lap + 1 + B.delta, r->io_alt);
        if (r->kind == kind && (wk == k1 || wk == k2 || (d && wk == k3))) {
            if (wk == k2) stats().wraps_in_call++;
            if (d && wk == k3 && wk != k2) stats().ends_at_ring_end++;
            stats().matched++;
            r->walk.matched = 1;
            return 1;
        }
        if (wk < k1) return 0;                       // ahead of the walker: this event is other work's
        // Behind the walker. Still the record's if its reserve wrapped and
        // the walker is between the two positions -- unless the next record
        // is the one here, which settles that the head was never drawn.
        bool missed = wk > k2;
        if (!missed && B.w + 1 < put) {
            Rec* n = q.at(B.w + 1);
            const uint32_t dn = delta_of(n->kind);
            missed = n->kind == kind && (wk == K(n->lap + B.delta, n->io + dn) ||
                                         wk == K(n->lap + 1 + B.delta, n->io_alt + dn));
        }
        if (!missed) return 0;
        stats().missed++;
        if (B.logged_miss++ < 20) {
            char line[400];
            int at = snprintf(line, sizeof line, "bridge: record %llu (kind %u, lap %u io %08X alt %08X) not reached; "
                              "the walker's last events:", (unsigned long long)r->seq, r->kind, r->lap, r->io, r->io_alt);
            for (uint32_t k = s_nrecent > 8 ? s_nrecent - 8 : 0; k < s_nrecent && at < (int)sizeof line - 32; k++) {
                const WEv& e = s_recent[k % 8];
                at += snprintf(line + at, sizeof line - at, " %u:%u/%08X", e.kind, e.laps, e.io);
            }
            report_add(line);
        }
        r->walk.matched = 0;
        mark_walked(r);
    }
}

// The engine's surface behind a record's need, copied into a slot when the
// native side does not already hold that content: other work wrote the
// address since PPFX last did (dirty), or no record has written it (the
// first use: the engine's surface as it was created, or other work's).
void handoffs(Rec* r)
{
    Stats& st = stats();
    for (uint32_t k = 0; k < r->nneed; k++) {
        const Need& n = r->need[k];
        const uint64_t key = (uint64_t)n.location << 32 | n.offset;
        u32 w = 0, h = 0, fmt = 0;
        const u32 handle = rsx_draw_engine_find_surface(n.location, n.offset, &w, &h, &fmt);
        if (!handle) continue;   // not a surface the engine has: a static texture, or nothing yet
        if (n.ppfx_wrote && !B.dirty.count(key)) continue;
        uint32_t bpp = 0;
        const uint32_t vf = be_vkformat_pub(fmt, &bpp);
        uint64_t img = 0;
        int slot = vf ? slot_image(vf, w, h, &img) : -1;
        if (slot < 0 && vf) {
            // Every slot is with records the native renderer has not done yet
            // (a slow consumer): wait for one -- a handoff is never dropped.
            // The engine's work is submitted first, so the renderer is not
            // left waiting for a signal only this thread would submit.
            const uint64_t t0 = bnow();
            rsx_vk_bridge_flush();
            st.hand_slot_waits++;
            while ((slot = slot_image(vf, w, h, &img)) < 0 && bnow() - t0 < 10000000000ull)
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            st.hand_slot_wait_ns += bnow() - t0;
        }
        if (slot < 0) { st.handoff_fail++; st.hand_no_slot++; continue; }
        if (rsx_vk_bridge_copy_image(handle, (VkImage)img, w, h)) {
            st.handoff_fail++; st.hand_copy_fail++;
            slot_release_image(slot);
            continue;
        }
        r->walk.hand[k] = (int16_t)slot;
        B.copies++;
        st.handoffs++;
        B.dirty.erase(key);
    }
}

// State-triggered comparison: a frame whose chain departs from the frame
// before -- another target or size, another program structure at the same
// place in the chain (an area's first frames, a dungeon's, programs reloaded
// elsewhere after the title) -- has every draw compared from there on, and so
// do the DOTHEROES_PPFX_COMPARE_AFTER_CHANGE (default 60) frames after it.
struct VState { uint32_t frame = ~0u, full_until = 0; std::vector<uint64_t> cur, prev; } V;

bool structure_changed(const Rec* r)
{
    static int after = -1;
    if (after < 0) { const char* e = getenv("DOTHEROES_PPFX_COMPARE_AFTER_CHANGE"); after = e && *e ? atoi(e) : 60; }
    if (r->frame != V.frame) { V.prev.swap(V.cur); V.cur.clear(); V.frame = r->frame; }
    const uint64_t prog = r->fp ? rsx_fp_structural_hash(r->fp->bytes.data(), (uint32_t)r->fp->bytes.size(), 0) : 0;
    const uint32_t place = r->need[0].format == 0x08 ? 0xD15Bu : r->need[0].offset;   // display buffers alternate
    const uint64_t key = fnv(&place, 4, fnv(&r->need[0].w, 8, prog));
    const size_t i = V.cur.size();
    V.cur.push_back(key);
    if (i >= V.prev.size() || V.prev[i] != key) {
        if (V.full_until < r->frame + (uint32_t)after) {
            if (V.full_until < r->frame) stats().cmp_triggers++;
            V.full_until = r->frame + (uint32_t)after;
        }
    }
    return r->frame <= V.full_until && V.frame != ~0u && after > 0;
}

// A draw about to be recorded by the engine.
struct BTime { uint64_t t0 = bnow(); ~BTime() { stats().bridge_ns += bnow() - t0; } };

void on_draw_pre(void*, u32 io, u32 laps, const u32* handles, const u32* locations, const u32* offsets, u32 n)
{
    (void)handles;
    BTime bt;
    if (!match(kDraw, laps, io)) { foreign(locations, offsets, n, 1); return; }
    Rec* r = queue().at(B.w);
    for (uint32_t k = 0; k < kMaxNeeds; k++) r->walk.hand[k] = -1;
    r->walk.ref = -1; r->walk.sem = -1; r->walk.ordinal = -1;
    B.copies = 0;
    if (B.xcheck) { const uint64_t x0 = bnow(); crosscheck(r); stats().xcheck_ns += bnow() - x0; }
    // DOTHEROES_PPFX_NO_HANDOFF=1 (a negative control): no handoffs, so the
    // native side lacks what other work drew and the comparison must see it
    static int nohand = -1;
    if (nohand < 0) { const char* e = getenv("DOTHEROES_PPFX_NO_HANDOFF"); nohand = e && *e == '1'; }
    if (mode() == 2 && render_ready() && !nohand) handoffs(r);
    // PPFX's own write: the native side will have these targets' content
    for (uint32_t i = 0; i < n; i++) B.dirty.erase((uint64_t)locations[i] << 32 | offsets[i]);
    B.pending = r;   // finished after the engine's draw (on_wrote)
}

// The copies recorded for a record go with the engine's next submission of
// the command buffer they are in; that submission signals one semaphore,
// shared by every record whose copies went into the same command buffer. The
// native side waits on it once, before the first record that needs it; the
// engine submits at its own points (its batches, the flip), not forced here.
void finish(Rec* r)
{
    if (B.copies) {
        int used = 0;
        long long ord = rsx_vk_bridge_signal(VK_NULL_HANDLE, &used);   // already one for this command buffer?
        int16_t slot = ord >= 0 && ord == B.last_ord ? B.last_slot : -1;
        if (ord == -1) {
            uint64_t sem = 0;
            int s = slot_semaphore(&sem);
            if (s < 0 && render_ready()) {   // as for an image slot: wait, the engine's work submitted first
                const uint64_t t0 = bnow();
                rsx_vk_bridge_flush();
                stats().hand_slot_waits++;
                while ((s = slot_semaphore(&sem)) < 0 && bnow() - t0 < 10000000000ull)
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                stats().hand_slot_wait_ns += bnow() - t0;
            }
            ord = s >= 0 ? rsx_vk_bridge_signal((VkSemaphore)sem, &used) : -2;
            if (ord >= 0 && used) { slot = (int16_t)s; B.last_ord = ord; B.last_slot = slot; }
            else if (s >= 0) slot_release_semaphore(s);
        }
        if (ord >= 0 && slot >= 0) {
            r->walk.sem = slot;
            r->walk.ordinal = ord;
        } else {
            // nothing would order the native side after the copies: they are
            // not used, and their slots are free again once the engine's
            // submission has run them (the next frame's, at the latest: the
            // slots are only released here, the copies are harmless)
            stats().handoff_fail += B.copies;
            for (uint32_t k = 0; k < kMaxNeeds; k++) { if (r->walk.hand[k] >= 0) slot_release_image(r->walk.hand[k]); r->walk.hand[k] = -1; }
            if (r->walk.ref >= 0) slot_release_buffer(r->walk.ref);
            r->walk.ref = -1;
            char line[160];
            snprintf(line, sizeof line, "bridge: record %llu: no semaphore for its copies", (unsigned long long)r->seq);
            report_add(line);
        }
    }
    mark_walked(r);
}

// After a draw (draw = 1; its draw_pre came first) or a clear.
void on_wrote(void*, u32 io, u32 laps, int draw, const u32* handles, const u32* locations, const u32* offsets, u32 n)
{
    BTime bt;
    if (draw) {
        Rec* r = B.pending;
        if (!r) return;   // other work's draw: decided at its draw_pre
        B.pending = nullptr;
        // the reference output, for the comparison
        static int cmp_off = -1;
        if (cmp_off < 0) { const char* e = getenv("DOTHEROES_PPFX_COMPARE"); cmp_off = e && *e == '0'; }
        const bool changed = mode() == 2 && !cmp_off && structure_changed(r);
        if (mode() == 2 && render_ready() && (compare_wanted(r) || changed) && n) {
            const Need& t = r->need[0];
            uint32_t bpp = 0;
            surface_vkformat(t.format, &bpp);
            uint64_t buf = 0;
            const int slot = bpp ? slot_buffer((uint64_t)t.w * t.h * bpp, &buf, t.format == 0x08) : -1;
            if (slot >= 0 && !rsx_vk_bridge_copy_buffer(handles[0], (VkBuffer)buf, 0, t.w, t.h)) {
                r->walk.ref = (int16_t)slot; r->walk.ref_w = t.w; r->walk.ref_h = t.h;
                B.copies++;
            } else {
                if (slot >= 0) slot_release_buffer(slot);
                stats().cmp_skipped++;
                stats().cmp_no_slot++;
            }
        }
        finish(r);
        return;
    }
    if (!match(kClear, laps, io)) { foreign(locations, offsets, n, 0); return; }
    Rec* r = queue().at(B.w);
    for (uint32_t k = 0; k < kMaxNeeds; k++) r->walk.hand[k] = -1;
    r->walk.ref = -1; r->walk.sem = -1; r->walk.ordinal = -1;
    B.copies = 0;
    for (uint32_t i = 0; i < n; i++) B.dirty.erase((uint64_t)locations[i] << 32 | offsets[i]);
    finish(r);
}

}  // namespace

void bridge_install()
{
    const char* x = getenv("DOTHEROES_PPFX_XCHECK");
    B.xcheck = !(x && *x == '0');
    static rsx_engine_bridge b = { nullptr, on_draw_pre, on_wrote };
    rsx_draw_engine_set_bridge(&b);
}

// The cross-check's per-register aggregate, for the report.
void bridge_report(std::vector<std::string>& out)
{
    char line[200];
    std::lock_guard<std::mutex> g(B.xr_lock);
    for (auto& kv : B.xr) {
        snprintf(line, sizeof line, "xcheck %s: %llu draws, record %08X engine %08X (the last); records %llu-%llu, "
                 "frames %u-%u", kv.first.c_str(), (unsigned long long)kv.second.n, kv.second.rec, kv.second.eng,
                 (unsigned long long)kv.second.seq0, (unsigned long long)kv.second.seq1, kv.second.frame0,
                 kv.second.frame1);
        out.push_back(line);
    }
}

}  // namespace ppfx
