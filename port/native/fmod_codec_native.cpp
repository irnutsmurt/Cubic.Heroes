// FMOD's codec_raw overlay (SPU image 9) run natively in the game (docs/AUDIO.md step A1;
// fmod_codec_raw.h for the port itself): the runtime's native-overlay hook (spu_overlay_set_native,
// ps3recomp 0136) calls this on the FMOD mixer's indirect branches into the resident overlay.
// DOTHEROES_NATIVE_FMOD_CODEC: unset or 1 native (the default since 2026-11-10: bit-exact in the check,
// the validation scenes passed with it); 0 the lifted overlay; check: at each call the native one on a copy of the local store and the registers (its DMA a GET
// stand-in from guest memory, the decrementer the runtime's), then the lifted one run to its return
// with its decrementer reads logged, then the two compared (the whole local store, r0, r1, r3,
// r80-r127). The read pacing keeps a clock in the local store from the decrementer, which the two runs
// read at different moments: when the two differ, the native one runs again from the same state with
// the lifted run's decrementer reads replayed in order, and that run is compared, clock included.
#include "fmod_codec_raw.h"
#include "fmod_sfxreverb.h"
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
// spu_channels.c: the decrementer values this host thread reads, logged into buf (cap entries) until
// spu_dec_log_end, which returns how many reads there were (it may exceed cap)
extern "C" void spu_dec_log_begin(uint32_t* buf, uint32_t cap);
extern "C" uint32_t spu_dec_log_end(void);

namespace {
enum Mode { kOff, kNative, kCheck };
int g_mode = kOff;
// Each comparison is counted once, when it is decided (a call still running at the report is not in it).
std::atomic<unsigned long long> g_handled{0}, g_declined{0}, g_halted{0}, g_equal{0}, g_equal_replayed{0},
    g_diff{0}, g_check_lifted_only{0}, g_diff_source{0}, g_diff_same{0}, g_replay_reads{0};
constexpr uint32_t kDecLogCap = 20000;   // the pacing spin reads the decrementer 17,500 times
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
    // the decrementer: the runtime's, or these values in order (the lifted run's reads, replayed)
    const uint32_t* replay = nullptr;
    uint32_t nreplay = 0, replay_i = 0;
    bool replay_over = false;
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
    if (ch == fmod_codec_raw::kSpuRdDec) {
        if (!m.replay) return spu_rdch(m.ctx, ch)._u32[0];
        if (m.replay_i < m.nreplay) return m.replay[m.replay_i++];
        m.replay_over = true;
        return 0;
    }
    m.odd = true;
    return 0;
}

bool same(const u128& a, const u128& b) { return !memcmp(&a, &b, sizeof a); }

// the native run's local store and registers against the lifted run's (the context)
bool equal_to_lifted(const uint8_t* ls, const u128* g, const spu_context* ctx)
{
    bool eq = !memcmp(ls, ctx->ls, 0x40000) && same(g[0], ctx->gpr[0]) && same(g[1], ctx->gpr[1]) &&
              same(g[3], ctx->gpr[3]);
    for (int i = 80; i < 128 && eq; i++) eq = same(g[i], ctx->gpr[i]);
    return eq;
}

