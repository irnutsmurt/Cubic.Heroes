// The native sfxreverb (fmod_sfxreverb.cpp) against the lifted overlay
// (build/gen/spu/spu_0024_at_00FC7180/spu_recomp.c), offline. Both run on
// copies of the same world -- a 256 KB local store holding the overlay image
// loaded as the mixer loads it (text and data GET to 0x80 + delta, bss zeroed
// or left with garbage), the DSP unit's 32 KB object (FMOD's DSPI, the
// reverb's state at this+0x170, its LS delay rings) in front of it, the
// mixer's in/out sample buffers and the caller's stack; a register file
// (every register and every lane random, then the arguments); a guest region
// (the reverb's delay lines in main memory) and a channel model shared by
// both sides (the MFC GET and PUT done as issued, as the runtime does; tag
// status = the mask). The lifted function is entered as the mixer's bisl does
// (ctx->ls_reloc = delta), through a stand-in for the runtime's drain loop.
// Each call compares
//   - every byte of the local store (the reverb object and its LS rings, the
//     stack frames -- 18.7 KB of them -- the sample buffers, the bss);
//   - every byte of the guest region (the PUTs into the delay lines);
//   - the channel sequence (every wrch and rdch, channel and value, in order);
//   - the outcome: handled = the lifted function returned; halted = the
//     lifted code halted (heqi) at the same link PC (read from its
//     HALT-ASSERT line, stderr redirected to a file);
//   - all 128 registers, all four words (the port keeps the whole register
//     file as the lift does: fmod_sfxreverb.h), after a return and at a halt;
// and fails on any channel the header says the overlay does not use, an MFC
// command other than GET and PUT, an indirect branch or a stop in the lifted run.
//
//   fmod_sfxreverb_check [<random cases> [<streams> [<calls per stream> [<elf>]]]]
//       handpicked cases (the mixer's two entries and the internal ones, on
//       the game's shape -- 256 samples, 8 channels -- and on edge values:
//       other channel counts and lengths, unaligned objects, buffers and stack,
//       the halts of each DMA check, the tag limit, parameter and delay
//       changes, denormal / NaN / extended-range samples), <random cases>
//       (default 2000) from random states, and <streams> (default 24) of
//       <calls per stream> (default 150) reads on persistent worlds: the
//       reverb's state -- its object, its LS rings and its delay lines in
//       main memory -- carried from call to call, new audio each call, and
//       now and then new parameters (the PPU's setParameter between blocks).
//   SFXREVERB_TRACE=1: a line per case. SFXREVERB_DMA=1: each call's transfers.
//   SFXREVERB_PEAKS=1 (and SFXREVERB_REGIONS=1): each stream call's output peak
//   (and the largest value in each part of the reverb's state).
//
// The states [C: the fields the code reads, fmod_sfxreverb.h; the values H, no
// capture of the game's yet]: the game's shape is 256-sample blocks, 8
// channels, delay rings sized in powers of two with their EAs 16-byte aligned
// and positions, delays and second taps in whole quadwords (as 256-sample
// blocks keep them), 8 late lines; "steady" = each smoothed value already at
// its target, so a call only crossfades and interpolates after a parameter
// change. Floats in [0, 0.6], except where that is not a reverb: the late
// lines' feedback gains are kept under 0.35 (the matrix's three butterfly
// stages can gain up to (1 + c)^3), the input biquad's poles inside the unit
// circle; the plain-audio streams then stay bounded (output peak about 4).
// Random scenes also take negative coefficients, unaligned objects and
// buffers, bad EAs, odd lengths and channel counts, extended-range values.
//
// 2026-11-10 (both builds; the generated port, fmod_sfxreverb.cpp):
//   default run, x86_64_v3 and x86_64_v2: 5235 calls, all equal -- 4231
//     handled, 1002 halted at the same PC, 2 declined (handpicked 81, random
//     2000: 1010 handled, 990 halted; streams 3156 calls, 2384 of them 256 x 8
//     reads of plain audio); 155 of the 157 blocks taken: never 0x2C44 and
//     0x2C50 (unreachable: fmod_sfxreverb.h);
//   20000 random, 60 streams of 300, x86_64_v3: 36589 calls, all equal (26663
//     handled, 9924 halted, 2 declined; 11960 game-shape reads);
//   10000 random, 30 streams of 300, x86_64_v2: 18185 calls, all equal.
//   Halts reached: 0x10E8, 0x1250, 0x1384, 0x1428, 0x1500, 0x164C, 0x17C0,
//     0x2190, 0x2728, 0x27E0 (not 0x1190, 0x16F0, 0x205C, 0x20DC, 0x2528).
// A call still running after 30 s is reported ([HANG], its name) and the run
// stops: a state whose LS ring mask passed the ring's 4,096 floats did that
// (the code then wrote over its own frame's loop bounds), so the random
// scenes keep that mask in range.
//
// Built with the bundled Zig, from the repository root (-mcpu x86_64_v3 as the
// game is built here; x86_64_v2, the shipped level, takes the helpers' other
// paths); OUT a scratch folder (here F:/3d dot game heroes/scratch/sfxreverb):
//   build/toolchain/zig-x86_64-windows-0.17.0/zig.exe cc -target x86_64-windows-gnu -mcpu=x86_64_v3 -O2 -c
//       -I build/ref/ps3recomp/runtime/spu -I build/ref/ps3recomp/include
//       build/gen/spu/spu_0024_at_00FC7180/spu_recomp.c -o OUT/sfxreverb_lift_v3.o
//   build/toolchain/zig-x86_64-windows-0.17.0/zig.exe c++ -target x86_64-windows-gnu -mcpu=x86_64_v3 -O2 -std=c++17
//       -DFMOD_SFXREVERB_COVERAGE -I port/native -I build/ref/ps3recomp/runtime/spu -I build/ref/ps3recomp/include
//       tests/native/fmod_sfxreverb_check.cpp port/native/fmod_sfxreverb.cpp OUT/sfxreverb_lift_v3.o -o OUT/fmod_sfxreverb_check_v3.exe
//   OUT/fmod_sfxreverb_check_v3.exe
// (the same with x86_64_v2 and _v2 names for the shipped level).
#define __USE_MINGW_SETJMP_NON_SEH   // setjmp(b) = _setjmp(b, NULL): longjmp restores registers, no SEH unwind
#include <setjmp.h>

#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "fmod_sfxreverb.h"
#include "spu_context.h"

namespace sr = fmod_sfxreverb;

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

constexpr uint32_t kLs = 0x40000;

// ---- a world: everything one call may touch ----------------------------------------
struct ChanOp {
    char kind;      // 'W' wrch, 'R' rdch
    uint32_t ch, v; // the channel; the value written or returned
    bool operator!=(const ChanOp& o) const { return kind != o.kind || ch != o.ch || v != o.v; }
};
struct World {
    std::vector<uint8_t> ls;
    uint8_t* live = nullptr;   // the local store the running side uses (ls, or the lifted context's)
    u128 gpr[128];
    uint32_t guest_base = 0;
    std::vector<uint8_t> guest;
    // the channel model
    uint32_t mfc_lsa = 0, mfc_eah = 0, mfc_eal = 0, mfc_size = 0, mfc_tag = 0, tag_mask = 0, tag_update = 0;
    std::vector<ChanOp> log;
    unsigned unexpected = 0, outside = 0, gets = 0, puts = 0;
    std::string first_unexpected;
};

static void unexpected(World& w, const char* what, uint32_t v)
{
    if (!w.unexpected++) {
        char m[96];
        snprintf(m, sizeof m, "%s %X", what, v);
        w.first_unexpected = m;
    }
}

static uint32_t model_rdch(World& w, uint32_t ch)
{
    uint32_t v = 0;
    if (ch == MFC_RdTagStat) v = w.tag_mask;   // every transfer is done when issued
    else unexpected(w, "rdch", ch);
    w.log.push_back({ 'R', ch, v });
    return v;
}

static void model_wrch(World& w, uint32_t ch, uint32_t v)
{
    w.log.push_back({ 'W', ch, v });
    switch (ch) {
    case MFC_LSA: w.mfc_lsa = v; break;
    case MFC_EAH: w.mfc_eah = v; break;
    case MFC_EAL: w.mfc_eal = v; break;
    case MFC_Size: w.mfc_size = v; break;
    case MFC_TagID: w.mfc_tag = v; break;
    case MFC_WrTagMask: w.tag_mask = v; break;
    case MFC_WrTagUpdate: w.tag_update = v; break;
    case MFC_Cmd: {
        if (v != MFC_GET_CMD && v != MFC_PUT_CMD) { unexpected(w, "MFC command", v); break; }
        const uint64_t ea = (uint64_t)w.mfc_eah << 32 | w.mfc_eal;
        const uint32_t n = w.mfc_size;
        const bool inside = ea >= w.guest_base && ea + n <= (uint64_t)w.guest_base + w.guest.size();
        if (!inside) w.outside++;
        if (v == MFC_GET_CMD) {
            w.gets++;
            for (uint32_t i = 0; i < n; i++)
                w.live[(w.mfc_lsa + i) & (kLs - 1)] = inside ? w.guest[(size_t)(ea - w.guest_base) + i] : 0;
        } else {
            w.puts++;
            if (inside)
                for (uint32_t i = 0; i < n; i++) w.guest[(size_t)(ea - w.guest_base) + i] = w.live[(w.mfc_lsa + i) & (kLs - 1)];
        }
        break;
    }
    default: unexpected(w, "wrch", ch); break;
    }
}

