// Native PPFX (docs/NATIVE_R3.md, docs/NATIVE_R3B.md): what the recorder
// (ppfx_recorder.cpp, the game's thread), the bridge (ppfx_bridge.cpp, the
// FIFO walker's thread) and the native renderer (ppfx_render.cpp, a thread of
// its own) share.
//
// A record is one PPFX operation -- a draw or a clear -- with an explicit
// description of everything it uses: its colour target, viewport and
// scissor, both programs by content with their parameters, the vertex
// program's constants, its vertex inputs and their bytes, the textures and
// samplers its fragment program reads, and the render state. Each item is
// what a call's arguments said (CellGcmSurface, CellGcmTexture, a
// cellGcmSetTextureAddress's wrap modes, ...), kept in libgcm's own vocabulary
// (its CELL_GCM_* constants and structure fields); none of it is a register
// word. State PPFX's draws use but PPFX did not set comes from whoever set it
// on the same thread: the game's own libgcm calls, or asp's state API
// (docs/NATIVE_R3B.md section 1).
//
// Records go through one bounded queue, in the order PPFX made them:
//   put    the recorder has published records [0, put)
//   walked the walker has reached each of [0, walked) in its own stream
//          (matched it, decided and recorded its handoffs) -- or found it
//          missing
//   done   the native renderer has submitted [0, done); a slot is free again
// The temporary bridge between the records and the FIFO path (the walker's
// matching, the handoffs) is ppfx_bridge.cpp alone; with every graphics
// producer native it goes, and the queue's `walked` with it.
//
// R4 (docs/NATIVE_R4.md, DOTHEROES_PPFX_BYPASS): the libgcm calls whose work
// the records carry do not run for PPFX -- no command of theirs reaches the
// stream, the engine draws none of PPFX's draws -- and the native renderer's
// output is what the rest of the frame sees. A bypassed record reaches the
// walker by a stream marker the recorder writes where its commands would
// have been; the bridge hands the native output back to the engine's
// surface before the engine next reads or writes it (a handback).
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <string>
#include <vector>

