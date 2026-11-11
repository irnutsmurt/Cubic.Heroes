// The native waver task (port/native/waver_task.cpp: the host around the task's code generated from the
// lift, port/native/spu_native.h) against the lifted one
// (build/gen/spu/spu_0001_at_00F48F80/spu_recomp.c), offline. The lifted
// image runs whole, from its entry (LS 0x3050) to its exit through the SPURS
// kernel's exit entry, on a bare SPU context with its ELF loaded afresh in
// local store for each run (a new task each frame, as in the game) and a
// stand-in MFC that does each GET and PUT as it is issued (the runtime's
// order); the native run_frame runs on a copy of the same guest memory. Each
// case compares
//   - every byte of the guest region both leave (the parameter block, the
//     field, both textures -- preset to patterns -- and the gaps around them);
//   - the transfer sequence (direction, EA, size, in order);
//   - the outcome: native done = the lifted task exited with code 0 through
//     the kernel's exit entry; native unsupported = the lifted task halted;
//   - for a run that is done, the lifted task's transfers, local-store
//     addresses, tags and tag waits against the order the task was read to make (below);
// and fails on any channel or MFC command waver_task.h says the task does
// not use (atomics, mailboxes, list DMA, an EA above 4 GB).
//
//   waver_task_check [<random cases> [<frames> [<elf>]]]
//       handpicked cases (the game's shape, edge parameters, edge tables and
//       maps, overlapping buffers, the halting EAs, local store left with
//       garbage), <random cases> (default 40) drawn from the edge values, and
//       <frames> (default 160) of the game's evolution: phase and radius from
//       the elapsed time (the radius capped at 0x80), the amplitude fading,
//       the texture alternating, on persistent memory.
//   WAVER_TRACE=1: a line at each run's end (how the lifted task ended, its
//   transfers so far).
// 2026-11-10: x86_64_v3 and x86_64_v2 builds, 249 runs each (89 cases, 160
// frames) all equal; 1000 random cases and 600 frames (1649 runs) equal.
//
// Built with the bundled Zig, from the repository root (-mcpu x86_64_v3 as
// the game is built here, tools/zig_build.py host_cpu; x86_64_v2 is the
// shipped level, PORTABLE_CPU, and takes the helpers' other paths):
//   build/toolchain/zig-x86_64-windows-0.17.0/zig.exe cc -target x86_64-windows-gnu -mcpu=x86_64_v3 -O2 -c
//       -I build/ref/ps3recomp/runtime/spu -I build/ref/ps3recomp/include
//       build/gen/spu/spu_0001_at_00F48F80/spu_recomp.c -o <scratch>/waver_lift_v3.o
//   python tools/gen_spu_native.py tools/spu_native/waver.py build/gen/spu/spu_0001_at_00F48F80/spu_recomp.c
//       build/gen/spu/images/spu_0001_at_00F48F80.elf tools/spu_native_task.cpp.in <scratch>/waver_gen.cpp
//   build/toolchain/zig-x86_64-windows-0.17.0/zig.exe c++ -target x86_64-windows-gnu -mcpu=x86_64_v3 -O2 -std=c++17
//       -I port/native -I build/ref/ps3recomp/runtime/spu -I build/ref/ps3recomp/include
//       tests/native/waver_task_check.cpp port/native/waver_task.cpp port/native/spu_native.cpp
//       <scratch>/waver_gen.cpp <scratch>/waver_lift_v3.o -o <scratch>/waver_task_check_v3.exe
//   <scratch>/waver_task_check_v3.exe
// (the same with x86_64_v2 and _v2 names for the shipped level).
#define __USE_MINGW_SETJMP_NON_SEH   // setjmp(b) = _setjmp(b, NULL): longjmp restores registers, no SEH unwind
#include <setjmp.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "spu_context.h"
#include "waver_task.h"

using waver_task::Params;

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// ---- a guest region: everything a case's run may touch ------------------------
struct Xfer {
    char kind;     // 'G' GET, 'P' PUT, 'W' a tag wait (rdch MFC_RdTagStat)
    uint32_t ea;   // G/P: the EA; W: the tag mask
    uint32_t n;    // G/P: the size; W: the update type
    uint32_t lsa;  // G/P: the local-store address (the lifted side only)
    uint32_t tag;  // G/P: the tag (the lifted side only)
};
struct Space {
    uint32_t base = 0;
    std::vector<uint8_t> mem;
    std::vector<Xfer> log;
    unsigned outside = 0;
    uint8_t* at(uint32_t ea, uint32_t n)
    {
        if (ea >= base && (uint64_t)ea + n <= (uint64_t)base + mem.size()) return mem.data() + (ea - base);
        if (!outside++) printf("  transfer outside the region: ea %08X, %u bytes\n", ea, n);
        return nullptr;
    }
};