// the port's channels
static void port_wrch(void* u, uint32_t ch, uint32_t v) { model_wrch(*(World*)u, ch, v); }
static uint32_t port_rdch(void* u, uint32_t ch) { return model_rdch(*(World*)u, ch); }

// ---- the lifted side: the stand-in runtime ------------------------------------------
static World* g_world;
static jmp_buf g_jb;
static int g_end;   // 0 returned, 1 halt, 2 stop, 3 indirect branch

extern "C" {
#define LIFTED(a) void spu_0024_at_00FC7180_spu_func_##a(spu_context* ctx);
LIFTED(00002CA8) LIFTED(00002C70) LIFTED(00002D88) LIFTED(00002D30) LIFTED(00000820) LIFTED(00000090)
LIFTED(00002D98) LIFTED(00002D20) LIFTED(00002CB0)
#undef LIFTED

__thread void (*g_spu_trampoline_fn)(spu_context*) = nullptr;
volatile int g_spu_direct = 1;     // SPU_DIRECT's tail calls, as in the game; the drain loop runs the rest
uint8_t g_spu_stack_reset_hint[1024];
int g_spu_ls_watch_n = 0;

void spu_indirect_branch(spu_context*) { g_end = 3; longjmp(g_jb, 1); }   // nothing reachable branches indirectly
void spu_indirect_branch_mt(spu_context* ctx) { spu_indirect_branch(ctx); }
void spu_stop(spu_context*) { g_end = 2; longjmp(g_jb, 1); }
void spu_halt(spu_context*) { g_end = 1; longjmp(g_jb, 1); }
void spu_img_restore(spu_context*, int32_t) {}
void spu_register_function(uint32_t, void (*)(spu_context*)) {}
void spu_drain_call(spu_context* ctx, uint32_t)
{
    while (g_spu_trampoline_fn) {
        void (*f)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = nullptr;
        f(ctx);
    }
}
void spu_ls_watch_slow(uint32_t, int, const uint8_t*, uint32_t, uint32_t) {}
int ps3_strict(void) { return 0; }
void ps3_strict_fail(const char* kind, const char*, ...) { printf("%s (not expected)\n", kind); exit(4); }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
u128 spu_rdch(spu_context*, uint32_t ch)
{
    u128 r = u128();
    r._u32[0] = model_rdch(*g_world, ch);
    return r;
}
#pragma clang diagnostic pop
uint32_t spu_rchcnt(spu_context*, uint32_t ch) { unexpected(*g_world, "rchcnt", ch); return 1; }
void spu_wrch(spu_context*, uint32_t ch, u128 v) { model_wrch(*g_world, ch, v._u32[0]); }
}  // extern "C"

// The lifted code prints its halts to stderr ("[spu] HALT-ASSERT heqi pc=0x..."):
// stderr goes to a file for the whole run, read after each halt.
static FILE* g_err_reader;
static std::string g_err_path;
static void capture_stderr()
{
    g_err_path = "F:/3d dot game heroes/scratch/sfxreverb/lift_stderr_" + std::to_string(_getpid()) + ".txt";
    fflush(stderr);
    const int fd = _open(g_err_path.c_str(), _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd < 0) { printf("cannot open %s\n", g_err_path.c_str()); exit(2); }
    _dup2(fd, 2);
    _close(fd);
    g_err_reader = fopen(g_err_path.c_str(), "rb");
}
static uint32_t lifted_halt_pc()
{
    fflush(stderr);
    char buf[4096];
    uint32_t pc = 0;
    size_t n;
    std::string s;
    while ((n = fread(buf, 1, sizeof buf, g_err_reader)) > 0) s.append(buf, n);
    clearerr(g_err_reader);
    const size_t at = s.rfind("pc=0x");
    if (at != std::string::npos) pc = (uint32_t)strtoul(s.c_str() + at + 5, nullptr, 16);
    return pc;
}

static spu_context* g_ctx;
static void (*lifted_entry(uint32_t link))(spu_context*)
{
    switch (link) {
    case 0x2CA8: return spu_0024_at_00FC7180_spu_func_00002CA8;
    case 0x2C70: return spu_0024_at_00FC7180_spu_func_00002C70;
    case 0x2D88: return spu_0024_at_00FC7180_spu_func_00002D88;
    case 0x2D30: return spu_0024_at_00FC7180_spu_func_00002D30;
    case 0x0820: return spu_0024_at_00FC7180_spu_func_00000820;
    case 0x0090: return spu_0024_at_00FC7180_spu_func_00000090;
    case 0x2D98: return spu_0024_at_00FC7180_spu_func_00002D98;
    case 0x2D20: return spu_0024_at_00FC7180_spu_func_00002D20;
    case 0x2CB0: return spu_0024_at_00FC7180_spu_func_00002CB0;
    }
    return nullptr;
}

// One lifted call on w: 0 returned, 1 halted (*pc), 2 stopped, 3 an indirect branch.
static int lifted_run(World& w, uint32_t link, uint32_t delta, uint32_t* pc)
{
    if (!g_ctx) {
        g_ctx = (spu_context*)calloc(1, sizeof(spu_context));
        g_ctx->ls = g_ctx->ls_store;
    }
    spu_context* ctx = g_ctx;
    memcpy(ctx->ls, w.ls.data(), kLs);
    memcpy(ctx->gpr, w.gpr, sizeof w.gpr);
    ctx->ls_reloc = delta;
    ctx->image_id = 25;
    ctx->pc = link + delta;
    ctx->host_depth = 1;
    ctx->steps = 1;
    ctx->status = SPU_STATUS_RUNNING;
    g_world = &w;
    w.live = ctx->ls;
    g_end = 0;
    g_spu_trampoline_fn = nullptr;
    void (*fn)(spu_context*) = lifted_entry(link);
    if (setjmp(g_jb) == 0) {
        fn(ctx);
        spu_drain_call(ctx, 0);
    }
    g_spu_trampoline_fn = nullptr;
    *pc = g_end == 1 ? lifted_halt_pc() : 0;
    memcpy(w.ls.data(), ctx->ls, kLs);
    memcpy(w.gpr, ctx->gpr, sizeof w.gpr);
    return g_end;
}

// ---- the overlay image, loaded as the mixer loads it -----------------------------------
static std::vector<uint8_t> g_elf;
static void load_elf(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stdout, "cannot open %s\n", path); exit(2); }
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) g_elf.insert(g_elf.end(), buf, buf + n);
    fclose(f);
    if (g_elf.size() < 0x3010 || memcmp(g_elf.data(), "\x7F" "ELF", 4) || rd32(&g_elf[24]) != 0x2CA8) {
        printf("%s: not sfxreverb (entry %X)\n", path, g_elf.size() >= 28 ? rd32(&g_elf[24]) : 0);
        exit(2);
    }
}
// The image as the mixer GETs it: [0x80, 0x2F20) (text, rodata, .fixup; file
// offset 0x100), the gap to 0x2F80, the .ctors word and its padding
// [0x2F80, 0x2F90) (file offset 0x3000); then the bss [0x2F90, 0x3020) the
// mixer memsets (or garbage, to show nothing reads it before writing).
static void place_image(std::vector<uint8_t>& ls, uint32_t delta, bool bss_garbage, std::mt19937& rng)
{
    const uint32_t base = 0x80 + delta;
    for (uint32_t i = 0; i < 0x2EA0; i++) ls[(base + i) & (kLs - 1)] = g_elf[0x100 + i];
    for (uint32_t i = 0x2EA0; i < 0x2F00; i++) ls[(base + i) & (kLs - 1)] = 0;
    for (uint32_t i = 0; i < 0x10; i++) ls[(base + 0x2F00 + i) & (kLs - 1)] = g_elf[0x3000 + i];
    for (uint32_t i = 0x2F10; i < 0x2FA0; i++) ls[(base + i) & (kLs - 1)] = bss_garbage ? (uint8_t)rng() : 0;
}

// ---- the DSP unit and the reverb's state ---------------------------------------------------
// The DSP unit's object ("this", FMOD's DSPI with the reverb after it) sits at
// the start of the mixer's overlay area, the image right after it (the
// mixer's loader: base = LS[0x35EA0] + unit[0xD0]). The read callback gets
// the DSP state at this+0x1C; the reverb proper is at R = this+0x170.
// Fields (offsets from R; [C] read in the code, the names [H]):
//   0x000 the mono input, 256 floats at R rounded up to 16 (R+0x410 points
//         at it during a call); 0x420 nine 1 KB LS buffers for the taps
//   0x2834..0x286C, 0x2870..0x2A1F, 0x2A40..0x2B13 floats: parameters,
//         their smoothing, filter states; 0x2838, 0x2ADC, 0x2AEC flag bytes
//   0x2890 + j, 0x2C00 + j (j < 8) per-line flag bytes (0x2C00: the line's
//         delay is changing: its old position is read from main memory by a
//         4-byte DMA per sample)
//   0x2B14 the late lines in use (<= 8)
//   0x2B40 + 4k line k's delay ring in main memory (EA); 0x2B80 its delay
//         (new), 0x2BA0 (old), 0x2BC0 its second tap, 0x2C10 its mask (2^n
//         - 1), 0x2C30 its write position; 0x2C50, 0x2C70, 0x2C90 positions
//         computed at each call
//   0x2CB0 the main delay ring in main memory (EA), 0x2D80 its mask, 0x2D90
//         its write position; 0x2D00 + 4i (i < 9) the early taps (new),
//         0x2D30 (old), 0x2D60 + i their changing flags, 0x2DA0 / 0x2DD0
//         positions computed at each call
//   0x2E00 a 4096-float LS ring: mask 0x6E50, position 0x6E60, seven taps 0x6E70
//   0x6E90, 0x7690 two 512-float LS rings (allpasses): masks 0x7EC0, 0x7EC4,
//         positions 0x7ED0 / 0x7EE0, 0x7ED4 / 0x7EE4
// and this+0x24 the speaker mask (u16), this+0x8074 a gain (float), this+0x8078 a word.
constexpr uint32_t kObjSize = 0x8080;

