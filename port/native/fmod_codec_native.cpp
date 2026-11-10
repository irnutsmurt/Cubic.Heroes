// FMOD's codec_raw overlay (SPU image 9) run natively in the game (docs/AUDIO.md step A1;
// fmod_codec_raw.h for the port itself): the runtime's native-overlay hook (spu_overlay_set_native,
// ps3recomp 0136) calls this on the FMOD mixer's indirect branches into the resident overlay.
// DOTHEROES_NATIVE_FMOD_CODEC: unset or 0 the lifted overlay (until checked in the game); 1 native;
// check: at each call the native one on a copy of the local store and the registers (its DMA a GET
// stand-in from guest memory, the decrementer the runtime's), then the lifted one run to its return,
// then the two compared (the whole local store, r0, r1, r3, r80-r127).
#include "fmod_codec_raw.h"
#include "spu_context.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" uint8_t* vm_base;
extern "C" void spu_halt(spu_context* ctx);
extern "C" void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);   // spu_channels.c
extern "C" u128 spu_rdch(spu_context* ctx, uint32_t channel);

namespace {
enum Mode { kOff, kNative, kCheck };
int g_mode = kOff;
std::atomic<unsigned long long> g_handled{0}, g_declined{0}, g_halted{0}, g_checked{0}, g_equal{0}, g_diff{0},
    g_check_lifted_only{0}, g_diff_source{0}, g_diff_same{0}, g_clock_only{0};
thread_local int t_inner;
unsigned g_lines = 12;

void wrch_real(void* u, uint32_t ch, uint32_t v) { spu_wrch((spu_context*)u, ch, spu_make_preferred_u32(v)); }
uint32_t rdch_real(void* u, uint32_t ch) { return spu_rdch((spu_context*)u, ch)._u32[0]; }

// The check's DMA stand-in: the overlay's only command is a GET; tags complete at once.
struct CopyMfc {
    spu_context* ctx;
    uint8_t* ls;
    uint32_t lsa = 0, eal = 0, size = 0, mask = 0;
    bool odd = false;
    // the GETs made, with the bytes read: re-read after the lifted call, to tell a source another thread
    // rewrote in between (FMOD's stream) from a different result on the same input
    struct Get { uint32_t lsa, ea, size; std::vector<uint8_t> bytes; };
    std::vector<Get> gets;
};
void wrch_copy(void* u, uint32_t ch, uint32_t v)
{
    CopyMfc& m = *(CopyMfc*)u;
    switch (ch) {
    case fmod_codec_raw::kMfcLsa: m.lsa = v; break;
    case fmod_codec_raw::kMfcEah: break;
    case fmod_codec_raw::kMfcEal: m.eal = v; break;
    case fmod_codec_raw::kMfcSize: m.size = v; break;
    case fmod_codec_raw::kMfcTagId: break;
    case fmod_codec_raw::kMfcCmd:
        if ((v & 0xFF) == 0x40 && m.size <= 0x4000 && (m.lsa & 0x3FFFF) + m.size <= 0x40000) {
            memcpy(m.ls + (m.lsa & 0x3FFFF), vm_base + m.eal, m.size);
            m.gets.push_back({ m.lsa & 0x3FFFF, m.eal, m.size, std::vector<uint8_t>(vm_base + m.eal, vm_base + m.eal + m.size) });
        } else
            m.odd = true;
        break;
    case fmod_codec_raw::kMfcWrTagMask: m.mask = v; break;
    case fmod_codec_raw::kMfcWrTagUpdate: break;
    default: m.odd = true;
    }
}
uint32_t rdch_copy(void* u, uint32_t ch)
{
    CopyMfc& m = *(CopyMfc*)u;
    if (ch == fmod_codec_raw::kMfcRdTagStat) return m.mask;
    if (ch == fmod_codec_raw::kSpuRdDec) return spu_rdch(m.ctx, ch)._u32[0];
    m.odd = true;
    return 0;
}

bool same(const u128& a, const u128& b) { return !memcmp(&a, &b, sizeof a); }

int native_call(spu_context* ctx, int image, uint32_t link, uint32_t delta)
{
    using namespace fmod_codec_raw;
    if (image != 9 || t_inner || g_mode == kOff) return 0;
    if (link != kEntryRead && link != kEntrySetPosition && link != kEntryDescriptor) return 0;
    if (g_mode == kNative) {
        const Channels ch{ ctx, wrch_real, rdch_real };
        uint32_t hpc = 0;
        const Result r = call(ctx->ls, ctx->gpr, ctx->pc, delta, ch, &hpc);
        if (r == Result::declined) { g_declined++; return 0; }
        if (r == Result::halted) {
            g_halted++;
            fprintf(stderr, "[spu] HALT-ASSERT heqi pc=0x%05X img=%d (codec_raw, native)\n", hpc, ctx->image_id);
            ctx->status = SPU_STATUS_STOPPED_BY_HALT;
            spu_halt(ctx);
            return 1;
        }
        g_handled++;
        ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
        return 1;
    }
    // check: the native call on copies, then the lifted one on the context, then the two compared
    static thread_local std::vector<uint8_t> ls(0x40000);
    static thread_local u128 g[128];
    memcpy(ls.data(), ctx->ls, 0x40000);
    memcpy(g, ctx->gpr, sizeof g);
    CopyMfc m;
    m.ctx = ctx;
    m.ls = ls.data();
    const Channels ch{ &m, wrch_copy, rdch_copy };
    uint32_t hpc = 0;
    const Result r = call(ls.data(), g, ctx->pc, delta, ch, &hpc);
    const uint32_t ret = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
    t_inner = 1;
    spu_indirect_branch(ctx);   // the lifted function (this hook declines inside)
    spu_drain_call(ctx, ret);
    t_inner = 0;
    if (r != Result::handled || m.odd) { g_check_lifted_only++; return 1; }
    g_checked++;
    // The read pacing's clock (fmod_codec_raw.cpp really_read): the milliseconds accumulated at delta + 0x2F10
    // and the previous decrementer at delta + 0x2F20, from the host's time, which the two runs read at
    // different moments. Compared apart: everything else must be equal.
    const uint32_t clk = (delta + 0x2F10u) & 0x3FFFFu;
    if (clk + 0x20 <= 0x40000 && memcmp(ls.data() + clk, ctx->ls + clk, 0x20)) {
        g_clock_only++;
        memcpy(ls.data() + clk, ctx->ls + clk, 0x20);
    }
    bool eq = !memcmp(ls.data(), ctx->ls, 0x40000) && same(g[0], ctx->gpr[0]) && same(g[1], ctx->gpr[1]) &&
              same(g[3], ctx->gpr[3]);
    for (int i = 80; i < 128 && eq; i++) eq = same(g[i], ctx->gpr[i]);
    if (eq) { g_equal++; return 1; }
    g_diff++;
    // the source rewritten between the two runs (another thread's), or the same input giving another result
    bool source_changed = false;
    for (const auto& gt : m.gets)
        if (memcmp(gt.bytes.data(), vm_base + gt.ea, gt.size)) { source_changed = true; break; }
    (source_changed ? g_diff_source : g_diff_same)++;
    if (g_lines && (!source_changed || g_lines > 6)) {
        g_lines--;
        uint32_t first = 0, last = 0, n = 0;
        for (uint32_t i = 0; i < 0x40000; i++)
            if (ls[i] != ctx->ls[i]) { if (!n++) first = i; last = i; }
        fprintf(stderr, "[codec-raw-check] call at link 0x%03X differs: %u local-store byte(s) in 0x%05X-0x%05X, r3 %08X "
                "native %08X lifted; %zu GET(s), the source %s\n", link, n, first, last, g[3]._u32[0], ctx->gpr[3]._u32[0],
                m.gets.size(), source_changed ? "rewritten between the runs" : "the same in both");
    }
    return 1;
}
}  // namespace

