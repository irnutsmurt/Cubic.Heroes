// The game's "waver" SPU task (runtime image 2, lifted as
// build/gen/spu/spu_0001_at_00F48F80) as native code: an expanding ripple,
// a 256 x 256 float height field made from a byte map and a 128-entry wave
// table, then that field's normal map into one of two 256 x 256 textures.
//
// [C] read in the code: the lift (spu_recomp.c), checked against the image's
//     own disassembly (build/ref/ps3recomp/tools/spu_disasm.py on
//     build/gen/spu/images/spu_0001_at_00F48F80.elf: entry 0x3050, text
//     0x3000-0x39A0, data 0x3A00-0x3A10, bss to 0x8B60);
// [H] inferred: the PPU's lifted code (build/r8/gen9: func_003CEB48 the
//     set-up, func_003CED20 the tables, func_003CEEE8 the update), or the shape
//     of the data. tests/native/waver_task_check.cpp runs the lifted image and
//     checks the [C] transfer order (addresses, sizes, local-store addresses,
//     tags, waits) on every case.
//
// What the task is:
// - [H] a one-shot SPURS task, one a frame: the update (func_003CEEE8) writes
//   the parameters (W + 0x8C..0xA0), waits for the previous frame's task
//   (func_004735DC) and creates the next (func_004736E0); the first frame runs
//   two (+0x8C = 0, then 1). The set-up (func_003CEB48) makes the SPURS
//   instance, the taskset, a 0x3D600-byte block, and puts the argument
//   { u64 EA of W + 0x80, u32 block + 0x3D400, 0 } at block + 0x3D480 (W = the
//   waver's state, 0x02AB2200 in the game). The parameters are written before
//   the wait: a task that had not started by then would read the next frame's.
// - [C] the entry (LS 0x3050) sets the stack (r1 = 0x3FFD0) and calls main
//   (LS 0x36D8) with r3 = the argument, then exit (LS 0x3828). main uses words
//   0-1 of r3 (the u64 EA; word 1 must be 16-byte aligned or it halts, LS
//   0x3704) and nothing else: word 2 (the event flag's EA) is never read, the
//   task neither waits on nor sets any event flag.
// - [C] no atomics (no GETLLAR/PUTLLC/PUTLLUC), no mailbox, no signal or event
//   channel, no list DMA: the only channels are MFC_LSA/EAH/EAL/Size/TagID/Cmd
//   (GET 0x40 and PUT 0x20 only), MFC_WrTagMask/WrTagUpdate (2: all) and
//   MFC_RdTagStat.
// - [C] exit: main returns 0; exit runs the atexit list (count at LS 0x8A1E,
//   bss: none), then LS 0x3920 stores the code, 0, in LS 0x2FD0's word 0 and
//   branches to the word at LS (word 2 of LS 0x2FB0) + 196 -- [H] the SPURS
//   kernel's task exit; the PPU learns the frame is done only from that exit.
//
// The task's transfers, one run (F = Params::field, T = the selected texture,
// P = LS 0x2FDA0, main's r1 + 32), each issued through its DMA helper (LS
// 0x3770: pieces of 16384, else size & ~15, then 8, 4, 2, 1 bytes) [C]:
//   GET 0x10224 <- the argument's EA -> P            tag 0: 0x4000 x 4, 0x220, 4
//   wait tag 0
//   field step (LS 0x3098), block o = 0..31 of 8 rows:
//     PUT 8192 -> F + 8192 o <- LS 0x3A10            tag 0; wait tag 0
//   normal map (LS 0x32E0), block k = 0..31 of 8 rows:
//     GET -> LS 0x3A10                               tag 0; wait tag 0
//         k = 0:      9216 bytes  <- F               (rows 0..8)
//         0 < k < 31: 10240 bytes <- F + 8192 k - 1024 (rows 8k-1..8k+8)
//         k = 31:     9216 bytes  <- F + 8192 k - 1024 (rows 247..255)
//     PUT 8192 -> T + 8192 k <- LS 0x6210             tag 1, not waited for
//   wait tag 1
// The next block's pixels are written into LS 0x6210 while tag 1's PUT may
// still be reading it (the loop waits for tag 0 only) [C]; the runtime runs
// each command as it is issued (spu_dma.h), so the lift has no race, and
// neither has this port. The field the normal map reads is the one the field
// step has just PUT, read back through guest memory [C].
//
// The algorithm, per element (x column, y row, 0..255) [C]:
//   field step: b = the byte map's [256 y + x];
//     inside = b <= radius (32-bit unsigned) and b <= 126 (clgtbi 126: a byte
//              above 126 is outside, whatever the radius);
//     i = ((b - phase) & 0xFFFF) * (frequency & 0xFFFF) & 127   (mpyu);
//     field = inside ? fm(table[i], amplitude) : -1.0f (0xBF800000)
//   normal map: h = field(x, y); neighbours clamp at the edges (x = 0's left
//     is itself, x = 255's right is itself, row 0's up is row 0, row 255's
//     down is row 255); a neighbour below zero (fcgt(0, v)) reads as
//     h9 = fm(h, 0.9f = 0x3F666666);
//     h below zero: pixel 0x008080FF;
//     else ny = fm(slope, up - down) (fs), nx = fm(right - left, slope),
//          s = fma(nx, nx, fm(ny, ny)),
//          y = fi(s, frsqest(s)), q = y s, len = fma(fnms(y, q, 1.0000001f), q 0.5, q),
//          d = len + 1 (fa), r = fi(d, frest(d)), t = 1 r, inv = fma(fnms(t, d, 1), r, t),
//          X = cfltu(fm(fma(fm(nx, inv), .5, .5), 255)), Y the same of ny, Z of inv itself,
//          pixel = X << 16 | Y << 8 | Z (32-bit shifts and ors)
//   (n = (nx, ny, 1) / (|(nx, ny)| + 1), not normalised; the computed flat
//   normal is 0x007F7FFF, the outside pixel 0x008080FF).
// All arithmetic in the SPU's single precision through the runtime's exact
// helpers (spu_helpers.h: truncation, denormals as zero, extended range), in
// the task's operation order, four columns at a time (the helpers are
// lane-wise; the task does one column at a time in the preferred slot).
//
// [H] from the PPU: the byte map is (int)sqrt((x - c)^2 + (y - c)^2), the
// distance from the centre (func_003CED20); the table is 128 samples of a
// scaled, offset sine; phase and radius are the same integer from the
// elapsed time, the radius capped at 0x80 (func_003CEEE8); amplitude is
// W + 0x9C, fading; +0x0C is the frame counter + 1 (its bit 0 alternates the
// two textures: double buffering).
#pragma once
#include <cstdint>

