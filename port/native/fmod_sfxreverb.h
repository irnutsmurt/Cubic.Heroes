// FMOD Ex's "sfxreverb" SPU plugin (runtime image 25, lifted as
// build/gen/spu/spu_0024_at_00FC7180: 158 lifted functions) as native code:
// the entry points FMOD's SPU mixer (image 6) branches to, bit-exact.
//
// [C] read in the code: the overlay's lift (spu_recomp.c), checked against the
//     image's own disassembly (build/ref/ps3recomp/tools/spu_disasm.py on
//     build/gen/spu/images/spu_0024_at_00FC7180.elf), and the mixer's
//     disassembly (spu_0005_at_00F86A80) for how it is loaded and called;
// [H] inferred: names (FMOD's classes, the reverb's parts), the PPU side.
// [L] seen in run logs, [D] from docs/AUDIO.md.
// tests/native/fmod_sfxreverb_check.cpp runs the lifted overlay and this port
// on the same local store, registers and guest memory and compares every byte.
//
// ---- The image [C] -----------------------------------------------------------
// Link address 0x80: .SpuGUID 0x80, .text 0x90-0x2DE0, .rodata 0x2DE0 (two
// shuffle patterns at 0x2DE0 / 0x2DF0 for the late matrix, the 16 byte-shift
// patterns of an inline memmove at 0x2E00), .fixup 0x2F10 (one entry), .ctors
// 0x2F80 (0x2D20), bss 0x2F90-0x3020 (the descriptor at 0x2F90; a static list
// head at 0x2FEC). Program headers: [0x80, 0x2F20) from file offset 0x100;
// [0x2F80, 0x2F90) from 0x3000 and the bss. ELF entry 0x2CA8. Nine functions:
// 0x90, 0x820, 0x2C70, 0x2CA8, 0x2CB0, 0x2D20, 0x2D30, 0x2D88, 0x2D98; the
// lift's 158 are their basic blocks, plus 0x2E00 (.rodata the lifter took for
// code: a "stop 0x203", never branched to).
//
// ---- How the mixer loads and calls it [C, mixer disassembly] -------------------
// The mixer keeps each DSP unit's object (FMOD's DSPI, then the plugin's own
// state) in main memory. To run a unit (the code around 0x5F60-0x615C) it
//   - GETs the object's bytes [LS[0x36060], unit[0xD0]) from EA unit[0x120]
//     to LS[0x35EA0] (+ the same offset): `this` = LS[0x35EA0], the start of
//     the mixer's overlay area;
//   - calls its loader (0xD278, a jump table on unit[0xD4]: 0 = a DSP, 0xD8E4):
//     GET the image (EA unit[0x13C], unit[0x144] bytes, tag 1) to base =
//     LS[0x35EA0] + unit[0xD0] -- right after the object --, memset the bss
//     (0x2640), then, when unit[0xAC] and unit[0xCC] are set, call the entry
//     (base + unit[0x140] - unit[0x148]: 0x2CA8, relocated) and keep desc+0x34
//     at unit[0x94] (when unit[0xCC] is 0 the entry itself is kept); .init and
//     the .ctors are not run;
//   - sets this+0x1C = this (the DSP state's instance) and calls unit[0x94]:
//       read(this+0x1C, in, out, length, channels, channels)   at 0x6118
//     (in and out: float buffers in the mixer's local store, the channels
//     interleaved -- 8 in the game [H: the 7.1 ring]; the output becomes the
//     next unit's input).
//     Image 25 is loaded only in the dungeons [D, L].
// A second call through unit[0x94], at 0x2878 in an indirectly called mixer
// function, passes in = 0 [C]; its role is not known.
//
// ---- The entries, at link addresses (LS = link + delta) --------------------------
// 0x2CA8 (-> 0x2C70) the descriptor [C]: stores 0x2D88 + delta at 0x2FC4,
//   returns r3 = 0x2F90 + delta ([H] FMOD_DSP_DESCRIPTION: read at +0x34; its
//   other words are what the loader's memset left).
// 0x2D88 read(state, in, out, length, inchannels, outchannels) [C]:
//   this = state ? state - 28 : 0; then 0x2D30: process(this + 0x170, in, out,
//   inchannels, length, (float)this[0x8078], this[0x8074], the speaker mask
//   this[0x24] (u16)); returns 0. 0x820 does not use the float.
// 0x820 process(R, in, out, channels, length, -, dry, mask) [C; the parts' names H]:
//   The reverb's state is R = this+0x170, about 32 KB of local store: the mono
//   input (256 floats at R rounded up to 16), the parameters and their
//   smoothing, filter states, three LS delay rings (4,096 floats at R+0x2E00,
//   two 512-float allpass rings at R+0x6E90 and R+0x7690) and the tables of
//   its delay rings in main memory: the main ring (EA R[0x2CB0], mask
//   R[0x2D80], write position R[0x2D90], nine early taps) and eight late lines
//   (EA R[0x2B40 + 4j], mask, write position, delay, second tap). Per call:
//   - out = in + or - 1.0e-15 (a denormal guard), then the input downmixed to
//     mono (0x90: memmove for one channel, sums for 6 and 8, a loop otherwise);
//   - new delays and taps, and new parameter values, found by comparing with
//     the previous ones: a changed one is crossfaded or interpolated over
//     the block (a line's old position is then read by a 4-byte DMA a sample);
//   - DMA in (tags 0-31, each GET checked: 16-byte aligned, at most 16 KB):
//     each early tap's `length` samples from the main ring, each late line's
//     `length` + 4, each output channel's second tap (two GETs where a read
//     wraps); waits;
//   - each sample: the input through a one-pole lowpass twice and an optional
//     biquad, into the main ring; early tap 0 through two allpasses into the
//     4,096-float ring, its seven taps the early reflections; taps 1-8 into the
//     eight late lines (gain, one-pole lowpass, three butterfly stages back
//     into the lines); each output channel in the speaker mask = the early
//     reflections on it + its line taps (clamped to [-1, 1]) + dry x in;
//   - DMA out: the filtered input into the main ring, the lines' new samples
//     into theirs (PUTs at the write positions, `length` samples, no wrap);
//     waits.
//   The comments in fmod_sfxreverb.cpp give the fields block by block.
// 0x90, 0x2D98, 0x2D30, 0x2C70, 0x2CB0, 0x2D20: internal (0x2D20 and 0x2CB0 are
//   the static constructor, which the mixer never runs).
//
// ---- Halts [C] -------------------------------------------------------------------
// heqi at 0x10E8, 0x1190 (group A's two GETs), 0x1250 (group A, unwrapped),
// 0x1384, 0x1428, 0x1500 (group B), 0x164C, 0x16F0, 0x17C0 (group C), 0x205C,
// 0x20DC, 0x2190 (the 4-byte GETs: EA not 4-byte aligned), 0x2528 (channel
// count 0 -- unreachable: that code runs only for a channel count above 0),
// 0x2728, 0x27E0 (the PUTs). Each DMA check: LSA, EA and size 16-byte
// aligned, size <= 16 KB, tag <= 31.
//
// ---- Channels [C] -------------------------------------------------------------
// Only: wrch MFC_LSA/EAH/EAL/Size/TagID/Cmd (GET 0x40 and PUT 0x20 only),
// WrTagMask, WrTagUpdate; rdch MFC_RdTagStat. No decrementer, no atomics, no
// mailbox, no signal, no list DMA, no indirect branch, no stop in the
// reachable code. This port makes the same channel operations in the same
// order with the same values, through the callbacks.
//
// ---- How it is made, and what "bit-exact" covers ---------------------------------
// fmod_sfxreverb.cpp is generated from the lift by gen_sfxreverb.py: the nine
// functions put back together (each lifted function a labelled block, the
// lift's runtime transfers gotos, calls and returns), each statement the
// lift's own, in its order, on the same spu_helpers.h helpers (SPU single
// precision exactly; the integer, shuffle and quadword operations every lane).
// Unlike the codec_raw port, it keeps the whole register file: each function
// holds the registers it uses in locals, loaded from `gpr` at entry and after
// each call, stored back before each call, return and halt. So after a call
// every local-store byte and all 128 registers (all four words) are what the
// lifted code leaves, and a halt (heqi, the lift's spu_halt) returns
// Result::halted with the link PC after the same side effects.
// The check (2026-11-10): the default run, 5235 calls on each of the
// x86_64_v3 and x86_64_v2 builds (handpicked, 2000 random states, 24 streams
// of up to 150 reads carrying the reverb's state), 36589 calls on x86_64_v3
// (20000 random, 60 streams of 300) and 18185 on x86_64_v2: every byte,
// channel operation and register equal, the halts at the same PC; 155 of the
// 157 blocks taken (0x2C44 / 0x2C50 cannot run: the note at 0x2B40).
// Per 256 x 8 read in the game's shape: about 485 us native against 595 us
// lifted at x86_64_v3 (the checker's stand-in runtime, not the game's).
//
// ---- Using it in place of the lifted entry -------------------------------------
// As fmod_codec_raw: the runtime resolves a branch into the overlay's span to
// image 25 at link address lsa - delta; for 0x2CA8 and 0x2D88 (and the
// internal entries) it can call call() instead (ls = ctx->ls, gpr = ctx->gpr,
// channels = spu_wrch / spu_rdch on ctx); on handled set ctx->pc = gpr[0]
// word 0 (the lift's SPU_RET), keep ctx->ls_reloc = delta; on halted do what
// the lift does (the HALT-ASSERT line with the returned pc,
// SPU_STATUS_STOPPED_BY_HALT, spu_halt); on declined run the lifted function.
#pragma once
#include <cstdint>