struct LineState { uint32_t ea, delay, olddelay, tap2, mask, wp; uint8_t xfade; };
struct Reverb {
    uint32_t nlines = 8;
    LineState line[8];
    uint32_t ring_ea = 0, ring_mask = 0x7FFF, ring_wp = 0;
    uint32_t tap[9], oldtap[9];
    uint8_t tapflag[9];
    uint32_t lsmask = 0xFFF, lspos = 0, lstap[7];
    uint32_t apmask[2] = { 0x1FF, 0x1FF }, appos[4];
    float hadamard = 0.5f;      // R+0x2AC0 [H]: the late lines' mixing matrix coefficient
    float gain_hi = 0.6f;       // the other floats drawn from [0, gain_hi]
    float drygain = 1.0f;       // this+0x8074
    uint32_t word8078 = 48000;
    uint16_t speakermask = 0xFFFF;
    bool flags_random = false;  // the flag bytes random 0/1 (else 0)
    bool steady = true;         // each smoothed value at its target: no change pending
    bool negatives = false;     // an eighth of the floats negative: coefficients the PPU would not set
                                // (a negative one-pole coefficient makes the late filters grow 1.4x a sample)
};

struct Layout {
    uint32_t delta = 0;
    uint32_t self = 0x15300;     // "this": the DSP unit's object
    uint32_t in = 0x24000, out = 0x26000;
    uint32_t sp = 0x3F6A0;       // the caller's r1
    uint32_t R() const { return self + 0x170; }
};

struct Scene {
    Layout L;
    Reverb V;
    bool bss_garbage = false, reg_lanes = true;
    uint32_t guest_base = 0x31A40000, guest_size = 0x100000;
    uint32_t seed = 1;
    int audio = 0;               // 0 music-like, 1 noise, 2 with edge values, 3 silence
};

static float rnd01(std::mt19937& rng) { return (float)(rng() >> 8) * (1.0f / 16777216.0f); }

// An edge value: SPU singles have no NaN, infinity or denormal; these
// patterns are read as extended-range numbers or flushed to zero.
static uint32_t edge_value(std::mt19937& rng)
{
    static const uint32_t v[] = { 0x00000001u, 0x007FFFFFu, 0x80400000u, 0x7FC00000u, 0xFFFFFFFFu, 0x7F800000u,
                                  0xFF800001u, 0x7F7FFFFFu, 0x71000000u, 0xF1000000u, 0x80000000u, 0x00000000u,
                                  0x0D000000u, 0x3F800000u, 0xBF800000u, 0x4B000000u };
    return v[rng() % (sizeof v / sizeof v[0])];
}

static uint32_t sample(std::mt19937& rng, int audio, uint32_t i, float phase)
{
    switch (audio) {
    case 3: return 0;
    case 1: return fbits(rnd01(rng) * 2.0f - 1.0f);
    case 2: if (rng() % 16 == 0) return edge_value(rng);
            return fbits((rnd01(rng) * 2.0f - 1.0f) * (rng() % 4 ? 1.0f : 1e-30f));
    default: return fbits(0.4f * std::sin(phase + (float)i * 0.0131f) + 0.2f * std::sin(phase * 3.0f + (float)i * 0.0571f) +
                          0.05f * (rnd01(rng) - 0.5f));
    }
}

static void place_audio(World& w, const Scene& sc, uint32_t frames, uint32_t channels, std::mt19937& rng, float phase)
{
    const uint32_t n = frames * (channels ? channels : 1);
    for (uint32_t i = 0; i < n && i < 0x800; i++) wr32(&w.ls[(sc.L.in + 4 * i) & (kLs - 1)], sample(rng, sc.audio, i, phase));
}

static void wrw(std::vector<uint8_t>& ls, uint32_t at, uint32_t v) { wr32(&ls[at & (kLs - 1)], v); }

// The input's second filter [C: 0x197C, run when R+0x2864 is not 0], a biquad:
//   y = b0 x + s1;  s1 = a1 y + b1 x + s2;  s2 = a2 y + b2 x
// with b0..b2, a1, a2 at R+0x2850..0x2860 and s1, s2 at R+0x2868, 0x286C. Its
// poles are the roots of z^2 - a1 z - a2: a stable filter has them inside the
// unit circle [H: FMOD's room filter]. Random coefficients in [0, 0.6] are not
// (a1 = a2 = 0.6 has a pole at 1.13: the state grows 1.06x a sample).
static void place_biquad(std::vector<uint8_t>& ls, uint32_t R, std::mt19937& rng)
{
    const float r = 0.3f + 0.65f * rnd01(rng), th = 1.57f * rnd01(rng);
    const float a1 = 2 * r * std::cos(th), a2 = -r * r, g = (1 - a1 - a2) * 0.25f;
    const float v[] = { g, 2 * g, g, a1, a2, rng() % 4 ? 1.0f : 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < 8; i++) wrw(ls, R + 0x2850 + 4 * i, fbits(v[i]));
}

static void place_reverb(World& w, const Scene& sc, std::mt19937& rng)
{
    std::vector<uint8_t>& ls = w.ls;
    const Reverb& V = sc.V;
    const uint32_t self = sc.L.self, R = sc.L.R();
    // the DSPI part: random words; the state's instance; the speaker mask
    for (uint32_t o = 0; o < 0x170; o += 4) wrw(ls, self + o, rng());
    wrw(ls, self + 0x1C, self);
    ls[(self + 0x24) & (kLs - 1)] = (uint8_t)(V.speakermask >> 8);
    ls[(self + 0x25) & (kLs - 1)] = (uint8_t)V.speakermask;
    // the reverb: floats in [0, gain_hi] everywhere first (parameters, filter
    // states, the LS rings' contents -- a reverb tail), then the structure
    for (uint32_t o = 0; o < 0x7F00; o += 4) wrw(ls, R + o, fbits(rnd01(rng) * V.gain_hi * (V.negatives && rng() % 8 == 0 ? -1.0f : 1.0f)));
    for (uint32_t o = 0x2E00; o < 0x7EC0; o += 4) wrw(ls, R + o, fbits((rnd01(rng) - 0.5f) * 0.2f));
    wrw(ls, R + 0x2AC0, fbits(V.hadamard));
    if (!V.negatives) {
        // the late lines' feedback gains [C: 0x1DE8, the line's output times
        // R+0x2A60+4j, then times R+0x2AFC, into the matrix]: the matrix's
        // three butterfly stages can gain up to (1 + c)^3, so a stable reverb
        // keeps these well under 1 [H: FMOD derives them from the decay time]
        for (uint32_t o = 0x2A40; o < 0x2AC0; o += 4) wrw(ls, R + o, fbits(rnd01(rng) * 0.35f));
        place_biquad(ls, R, rng);
    }
    if (V.steady) {
        // the values the code compares with their targets at each call [C]:
        // equal, as after a block with no new parameters
        for (int j = 0; j < 8; j++) {
            wrw(ls, R + 0x2A80 + 4 * j, rd32(&ls[(R + 0x2A40 + 4 * j) & (kLs - 1)]));
            wrw(ls, R + 0x2900 + 4 * j, rd32(&ls[(R + 0x2870 + 4 * j) & (kLs - 1)]));
        }
        wrw(ls, R + 0x2AE8, rd32(&ls[(R + 0x2AD8) & (kLs - 1)]));
        wrw(ls, R + 0x2AF8, rd32(&ls[(R + 0x2AD4) & (kLs - 1)]));
        wrw(ls, R + 0x2844, rd32(&ls[(R + 0x2834) & (kLs - 1)]));
    }
    // flag bytes
    auto flag = [&](uint32_t at) { ls[at & (kLs - 1)] = V.flags_random ? (uint8_t)(rng() % 2) : 0; };
    flag(R + 0x2838); flag(R + 0x2ADC); flag(R + 0x2AEC);
    for (int j = 0; j < 8; j++) { flag(R + 0x2890 + j); ls[(R + 0x2C00 + j) & (kLs - 1)] = V.line[j].xfade; }
    wrw(ls, R + 0x2B14, V.nlines);
    for (int k = 0; k < 8; k++) {
        const LineState& l = V.line[k];
        wrw(ls, R + 0x2B40 + 4 * k, l.ea);
        wrw(ls, R + 0x2B80 + 4 * k, l.delay);
        wrw(ls, R + 0x2BA0 + 4 * k, l.olddelay);
        wrw(ls, R + 0x2BC0 + 4 * k, l.tap2);
        wrw(ls, R + 0x2C10 + 4 * k, l.mask);
        wrw(ls, R + 0x2C30 + 4 * k, l.wp);
        wrw(ls, R + 0x2C50 + 4 * k, (l.delay + l.wp - 1) & l.mask);
        wrw(ls, R + 0x2C70 + 4 * k, (l.olddelay + l.wp - 1) & l.mask);
        wrw(ls, R + 0x2C90 + 4 * k, (l.tap2 + l.wp - 1) & l.mask);
    }
    wrw(ls, R + 0x2CB0, V.ring_ea);
    wrw(ls, R + 0x2D80, V.ring_mask);
    wrw(ls, R + 0x2D90, V.ring_wp);
    for (int i = 0; i < 9; i++) {
        wrw(ls, R + 0x2D00 + 4 * i, V.tap[i]);
        wrw(ls, R + 0x2D30 + 4 * i, V.oldtap[i]);
        ls[(R + 0x2D60 + i) & (kLs - 1)] = V.tapflag[i];
        wrw(ls, R + 0x2DA0 + 4 * i, (V.tap[i] + V.ring_wp - 1) & (V.ring_mask & ~3u));
        wrw(ls, R + 0x2DD0 + 4 * i, (V.oldtap[i] + V.ring_wp - 1) & V.ring_mask);
    }
    wrw(ls, R + 0x6E50, V.lsmask);
    wrw(ls, R + 0x6E60, V.lspos);
    for (int k = 0; k < 7; k++) wrw(ls, R + 0x6E70 + 4 * k, V.lstap[k]);
    wrw(ls, R + 0x7EC0, V.apmask[0]);
    wrw(ls, R + 0x7EC4, V.apmask[1]);
    wrw(ls, R + 0x7ED0, V.appos[0]);
    wrw(ls, R + 0x7ED4, V.appos[1]);
    wrw(ls, R + 0x7EE0, V.appos[2]);
    wrw(ls, R + 0x7EE4, V.appos[3]);
    wrw(ls, self + 0x8074, fbits(V.drygain));
    wrw(ls, self + 0x8078, V.word8078);
}