namespace ppfx {

// ---- the explicit state -------------------------------------------------------

// Who set an item last: PPFX, the game's own libgcm calls, asp's state API,
// or nobody (the GPU's reset state).
enum By : uint8_t { kByReset = 0, kByPpfx = 1, kByGame = 2, kByAsp = 3 };

struct Surface {            // cellGcmSetSurface: its CellGcmSurface
    uint8_t type, antialias, color_format, color_target, depth_format, depth_location;
    uint8_t color_location[4];
    uint32_t color_offset[4], color_pitch[4], depth_offset, depth_pitch;
    uint16_t width, height, x, y;
};
struct Viewport {           // cellGcmSetViewport
    uint16_t x, y, w, h;
    float zmin, zmax, scale[4], offset[4];
};
struct Scissor { uint16_t x, y, w, h; };   // cellGcmSetScissor
struct Texture {            // cellGcmSetTexture: its CellGcmTexture
    uint8_t format, mipmap, dimension, cubemap, location;
    uint32_t remap;         // the component crossbar, as CellGcmTexture.remap gives it
    uint16_t width, height, depth;
    uint32_t pitch, offset;
};
struct Sampler {
    // cellGcmSetTextureControl
    uint8_t enable, max_aniso; uint16_t min_lod, max_lod;          // lod: 4.8 fixed point
    // cellGcmSetTextureFilter
    uint8_t min_filter, mag_filter, conv; uint16_t bias;           // bias: 13-bit fixed point
    uint8_t filter_signed;                                         // the signed components (none by this API)
    // cellGcmSetTextureAddress
    uint8_t wrap_s, wrap_t, wrap_r, unsigned_remap, zfunc, gamma;
    uint32_t border;        // cellGcmSetTextureBorderColor (ARGB)
    uint8_t address_by;     // By
};
struct VertexInput {        // cellGcmSetVertexDataArray
    uint8_t size, type, stride, location;
    uint16_t frequency;
    uint32_t offset;
};
struct FragmentProgram {    // the bind (CgBinaryProgram, offset, location) and its shader control
    uint32_t offset; uint8_t location;
    uint32_t input_mask;    // CgBinaryFragmentProgram.attributeInputMask
    uint16_t tex_mask, tex_2d, tex_centroid;   // texCoordsInputMask, texCoords2D, texCoordsCentroid
    uint8_t reg_count, depth_replace, output_from_h0, pixel_kill, txp;
    uint8_t gamma;          // cellGcmSetFragmentProgramGammaEnable: the shader packer's sRGB output
};
struct VertexProgram {      // cellGcmSetVertexProgram: its CgBinaryVertexProgram
    uint16_t start;         // instructionSlot
    uint32_t input_mask;    // attributeInputMask
    uint8_t regs_le32;      // registerCount <= 32
};
struct Raster {             // the render state, as the calls gave it
    uint32_t color_mask;    // cellGcmSetColorMask's CELL_GCM_COLOR_MASK_{B,G,R,A} bits
    uint8_t color_mask_by;  // By
    uint8_t blend, alpha_test, depth_test, depth_write, stencil_test, cull, logic_op, dither;
    uint8_t poly_offset_fill, line_smooth, poly_smooth, poly_stipple, line_stipple, two_side_light;
    uint16_t cull_face, front_face, depth_func, shade_model, front_poly_mode, back_poly_mode;
    float poly_offset_factor, poly_offset_units;
    uint8_t aa_enable, alpha_to_coverage, alpha_to_one; uint16_t sample_mask;
    // occlusion queries (cellGcmSetZpassPixelCountEnable, cellGcmSetClearReport,
    // cellGcmSetReport, cellGcmSetRenderEnable): not rendering state, kept for
    // R4's accounting of PPFX's side effects
    uint8_t zpass_count, render_enable_mode;
    // R5: what asp's draws set besides (PPFX turns these off: blend, the
    // alpha and stencil tests). The libgcm constants (CELL_GCM_*), as the
    // calls' arguments give them.
    uint16_t blend_sf_rgb, blend_df_rgb, blend_sf_a, blend_df_a, blend_eq_rgb, blend_eq_a;   // cellGcmSetBlendFunc / Equation
    uint16_t alpha_func; uint8_t alpha_ref;                    // cellGcmSetAlphaFunc
    uint16_t stencil_func, stencil_fail, stencil_zfail, stencil_zpass;   // cellGcmSetStencilFunc / Op
    uint8_t stencil_ref, stencil_func_mask, stencil_write_mask;
};
struct VertexData {
    uint32_t base_offset, base_index;   // cellGcmSetVertexDataBase
    uint16_t divider_op;                // cellGcmSetFrequencyDividerOperation
    float current[16][4];               // a disabled input's value (never set by PPFX: the reset (0,0,0,1))
};

// Everything a record draws with. Copied whole into each record.
struct State {
    Surface surface;
    Viewport viewport;
    Scissor scissor;
    FragmentProgram fp;
    VertexProgram vp;
    Texture tex[16];
    Sampler samp[16];
    VertexInput vin[16];
    VertexData vdata;
    Raster raster;
    uint32_t clear_color;   // cellGcmSetClearColor (ARGB)
    uint32_t clear_depth;   // cellGcmSetClearDepthStencil
};
void reset_state(State* s);   // the GPU's reset state

enum { kVpWords = 544 * 4 };

// Immutable bytes known by their content: a program.
struct Blob {
    uint64_t hash;
    std::vector<uint8_t> bytes;
};
using BlobP = std::shared_ptr<const Blob>;
uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull);

// ---- records ---------------------------------------------------------------

// kAspDraw (R5, docs/NATIVE_R4.md section 10): one of asp's draws, described from
// asp's own methods; checked against the engine's state at that draw, not rendered yet
enum Kind : uint32_t { kDraw = 1, kClear = 2, kAspDraw = 3 };

