// The native codec_raw (port/native/fmod_codec_raw.cpp) against the lifted
// overlay (build/gen/spu/spu_0008_at_00FA9B80/spu_recomp.c), offline. Both run
// on copies of the same world -- a 256 KB local store holding the overlay
// image loaded as the mixer loads it (text + data GET to 0x80 + delta, bss
// zeroed or left with garbage) and a mixer-like state around it (the channel
// block with its Codec, File and wave format, the File's buffer, destination
// buffers, the caller's stack), a register file (every register and every
// lane random, then the arguments), a guest region (the sample data) and a
// channel model shared by both sides (the MFC GET done as issued, as the
// runtime does; the decrementer a per-world deterministic sequence; tag status
// = the mask). The lifted function is entered as the mixer's bisl/brsl does
// (ctx->ls_reloc = delta), through a stand-in for the runtime's drain loop.
// Each call compares
//   - every byte of the local store (the lifted code's own stack frames, the
//     File's whole-quadword fields, the bss, the buffers);
//   - every byte of the guest region;
//   - the channel sequence (every wrch and rdch, channel and value, in order);
//   - the outcome: handled = the lifted function returned; halted = the
//     lifted code halted (heqi) at the same link PC (read from its
//     HALT-ASSERT line, stderr redirected to a file);
//   - registers r0, r1, r3 and r80-r127, all four words (the volatile r2,
//     r4-r79 are counted, not compared: fmod_codec_raw.h);
// and fails on any channel the header says the overlay does not use, a
// command other than GET, an indirect branch or a stop in the lifted run.
//
//   fmod_codec_raw_check [<random cases> [<streams> [<calls per stream> [<elf>]]]]
//       handpicked cases (the mixer's three entries and the internal ones on
//       the game's shape and on edge values: zero and odd sizes, unaligned
//       buffers, stack pointers and objects, the halts, the end of the data,
//       keys, the pacing), <random cases> (default 3000) from random states,
//       and <streams> (default 24) of <calls per stream> (default 120) calls on
//       persistent worlds (descriptor, setposition, then reads to the end and
//       around, as a playing sound).
//   CODEC_RAW_TRACE=1: a line per case.
// 2026-11-10: x86_64_v3 and x86_64_v2 builds, the default run (5169 calls:
// 4228 handled, 940 halted, 1 declined) all equal; 30000 random cases and
// 60 streams of 300 (44077 calls) equal. Never taken (unreachable with this
// image's reallyRead, fmod_codec_raw.h): refill's loop for short reads, its
// length update, fill's result 21.
//
// Built with the bundled Zig, from the repository root (-mcpu x86_64_v3 as the
// game is built here; x86_64_v2, the shipped level, takes the helpers' other
// paths):
//   build/toolchain/zig-x86_64-windows-0.17.0/zig.exe cc -target x86_64-windows-gnu -mcpu=x86_64_v3 -O2 -c
//       -I build/ref/ps3recomp/runtime/spu -I build/ref/ps3recomp/include
//       build/gen/spu/spu_0008_at_00FA9B80/spu_recomp.c -o "F:/3d dot game heroes/scratch/codec_raw/codec_raw_lift_v3.o"
//   build/toolchain/zig-x86_64-windows-0.17.0/zig.exe c++ -target x86_64-windows-gnu -mcpu=x86_64_v3 -O2 -std=c++17
//       -DFMOD_CODEC_RAW_COVERAGE -I port/native -I build/ref/ps3recomp/runtime/spu -I build/ref/ps3recomp/include
//       tests/native/fmod_codec_raw_check.cpp port/native/fmod_codec_raw.cpp
//       "F:/3d dot game heroes/scratch/codec_raw/codec_raw_lift_v3.o"
//       -o "F:/3d dot game heroes/scratch/codec_raw/fmod_codec_raw_check_v3.exe"
//   "F:/3d dot game heroes/scratch/codec_raw/fmod_codec_raw_check_v3.exe"
// (the same with x86_64_v2 and _v2 names for the shipped level).
#define __USE_MINGW_SETJMP_NON_SEH   // setjmp(b) = _setjmp(b, NULL): longjmp restores registers, no SEH unwind
#include <setjmp.h>

#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "fmod_codec_raw.h"
#include "spu_context.h"

namespace cr = fmod_codec_raw;

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

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
    uint32_t dec = 0;          // the next decrementer value
    uint32_t dec_seed = 1;     // its steps
    uint32_t dec_step = 4000;  // mean ticks between reads (79.8 MHz)
    std::vector<ChanOp> log;
    unsigned unexpected = 0, outside = 0, gets = 0;
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

static uint32_t next_dec(World& w)
{
    const uint32_t v = w.dec;
    w.dec_seed = w.dec_seed * 1103515245u + 12345u;
    const uint32_t r = w.dec_seed >> 8;
    if (r % 97 == 0) w.dec += 0x10000u + r % 0x100000u;   // now and then the counter jumps up (a reload)
    else w.dec -= w.dec_step / 2 + r % (w.dec_step + 1);
    return v;
}

