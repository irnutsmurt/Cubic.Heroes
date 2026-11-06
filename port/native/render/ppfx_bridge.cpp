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
// matched draw, the record's explicit state against the engine's register
// file decoded into the same explicit form (engine_state: the only place
// registers are read, development only) -- the items the draw uses, the
// vertex-program constants and words, the fragment program's bytes in local
// memory now (after every queued patch landed), and the vertex and static
// texture bytes the engine is about to read. It only counts and reports; the
// records never take anything from the engine.
//
// R4 (docs/NATIVE_R4.md, DOTHEROES_PPFX_BYPASS): a bypassed record has no
// commands. In their place the recorder writes a stream marker
// (bridge_marker, below): NV4097 method 0x0100 (no operation), not
// incrementing, three words -- kMarkMagic and the record's sequence number --
// which the engine ignores and hands to on_marker. At a marker the walker
// does for the record what it did at its draw: the engine's surfaces it
// needs copied for it (handoffs). Its target is then the native side's: the
// engine's copy is stale. Before the engine next reads or writes such a
// surface -- another draw renders into it, samples it, a clear, a 2D
// transfer or a guest read of its memory (a snapshot's JPEG encoder), the
// present -- the walker hands the native image back (handback): the native
// renderer submits everything walked with the image ready to copy from and a
// semaphore to signal; the engine's next submission waits on it and copies
// the image into its surface first. A later native batch waits on a signal
// of that engine submission before it touches the image again.
//
// Ordering: a marker sits where the record's commands were, so the walker
// meets it after every earlier command and before every later one -- other
// work's included -- and the record's sequence number is in it. All of R4's
// bridging -- the marker's format and its writing, the walker's hooks, the
// handbacks and their semaphores -- is here; when the engine draws nothing
// PPFX hands over to (asp and the HUD native), it goes with this file.

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
#include "rsx_texture_layout.h"

extern "C" uint8_t* vm_base;
extern "C" uint32_t cellGcmResolveLocated(int local, uint32_t offset);
extern "C" void cellGcm_fifo_recycle_n(uint32_t ctx_ea, uint32_t count);   // cellGcmSys.c: the runtime's reserve callback
extern "C" uint32_t ppu_hle_inject_base;                                   // runtime/memory/vm.h
extern "C" uint32_t rsx_fp_program_size(const uint8_t* ucode, uint32_t max_bytes);

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
    // R4: surfaces whose newest content is the native side's (key -> the record that wrote it)
    struct Held { uint64_t seq; uint32_t w, h, format, pitch; };
    std::unordered_map<uint64_t, Held> native;
    bool war_pending;                       // a handback's copy: the next record waits for the engine past it
    std::vector<int> pending_release;       // the handbacks' native semaphores, freed with that record's batch
    int mphase; uint32_t mlo;               // the marker being read
    uint8_t last_bypassed;
    // R5 (DOTHEROES_ASP_SHARE=1): surfaces asp's records drew natively, by the
    // frame they did -- PPFX reads the native image of them, not a handoff
    std::unordered_map<uint64_t, uint32_t> asp_drawn;
    std::map<uint64_t, uint64_t> hand_by;   // handoffs by surface (the report)
} B;

int asp_share()
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DOTHEROES_ASP_SHARE"); on = e && *e == '1' && getenv("DOTHEROES_ASP_NATIVE") && *getenv("DOTHEROES_ASP_NATIVE") == '1'; }
    return on;
}

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

// Development (with the cross-check): what other work's draws take from
// PPFX's outputs -- a PPFX target their fragment program samples, a PPFX
// target they render into -- counted by address and unit
// (docs/NATIVE_R3B.md section 5). PPFX's targets: every record's colour
// target so far.
std::unordered_map<uint64_t, uint8_t> s_ppfx_targets;   // (location << 32 | offset) -> 1
std::map<std::string, uint64_t> s_foreign_reads;
std::mutex s_fr_lock;