// A guest surface a record reads (a texture unit) or writes (its colour
// target), as the recorder saw it.
struct Need {
    uint32_t location, offset;     // RSX_LOCATION_*, offset
    uint32_t w, h, pitch;          // the extent the record uses
    uint32_t format;               // texture: the format byte; target: the surface colour format
    uint8_t unit;                  // 0..15 a texture unit, 0xFF the colour target, 0xFE the depth target (R5)
    uint8_t ppfx_wrote;            // an earlier record wrote this address (the native side has it)
};
enum { kMaxNeeds = 17 };

// A snapshot of guest bytes a draw's vertex arrays read.
struct VtxRange { uint32_t location, offset, bytes, at; };

// What the walker decided for a record, read by the native renderer once
// `walked` has passed it. Slots are the native renderer's (slot_* below):
// the walker copies the engine's surfaces into them in the engine's own
// command stream, and one semaphore, signalled by the engine's submission of
// those copies, orders the native renderer's use after them.
struct Walk {
    uint8_t matched;               // 1 the walker reached it; 0 it never did (counted)
    int16_t hand[kMaxNeeds];       // per need: the slot holding the engine's copy (handoff), or -1
    int16_t ref;                   // the engine's output of this record (a buffer slot, comparison), or -1
    uint32_t ref_w, ref_h;         // its extent
    int16_t sem;                   // the semaphore slot to wait on, or -1
    int64_t ordinal;               // that signal's place in the engine's signal order
    uint8_t want_cmp;              // the comparison asked for this record's output ...
    uint8_t cmp_why;               // ... why (kCmp*), and whether the engine's copy was made
    // R4: native semaphores the engine waited on for handbacks, free again
    // once this record's batch (which waits for an engine signal after those
    // waits) has completed; -1 none
    int16_t release_sem[4];
    // dev cross-check: state items that differ from the engine's at the matched draw
    uint32_t diff_regs, diff_consts, diff_vp, diff_fp, diff_vtx;
};
enum CmpWhy : uint8_t { kCmpNone = 0, kCmpFinal = 1, kCmpEvery = 2, kCmpChange = 3 };

struct Rec {
    uint64_t seq;                  // monotonic: the record's place in PPFX's order
    uint32_t kind;
    uint32_t frame;                // the game's flips before it
    // Where its commands go: the title's ring lap and the IO of the first
    // command, as the call began -- or, when the call's own reserve wraps the
    // ring, lap + 1 at io_alt (the ring's start). The walker tells which.
    uint32_t lap, io, io_alt;
    uint32_t lap_end, io_end;      // kAspDraw: where its commands ended (its draw's return)
    uint32_t why;                  // 0, or a reason the record is not drawable (kWhy*)
    State st;                      // what it draws with
    uint32_t vconst[512][4];       // the vertex program's constants
    uint32_t vconst_written[16];   // bit per constant ever set (512 bits)
    BlobP vp;                      // vertex program words from its start slot, through its last instruction
    BlobP fp;                      // fragment program bytes as the GPU will read them at this draw
    // the operation
    uint32_t prim, first, count;   // draw arrays (kAspDraw: count is the index count, or the vertex count)
    // kAspDraw (R5): the draw object's index buffer (its address; 0 none,
    // DRAW_ARRAYS) and index type (2: 16-bit), and its vertex count: the
    // vertices [0, vcount) of each enabled stream, read from guest memory
    // by the native renderer (vr[] give the streams' ranges, no bytes)
    uint32_t idx_ea, idx_type, vcount;
    uint64_t src_hash;             // R5 (DOTHEROES_ASP_RACE_CHECK=1): its vertex and index bytes' hash when the game drew it
    // kAspDraw (R5): asp's clears since its previous draw, done before this
    // draw (each on the surface it was made with): CELL_GCM_CLEAR_* bits,
    // the colour (ARGB8, or four floats for a float target), depth (24-bit) and stencil
    struct AspClear { uint32_t mask, argb, depth, stencil; float rgba[4]; Surface surface; } aclear[32];
    uint32_t naclear;
    uint32_t clear_mask;           // clear
    VtxRange vr[16];
    uint32_t nvr;
    std::vector<uint8_t> vdata;
    Need need[kMaxNeeds];
    uint32_t nneed;
    // a guest-memory texture's bytes as the recorder saw them (per unit,
    // only for units whose address no record wrote): small static textures;
    // tex[k].location is the unit
    VtxRange tex[16];
    uint32_t ntex;
    std::vector<uint8_t> tdata;
    uint8_t bypassed;              // R4: its call's body did not run; the walker meets it at its marker
    Walk walk;
};