#include "ps3emu/ps3types.h"   // u128: the runtime's register (word 0 = the preferred slot)

namespace fmod_sfxreverb {

constexpr uint32_t kLinkBase = 0x80;           // the image's link address
constexpr uint32_t kImageEnd = 0x3020;         // the end of its bss

// The entries the mixer branches to (link addresses).
constexpr uint32_t kEntryDescriptor = 0x2CA8;  // the ELF entry
constexpr uint32_t kEntryRead = 0x2D88;        // desc+0x34
// Internal entries, reached by those (for the check; the mixer never branches
// to them).
constexpr uint32_t kEntryDescriptorBody = 0x2C70;
constexpr uint32_t kEntryReadThis = 0x2D30;    // read on `this`
constexpr uint32_t kEntryProcess = 0x0820;     // the reverb
constexpr uint32_t kEntrySampleLoop = 0x0090;  // the downmix to mono
constexpr uint32_t kEntryDma = 0x2D98;         // one DMA and its wait
constexpr uint32_t kEntryConstructor = 0x2D20; // the .ctors entry
constexpr uint32_t kEntryStaticInit = 0x2CB0;

// The overlay's channel operations, in its order: wrch passes word 0 of the
// register (as spu_wrch uses it); rdch returns the word the runtime puts in
// word 0 of an otherwise zero register (spu_pref_u32).
struct Channels {
    void* user;
    void (*wrch)(void* user, uint32_t channel, uint32_t value);
    uint32_t (*rdch)(void* user, uint32_t channel);
};

enum class Result {
    handled,    // done: LS and every register as the lifted code leaves them
    declined,   // not an entry this port has; nothing was touched
    halted,     // the lifted code halts here (*halt_pc, a link address), after the same side effects
};

// Runs the overlay's function at local-store address entry_lsa (link address
// entry_lsa - reloc_delta) on the 256 KB local store `ls` and the register
// file `gpr` (128 registers), as the lifted function would when branched to
// with these registers.
Result call(uint8_t* ls, u128* gpr, uint32_t entry_lsa, uint32_t reloc_delta, const Channels& ch,
            uint32_t* halt_pc = nullptr);

// Block counters (the check reports which of the 157 blocks -- the lift's
// functions, 0x2E00 aside -- ran). Zero-cost unless the port is compiled with
// FMOD_SFXREVERB_COVERAGE.
int block_count();
uint32_t block_address(int block);    // a link address
uint32_t block_function(int block);   // the entry of the function it is in
extern unsigned long long g_block_hits[];

}  // namespace fmod_sfxreverb