static uint32_t model_rdch(World& w, uint32_t ch)
{
    uint32_t v = 0;
    if (ch == MFC_RdTagStat) v = w.tag_mask;   // every transfer is done when issued
    else if (ch == SPU_RdDec) v = next_dec(w);
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
        if (v != MFC_GET_CMD) { unexpected(w, "MFC command", v); break; }
        w.gets++;
        const uint64_t ea = (uint64_t)w.mfc_eah << 32 | w.mfc_eal;
        const uint32_t n = w.mfc_size;
        const bool inside = ea >= w.guest_base && ea + n <= (uint64_t)w.guest_base + w.guest.size();
        if (!inside) w.outside++;
        for (uint32_t i = 0; i < n; i++)
            w.live[(w.mfc_lsa + i) & (kLs - 1)] = inside ? w.guest[(size_t)(ea - w.guest_base) + i] : 0;
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
#define LIFTED(a) void spu_0008_at_00FA9B80_spu_func_##a(spu_context* ctx);
LIFTED(00000218) LIFTED(000001C8) LIFTED(00000490) LIFTED(000003E0) LIFTED(000003D0) LIFTED(000002B0)
LIFTED(00000E70) LIFTED(00000550)
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
    g_err_path = "F:/3d dot game heroes/scratch/codec_raw/lift_stderr_" + std::to_string(_getpid()) + ".txt";
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
    case 0x218: return spu_0008_at_00FA9B80_spu_func_00000218;
    case 0x1C8: return spu_0008_at_00FA9B80_spu_func_000001C8;
    case 0x490: return spu_0008_at_00FA9B80_spu_func_00000490;
    case 0x3E0: return spu_0008_at_00FA9B80_spu_func_000003E0;
    case 0x3D0: return spu_0008_at_00FA9B80_spu_func_000003D0;
    case 0x2B0: return spu_0008_at_00FA9B80_spu_func_000002B0;
    case 0xE70: return spu_0008_at_00FA9B80_spu_func_00000E70;
    case 0x550: return spu_0008_at_00FA9B80_spu_func_00000550;
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
    ctx->image_id = 9;
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
    if (g_elf.size() < 0x2F20 || memcmp(g_elf.data(), "\x7F" "ELF", 4) || rd32(&g_elf[24]) != 0x218) {
        printf("%s: not codec_raw (entry %X)\n", path, g_elf.size() >= 28 ? rd32(&g_elf[24]) : 0);
        exit(2);
    }
}
// GET [0x80, 0x2EA0) of the image (text and data, file offset 0x100) to base;
// memset the bss [0x2EA0, 0x3090).
static void place_image(std::vector<uint8_t>& ls, uint32_t delta, bool bss_garbage, std::mt19937& rng)
{
    const uint32_t base = 0x80 + delta;
    memcpy(&ls[base], &g_elf[0x100], 0x2E20);
    for (uint32_t i = 0; i < 0x1F0; i++) ls[base + 0x2E20 + i] = bss_garbage ? (uint8_t)rng() : 0;
}

// ---- the mixer-like state ----------------------------------------------------------------
struct FileState {
    uint32_t vtable = 0x01234560, w10 = 0, length = 0, w18 = 0, w1c = 0;
    uint8_t key[32] = {};
    uint32_t keylen = 0, keyidx = 0;
    uint32_t bufpos = 0, bufsize = 0x100, f190 = 0, blocksize = 0x100, f1b0 = 0, pos = 0, f1d0 = 0, f1e0 = 0, start = 0;
    uint32_t f200 = 0, f210 = 0, ptr230 = 0, flags = 0;
    uint32_t buffer = 0, w344 = 0, offset = 0, ea = 0;
};
struct WaveFormat { uint32_t format = 2, channels = 2, frequency = 48000, lengthbytes = 0, lengthpcm = 0, blockalign = 4; };

struct Layout {
    uint32_t delta = cr::kGameDelta;
    uint32_t obj = 0x26000;      // the channel block (the mixer's bss)
    uint32_t codec = 0x26000 + 0x9A4, file = 0x26000 + 0x220, wf = 0x26000 + 0x5E4;
    uint32_t sp = 0x3F6A0;        // the caller's r1
    uint32_t dst = 0x30000;       // a read's destination
    uint32_t bytesread = 0x3F6C0; // the caller's local
    uint32_t data_start = 0;      // codec+0xB8
};

static void write_quad_field(std::vector<uint8_t>& ls, uint32_t at, uint32_t v, bool lanes, std::mt19937& rng)
{
    const uint32_t q = at & ~15u, slot = at & 12u;
    for (uint32_t k = 0; k < 16; k += 4) wr32(&ls[(q + k) & (kLs - 1)], lanes ? (uint32_t)rng() : 0);
    wr32(&ls[(q + slot) & (kLs - 1)], v);
}
static void wrw(std::vector<uint8_t>& ls, uint32_t at, uint32_t v)
{
    for (int k = 0; k < 4; k++) ls[(at + k) & (kLs - 1)] = (uint8_t)(v >> (24 - 8 * k));
}
static void place_file(std::vector<uint8_t>& ls, uint32_t f, const FileState& s, bool lanes, std::mt19937& rng)
{
    wrw(ls, f + 0x00, s.vtable);
    wrw(ls, f + 0x10, s.w10); wrw(ls, f + 0x14, s.length); wrw(ls, f + 0x18, s.w18); wrw(ls, f + 0x1C, s.w1c);
    for (int i = 0; i < 32; i++) ls[(f + 0x130 + i) & (kLs - 1)] = s.key[i];
    write_quad_field(ls, f + 0x150, s.keylen, lanes, rng);
    write_quad_field(ls, f + 0x160, s.keyidx, lanes, rng);
    write_quad_field(ls, f + 0x170, s.bufpos, lanes, rng);
    write_quad_field(ls, f + 0x180, s.bufsize, lanes, rng);
    write_quad_field(ls, f + 0x190, s.f190, lanes, rng);
    write_quad_field(ls, f + 0x1A0, s.blocksize, lanes, rng);
    write_quad_field(ls, f + 0x1B0, s.f1b0, lanes, rng);
    write_quad_field(ls, f + 0x1C0, s.pos, lanes, rng);
    write_quad_field(ls, f + 0x1D0, s.f1d0, lanes, rng);
    write_quad_field(ls, f + 0x1E0, s.f1e0, lanes, rng);
    write_quad_field(ls, f + 0x1F0, s.start, lanes, rng);
    write_quad_field(ls, f + 0x200, s.f200, lanes, rng);
    write_quad_field(ls, f + 0x210, s.f210, lanes, rng);
    wrw(ls, f + 0x230, s.ptr230); wrw(ls, f + 0x234, s.flags);
    wrw(ls, f + 0x340, s.buffer); wrw(ls, f + 0x344, s.w344); wrw(ls, f + 0x348, s.offset); wrw(ls, f + 0x34C, s.ea);
}