// ---- the native side's transfers ---------------------------------------------
static void space_get(void* u, void* dst, uint32_t ea, uint32_t n)
{
    Space* s = (Space*)u;
    s->log.push_back({ 'G', ea, n, 0, 0 });
    const uint8_t* src = s->at(ea, n);
    if (src) memcpy(dst, src, n); else memset(dst, 0, n);
}
static void space_put(void* u, uint32_t ea, const void* src, uint32_t n)
{
    Space* s = (Space*)u;
    s->log.push_back({ 'P', ea, n, 0, 0 });
    if (uint8_t* d = s->at(ea, n)) memcpy(d, src, n);
}

// ---- the lifted side: the stand-in runtime ------------------------------------
// The SPURS kernel's data the exit reads (LS 0x3920): word 2 of LS 0x2FB0 is
// a pointer, the word at that pointer + 196 the exit entry. Made up here.
constexpr uint32_t kKernelPtr = 0x100, kKernelExit = 0x2A00;

static Space* g_space;
static jmp_buf g_jb;           // a halt, the exit and a stop end the run, as the runtime's longjmp does
static int g_end;              // 1 halt, 2 the exit entry, 3 stop, 4 returned
static uint32_t g_end_pc, g_exit_code, g_tag_mask, g_tag_update;
static unsigned g_unexpected;
static void unexpected(const char* what, uint32_t v)
{
    if (!g_unexpected++) printf("  unexpected: %s %X\n", what, v);
}

extern "C" {
void spu_0001_at_00F48F80_spu_func_00003050(spu_context* ctx);

__thread void (*g_spu_trampoline_fn)(spu_context*) = nullptr;
volatile int g_spu_direct = 0;          // always the trampoline: the drain loop below runs every transfer
uint8_t g_spu_stack_reset_hint[1024];
int g_spu_ls_watch_n = 0;

static void end_run(int why, spu_context* ctx)
{
    g_end = why;
    g_end_pc = ctx->pc;
    if (getenv("WAVER_TRACE")) printf("  end %d at pc %05X, %zu transfers\n", why, ctx->pc, g_space->log.size());
    longjmp(g_jb, 1);
}
void spu_indirect_branch(spu_context* ctx)   // the task's only computed branches: the exit (and atexit handlers: none)
{
    g_exit_code = rd32(ctx->ls + 0x2FD0);
    end_run(2, ctx);
}
void spu_indirect_branch_mt(spu_context* ctx) { spu_indirect_branch(ctx); }
void spu_stop(spu_context* ctx) { end_run(3, ctx); }
void spu_halt(spu_context* ctx) { end_run(1, ctx); }
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
    if (ch == MFC_RdTagStat) {   // every transfer is done when issued: the masked tags, complete
        g_space->log.push_back({ 'W', g_tag_mask, g_tag_update, 0, 0 });
        r._u32[0] = g_tag_mask;
        return r;
    }
    unexpected("rdch", ch);
    return r;
}
#pragma clang diagnostic pop
uint32_t spu_rchcnt(spu_context*, uint32_t ch) { unexpected("rchcnt", ch); return 1; }
void spu_wrch(spu_context* ctx, uint32_t ch, u128 v)
{
    const uint32_t x = v._u32[0];
    switch (ch) {
    case MFC_LSA: ctx->mfc_lsa = x; break;
    case MFC_EAH: ctx->mfc_eah = x; break;
    case MFC_EAL: ctx->mfc_eal = x; break;
    case MFC_Size: ctx->mfc_size = x; break;
    case MFC_TagID: ctx->mfc_tag = x; break;
    case MFC_WrTagMask: g_tag_mask = x; break;
    case MFC_WrTagUpdate: g_tag_update = x; break;
    case MFC_Cmd: {
        const uint32_t lsa = ctx->mfc_lsa & SPU_LS_MASK, n = ctx->mfc_size;
        if (ctx->mfc_eah) unexpected("EAH", ctx->mfc_eah);
        if (x != MFC_GET_CMD && x != MFC_PUT_CMD) { unexpected("MFC command", x); break; }
        if (lsa + n > SPU_LS_SIZE) { unexpected("LS range past the end at", lsa); break; }
        g_space->log.push_back({ x == MFC_GET_CMD ? 'G' : 'P', ctx->mfc_eal, n, lsa, ctx->mfc_tag });
        uint8_t* m = g_space->at(ctx->mfc_eal, n);
        if (x == MFC_GET_CMD) { if (m) memcpy(ctx->ls + lsa, m, n); else memset(ctx->ls + lsa, 0, n); }
        else if (m) memcpy(m, ctx->ls + lsa, n);
        break;
    }
    default: unexpected("wrch", ch); break;
    }
}
}  // extern "C"

