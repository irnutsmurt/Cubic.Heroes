// FMOD Ex's SPU mixer (runtime image 6, fmodex_spu.elf, lifted as build/gen/spu/spu_0005_at_00F86A80: 922
// lifted functions) as native code, bit-exact with the lift: the thread from its entry (0xF0), or any function
// of it, run on a 256 KB local store and a register file, with every operation that leaves the SPU an
// explicit callback in the lift's order and with the lift's values.
//
// [C] read in the code (the lift, and the image's disassembly by build/ref/ps3recomp/tools/spu_disasm.py);
// [H] inferred (names, roles); [T] seen in the game's channel traces (docs/AUDIO.md "A0, the captures").
//
// ---- The image [C] ------------------------------------------------------------------------------------------
// Link address 0x80: .SpuGUID 0x80, .init 0x90, .text 0xE0-0x103B0, .fini 0x103B0, .rodata 0x103E0 (the lift
// also took it for code: 0x10420-0x104B0, never reached), .ctors 0x10600 (-1, 0x9150, 0xA040, 0xCC08, 0xD068,
// 0), .dtors 0x10618, .data 0x10620-0x107F0, .bss 0x10800-0x36470. Entry 0xF0.
//
// ---- How it runs [C, T] ---------------------------------------------------------------------------------------
// 0xF0 crt0: r1 = 0x3FFD0, .init (0x90: the four static constructors through .ctors, bisl), main(r3-r6)
//   (0xE190), exit (0xEA98), stop 0.
// 0xE190 main(the cellAudio ring EA, FMOD's system object, the parameter block EA): GET the 0x80-byte parameter
//   block to LS 0x36180 (tag 1), keep its fields, initialise (0x102A0: send_event(9, 0, ...) the first reply,
//   0xCFD8, 0x7A8), then the main loop at 0xE39C:
//     - sys_spu_thread_receive_event(0x63) (0x10340): rchcnt RdInMbox, WrOutMbox 0x63, stop 0x110, RdInMbox
//       the status and three data words;
//     - two decrement-if-nonzero on lines shared with the PPU (0xE6C8 on LS 0x36200 and 0x36280: GETLLAR,
//       RdAtomicStat, the word, PUTLLC and RdAtomicStat until it holds); 0xFEEDBEEF ends the loop;
//     - the block (0xDA30; 0xDD70 on the other path): GETs of the voices, the codec plugins through their
//       objects (overlays loaded at run time, called by bisl), the DSP units (0x5F60-0x615C: the plugin's read
//       callback, bisl at 0x6118), the mix, the PUT of the block into the ring; about 63 GETs, 48 PUTs, 70
//       decrementer reads a block [T];
//     - sys_spu_thread_send_event(9, 0, 0) (0x102A0): rchcnt RdInMbox, WrOutMbox, WrOutIntrMbox, RdInMbox.
//   fmod_mixer.cpp has a line on each function (what reaches it), gen_spu_native.py's report the whole map.
//
// ---- What the port makes explicit ------------------------------------------------------------------------------
//   - channels: every wrch (word 0 of the register), rdch (its value goes into word 0 of an otherwise zero
//     register, as spu_rdch returns it) and rchcnt (the same), in the lift's order: the MFC (LSA, EAH, EAL,
//     Size, TagID, Cmd -- GET 0x40, PUT 0x20, GETLLAR 0xD0, PUTLLC 0xB4 are the commands the traces show --,
//     WrTagMask, WrTagUpdate, RdTagStat, RdAtomicStat), the mailboxes (RdInMbox, WrOutMbox, WrOutIntrMbox),
//     the decrementer (RdDec). A DMA is its channel writes: the host does it on the wrch of MFC_Cmd, as the
//     runtime does.
//   - stops: stop(code, pc, block). The lift continues past a stop only where it tests the status after it
//     (the 0x110 receive at 0x10364): there a 1 from the host goes on, 0 ends the run. Every other stop
//     (0x102 exit at 0x10294, 0 after crt0's exit, and the data the lift took for code) ends the run after
//     the callback (Result::stopped).
//   - indirect branches: a call through a pointer (bisl, 12 sites) or a tail jump (bi $rN: 0x91D8's pointer
//     tail call, 0xEA74 into the loaded MP3 decoder) goes to the port's own function when the target is one
//     of its entries (ADDRESS_TAKEN in the configuration: the four constructors and eight internal
//     callbacks; and any other function it has); anything else goes to the host's branch callback with its
//     kind: overlay_call / overlay_jump for a target outside the image's code (a plugin the mixer loaded:
//     codec_raw, sfxreverb, ...), unresolved_call / unresolved_jump inside the image but not a function the
//     port has. The five jump tables (0x231C, 0x245C, 0x873C, 0xCD04, 0xD2FC, read from the image) are
//     switches of gotos over their targets; a target not in the table -- possible only if the image's text in
//     the local store was changed -- ends the run (Result::unresolved). Nothing falls back silently.
//   - the return check of the lift's drain: after a call, the callee must have returned (bi $r0) to the link;
//     anything else ends the run (Result::bad_return), where the runtime would go on at that pc.
//   What the port does not do as the runtime does, none of it met in the mixer [C]: the runtime's drain also
//   takes an indirect jump to the innermost call's return pc as that call's return ("return through another
//   register"); the port hands such a jump to the host (unresolved_jump). The mixer's only indirect jumps that
//   are not jump tables are 0x91D8's tail call through an object's pointer and 0xEA74's jump into the loaded MP3
//   decoder. A branch target with its low two bits set is matched exactly, as the runtime's registry does (the
//   SPU would ignore those bits). Interrupts (bie/bid, the drain's interrupt hook) are not modelled: the mixer
//   has none.
//   - halts (heqi, 104 sites): Result::halted with the pc, after the same side effects.
//   - .word the lift could not translate (only in data it took for code): the host's note, then on, as the
//     lift's spu_unsupported does.
//
// ---- Bit-exact ------------------------------------------------------------------------------------------------------
// fmod_mixer.cpp is generated (gen_spu_native.py, fmod_mixer_config.py, fmod_mixer.cpp.in): each statement the
// lift's own, in its order, on the same spu_helpers.h helpers; the whole register file kept. A function holds
// the registers it names in locals, loaded at entry; around a call it stores only the ones it assigned that the
// callee can reach and reloads only those the callee can change (the generator's interprocedural effects: a
// callee that can reach a stop or a branch to the host reaches every register); at a return, exit, stop, tail
// jump and branch to the host it stores all it assigned. So at every stop and branch callback and at every exit,
// all 128 registers (all four words) and every local-store byte are what the lifted code has at that point; at a
// channel callback the local store is, the registers not (no host needs them there). Within a function a forward
// dataflow keeps the set that may be dirty at each site: each store is of what may differ from the file there.
//
// ---- Speed (2026-11-11) ---------------------------------------------------------------------------------------------
// The first port saved and loaded the function's whole register set around every call: in the game the mixer's
// work a block was 150.6 us native against 126.0 us lifted (the river fight; docs/AUDIO.md A3 part 2). Measured
// off-line (fmod_mixer_check --profile: the river trace preloaded, the time in the model's channel operations,
// DMA and plugins counted apart, so the rest is the mixer's code alone): the lifted mixer 49.9 us a block, the
// first port 69.2 us (+38.5%); sampled, 18 us of the port's were on its register saves and loads around calls,
// at entries and at returns (the plugin loader's helpers called per DSP unit saving ~100 registers each time),
// 2.4 us in lsr / lsw left out of line. With the effects across calls, the dataflow within functions and lsr /
// lsw always inlined, on the same harness (the mixer's code a block, best of the passes): the river trace
// x86_64_v3 the port 42.3 us against the lifted 50.2 (-15.9%), x86_64_v2 59.9 against 70.6 (-15.2%); the long
// dungeon trace (24,070 blocks, sfxreverb through 0x6118) x86_64_v3 44.3 against 53.0 (-16.6%). What is left of
// the register traffic is mostly where the file must be whole: the plugin loader's calls into a plugin
// (0xD6A4, 0xD9D4) and the calls that can reach them.
// Checked by fmod_mixer_check.cpp (2026-11-10): the game's two channel traces of the mixer's thread (rel24:
// the river fight's run and the first dungeon's, each from the thread's entry to its 1,500 MB limit, about 28 s
// of audio) replayed into the lifted mixer and into this port, every record fed or checked (each rdch value,
// GET and GETLLAR bytes fed; each wrch value, MFC command, PUT and PUTLLC byte and stop checked): the river 5,189
// blocks, the dungeon 5,375, all equal for both, and the two equal at every block boundary (all 128 registers,
// the whole local store), at x86_64_v3 and x86_64_v2. Synthesized cases from the traces' states (the block with
// what its GETs read mutated, single functions with drawn arguments): the numbers in the checker. Neither trace
// calls sfxreverb (codec_raw: 31,088 and 30,978 calls), and neither reaches the DSP plugin call at 0x6118; the
// long dungeon trace (24,070 blocks, 15,308 sfxreverb calls through 0x6118) and the trace of the native mixer's
// own run in the game (24,445 blocks) do, and replay equal too. Rechecked after the register effects (2026-11-11):
// all four traces, every block boundary equal, at both levels; 77,855 synthesized cases equal.
//
// ---- What the host provides in place of the SPU thread (the runtime does each of these today) ----------------
// Run call(ctx->ls, ctx->gpr, kEntry, host) on the thread's own host thread, from the registers lv2's group start
// gives it (r3-r6 the arguments; crt0 sets r1). It returns only when the thread ends. Every DMA is done when its
// MFC_Cmd is written (as the runtime does: synchronously).
//   wrch  MFC_LSA/EAH/EAL/Size/TagID: stage the command. MFC_Cmd: do it -- GET 0x40 (main memory to LS[LSA,
//           +Size)), PUT 0x20 (LS to main memory, through the same coherence path as the runtime's PUT: it
//           breaks PPU reservations on the lines it writes), GETLLAR 0xD0 (the 128-byte line to LS, a
//           reservation taken), PUTLLC 0xB4 (the line stored only if the reservation holds, atomically against
//           the PPU's lwarx/stwcx on the same lines: A4). A GET of a plugin's image (its EA is registered as the
//           runtime's overlay registry does today) makes that plugin resident at LSA: remember its span and
//           delta for branch(). MFC_WrTagMask, MFC_WrTagUpdate: the tag mask (everything is complete).
//           SPU_WrOutMbox: the next event call's word (the spuq before stop 0x110; data1 before a send; the
//           status before stop 0x102). SPU_WrOutIntrMbox: sys_spu_thread_send_event / throw_event --
//           (spup << 24 | data0), 0x40000000 set for a throw; send the event to the port (spup 9: the reply to
//           FMOD's "libAudio event receive" thread; spup 11 in output modes 2 and 6, never connected) and, for a
//           send, queue its result (CELL_OK) for the next RdInMbox.
//   rdch  MFC_RdTagStat: the mask. MFC_RdAtomicStat: after GETLLAR 4; after PUTLLC 0 when it held, 1 when the
//           reservation was lost (the mixer then retries from the GETLLAR) -- spu_channels.c's values. SPU_RdDec:
//           the decrementer, as spu_channels.c derives it from the host's clock at 79.8 MHz (the mixer never
//           writes WrDec, so it counts down from 0 at the first read; the traces start 0, FFFFAEFC, ...).
//           SPU_RdInMbox: the queued words: after stop 0x110 {status, data1, data2, data3}, after a send its
//           result.
//   rchcnt  SPU_RdInMbox: the words queued (0 at both sites in normal running; nonzero makes the send/receive
//           helpers return EBUSY 0x8001000A without a stop). MFC_RdTagStat, MFC_WrTagUpdate: 1.
//   stop  0x110 sys_spu_thread_receive_event: wait for an event on the queue bound to the spuq just written
//           (0x63: the block clock), queue its four words, return 1; return 0 to end the thread (group
//           terminate). 0x102 sys_spu_thread_exit and 0 (after crt0's exit): the thread ends (Result::stopped).
//   branch  overlay_call / overlay_jump: the plugin resident at target (its span, delta = LSA - 0x80): run its
//           function at link = target - delta on the same LS and register file -- codec_raw's 0x218, 0x490,
//           0x3D0 and sfxreverb's 0x2CA8, 0x2D88 natively (their ports), anything else lifted -- and return 1
//           with *ret_pc = gpr[0] word 0 (where it returned). A target in no resident plugin: report it and return
//           0 (the runtime reports an unlifted branch there). unresolved_call / unresolved_jump: never in the game
//           traces nor in any real block the checker re-ran; report and return 0 (or run the lifted function, if
//           the lift is kept for diagnostics).
//   Result::halted: the runtime's halt (its HALT-ASSERT line; the thread stops); bad_return, unresolved: report,
//   as fatal.
#pragma once
#include <cstdint>