static u128 quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    u128 r;
    r._u32[0] = a; r._u32[1] = b; r._u32[2] = c; r._u32[3] = d;
    return r;
}
static u128 lanes(uint32_t v, std::mt19937& rng, bool garbage) { return garbage ? quad(v, rng(), rng(), rng()) : quad(v, v, v, v); }

// A fresh world: LS garbage, the image, the structures, random registers.
struct Scene {
    Layout L;
    FileState F;
    WaveFormat W;
    bool bss_garbage = false, field_lanes = true, reg_lanes = true;
    uint32_t guest_base = 0x31A40000, guest_size = 0x40000;
    uint32_t dec = 0x7F000000, dec_step = 4000;
    uint32_t ls_seed = 1;
};

static void build(World& w, const Scene& sc)
{
    std::mt19937 rng(sc.ls_seed);
    w.ls.assign(kLs, 0);
    for (uint32_t i = 0; i < kLs; i++) w.ls[i] = (uint8_t)rng();
    place_image(w.ls, sc.L.delta, sc.bss_garbage, rng);
    // the Codec
    wrw(w.ls, sc.L.codec + 0x20, sc.L.wf);
    wrw(w.ls, sc.L.codec + 0xB8, sc.L.data_start);
    wrw(w.ls, sc.L.codec + 0xFC, sc.L.file);
    // the wave format
    wrw(w.ls, sc.L.wf + 0x100, sc.W.format);
    wrw(w.ls, sc.L.wf + 0x104, sc.W.channels);
    wrw(w.ls, sc.L.wf + 0x108, sc.W.frequency);
    wrw(w.ls, sc.L.wf + 0x10C, sc.W.lengthbytes);
    wrw(w.ls, sc.L.wf + 0x110, sc.W.lengthpcm);
    wrw(w.ls, sc.L.wf + 0x114, sc.W.blockalign);
    place_file(w.ls, sc.L.file, sc.F, sc.field_lanes, rng);
    // the guest region: the sample data
    w.guest_base = sc.guest_base;
    w.guest.assign(sc.guest_size, 0);
    for (uint32_t i = 0; i < sc.guest_size; i++) w.guest[i] = (uint8_t)rng();
    // registers: every one and every lane random
    for (int r = 0; r < 128; r++) w.gpr[r] = quad(rng(), rng(), rng(), rng());
    w.gpr[1] = quad(sc.L.sp, 0x3F000, rng(), rng());   // word 1: the stack space, as the ABI has it
    w.dec = sc.dec;
    w.dec_seed = sc.ls_seed * 2654435761u + 7;
    w.dec_step = sc.dec_step;
    w.log.clear();
    w.unexpected = w.outside = w.gets = 0;
    w.first_unexpected.clear();
    w.mfc_lsa = w.mfc_eah = w.mfc_eal = w.mfc_size = w.mfc_tag = w.tag_mask = w.tag_update = 0;
}

// ---- the calls -----------------------------------------------------------------------
struct Call {
    uint32_t link = 0;
    u128 r0, r3, r4, r5, r6, r7;
    bool set_r7 = false;
    Call() = default;
    explicit Call(uint32_t l) : link(l) {}
};
static void apply(World& w, const Call& c)
{
    w.gpr[0] = c.r0; w.gpr[3] = c.r3; w.gpr[4] = c.r4; w.gpr[5] = c.r5; w.gpr[6] = c.r6;
    if (c.set_r7) w.gpr[7] = c.r7;
}
static Call call_descriptor(std::mt19937& rng) { Call c{ 0x218 }; c.r0 = quad(0xD6A8, 0, 0, 0); c.r3 = quad(rng(), rng(), rng(), rng()); c.r4 = c.r3; c.r5 = c.r3; c.r6 = c.r3; return c; }
static Call call_read(const Layout& L, uint32_t bytes, std::mt19937& rng, bool garbage = true, uint32_t r0 = 0x8E10)
{
    Call c{ cr::kEntryRead };
    c.r0 = quad(r0, 0, 0, 0);
    c.r3 = lanes(L.codec + 28, rng, garbage);
    c.r4 = lanes(L.dst, rng, garbage);
    c.r5 = lanes(bytes, rng, garbage);
    c.r6 = lanes(L.bytesread, rng, garbage);
    return c;
}
static Call call_setpos(const Layout& L, uint32_t position, uint32_t postype, std::mt19937& rng, bool garbage = true)
{
    Call c{ cr::kEntrySetPosition };
    c.r0 = quad(0x8BD0, 0, 0, 0);
    c.r3 = lanes(L.codec + 28, rng, garbage);
    c.r4 = lanes(rng() % 3, rng, garbage);
    c.r5 = lanes(position, rng, garbage);
    c.r6 = lanes(postype, rng, garbage);
    return c;
}
static Call call_file_read(const Layout& L, uint32_t size, uint32_t count, std::mt19937& rng)
{
    Call c{ cr::kEntryFileRead };
    c.r0 = quad(0x468 + L.delta, 0, 0, 0);
    c.r3 = lanes(L.file, rng, true);
    c.r4 = lanes(L.dst, rng, true);
    c.r5 = lanes(size, rng, true);
    c.r6 = lanes(count, rng, true);
    c.r7 = lanes(L.bytesread, rng, true);
    c.set_r7 = true;
    return c;
}
static Call call_seek(const Layout& L, uint32_t pos, uint32_t mode, std::mt19937& rng)
{
    Call c{ cr::kEntryFileSeek };
    c.r0 = quad(0x3D0 + L.delta, 0, 0, 0);
    c.r3 = lanes(L.file, rng, true);
    c.r4 = lanes(pos, rng, true);
    c.r5 = lanes(mode, rng, true);
    c.r6 = quad(rng(), rng(), rng(), rng());
    return c;
}

