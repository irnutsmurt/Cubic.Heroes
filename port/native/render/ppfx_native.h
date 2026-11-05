// R3's native PPFX (docs/NATIVE_R3.md): what the recorder (ppfx_recorder.cpp,
// the game's thread), the bridge (ppfx_bridge.cpp, the FIFO walker's thread)
// and the native renderer (ppfx_render.cpp, a thread of its own) share.
//
// A record is one PPFX operation -- a draw or a clear -- made from PPFX's own
// libgcm calls: the state they set, kept by the recorder in a private block
// laid out like the RSX register file (the layout the shader translators and
// the vertex fetch read; nothing else writes it), the vertex program's
// constants, both programs by content, and the vertex data the draw reads.
// Records go through one bounded queue, in the order PPFX made them:
//   put    the recorder has published records [0, put)
//   walked the walker has reached each of [0, walked) in its own stream
//          (matched it, decided and recorded its handoffs) -- or found it
//          missing
//   done   the native renderer has submitted [0, done); a slot is free again
// The temporary bridge between the records and the FIFO path (the walker's
// matching, the handoffs) is ppfx_bridge.cpp alone; with every graphics
// producer native it goes, and the queue's `walked` with it.
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <string>
#include <vector>

namespace ppfx {

// ---- the state block -------------------------------------------------------

// The register ranges the block covers (method addresses, [lo, hi)). Every
// encoder writes inside them; a record carries their values.
struct RegRange { uint32_t lo, hi; };
extern const RegRange kRanges[];
extern const uint32_t kNRanges;
uint32_t reg_count();                 // words in a record's register snapshot
uint32_t reg_index(uint32_t method);  // its index there, or ~0u outside the ranges
uint32_t reg_method(uint32_t index);
bool reg_written(uint32_t method);    // the recorder has written it (since the start)

enum { kMaxRegs = 1024, kVpWords = 544 * 4 };

// Immutable bytes known by their content: a program.
struct Blob {
    uint64_t hash;
    std::vector<uint8_t> bytes;
};
using BlobP = std::shared_ptr<const Blob>;
uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull);

// ---- records ---------------------------------------------------------------

enum Kind : uint32_t { kDraw = 1, kClear = 2 };

// A guest surface a record reads (a texture unit) or writes (its colour
// target), as the recorder saw it.
struct Need {
    uint32_t location, offset;     // RSX_LOCATION_*, offset
    uint32_t w, h, pitch;          // the extent the record uses
    uint32_t format;               // texture: the format byte; target: the surface colour format
    uint8_t unit;                  // 0..15 a texture unit, 0xFF the colour target
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
    // dev cross-check: state items that differ from the engine's at the matched draw
    uint32_t diff_regs, diff_consts, diff_vp, diff_fp, diff_vtx;
};

struct Rec {
    uint64_t seq;                  // monotonic: the record's place in PPFX's order
    uint32_t kind;
    uint32_t frame;                // the game's flips before it
    // Where its commands go: the title's ring lap and the IO of the first
    // command, as the call began -- or, when the call's own reserve wraps the
    // ring, lap + 1 at io_alt (the ring's start). The walker tells which.
    uint32_t lap, io, io_alt;
    uint32_t why;                  // 0, or a reason the record is not drawable (kWhy*)
    // state
    uint32_t regs[kMaxRegs];       // the block's ranges (reg_index)
    uint32_t vconst[512][4];
    uint32_t vconst_written[16];   // bit per constant ever written by the recorder (512 bits)
    BlobP vp;                      // vertex program words from its start slot, through its last instruction
    uint32_t vp_start;
    BlobP fp;                      // fragment program bytes as the GPU will read them at this draw
    uint32_t fp_off, fp_loc;
    // the operation
    uint32_t prim, first, count;   // draw arrays
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
    Walk walk;
};

enum Why : uint32_t {
    kWhyNone = 0,
    kWhyNoVp = 1, kWhyNoFp = 2, kWhyVertex = 3, kWhyUnsupported = 4,
};

// ---- the queue -------------------------------------------------------------

struct Queue {
    enum { kCap = 1024 };
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
// A frame in which the cross-check found a PPFX draw using state PPFX did
// not set (the walker's thread): its comparisons' differences are counted
// apart from the others.
void frame_flag_inherited(uint32_t frame);
// The native renderer's start (ppfx_render.cpp), mode 2.
void render_start();

// The native renderer's resources the bridge copies into (walker thread).
// Each returns a slot index (or -1: none to be had) and the Vulkan handle; a
// slot is the native renderer's again once its work with it has completed.
bool render_ready();
int slot_image(uint32_t vkformat, uint32_t w, uint32_t h, uint64_t* image);
int slot_buffer(uint64_t bytes, uint64_t* buffer, bool final_);   // a final composite may use the reserve
int slot_semaphore(uint64_t* semaphore);
void slot_release_semaphore(int slot);   // taken but not used
void slot_release_image(int slot);
void slot_release_buffer(int slot);
// Which records' outputs the walker copies for comparison.
bool compare_wanted(const Rec* r);
// The VkFormat the engine keeps a colour surface of this SET_SURFACE_FORMAT
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
    std::atomic<uint64_t> draws{0}, clears{0}, depth_clears{0}, frames{0};
    std::atomic<uint64_t> producer_threads{0};    // > 1: the single-producer assumption broke
    std::atomic<uint64_t> wraps_in_call{0};       // records the walker found at their wrapped position
    std::atomic<uint64_t> ends_at_ring_end{0};    // records whose commands ended at the ring's end
    std::atomic<uint64_t> rec_ns{0};              // the recorder's own time on the game's thread
    std::atomic<uint64_t> matched{0}, missed{0}, foreign_draws{0}, foreign_clears{0};
    std::atomic<uint64_t> lap_delta_changes{0};
    std::atomic<uint64_t> xc_draws{0}, xc_regs{0}, xc_consts{0}, xc_vp{0}, xc_fp{0}, xc_vtx{0}, xc_tex{0};
    std::atomic<uint64_t> handoffs{0}, handoff_fail{0};
    // the native renderer
    std::atomic<uint64_t> n_draws{0}, n_clears{0}, n_skipped{0}, n_unsupported{0}, n_batches{0};
    std::atomic<uint64_t> n_pipelines{0}, n_images{0}, n_image_bytes{0}, n_static_textures{0}, n_feedback{0};
    std::atomic<uint64_t> n_retired{0};   // images replaced at an address (another size or format) and destroyed later
    std::atomic<uint64_t> cmp_records{0}, cmp_equal{0}, cmp_differ{0}, cmp_skipped{0};
    std::atomic<uint64_t> cmp_differ_inherited{0};   // differences in frames with inherited state
    std::atomic<uint64_t> cmp_finals{0}, cmp_finals_equal{0}, cmp_triggers{0}, cmp_queue_waits{0}, cmp_queue_wait_ns{0};
    std::atomic<uint64_t> cmp_no_slot{0}, hand_no_slot{0}, hand_copy_fail{0}, hand_slot_waits{0}, hand_slot_wait_ns{0};
    std::atomic<uint64_t> wait_walker_us{0}, wait_signal_us{0}, gpu_ns{0}, cpu_record_ns{0}, compare_ns{0};
    std::atomic<uint64_t> bridge_ns{0}, xcheck_ns{0};   // the walker's thread: the bridge's callbacks, the cross-check in them
};
Stats& stats();
void report_add(const char* line);   // a line in the next report (diagnostics)

}  // namespace ppfx
