// The game's water SPU task (runtime image 1, lifted by the setup as build/gen/spu/spu_0000_at_00F47380) run as
// native code without SPURS or an SPU thread. docs/SPU_NON_AUDIO.md section 3.
//
// The task's own code is not here: it is generated on the player's machine from the player's lift
// (water_gen, port/native/spu_native.h; tools/spu_native/water.py). This file is the host around it: what
// the task's main does between its waits (the step's 24 bytes into local store, its two functions called),
// the transfers done at once on guest memory, and the game's side (its signal, set-up and shutdown).
//
// What the task is, as the game uses it (read in the lift and its PPU side, 2026-11-10):
// - a persistent SPURS task of the "Water" instance, created by the water's set-up (func_003C7BA8 ->
//   func_004736E0) with the argument { u64 EA of a 24-byte parameter block, u32 EA of its event flag, 0 } at
//   block + 0x3D480 (block: a 0x3D600-byte allocation, the task's context save area at +0, the event flag at
//   +0x3D400, the argument at +0x3D480);
// - it waits on the flag; bit 1: it exits; bit 0: one simulation step from the 24 bytes (the heights of the
//   layer the step writes, from the other layer; then the normal map's texture), then it waits again. It
//   tells the PPU nothing when it is done;
// - the PPU (func_003C7460(water, n, dt), the water's update) sets bit 0 once per simulation step (when the
//   frame time it accumulates passes a period), after a drop and writing the block's index and scale; then
//   flips the water's index (+0x1EC). func_003C7AF4 (shutdown) sets bit 1 and waits for the task's exit code.
#pragma once
#include <cstdint>

#include "spu_native.h"

namespace water_task {

// The 24-byte block the task GETs each step (the PPU's water globals + 0x80).
struct Params {
    uint32_t heights = 0;   // +0x00 two layers of width x width floats; layer n at heights + 4*width*width*n
    uint32_t texture = 0;   // +0x04 the normal map: width x width 32-bit pixels, pitch 4*width (RSX local memory)
    int32_t  width = 0;     // +0x08 128 in the game
    uint32_t index = 0;     // +0x0C the layer this step writes; the other one (1 - index) is read
    uint32_t damping = 0;   // +0x10 float bits
    uint32_t scale = 0;     // +0x14 float bits (the water's parameters + 0x2C, per step)
};
constexpr uint32_t kParamBytes = 24;
Params parse_params(const uint8_t bytes[kParamBytes]);   // big-endian, as in guest memory

// The game's block around the task (func_003C7BA8, func_00473668).
constexpr uint32_t kBlockFlag = 0x3D400, kBlockArgument = 0x3D480;

// Where the task's transfers go: a GET copies guest bytes into local store, a PUT copies them back, each as
// it is issued (the runtime's order).
struct Io {
    void* user;
    void (*get)(void* user, void* dst, uint32_t ea, uint32_t bytes);
    void (*put)(void* user, uint32_t ea, const void* src, uint32_t bytes);
};
// Directly on a guest address space laid out at `base` (vm_base in the game).
Io guest_io(uint8_t* base);

// The task's local store and registers, kept from step to step as the persistent task keeps them.
struct Local {
    spu_native::Context c;
    bool loaded = false;
    const char* why = nullptr;   // the last run that was not done: what happened
};

enum class Status { done, nothing, unsupported };

// nullptr when a step with these parameters stays inside the ranges a snapshot of it holds (the two layers,
// the texture: check mode and captures); else why not (an address not 16-byte aligned or a width not a
// multiple of 4, where the task halts; a width above 408; an index other than 0 and 1, which the game never
// passes).
const char* unsupported_reason(const Params& p, uint32_t param_ea);

// One step of the task, as it does it after waking: the 24 bytes at `param_ea` read through `io`, then its
// heights and its normal map (the generated code, on `ls`). done; nothing (a width under 8: no block);
// unsupported (the task halted, or did something a data-moving task does not: ls.why). `used`, when given,
// gets the parameters read.
Status run_frame(uint32_t param_ea, const Io& io, Local& ls, Params* used = nullptr);

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
//
// Native scheduling (DOTHEROES_NATIVE_WATER=1 or sync, no capture): the
// water's set-up (func_003C7BA8) and shutdown (func_003C7AF4) are the port's
// too, so that no SPURS instance, taskset, event flag, task or 0x3D600-byte
// block is made. The set-up does the game's own stores and its two texture
// calls, then puts the markers below where the instance (globals + 0x9C), the
// taskset (+0x4) and the block (+0x98) went: the water's update tests +0x4
// for non-zero and passes +0x98 to the signal; nothing else reads them (the
// lift: func_003C7460, func_003C7AF4, func_003C7BA8 and the module's static
// initializer only). The shutdown waits for the worker, then clears +0x4 and
// +0x9C as the game's does.
namespace water_task {
constexpr uint32_t kNativeBlock = 0xFFFFF001u, kNativeTaskset = 0xFFFFF002u, kNativeInstance = 0xFFFFF003u;
}
extern "C" int g_water_hook;                  // the hook's guard: 1 when any mode or capture is on
extern "C" int water_task_native_setup(void);                 // 1: the set-up and shutdown are native
extern "C" void water_task_native_started(uint32_t params);   // the set-up's: the 24 bytes' EA (globals + 0x80)
extern "C" void water_task_native_stopped(void);              // the shutdown's: returns once the worker is idle
extern "C" void water_task_note(const char* what);            // a line in DOTHEROES_WATER_LOG=1's log
extern "C" void water_task_init(void);        // at start-up, beside dotheroes_native_init
extern "C" void water_task_report(void);      // the counts, once (the test stop and atexit)
extern "C" int water_task_signal(uint32_t block, uint32_t bits);
// What it has done so far (tests; the exit report prints the same).
struct WaterTaskCounts {
    uint64_t signals, steps, coalesced, unsupported, shape;   // shape: blocks not built as the game builds them
    uint64_t checked, equal, busy;                            // check mode
    uint64_t setups, shutdowns;                               // native scheduling
};
extern "C" void water_task_counts(WaterTaskCounts* out);