// ---- one comparison ----------------------------------------------------------------------
struct Tally {
    unsigned calls = 0, passed = 0, handled = 0, halted = 0, declined = 0, volatile_diff = 0, gets = 0;
    unsigned long long chan_ops = 0;
} g_tally;
static double g_port_us = 0, g_lift_us = 0;
static unsigned g_timed = 0;

static const char* region(const Layout& L, uint32_t a, uint32_t* off)
{
    struct { const char* name; uint32_t at, n; } r[] = {
        { "the File", L.file, 0x350 },        { "the Codec", L.codec, 0x100 },     { "the wave format", L.wf, 0x118 },
        { "the overlay", 0x80 + L.delta, 0x3010 }, { "the destination", L.dst, 0x2000 }, { "the stack (frames)", L.sp - 0x400, 0x400 },
        { "the stack (caller)", L.sp, 0x400 },
    };
    for (auto& x : r)
        if (a - x.at < x.n) { *off = a - x.at; return x.name; }
    *off = a;
    return "local store";
}

static bool compare(const char* name, const Layout& L, const Call& c, World& a, World& b, bool quiet)
{
    a.log.clear(); b.log.clear();
    a.unexpected = b.unexpected = a.outside = b.outside = a.gets = b.gets = 0;
    const uint32_t entry = c.link + L.delta;
    uint32_t port_pc = 0, lift_pc = 0;
    a.live = a.ls.data();
    const auto t0 = std::chrono::steady_clock::now();
    const cr::Channels ch{ &a, port_wrch, port_rdch };
    const cr::Result res = cr::call(a.ls.data(), a.gpr, entry, L.delta, ch, &port_pc);
    const auto t1 = std::chrono::steady_clock::now();
    std::vector<std::string> why;
    int end = -1;
    if (res == cr::Result::declined) {
        g_tally.declined++;
    } else {
        end = lifted_run(b, c.link, L.delta, &lift_pc);
        const auto t2 = std::chrono::steady_clock::now();
        if (res == cr::Result::handled && end == 0) {
            g_port_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
            g_lift_us += std::chrono::duration<double, std::micro>(t2 - t1).count();
            g_timed++;
        }
    }
    char m[256];
    if (res == cr::Result::handled) g_tally.handled++;
    if (res == cr::Result::halted) g_tally.halted++;
    const bool same_end = (res == cr::Result::handled && end == 0) || (res == cr::Result::halted && end == 1 && port_pc == lift_pc) ||
                          res == cr::Result::declined;
    if (!same_end) {
        snprintf(m, sizeof m, "outcome: port %s (pc %05X), lifted end %d (pc %05X)",
                 res == cr::Result::handled ? "handled" : res == cr::Result::halted ? "halted" : "declined", port_pc, end, lift_pc);
        why.push_back(m);
    }
    if (res != cr::Result::declined) {
        // the local store
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
        // the guest region
        for (size_t k = 0; k < a.guest.size(); k++)
            if (a.guest[k] != b.guest[k]) {
                snprintf(m, sizeof m, "guest: first difference at %08X", a.guest_base + (uint32_t)k);
                why.push_back(m);
                break;
            }
        // the channels
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
        // the registers (a handled call)
        if (res == cr::Result::handled && end == 0) {
            static const int kept[] = { 0, 1, 3 };
            for (int r : kept)
                if (memcmp(&a.gpr[r], &b.gpr[r], 16)) {
                    snprintf(m, sizeof m, "r%d: port %08X %08X %08X %08X, lifted %08X %08X %08X %08X", r, a.gpr[r]._u32[0], a.gpr[r]._u32[1],
                             a.gpr[r]._u32[2], a.gpr[r]._u32[3], b.gpr[r]._u32[0], b.gpr[r]._u32[1], b.gpr[r]._u32[2], b.gpr[r]._u32[3]);
                    why.push_back(m);
                }
            for (int r = 80; r < 128; r++)
                if (memcmp(&a.gpr[r], &b.gpr[r], 16)) {
                    snprintf(m, sizeof m, "r%d (callee-saved) differs", r);
                    why.push_back(m);
                    break;
                }
            for (int r = 2; r < 80; r++)
                if (r != 3 && memcmp(&a.gpr[r], &b.gpr[r], 16)) { g_tally.volatile_diff++; break; }
        }
        g_tally.gets += b.gets;
        g_tally.chan_ops += b.log.size();
    } else {
        // declined: nothing touched
        if (!a.log.empty()) why.push_back("declined but made channel operations");
        if (a.ls != b.ls || memcmp(a.gpr, b.gpr, sizeof a.gpr)) why.push_back("declined but changed the local store or registers");
    }
    g_tally.calls++;
    const bool ok = why.empty();
    g_tally.passed += ok;
    if (!ok || !quiet || getenv("CODEC_RAW_TRACE")) {
        printf("[%s] %s: %s", ok ? "PASS" : "FAIL", name,
               res == cr::Result::handled ? "handled" : res == cr::Result::halted ? "halted" : "declined");
        if (res == cr::Result::halted) printf(" at %05X", port_pc);
        if (res == cr::Result::handled) printf(", r3 %08X, %zu channel ops, %u GETs", a.gpr[3]._u32[0], a.log.size(), a.gets);
        printf("\n");
        for (const std::string& s : why) printf("  %s\n", s.c_str());
    }
    return ok;
}

// A single call on a fresh scene.
static bool one(const char* name, const Scene& sc, const Call& c, bool quiet = false)
{
    World a;
    build(a, sc);
    apply(a, c);
    World b = a;
    return compare(name, sc.L, c, a, b, quiet);
}