int codec_call(spu_context* ctx, uint32_t link, uint32_t delta)
{
    using namespace fmod_codec_raw;
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
    static thread_local std::vector<uint8_t> pre(0x40000), ls(0x40000);
    static thread_local u128 gpre[128], g[128];
    static thread_local std::vector<uint32_t> declog(kDecLogCap);
    memcpy(pre.data(), ctx->ls, 0x40000);
    memcpy(gpre, ctx->gpr, sizeof gpre);
    memcpy(ls.data(), pre.data(), 0x40000);
    memcpy(g, gpre, sizeof g);
    const uint32_t pc = ctx->pc;
    CopyMfc m;
    m.ctx = ctx;
    m.ls = ls.data();
    uint32_t hpc = 0;
    const Result r = call(ls.data(), g, pc, delta, Channels{ &m, wrch_copy, rdch_copy }, &hpc);
    const uint32_t ret = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
    t_inner = 1;
    spu_dec_log_begin(declog.data(), kDecLogCap);
    spu_indirect_branch(ctx);   // the lifted function (this hook declines inside)
    spu_drain_call(ctx, ret);
    const uint32_t ndec = spu_dec_log_end();
    t_inner = 0;
    if (r != Result::handled || m.odd) { g_check_lifted_only++; return 1; }
    if (equal_to_lifted(ls.data(), g, ctx)) { g_equal++; return 1; }
    // The read pacing (fmod_codec_raw.cpp really_read) keeps the milliseconds between reads at delta + 0x2F10
    // and the last decrementer at delta + 0x2F20, from the time, which the two runs read at different
    // moments: the native one again from the same state, with the lifted run's reads replayed.
    bool replay_reads_ok = false;
    if (ndec <= kDecLogCap) {
        memcpy(ls.data(), pre.data(), 0x40000);
        memcpy(g, gpre, sizeof g);
        CopyMfc m2;
        m2.ctx = ctx;
        m2.ls = ls.data();
        m2.replay = declog.data();
        m2.nreplay = ndec;
        const Result r2 = call(ls.data(), g, pc, delta, Channels{ &m2, wrch_copy, rdch_copy }, &hpc);
        replay_reads_ok = !m2.replay_over && m2.replay_i == ndec;
        if (r2 == Result::handled && !m2.odd && replay_reads_ok && equal_to_lifted(ls.data(), g, ctx)) {
            g_equal_replayed++;
            return 1;
        }
    }
    g_diff++;
    if (!replay_reads_ok) g_replay_reads++;
    // the source rewritten between the runs (another thread's), or the same input giving another result
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
                "native %08X lifted; %zu GET(s), the source %s; the lifted run read the decrementer %u time(s)%s\n",
                link, n, first, last, g[3]._u32[0], ctx->gpr[3]._u32[0], m.gets.size(),
                source_changed ? "rewritten between the runs" : "the same in both", ndec,
                replay_reads_ok ? "" : ", the replay read it a different number of times");
    }
    return 1;
}

// ---- FMOD's sfxreverb (image 25; docs/AUDIO.md step A2, fmod_sfxreverb.h) ------------------------------
// DOTHEROES_NATIVE_FMOD_REVERB: unset or 1 native (the default since 2026-11-10: bit-exact in the game's
// check); 0 the lifted overlay; check:
// the native call on copies of the local store and the registers -- its GETs read guest memory, its PUTs
// kept aside (a later GET in the same call sees them) -- then the lifted one on the context, then the whole
// local store, all 128 registers and every byte the native one would have PUT compared with what the
// lifted one left. The reverb reads and writes only its own delay lines in main memory; no time, no atomics.
int g_rmode = kOff;
std::atomic<unsigned long long> g_r_handled{0}, g_r_declined{0}, g_r_halted{0}, g_r_equal{0}, g_r_diff{0},
    g_r_lifted_only{0}, g_r_puts{0};
unsigned g_r_lines = 8;