// A plausible reverb (the game's shape as far as the code tells it: 256-sample
// blocks; delay rings sized in powers of two, positions moving 256 a block).
static Reverb game_reverb(std::mt19937& rng, uint32_t guest_base)
{
    Reverb V;
    V.ring_ea = guest_base;
    V.ring_mask = 0x7FFF;                      // 32768 samples, 128 KB
    V.ring_wp = 256 * (rng() % 128);
    for (int i = 0; i < 9; i++) {
        V.tap[i] = 200 + rng() % 20000;
        V.oldtap[i] = V.tap[i];
        V.tapflag[i] = 0;
    }
    for (int k = 0; k < 8; k++) {
        LineState& l = V.line[k];
        l.ea = guest_base + 0x20000 + 0x10000 * k;
        l.mask = (rng() % 2 ? 0x3FFF : 0x1FFF);
        l.delay = 4 * (150 + rng() % 1500);
        l.olddelay = l.delay;
        l.tap2 = k % 3 == 2 ? 4 * (rng() % 64) : 4 * (50 + rng() % 1000);   // some within a block of the write position
        l.wp = 256 * (rng() % ((l.mask + 1) / 256));
        l.xfade = 0;
    }
    V.lsmask = 0xFFF;
    V.lspos = rng() % 4096;
    for (int k = 0; k < 7; k++) V.lstap[k] = rng() % 4096;
    V.apmask[0] = 0x1FF;
    V.apmask[1] = 0xFF + (rng() % 2) * 0x100;
    for (int k = 0; k < 4; k++) V.appos[k] = rng() % 512;
    return V;
}

static void build(World& w, const Scene& sc)
{
    std::mt19937 rng(sc.seed);
    w.ls.assign(kLs, 0);
    for (uint32_t i = 0; i < kLs; i++) w.ls[i] = (uint8_t)rng();
    place_image(w.ls, sc.L.delta, sc.bss_garbage, rng);
    place_reverb(w, sc, rng);
    // the guest region: the delay lines' previous contents (a reverb tail)
    w.guest_base = sc.guest_base;
    w.guest.assign(sc.guest_size, 0);
    for (uint32_t i = 0; i + 4 <= sc.guest_size; i += 4) wr32(&w.guest[i], fbits((rnd01(rng) - 0.5f) * 0.1f));
    // registers: every one and every lane random
    for (int r = 0; r < 128; r++) for (int k = 0; k < 4; k++) w.gpr[r]._u32[k] = rng();
    w.gpr[1]._u32[0] = sc.L.sp;
    w.gpr[1]._u32[1] = 0x3F000;   // word 1: the stack space, as the ABI has it
    w.log.clear();
    w.unexpected = w.outside = w.gets = w.puts = 0;
    w.first_unexpected.clear();
    w.mfc_lsa = w.mfc_eah = w.mfc_eal = w.mfc_size = w.mfc_tag = w.tag_mask = w.tag_update = 0;
}

// ---- the calls -----------------------------------------------------------------------
struct Call {
    uint32_t link = 0;
    int nargs = 0;               // r3.. set from a[]
    u128 r0, a[8];
    Call() = default;
    explicit Call(uint32_t l) : link(l) {}
};
static u128 quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    u128 r;
    r._u32[0] = a; r._u32[1] = b; r._u32[2] = c; r._u32[3] = d;
    return r;
}
static u128 lanes(uint32_t v, std::mt19937& rng, bool garbage) { return garbage ? quad(v, rng(), rng(), rng()) : quad(v, v, v, v); }
static void apply(World& w, const Call& c)
{
    w.gpr[0] = c.r0;
    for (int i = 0; i < c.nargs; i++) w.gpr[3 + i] = c.a[i];
}

// The mixer's call (0x6118): r3 = the DSP state (this+0x1C), r4 = in, r5 = out,
// r6 = length, r7 = inchannels, r8 = outchannels (the same).
static Call call_read(const Layout& L, uint32_t length, uint32_t channels, std::mt19937& rng, bool garbage = true)
{
    Call c{ sr::kEntryRead };
    c.r0 = quad(0x611C, 0, 0, 0);
    c.nargs = 6;
    c.a[0] = lanes(L.self + 0x1C, rng, garbage);
    c.a[1] = lanes(L.in, rng, garbage);
    c.a[2] = lanes(L.out, rng, garbage);
    c.a[3] = lanes(length, rng, garbage);
    c.a[4] = lanes(channels, rng, garbage);
    c.a[5] = lanes(channels, rng, garbage);
    return c;
}
static Call call_descriptor(std::mt19937& rng, uint32_t link = sr::kEntryDescriptor)
{
    Call c{ link };
    c.r0 = quad(0xD9D8, 0, 0, 0);
    c.nargs = 4;
    for (int i = 0; i < 4; i++) c.a[i] = quad(rng(), rng(), rng(), rng());
    return c;
}
// 0x820 directly: r3 = R, r4 = in, r5 = out, r6 = channels, r7 = length,
// r8 = a float, r9 = the gain (float), r10 = the speaker mask (preferred halfword).
static Call call_process(const Layout& L, uint32_t length, uint32_t channels, float gain, uint16_t mask, std::mt19937& rng)
{
    Call c{ sr::kEntryProcess };
    c.r0 = quad(0x2D78 + L.delta, 0, 0, 0);
    c.nargs = 8;
    c.a[0] = lanes(L.R(), rng, true);
    c.a[1] = lanes(L.in, rng, true);
    c.a[2] = lanes(L.out, rng, true);
    c.a[3] = lanes(channels, rng, true);
    c.a[4] = lanes(length, rng, true);
    c.a[5] = lanes(fbits(48000.0f), rng, true);
    c.a[6] = lanes(fbits(gain), rng, true);
    c.a[7] = lanes((uint32_t)mask << 16 | (rng() & 0xFFFF), rng, true);
    return c;
}
// 0x90 directly: r3 = R (R+0x410: the mono buffer), r4 = length, r5 = channels, r6 = the source.
static Call call_downmix(const Layout& L, uint32_t length, uint32_t channels, uint32_t src, std::mt19937& rng)
{
    Call c{ sr::kEntrySampleLoop };
    c.r0 = quad(0xAA0 + L.delta, 0, 0, 0);
    c.nargs = 4;
    c.a[0] = lanes(L.R(), rng, true);
    c.a[1] = lanes(length, rng, true);
    c.a[2] = lanes(channels, rng, true);
    c.a[3] = lanes(src, rng, true);
    return c;
}
// 0x2D98 directly: r3 = LSA, r4 = {EAH, EAL}, r5 = size, r6 = tag, r7 = command.
static Call call_dma(uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag, uint32_t cmd, std::mt19937& rng)
{
    Call c{ sr::kEntryDma };
    c.r0 = quad(0x2080, 0, 0, 0);
    c.nargs = 5;
    c.a[0] = lanes(lsa, rng, true);
    c.a[1] = quad(0, ea, rng(), rng());
    c.a[2] = lanes(size, rng, true);
    c.a[3] = lanes(tag, rng, true);
    c.a[4] = lanes(cmd, rng, true);
    return c;
}
static Call call_static_init(uint32_t init, uint32_t prio, std::mt19937& rng, uint32_t link = sr::kEntryStaticInit)
{
    Call c{ link };
    c.r0 = quad(0x98, 0, 0, 0);
    c.nargs = 2;
    c.a[0] = lanes(init, rng, true);
    c.a[1] = lanes(prio, rng, true);
    return c;
}

// ---- one comparison ----------------------------------------------------------------------
struct Tally {
    unsigned calls = 0, passed = 0, handled = 0, halted = 0, declined = 0, gets = 0, puts = 0;
    unsigned long long chan_ops = 0;
} g_tally;
static double g_port_us = 0, g_lift_us = 0;
static unsigned g_timed = 0;
static bool g_game_stream = false;   // a game-shape stream with plain audio: timed apart
static double g_gs_port_us = 0, g_gs_lift_us = 0;
static unsigned g_gs_timed = 0;
static unsigned g_halt_at[0x3000 / 4];