// ---- scenes ------------------------------------------------------------------------------
// The game's shape as far as it is known: the File's buffer at File+0x240
// (256 bytes, the loader sets it), the data's EA 16-byte aligned.
static Scene game_scene(uint32_t seed, uint32_t length = 0x23450)
{
    Scene sc;
    sc.ls_seed = seed;
    sc.F.length = length;
    sc.F.buffer = sc.L.file + 0x240;
    sc.F.ea = sc.guest_base + 0x1000;
    sc.F.bufsize = 0x100;
    sc.F.blocksize = 0x100;
    sc.F.flags = 0x1;
    sc.W.format = 2;
    sc.W.channels = 2;
    sc.W.blockalign = 4;
    sc.W.lengthbytes = length;
    sc.W.lengthpcm = length / 4;
    return sc;
}

static uint32_t pick(std::mt19937& rng, std::initializer_list<uint32_t> v) { return *(v.begin() + rng() % v.size()); }

// A random scene: fields from plausible values, edge values and noise.
static Scene random_scene(std::mt19937& rng, uint32_t seed)
{
    Scene sc;
    sc.ls_seed = seed;
    Layout& L = sc.L;
    L.delta = pick(rng, { cr::kGameDelta, cr::kGameDelta, cr::kGameDelta, 0x0u, 0x1FF80u, 0x8000u });
    L.obj = pick(rng, { 0x26000u, 0x26000u, 0x2A000u, 0x26004u, 0x26008u, 0x2600Cu });
    L.codec = L.obj + (rng() % 2 ? 0x9A4 : 0x4B24);
    L.file = L.obj + 0x220;
    L.wf = L.obj + 0x5E4;
    L.sp = pick(rng, { 0x3F6A0u, 0x3F6A0u, 0x3E000u, 0x3F6A4u, 0x3F6A8u, 0x3F6ACu, 0x3B110u });
    L.dst = pick(rng, { 0x30000u, 0x30000u, 0x31230u, 0x30004u, 0x30001u, 0x3000Fu, L.sp + 48, L.file + 0x240 });
    L.bytesread = pick(rng, { L.sp + 32, L.sp + 32, L.sp + 48, 0x2F000u, 0x2F002u, 0u });
    L.data_start = pick(rng, { 0u, 0u, 0x2Cu, 0x800u, (uint32_t)rng() % 0x1000 });
    sc.bss_garbage = rng() % 4 == 0;
    sc.field_lanes = rng() % 8 != 0;
    sc.reg_lanes = rng() % 8 != 0;
    if (rng() % 6 == 0) sc.guest_base = 0xC0100000u;   // the pacing's range
    FileState& F = sc.F;
    F.length = pick(rng, { 0x23450u, 0x1000u, 0x100u, 0x7FFu, 0xFFFFFFFFu, 0x3F000u, (uint32_t)rng() % 0x30000, 0u, 16u });
    F.w10 = rng(); F.w18 = rng(); F.w1c = rng();
    for (auto& k : F.key) k = (uint8_t)rng();
    F.keylen = rng() % 5 == 0 ? pick(rng, { 1u, 7u, 16u, 32u, (uint32_t)rng() % 33 }) : 0;
    F.keyidx = F.keylen ? rng() % (F.keylen + 1) : 0;
    F.bufsize = pick(rng, { 0x100u, 0x100u, 0x80u, 0x40u, 0x10u, 0u, 0x20u, 0x200u, (uint32_t)rng() % 0x101 });
    F.blocksize = pick(rng, { F.bufsize, F.bufsize, 0x100u, 0x80u, 0x10u, 0x30u, 0u, 0x20u, (uint32_t)rng() % 0x101 + 1 });
    F.bufpos = F.bufsize && rng() % 2 ? rng() % F.bufsize : pick(rng, { 0u, 0u, 0x10u, (uint32_t)rng() % 0x200 });
    F.f190 = pick(rng, { 0u, 0u, 0x10u, F.blocksize, (uint32_t)rng() % 0x200 });
    F.f1b0 = pick(rng, { 0u, 0u, 0x10u, 0x80u, (uint32_t)rng() % 0x200 & ~15u });
    F.pos = pick(rng, { 0u, 0u, 0x100u, F.length, F.length - 1, (uint32_t)rng() % 0x30000, 0x10u });
    F.f1d0 = pick(rng, { 0u, 0u, F.pos, F.pos + 0x100, (uint32_t)rng() % 0x30000 });
    F.f1e0 = pick(rng, { 0u, F.f1d0, F.pos, (uint32_t)rng() % 0x30000 });
    F.start = pick(rng, { 0u, 0u, 0x10u, 0x2Cu, (uint32_t)rng() % 0x1000 });
    F.f200 = rng(); F.f210 = rng();
    F.ptr230 = rng() % 3 ? 0 : rng();
    static const uint32_t bits[] = { 0x1, 0x2, 0x8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x400 };
    F.flags = 0;
    for (uint32_t b : bits) if (rng() % 4 == 0) F.flags |= b;
    if (rng() % 8 == 0) F.flags = rng();
    F.buffer = pick(rng, { L.file + 0x240, L.file + 0x240, L.file + 0x240, 0x2E000u, L.file + 0x248 });
    F.w344 = rng();
    F.offset = pick(rng, { 0u, 0u, 0x100u, F.pos, F.length, (uint32_t)rng() % 0x30000 & ~15u, (uint32_t)rng() % 0x30000 });
    F.ea = sc.guest_base + pick(rng, { 0x1000u, 0x1000u, 0x2000u, 0x1008u, 0u });
    WaveFormat& W = sc.W;
    W.format = pick(rng, { 2u, 2u, 2u, 1u, 3u, 4u, 5u, 0u });
    W.channels = pick(rng, { 1u, 2u, 2u, 6u, 8u });
    W.blockalign = W.channels * (W.format == 2 ? 2 : W.format == 1 ? 1 : 4);
    if (rng() % 16 == 0) W.blockalign = pick(rng, { 0u, 3u, 1u, (uint32_t)rng() });
    W.lengthbytes = F.length;
    W.lengthpcm = W.blockalign ? F.length / W.blockalign : F.length;
    if (rng() % 16 == 0) W.lengthpcm = pick(rng, { 0u, 1u, 0xFFFFFFFFu, (uint32_t)rng() });
    if (rng() % 16 == 0) W.lengthbytes = pick(rng, { 0xFFFFFFFFu, (uint32_t)rng(), 0x80000000u });
    sc.dec = pick(rng, { 0x7F000000u, 0x00001000u, 0xFFFFFF00u, (uint32_t)rng() });
    sc.dec_step = pick(rng, { 4000u, 40000u, 200000u, 100u });
    return sc;
}