static std::vector<uint8_t> g_image;   // the ELF's loadable segments, at their LS addresses
static void load_elf(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    std::vector<uint8_t> e;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) e.insert(e.end(), buf, buf + n);
    fclose(f);
    auto be16 = [&](size_t o) { return (uint32_t)(e[o] << 8 | e[o + 1]); };
    if (e.size() < 52 || memcmp(e.data(), "\x7F" "ELF", 4)) { fprintf(stderr, "%s: not an ELF\n", path); exit(2); }
    if (rd32(&e[24]) != 0x3050) { fprintf(stderr, "%s: entry %X, not 0x3050\n", path, rd32(&e[24])); exit(2); }
    g_image.assign(SPU_LS_SIZE, 0);
    const uint32_t phoff = rd32(&e[28]), phent = be16(42), phnum = be16(44);
    for (uint32_t i = 0; i < phnum; i++) {
        const size_t ph = phoff + i * phent;
        if (rd32(&e[ph]) != 1) continue;   // PT_LOAD (memsz past filesz: bss, zero)
        const uint32_t off = rd32(&e[ph + 4]), va = rd32(&e[ph + 8]), fsz = rd32(&e[ph + 16]);
        if ((uint64_t)va + fsz <= SPU_LS_SIZE && (uint64_t)off + fsz <= e.size()) memcpy(&g_image[va], &e[off], fsz);
    }
}

// One run of the lifted task: the image loaded, r3 = the argument as the game
// builds it ({ u64 EA, u32 flag, 0 }), from the entry to its end. `garbage`:
// the bss (but the atexit count) and the stack filled with noise first, to
// show that nothing the task leaves in guest memory depends on them.
static spu_context* g_ctx;
static void lifted_run(Space* s, uint32_t param_ea, uint32_t flag_ea, bool garbage, uint32_t seed)
{
    if (!g_ctx) {
        g_ctx = (spu_context*)calloc(1, sizeof(spu_context));
        g_ctx->ls = g_ctx->ls_store;
    }
    spu_context* ctx = g_ctx;
    memcpy(ctx->ls, g_image.data(), SPU_LS_SIZE);
    if (garbage) {
        std::mt19937 rng(seed);
        for (uint32_t a = 0x3A10; a < 0x40000; a++)
            if (a < 0x8A10 || a >= 0x8A20) ctx->ls[a] = (uint8_t)rng();
    }
    wr32(ctx->ls + 0x2FB8, kKernelPtr);
    wr32(ctx->ls + kKernelPtr + 196, kKernelExit);
    g_space = s;
    g_end = 0;
    g_end_pc = 0;
    g_exit_code = ~0u;
    g_tag_mask = g_tag_update = 0;
    ctx->mfc_lsa = ctx->mfc_eah = ctx->mfc_eal = ctx->mfc_size = ctx->mfc_tag = 0;
    for (int r = 0; r < 128; r++) ctx->gpr[r] = u128();
    ctx->gpr[3]._u32[0] = 0;
    ctx->gpr[3]._u32[1] = param_ea;
    ctx->gpr[3]._u32[2] = flag_ea;
    ctx->host_depth = 1;
    ctx->steps = 1;
    g_spu_trampoline_fn = nullptr;
    if (setjmp(g_jb) == 0) {
        spu_0001_at_00F48F80_spu_func_00003050(ctx);
        spu_drain_call(ctx, 0);
        g_end = 4;
    }
    g_spu_trampoline_fn = nullptr;
}

// ---- a case's guest region ----------------------------------------------------------
struct Layout {
    uint32_t base, size;   // the region
    uint32_t param, field, tex0, tex1;
};
static Layout standard(uint32_t W)
{
    // as the game has them in spirit: the block at W + 0x80, the field and the
    // textures elsewhere, gaps between them (part of the region: compared)
    return Layout{ W, 0xD1000, W + 0x80, W + 0x10400, W + 0x50800, W + 0x90C00 };
}

enum MapKind { kMapDistance, kMapRandom, kMapGradient, kMapAllFF, kMapAllZero, kMapOffCentre, kMapKinds };
enum TableKind { kTableSine, kTableEdge, kTableRandom, kTableSigned, kTableKinds };
static const char* const kMapName[] = { "distance map", "random map", "gradient map", "map all 0xFF", "map all 0", "off-centre map" };
static const char* const kTableName[] = { "sine table", "edge table", "random table", "signed table" };

