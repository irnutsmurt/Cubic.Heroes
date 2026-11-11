// The game's "waver" SPU task (runtime image 2, lifted by the setup as build/gen/spu/spu_0001_at_00F48F80) run
// as native code without SPURS or an SPU thread: an expanding ripple, a 256 x 256 float height field made from
// a byte map and a 128-entry wave table, then that field's normal map into one of two 256 x 256 textures.
// docs/SPU_NON_AUDIO.md section 4.
//
// The task's own code is not here: it is generated on the player's machine from the player's lift
// (waver_gen, port/native/spu_native.h; tools/spu_native/waver.py). This file is the host around it: the
// task's start as SPURS gives it (its image loaded afresh, the stack, the argument), its transfers done at
// once on guest memory; waver_game.cpp is the game's side.
//
// What the task is, as the game uses it:
// - [H] a one-shot SPURS task, one a frame: the update (func_003CEEE8) writes the parameters (W + 0x8C..0xA0),
//   waits for the previous frame's task (func_004735DC) and creates the next (func_004736E0); the first
//   frame runs two (+0x8C = 0, then 1). The set-up (func_003CEB48) makes the SPURS instance, the taskset, a
//   0x3D600-byte block, and puts the argument { u64 EA of W + 0x80, u32 block + 0x3D400, 0 } at
//   block + 0x3D480 (W = the waver's state, 0x02AB2200 in the game). The parameters are written before the
//   wait: a task that had not started by then would read the next frame's.
// - [C] the entry sets the stack (r1 = 0x3FFD0) and calls main with r3 = the argument; main reads the block
//   at the argument's u64 EA (16-byte aligned, or it halts), writes the field, reads it back and writes the
//   selected texture, and returns 0. No atomics, mailbox, event flag or list DMA: GETs and PUTs only.
//
// [H] from the PPU: the byte map is the distance from the centre (func_003CED20); the table is 128 samples
// of a scaled, offset sine; phase and radius are the same integer from the elapsed time, the radius capped at
// 0x80 (func_003CEEE8); amplitude is W + 0x9C, fading; +0x0C is the frame counter + 1 (its bit 0 alternates
// the two textures: double buffering).
#pragma once
#include <cstdint>

#include "spu_native.h"

namespace waver_task {

// The block the task GETs (W + 0x80 .. W + 0x102A4), big-endian as in guest
// memory: the parameters, then the byte map and the table.
constexpr uint32_t kParamBytes = 0x10224;
constexpr uint32_t kMapOffset = 0x24;       // 256 x 256 bytes (W + 0xA4)
constexpr uint32_t kTableOffset = 0x10024;  // 128 floats (W + 0x100A4)
constexpr uint32_t kFieldBytes = 0x40000;   // 256 x 256 floats, pitch 1024
constexpr uint32_t kTextureBytes = 0x40000; // 256 x 256 32-bit pixels, pitch 1024

struct Params {
    uint32_t field = 0;        // +0x00 the height field's EA (W + 0x102A4's word at set-up) [C: the PUTs, the GETs]
    uint32_t texture[2] = {};  // +0x04, +0x08 the two textures (func_003A4740 of W + 0x28, W + 0x40) [C: the PUTs]
    uint32_t select = 0;       // +0x0C bit 0 picks the texture [C]
    uint32_t phase = 0;        // +0x10 subtracted from the byte [C]
    uint32_t radius = 0;       // +0x14 bytes above it are outside [C]
    uint32_t frequency = 0;    // +0x18 its low 16 bits multiply (byte - phase) [C]
    uint32_t amplitude = 0;    // +0x1C float bits, times the table's entry [C]
    uint32_t slope = 0;        // +0x20 float bits, times the field's differences [C]
    uint32_t texture_ea() const { return texture[select & 1]; }
};
Params parse_params(const uint8_t bytes[kParamBytes]);

// The game's block around the task (func_003CEB48), as the water's.
constexpr uint32_t kBlockFlag = 0x3D400, kBlockArgument = 0x3D480;

// Where the task's transfers go. A GET copies guest bytes into the task's
// buffers, a PUT copies them back -- in the task's order and in its DMA
// helper's pieces, so a destination a later transfer reads (the field) or
// overwrites is as the task has it.
struct Io {
    void* user;
    void (*get)(void* user, void* dst, uint32_t ea, uint32_t bytes);
    void (*put)(void* user, uint32_t ea, const void* src, uint32_t bytes);
};
// Directly on a guest address space laid out at `base` (vm_base in the game).
Io guest_io(uint8_t* base);

// The task's local store and registers (its image loaded afresh for each run, as SPURS starts the task).
struct Local {
    spu_native::Context c;
    const char* why = nullptr;   // the last run that was not done: what happened
};

enum class Status { done, unsupported };

// nullptr when a run with these parameters stays inside the ranges a snapshot of it holds (check mode); else
// why not (an EA the task halts on: the argument, the field, the selected texture not 16-byte aligned; or a
// block, field or texture running past 4 GB).
const char* unsupported_reason(const Params& p, uint32_t param_ea);

// One run of the task: its main (the generated code) on a freshly loaded image, the block at `param_ea` read
// through `io`. unsupported: the task halted, or did something a data-moving task does not (ls.why). `used`,
// when given, gets the parameters read.
Status run_frame(uint32_t param_ea, const Io& io, Local& ls, Params* used = nullptr);

// Whether this build has the task's generated code (a tree without the lift of its image has the stub).
bool available();

}  // namespace waver_task
