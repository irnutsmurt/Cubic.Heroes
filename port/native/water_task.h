// The game's water SPU task (runtime image 1, lifted as
// build/gen/spu/spu_0000_at_00F47380) as native code. docs/SPU_NON_AUDIO.md
// section 3.
//
// What the task is (read in the lift and its PPU side, 2026-11-10):
// - a persistent SPURS task of the "Water" instance, created by the water's
//   set-up (func_003C7BA8 -> func_004736E0) with the argument
//   { u64 EA of a 24-byte parameter block, u32 EA of its event flag, 0 } at
//   block + 0x3D480 (block: a 0x3D600-byte allocation, the task's context save
//   area at +0, the event flag at +0x3D400, the argument at +0x3D480);
// - its main (LS 0x3B68) loops: cellSpursEventFlagWait(flag, 3, OR) (inlined,
//   LS 0x3F70: the task's four atomic sites, all on the flag's line); bit 1:
//   return (exit); bit 0: GET the 24 bytes, the height step (LS 0x3098), the
//   normal map (LS 0x35E0); loop. It tells the PPU nothing when it is done;
// - the PPU (func_003C7460(water, n, dt), the water's update) sets bit 0 once
//   per simulation step (when the frame time it accumulates passes a period
//   from TOC constants and the water's parameters + 0x30; the sum then reset
//   to 0), after a drop (1.0 at a random inner element of the layer the step
//   writes, when rand() % n == 0) and writing the block's index and scale;
//   then flips the water's index (+0x1EC). func_003C7AF4 (shutdown) sets bit 1
//   and waits for the task's exit code. Without the taskset (globals + 4 zero)
//   the PPU steps the heights itself (its own loop, row 127 wrapping to row
//   0) and makes no normal map.
#pragma once
#include <cstdint>

namespace water_task {

// The 24-byte block the task GETs each step (the PPU's water globals + 0x80).
struct Params {
    uint32_t heights = 0;   // +0x00 two layers of width x width floats; layer n at heights + 4*width*width*n
    uint32_t texture = 0;   // +0x04 the normal map: width x width 32-bit pixels, pitch 4*width (RSX local memory)
    int32_t  width = 0;     // +0x08 128 in the game
    uint32_t index = 0;     // +0x0C the layer this step writes; the other one (1 - index) is read
    uint32_t damping = 0;   // +0x10 float bits (the game's constant at TOC - 0x3768)
    uint32_t scale = 0;     // +0x14 float bits: slope to normal (the water's parameters + 0x2C, per step)
};
constexpr uint32_t kParamBytes = 24;
Params parse_params(const uint8_t bytes[kParamBytes]);   // big-endian, as in guest memory

// The game's block around the task (func_003C7BA8, func_00473668).
constexpr uint32_t kBlockFlag = 0x3D400, kBlockArgument = 0x3D480;

// Where the task's transfers go. A GET copies guest bytes into the task's
// buffers, a PUT copies them back -- in the task's order, so a destination a
// later transfer overwrites, or a row a block leaves for the next, is as the
// task has it.
struct Io {
    void* user;
    void (*get)(void* user, void* dst, uint32_t ea, uint32_t bytes);
    void (*put)(void* user, uint32_t ea, const void* src, uint32_t bytes);
};
// Directly on a guest address space laid out at `base` (vm_base in the game).
Io guest_io(uint8_t* base);

// The task's local-store buffers this code reproduces: the other layer's rows
// (LS 0x4610, ten rows a block: the row above, the block's eight, the row
// below) and the rows a block computes (LS 0x8610: heights, then pixels).
struct Local {
    uint8_t in[0x4000];
    uint8_t out[0x4000];
};

enum class Status { done, nothing, unsupported };

// nullptr when run_frame reproduces the task for these parameters; else why
// not (the task would halt: an address not 16-byte aligned, a width not a
// multiple of 4; or its buffers would overlap: width > 408; or an index other
// than 0 and 1, which the game never passes).
const char* unsupported_reason(const Params& p, uint32_t param_ea);

// One step of the task, as it does it after waking: the 24 bytes at
// `param_ea` read through `io`, then
// 1. the height step, block by block of 8 rows: for each element of the
//    `index` layer, ((left + right + up + down) of the other layer * 0.5 -
//    itself) * damping, left/right wrapping round the row; up/down from the
//    rows the task holds -- the row above row 0 is the last row, but the row
//    below the last block's last row is the last block's first row, not row 0
//    (the task fetches row 0 into the buffer's ninth row and its own block's
//    rows overwrite it; the tenth keeps the previous block's);
// 2. the normal map from the other layer (the previous step's heights), same
//    rows: n = (dx * scale, dy * scale, 1) normalised (frsqest, fi and a Newton
//    step for the length; frest, fi and a Newton step for 1/length),
//    dx = right - left, dy = up - down; pixel = 0x00RRGGBB with
//    R, G, B = cfltu((n * 0.5 + 0.5) * 255) of x, y, z.
// All arithmetic in the SPU's single precision through the runtime's exact
// helpers (spu_helpers.h: truncation, denormals as zero, extended range), in
// the task's operation order. `used`, when given, gets the parameters read.
Status run_frame(uint32_t param_ea, const Io& io, Local& ls, Params* used = nullptr);

// The two halves, for checking each alone.
void height_step(const Params& p, const Io& io, Local& ls);
void normal_map(const Params& p, const Io& io, Local& ls);

}  // namespace water_task

// The game's side (water_task.cpp), for an entry hook on func_00473668(r3 =
// block, r4 = bits), the water's cellSpursEventFlagSet(block + 0x3D400, bits).
// 1: the step was done (or queued) natively and the flag is not to be set (the
// hook then returns r3 = 0, as the set does); 0: decline, the lifted function
// sets the flag as before. DOTHEROES_NATIVE_WATER (read by water_task_init):
//   unset/0  lifted (the hook declines everything; captures still work);
//   1        native, on a worker thread of its own (the task's asynchrony: the
//            PPU awaits nothing; signals while a step runs coalesce into one
//            more run, as the event flag's bits do);
//   sync     native, on the signalling thread before it returns;
//   check    lifted, and the native step into scratch beside it: inputs taken
//            at the signal, compared with the lifted task's results at the
//            next signal when the task is back in its wait.
// The quit (bit 1) is always declined, after the native worker is idle.
extern "C" int g_water_hook;                  // the hook's guard: 1 when any mode or capture is on
extern "C" void water_task_init(void);        // at start-up, beside dotheroes_native_init
extern "C" void water_task_report(void);      // the counts, once (the test stop and atexit)
extern "C" int water_task_signal(uint32_t block, uint32_t bits);
// What it has done so far (tests; the exit report prints the same).
struct WaterTaskCounts {
    uint64_t signals, steps, coalesced, unsupported, shape;   // shape: blocks not built as the game builds them
    uint64_t checked, equal, busy;                            // check mode
};
extern "C" void water_task_counts(WaterTaskCounts* out);