enum Why : uint32_t {
    kWhyNone = 0,
    kWhyNoVp = 1, kWhyNoFp = 2, kWhyVertex = 3, kWhyUnsupported = 4,
};

// ---- the queue -------------------------------------------------------------

struct Queue {
    enum { kCap = 4096 };   // R5: asp's records too (about 250 a river frame)
    Rec* slot[kCap];
    std::atomic<uint64_t> put{0}, walked{0}, done{0};
    std::mutex m;
    std::condition_variable cv;    // any of the three moved
    // backpressure: how often and how long the producer waited for room
    std::atomic<uint64_t> full_waits{0}, full_wait_us{0};
    Rec* at(uint64_t i) { return slot[i % kCap]; }
};
Queue& queue();

// Which parts run (DOTHEROES_PPFX_NATIVE): 0 off; 1 records, the walker's
// match and cross-check (no native rendering: "check"); 2 everything.
int mode();

// The bridge, on the walker's thread (ppfx_bridge.cpp): installed by the
// recorder's start-up when the mode asks for it.
void bridge_install();
void bridge_report(std::vector<std::string>& out);   // the cross-check's aggregate
void render_report(std::vector<std::string>& out);   // comparisons per chain variant
// A frame in which the cross-check found a PPFX draw using state that
// differs from the engine's (the walker's thread): its comparisons'
// differences are counted apart from the others.
void frame_flag_inherited(uint32_t frame);
// The native renderer's start (ppfx_render.cpp), mode 2.
void render_start();
// The native renderer's end: everything recorded is submitted and every
// comparison asked for has run (or is counted as unfinished). At exit.
void render_drain(uint32_t timeout_ms);

// The native renderer's resources the bridge copies into (walker thread).
// Each returns a slot index (or -1: none to be had) and the Vulkan handle; a
// slot is the native renderer's again once its work with it has completed.
bool render_ready();
int slot_image(uint32_t vkformat, uint32_t w, uint32_t h, uint64_t* image);
// A buffer for the engine's output; `prio` 0 sampled, 1 a structure-change
// frame's, 2 a final composite: the higher may use the reserves.
int slot_buffer(uint64_t bytes, uint64_t* buffer, int prio);
int slot_semaphore(uint64_t* semaphore);
void slot_release_semaphore(int slot);   // taken but not used
void slot_release_image(int slot);
void slot_release_buffer(int slot);
// Which records' outputs the walker copies for comparison (kCmp*, or 0).
int compare_wanted(const Rec* r);