static const char* region(const Layout& L, uint32_t a, uint32_t* off)
{
    struct { const char* name; uint32_t at, n; } r[] = {
        { "the reverb (R)", L.R(), 0x7F10 },  { "the DSP unit", L.self, kObjSize }, { "the overlay", 0x80 + L.delta, 0x2FA0 },
        { "in", L.in, 0x2000 }, { "out", L.out, 0x2000 }, { "the stack (frames)", L.sp - 0x5000, 0x5000 },
        { "the stack (caller)", L.sp, 0x400 },
    };
    for (auto& x : r)
        if (a - x.at < x.n) { *off = a - x.at; return x.name; }
    *off = a;
    return "local store";
}

// The watchdog: a call that runs for over 30 s (a state that sends the code
// round a loop it cannot leave) is reported by name, and the run stops.
static std::atomic<long long> g_call_started{ 0 };   // steady-clock ms, 0 between calls
static char g_call_name[160];
static void watchdog()
{
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const long long t = g_call_started.load();
        const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (t && now - t > 30000) {
            printf("[HANG] %s: still running after 30 s\nFAILED\n", g_call_name);
            fflush(stdout);
            _exit(3);
        }
    }
}

static bool compare(const char* name, const Layout& L, const Call& c, World& a, World& b, bool quiet)
{
    snprintf(g_call_name, sizeof g_call_name, "%s", name);
    g_call_started = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    struct Done { ~Done() { g_call_started = 0; } } done;
    a.log.clear(); b.log.clear();
    a.unexpected = b.unexpected = a.outside = b.outside = a.gets = b.gets = a.puts = b.puts = 0;
    const uint32_t entry = c.link + L.delta;
    uint32_t port_pc = 0, lift_pc = 0;
    a.live = a.ls.data();
    const auto t0 = std::chrono::steady_clock::now();
    const sr::Channels ch{ &a, port_wrch, port_rdch };
    const sr::Result res = sr::call(a.ls.data(), a.gpr, entry, L.delta, ch, &port_pc);
    const auto t1 = std::chrono::steady_clock::now();
    std::vector<std::string> why;
    int end = -1;
    if (res == sr::Result::declined) {
        g_tally.declined++;
    } else {
        end = lifted_run(b, c.link, L.delta, &lift_pc);
        const auto t2 = std::chrono::steady_clock::now();
        if (res == sr::Result::handled && end == 0 && c.link == sr::kEntryRead) {
            g_port_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
            g_lift_us += std::chrono::duration<double, std::micro>(t2 - t1).count();
            g_timed++;
            if (g_game_stream) {
                g_gs_port_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
                g_gs_lift_us += std::chrono::duration<double, std::micro>(t2 - t1).count();
                g_gs_timed++;
            }
        }
    }
    char m[256];
    if (res == sr::Result::handled) g_tally.handled++;
    if (res == sr::Result::halted) {
        g_tally.halted++;
        if (port_pc < 0x3000) g_halt_at[port_pc / 4]++;
    }
    const bool same_end = (res == sr::Result::handled && end == 0) || (res == sr::Result::halted && end == 1 && port_pc == lift_pc) ||
                          res == sr::Result::declined;
    if (!same_end) {
        snprintf(m, sizeof m, "outcome: port %s (pc %05X), lifted end %d (pc %05X)",
                 res == sr::Result::handled ? "handled" : res == sr::Result::halted ? "halted" : "declined", port_pc, end, lift_pc);
        why.push_back(m);
    }
    if (res != sr::Result::declined) {
        unsigned diff = 0;
        uint32_t first = 0;
        for (uint32_t k = 0; k < kLs; k++)
            if (a.ls[k] != b.ls[k]) { if (!diff++) first = k; }
        if (diff) {
            uint32_t off;
            const char* in = region(L, first, &off);
            snprintf(m, sizeof m, "local store: %u bytes differ, first at %05X (%s +%X): port %02X lifted %02X", diff, first, in, off,
                     a.ls[first], b.ls[first]);
            why.push_back(m);
        }
        if (a.guest != b.guest)
            for (size_t k = 0; k < a.guest.size(); k++)
                if (a.guest[k] != b.guest[k]) {
                    snprintf(m, sizeof m, "guest: first difference at %08X", a.guest_base + (uint32_t)k);
                    why.push_back(m);
                    break;
                }
        size_t i = 0;
        while (i < a.log.size() && i < b.log.size() && !(a.log[i] != b.log[i])) i++;
        if (i != a.log.size() || i != b.log.size()) {
            snprintf(m, sizeof m, "channels: %zu port, %zu lifted, first difference at #%zu (port %c%u=%X, lifted %c%u=%X)", a.log.size(),
                     b.log.size(), i, i < a.log.size() ? a.log[i].kind : '-', i < a.log.size() ? a.log[i].ch : 0,
                     i < a.log.size() ? a.log[i].v : 0, i < b.log.size() ? b.log[i].kind : '-', i < b.log.size() ? b.log[i].ch : 0,
                     i < b.log.size() ? b.log[i].v : 0);
            why.push_back(m);
        }
        if (a.unexpected || b.unexpected) why.push_back("channels not expected: " + (a.unexpected ? a.first_unexpected : b.first_unexpected));
        if (end == 2 || end == 3) why.push_back(end == 2 ? "the lifted code stopped" : "the lifted code branched indirectly");
        // every register, every lane
        for (int r = 0; r < 128; r++)
            if (memcmp(&a.gpr[r], &b.gpr[r], 16)) {
                snprintf(m, sizeof m, "r%d: port %08X %08X %08X %08X, lifted %08X %08X %08X %08X", r, a.gpr[r]._u32[0], a.gpr[r]._u32[1],
                         a.gpr[r]._u32[2], a.gpr[r]._u32[3], b.gpr[r]._u32[0], b.gpr[r]._u32[1], b.gpr[r]._u32[2], b.gpr[r]._u32[3]);
                why.push_back(m);
                break;
            }
        g_tally.gets += b.gets;
        g_tally.puts += b.puts;
        g_tally.chan_ops += b.log.size();
    } else {
        if (!a.log.empty()) why.push_back("declined but made channel operations");
        if (a.ls != b.ls || memcmp(a.gpr, b.gpr, sizeof a.gpr)) why.push_back("declined but changed the local store or registers");
    }
    g_tally.calls++;
    const bool ok = why.empty();
    g_tally.passed += ok;
    if (!ok || !quiet || getenv("SFXREVERB_TRACE")) {
        printf("[%s] %s: %s", ok ? "PASS" : "FAIL", name,
               res == sr::Result::handled ? "handled" : res == sr::Result::halted ? "halted" : "declined");
        if (res == sr::Result::halted) printf(" at %05X", port_pc);
        if (res == sr::Result::handled) printf(", r3 %08X, %zu channel ops, %u GETs, %u PUTs", a.gpr[3]._u32[0], a.log.size(), a.gets, a.puts);
        printf("\n");
        for (const std::string& s : why) printf("  %s\n", s.c_str());
    }
    if (getenv("SFXREVERB_DMA")) {
        // the transfers by command and size, and the first few in full
        std::vector<std::pair<uint64_t, unsigned>> h;
        uint32_t size = 0, lsa = 0, eal = 0;
        unsigned shown = 0;
        for (const ChanOp& o : a.log) {
            if (o.kind != 'W') continue;
            if (o.ch == MFC_Size) size = o.v;
            if (o.ch == MFC_LSA) lsa = o.v;
            if (o.ch == MFC_EAL) eal = o.v;
            if (o.ch != MFC_Cmd) continue;
            if (shown++ < 40) printf("    %s lsa %05X ea %08X size %X\n", o.v == MFC_GET_CMD ? "GET" : "PUT", lsa, eal, size);
            const uint64_t key = (uint64_t)o.v << 32 | size;
            bool found = false;
            for (auto& e : h) if (e.first == key) { e.second++; found = true; }
            if (!found) h.push_back({ key, 1 });
        }
        for (auto& e : h) printf("    %s size %X: %u\n", (e.first >> 32) == MFC_GET_CMD ? "GET" : "PUT", (uint32_t)e.first, e.second);
    }
    return ok;
}

static bool one(const char* name, const Scene& sc, const Call& c, bool quiet = false)
{
    World a;
    build(a, sc);
    apply(a, c);
    std::mt19937 arng(sc.seed * 7 + 3);
    if (c.link == sr::kEntryRead || c.link == sr::kEntryReadThis || c.link == sr::kEntryProcess)
        place_audio(a, sc, 256, 8, arng, 0.0f);
    World b = a;
    return compare(name, sc.L, c, a, b, quiet);
}

// ---- scenes ------------------------------------------------------------------------------
// The overlay area at 0x15300 (the DSP unit's 32 KB object), the image after
// it at 0x1D380 (delta 0x1D300); the sample buffers and the stack above.
static Scene game_scene(uint32_t seed)
{
    Scene sc;
    sc.seed = seed;
    sc.L.self = 0x15300;
    sc.L.delta = sc.L.self + kObjSize - 0x80;
    std::mt19937 rng(seed * 31 + 1);
    sc.V = game_reverb(rng, sc.guest_base);
    if (getenv("SFXREVERB_GAIN")) sc.V.gain_hi = (float)atof(getenv("SFXREVERB_GAIN"));
    return sc;
}

static uint32_t pick(std::mt19937& rng, std::initializer_list<uint32_t> v) { return *(v.begin() + rng() % v.size()); }