static const uint32_t kEdgeFloats[] = {
    0x00000000u, 0x80000000u, 0x00000001u, 0x807FFFFFu, 0x00800000u, 0x80800000u, 0x7F7FFFFFu, 0xFF7FFFFFu,
    0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7FFFFFFFu, 0xFFFFFFFFu, 0x3F800000u, 0xBF800000u, 0x3F000000u,
    0x0D800000u, 0x72800000u, 0x3F7FFFFFu, 0x3F800001u,
};

struct Case {
    std::string name;
    Layout lay;
    uint32_t select, phase, radius, frequency, amplitude, slope;
    int map, table;
    bool garbage;
};

static void fill(Space& s, const Case& c, uint32_t seed)
{
    const Layout& L = c.lay;
    s.base = L.base;
    s.mem.assign(L.size, 0);
    s.log.clear();
    s.outside = 0;
    for (uint32_t i = 0; i < L.size; i++) s.mem[i] = (uint8_t)(0x5A + i * 7);   // the gaps
    auto words = [&](uint32_t ea, uint32_t bytes, uint32_t tag) {
        if (ea < L.base || (uint64_t)ea + bytes > (uint64_t)L.base + L.size) return;
        for (uint32_t k = 0; k + 4 <= bytes; k += 4) wr32(&s.mem[ea - L.base + k], tag | (k >> 2));
    };
    words(L.field, waver_task::kFieldBytes, 0xC0000000u);   // what the field step overwrites
    words(L.tex0, waver_task::kTextureBytes, 0xA0000000u);  // both textures: one is written, the other must stay
    words(L.tex1, waver_task::kTextureBytes, 0xB0000000u);
    if (L.param < L.base || (uint64_t)L.param + waver_task::kParamBytes > (uint64_t)L.base + L.size) return;
    uint8_t* p = &s.mem[L.param - L.base];
    wr32(p + 0x00, L.field);
    wr32(p + 0x04, L.tex0);
    wr32(p + 0x08, L.tex1);
    wr32(p + 0x0C, c.select);
    wr32(p + 0x10, c.phase);
    wr32(p + 0x14, c.radius);
    wr32(p + 0x18, c.frequency);
    wr32(p + 0x1C, c.amplitude);
    wr32(p + 0x20, c.slope);
    std::mt19937 rng(seed);
    uint8_t* map = p + waver_task::kMapOffset;
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) {
            uint8_t b = 0;
            switch (c.map) {
            case kMapDistance: { const float dx = x - 128.0f, dy = y - 128.0f; b = (uint8_t)(int)sqrtf(dx * dx + dy * dy); break; }
            case kMapRandom: b = (uint8_t)rng(); break;
            case kMapGradient: b = (uint8_t)(x + y); break;
            case kMapAllFF: b = 0xFF; break;
            case kMapAllZero: b = 0; break;
            case kMapOffCentre: { const float dx = x - 40.0f, dy = y - 70.0f; b = (uint8_t)(int)sqrtf(dx * dx + dy * dy); break; }
            }
            map[256 * y + x] = b;
        }
    uint8_t* table = p + waver_task::kTableOffset;
    for (int i = 0; i < 128; i++) {
        const float s1 = sinf(6.2831853f * (float)i / 128.0f);
        uint32_t v = 0;
        switch (c.table) {
        case kTableSine: v = fbits((s1 + 1.0f) * 0.5f); break;
        case kTableEdge: v = i % 3 ? kEdgeFloats[rng() % (sizeof kEdgeFloats / 4)] : fbits(s1); break;
        case kTableRandom: v = (uint32_t)rng(); break;
        case kTableSigned: v = fbits(s1 * 0.75f); break;
        }
        wr32(table + 4 * i, v);
    }
}

// The lifted task's transfers and waits for a run that is done, as
// the task was read to make them (2026-11-10).
static std::vector<Xfer> documented(const Params& p, uint32_t param_ea)
{
    std::vector<Xfer> v;
    uint32_t off = 0, left = waver_task::kParamBytes;
    while (left) {
        const uint32_t n = left > 16383 ? 16384 : left > 15 ? left & ~15u : left > 7 ? 8 : left > 3 ? 4 : left > 1 ? 2 : 1;
        v.push_back({ 'G', param_ea + off, n, 0x2FDA0 + off, 0 });
        off += n;
        left -= n;
    }
    v.push_back({ 'W', 1, 2, 0, 0 });
    for (uint32_t o = 0; o < 32; o++) {
        v.push_back({ 'P', p.field + 8192 * o, 8192, 0x3A10, 0 });
        v.push_back({ 'W', 1, 2, 0, 0 });
    }
    for (uint32_t k = 0; k < 32; k++) {
        v.push_back({ 'G', p.field + 8192 * k - (k ? 1024 : 0), k == 0 || k == 31 ? 9216u : 10240u, 0x3A10, 0 });
        v.push_back({ 'W', 1, 2, 0, 0 });
        v.push_back({ 'P', p.texture_ea() + 8192 * k, 8192, 0x6210, 1 });
    }
    v.push_back({ 'W', 2, 2, 0, 0 });
    return v;
}