// R4's handback (walker thread): the native renderer finishes everything
// walked, puts the native image of the surface at (location, offset) in a
// layout the engine copies from, and submits that with a semaphore to
// signal. Blocks until submitted (or timeout_ms). check: the image is also
// read back in the same batch (*check_rows: its rows once that batch is
// done, *check_slot: give it back with handback_check_done). 0 when done.
struct HandOut {
    uint64_t image;        // VkImage
    uint32_t layout;       // VkImageLayout it is in
    uint32_t w, h, vkformat, bpp;
    int sem_slot;          // the semaphore the native submission signals (a slot)
    uint64_t sem;          // VkSemaphore
    const uint8_t* check_rows;
    int check_slot;
};
int handback_take(uint32_t location, uint32_t offset, bool check, uint32_t timeout_ms, HandOut* out);
void handback_check_done(int check_slot);
// rsx_be_format the engine keeps a cellGcmSetSurface colour format in
uint32_t surface_beformat(uint32_t color_format);
// R4's stream marker (ppfx_bridge.cpp, the producer's thread): written into
// the command buffer of the context at ctx_ea where a bypassed record's
// commands would have gone. false when it could not be written (counted).
bool bridge_marker(uint32_t ctx_ea, uint64_t seq);
// The VkFormat the engine keeps a colour surface of this cellGcmSetSurface
// colour format in (the engine's eng_surface_format, the backend's table).
uint32_t surface_vkformat(uint32_t color_format, uint32_t* bpp);
// The VkFormat of an rsx_be_format (the backend's table), 0 when none.
uint32_t be_vkformat_pub(uint32_t be_format, uint32_t* bpp);