extern "C" void fmod_codec_native_report(void)
{
    static std::atomic<int> once{0};
    if (g_mode == kOff || once.exchange(1)) return;
    if (g_mode == kNative)
        fprintf(stderr, "[codec-raw] native: %llu calls, %llu declined to the lifted overlay, %llu halts\n",
                g_handled.load(), g_declined.load(), g_halted.load());
    else
        fprintf(stderr, "[codec-raw-check] %llu calls compared: %llu equal, %llu differing (%llu with the source rewritten "
                "between the two runs, %llu on the same source); the pacing clock read at another moment in %llu "
                "(compared apart); %llu run lifted only (the native one declined, halted or met a transfer the check "
                "does not model)\n", g_checked.load(), g_equal.load(), g_diff.load(), g_diff_source.load(),
                g_diff_same.load(), g_clock_only.load(), g_check_lifted_only.load());
}

extern "C" void fmod_codec_native_init(void)
{
    const char* e = getenv("DOTHEROES_NATIVE_FMOD_CODEC");
    if (!e || !*e || !strcmp(e, "0")) return;
    g_mode = !strcmp(e, "check") ? kCheck : kNative;
    spu_overlay_set_native(native_call);
    atexit(fmod_codec_native_report);
    fprintf(stderr, "[codec-raw] FMOD's codec_raw overlay: %s\n", g_mode == kNative ? "native" : "check (native beside lifted)");
}