// A random scene: the game's structure with fields varied, edge values and noise.
static Scene random_scene(std::mt19937& rng, uint32_t seed)
{
    Scene sc = game_scene(seed);
    Layout& L = sc.L;
    // `this` not 16-byte aligned now and then: the code assumes it is (it reads
    // R+0x2CB0 as word 0 of its quadword), so such a call mostly halts at a GET
    L.self = rng() % 8 ? pick(rng, { 0x15300u, 0x15300u, 0x15300u, 0x14000u, 0x10000u, 0x15380u })
                       : pick(rng, { 0x15304u, 0x15308u, 0x1530Cu });
    L.delta = pick(rng, { (L.self + kObjSize - 0x80) & ~15u, (L.self + kObjSize - 0x80) & ~15u, 0x1D300u, 0x20000u, 0x0u });
    if (L.delta == 0) L.self = 0x3080;   // the image at its link address, the unit after it
    L.in = pick(rng, { 0x24000u, 0x24000u, 0x24004u, 0x25000u });
    L.out = pick(rng, { 0x26000u, 0x26000u, L.in, 0x26008u });
    L.sp = pick(rng, { 0x3F6A0u, 0x3F6A0u, 0x3F000u, 0x3F6A4u, 0x3F6A8u, 0x3FF00u });
    sc.bss_garbage = rng() % 4 == 0;
    sc.reg_lanes = rng() % 8 != 0;
    sc.audio = rng() % 4;
    Reverb& V = sc.V;
    V.nlines = pick(rng, { 8u, 8u, 8u, 4u, 0u, 1u, 7u, 9u, 0x80000000u });
    V.ring_mask = pick(rng, { 0x7FFFu, 0x7FFFu, 0x3FFFu, 0xFFFFu, 0x1FFu, 0x7FF0u });
    // Half the scenes keep what the DMA checks need (positions and delays in
    // whole quadwords, aligned EAs: they run to the end); the other half also
    // break something now and then (they mostly halt at one of the checks).
    const bool clean = rng() % 2 == 0;
    V.ring_wp = pick(rng, { V.ring_wp, V.ring_wp, (uint32_t)(4 * (rng() % 8192)), 0u,
                            clean ? 256u : (uint32_t)(rng() % 0x8000) });
    for (int i = 0; i < 9; i++) {
        V.tap[i] = pick(rng, { V.tap[i], V.tap[i], (uint32_t)rng() % 0x8000, 0u, 1u, 0xFFFFFFFFu });
        V.oldtap[i] = rng() % 3 ? V.tap[i] : (uint32_t)rng() % 0x8000;
        V.tapflag[i] = (uint8_t)(rng() % 3 ? 0 : 1 + rng() % 2);
    }
    for (int k = 0; k < 8; k++) {
        LineState& l = V.line[k];
        if (rng() % 3 == 0) l.delay = pick(rng, { l.delay + 4, clean ? l.delay : l.delay + 1, 4u * (uint32_t)(rng() % 3000), 0u, l.mask & ~3u });
        l.olddelay = rng() % 3 ? l.delay : 4 * (uint32_t)(rng() % 2000);
        if (rng() % 4 == 0) l.tap2 = pick(rng, { clean ? l.tap2 + 4 : l.tap2 + 2, 0u, 4u * (uint32_t)(rng() % 3000) });
        if (rng() % 8 == 0) l.wp = pick(rng, { l.wp + 4, clean ? l.wp + 8 : l.wp + 2, 4 * (uint32_t)(rng() % ((l.mask + 1) / 4)) });
        if (!clean && rng() % 10 == 0) l.ea = pick(rng, { l.ea + 4, l.ea + 8, 0x10000000u, l.ea + 0x100000u });
        l.xfade = (uint8_t)(rng() % 4 ? 0 : 1);
    }
    if (!clean && rng() % 10 == 0) V.ring_ea = pick(rng, { V.ring_ea + 4, V.ring_ea + 0x200000u, 0x40000000u });
    // the LS ring's mask within its 4,096 floats: a larger one overruns into the
    // allpass rings' masks and positions, and the code then writes all over
    // the local store -- its own frame's loop bounds included (a stream with
    // 0x1FFF ran one call for minutes, on both sides alike)
    V.lsmask = pick(rng, { 0xFFFu, 0xFFFu, 0x7FFu, 0x3FFu });
    V.apmask[0] = pick(rng, { 0x1FFu, 0x1FFu, 0xFFu, 0x3FFu });
    V.hadamard = rng() % 4 ? 0.5f : bitsf(rng() % 4 ? fbits(rnd01(rng)) : edge_value(rng));
    V.gain_hi = pick(rng, { 0x3F19999Au, 0x3F19999Au, 0x3F800000u, 0x3E4CCCCDu }) == 0x3F800000u ? 1.0f : 0.6f;
    V.drygain = rng() % 4 ? 1.0f : bitsf(pick(rng, { 0u, 0x3F000000u, 0x80000000u, edge_value(rng) }));
    V.word8078 = rng();
    V.speakermask = (uint16_t)pick(rng, { 0xFFFFu, 0xFFFFu, 0x00FFu, 0x0003u, 0x0000u, 0x0055u, (uint32_t)rng() & 0xFFFF });
    V.flags_random = rng() % 3 == 0;
    V.steady = rng() % 2 == 0;
    V.negatives = rng() % 2 == 0;
    return sc;
}

static Call random_call(const Scene& sc, std::mt19937& rng)
{
    const uint32_t k = rng() % 20;
    const bool g = sc.reg_lanes;
    if (k < 1) return call_descriptor(rng, rng() % 2 ? sr::kEntryDescriptor : sr::kEntryDescriptorBody);
    // up to the 256 samples of a mixer block: the mono buffer and the stack
    // buffers hold 256 (260 for a line's reads); more overruns them (the
    // handpicked 260 shows the lift and the port agreeing there too)
    const uint32_t length = pick(rng, { 256u, 256u, 256u, 256u, 128u, 64u, 4u, 0u, 252u, 3u, 255u, 8u, 100u, 200u });
    const uint32_t channels = pick(rng, { 8u, 8u, 8u, 8u, 2u, 6u, 1u, 0u, 3u, 5u, 7u, 4u, 16u });
    if (k < 15) {
        Call c = call_read(sc.L, length, channels, rng, g);
        if (k == 14) { c.link = sr::kEntryReadThis; c.a[0] = lanes(sc.L.self, rng, g); }
        return c;
    }
    if (k < 17) return call_process(sc.L, length, channels, rng() % 2 ? 1.0f : rnd01(rng), (uint16_t)rng(), rng);
    if (k < 18) return call_downmix(sc.L, length, pick(rng, { 1u, 6u, 8u, 2u, 0u, 3u }), pick(rng, { sc.L.in, sc.L.in + 4, sc.L.in + 8 }), rng);
    if (k < 19)
        return call_dma(pick(rng, { 0x30000u, 0x30004u, sc.L.R() + 0x420 }), sc.guest_base + 16 * (rng() % 0x1000) + pick(rng, { 0u, 0u, 4u }),
                        pick(rng, { 4u, 16u, 1024u, 16384u }), rng() % 32, pick(rng, { 64u, 32u }), rng);
    return call_static_init(rng() % 3 ? 1 : 0, rng() % 3 ? 0xFFFF : rng(), rng, rng() % 2 ? sr::kEntryStaticInit : sr::kEntryConstructor);
}

// The PPU's setParameter between blocks [H]: new targets for what the code
// compares and smooths at each call [C] -- the late delays, the early taps,
// the per-line values at 0x2A40 and 0x2870, the values at 0x2AD8, 0x2AD4 and
// 0x2834 -- or other gains.
static void change_parameters(World& a, World& b, const Scene& sc, std::mt19937& rng)
{
    const uint32_t R = sc.L.R();
    int what = rng() % 6;
    if (getenv("SFXREVERB_ONLY")) what = atoi(getenv("SFXREVERB_ONLY"));
    if (getenv("SFXREVERB_PEAKS")) printf("  change %d\n", what);
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    if (what == 0)
        for (int k = 0; k < 8; k++) writes.push_back({ R + 0x2B80 + 4 * k, 4 * (150 + (uint32_t)rng() % 1500) });
    else if (what == 1)
        for (int i = 0; i < 9; i++) writes.push_back({ R + 0x2D00 + 4 * i, 200 + (uint32_t)rng() % 20000 });
    else if (what == 2)
        for (int k = 0; k < 8; k++) {
            writes.push_back({ R + 0x2A40 + 4 * k, fbits(rnd01(rng) * 0.35f) });
            writes.push_back({ R + 0x2870 + 4 * k, fbits(rnd01(rng) * 0.6f) });
        }
    else if (what == 3) {
        writes.push_back({ R + 0x2AD8, fbits(rnd01(rng) * 0.6f) });
        writes.push_back({ R + 0x2AD4, fbits(rnd01(rng) * 0.6f) });
        writes.push_back({ R + 0x2834, fbits(rnd01(rng) * 0.6f) });
        std::vector<uint8_t> tmp(kLs);
        place_biquad(tmp, R, rng);
        for (uint32_t o = 0x2850; o < 0x2868; o += 4) writes.push_back({ R + o, rd32(&tmp[(R + o) & (kLs - 1)]) });
    } else if (what == 4)
        for (uint32_t o : { 0x2AC4u, 0x2AC8u, 0x2ACCu, 0x2AD0u, 0x2AFCu, 0x2B00u, 0x2B04u, 0x2B08u, 0x2B0Cu, 0x2B10u })
            writes.push_back({ R + o, fbits(rnd01(rng) * 0.6f) });
    else
        writes.push_back({ sc.L.self + 0x8074, fbits(rnd01(rng)) });
    for (auto& w : writes) { wrw(a.ls, w.first, w.second); wrw(b.ls, w.first, w.second); }
}