#include "ps3emu/ps3types.h"   // u128: the runtime's register (word 0 = the preferred slot)

namespace fmod_mixer {

constexpr uint32_t kEntry = 0xF0;              // the ELF entry (crt0): the thread's start
constexpr uint32_t kEntryMain = 0xE190;
constexpr uint32_t kEntryBlock = 0xDA30;       // the block, as the main loop calls it
// The image's span for Branch: [.SpuGUID, .fini end) -- the lift took .SpuGUID's bytes for code too
constexpr uint32_t kCodeBegin = 0x80, kCodeEnd = 0x103E0;

enum class Branch : uint32_t {
    overlay_call = 1,      // bisl to code outside the image (a plugin): r0 = the link
    overlay_jump = 2,      // bi to code outside the image: r0 = the current function's link (a tail call)
    unresolved_call = 3,   // bisl into the image, not a function the port has
    unresolved_jump = 4,   // bi into the image, not a function the port has nor a target of its table
};

struct Host {
    void* user;
    void (*wrch)(void* user, uint32_t channel, uint32_t value);
    uint32_t (*rdch)(void* user, uint32_t channel);
    uint32_t (*rchcnt)(void* user, uint32_t channel);
    // stop-and-signal: code, the stop's pc, and `block`: the lifted function it is in (what the runtime's
    // ctx->pc holds there when that block was entered by a transfer; the traces show it). The register file is
    // current, and the host must not change it (the port goes on with the registers it holds); the local store
    // it may. For a continuable stop (0x110) return 1 to go on past it, 0 to end the run; for the others the
    // return value is not used (the run ends).
    int (*stop)(void* user, uint32_t code, uint32_t pc, uint32_t block);
    // An indirect branch the port does not place (Branch). The register file and LS are current (r0 = the
    // link for a call). Run the code at `target` (an LS address, masked) as the runtime would and return 1
    // with *ret_pc = the pc it returned to (gpr[0] word 0 at its bi $r0), the file and LS updated; or 0 to
    // end the run (Result::ended). `site` is the branch instruction's pc.
    int (*branch)(void* user, Branch kind, uint32_t target, uint32_t site, uint32_t* ret_pc);
    // Optional (may be null): an instruction the lift could not translate ran (`pc`).
    void (*unsupported)(void* user, uint32_t pc);
};

enum class Result {
    handled,      // the function returned (*ret_pc: the pc, gpr[0] word 0 at its bi $r0)
    declined,     // not an entry the port has; nothing was touched
    halted,       // a halt (heqi) at *pc
    stopped,      // a stop the lift does not continue past (*pc, *code), after the stop callback
    ended,        // a callback ended the run (stop returned 0, or branch returned 0)
    bad_return,   // a callee returned to *pc, not to its link (the lift's drain would go on there)
    unresolved,   // a jump table's target outside its table (*code) at the jump *pc: the image's text was changed
};

struct Outcome {
    uint32_t ret_pc = 0;   // handled: where it returned
    uint32_t pc = 0;       // halted, stopped, bad_return, unresolved: the pc; ended: the stop's or branch's site
    uint32_t code = 0;     // stopped, ended at a stop: the stop code; unresolved: the target
};

// Runs the function at local-store address `entry` on `ls` (256 KB) and `gpr` (128 registers) as the lifted
// function entered there would, the host's callbacks for everything outside.
Result call(uint8_t* ls, u128* gpr, uint32_t entry, const Host& host, Outcome* out = nullptr);

// The functions the port has (their entries, ascending), and whether an address is one.
int entry_count();
uint32_t entry_address(int k);
bool has_entry(uint32_t lsa);

// Block counters (the lift's functions the port generates). Zero-cost unless compiled with
// FMOD_MIXER_COVERAGE.
int block_count();
uint32_t block_address(int block);    // the lifted function's address
uint32_t block_function(int block);   // the entry of the (first) function it is in
extern unsigned long long g_block_hits[];

}  // namespace fmod_mixer