// A stream's scene: varied but consistent (aligned data and buffer, block size
// a multiple of 16 not above the buffer size), so that it plays to the end.
static Scene stream_scene(std::mt19937& rng, uint32_t seed)
{
    Scene sc = game_scene(seed, pick(rng, { 0x23450u, 0x8000u, 0x12345u, 0x3F001u, 0x4400u, (uint32_t)rng() % 0x30000 + 0x100 }));
    FileState& F = sc.F;
    F.bufsize = pick(rng, { 0x100u, 0x100u, 0x80u, 0x40u, 0x20u, 0x10u, 0u });
    F.blocksize = F.bufsize ? pick(rng, { F.bufsize, F.bufsize, 0x10u }) : pick(rng, { 0x100u, 0x40u });
    F.flags = pick(rng, { 0x1u, 0x1u, 0x1u, 0x9u, 0x0u, 0x41u, 0x201u });
    if (!F.bufsize) F.flags &= ~1u;   // realign divides by the buffer size (halts at 0x780)
    if (rng() % 4 == 0) {
        F.keylen = pick(rng, { 1u, 5u, 16u, 32u });
        for (auto& k : F.key) k = (uint8_t)rng();
        if (rng() % 2) F.flags |= 0x400;
    }
    sc.L.data_start = pick(rng, { 0u, 0u, 0x2Cu, 0x40u });
    sc.L.dst = pick(rng, { 0x30000u, 0x30000u, 0x30008u, sc.L.sp + 48 });
    sc.W.format = pick(rng, { 2u, 2u, 1u, 5u });
    sc.W.channels = pick(rng, { 1u, 2u, 2u, 6u });
    sc.W.blockalign = sc.W.channels * (sc.W.format == 2 ? 2 : sc.W.format == 1 ? 1 : 4);
    sc.W.lengthbytes = F.length;
    sc.W.lengthpcm = F.length / sc.W.blockalign;
    sc.bss_garbage = rng() % 3 == 0;
    if (rng() % 4 == 0) { sc.guest_base = 0xC0100000u; F.ea = sc.guest_base + 0x1000; }
    sc.dec_step = pick(rng, { 4000u, 40000u, 120000u, 400000u });
    sc.dec = rng();
    return sc;
}