static float peak(const World& w, const Layout& L, uint32_t n)
{
    float p = 0;
    for (uint32_t i = 0; i < n; i++) {
        const float v = std::fabs(bitsf(rd32(&w.ls[(L.out + 4 * i) & (kLs - 1)])));
        if (v > p) p = v;
    }
    return p;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::thread(watchdog).detach();
    g_spu_ls_rdiag = 0;
    g_spu_ls_wdiag = 0;
    const unsigned random_cases = argc >= 2 ? (unsigned)atoi(argv[1]) : 2000u;
    const unsigned streams = argc >= 3 ? (unsigned)atoi(argv[2]) : 24u;
    const unsigned per_stream = argc >= 4 ? (unsigned)atoi(argv[3]) : 150u;
    load_elf(argc >= 5 ? argv[4] : "build/gen/spu/images/spu_0024_at_00FC7180.elf");
    capture_stderr();
    std::mt19937 rng(20261110);

    // ---- handpicked ------------------------------------------------------------------
    {
        Scene sc = game_scene(1);
        one("descriptor (the ELF entry, the game's load)", sc, call_descriptor(rng));
        one("descriptor's body (0x2C70)", sc, call_descriptor(rng, sr::kEntryDescriptorBody));
        Scene s2 = sc;
        s2.bss_garbage = true;
        one("descriptor, bss with garbage", s2, call_descriptor(rng));
        Scene s3 = sc;
        s3.L.sp = 0x3F6A4;
        one("descriptor, r1 not 16-byte aligned", s3, call_descriptor(rng));
        Scene s4 = sc;
        s4.L.delta = 0;
        s4.L.self = 0x3080;
        one("descriptor at the link address", s4, call_descriptor(rng));
        Call bad = call_descriptor(rng);
        bad.link = 0x2E00;
        one("0x2E00 (rodata the lift took for code): declined", sc, bad);
        bad.link = 0x2CAC;
        one("not an entry (0x2CAC): declined", sc, bad);
        one("constructor (0x2D20)", sc, call_static_init(1, 0xFFFF, rng, sr::kEntryConstructor));
        one("static init (1, 0xFFFF)", sc, call_static_init(1, 0xFFFF, rng));
        one("static init (0, 0xFFFF): nothing", sc, call_static_init(0, 0xFFFF, rng));
        one("static init (1, 5): nothing", sc, call_static_init(1, 5, rng));
    }
    {
        Scene sc = game_scene(2);
        one("read 256 x 8 (the game's shape)", sc, call_read(sc.L, 256, 8, rng));
        one("read 256 x 8, registers without lane garbage", sc, call_read(sc.L, 256, 8, rng, false));
        for (uint32_t ch : { 1u, 2u, 6u, 3u, 5u, 7u, 4u }) {
            char name[64];
            snprintf(name, sizeof name, "read 256 x %u", ch);
            one(name, sc, call_read(sc.L, 256, ch, rng));
        }
        one("read 256 x 0 (no channels)", sc, call_read(sc.L, 256, 0, rng));
        for (uint32_t len : { 0u, 4u, 64u, 128u, 252u, 3u, 255u, 260u }) {
            char name[64];
            snprintf(name, sizeof name, "read %u x 8", len);
            one(name, sc, call_read(sc.L, len, 8, rng));
        }
        Scene se = sc;
        se.audio = 2;
        one("read 256 x 8, denormal / NaN / extended-range samples", se, call_read(se.L, 256, 8, rng));
        se.audio = 3;
        one("read 256 x 8, silence", se, call_read(se.L, 256, 8, rng));
        Scene sio = sc;
        sio.L.out = sio.L.in;
        one("read 256 x 8, in and out the same buffer", sio, call_read(sio.L, 256, 8, rng));
        Scene su = sc;
        su.L.in = 0x24004;
        su.L.out = 0x26008;
        one("read 256 x 8, buffers not 16-byte aligned", su, call_read(su.L, 256, 8, rng));
        one("read 256 x 2, buffers not 16-byte aligned", su, call_read(su.L, 256, 2, rng));
        Scene s1 = sc;
        s1.L.sp = 0x3F6A8;
        one("read 256 x 8, r1 not 16-byte aligned", s1, call_read(s1.L, 256, 8, rng));
        Scene so = sc;
        so.L.self = 0x15304;
        one("read 256 x 8, the object not 16-byte aligned", so, call_read(so.L, 256, 8, rng));
        Scene sm = sc;
        sm.V.speakermask = 0x0003;
        one("read 256 x 8, speaker mask 0x3", sm, call_read(sm.L, 256, 8, rng));
        sm.V.speakermask = 0;
        one("read 256 x 8, speaker mask 0", sm, call_read(sm.L, 256, 8, rng));
        Scene sd = sc;
        sd.V.drygain = 0.0f;
        one("read 256 x 8, gain 0", sd, call_read(sd.L, 256, 8, rng));
        Scene sx = sc;
        for (auto& l : sx.V.line) { l.olddelay = l.delay + 64; l.xfade = 1; }
        one("read 256 x 8, the late delays changing (per-sample DMA)", sx, call_read(sx.L, 256, 8, rng));
        Scene sxa = sc;
        for (auto& l : sxa.V.line) l.delay += 400;
        one("read 256 x 8, new late delays", sxa, call_read(sxa.L, 256, 8, rng));
        Scene st = sc;
        for (int i = 0; i < 9; i++) st.V.tap[i] += 100 * (i + 1);
        one("read 256 x 8, new early taps", st, call_read(st.L, 256, 8, rng));
        Scene sw = sc;
        sw.V.ring_wp = 100;   // the taps' reads wrap: two GETs each
        one("read 256 x 8, main ring reads wrapping", sw, call_read(sw.L, 256, 8, rng));
        Scene sl = sc;
        for (auto& l : sl.V.line) l.wp = 8;
        one("read 256 x 8, line reads wrapping (tags past 31: halts)", sl, call_read(sl.L, 256, 8, rng));
        Scene sn = sc;
        sn.V.nlines = 4;
        one("read 256 x 8, 4 late lines", sn, call_read(sn.L, 256, 8, rng));
        sn.V.nlines = 0;
        one("read 256 x 8, no late lines", sn, call_read(sn.L, 256, 8, rng));
        Scene sh = sc;
        sh.V.hadamard = bitsf(0x7F800000u);
        one("read 256 x 8, an extended-range matrix coefficient", sh, call_read(sh.L, 256, 8, rng));
        Scene sf = sc;
        sf.V.flags_random = true;
        one("read 256 x 8, flag bytes set", sf, call_read(sf.L, 256, 8, rng));
        Scene sa = sc;
        sa.V.ring_ea += 4;
        one("read, main ring EA not 16-byte aligned (halts at a GET)", sa, call_read(sa.L, 256, 8, rng));
        Scene sb = sc;
        sb.V.line[3].ea += 8;
        one("read, a line's EA not 16-byte aligned (halts)", sb, call_read(sb.L, 256, 8, rng));
        Scene sp = sc;
        sp.V.ring_wp += 2;
        one("read, main write position not a multiple of 4 (halts at the PUT)", sp, call_read(sp.L, 256, 8, rng));
        Scene sq = sc;
        sq.V.line[5].wp += 2;
        one("read, a line's write position not a multiple of 4 (halts)", sq, call_read(sq.L, 256, 8, rng));
        Scene sz = sc;
        sz.L.delta = 0;
        sz.L.self = 0x3080;
        one("read 256 x 8 at the link address", sz, call_read(sz.L, 256, 8, rng));
        Scene sg = sc;
        sg.bss_garbage = true;
        one("read 256 x 8, bss with garbage", sg, call_read(sg.L, 256, 8, rng));
        Call c0 = call_read(sc.L, 256, 8, rng);
        c0.a[1] = quad(0, 1, 2, 3);
        one("read with in = 0 (the mixer's call at 0x2878 passes no input)", sc, c0);
        Scene sB = sc;
        sB.V.ring_wp = 0;
        for (int i = 0; i < 9; i++) sB.V.tap[i] = sB.V.oldtap[i] = i < 8 ? 100 : 5000;   // 8 taps' reads wrap: 17 tags
        for (auto& l : sB.V.line) { l.wp = 0; l.delay = l.olddelay = 100; }               // every line's read wraps
        one("read, tags past 31 at a line's second GET (halts)", sB, call_read(sB.L, 256, 8, rng));
        Scene sC = sc;
        sC.V.ring_wp = 1024;
        for (int i = 0; i < 9; i++) sC.V.tap[i] = sC.V.oldtap[i] = 4000 + 100 * i;
        for (auto& l : sC.V.line) { l.wp = 0; l.delay = l.olddelay = 2000; l.tap2 = 100; }   // only the channel taps wrap
        one("read, every channel tap's read wrapping (16 GETs: their tags restart at 0)", sC, call_read(sC.L, 256, 8, rng));
        Call cn = call_read(sc.L, 256, 8, rng);
        cn.a[0] = quad(0, 1, 2, 3);
        one("read, DSP state null (this = 0)", sc, cn);
        Call ct = call_read(sc.L, 256, 8, rng);
        ct.link = sr::kEntryReadThis;
        ct.a[0] = lanes(sc.L.self, rng, true);
        one("DSPSfxReverb::read on this (0x2D30)", sc, ct);
        one("process (0x820) directly", sc, call_process(sc.L, 256, 8, 0.7f, 0xFFFF, rng));
        one("process (0x820), 2 channels, mask 0x5", sc, call_process(sc.L, 256, 2, 1.0f, 0x5, rng));
    }
    {
        Scene sc = game_scene(3);
        for (uint32_t ch : { 1u, 6u, 8u, 2u, 0u, 3u }) {
            for (uint32_t src : { sc.L.in, sc.L.in + 4 }) {
                char name[80];
                snprintf(name, sizeof name, "downmix (0x90) 256 x %u from %05X", ch, src);
                Scene s = sc;
                std::mt19937 r2(ch * 10 + src);
                World a;
                build(a, s);
                place_audio(a, s, 256, 8, r2, 1.0f);
                wrw(a.ls, s.L.R() + 0x410, ch == 1 && src != s.L.in ? s.L.R() + 8 : s.L.R());   // the mono buffer, as 0x820 sets it
                const Call c = call_downmix(s.L, ch == 1 ? 255 : 256, ch, src, rng);
                apply(a, c);
                World b = a;
                compare(name, s.L, c, a, b, false);
            }
        }
        one("downmix 0 samples", sc, call_downmix(sc.L, 0, 8, sc.L.in, rng));
        for (int back = 0; back < 2; back++) {
            // mode 1 is a memmove: the buffers overlapping, the copy goes backwards
            World a;
            build(a, sc);
            const uint32_t src = sc.L.R() + 0x40;
            wrw(a.ls, sc.L.R() + 0x410, back ? src + 4 : src - 8);
            const Call c = call_downmix(sc.L, 100, 1, src, rng);
            apply(a, c);
            World b = a;
            compare(back ? "downmix x 1, overlapping (memmove backwards)" : "downmix x 1, overlapping (forwards)", sc.L, c, a, b, false);
        }
        one("DMA helper GET 1 KB", sc, call_dma(0x30000, sc.guest_base + 0x1000, 1024, 3, 64, rng));
        one("DMA helper PUT 16 KB", sc, call_dma(0x30000, sc.guest_base + 0x2000, 16384, 31, 32, rng));
        one("DMA helper GET 4 bytes", sc, call_dma(0x30004, sc.guest_base + 0x1004, 4, 0, 64, rng));
    }

    // ---- random ------------------------------------------------------------------------
    {
        unsigned equal = 0;
        const unsigned handled0 = g_tally.handled, halted0 = g_tally.halted;
        for (unsigned n = 0; n < random_cases; n++) {
            const Scene sc = random_scene(rng, 1000 + n);
            const Call c = random_call(sc, rng);
            char name[64];
            snprintf(name, sizeof name, "random %u (entry %X)", n + 1, c.link);
            if (getenv("SFXREVERB_SHOW") && (unsigned)atoi(getenv("SFXREVERB_SHOW")) == n + 1) {
                printf("case %u: self %X delta %X in %X out %X sp %X nlines %X ringmask %X wp %X lsmask %X ap %X/%X steady %d audio %d\n",
                       n + 1, sc.L.self, sc.L.delta, sc.L.in, sc.L.out, sc.L.sp, sc.V.nlines, sc.V.ring_mask, sc.V.ring_wp, sc.V.lsmask,
                       sc.V.apmask[0], sc.V.apmask[1], sc.V.steady, sc.audio);
                for (int i = 0; i < c.nargs; i++) printf("  r%d = %X\n", 3 + i, c.a[i]._u32[0]);
                for (int k = 0; k < 8; k++)
                    printf("  line %d: ea %X delay %X old %X tap2 %X mask %X wp %X\n", k, sc.V.line[k].ea, sc.V.line[k].delay,
                           sc.V.line[k].olddelay, sc.V.line[k].tap2, sc.V.line[k].mask, sc.V.line[k].wp);
            }
            equal += one(name, sc, c, true);
        }
        printf("[%s] random: %u of %u equal (%u handled, %u halted)\n", equal == random_cases ? "PASS" : "FAIL", equal, random_cases,
               g_tally.handled - handled0, g_tally.halted - halted0);
    }

    // ---- streams: persistent worlds, as a reverb in a dungeon -----------------------------------
    {
        unsigned equal = 0, total = 0, ended_at_halt = 0, changes = 0;
        float max_peak = 0;   // of the streams with plain audio (no edge values)
        for (unsigned s = 0; s < streams; s++) {
            Scene sc = s % 6 == 5 ? random_scene(rng, 5000 + s) : game_scene(5000 + s);
            if (s % 6 != 5) {
                sc.audio = s % 4 == 3 ? 2 : s % 4 == 2 ? 1 : 0;
                sc.V.speakermask = (uint16_t)(s % 3 == 1 ? 0x00FF : 0xFFFF);
            }
            const uint32_t channels = s % 6 == 5 ? pick(rng, { 8u, 2u, 6u }) : (s % 8 == 7 ? 2u : 8u);
            World a;
            build(a, sc);
            World b = a;
            float phase = 0;
            for (unsigned k = 0; k < per_stream; k++) {
                Call c = k == 0 ? call_descriptor(rng) : call_read(sc.L, 256, channels, rng, true);
                for (int r = 2; r < 80; r++) for (int q = 0; q < 4; q++) a.gpr[r]._u32[q] = rng();
                memcpy(b.gpr, a.gpr, sizeof a.gpr);
                apply(a, c);
                apply(b, c);
                if (k) {
                    std::mt19937 arng(rng());
                    place_audio(a, sc, 256, channels, arng, phase);
                    for (uint32_t i = 0; i < 256 * channels; i++) wrw(b.ls, sc.L.in + 4 * i, rd32(&a.ls[(sc.L.in + 4 * i) & (kLs - 1)]));
                    phase += 256 * 0.0131f;
                    if (rng() % 16 == 0) { change_parameters(a, b, sc, rng); changes++; }
                }
                char name[160];
                snprintf(name, sizeof name, "stream %u call %u (entry %X; this %X delta %X, %u channels, lines %X, LS ring mask %X, audio %d)",
                         s + 1, k + 1, c.link, sc.L.self, sc.L.delta, channels, sc.V.nlines, sc.V.lsmask, sc.audio);
                const unsigned halted = g_tally.halted;
                g_game_stream = s % 6 != 5 && sc.audio < 2 && channels == 8;
                const bool ok = compare(name, sc.L, c, a, b, true);
                g_game_stream = false;
                equal += ok;
                total++;
                if (!ok) break;
                if (g_tally.halted != halted) { ended_at_halt++; break; }
                if (k && s % 6 != 5 && sc.audio < 2) { const float p = peak(a, sc.L, 256 * channels); if (p > max_peak) max_peak = p; }
                if (getenv("SFXREVERB_PEAKS") && k) {
                    printf("  stream %u call %u peak %g", s + 1, k + 1, peak(a, sc.L, 256 * channels));
                    if (getenv("SFXREVERB_REGIONS")) {
                        // the largest magnitude in each part of the reverb's state
                        struct { const char* n; uint32_t a, b; } rg[] = { { "2834", 0x2834, 0x2890 }, { "2870", 0x2870, 0x2920 },
                            { "2920", 0x2920, 0x2A20 }, { "2A40", 0x2A40, 0x2B14 }, { "ring", 0x2E00, 0x6E50 }, { "ap", 0x6E90, 0x7EC0 } };
                        for (auto& x : rg) {
                            float m = 0;
                            for (uint32_t o = x.a; o < x.b; o += 4) m = std::fmax(m, std::fabs(bitsf(rd32(&a.ls[(sc.L.R() + o) & (kLs - 1)]))));
                            printf(" %s %.3g", x.n, m);
                        }
                        float gm = 0;
                        for (size_t o = 0; o + 4 <= a.guest.size(); o += 4) gm = std::fmax(gm, std::fabs(bitsf(rd32(&a.guest[o]))));
                        printf(" guest %.3g", gm);
                    }
                    printf("\n");
                }
            }
        }
        printf("[%s] streams: %u of %u calls equal (%u streams, %u ended at a halt, %u parameter changes; output peak of the plain-audio streams %g)\n",
               equal == total ? "PASS" : "FAIL", equal, total, streams, ended_at_halt, changes, max_peak);
    }

    printf("[info] %u calls: %u passed; %u handled, %u halted (same PC), %u declined; %u GETs, %u PUTs, %llu channel ops\n",
           g_tally.calls, g_tally.passed, g_tally.handled, g_tally.halted, g_tally.declined, g_tally.gets, g_tally.puts, g_tally.chan_ops);
    if (g_timed)
        printf("[info] mean per handled read: port %.1f us, lifted (stand-in runtime) %.1f us\n", g_port_us / g_timed, g_lift_us / g_timed);
    if (g_gs_timed)
        printf("[info] the game's shape (256 x 8, plain audio, streams; %u reads): port %.1f us, lifted %.1f us\n", g_gs_timed,
               g_gs_port_us / g_gs_timed, g_gs_lift_us / g_gs_timed);
    printf("[info] halts by PC:");
    for (unsigned i = 0; i < sizeof g_halt_at / sizeof g_halt_at[0]; i++)
        if (g_halt_at[i]) printf(" %05X x%u", i * 4, g_halt_at[i]);
    printf("\n");
    const int nb = sr::block_count();
    int taken = 0;
    for (int k = 0; k < nb; k++) taken += sr::g_block_hits[k] != 0;
    printf("[info] blocks taken: %d of %d\n", taken, nb);
    if (taken < nb) {
        printf("[info] blocks never taken:");
        for (int k = 0; k < nb; k++)
            if (!sr::g_block_hits[k]) printf(" %04X (in %04X)", sr::block_address(k), sr::block_function(k));
        printf("\n");
    }
    const bool all = g_tally.passed == g_tally.calls;
    printf("%s\n", all ? "all passed" : "FAILED");
    return all ? 0 : 1;
}