// Counters every part adds to, reported every 10 s and at exit
// (DOTHEROES_PPFX_REPORT, default ppfx_report.txt).
struct Stats {
    std::atomic<uint64_t> calls{0}, ppfx_calls{0}, nested_ppfx{0}, foreign_state_calls{0}, other_thread_calls{0};
    std::atomic<uint64_t> unmodelled{0};          // PPFX calls with commands the recorder has no encoder for
    std::atomic<uint64_t> local_copies{0};        // the game's copies into local memory seen
    std::atomic<uint64_t> asp_calls{0}, asp_restores{0}, asp_store_mismatch{0};   // asp's state API seen
    std::atomic<uint64_t> transfers{0}, reports{0};   // PPFX's report copies and report requests (side effects)
    std::atomic<uint64_t> draws{0}, clears{0}, depth_clears{0}, frames{0};
    std::atomic<uint64_t> producer_threads{0};    // > 1: the single-producer assumption broke
    std::atomic<uint64_t> flushes_for_room{0};    // the ring's put moved forward for a full queue (R5)
    std::atomic<uint64_t> wraps_in_call{0};       // records the walker found at their wrapped position
    std::atomic<uint64_t> ends_at_ring_end{0};    // records whose commands ended at the ring's end
    std::atomic<uint64_t> rec_ns{0};              // the recorder's own time on the game's thread
    std::atomic<uint64_t> matched{0}, missed{0}, foreign_draws{0}, foreign_clears{0};
    std::atomic<uint64_t> lap_delta_changes{0};
    std::atomic<uint64_t> xc_draws{0}, xc_regs{0}, xc_consts{0}, xc_vp{0}, xc_fp{0}, xc_vtx{0}, xc_tex{0};
    std::atomic<uint64_t> handoffs{0}, handoff_fail{0};
    std::atomic<uint64_t> handoffs_shared{0};   // R5: handoffs not needed: asp drew the surface natively
    // the native renderer
    std::atomic<uint64_t> n_draws{0}, n_clears{0}, n_skipped{0}, n_unsupported{0}, n_batches{0};
    std::atomic<uint64_t> n_pipelines{0}, n_images{0}, n_image_bytes{0}, n_static_textures{0}, n_feedback{0};
    std::atomic<uint64_t> n_frames{0};    // frames with PPFX records (the per-frame figures' denominator)
    std::atomic<uint64_t> n_asp_draws{0}, n_asp_vertex_bytes{0}, n_asp_clears{0};   // R5: asp's records drawn natively
    std::atomic<uint64_t> asp_race_checked{0}, asp_race_changed{0};   // R5: guest bytes changed between the record and its native draw
    std::atomic<uint64_t> n_retired{0};   // images replaced at an address (another size or format) and destroyed later
    std::atomic<uint64_t> n_barriers{0}, n_barriers_full{0}, n_handoff_copies{0}, n_snapshot_copies{0};
    // comparisons: wanted = asked for; then exactly one of equal, differ,
    // skipped_* (never compared, and why) or unfinished (in flight at the end)
    std::atomic<uint64_t> cmp_wanted{0}, cmp_records{0}, cmp_equal{0}, cmp_differ{0};
    std::atomic<uint64_t> cmp_skip_no_ref{0}, cmp_skip_no_readback{0}, cmp_skip_extent{0}, cmp_skip_not_drawn{0};
    std::atomic<uint64_t> cmp_unfinished{0};
    std::atomic<uint64_t> cmp_change_wanted{0}, cmp_change_done{0};   // of them, structure-change frames'
    std::atomic<uint64_t> cmp_differ_inherited{0};   // differences in frames the cross-check flagged
    std::atomic<uint64_t> cmp_finals{0}, cmp_finals_equal{0}, cmp_triggers{0}, cmp_queue_waits{0}, cmp_queue_wait_ns{0};
    // the walker waiting for a comparison's readback buffer (a final's or a
    // change frame's), and with DOTHEROES_PPFX_COMPARE_NOWAIT=1 the ones skipped instead
    std::atomic<uint64_t> cmp_slot_waits{0}, cmp_slot_wait_ns{0}, cmp_slot_skips{0};
    std::atomic<uint64_t> hand_no_slot{0}, hand_copy_fail{0}, hand_slot_waits{0}, hand_slot_wait_ns{0};
    // textures taken from guest memory by the walker, at the record's place in the stream
    std::atomic<uint64_t> guest_tex{0}, guest_tex_bytes{0}, guest_tex_fail{0};
    std::atomic<uint64_t> wait_walker_us{0}, wait_signal_us{0}, gpu_ns{0}, cpu_record_ns{0}, compare_ns{0};
    std::atomic<uint64_t> gpu_draw_ns{0}, gpu_copy_ns{0}, gpu_readback_ns{0}, gpu_other_ns{0};   // the batches' timestamps, by section
    std::atomic<uint64_t> bridge_ns{0}, xcheck_ns{0};   // the walker's thread: the bridge's callbacks, the cross-check in them
    // R4: the bypass (the recorder), the markers and handbacks (the bridge)
    std::atomic<uint64_t> r4_frames{0}, r4_records{0}, r4_skipped{0}, r4_kept{0}, r4_exit_mismatch{0};
    std::atomic<uint64_t> markers_written{0}, marker_wraps{0}, marker_fail{0};
    std::atomic<uint64_t> markers_met{0}, marker_unknown{0}, marker_r3_matched{0}, matched_r3_in_r4{0};
    std::atomic<uint64_t> handbacks{0}, handback_fail{0}, handback_ns{0}, handback_wait_ns{0};
    std::atomic<uint64_t> handbacks_by_kind[6];   // 0 a draw's target, 1 texture, 2 transfer/guest, 3 present, 4 clear, 5 other
    std::atomic<uint64_t> hb_checked{0}, hb_equal{0}, hb_differ{0};
    std::atomic<uint64_t> present_hashes{0};
    // R5: asp's records
    std::atomic<uint64_t> asp_records{0}, asp_matched{0}, asp_missed{0}, asp_xc_draws{0}, asp_xc_differ{0};
    std::atomic<uint64_t> asp_draw_events{0};   // engine draw events inside a matched asp draw's commands
    std::atomic<uint64_t> asp_group_restores{0};   // asp's state items restored from its store by a re-emission
    std::atomic<uint64_t> game_draw_records{0};    // the game's own indexed draws recorded with asp's (R5)
    std::atomic<uint64_t> asp_tex_snapshots{0};    // small textures taken as the game drew (R5)
    std::atomic<uint64_t> asp_bypassed{0}, asp_bypassed_words{0}, asp_not_bypassed{0}, asp_marker_fail{0};   // R5's bypass
    std::atomic<uint64_t> asp_state_bypassed{0}, asp_state_words{0}, asp_state_kept{0};   // R5's bypass, stage 2
};
Stats& stats();
void report_add(const char* line);   // a line in the next report (diagnostics)

}  // namespace ppfx
