// Native code generated from the player's own SPU programs (docs/SPU_NON_AUDIO.md, "Generated on the player's
// machine"): tools/gen_spu_native.py turns an image's lift (the setup's build/gen/spu, made from the player's
// executable) into C++ -- one function per SPU function, every statement the lift's own -- with
// tools/spu_native_task.cpp.in and a per-image configuration (tools/spu_native/*.py: where the image's code is,
// its entry, its jump tables). Nothing of an image is in this repository: its generated code exists only in the
// player's build (and a developer's), like the lift.
//
// The generated code runs on a 256 KB local store and a register file of the caller's, as the lifted function
// entered at the same address would, and calls the host (Host) for everything that leaves the SPU: channel
// reads and writes (DMA, tags, mailboxes, the decrementer), stop-and-signal, branches to code it does not have.
// It needs no SPU thread, SPURS or the runtime's channel machinery: a host that does the transfers itself
// (Dma below, for a task that only GETs and PUTs) runs it on any thread.
#pragma once
#include <cstdint>

#include "ps3emu/ps3types.h"   // u128: the runtime's register (word 0 = the preferred slot)

namespace spu_native {

enum class Branch : uint32_t {
    overlay_call = 1,      // bisl to code outside the image: r0 = the link
    overlay_jump = 2,      // bi to code outside the image: r0 = the current function's link (a tail call)
    unresolved_call = 3,   // bisl into the image, not a function the generated code has
    unresolved_jump = 4,   // bi into the image, not a function it has nor a target of its table
};

struct Host {
    void* user;
    void (*wrch)(void* user, uint32_t channel, uint32_t value);
    uint32_t (*rdch)(void* user, uint32_t channel);
    uint32_t (*rchcnt)(void* user, uint32_t channel);
    // stop-and-signal: code, the stop's pc, and the lifted function it is in. For a stop the lift continues
    // past, return 1 to go on, 0 to end the run; for the others the value is not used (the run ends).
    int (*stop)(void* user, uint32_t code, uint32_t pc, uint32_t block);
    // An indirect branch the generated code does not place: run it and return 1 with *ret_pc = where it
    // returned, or 0 to end the run.
    int (*branch)(void* user, Branch kind, uint32_t target, uint32_t site, uint32_t* ret_pc);
    // Optional: an instruction the lift could not translate ran at `pc`.
    void (*unsupported)(void* user, uint32_t pc);
};

enum class Result {
    handled,      // the function returned (Outcome::ret_pc: gpr[0] word 0 at its bi $r0)
    declined,     // not an entry the generated code has; nothing was touched
    halted,       // a halt (heqi) at Outcome::pc
    stopped,      // a stop the lift does not continue past (pc, code), after the stop callback
    ended,        // a callback ended the run
    bad_return,   // a callee returned to Outcome::pc, not to its link
    unresolved,   // a jump table's target outside its table (code) at the jump (pc): the image's text was changed
};

struct Outcome {
    uint32_t ret_pc = 0, pc = 0, code = 0;
};

const char* result_name(Result r);

// A host for a task that only moves data: every GET (0x40-0x43) and PUT (0x20-0x23) done when its command is
// written, through the caller's get/put (guest memory, a snapshot, scratch); tag waits answered at once (all
// complete); anything else -- another command, a mailbox, a stop, a branch out of the image -- noted in
// `trouble` and the run ended.
struct Dma {
    void* user = nullptr;
    void (*get)(void* user, void* dst, uint32_t ea, uint32_t bytes) = nullptr;
    void (*put)(void* user, uint32_t ea, const void* src, uint32_t bytes) = nullptr;
    // optional: each transfer as issued (for checks of the transfer order)
    void (*trace)(void* user, uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag) = nullptr;
    uint8_t* ls = nullptr;
    uint32_t lsa = 0, eah = 0, eal = 0, size = 0, tag = 0, mask = 0;
    const char* trouble = nullptr;
    uint32_t trouble_value = 0;
    Host host();
};

// The local store and register file one generated run needs.
struct Context {
    alignas(16) uint8_t ls[0x40000];
    u128 gpr[128];
};

}  // namespace spu_native

// What each generated image provides (tools/spu_native_task.cpp.in), in the namespace its configuration names.
#define SPU_NATIVE_IMAGE(ns)                                                                                        \
    namespace ns {                                                                                                  \
    /* the image's loadable bytes into `ls` (its code, read-only data and data; bss zero) */                       \
    void load(uint8_t* ls);                                                                                         \
    /* the function at `entry` run as the lifted one entered there would */                                       \
    spu_native::Result call(uint8_t* ls, u128* gpr, uint32_t entry, const spu_native::Host& host,                \
                            spu_native::Outcome* out = nullptr);                                                    \
    bool has_entry(uint32_t lsa);                                                                                   \
    extern const char* const kSource;   /* the lift and image it was generated from */                             \
    }

SPU_NATIVE_IMAGE(water_gen)   // the water's task (runtime image 1): water_task.cpp
SPU_NATIVE_IMAGE(waver_gen)   // the waver's task (runtime image 2): waver_task.cpp