void foreign_reads(const uint32_t* locations, const uint32_t* offsets, uint32_t n)
{
    const rsx_dispatch* E = rsx_draw_engine_state();
    if (!E) return;
    char key[96];
    std::lock_guard<std::mutex> g(s_fr_lock);
    for (uint32_t i = 0; i < n; i++)
        if (s_ppfx_targets.count((uint64_t)locations[i] << 32 | offsets[i])) {
            snprintf(key, sizeof key, "renders into PPFX target %u:%08X", locations[i], offsets[i]);
            s_foreign_reads[key]++;
        }
    uint32_t loc = 0;
    const uint32_t off = rsx_dsp_fragment_program(E, &loc);
    const uint32_t ea = cellGcmResolveLocated(loc == RSX_LOCATION_LOCAL, off);
    if (!ea) return;
    const uint32_t units = rsx_fp_texture_units(vm_base + ea, 0x8000);
    for (uint32_t u = 0; u < 16; u++) {
        if (!(units >> u & 1)) continue;
        rsx_dsp_texture t; rsx_dsp_get_texture(E, u, &t);
        if (!t.enabled || !s_ppfx_targets.count((uint64_t)t.location << 32 | t.offset)) continue;
        snprintf(key, sizeof key, "samples PPFX target %u:%08X on unit %u", t.location, t.offset, u);
        s_foreign_reads[key]++;
    }
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

/// One state item the record and the engine disagree on (R5: an asp
/// record's items under "asp ").
bool s_xc_asp;
void xdiff(const char* what, uint32_t rec, uint32_t eng, uint32_t& n)
{
    n++;
    auto& x = B.xr[s_xc_asp ? std::string("asp ") + what : std::string(what)];
    if (!x.n) { x.seq0 = B.cur->seq; x.frame0 = B.cur->frame; }
    x.n++; x.rec = rec; x.eng = eng; x.seq1 = B.cur->seq; x.frame1 = B.cur->frame;
}

uint32_t fu(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// The engine's state at the matched draw, in the explicit form
// (development only: the register file read through the toolkit's getters,
// decoded as the recorder's encoders would have set the same items). Only
// the cross-check uses it; records never take anything from it.
void engine_state(const rsx_dispatch* E, State* o)
{
    reset_state(o);
    rsx_dsp_surface sf; rsx_dsp_get_surface(E, &sf);
    Surface& s = o->surface;
    s.color_format = (uint8_t)sf.color_format; s.depth_format = (uint8_t)sf.depth_format; s.type = (uint8_t)sf.raster_type;
    s.antialias = (uint8_t)sf.antialias; s.color_target = (uint8_t)sf.color_target;
    for (int i = 0; i < 4; i++) {
        s.color_location[i] = (uint8_t)sf.color_location[i]; s.color_offset[i] = sf.color_offset[i];
        s.color_pitch[i] = sf.color_pitch[i];
    }
    s.depth_location = (uint8_t)sf.zeta_location; s.depth_offset = sf.zeta_offset; s.depth_pitch = sf.zeta_pitch;
    s.width = (uint16_t)(sf.clip_w / (sf.aa_sx ? sf.aa_sx : 1)); s.height = (uint16_t)(sf.clip_h / (sf.aa_sy ? sf.aa_sy : 1));
    s.x = (uint16_t)sf.clip_x; s.y = (uint16_t)sf.clip_y;
    rsx_dsp_viewport vp; rsx_dsp_get_viewport(E, &vp);
    Viewport& v = o->viewport;
    v.x = (uint16_t)vp.x; v.y = (uint16_t)vp.y; v.w = (uint16_t)vp.w; v.h = (uint16_t)vp.h;
    memcpy(v.scale, vp.scale, 16); memcpy(v.offset, vp.translate, 16);
    { uint32_t a = rsx_dsp_reg(E, 0x0394), b = rsx_dsp_reg(E, 0x0398); memcpy(&v.zmin, &a, 4); memcpy(&v.zmax, &b, 4); }
    const uint32_t sh = rsx_dsp_reg(E, 0x08C0), sv = rsx_dsp_reg(E, 0x08C4);
    o->scissor = { (uint16_t)sh, (uint16_t)sv, (uint16_t)(sh >> 16), (uint16_t)(sv >> 16) };
    const uint32_t fpo = rsx_dsp_reg(E, 0x08E4), ctl = rsx_dsp_shader_control(E);
    FragmentProgram& f = o->fp;
    f.offset = fpo & ~3u; f.location = (uint8_t)((fpo & 3u) - 1u);
    f.reg_count = (uint8_t)(ctl >> 24); f.txp = (uint8_t)(ctl >> 15 & 1); f.output_from_h0 = !(ctl & 0x40u);
    f.depth_replace = (ctl & 0xEu) != 0; f.pixel_kill = (uint8_t)(ctl >> 7 & 1);
    f.input_mask = rsx_dsp_reg(E, 0x1FF4) & ~0x20u;
    f.gamma = (uint8_t)rsx_dsp_reg(E, 0x1FEC);
    o->vp.start = (uint16_t)rsx_dsp_vp_start(E);
    o->vp.input_mask = rsx_dsp_reg(E, 0x1FF0);
    o->vp.regs_le32 = (rsx_dsp_reg(E, 0x1EF8) >> 16) == 0x20u;
    for (uint32_t u = 0; u < 16; u++) {
        rsx_dsp_texture t; rsx_dsp_get_texture(E, u, &t);
        Texture& x = o->tex[u];
        x.format = (uint8_t)t.format; x.mipmap = (uint8_t)t.mipmaps; x.dimension = (uint8_t)t.dimension;
        x.cubemap = (uint8_t)t.cubemap; x.location = (uint8_t)t.location; x.remap = t.remap;
        x.width = (uint16_t)t.width; x.height = (uint16_t)t.height; x.depth = (uint16_t)t.depth;
        x.pitch = t.pitch; x.offset = t.offset;
        Sampler& m = o->samp[u];
        m.enable = (uint8_t)(t.control0 >> 31); m.max_aniso = (uint8_t)(t.control0 >> 4 & 7);
        m.min_lod = (uint16_t)(t.control0 >> 19 & 0xFFF); m.max_lod = (uint16_t)(t.control0 >> 7 & 0xFFF);
        m.min_filter = (uint8_t)(t.filter >> 16 & 7); m.mag_filter = (uint8_t)(t.filter >> 24 & 7);
        m.conv = (uint8_t)(t.filter >> 13 & 7); m.bias = (uint16_t)(t.filter & 0x1FFF);
        m.filter_signed = (uint8_t)(t.filter >> 28);
        m.wrap_s = t.wrap & 0xF; m.wrap_t = t.wrap >> 8 & 0xF; m.unsigned_remap = t.wrap >> 12 & 0xF;
        m.wrap_r = t.wrap >> 16 & 0xF; m.gamma = t.wrap >> 20 & 0xF; m.zfunc = t.wrap >> 28 & 0xF;
        m.border = t.border_color;
    }
    for (uint32_t i = 0; i < 16; i++) {
        rsx_dsp_vertex_attr a; rsx_dsp_get_vertex_attr(E, i, &a);
        VertexInput& in = o->vin[i];
        in.size = (uint8_t)a.size; in.type = (uint8_t)a.type; in.stride = (uint8_t)a.stride;
        in.frequency = (uint16_t)a.frequency; in.offset = a.offset; in.location = (uint8_t)a.location;
        rsx_dsp_vertex_default(E, i, o->vdata.current[i]);
    }
    o->vdata.base_offset = rsx_dsp_vertex_data_base_offset(E);
    o->vdata.base_index = rsx_dsp_reg(E, 0x173C);
    o->vdata.divider_op = (uint16_t)rsx_dsp_reg(E, 0x1FC0);
    rsx_be_render_state rs; rsx_draw_engine_decode_render_state(E, &rs);
    Raster& r = o->raster;
    r.color_mask = rs.color_mask; r.blend = (uint8_t)rs.blend_enable; r.alpha_test = (uint8_t)rs.alpha_test_enable;
    r.depth_test = (uint8_t)rs.depth_test; r.depth_write = (uint8_t)rs.depth_write; r.depth_func = (uint16_t)rs.depth_func;
    r.cull = (uint8_t)rs.cull_enable; r.cull_face = (uint16_t)rs.cull_face; r.front_face = (uint16_t)rs.front_face;
    r.stencil_test = (uint8_t)rs.stencil_enable;
    const uint32_t aa = rsx_dsp_reg(E, 0x1D7C);
    r.aa_enable = aa & 1; r.alpha_to_coverage = aa >> 4 & 1; r.alpha_to_one = aa >> 8 & 1; r.sample_mask = (uint16_t)(aa >> 16);
    r.poly_offset_fill = (uint8_t)(rsx_dsp_reg(E, 0x0A68) & 1);
    r.blend_sf_rgb = (uint16_t)rs.sf_rgb; r.blend_df_rgb = (uint16_t)rs.df_rgb; r.blend_sf_a = (uint16_t)rs.sf_a;
    r.blend_df_a = (uint16_t)rs.df_a; r.blend_eq_rgb = (uint16_t)rs.eq_rgb; r.blend_eq_a = (uint16_t)rs.eq_a;
    r.alpha_func = (uint16_t)rs.alpha_func; r.alpha_ref = (uint8_t)rsx_dsp_reg(E, 0x030C);
    r.stencil_func = (uint16_t)rs.s_func; r.stencil_fail = (uint16_t)rs.s_fail; r.stencil_zfail = (uint16_t)rs.s_zfail;
    r.stencil_zpass = (uint16_t)rs.s_zpass; r.stencil_ref = (uint8_t)rsx_dsp_reg(E, 0x0334);
    r.stencil_func_mask = (uint8_t)rs.s_func_mask; r.stencil_write_mask = (uint8_t)rs.s_write_mask;
    r.front_poly_mode = (uint16_t)rsx_dsp_reg(E, 0x1828); r.back_poly_mode = (uint16_t)rsx_dsp_reg(E, 0x182C);
    { uint32_t a = rsx_dsp_reg(E, 0x0A78), b = rsx_dsp_reg(E, 0x0A7C);
      memcpy(&r.poly_offset_factor, &a, 4); memcpy(&r.poly_offset_units, &b, 4); }
}

const char* by_name(uint8_t b) { return b == kByPpfx ? "" : b == kByAsp ? " (asp's)" : b == kByGame ? " (the game's)" : " (reset)"; }

// The effective state of the matched draw, record against engine: what the
// engine's derivation reads for this draw -- the surface, viewport and
// scissor, the programs' parameters, the units the fragment program samples,
// the inputs the vertex program reads, the render state that matters with
// the enables as they are, the constants the vertex program reads, the
// programs' bytes, and the vertex bytes.
void crosscheck(Rec* r)
{
    const rsx_dispatch* E = rsx_draw_engine_state();
    if (!E) return;
    Stats& st = stats();
    st.xc_draws++;
    static State e;
    engine_state(E, &e);
    const State& a = r->st;
    std::lock_guard<std::mutex> g(B.xr_lock);
    B.cur = r;
    uint32_t dr = 0, dc = 0;
    char name[64];
#define X(field, label) if ((uint32_t)(a.field) != (uint32_t)(e.field)) xdiff(label, (uint32_t)(a.field), (uint32_t)(e.field), dr)
#define XF(field, label) if (fu(a.field) != fu(e.field)) xdiff(label, fu(a.field), fu(e.field), dr)
    X(surface.color_format, "surface.color_format"); X(surface.color_target, "surface.color_target");
    X(surface.antialias, "surface.antialias"); X(surface.color_location[0], "surface.color_location");
    X(surface.color_offset[0], "surface.color_offset"); X(surface.color_pitch[0], "surface.color_pitch");
    X(surface.width, "surface.width"); X(surface.height, "surface.height");
    X(surface.x, "surface.x"); X(surface.y, "surface.y");
    X(viewport.x, "viewport.x"); X(viewport.y, "viewport.y"); X(viewport.w, "viewport.w"); X(viewport.h, "viewport.h");
    XF(viewport.zmin, "viewport.zmin"); XF(viewport.zmax, "viewport.zmax");
    for (int i = 0; i < 4; i++) {
        snprintf(name, sizeof name, "viewport.scale[%d]", i); XF(viewport.scale[i], name);
        snprintf(name, sizeof name, "viewport.offset[%d]", i); XF(viewport.offset[i], name);
    }
    X(scissor.x, "scissor.x"); X(scissor.y, "scissor.y"); X(scissor.w, "scissor.w"); X(scissor.h, "scissor.h");
    X(fp.offset, "fp.offset"); X(fp.location, "fp.location");
    if ((a.fp.reg_count < 2 ? 2u : a.fp.reg_count) != e.fp.reg_count) xdiff("fp.reg_count", a.fp.reg_count, e.fp.reg_count, dr);
    X(fp.txp, "fp.txp"); X(fp.output_from_h0, "fp.output_from_h0"); X(fp.depth_replace, "fp.depth_replace");
    X(fp.pixel_kill, "fp.pixel_kill"); X(fp.gamma, "fp.gamma");
    if ((a.fp.input_mask & ~0x20u) != e.fp.input_mask) xdiff("fp.input_mask", a.fp.input_mask, e.fp.input_mask, dr);
    X(vp.start, "vp.start"); X(vp.input_mask, "vp.input_mask"); X(vp.regs_le32, "vp.regs_le32");
    // the units the fragment program samples (R5: an asp record does not
    // describe its programs yet -- the engine's choose what is compared)
    const bool asp = r->kind == kAspDraw;
    Blob evp, efp;
    if (asp && (!r->vp || !r->fp)) {   // a record without its programs: the engine's choose what is compared
        const uint32_t st0 = rsx_dsp_vp_start(E);
        const uint32_t n = st0 < 544 ? rsx_vp_program_size_instrs((const uint8_t*)(E->vp + st0 * 4), (544 - st0) * 16) : 0;
        if (n) evp.bytes.assign((const uint8_t*)(E->vp + st0 * 4), (const uint8_t*)(E->vp + st0 * 4) + n * 16);
        uint32_t floc = 0;
        const uint32_t foff = rsx_dsp_fragment_program(E, &floc);
        const uint32_t fea = cellGcmResolveLocated(floc == RSX_LOCATION_LOCAL, foff);
        const uint32_t fsz = fea ? rsx_fp_program_size(vm_base + fea, 0x8000) : 0;
        if (fsz) efp.bytes.assign(vm_base + fea, vm_base + fea + fsz);
    }
    const Blob* vpb = asp && !r->vp ? (evp.bytes.empty() ? nullptr : &evp) : r->vp.get();
    const Blob* fpb = asp && !r->fp ? (efp.bytes.empty() ? nullptr : &efp) : r->fp.get();
    const uint32_t units = fpb ? rsx_fp_texture_units(fpb->bytes.data(), (uint32_t)fpb->bytes.size()) : 0;
    for (uint32_t u = 0; u < 16; u++) {
        if (!(units >> u & 1)) continue;
#define XU(field) snprintf(name, sizeof name, "tex%u." #field "%s", u, by_name(a.samp[u].address_by)); \
                  if ((uint32_t)a.field != (uint32_t)e.field) xdiff(name, (uint32_t)a.field, (uint32_t)e.field, dr)
        XU(samp[u].enable);
        if (!a.samp[u].enable) continue;
        XU(tex[u].format); XU(tex[u].mipmap); XU(tex[u].dimension); XU(tex[u].cubemap); XU(tex[u].location);
        XU(tex[u].remap); XU(tex[u].width); XU(tex[u].height); XU(tex[u].depth); XU(tex[u].pitch); XU(tex[u].offset);
        XU(samp[u].max_aniso); XU(samp[u].min_lod); XU(samp[u].max_lod); XU(samp[u].min_filter); XU(samp[u].mag_filter);
        XU(samp[u].conv); XU(samp[u].bias); XU(samp[u].filter_signed); XU(samp[u].wrap_s); XU(samp[u].wrap_t);
        XU(samp[u].wrap_r); XU(samp[u].unsigned_remap); XU(samp[u].zfunc); XU(samp[u].gamma); XU(samp[u].border);
#undef XU
    }
    // the inputs the vertex program reads
    rsx_vp_input_analysis an = { 0xFFFFu, 0 };
    if (vpb) rsx_vp_analyze_inputs(vpb->bytes.data(), (uint32_t)vpb->bytes.size(), &an);
    for (uint32_t i = 0; i < 16; i++) {
        if (!(an.input_mask >> i & 1)) continue;
        snprintf(name, sizeof name, "vin%u.size", i); X(vin[i].size, name);
        if (!a.vin[i].size) {
            for (int k = 0; k < 4; k++) { snprintf(name, sizeof name, "vin%u.current[%d]", i, k); XF(vdata.current[i][k], name); }
            continue;
        }
        if ((a.vin[i].type & 7u) != e.vin[i].type) { snprintf(name, sizeof name, "vin%u.type", i); xdiff(name, a.vin[i].type, e.vin[i].type, dr); }
        snprintf(name, sizeof name, "vin%u.stride", i); X(vin[i].stride, name);
        snprintf(name, sizeof name, "vin%u.frequency", i); X(vin[i].frequency, name);
        snprintf(name, sizeof name, "vin%u.location", i); X(vin[i].location, name);
        if ((a.vin[i].offset & 0x7FFFFFFFu) != e.vin[i].offset) {
            snprintf(name, sizeof name, "vin%u.offset", i); xdiff(name, a.vin[i].offset, e.vin[i].offset, dr);
        }
    }
    X(vdata.base_offset, "vdata.base_offset"); X(vdata.base_index, "vdata.base_index"); X(vdata.divider_op, "vdata.divider_op");
    // the render state, with what the enables make irrelevant left out
    snprintf(name, sizeof name, "raster.color_mask%s", by_name(a.raster.color_mask_by)); X(raster.color_mask, name);
    X(raster.blend, "raster.blend"); X(raster.alpha_test, "raster.alpha_test"); X(raster.stencil_test, "raster.stencil_test");
    if (a.raster.blend) {
        X(raster.blend_sf_rgb, "raster.blend_sf_rgb"); X(raster.blend_df_rgb, "raster.blend_df_rgb");
        X(raster.blend_sf_a, "raster.blend_sf_a"); X(raster.blend_df_a, "raster.blend_df_a");
        X(raster.blend_eq_rgb, "raster.blend_eq_rgb"); X(raster.blend_eq_a, "raster.blend_eq_a");
    }
    if (a.raster.alpha_test) { X(raster.alpha_func, "raster.alpha_func"); X(raster.alpha_ref, "raster.alpha_ref"); }
    if (a.raster.stencil_test) {
        X(raster.stencil_func, "raster.stencil_func"); X(raster.stencil_ref, "raster.stencil_ref");
        X(raster.stencil_func_mask, "raster.stencil_func_mask"); X(raster.stencil_write_mask, "raster.stencil_write_mask");
        X(raster.stencil_fail, "raster.stencil_fail"); X(raster.stencil_zfail, "raster.stencil_zfail");
        X(raster.stencil_zpass, "raster.stencil_zpass");
    }
    if (asp) { X(raster.front_poly_mode, "raster.front_poly_mode"); X(raster.back_poly_mode, "raster.back_poly_mode"); }
    X(raster.depth_test, "raster.depth_test");
    if (a.raster.depth_test) { X(raster.depth_write, "raster.depth_write"); X(raster.depth_func, "raster.depth_func"); }
    X(raster.cull, "raster.cull");
    if (a.raster.cull) { X(raster.cull_face, "raster.cull_face"); X(raster.front_face, "raster.front_face"); }
    X(raster.aa_enable, "raster.aa_enable"); X(raster.alpha_to_coverage, "raster.alpha_to_coverage");
    X(raster.alpha_to_one, "raster.alpha_to_one"); X(raster.sample_mask, "raster.sample_mask");
    X(raster.poly_offset_fill, "raster.poly_offset_fill");
    if (a.raster.poly_offset_fill) { XF(raster.poly_offset_factor, "raster.poly_offset_factor"); XF(raster.poly_offset_units, "raster.poly_offset_units"); }
#undef X
#undef XF
    // the constants the vertex program reads
    uint32_t cm[16];
    vp_const_mask(vpb, cm);
    for (uint32_t k = 0; k < 512; k++)
        if ((cm[k >> 5] >> (k & 31) & 1) && memcmp(E->constants[k], r->vconst[k], 16)) {
            snprintf(name, sizeof name, "const c%u%s", k,
                     r->vconst_written[k >> 5] >> (k & 31) & 1 ? "" : " (never set)");
            xdiff(name, r->vconst[k][0], E->constants[k][0], dc);
        }
    r->walk.diff_regs = dr; r->walk.diff_consts = dc;
    if (asp) {   // R5: asp's items only; its programs and vertex bytes are not described yet
        static int probe = -1, n = 0;
        if (probe < 0) { const char* e = getenv("DOTHEROES_ASP_PROBE"); probe = e && *e == '1'; }
        if (probe && n < 64) {   // what the engine runs at this asp draw, beside the probe's containers
            n++;
            const uint32_t vs = rsx_dsp_vp_start(E);
            uint32_t floc = 0;
            const uint32_t foff = rsx_dsp_fragment_program(E, &floc);
            const uint32_t fea = cellGcmResolveLocated(floc == RSX_LOCATION_LOCAL, foff);
            char line[400];
            snprintf(line, sizeof line, "asp probe draw seq %llu: engine vp start %u instrs %zu first %08X %08X %08X %08X | fp %u:%08X "
                     "first %08X %08X %08X %08X control %08X", (unsigned long long)r->seq, vs, evp.bytes.size() / 16,
                     vs < 544 ? E->vp[vs * 4] : 0, vs < 544 ? E->vp[vs * 4 + 1] : 0, vs < 544 ? E->vp[vs * 4 + 2] : 0,
                     vs < 544 ? E->vp[vs * 4 + 3] : 0, floc, foff,
                     fea ? __builtin_bswap32(*(const uint32_t*)(vm_base + fea)) : 0,
                     fea ? __builtin_bswap32(*(const uint32_t*)(vm_base + fea + 4)) : 0,
                     fea ? __builtin_bswap32(*(const uint32_t*)(vm_base + fea + 8)) : 0,
                     fea ? __builtin_bswap32(*(const uint32_t*)(vm_base + fea + 12)) : 0, rsx_dsp_shader_control(E));
            report_add(line);
            char l2[400];
            int at = snprintf(l2, sizeof l2, "asp probe draw seq %llu streams record/engine:", (unsigned long long)r->seq);
            for (uint32_t i = 0; i < 16 && at < (int)sizeof l2 - 40; i++)
                if (r->st.vin[i].size || e.vin[i].size)
                    at += snprintf(l2 + at, sizeof l2 - at, " v%u %X/%X off %08X/%08X", i, r->st.vin[i].size, e.vin[i].size,
                                   r->st.vin[i].offset, e.vin[i].offset);
            report_add(l2);
        }
        // its programs against what the engine runs
        {
            const uint32_t s0 = rsx_dsp_vp_start(E);
            if (!r->vp || s0 >= 544 || r->vp->bytes.size() > (544 - s0) * 16 ||
                memcmp(E->vp + s0 * 4, r->vp->bytes.data(), r->vp->bytes.size()))
                xdiff(r->vp ? "vp.bytes" : "vp.bytes (none recorded)", r->vp ? (uint32_t)r->vp->bytes.size() : 0, s0, dr);
            uint32_t floc = 0;
            const uint32_t foff = rsx_dsp_fragment_program(E, &floc);
            const uint32_t fea = cellGcmResolveLocated(floc == RSX_LOCATION_LOCAL, foff);
            if (!r->fp || !fea || memcmp(vm_base + fea, r->fp->bytes.data(), r->fp->bytes.size())) {
                uint32_t first = ~0u;
                if (r->fp && fea)
                    for (size_t i = 0; i + 4 <= r->fp->bytes.size() && first == ~0u; i += 4)
                        if (memcmp(vm_base + fea + i, &r->fp->bytes[i], 4)) first = (uint32_t)i;
                xdiff(r->fp ? "fp.bytes (first differing byte)" : "fp.bytes (none recorded)", first,
                      r->fp ? (uint32_t)r->fp->bytes.size() : 0, dr);
                static int said = 0;
                if (r->fp && fea && said++ < 12) {
                    char line[400];
                    int at = snprintf(line, sizeof line, "xfp asp seq %llu frame %u off %08X (record %08X) bytes %zu | record:",
                                      (unsigned long long)r->seq, r->frame, foff, r->st.fp.offset, r->fp->bytes.size());
                    for (size_t i = 0; i < 16 && i + 4 <= r->fp->bytes.size(); i += 4)
                        at += snprintf(line + at, sizeof line - at, " %02X%02X%02X%02X", r->fp->bytes[i], r->fp->bytes[i + 1],
                                       r->fp->bytes[i + 2], r->fp->bytes[i + 3]);
                    at += snprintf(line + at, sizeof line - at, " | engine:");
                    for (uint32_t i = 0; i < 16; i += 4)
                        at += snprintf(line + at, sizeof line - at, " %02X%02X%02X%02X", vm_base[fea + i], vm_base[fea + i + 1],
                                       vm_base[fea + i + 2], vm_base[fea + i + 3]);
                    report_add(line);
                }
            }
        }
        st.asp_xc_draws++;
        if (dr || dc) st.asp_xc_differ++;
        return;
    }
    if (dr || dc) frame_flag_inherited(r->frame);
    if (dr) st.xc_regs++;
    if (dc) st.xc_consts++;
    // the vertex program the engine will run
    const uint32_t start = rsx_dsp_vp_start(E);
    if (!r->vp || start != r->st.vp.start || start >= 544 || r->vp->bytes.size() > (544 - start) * 16 ||
        memcmp(E->vp + start * 4, r->vp->bytes.data(), r->vp->bytes.size())) {
        r->walk.diff_vp = 1; st.xc_vp++;
    }
    // the fragment program as local memory has it now
    uint32_t loc = 0;
    const uint32_t off = rsx_dsp_fragment_program(E, &loc);
    if (!r->fp || off != r->st.fp.offset) { r->walk.diff_fp = 1; st.xc_fp++; }
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
        if (r->bypassed) return 0;   // R4: met at its marker, never by its commands
        if (r->kind == kAspDraw) {
            // R5: an asp draw is the engine's first draw inside its commands
            // (the context's position at its call to the one at its return)
            if (!B.have_delta) {
                if (kind == kDraw && io >= r->io && io <= r->io_end && r->lap_end == r->lap) {
                    B.delta = (int64_t)laps - r->lap; B.have_delta = 1;
                    report_add("bridge: first match at an asp record");
                } else if (io > r->io_end && r->lap_end == r->lap && io - r->io_end < 0x100000u) {
                    stats().asp_missed++; r->walk.matched = 0; mark_walked(r); continue;
                } else return 0;
            }
            const uint64_t wk = K(laps, io);
            const uint64_t lo = K(r->lap + B.delta, r->io), hi = K(r->lap_end + B.delta, r->io_end);
            if (kind == kDraw && wk >= lo && wk <= hi) { r->walk.matched = 1; stats().asp_matched++; return 1; }
            if (wk <= hi) return 0;   // ahead of it: other work's
            stats().asp_missed++;     // behind it: the asp draw made no engine draw
            r->walk.matched = 0;
            mark_walked(r);
            continue;
        }
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
            if (kind == kDraw) stats().marker_r3_matched++;   // a PPFX draw the engine draws itself
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

void guest_texture(Rec* r, const Need& n, uint32_t limit);
// R5: an asp record's small uncompressed guest textures, taken at its place
// in the stream (ppfx_wrote marks the DXT ones and the surfaces: not taken)
void asp_guest_textures(Rec* r)
{
    for (uint32_t k = 0; k < r->nneed; k++) {
        const Need& n = r->need[k];
        if (n.unit < 16 && !n.ppfx_wrote) guest_texture(r, n, 256u << 10);
    }
}

// The engine's surface behind a record's need, copied into a slot when the
// native side does not already hold that content: other work wrote the
// address since PPFX last did (dirty), or no record has written it (the
// first use: the engine's surface as it was created, or other work's).
// A texture unit's bytes as guest memory holds them now, at the record's
// place in the stream, for a texture neither the native renderer nor the
// engine holds an image of: what the engine itself would read there -- bytes
// the game wrote, or a 2D transfer the walker has just run (the engine's
// transfers read a surface back into guest memory: the post-boss warp's
// capture of the last displayed frame, 0:05530000, docs/NATIVE_R4.md section
// 11). The recorder's own snapshot at the call (small textures) is kept when
// there is one.
void guest_texture(Rec* r, const Need& n, uint32_t limit = 16u << 20)
{
    for (uint32_t k = 0; k < r->ntex; k++) if (r->tex[k].location == n.unit) return;   // the recorder's
    const Texture& t = r->st.tex[n.unit];
    if (t.cubemap || r->ntex >= 16) return;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const uint32_t nlv = rsx_texture_mip_chain(t.format, t.width, t.height, t.mipmap, t.pitch, lv);
    const uint32_t bytes = nlv ? lv[nlv - 1].offset + lv[nlv - 1].tl.face_bytes : 0;
    if (bytes > limit) return;
    const uint32_t ea = cellGcmResolveLocated(n.location == 0, n.offset);
    if (!bytes || bytes > (16u << 20) || ea < 0x10000u || (uint64_t)ea + bytes > 0xE0000000ull) { stats().guest_tex_fail++; return; }
    VtxRange& tr = r->tex[r->ntex++];
    tr.location = n.unit; tr.offset = n.offset; tr.bytes = bytes; tr.at = (uint32_t)r->tdata.size();
    r->tdata.insert(r->tdata.end(), vm_base + ea, vm_base + ea + bytes);
    stats().guest_tex++;
    stats().guest_tex_bytes += bytes;
}

void handoffs(Rec* r)
{
    Stats& st = stats();
    for (uint32_t k = 0; k < r->nneed; k++) {
        const Need& n = r->need[k];
        const uint64_t key = (uint64_t)n.location << 32 | n.offset;
        if (asp_share()) {   // R5: asp's scene, drawn natively this frame and not written since by other work
            auto ad = B.asp_drawn.find(key);
            if (ad != B.asp_drawn.end() && ad->second == r->frame && !B.dirty.count(key)) { st.handoffs_shared++; continue; }
        }
        u32 w = 0, h = 0, fmt = 0;
        const u32 handle = rsx_draw_engine_find_surface(n.location, n.offset, &w, &h, &fmt);
        if (!handle) {   // not a surface the engine has: a texture in guest memory, or nothing yet
            if (n.unit != 0xFF && !n.ppfx_wrote) guest_texture(r, n);
            continue;
        }
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
        { std::lock_guard<std::mutex> g(B.xr_lock); B.hand_by[key]++; }
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

// ---- R4: handbacks --------------------------------------------------------------------

void finish(Rec* r);   // below

void walk_init(Rec* r)
{
    for (uint32_t k = 0; k < kMaxNeeds; k++) r->walk.hand[k] = -1;
    r->walk.ref = -1; r->walk.sem = -1; r->walk.ordinal = -1;
}

// DOTHEROES_PPFX_HANDBACK_CHECK (default 1 with comparisons, 0 without):
// every handback of a final composite, and every 60th otherwise, is read
// back on both sides after the copy and compared byte for byte -- a copy
// that ran before the native side's work was done would differ. =<n> (n > 1):
// every n-th handback (a lighter sample: each check waits for the GPU).

FILE* s_present_hash;   // DOTHEROES_PPFX_PRESENT_HASH=<file>: every present's displayed image, hashed

// The native image of `key` handed back into the engine's surface, here in
// the engine's stream (kind: why, for the counts).
void handback(uint64_t key, int kind)
{
    auto it = B.native.find(key);
    if (it == B.native.end()) return;
    const BState::Held held = it->second;
    B.native.erase(it);
    Stats& st = stats();
    const uint64_t t0 = bnow();
    const uint32_t loc = (uint32_t)(key >> 32), off = (uint32_t)key;
    // the engine's surface there, made if it has none (the native side's size and format)
    u32 w = 0, h = 0, fmt = 0;
    u32 handle = rsx_draw_engine_find_surface(loc, off, &w, &h, &fmt);
    if (!handle) handle = rsx_draw_engine_surface_ensure(loc, off, held.w, held.h, surface_beformat(held.format), held.pitch);
    if (!handle) {
        st.handback_fail++;
        char line[160];
        snprintf(line, sizeof line, "bridge: handback of %u:%08X (record %llu): the engine has no surface for it", loc, off,
                 (unsigned long long)held.seq);
        report_add(line);
        return;
    }
    // the engine's work so far is submitted, with the signals the native side may be waiting for
    rsx_vk_bridge_flush();
    static int chk = -1;
    if (chk < 0) {
        const char* e = getenv("DOTHEROES_PPFX_HANDBACK_CHECK");
        const char* c = getenv("DOTHEROES_PPFX_COMPARE");
        chk = e && *e ? atoi(e) : !(c && *c == '0');
    }
    // 1: every final composite's and every 60th other; n > 1: every n-th of any
    static uint64_t n = 0;
    const bool check = chk == 1 ? (held.format == 0x08 || n++ % 60 == 0) : chk > 1 && n++ % (uint64_t)chk == 0;
    HandOut o;
    const uint64_t tw = bnow();
    const int rc = handback_take(loc, off, check, 10000, &o);
    st.handback_wait_ns += bnow() - tw;
    if (rc) {
        st.handback_fail++;
        char line[160];
        snprintf(line, sizeof line, "bridge: handback of %u:%08X (record %llu) failed (%d)", loc, off,
                 (unsigned long long)held.seq, rc);
        report_add(line);
        return;
    }
    if (rsx_vk_bridge_copy_into(handle, (VkImage)o.image, (VkImageLayout)o.layout, o.w, o.h, (VkSemaphore)o.sem)) {
        st.handback_fail++;   // its semaphore is signalled and never waited: the slot stays taken
        if (o.check_slot >= 0) handback_check_done(o.check_slot);
        return;
    }
    B.war_pending = true;
    B.pending_release.push_back(o.sem_slot);
    B.dirty.erase(key);
    st.handbacks++;
    st.handbacks_by_kind[kind < 6 ? kind : 5]++;
    if (o.check_slot >= 0) {
        // both sides read back: the engine's surface after the copy (this waits
        // for the engine, which waited for the native side)
        static std::vector<uint8_t> eng;
        const uint32_t bytes = o.w * o.h * o.bpp;
        if (eng.size() < (size_t)bytes * 2) eng.resize((size_t)bytes * 2);
        u32 ew = 0, eh = 0, eb = 0;
        if (!rsx_draw_engine_readback_surface(loc, off, eng.data(), (u32)eng.size(), &ew, &eh, &eb) && eb == o.bpp) {
            const uint32_t cw = ew < o.w ? ew : o.w, chh = eh < o.h ? eh : o.h;
            bool same = true;
            for (uint32_t y = 0; y < chh && same; y++)
                same = !memcmp(&eng[(size_t)y * ew * eb], o.check_rows + (size_t)y * o.w * o.bpp, (size_t)cw * eb);
            st.hb_checked++;
            if (same) st.hb_equal++;
            else {
                st.hb_differ++;
                char line[160];
                snprintf(line, sizeof line, "bridge: handback of %u:%08X (record %llu): the engine's copy differs from the "
                         "native image", loc, off, (unsigned long long)held.seq);
                report_add(line);
            }
        }
        handback_check_done(o.check_slot);
    }
    st.handback_ns += bnow() - t0;
}

// The engine is about to read or write the surface at (loc, off) (rsx_engine_bridge.touch).
void on_touch(void*, u32 loc, u32 off, int kind)
{
    if (!B.native.empty()) {
        BTime bt;
        handback((uint64_t)loc << 32 | off, kind);
    }
    if (kind == 3 && s_present_hash) {
        // what is displayed: the engine's surface now, hashed (development; a GPU wait)
        static std::vector<uint8_t> buf(1280u * 720u * 16u);
        static uint64_t presents = 0;
        u32 w = 0, h = 0, bpp = 0;
        if (!rsx_draw_engine_readback_surface(loc, off, buf.data(), (u32)buf.size(), &w, &h, &bpp)) {
            const uint32_t frame = B.w ? queue().at(B.w - 1)->frame : 0;
            fprintf(s_present_hash, "%llu %u %u %016llx %ux%u\n", (unsigned long long)presents, frame, B.last_bypassed,
                    (unsigned long long)fnv(buf.data(), (size_t)w * h * bpp), w, h);
            stats().present_hashes++;
            if ((presents & 63) == 0) fflush(s_present_hash);
        }
        presents++;
    }
}

// The record a marker names, reached (the walker's thread).
void marker_reached(uint64_t seq)
{
    BTime bt;
    Queue& q = queue();
    const uint64_t put = q.put.load(std::memory_order_acquire);
    if (seq >= put || seq < B.w) { stats().marker_unknown++; return; }
    while (B.w < seq) {   // records before it the walker never met: counted, passed on
        Rec* m = q.at(B.w);
        walk_init(m);
        m->walk.matched = 0;
        stats().missed++;
        mark_walked(m);
    }
    Rec* r = q.at(B.w);
    if (!r->bypassed) { stats().marker_unknown++; return; }
    stats().markers_met++;
    walk_init(r);
    r->walk.matched = 1;
    B.copies = 0;
    if (r->kind == kAspDraw) {   // R5: asp's draw, the native side's alone; its colour target the native side's now
        asp_guest_textures(r);
        for (uint32_t k = 0; k < r->nneed; k++) {
            const Need& t = r->need[k];
            if (t.unit != 0xFF && t.unit != 0xFE) continue;
            const uint64_t key = (uint64_t)t.location << 32 | t.offset;
            B.dirty.erase(key);
            B.asp_drawn[key] = r->frame;
            if (t.unit == 0xFF) B.native[key] = { r->seq, t.w, t.h, t.format, t.pitch };
        }
        B.last_bypassed = 1;
        finish(r);
        return;
    }
    const Need& t = r->need[0];
    const uint64_t key = (uint64_t)t.location << 32 | t.offset;
    if (B.xcheck) { std::lock_guard<std::mutex> g(s_fr_lock); s_ppfx_targets[key] = 1; }
    static int nohand = -1;
    if (nohand < 0) { const char* e = getenv("DOTHEROES_PPFX_NO_HANDOFF"); nohand = e && *e == '1'; }
    if (mode() == 2 && render_ready() && !nohand) handoffs(r);
    // its target: the native side's now
    B.dirty.erase(key);
    B.native[key] = { r->seq, t.w, t.h, t.format, t.pitch };
    B.last_bypassed = 1;
    finish(r);
}

// Each NV4097 no-operation word the engine meets (rsx_engine_bridge.marker).
const uint32_t kMarkMagic = 0x52344D4Bu;   // "R4MK"
void on_marker(void*, u32, u32, u32 word)
{
    if (B.mphase == 0) { if (word == kMarkMagic) B.mphase = 1; return; }
    if (B.mphase == 1) { B.mlo = word; B.mphase = 2; return; }
    B.mphase = 0;
    marker_reached((uint64_t)word << 32 | B.mlo);
}

// DOTHEROES_ASP_DESCRIBE_FRAME=<frame> (a diagnostic, with DOTHEROES_PPFX_DUMP):
// every draw of that frame that is no record's -- the game's own libgcm
// draws -- with the engine's state at it, a line each (foreign_f<frame>.txt)
void foreign_describe(uint32_t io)
{
    static long long want = -2;
    static FILE* f = nullptr;
    if (want == -2) {
        const char* e = getenv("DOTHEROES_ASP_DESCRIBE_FRAME");
        want = e && *e ? atoll(e) : -1;
    }
    if (want < 0) return;
    const uint32_t frame = B.w ? queue().at(B.w - 1)->frame : 0;
    if ((long long)frame != want) { if (f && (long long)frame > want) { fclose(f); f = nullptr; want = -1; } return; }
    const char* dir = getenv("DOTHEROES_PPFX_DUMP");
    if (!dir || !*dir) return;
    if (!f) { char path[600]; snprintf(path, sizeof path, "%s/foreign_f%u.txt", dir, (uint32_t)want); if (!(f = fopen(path, "w"))) { want = -1; return; } }
    const rsx_dispatch* E = rsx_draw_engine_state();
    if (!E) return;
    State e;
    engine_state(E, &e);
    const Raster& r = e.raster;
    fprintf(f, "io %08X after record %llu | tgt %08X fmt %u dz %08X | cmask %08X | depth %u/%u %X | stencil %u %X ref %u mask %X/%X "
               "ops %X/%X/%X | blend %u | cull %u | fp %08X vp %u\n",
            io, (unsigned long long)(B.w ? queue().at(B.w - 1)->seq : 0), e.surface.color_offset[0], e.surface.color_format,
            e.surface.depth_offset, r.color_mask, r.depth_test, r.depth_write, r.depth_func, r.stencil_test, r.stencil_func,
            r.stencil_ref, r.stencil_func_mask, r.stencil_write_mask, r.stencil_fail, r.stencil_zfail, r.stencil_zpass, r.blend,
            r.cull, e.fp.offset, e.vp.start);
    fflush(f);
}

// DOTHEROES_ASP_FOREIGN_STATES=<file> (a check of R5's stage 2): the
// distinct engine states the draws that are no record's run with (the HUD,
// the menus) -- the same set with and without asp's state commands means
// they do not depend on them
std::map<uint64_t, std::pair<uint64_t, std::string>> s_fstates;
void foreign_states()
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DOTHEROES_ASP_FOREIGN_STATES"); on = e && *e; }
    if (!on) return;
    const rsx_dispatch* E = rsx_draw_engine_state();
    if (!E) return;
    State e;
    engine_state(E, &e);
    const Raster& r = e.raster;
    char d[400];
    snprintf(d, sizeof d, "tgt %08X fmt %u cmask %08X depth %u/%u %X stencil %u %X/%u cull %u %X/%X blend %u %X/%X %X/%X eq %X alpha %u %X/%u "
             "poff %u scissor %u,%u %ux%u viewport %u,%u %ux%u",
             e.surface.color_offset[0], e.surface.color_format, r.color_mask, r.depth_test, r.depth_write, r.depth_func, r.stencil_test,
             r.stencil_func, r.stencil_ref, r.cull, r.cull_face, r.front_face, r.blend, r.blend_sf_rgb, r.blend_df_rgb, r.blend_sf_a,
             r.blend_df_a, r.blend_eq_rgb, r.alpha_test, r.alpha_func, r.alpha_ref, r.poly_offset_fill, e.scissor.x, e.scissor.y,
             e.scissor.w, e.scissor.h, e.viewport.x, e.viewport.y, e.viewport.w, e.viewport.h);
    const uint64_t h = fnv(d, strlen(d));
    auto& slot = s_fstates[h];
    if (!slot.first) slot.second = d;
    slot.first++;
    static uint64_t n = 0;
    if ((++n & 1023) == 0 || n < 4) {
        FILE* f = fopen(getenv("DOTHEROES_ASP_FOREIGN_STATES"), "w");
        if (f) { for (auto& kv : s_fstates) fprintf(f, "%llu %s\n", (unsigned long long)kv.second.first, kv.second.second.c_str()); fclose(f); }
    }
}

void on_draw_pre(void*, u32 io, u32 laps, const u32* handles, const u32* locations, const u32* offsets, u32 n)
{
    (void)handles;
    BTime bt;
    // R4: a target whose newest content is the native side's comes back
    // first -- the draw blends onto it (the HUD over the final composite)
    if (!B.native.empty())
        for (uint32_t i = 0; i < n; i++) handback((uint64_t)locations[i] << 32 | offsets[i], 0);
    if (!match(kDraw, laps, io)) {
        foreign(locations, offsets, n, 1);
        if (B.xcheck) { const uint64_t x0 = bnow(); foreign_reads(locations, offsets, n); stats().xcheck_ns += bnow() - x0; }
        foreign_describe(io);
        foreign_states();
        return;
    }
    Rec* r = queue().at(B.w);
    if (r->kind == kAspDraw) {   // R5: asp's draw, the engine's to draw; checked against it
        if (asp_share() && mode() == 2) {   // drawn natively as well: its targets' newest content is the native side's
            for (uint32_t i = 0; i < n; i++) {
                const uint64_t key = (uint64_t)locations[i] << 32 | offsets[i];
                B.asp_drawn[key] = r->frame;
                B.dirty.erase(key);
            }
            stats().foreign_draws++;
        } else
        foreign(locations, offsets, n, 1);
        if (B.xcheck) {
            const uint64_t x0 = bnow();
            foreign_reads(locations, offsets, n);
            s_xc_asp = true; crosscheck(r); s_xc_asp = false;
            stats().xcheck_ns += bnow() - x0;
        }
        walk_init(r);
        if (mode() == 2) asp_guest_textures(r);
        // R5: a draw compared one by one (DOTHEROES_ASP_COMPARE_DRAWS): the
        // engine's target copied after this draw, as for PPFX's
        if (mode() == 2 && render_ready() && compare_wanted(r) != kCmpNone) { B.copies = 0; B.pending = r; return; }
        mark_walked(r);
        return;
    }
    if (B.xcheck) { std::lock_guard<std::mutex> g(s_fr_lock); s_ppfx_targets[(uint64_t)r->need[0].location << 32 | r->need[0].offset] = 1; }
    for (uint32_t k = 0; k < kMaxNeeds; k++) r->walk.hand[k] = -1;
    r->walk.ref = -1; r->walk.sem = -1; r->walk.ordinal = -1;
    B.last_bypassed = 0;
    B.copies = 0;
    if (B.xcheck) { const uint64_t x0 = bnow(); crosscheck(r); stats().xcheck_ns += bnow() - x0; }
    // DOTHEROES_PPFX_NO_HANDOFF=1 (a negative control): no handoffs, so the
    // native side lacks what other work drew and the comparison must see it
    static int nohand = -1;
    if (nohand < 0) { const char* e = getenv("DOTHEROES_PPFX_NO_HANDOFF"); nohand = e && *e == '1'; }
    if (mode() == 2 && render_ready() && !nohand) handoffs(r);
    // PPFX's own write: the native side will have these targets' content --
    // and the engine draws it too (R3): neither side's is newer
    for (uint32_t i = 0; i < n; i++) {
        B.dirty.erase((uint64_t)locations[i] << 32 | offsets[i]);
        B.native.erase((uint64_t)locations[i] << 32 | offsets[i]);
    }
    B.pending = r;   // finished after the engine's draw (on_wrote)
}

// The copies recorded for a record go with the engine's next submission of
// the command buffer they are in; that submission signals one semaphore,
// shared by every record whose copies went into the same command buffer. The
// native side waits on it once, before the first record that needs it; the
// engine submits at its own points (its batches, the flip), not forced here.
void finish(Rec* r)
{
    // R4: after a handback, the next record walked waits for an engine signal
    // past the handback's copy (the native side must not touch the image the
    // engine copies from before that), and frees the handbacks' semaphores
    // once its batch is done
    const bool war = B.war_pending;
    if (B.copies || war) {
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
            // with copies the engine's batch is open; a handback's signal alone opens one
            // (the engine's work after the handback's copy)
            ord = s >= 0 ? (B.copies ? rsx_vk_bridge_signal((VkSemaphore)sem, &used)
                                     : rsx_vk_bridge_signal_open((VkSemaphore)sem, &used)) : -2;
            if (ord >= 0 && used) { slot = (int16_t)s; B.last_ord = ord; B.last_slot = slot; }
            else if (s >= 0) slot_release_semaphore(s);
        }
        if (ord >= 0 && slot >= 0) {
            r->walk.sem = slot;
            r->walk.ordinal = ord;
            if (war) {
                for (size_t i = 0; i < B.pending_release.size() && i < 4; i++) r->walk.release_sem[i] = (int16_t)B.pending_release[i];
                B.pending_release.clear();
                B.war_pending = false;
            }
        } else if (!B.copies) {
            // only the handback's signal, and none could be had: the next record tries again
        } else {
            // nothing would order the native side after the copies: they are
            // not used, and their slots are free again once the engine's
            // submission has run them (the next frame's, at the latest: the
            // slots are only released here, the copies are harmless)
            stats().handoff_fail += B.copies;
            for (uint32_t k = 0; k < kMaxNeeds; k++) { if (r->walk.hand[k] >= 0) slot_release_image(r->walk.hand[k]); r->walk.hand[k] = -1; }
            if (r->walk.ref >= 0) { slot_release_buffer(r->walk.ref); stats().cmp_skip_no_ref++; }
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
        const bool changed = mode() == 2 && !cmp_off && r->kind != kAspDraw && structure_changed(r);
        int why = mode() == 2 && render_ready() ? compare_wanted(r) : kCmpNone;
        if (mode() == 2 && render_ready() && why == kCmpNone && changed) why = kCmpChange;
        if (why != kCmpNone && n) {
            // asked for: from here it ends counted as compared (equal or
            // differ), skipped (and why) or unfinished at exit
            Stats& st = stats();
            st.cmp_wanted++;
            if (why == kCmpChange || (changed && why == kCmpEvery)) { why = kCmpChange; st.cmp_change_wanted++; }
            r->walk.want_cmp = 1; r->walk.cmp_why = (uint8_t)why;
            const Need& t = r->need[0];
            uint32_t bpp = 0;
            surface_vkformat(t.format, &bpp);
            uint64_t buf = 0;
            // the finals and the frames after a structure change may use the
            // reserves; with every buffer busy, a final or a change frame's
            // comparison waits for one (the engine's work submitted first).
            // That wait is the walker's: where effects change often (the
            // owner's boss fight) the frames miss their pace.
            // DOTHEROES_PPFX_COMPARE_NOWAIT=1 (play sessions): no wait, the
            // comparison skipped (counted as without an engine copy).
            static int nowait = -1;
            if (nowait < 0) { const char* e = getenv("DOTHEROES_PPFX_COMPARE_NOWAIT"); nowait = e && *e == '1'; }
            const int prio = why == kCmpFinal ? 2 : why == kCmpChange ? 1 : 0;
            int slot = bpp ? slot_buffer((uint64_t)t.w * t.h * bpp, &buf, prio) : -1;
            if (slot < 0 && bpp && prio > 0 && !nowait) {
                const uint64_t t0 = bnow();
                rsx_vk_bridge_flush();
                while ((slot = slot_buffer((uint64_t)t.w * t.h * bpp, &buf, prio)) < 0 && bnow() - t0 < 10000000000ull)
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                st.cmp_slot_waits++;
                st.cmp_slot_wait_ns += bnow() - t0;
            } else if (slot < 0 && bpp && prio > 0) {
                st.cmp_slot_skips++;
            }
            if (slot >= 0 && !rsx_vk_bridge_copy_buffer(handles[0], (VkBuffer)buf, 0, t.w, t.h)) {
                r->walk.ref = (int16_t)slot; r->walk.ref_w = t.w; r->walk.ref_h = t.h;
                B.copies++;
            } else {
                if (slot >= 0) slot_release_buffer(slot);
                st.cmp_skip_no_ref++;
            }
        }
        finish(r);
        return;
    }
    if (!match(kClear, laps, io)) { foreign(locations, offsets, n, 0); return; }
    if (queue().at(B.w)->kind == kAspDraw) { foreign(locations, offsets, n, 0); return; }
    Rec* r = queue().at(B.w);
    for (uint32_t k = 0; k < kMaxNeeds; k++) r->walk.hand[k] = -1;
    r->walk.ref = -1; r->walk.sem = -1; r->walk.ordinal = -1;
    B.copies = 0;
    for (uint32_t i = 0; i < n; i++) {
        B.dirty.erase((uint64_t)locations[i] << 32 | offsets[i]);
        B.native.erase((uint64_t)locations[i] << 32 | offsets[i]);
    }
    finish(r);
}

}  // namespace

void bridge_install()
{
    const char* x = getenv("DOTHEROES_PPFX_XCHECK");
    B.xcheck = !(x && *x == '0');
    static rsx_engine_bridge b = { nullptr, on_draw_pre, on_wrote, on_marker, on_touch };
    const char* ph = getenv("DOTHEROES_PPFX_PRESENT_HASH");
    if (ph && *ph) {
        s_present_hash = fopen(ph, "w");
        if (s_present_hash) fprintf(s_present_hash, "# present frame_of_last_record last_record_bypassed hash size\n");
    }
    rsx_draw_engine_set_bridge(&b);
}

// R4's stream marker (the producer's thread, from the recorder): four words
// at the context's current position -- a non-incrementing NV4097 method
// 0x0100 header for three words, kMarkMagic, the sequence number's low and
// high halves -- reserved as libgcm's own reserve does: when they do not fit
// before the context's end, its callback first (here the runtime's own,
// cellGcm_fifo_recycle_n, the only one this title installs; another is not
// called: counted, and the record is missed).
bool bridge_marker(uint32_t ctx_ea, uint64_t seq)
{
    auto rd = [](uint32_t ea) { return (uint32_t)vm_base[ea] << 24 | (uint32_t)vm_base[ea + 1] << 16 |
                                       (uint32_t)vm_base[ea + 2] << 8 | vm_base[ea + 3]; };
    auto wr = [](uint32_t ea, uint32_t v) { vm_base[ea] = (uint8_t)(v >> 24); vm_base[ea + 1] = (uint8_t)(v >> 16);
                                            vm_base[ea + 2] = (uint8_t)(v >> 8); vm_base[ea + 3] = (uint8_t)v; };
    if (ctx_ea < 0x10000u || ctx_ea >= 0xE0000000u) { stats().marker_fail++; return false; }
    uint32_t cur = rd(ctx_ea + 8), end = rd(ctx_ea + 4);
    if (cur + 16 > end) {
        const uint32_t opd = rd(ctx_ea + 12);
        if (opd < 0x10000u || opd >= 0xE0000000u || rd(opd) != ppu_hle_inject_base + 0x2F00u) {
            stats().marker_fail++;
            return false;
        }
        cellGcm_fifo_recycle_n(ctx_ea, 4);
        cur = rd(ctx_ea + 8); end = rd(ctx_ea + 4);
        if (cur + 16 > end) { stats().marker_fail++; return false; }
        stats().marker_wraps++;
    }
    wr(cur, 0x400C0100u);           // NV4097 0x0100, 3 words, not incrementing (subchannel 0)
    wr(cur + 4, kMarkMagic);
    wr(cur + 8, (uint32_t)seq);
    wr(cur + 12, (uint32_t)(seq >> 32));
    wr(ctx_ea + 8, cur + 16);
    stats().markers_written++;
    return true;
}

// The cross-check's per-register aggregate, for the report.
void bridge_report(std::vector<std::string>& out)
{
    {
        std::lock_guard<std::mutex> g(B.xr_lock);
        for (auto& kv : B.hand_by) {
            char line[160];
            snprintf(line, sizeof line, "handoffs of %u:%08X: %llu", (uint32_t)(kv.first >> 32), (uint32_t)kv.first,
                     (unsigned long long)kv.second);
            out.push_back(line);
        }
    }
    char line[200];
    {
        std::lock_guard<std::mutex> g(s_fr_lock);
        for (auto& kv : s_foreign_reads) {
            snprintf(line, sizeof line, "other work's draws: %s: %llu draws", kv.first.c_str(), (unsigned long long)kv.second);
            out.push_back(line);
        }
    }
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