struct ShadowMfc {
    uint8_t* ls;
    uint32_t lsa = 0, eal = 0, size = 0, mask = 0;
    bool odd = false;
    struct Put { uint32_t ea, size; std::vector<uint8_t> bytes; };
    std::vector<Put> puts;
};
void wrch_shadow(void* u, uint32_t ch, uint32_t v)
{
    using namespace fmod_codec_raw;   // the MFC channel numbers (architectural)
    ShadowMfc& m = *(ShadowMfc*)u;
    switch (ch) {
    case kMfcLsa: m.lsa = v; break;
    case kMfcEah: break;
    case kMfcEal: m.eal = v; break;
    case kMfcSize: m.size = v; break;
    case kMfcTagId: break;
    case kMfcCmd: {
        const uint32_t cmd = v & 0xFF, lsa = m.lsa & 0x3FFFF;
        if (m.size > 0x4000 || lsa + m.size > 0x40000) { m.odd = true; break; }
        if (cmd == 0x40) {   // GET: guest memory, then what this call PUT over it
            memcpy(m.ls + lsa, vm_base + m.eal, m.size);
            for (const auto& p : m.puts) {
                const uint32_t lo = p.ea > m.eal ? p.ea : m.eal;
                const uint32_t hi = (p.ea + p.size < m.eal + m.size) ? p.ea + p.size : m.eal + m.size;
                if (lo < hi) memcpy(m.ls + lsa + (lo - m.eal), p.bytes.data() + (lo - p.ea), hi - lo);
            }
        } else if (cmd == 0x20) {   // PUT: kept aside
            m.puts.push_back({ m.eal, m.size, std::vector<uint8_t>(m.ls + lsa, m.ls + lsa + m.size) });
        } else {
            m.odd = true;
        }
        break;
    }
    case kMfcWrTagMask: m.mask = v; break;
    case kMfcWrTagUpdate: break;
    default: m.odd = true;
    }
}
uint32_t rdch_shadow(void* u, uint32_t ch)
{
    ShadowMfc& m = *(ShadowMfc*)u;
    if (ch == fmod_codec_raw::kMfcRdTagStat) return m.mask;
    m.odd = true;
    return 0;
}

int reverb_call(spu_context* ctx, uint32_t link, uint32_t delta)
{
    using namespace fmod_sfxreverb;
    if (link != kEntryRead && link != kEntryDescriptor) return 0;
    if (g_rmode == kNative) {
        const Channels ch{ ctx, wrch_real, rdch_real };
        uint32_t hpc = 0;
        const Result r = call(ctx->ls, ctx->gpr, ctx->pc, delta, ch, &hpc);
        if (r == Result::declined) { g_r_declined++; return 0; }
        if (r == Result::halted) {
            g_r_halted++;
            fprintf(stderr, "[spu] HALT-ASSERT heqi pc=0x%05X img=%d (sfxreverb, native)\n", hpc, ctx->image_id);
            ctx->status = SPU_STATUS_STOPPED_BY_HALT;
            spu_halt(ctx);
            return 1;
        }
        g_r_handled++;
        ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
        return 1;
    }
    static thread_local std::vector<uint8_t> ls(0x40000);
    static thread_local u128 g[128];
    memcpy(ls.data(), ctx->ls, 0x40000);
    memcpy(g, ctx->gpr, sizeof g);
    ShadowMfc m;
    m.ls = ls.data();
    uint32_t hpc = 0;
    const Result r = call(ls.data(), g, ctx->pc, delta, Channels{ &m, wrch_shadow, rdch_shadow }, &hpc);
    const uint32_t ret = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
    t_inner = 1;
    spu_indirect_branch(ctx);   // the lifted function (this hook declines inside)
    spu_drain_call(ctx, ret);
    t_inner = 0;
    if (r != Result::handled || m.odd) { g_r_lifted_only++; return 1; }
    g_r_puts += m.puts.size();
    const bool ls_eq = !memcmp(ls.data(), ctx->ls, 0x40000);
    int regs_diff = 0;
    for (int i = 0; i < 128; i++) regs_diff += !same(g[i], ctx->gpr[i]);
    size_t puts_diff = 0;
    for (const auto& p : m.puts)
        if (memcmp(p.bytes.data(), vm_base + p.ea, p.size)) puts_diff++;
    if (ls_eq && !regs_diff && !puts_diff) { g_r_equal++; return 1; }
    g_r_diff++;
    if (g_r_lines) {
        g_r_lines--;
        uint32_t first = 0, last = 0, n = 0;
        for (uint32_t i = 0; i < 0x40000; i++)
            if (ls[i] != ctx->ls[i]) { if (!n++) first = i; last = i; }
        fprintf(stderr, "[sfxreverb-check] call at link 0x%04X differs: %u local-store byte(s) in 0x%05X-0x%05X, "
                "%d register(s), %zu of %zu PUT(s)\n", link, n, first, last, regs_diff, puts_diff, m.puts.size());
    }
    return 1;
}