static Call random_call(const Scene& sc, std::mt19937& rng)
{
    const uint32_t k = rng() % 16;
    const bool g = sc.reg_lanes;
    if (k < 1) return call_descriptor(rng);
    if (k < 9) {
        const uint32_t bytes = pick(rng, { 4608u, 4608u, 2048u, 1024u, 256u, 16u, 1u, 3u, 0u, 4607u, (uint32_t)rng() % 0x2000,
                                           0x80000000u | (uint32_t)rng() % 0x100, 0xFFFFFFFEu });
        return call_read(sc.L, bytes, rng, g, rng() % 2 ? 0x8E10 : 0x8D10);
    }
    if (k < 13) {
        const uint32_t postype = pick(rng, { 2u, 2u, 2u, 8u, 8u, 1u, 4u, (uint32_t)rng() });
        const uint32_t position = pick(rng, { 0u, 0u, sc.W.lengthpcm / 2, sc.W.lengthpcm, sc.W.lengthpcm + 1, (uint32_t)rng() % 0x10000,
                                              (uint32_t)rng(), 0xFFFFFFFFu });
        return call_setpos(sc.L, position, postype, rng, g);
    }
    if (k < 15) return call_file_read(sc.L, pick(rng, { 1u, 2u, 4u, 4u, 3u, 8u }), pick(rng, { 0u, 1u, 7u, 64u, 1152u, (uint32_t)rng() % 0x800 }), rng);
    return call_seek(sc.L, pick(rng, { 0u, 0x10u, 0x100u, sc.F.length, 0xFFFFFFF0u, (uint32_t)rng() % 0x30000 }), pick(rng, { 0u, 1u, 2u, 3u, 0xFFFFFFFFu }), rng);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_spu_ls_rdiag = 0;
    g_spu_ls_wdiag = 0;
    const unsigned random_cases = argc >= 2 ? (unsigned)atoi(argv[1]) : 3000u;
    const unsigned streams = argc >= 3 ? (unsigned)atoi(argv[2]) : 24u;
    const unsigned per_stream = argc >= 4 ? (unsigned)atoi(argv[3]) : 120u;
    load_elf(argc >= 5 ? argv[4] : "build/gen/spu/images/spu_0008_at_00FA9B80.elf");
    capture_stderr();
    std::mt19937 rng(20261110);

    // ---- handpicked ------------------------------------------------------------------
    {
        Scene sc = game_scene(1);
        one("descriptor (the game's load)", sc, call_descriptor(rng));
        Scene s2 = sc;
        s2.bss_garbage = true;
        one("descriptor, bss with garbage", s2, call_descriptor(rng));
        Scene s3 = sc;
        s3.L.sp = 0x3F6A4;
        one("descriptor, r1 not 16-byte aligned", s3, call_descriptor(rng));
        Scene s4 = sc;
        s4.L.delta = 0;
        one("descriptor at the link address", s4, call_descriptor(rng));
        Call cd = call_descriptor(rng);
        cd.link = 0x1C8;
        one("descriptor's body (0x1C8)", sc, cd);
        Call bad = call_descriptor(rng);
        bad.link = 0x220;
        one("not an entry (0x220): declined", sc, bad);
    }
    {
        Scene sc = game_scene(2);
        one("read 4608 bytes PCM16 (the game's shape)", sc, call_read(sc.L, 4608, rng));
        one("read 4608 bytes PCM16, mixer's buffered path r0", sc, call_read(sc.L, 4608, rng, true, 0x8D10));
        one("read 0 bytes", sc, call_read(sc.L, 0, rng));
        one("read 1 byte (PCM16: 0 elements)", sc, call_read(sc.L, 1, rng));
        one("read 3 bytes (PCM16: 1 element)", sc, call_read(sc.L, 3, rng));
        one("read 17 bytes", sc, call_read(sc.L, 17, rng));
        one("read 0x80000000 bytes (PCM16: 0x40000000 elements -> clipped)", sc, call_read(sc.L, 0x80000000u, rng));
        Scene s8 = sc;
        s8.W.format = 1;
        one("read 4608 bytes PCM8", s8, call_read(s8.L, 4608, rng));
        one("read 0x80000001 bytes PCM8 (negative: 37)", s8, call_read(s8.L, 0x80000001u, rng));
        Scene sd = sc;
        sd.L.dst = 0;
        one("read into buffer 0 (37)", sd, call_read(sd.L, 64, rng));
        Scene su = sc;
        su.L.dst = 0x30005;
        one("read into an unaligned buffer", su, call_read(su.L, 999, rng));
        Scene ss = sc;
        ss.L.dst = ss.L.sp + 48;
        one("read into the caller's stack (the mixer's 0xA5A4)", ss, call_read(ss.L, 4608, rng));
        Scene sn = sc;
        sn.L.bytesread = 0;
        one("read with bytesread null", sn, call_read(sn.L, 512, rng));
        Scene sb = sc;
        sb.L.bytesread = sb.L.sp + 33;
        one("read with an unaligned bytesread", sb, call_read(sb.L, 512, rng));
        Scene se = sc;
        se.F.pos = se.F.length - 100;
        se.F.offset = (se.F.length - 100) & ~15u;
        one("read across the end (22)", se, call_read(se.L, 4608, rng));
        Scene sp = sc;
        sp.F.pos = sp.F.length + 4;
        one("read past the end (19)", sp, call_read(sp.L, 64, rng));
        Scene sl = sc;
        sl.F.length = 0xFFFFFFFF;
        sl.F.offset = 0x23000;
        one("read, length unknown", sl, call_read(sl.L, 4608, rng));
        Scene sf = sc;
        sf.F.flags = 0x8;
        one("read PCM16, flag 0x8 (no swap)", sf, call_read(sf.L, 1024, rng));
        Scene sk = sc;
        sk.F.keylen = 16;
        sk.F.keyidx = 3;
        for (int i = 0; i < 32; i++) sk.F.key[i] = (uint8_t)(0x5A ^ i * 37);
        one("read with a key", sk, call_read(sk.L, 1024, rng));
        sk.F.flags |= 0x400;
        one("read with a key, XOR first (0x400)", sk, call_read(sk.L, 1024, rng));
        Scene sz = sc;
        sz.F.blocksize = 0;
        one("read, block size 0 (halts)", sz, call_read(sz.L, 64, rng));
        Scene sa = sc;
        sa.F.ea = sa.guest_base + 0x1008;
        one("read, data EA not 16-byte aligned (halts at the DMA)", sa, call_read(sa.L, 64, rng));
        Scene sr = sc;
        sr.guest_base = 0xC0100000u;
        sr.F.ea = sr.guest_base + 0x1000;
        sr.dec_step = 300000;
        one("read from 0xC01xxxxx with 3.7 ms between reads (the pacing spins)", sr, call_read(sr.L, 4608, rng));
        Scene sw = sc;
        sw.F.bufsize = 0;
        sw.F.flags = 0x1;
        one("read, buffer size 0", sw, call_read(sw.L, 256, rng));
        Scene s1 = sc;
        s1.L.sp = 0x3F6A8;
        one("read, r1 not 16-byte aligned", s1, call_read(s1.L, 4608, rng));
        Scene so = sc;
        so.L.obj = 0x26004;
        so.L.codec = so.L.obj + 0x9A4;
        so.L.file = so.L.obj + 0x220;
        so.L.wf = so.L.obj + 0x5E4;
        so.F.buffer = so.L.file + 0x240;
        one("read, objects not 16-byte aligned", so, call_read(so.L, 4608, rng));
        Call cn = call_read(sc.L, 64, rng);
        cn.r3 = quad(0, 1, 2, 3);
        one("read, codec state null", sc, cn);
        Call ci = call_read(sc.L, 4608, rng);
        ci.link = 0x3E0;
        ci.r3 = lanes(sc.L.codec, rng, true);
        one("CodecRaw::read (0x3E0)", sc, ci);
    }
    {
        Scene sc = game_scene(3);
        one("setposition PCM 0 (the game's)", sc, call_setpos(sc.L, 0, 2, rng));
        one("setposition PCM half way", sc, call_setpos(sc.L, sc.W.lengthpcm / 2, 2, rng));
        one("setposition PCM at the end", sc, call_setpos(sc.L, sc.W.lengthpcm, 2, rng));
        one("setposition PCM past the end", sc, call_setpos(sc.L, sc.W.lengthpcm + 77, 2, rng));
        one("setposition raw bytes", sc, call_setpos(sc.L, 0x1234, 8, rng));
        one("setposition PCM 0xFFFFFFFF (64-bit product)", sc, call_setpos(sc.L, 0xFFFFFFFF, 2, rng));
        Scene sb = sc;
        sb.W.blockalign = 0;
        one("setposition, blockalign 0 (halts)", sb, call_setpos(sb.L, 100, 2, rng));
        Scene sp = sc;
        sp.W.lengthpcm = 0;
        one("setposition, lengthpcm 0 (halts in __udivdi3)", sp, call_setpos(sp.L, 100, 2, rng));
        Scene sh = sc;
        sh.W.lengthpcm = 0x00010001;
        sh.W.lengthbytes = 0xFFFFFFFF;
        one("setposition, a divisor above 16 bits", sh, call_setpos(sh.L, 0x12345678, 2, rng));
        Scene sk = sc;
        sk.F.keylen = 13;
        one("setposition with a key", sk, call_setpos(sk.L, 1000, 2, rng));
        Scene su = sc;
        su.F.bufsize = 0;
        one("setposition, unbuffered", su, call_setpos(su.L, 1000, 2, rng));
        Scene sn = sc;
        sn.F.flags = 0;
        sn.F.f1d0 = 0x100;
        one("setposition, not seekable", sn, call_setpos(sn.L, 5000, 2, rng));
        Call ci = call_setpos(sc.L, 999, 2, rng);
        ci.link = 0x2B0;
        ci.r3 = lanes(sc.L.codec, rng, true);
        one("CodecRaw::setPosition (0x2B0)", sc, ci);
    }
    {
        Scene sc = game_scene(4);
        for (uint32_t mode : { 0u, 1u, 2u, 3u, 0xFFFFFFFFu }) {
            char name[64];
            snprintf(name, sizeof name, "File::seek mode %d", (int)mode);
            one(name, sc, call_seek(sc.L, 0x400, mode, rng));
        }
        one("File::seek negative", sc, call_seek(sc.L, 0xFFFFFF00u, 0, rng));
        for (uint32_t size : { 1u, 2u, 3u, 4u, 8u }) {
            char name[64];
            snprintf(name, sizeof name, "File::read size %u", size);
            one(name, sc, call_file_read(sc.L, size, 1152 / size, rng));
        }
        one("File::read size 0 (halts)", sc, call_file_read(sc.L, 0, 7, rng));
        Scene s4 = sc;
        s4.F.flags = 0x8;
        one("File::read size 4, flag 0x8", s4, call_file_read(s4.L, 4, 100, rng));
    }

    // ---- random ------------------------------------------------------------------------
    {
        unsigned equal = 0;
        for (unsigned n = 0; n < random_cases; n++) {
            const Scene sc = random_scene(rng, 1000 + n);
            const Call c = random_call(sc, rng);
            char name[64];
            snprintf(name, sizeof name, "random %u (entry %X)", n + 1, c.link);
            equal += one(name, sc, c, true);
        }
        printf("[%s] random: %u of %u equal\n", equal == random_cases ? "PASS" : "FAIL", equal, random_cases);
    }

    // ---- streams: persistent worlds, as a playing sound -----------------------------------
    {
        unsigned equal = 0, total = 0, ended_at_halt = 0, ends = 0;
        for (unsigned s = 0; s < streams; s++) {
            Scene sc = s % 6 == 5 ? random_scene(rng, 5000 + s) : s == 0 ? game_scene(5000) : stream_scene(rng, 5000 + s);
            World a;
            build(a, sc);
            World b = a;
            uint32_t last = 0;   // the previous call's result
            for (unsigned k = 0; k < per_stream; k++) {
                Call c;
                if (k == 0) c = call_descriptor(rng);
                else if ((last == 22 || last == 19) && rng() % 2) c = call_setpos(sc.L, 0, 2, rng);   // a looping sound
                else if (k == 1 || rng() % 23 == 0) c = call_setpos(sc.L, k == 1 ? 0 : rng() % (sc.W.lengthpcm + 2), rng() % 5 ? 2 : 8, rng);
                else c = call_read(sc.L, pick(rng, { 4608u, 4608u, 2048u, 1536u, 4096u }), rng, true, rng() % 2 ? 0x8E10 : 0x8D10);
                // the rest of the registers: the mixer's, changing from call to call
                for (int r = 2; r < 80; r++) a.gpr[r] = quad(rng(), rng(), rng(), rng());
                memcpy(b.gpr, a.gpr, sizeof a.gpr);
                apply(a, c);
                apply(b, c);
                char name[80];
                snprintf(name, sizeof name, "stream %u call %u (entry %X)", s + 1, k + 1, c.link);
                const unsigned halted = g_tally.halted;
                const bool ok = compare(name, sc.L, c, a, b, true);
                equal += ok;
                total++;
                if (!ok) break;                        // the worlds have diverged
                if (g_tally.halted != halted) {        // the SPU stops there
                    ended_at_halt++;
                    break;
                }
                last = a.gpr[3]._u32[0];
                ends += last == 22 || last == 19;
            }
        }
        printf("[%s] streams: %u of %u calls equal (%u streams, %u ended at a halt; %u reads at the end of the data)\n",
               equal == total ? "PASS" : "FAIL", equal, total, streams, ended_at_halt, ends);
    }

    printf("[info] %u calls: %u passed; %u handled, %u halted (same PC), %u declined; %u GETs, %llu channel ops;"
           " %u with volatile registers differing (not compared)\n",
           g_tally.calls, g_tally.passed, g_tally.handled, g_tally.halted, g_tally.declined, g_tally.gets, g_tally.chan_ops,
           g_tally.volatile_diff);
    if (g_timed)
        printf("[info] mean per handled call: port %.2f us, lifted (stand-in runtime) %.2f us\n", g_port_us / g_timed, g_lift_us / g_timed);
    printf("[info] paths:");
    unsigned missing = 0;
    for (int p = 0; p < cr::kPathCount; p++) {
        printf("%s %s %llu", p ? "," : "", cr::path_name(p), cr::g_path_hits[p]);
        missing += cr::g_path_hits[p] == 0;
    }
    printf("\n");
    if (missing) {
        printf("[info] paths never taken:");
        for (int p = 0; p < cr::kPathCount; p++)
            if (!cr::g_path_hits[p]) printf(" [%s]", cr::path_name(p));
        printf("\n");
    }
    const bool all = g_tally.passed == g_tally.calls;
    printf("%s\n", all ? "all passed" : "FAILED");
    return all ? 0 : 1;
}
