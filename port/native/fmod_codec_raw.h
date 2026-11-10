// FMOD Ex's "codec_raw" SPU plugin (runtime image 9, lifted as
// build/gen/spu/spu_0008_at_00FA9B80, 197 functions) as native code: the three
// entry points FMOD's SPU mixer (image 6) branches to, bit-exact.
//
// [C] read in the code: the overlay's lift (spu_recomp.c), checked against the
//     image's own disassembly (build/ref/ps3recomp/tools/spu_disasm.py on
//     build/gen/spu/images/spu_0008_at_00FA9B80.elf), and the mixer's lift and
//     disassembly (spu_0005_at_00F86A80) for how it is loaded and called;
// [H] inferred: names (FMOD's classes and constants), the PPU side, the shape of
//     the data. [L] seen in run logs, [D] from docs/AUDIO.md.
// tests/native/fmod_codec_raw_check.cpp runs the lifted overlay and this port
// on the same local store, registers and guest memory and compares every byte.
//
// ---- The image [C] -----------------------------------------------------------
// Link address 0x80: .SpuGUID 0x80, .init 0x90 (self-relocation through the
// .fixup list at 0x2D70, then the constructor list), .text 0x140, .fini,
// .rodata 0x2C50 (a shuffle pattern at 0x2C50, the 16 byte-shift patterns of an
// inline memcpy at 0x2C60), data 0x2E00-0x2EA0 (.ctors, two vtables in
// .data.rel.ro at 0x2E20 and 0x2E50, .data), bss 0x2EA0-0x3090. ELF entry 0x218.
//
// ---- How the mixer loads and calls it [C, mixer func_0000D57C] ------------------
// For a codec of this kind (a jump table on the channel's codec type) the
// mixer, per use:
//   - GETs 64 bytes of the plugin's ELF header to LS 0x36070, then the image
//     (EA and size from its per-channel block, obj+0x13C/+0x144, tag 1) to
//     base = LS[0x35EA0] + obj[0xD0] ([L] 0x156F0: relocation delta 0x15670);
//   - memsets the bss (base + obj[0x14C] - obj[0x148], obj[0x150] bytes, byte
//     obj[0x154]) [H: 0x2EA0, 0x1F0 bytes, 0 -- from the ELF]: the plugin's
//     static state (the pacing's two words) starts from zero at every load;
//   - calls the entry (base + e_entry - 0x80) WITHOUT running .init: the
//     .fixup relocations and the constructor are never applied, so the vtables
//     and the constructor list keep link addresses; nothing below reads them;
//   - takes desc+0x18 (read) and desc+0x20 (setposition) from the returned
//     descriptor into its own Codec object (obj+0x9A4 when obj[0xD4] == 2, else
//     obj+0x4B24): Codec+0x60 = read, +0x68 = setposition, +0x54 = 10 (the time
//     units [H: PCM | RAWBYTES]), +0x74 = the mixer's own getwaveformat (0x9F68),
//     +0x20 = the wave format (obj+0x5E4), +0xFC = the File (obj+0x220), and
//     File+0x340 = File+0x240 (the File's buffer in local store).
// Then [C, mixer func_00008C40 Codec::read, func_000088D8 Codec::setPosition]:
//   read(Codec+28, buffer, bytes, &bytesread)  at 0x8D10 (into the Codec's PCM
//       buffer, Codec[0xD8], Codec[0xE4] bytes) or 0x8E10 (the caller's
//       buffer; one caller passes a stack buffer of up to 4608 bytes);
//   setposition(Codec+28, subsound, position, 2)  at 0x8BD0 (2 [H]
//       FMOD_TIMEUNIT_PCM: the callers pass il 2).
// After each call the mixer reads r3 and its own callee-saved registers only
// (it redefines every other register first) [C, 0xD6A8, 0x8BD0, 0x8D10, 0x8E10].
//
// ---- The entries, at link addresses (LS = link + delta) --------------------------
// 0x218 (-> 0x1C8) the descriptor [C]: stores 0x490 + delta at 0x2EB8 and
//   0x3D0 + delta at 0x2EC0 (single words), returns r3 = 0x2EA0 + delta (its
//   other words: 0x2EA0 - 0x1D0). The descriptor lives in the bss; its other
//   words are what the loader's memset left. ([H] FMOD_CODEC_DESCRIPTION:
//   read at +0x18, setposition at +0x20.)
// 0x490 read(state, buffer, sizebytes, bytesread*) [C]:
//   this = state ? state - 28 : 0 (CodecRaw; the FMOD_CODEC_STATE is at +28);
//   if the wave format's word at +0x100 (this[0x20] + 0x100; [H] the
//   FMOD_SOUND_FORMAT, 2 = PCM16) is 2:
//       r = File::read(this[0xFC], buffer, 2, sizebytes >> 1, bytesread);
//       *bytesread *= 2 (the word at the pointer, read back)
//   else r = File::read(this[0xFC], buffer, 1, sizebytes, bytesread).
//   Returns r. No sample is converted: codec_raw copies raw bytes from main
//   memory into the local-store buffer; File::read swaps 16-bit elements
//   (size 2) unless File flag 0x8 is set, and can decrypt (below).
// 0x3D0 setposition(state, subsound, position, postype) [C]:
//   this = state ? state - 28 : 0; subsound unused;
//   postype 8 ([H] RAWBYTES): bytes = position;
//   else bytes = (u64)position * wf[0x10C] / wf[0x110] (__muldi3, __udivdi3:
//       [H] lengthbytes / lengthpcm), then (low word / wf[0x114]) * wf[0x114]
//       ([H] blockalign; 0 halts at 0x328; wf[0x110] = 0 halts in __udivdi3);
//   File::seek(this[0xFC], bytes + this[0xB8] ([H] the data's start), 0).
//
// ---- The File (an FMOD MemoryFile in the mixer's local store) [C] ----------------
// Word fields: +0x14 length (0xFFFFFFFF unknown), +0x230 a pointer, +0x234
// flags, +0x340 the buffer (LS), +0x348 the read offset, +0x34C the data's EA.
// Fields each in their own quadword (the SPU compiler stores whole registers
// there, words 1-3 included): +0x150 key length, +0x160 key index, +0x170
// buffer position, +0x180 buffer size, +0x190, +0x1A0 block size, +0x1B0,
// +0x1C0 position, +0x1D0, +0x1E0, +0x1F0 start offset, +0x200 a percentage,
// +0x210 the last read result. The key: bytes at +0x130.
// File::read(file, buffer, size, count, bytesread*) (0xE70): n = size * count
//   (37 if buffer is 0 or n < 0 as signed; 19 if the position is past the end;
//   clipped to the end, then 22 at the end); then per block: fill (0xAE8)
//   and copy from the File's buffer (an inline memcpy, aligned or with the
//   rodata shift patterns), advancing +0x170 (back to 0 at +0x180) and
//   +0x1C0; then the bytes read are swapped as size 2 (and size 4) elements
//   unless flag 0x8 ([H] little-endian data to the SPU's order); then with a
//   key the first (bytes read / size) bytes [sic] are bit-reversed and XORed
//   with the key byte at +0x130 + the key index (flag 0x400: XOR first), the
//   index advancing and wrapping at the key length (a full-quadword store);
//   *bytesread = bytes read / size (the pointer may be 0: the word at LS 0 is
//   rewritten unchanged).
// fill (0xAE8): decides from +0x1C0/+0x1D0/+0x1A0/+0x180/+0x190 and flags
//   0x1, 0x280 whether to refill; stores the percentage (float: (+0x1E0 -
//   +0x1C0) / +0x180 * 100, SPU single precision) at +0x200; refills through
//   0x818 or, with flag 0x1, realigns through 0x760.
// refill (0x818): reads +0x1A0 - +0x190 bytes into the buffer at +0x1B0 +
//   +0x190 through reallyRead, updating +0x1E0, +0x200, +0x1B0 (back to 0 at
//   +0x180), +0x190, +0x210 and flags 0x10/0x40/0x80/0x100/0x200. Its loop for
//   short reads, its setting of an unknown length at the end, and fill's
//   handling of a result 21 are never reached with this reallyRead (it
//   returns 0 with the whole piece, or 22 with what is left; and an unknown
//   length, 0xFFFFFFFF, never ends) [C]: ported, not exercised.
// realign (0x760): position rounded down to the buffer size; reallySeek.
// reallySeek (0x1A80): +0x348 = min(pos, length).
// reallyRead (0x1AC8): THE ONE DMA: GET n bytes -- or, when +0x348 + n passes
//   the length, (length - +0x348) rounded up to 16 (up to 15 bytes past the
//   data land in the buffer) -- from EA (u64)+0x34C + +0x348 (EAH = the
//   carry) to the buffer, tag 1, wait (WrTagMask 2, WrTagUpdate 2,
//   RdTagStat); halts at 0x1C50 if the LS address, the EA or the size is not
//   16-byte aligned or the size is above 16384; bytes = n or length - +0x348;
//   +0x348 += bytes; *got = bytes; result 0, or 22 when clipped. Before it,
//   a pacing of reads from EAs 0xC0000000-0xCF8FFFFF ([H] RSX local memory):
//   the decrementer (79.8 MHz) since the previous read accumulates, in
//   milliseconds (float), at bss 0x2F10; above 1.0 with such an EA it spins
//   17500 decrementer reads and resets; the last decrementer is kept at 0x2F20.
// seek (0x550): mode 0/1/2 (set, current, end; above 2: 37); clamps; 20
//   where a non-seekable buffered file cannot; sets +0x1C0, +0x160 (position %
//   key length), +0x170 (position % buffer size) or, unbuffered, reallySeek.
// The size-4 path and seek modes 1, 2 and above are not reached from the
//   mixer's entries (the codec reads sizes 1 and 2 and seeks from the start);
//   the internal entries below reach them for the check.
//
// ---- Channels [C] -------------------------------------------------------------
// Only: wrch MFC_LSA/EAH/EAL/Size/TagID/Cmd (one command site, GET 0x40),
// WrTagMask, WrTagUpdate; rdch MFC_RdTagStat, SPU_RdDec. No atomics, no
// mailbox, no signal, no list DMA, no indirect branch in the reachable code.
// This port makes the same channel operations in the same order with the same
// values, through the callbacks.
//
// ---- What "bit-exact" covers here ---------------------------------------------
// Every local-store byte the lifted code writes -- its own stack frames below
// the caller's r1 (saved registers, back chains, the 16-byte LR slot at
// r1 + 16), the File's whole-quadword fields (words 1-3 are SPU register
// lanes: this port computes all four lanes with the runtime's helpers), the
// bss, the buffers -- with the same values and in the same order; the channel
// sequence; the return value r3 (all four words); r0, r1, r80-r127 as the
// lifted code leaves them (reloaded from its own frames, as it does). Not
// reproduced: the volatile registers r2 and r4-r79 (dead after the call: see
// above). A halt (heqi, the lift's spu_halt) returns Result::halted with the
// link PC after the same side effects.
// Floating point (fill's and refill's percentages, the pacing's milliseconds)
// goes through spu_helpers.h's exact SPU single precision.
// The two libgcc helpers __muldi3 (0x1FC0) and __udivdi3 (0x2078) are
// transcribed instruction by instruction (their lanes reach the File's
// position field's words 1-3 through setposition).
// The check (2026-11-10): the default run, 5169 calls on each of the
// x86_64_v3 and x86_64_v2 builds (handpicked, 3000 random states, 24
// streams), and 44077 calls on x86_64_v3 (30000 random states, 60 streams
// of up to 300 calls): every byte, channel operation and kept register
// equal, the halts at the same PC.
//
// ---- Using it in place of the lifted entry -------------------------------------
// The runtime resolves a branch into the overlay's span to image 9 at link
// address lsa - delta; for 0x218, 0x490 and 0x3D0 it can call call() instead
// (ls = ctx->ls, gpr = ctx->gpr, channels = spu_wrch / spu_rdch on ctx); on
// handled set ctx->pc = gpr[0] word 0 (the lift's SPU_RET), keep
// ctx->ls_reloc = delta; on halted do what the lift does (the HALT-ASSERT
// line with the returned pc, SPU_STATUS_STOPPED_BY_HALT, spu_halt); on
// declined run the lifted function.
#pragma once
#include <cstdint>

