// FMOD's SPU mixer (image 6) run as native code in the game (docs/AUDIO.md step A3, part 2; fmod_mixer.h for the
// port itself): the mixer's thread runs the generated port from its entry in place of the lifted program
// (ps3recomp spu_thread_set_native), on the same context, every callback on the runtime's own services --
// channel reads and writes through spu_rdch / spu_wrch (DMA, atomics, mailboxes, the decrementer), stops through
// spu_stop (the block event's receive, the thread's exit), and the indirect calls into plugins through the
// runtime's overlay dispatch (where the native codec_raw and sfxreverb answer). So only the mixer's own code
// changes; the SPU environment around it is the runtime's still (A3's retirements come after).
// DOTHEROES_NATIVE_FMOD_MIXER: unset or 0 the lifted mixer (until checked in the game); 1 the native one.
#include "fmod_mixer.h"
#include "spu_context.h"
#include "spu_lifted_thread.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void spu_halt(spu_context* ctx);
extern "C" void spu_stop(spu_context* ctx);
extern "C" void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
extern "C" u128 spu_rdch(spu_context* ctx, uint32_t channel);
extern "C" uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel);

namespace {
constexpr int kMixerImage = 6;   // the runtime's id for fmodex_spu.elf (docs/AUDIO.md 1.1)
bool g_on = false;
std::atomic<unsigned long long> g_blocks{0}, g_plugin_calls{0}, g_runs{0};
std::atomic<int> g_reported{0};

spu_context* C(void* u) { return (spu_context*)u; }

void wrch(void* u, uint32_t ch, uint32_t v) { spu_wrch(C(u), ch, spu_make_preferred_u32(v)); }
uint32_t rdch(void* u, uint32_t ch) { return spu_rdch(C(u), ch)._u32[0]; }
uint32_t rchcnt(void* u, uint32_t ch) { return spu_rchcnt(C(u), ch); }

// As the lift does at a stop: the code and status set, spu_stop (lv2's services: 0x110 receives the block event
// into the inbound mailbox and sets the status back to running), on only when it is running again.
int stop(void* u, uint32_t code, uint32_t pc, uint32_t block)
{
    (void)pc;
    spu_context* ctx = C(u);
    ctx->pc = block;
    ctx->stop_code = code;
    ctx->status = SPU_STATUS_STOPPED_BY_STOP;
    spu_stop(ctx);
    if (code == 0x110 && ctx->status == SPU_STATUS_RUNNING) { g_blocks++; return 1; }
    return ctx->status == SPU_STATUS_RUNNING;
}

// An indirect call or jump into a plugin: the runtime's overlay dispatch, as the lift's indirect branch does
// (the native-overlay hook answers for codec_raw and sfxreverb), then its drain to the link.
int branch(void* u, fmod_mixer::Branch kind, uint32_t target, uint32_t site, uint32_t* ret_pc)
{
    spu_context* ctx = C(u);
    if (kind != fmod_mixer::Branch::overlay_call && kind != fmod_mixer::Branch::overlay_jump) {
        fprintf(stderr, "[fmod-mixer] an indirect branch the port does not place (kind %u) to 0x%05X at 0x%05X: the "
                "thread ends\n", (unsigned)kind, target, site);
        return 0;
    }
    const uint32_t link = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
    ctx->pc = target & SPU_LS_MASK;
    spu_indirect_branch(ctx);
    spu_drain_call(ctx, link);
    if (ctx->status != SPU_STATUS_RUNNING) return 0;
    g_plugin_calls++;
    *ret_pc = link;
    return 1;
}

void unsupported(void* u, uint32_t pc)
{
    fprintf(stderr, "[fmod-mixer] an instruction the lift could not translate ran at 0x%05X (img %d)\n", pc,
            C(u)->image_id);
}

void mixer_thread(spu_context* ctx)
{
    g_runs++;
    const fmod_mixer::Host host{ ctx, wrch, rdch, rchcnt, stop, branch, unsupported };
    fmod_mixer::Outcome o;
    const fmod_mixer::Result r = fmod_mixer::call(ctx->ls, ctx->gpr, ctx->pc & SPU_LS_MASK, host, &o);
    switch (r) {
    case fmod_mixer::Result::stopped:   // stop 0x102 (the thread's exit) or 0: the status is the stop's
    case fmod_mixer::Result::ended:     // a terminate (the stop service ended it) or a branch refused
        return;
    case fmod_mixer::Result::halted:
        fprintf(stderr, "[spu] HALT-ASSERT heqi pc=0x%05X img=%d (the mixer, native)\n", o.pc, ctx->image_id);
        ctx->status = SPU_STATUS_STOPPED_BY_HALT;
        spu_halt(ctx);
        return;
    case fmod_mixer::Result::handled:
        fprintf(stderr, "[fmod-mixer] the mixer's entry returned (to 0x%05X): the thread ends\n", o.ret_pc);
        ctx->status = SPU_STATUS_STOPPED;
        return;
    default:
        fprintf(stderr, "[fmod-mixer] the native mixer ended badly (result %d, pc 0x%05X): the thread stops\n",
                (int)r, o.pc);
        ctx->status = SPU_STATUS_STOPPED_BY_HALT;
        spu_halt(ctx);
        return;
    }
}
}  // namespace

extern "C" void fmod_mixer_native_report(void)
{
    if (!g_on || g_reported.exchange(1)) return;
    fprintf(stderr, "[fmod-mixer] native: %llu run(s), %llu blocks (block events received), %llu plugin calls\n",
            g_runs.load(), g_blocks.load(), g_plugin_calls.load());
}

extern "C" void fmod_mixer_native_init(void)
{
    const char* e = getenv("DOTHEROES_NATIVE_FMOD_MIXER");
    g_on = e && *e && strcmp(e, "0") != 0;
    if (!g_on) return;
    spu_thread_set_native(kMixerImage, mixer_thread);
    atexit(fmod_mixer_native_report);
    fprintf(stderr, "[fmod-mixer] FMOD's mixer (image %d): native\n", kMixerImage);
}