static std::vector<Xfer> transfers(const std::vector<Xfer>& log)
{
    std::vector<Xfer> v;
    for (const Xfer& x : log)
        if (x.kind != 'W') v.push_back(x);
    return v;
}

static const char* where(const Layout& L, uint32_t ea, uint32_t* off)
{
    struct { const char* name; uint32_t at, n; } r[] = {
        { "parameter block", L.param, waver_task::kParamBytes }, { "field", L.field, waver_task::kFieldBytes },
        { "texture 0", L.tex0, waver_task::kTextureBytes }, { "texture 1", L.tex1, waver_task::kTextureBytes } };
    for (auto& x : r)
        if (ea >= x.at && ea - x.at < x.n) { *off = ea - x.at; return x.name; }
    *off = ea - L.base;
    return "the region (outside the buffers)";
}

static waver_task::Local g_local;
struct Tally { unsigned cases = 0, passed = 0, done = 0, halted = 0, documented = 0; } g_tally;
static double g_native_us = 0, g_lifted_us = 0;
static unsigned g_timed = 0;

// One case: native on `a`, lifted on `b` (both filled alike by the caller, or
// carried over from the previous frame), compared.
static bool run_case(const Case& c, Space& a, Space& b, uint32_t seed, bool quiet)
{
    a.log.clear(); a.outside = 0;
    b.log.clear(); b.outside = 0;
    g_unexpected = 0;
    const uint32_t flag_ea = c.lay.base + 0x3D400;
    const auto t0 = std::chrono::steady_clock::now();
    Params used;
    const waver_task::Status st = waver_task::run_frame(c.lay.param, waver_task::Io{ &a, space_get, space_put }, g_local, &used);
    const auto t1 = std::chrono::steady_clock::now();
    if (getenv("WAVER_TRACE")) printf("  native %d, running the lifted task\n", (int)st);
    lifted_run(&b, c.lay.param, flag_ea, c.garbage, seed);
    if (getenv("WAVER_TRACE")) printf("  lifted end %d pc %05X\n", g_end, g_end_pc);
    const auto t2 = std::chrono::steady_clock::now();
    const bool done = st == waver_task::Status::done;
    if (done) {
        g_native_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
        g_lifted_us += std::chrono::duration<double, std::micro>(t2 - t1).count();
        g_timed++;
    }
    std::vector<std::string> why;
    // the outcome
    const bool lifted_done = g_end == 2 && g_exit_code == 0 && g_end_pc == kKernelExit;
    if (done != lifted_done || (!done && g_end != 1)) {
        char m[160];
        snprintf(m, sizeof m, "outcome: native %s, lifted end %d (pc %05X, exit code %08X)", done ? "done" : "unsupported",
                 g_end, g_end_pc, g_exit_code);
        why.push_back(m);
    }
    // the bytes
    for (size_t k = 0; k < a.mem.size(); k++)
        if (a.mem[k] != b.mem[k]) {
            const uint32_t ea = a.base + (uint32_t)(k & ~3u);
            uint32_t off;
            const char* in = where(c.lay, ea, &off);
            char m[200];
            snprintf(m, sizeof m, "memory: first difference at %08X (%s +%05X, row %u x %u): native %08X lifted %08X", ea, in, off,
                     off / 1024, off % 1024 / 4, rd32(&a.mem[k & ~3u]), rd32(&b.mem[k & ~3u]));
            why.push_back(m);
            break;
        }
    // the transfers
    const std::vector<Xfer> na = transfers(a.log), li = transfers(b.log);
    size_t i = 0;
    while (i < na.size() && i < li.size() && na[i].kind == li[i].kind && na[i].ea == li[i].ea && na[i].n == li[i].n) i++;
    if (i != na.size() || i != li.size()) {
        char m[200];
        snprintf(m, sizeof m, "transfers: %zu native, %zu lifted, first difference at #%zu", na.size(), li.size(), i);
        why.push_back(m);
    }
    if (a.outside || b.outside) why.push_back("transfers outside the region");
    if (g_unexpected) why.push_back("channels or commands not expected");
    // the documented order (a run that is done)
    if (done && lifted_done) {
        const std::vector<Xfer> d = documented(used, c.lay.param);
        size_t j = 0;
        while (j < d.size() && j < b.log.size() && d[j].kind == b.log[j].kind && d[j].ea == b.log[j].ea && d[j].n == b.log[j].n &&
               d[j].lsa == b.log[j].lsa && d[j].tag == b.log[j].tag)
            j++;
        if (j != d.size() || j != b.log.size()) {
            char m[200];
            snprintf(m, sizeof m, "lifted order: %zu entries, %zu documented, first difference at #%zu", b.log.size(), d.size(), j);
            why.push_back(m);
        } else {
            g_tally.documented++;
        }
    }
    g_tally.cases++;
    g_tally.done += done;
    g_tally.halted += !done && g_end == 1;
    // not vacuous: what the field and the selected texture hold
    unsigned inside = 0, flat = 0;
    if (done && c.lay.field >= a.base && c.lay.field - a.base + waver_task::kFieldBytes <= a.mem.size() &&
        used.texture_ea() >= a.base && used.texture_ea() - a.base + waver_task::kTextureBytes <= a.mem.size()) {
        const bool overlap = used.texture_ea() < c.lay.field + waver_task::kFieldBytes && c.lay.field < used.texture_ea() + waver_task::kTextureBytes;
        for (uint32_t k = 0; k < 65536; k++) {
            if (!overlap) inside += rd32(&a.mem[c.lay.field - a.base + 4 * k]) != 0xBF800000u;
            flat += rd32(&a.mem[used.texture_ea() - a.base + 4 * k]) == 0x008080FFu;
        }
    }
    const bool ok = why.empty();
    g_tally.passed += ok;
    if (!ok || !quiet) {
        printf("[%s] %s: %s", ok ? "PASS" : "FAIL", c.name.c_str(), done ? "done" : "unsupported (lifted halted)");
        if (done) printf(", %u inside, %u flat pixels", inside, flat);
        else printf(": %s", waver_task::unsupported_reason(used, c.lay.param));
        printf("\n");
        for (const std::string& w : why) printf("  %s\n", w.c_str());
    }
    return ok;
}