namespace waver_task {

// The block the task GETs (W + 0x80 .. W + 0x102A4), big-endian as in guest
// memory; the byte map and the table stay in Local::params.
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

// The task's local-store buffers this code reproduces.
struct Local {
    alignas(16) uint8_t params[0x10230];   // LS 0x2FDA0: the block as GET
    alignas(16) uint8_t rows[0x2800];      // LS 0x3A10: the field step's 8 rows out; the normal map's 9 or 10 rows in
    alignas(16) uint8_t out[0x2000];       // LS 0x6210: the normal map's 8 rows of pixels
};

enum class Status { done, unsupported };

// nullptr when run_frame reproduces the task for these parameters; else why
// not: an EA the task halts on (the argument, the field, the selected
// texture not 16-byte aligned -- run_frame then makes the transfers the task
// makes before its halt and returns unsupported), or a block, field or
// texture running past 4 GB (not reproduced: nothing is done).
const char* unsupported_reason(const Params& p, uint32_t param_ea);

// One run of the task: the block at `param_ea` read through `io`, the field
// step, the normal map. `used`, when given, gets the parameters read.
Status run_frame(uint32_t param_ea, const Io& io, Local& ls, Params* used = nullptr);

// The two halves (ls.params holding the block): false where the task halts
// (the field's EA at its first PUT, LS 0x3254; the texture's at its first
// PUT, LS 0x3664), after the transfers the task makes before it.
bool field_step(const Params& p, const Io& io, Local& ls);
bool normal_map(const Params& p, const Io& io, Local& ls);

}  // namespace waver_task