#include "ps3emu/ps3types.h"   // u128: the runtime's register (word 0 = the preferred slot)

namespace fmod_codec_raw {

constexpr uint32_t kLinkBase = 0x80;           // the image's link address
constexpr uint32_t kImageEnd = 0x3090;         // the end of its bss
constexpr uint32_t kLoadedLsa = 0x156F0;       // [L] where the game loads it
constexpr uint32_t kGameDelta = kLoadedLsa - kLinkBase;

// The entries the mixer branches to (link addresses).
constexpr uint32_t kEntryDescriptor = 0x218;   // the ELF entry
constexpr uint32_t kEntryRead = 0x490;         // desc+0x18
constexpr uint32_t kEntrySetPosition = 0x3D0;  // desc+0x20
// Internal entries, reached by those (for the check; the mixer never branches
// to them): 0x1C8 (the descriptor's body), 0x3E0 (CodecRaw::read on `this`),
// 0x2B0 (CodecRaw::setPosition on `this`), 0xE70 (File::read), 0x550
// (File::seek).
constexpr uint32_t kEntryFileRead = 0xE70;
constexpr uint32_t kEntryFileSeek = 0x550;

// The channel numbers used (spu_context.h's).
constexpr uint32_t kSpuRdDec = 8;
constexpr uint32_t kMfcLsa = 16, kMfcEah = 17, kMfcEal = 18, kMfcSize = 19, kMfcTagId = 20, kMfcCmd = 21;
constexpr uint32_t kMfcWrTagMask = 22, kMfcWrTagUpdate = 23, kMfcRdTagStat = 24;

// The overlay's channel operations, in its order: wrch passes word 0 of the
// register (as spu_wrch uses it); rdch returns the word the runtime puts in
// word 0 of an otherwise zero register (spu_make_preferred_u32).
struct Channels {
    void* user;
    void (*wrch)(void* user, uint32_t channel, uint32_t value);
    uint32_t (*rdch)(void* user, uint32_t channel);
};

enum class Result {
    handled,    // done: LS, r3, r0, r1, r80-r127 as the lifted code leaves them
    declined,   // not an entry this port has; nothing was touched
    halted,     // the lifted code halts here (*halt_pc, a link address), after the same side effects
};

// Runs the overlay's function at local-store address entry_lsa (link address
// entry_lsa - reloc_delta) on the 256 KB local store `ls` and the register
// file `gpr` (128 registers), as the lifted function would when branched to
// with these registers.
Result call(uint8_t* ls, u128* gpr, uint32_t entry_lsa, uint32_t reloc_delta, const Channels& ch,
            uint32_t* halt_pc = nullptr);

// Path counters (the check reports which paths ran). Zero-cost unless the port
// is compiled with FMOD_CODEC_RAW_COVERAGE.
enum Path : int {
    kPathDescriptor, kPathRead8, kPathRead16, kPathSetPosRaw, kPathSetPosPcm,
    kPathFileReadNoBuffer, kPathFileReadNegative, kPathFileReadPastEnd, kPathFileReadClipped,
    kPathCopyAligned, kPathCopyUnaligned, kPathCopyTail, kPathBufferWrap, kPathFillEof,
    kPathSwap16, kPathSwap32, kPathDecrypt, kPathDecryptXorFirst, kPathBytesReadNull, kPathResultEof,
    kPathFillRefill, kPathFillRealign, kPathFillAgain, kPathFillResult21, kPathFillNegativePercent,
    kPathRefillPieces, kPathRefillLength, kPathRefillFlag40, kPathRefillWrap, kPathRefillNothing,
    kPathReadFits, kPathReadClipped, kPathPaceAccumulate, kPathPaceSpin, kPathDecWrapped,
    kPathSeekSet, kPathSeekCur, kPathSeekEnd, kPathSeekBadMode, kPathSeekNegative, kPathSeekClamp,
    kPathSeekCannot, kPathSeekKey, kPathSeekBuffered, kPathSeekUnbuffered,
    kPathHalt, kPathCount
};
const char* path_name(int path);
extern unsigned long long g_path_hits[kPathCount];

}  // namespace fmod_codec_raw