// The runtime's native-overlay hook: each image's port, when its switch is on.
int native_call(spu_context* ctx, int image, uint32_t link, uint32_t delta)
{
    if (t_inner) return 0;
    if (image == 9 && g_mode != kOff) return codec_call(ctx, link, delta);
    if (image == 25 && g_rmode != kOff) return reverb_call(ctx, link, delta);
    return 0;
}
}  // namespace

extern "C" void fmod_codec_native_report(void)
{
    static std::atomic<int> once{0};
    if (once.exchange(1)) return;
    if (g_rmode == kNative)
        fprintf(stderr, "[sfxreverb] native: %llu calls, %llu declined to the lifted overlay, %llu halts\n",
                g_r_handled.load(), g_r_declined.load(), g_r_halted.load());
    else if (g_rmode == kCheck)
        fprintf(stderr, "[sfxreverb-check] %llu calls compared, every byte of the local store, all 128 registers and every "
                "byte PUT (%llu PUTs): %llu equal, %llu differing; %llu run lifted only (the native one declined, halted or "
                "met a transfer the check does not model)\n", g_r_equal.load() + g_r_diff.load(), g_r_puts.load(),
                g_r_equal.load(), g_r_diff.load(), g_r_lifted_only.load());
    if (g_mode == kOff) return;
    if (g_mode == kNative)
        fprintf(stderr, "[codec-raw] native: %llu calls, %llu declined to the lifted overlay, %llu halts\n",
                g_handled.load(), g_declined.load(), g_halted.load());
    else {
        const unsigned long long eq = g_equal.load(), rep = g_equal_replayed.load(), diff = g_diff.load();
        fprintf(stderr, "[codec-raw-check] %llu calls compared, every byte of the local store and the registers: %llu "
                "equal as run, %llu equal with the lifted run's decrementer reads replayed into the native one (the "
                "read pacing's clock), %llu differing (%llu with the source rewritten between the runs, %llu on the "
                "same source; %llu where the replay read the decrementer a different number of times); %llu run "
                "lifted only (the native one declined, halted or met a transfer the check does not model)\n",
                eq + rep + diff, eq, rep, diff, g_diff_source.load(), g_diff_same.load(), g_replay_reads.load(),
                g_check_lifted_only.load());
    }
}

extern "C" void fmod_codec_native_init(void)
{
    const char* e = getenv("DOTHEROES_NATIVE_FMOD_CODEC");
    if (e && !strcmp(e, "0")) {
        g_mode = kOff;
        fprintf(stderr, "[codec-raw] FMOD's codec_raw overlay: lifted (DOTHEROES_NATIVE_FMOD_CODEC=0)\n");
    } else {
        g_mode = e && !strcmp(e, "check") ? kCheck : kNative;
        fprintf(stderr, "[codec-raw] FMOD's codec_raw overlay: %s\n", g_mode == kNative ? "native" : "check (native beside lifted)");
    }
    const char* r = getenv("DOTHEROES_NATIVE_FMOD_REVERB");
    g_rmode = r && !strcmp(r, "0") ? kOff : r && !strcmp(r, "check") ? kCheck : kNative;
    fprintf(stderr, "[sfxreverb] FMOD's sfxreverb overlay: %s\n", g_rmode == kNative ? "native" : g_rmode == kCheck ?
            "check (native beside lifted)" : "lifted (DOTHEROES_NATIVE_FMOD_REVERB=0)");
    if (g_mode == kOff && g_rmode == kOff) return;
    spu_overlay_set_native(native_call);
    atexit(fmod_codec_native_report);
}