static Case make(const char* name, uint32_t select, uint32_t phase, uint32_t radius, uint32_t frequency, float amplitude,
                 float slope, int map = kMapDistance, int table = kTableSine)
{
    return Case{ name, standard(0x02AB2200u), select, phase, radius, frequency, fbits(amplitude), fbits(slope), map, table, false };
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // each line as it is printed
    g_spu_ls_rdiag = 0;   // no local-store diagnostics in this stand-in
    g_spu_ls_wdiag = 0;
    const unsigned random_cases = argc >= 2 ? (unsigned)atoi(argv[1]) : 40u;
    const unsigned frames = argc >= 3 ? (unsigned)atoi(argv[2]) : 160u;
    load_elf(argc >= 4 ? argv[3] : "build/gen/spu/images/spu_0001_at_00F48F80.elf");

    std::vector<Case> cases;
    cases.push_back(make("the game's shape, texture 1", 1, 40, 40, 6, 0.3f, 8.0f));
    cases.push_back(make("the game's shape, texture 0", 0, 40, 40, 6, 0.3f, 8.0f));
    cases.push_back(make("phase 0, radius 0", 2, 0, 0, 6, 0.3f, 8.0f));
    cases.push_back(make("phase 0x7F, radius 0x80", 3, 0x7F, 0x80, 6, 0.3f, 8.0f));
    cases.push_back(make("phase 0x90, radius 0x80", 1, 0x90, 0x80, 6, 0.3f, 8.0f));
    cases.push_back(make("phase 0x7E, radius 0x7E", 0, 0x7E, 0x7E, 5, 0.2f, 4.0f));
    cases.push_back(make("radius 0xFFFFFFFF, phase 0x12345678, frequency 0xFFFFFFFF", 0xFFFFFFFFu, 0x12345678u, 0xFFFFFFFFu,
                         0xFFFFFFFFu, 0.5f, 3.0f, kMapRandom, kTableEdge));
    cases.push_back(make("frequency 0", 1, 60, 60, 0, 0.3f, 8.0f));
    cases.push_back(make("frequency 0x10003 (low half 3)", 0, 60, 60, 0x10003, 0.3f, 8.0f));
    cases.push_back(make("frequency 0xFFFF, phase 0xFFFF", 1, 0xFFFF, 200, 0xFFFF, 0.3f, 8.0f, kMapGradient));
    cases.push_back(make("frequency 128, phase 0x10000", 0, 0x10000, 0x100, 128, 0.3f, 8.0f, kMapRandom));
    cases.push_back(make("signed table: field below zero inside", 1, 100, 0x80, 7, 0.6f, 5.0f, kMapDistance, kTableSigned));
    cases.push_back(make("signed table, random map, negative slope", 0, 30, 0xFF, 3, 1.5f, -3.0f, kMapRandom, kTableSigned));
    cases.push_back(make("map all 0xFF: everything outside", 1, 50, 0xFFFFFFFFu, 6, 0.3f, 8.0f, kMapAllFF));
    cases.push_back(make("map all 0, radius 0: everything inside", 0, 0, 0, 6, 0.3f, 8.0f, kMapAllZero));
    cases.push_back(make("off-centre map (bytes wrap past 255)", 1, 90, 0x80, 9, 0.4f, 6.0f, kMapOffCentre, kTableSigned));
    cases.push_back(make("random table bits", 0, 80, 0x80, 13, 1.0f, 1.0f, kMapRandom, kTableRandom));
    cases.push_back(make("edge table", 1, 70, 0x80, 1, 1.0f, 2.0f, kMapGradient, kTableEdge));
    {
        static const struct { const char* name; uint32_t bits; } amps[] = {
            { "amplitude 0", 0x00000000u }, { "amplitude -0", 0x80000000u }, { "amplitude denormal", 0x00000001u },
            { "amplitude -denormal", 0x807FFFFFu }, { "amplitude FLT_MAX", 0x7F7FFFFFu }, { "amplitude extended max", 0x7FFFFFFFu },
            { "amplitude extended -max", 0xFFFFFFFFu }, { "amplitude 'inf' pattern", 0x7F800000u }, { "amplitude 1e-30", 0x0DA24260u },
            { "amplitude -2", 0xC0000000u },
        };
        for (auto& x : amps) {
            Case c = make(x.name, 1, 64, 0x80, 6, 0.0f, 8.0f, kMapDistance, kTableSigned);
            c.amplitude = x.bits;
            cases.push_back(c);
        }
        static const struct { const char* name; uint32_t bits; } slopes[] = {
            { "slope 0", 0x00000000u }, { "slope -0", 0x80000000u }, { "slope denormal", 0x00000001u }, { "slope 1e20", 0x60AD78ECu },
            { "slope extended max", 0x7FFFFFFFu }, { "slope extended -max", 0xFFFFFFFFu }, { "slope 1e-20", 0x1E3CE508u },
            { "slope 0.5", 0x3F000000u },
        };
        for (auto& x : slopes) {
            Case c = make(x.name, 0, 64, 0x80, 6, 0.3f, 0.0f, kMapRandom, kTableSigned);
            c.slope = x.bits;
            cases.push_back(c);
        }
    }
    cases.push_back(make("select 2", 2, 40, 40, 6, 0.3f, 8.0f));
    cases.push_back(make("select 0xFFFFFFFE", 0xFFFFFFFEu, 40, 40, 6, 0.3f, 8.0f));
    {   // local store left with garbage
        Case c = make("garbage in local store (bss, stack)", 1, 50, 0x80, 6, 0.3f, 8.0f, kMapRandom, kTableSigned);
        c.garbage = true;
        cases.push_back(c);
        Case d = make("garbage in local store, the game's shape", 0, 40, 40, 6, 0.3f, 8.0f);
        d.garbage = true;
        cases.push_back(d);
    }
    {   // buffers overlapping: the order of the transfers decides
        Case c = make("texture = field (the normal map's PUTs over the rows it reads next)", 0, 60, 0x80, 6, 0.4f, 8.0f, kMapDistance, kTableSigned);
        c.lay.tex0 = c.lay.field;
        cases.push_back(c);
        Case d = make("texture over the field's second half", 1, 60, 0x80, 6, 0.4f, 8.0f, kMapRandom, kTableSigned);
        d.lay.tex1 = d.lay.field + 0x20000 + 0x400;
        cases.push_back(d);
        Case e = make("field over the parameter block (after its GET)", 0, 60, 0x80, 6, 0.4f, 8.0f, kMapDistance, kTableSigned);
        e.lay.field = e.lay.param + 0x30;
        cases.push_back(e);
        Case f = make("texture over the parameter block", 1, 60, 0x80, 6, 0.4f, 8.0f);
        f.lay.tex1 = f.lay.param - 0x80;
        cases.push_back(f);
    }
    {   // a region near the top of the 32-bit space
        Case c = make("region just below 4 GB", 1, 40, 40, 6, 0.3f, 8.0f);
        c.lay = standard(0xFFFFF000u - 0xD1000u);
        cases.push_back(c);
    }
    {   // the halting EAs
        Case c = make("argument EA + 8 (halts before the GET)", 1, 40, 40, 6, 0.3f, 8.0f);
        c.lay.param += 8;
        cases.push_back(c);
        Case d = make("field EA + 4 (halts before the first PUT)", 1, 40, 40, 6, 0.3f, 8.0f);
        d.lay.field += 4;
        cases.push_back(d);
        Case e = make("selected texture EA + 8 (halts after the field)", 1, 40, 40, 6, 0.3f, 8.0f);
        e.lay.tex1 += 8;
        cases.push_back(e);
        Case f = make("unselected texture EA + 8 (no halt)", 0, 40, 40, 6, 0.3f, 8.0f);
        f.lay.tex1 += 8;
        cases.push_back(f);
    }
    // random cases from the edge values
    {
        std::mt19937 rng(20261110);
        static const uint32_t kInts[] = { 0, 1, 2, 5, 6, 7, 0x3F, 0x40, 0x7E, 0x7F, 0x80, 0x81, 0xFF, 0x100, 0xFFFF, 0x10000,
                                          0x10001, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu };
        for (unsigned n = 0; n < random_cases; n++) {
            auto pick_int = [&]() { return rng() % 4 ? kInts[rng() % (sizeof kInts / 4)] : (uint32_t)rng(); };
            auto pick_float = [&]() {
                const uint32_t r = rng() % 8;
                return r < 3 ? kEdgeFloats[rng() % (sizeof kEdgeFloats / 4)] : r < 7 ? fbits(((float)(rng() % 20001) - 10000.0f) / 1000.0f)
                                                                                      : (uint32_t)rng();
            };
            char name[64];
            snprintf(name, sizeof name, "random case %u", n + 1);
            Case c = make(name, (uint32_t)rng(), pick_int(), pick_int(), pick_int(), 0.0f, 0.0f, (int)(rng() % kMapKinds),
                          (int)(rng() % kTableKinds));
            c.amplitude = pick_float();
            c.slope = pick_float();
            c.garbage = rng() % 2;
            c.name += std::string(" (") + kMapName[c.map] + ", " + kTableName[c.table] + ")";
            cases.push_back(c);
        }
    }

    unsigned seed = 1;
    for (const Case& c : cases) {
        Space a, b;
        fill(a, c, seed);
        b = a;
        run_case(c, a, b, seed++, false);
    }

    // The game's evolution: the field and both textures persist from frame
    // to frame on each side (the task never reads them before writing, but a
    // stale value would show); phase and radius as func_003CEEE8 makes them.
    if (frames) {
        Case c = make("", 0, 0, 0, 6, 0.0f, 7.0f);
        Space a, b;
        fill(a, c, 777);
        b = a;
        unsigned equal = 0;
        for (unsigned n = 0; n < frames; n++) {
            const float elapsed = (float)n / 60.0f;
            const uint32_t t = (uint32_t)(elapsed * 90.0f);                  // the radius, map units
            c.phase = t;
            c.radius = t > 0x7F ? 0x80 : t;
            c.select = n + 1;                                                 // the frame counter + 1
            c.amplitude = fbits(0.35f * (1.0f - (float)n / (float)frames));  // fading
            c.garbage = n % 3 == 0;
            for (Space* s : { &a, &b }) {
                uint8_t* p = &s->mem[c.lay.param - c.lay.base];
                wr32(p + 0x0C, c.select);
                wr32(p + 0x10, c.phase);
                wr32(p + 0x14, c.radius);
                wr32(p + 0x1C, c.amplitude);
            }
            char name[64];
            snprintf(name, sizeof name, "frame %u (phase %u, radius %u)", n + 1, c.phase, c.radius);
            c.name = name;
            equal += run_case(c, a, b, 1000 + n, true);
        }
        printf("[%s] the game's evolution: %u of %u frames equal\n", equal == frames ? "PASS" : "FAIL", equal, frames);
    }

    printf("[info] %u runs: %u passed; %u done (%u with the lifted order as documented), %u halting\n", g_tally.cases,
           g_tally.passed, g_tally.done, g_tally.documented, g_tally.halted);
    if (g_timed)
        printf("[info] mean per frame: native %.0f us, lifted (stand-in runtime) %.0f us\n", g_native_us / g_timed, g_lifted_us / g_timed);
    const bool all = g_tally.passed == g_tally.cases;
    printf("%s\n", all ? "all passed" : "FAILED");
    return all ? 0 : 1;
}
