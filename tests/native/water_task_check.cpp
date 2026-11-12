// The native water task (port/native/water_task.cpp: the host around the task's code generated from the
// lift, port/native/spu_native.h) against the lifted one
// (build/gen/spu/spu_0000_at_00F47380/spu_recomp.c), offline: the lifted
// task's two step functions (LS 0x3098, the height step; LS 0x35E0, the
// normal map) run on a bare SPU context with its image in local store and a
// stand-in MFC that does each GET and PUT as it is issued (the runtime's
// order), beside the native step on a copy of the same memory; every byte
// both leave compared.
//
//   water_task_check synth [<steps>] [<elf>]
//       made-up steps: widths 8..408 (nb = 1, 2, 3, the game's 128, the
//       largest supported), both layer indices, damping and scale varied,
//       heights small with drops plus edge values (zeros of both signs,
//       denormals, huge, NaN/Inf patterns); then <steps> (default 300) of the
//       game's evolution at 128 (a drop a step, the index flipped), each
//       side fed by its own previous results.
//   water_task_check snap <wN_in.bin> [<wN_out.bin>] [<elf>]
//       a step captured in the game (DOTHEROES_WATER_CAPTURE, or a check-mode
//       mismatch's wbadN pair): native and lifted on its inputs, against each
//       other and, given the _out file, against what the game's lifted task
//       left (the layer it wrote and the texture).
//   water_task_check mxcsr
//       the float mode's switches: every transfer in the caller's MXCSR, the caller's MXCSR back after each step,
//       a step nested in another's transfer, four threads in four modes at once; the outputs as in the default
//       mode (mode_check below).
//   water_task_check game <1|sync|check> [<steps>] [<capture prefix>]
//       the game's side (water_task_signal, its modes, the check and the
//       capture) on a stand-in guest laid out as the game lays it out.
//
// Built with the bundled Zig (from the repository root; <ps3recomp> =
// build/ref/ps3recomp; -mcpu as the game is built, tools/zig_build.py host_cpu), the task's native code
// generated first from the same lift:
//   python tools/gen_spu_native.py tools/spu_native/water.py build/gen/spu/spu_0000_at_00F47380/spu_recomp.c
//       build/gen/spu/images/spu_0000_at_00F47380.elf tools/spu_native_task.cpp.in water_gen.cpp
//   zig cc -target x86_64-windows-gnu -mcpu=x86_64_v3 -O2 -c -I <ps3recomp>/runtime/spu
//       -I <ps3recomp>/include build/gen/spu/spu_0000_at_00F47380/spu_recomp.c -o water_lift.o
//   zig c++ -target x86_64-windows-gnu -mcpu=x86_64_v3 -O2 -std=c++17 -I port/native
//       -I <ps3recomp>/runtime/spu -I <ps3recomp>/include tests/native/water_task_check.cpp
//       port/native/water_task.cpp port/native/spu_native.cpp water_gen.cpp water_lift.o -o water_task_check.exe
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <immintrin.h>
#include <memory>
#include <thread>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "spu_context.h"
#include "water_task.h"

using water_task::Params;

// ---- a sparse guest space: the ranges a step touches ------------------------
struct Space {
    struct Range { uint32_t ea; std::vector<uint8_t> b; };
    std::vector<Range> ranges;
    unsigned outside = 0;
    uint8_t* at(uint32_t ea, uint32_t n)
    {
        for (Range& r : ranges)
            if (ea >= r.ea && (uint64_t)ea + n <= (uint64_t)r.ea + r.b.size()) return r.b.data() + (ea - r.ea);
        if (!outside++) fprintf(stderr, "transfer outside the space: ea %08X, %u bytes\n", ea, n);
        return nullptr;
    }
    Range& add(uint32_t ea, uint32_t n) { ranges.push_back({ ea, std::vector<uint8_t>(n) }); return ranges.back(); }
};

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

// ---- the native side's transfers ---------------------------------------------
static void space_get(void* u, void* dst, uint32_t ea, uint32_t n)
{
    const uint8_t* s = ((Space*)u)->at(ea, n);
    if (s) memcpy(dst, s, n); else memset(dst, 0, n);
}
static void space_put(void* u, uint32_t ea, const void* src, uint32_t n)
{
    if (uint8_t* d = ((Space*)u)->at(ea, n)) memcpy(d, src, n);
}

// ---- the lifted side: the stand-in runtime ------------------------------------
extern "C" {
void spu_0000_at_00F47380_spu_func_00003098(spu_context* ctx);
void spu_0000_at_00F47380_spu_func_000035E0(spu_context* ctx);

uint8_t* vm_base = nullptr;             // the game side's guest memory (the `game` test)
__thread void (*g_spu_trampoline_fn)(spu_context*) = nullptr;
volatile int g_spu_direct = 0;          // always the trampoline: the drain loop below runs every transfer
uint8_t g_spu_stack_reset_hint[1024];
static Space* g_space;
static int g_halted;

void spu_indirect_branch(spu_context* ctx) { fprintf(stderr, "indirect branch to %05X (not expected)\n", ctx->pc); exit(4); }
void spu_indirect_branch_mt(spu_context* ctx) { spu_indirect_branch(ctx); }
void spu_stop(spu_context* ctx) { fprintf(stderr, "stop %X (not expected)\n", ctx->stop_code); exit(4); }
void spu_halt(spu_context*) { g_halted = 1; }
void spu_img_restore(spu_context*, int32_t) {}
void spu_register_function(uint32_t, void (*)(spu_context*)) {}
void spu_drain_call(spu_context* ctx, uint32_t)
{
    while (g_spu_trampoline_fn && !g_halted) {
        void (*f)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = nullptr;
        f(ctx);
    }
    g_spu_trampoline_fn = nullptr;
}
int g_spu_ls_watch_n = 0;
void spu_ls_watch_slow(uint32_t, int, const uint8_t*, uint32_t, uint32_t) {}
int ps3_strict(void) { return 0; }
void ps3_strict_fail(const char* kind, const char*, ...) { fprintf(stderr, "%s (not expected)\n", kind); exit(4); }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
u128 spu_rdch(spu_context*, uint32_t) { return u128(); }   // tag status: everything complete
#pragma clang diagnostic pop
uint32_t spu_rchcnt(spu_context*, uint32_t) { return 1; }
void spu_wrch(spu_context* ctx, uint32_t ch, u128 v)
{
    const uint32_t x = v._u32[0];
    switch (ch) {
    case MFC_LSA: ctx->mfc_lsa = x; break;
    case MFC_EAH: ctx->mfc_eah = x; break;
    case MFC_EAL: ctx->mfc_eal = x; break;
    case MFC_Size: ctx->mfc_size = x; break;
    case MFC_TagID: ctx->mfc_tag = x; break;
    case MFC_Cmd: {
        const uint32_t lsa = ctx->mfc_lsa & SPU_LS_MASK, n = ctx->mfc_size;
        if (ctx->mfc_eah) { fprintf(stderr, "EAH %08X (not expected)\n", ctx->mfc_eah); exit(4); }
        uint8_t* m = g_space ? g_space->at(ctx->mfc_eal, n) : vm_base + ctx->mfc_eal;
        if (x == 0x40) { if (m) memcpy(ctx->ls + lsa, m, n); }        // GET
        else if (x == 0x20) { if (m) memcpy(m, ctx->ls + lsa, n); }   // PUT
        else { fprintf(stderr, "MFC command %X (not expected)\n", x); exit(4); }
        break;
    }
    default: break;   // tag mask and update: every transfer is done when issued
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
    g_image.assign(SPU_LS_SIZE, 0);
    const uint32_t phoff = rd32(&e[28]), phent = be16(42), phnum = be16(44);
    for (uint32_t i = 0; i < phnum; i++) {
        const size_t ph = phoff + i * phent;
        if (rd32(&e[ph]) != 1) continue;   // PT_LOAD
        const uint32_t off = rd32(&e[ph + 4]), va = rd32(&e[ph + 8]), fsz = rd32(&e[ph + 16]);
        if ((uint64_t)va + fsz <= SPU_LS_SIZE && (uint64_t)off + fsz <= e.size()) memcpy(&g_image[va], &e[off], fsz);
    }
}

// The lifted step: P's 24 bytes in local store, r3 its address, as the task's
// main has them (r1 + 0x30), then 0x3098 and 0x35E0 as the main calls them.
// On `s`, or with none on vm_base (the `game` test).
static spu_context* g_ctx;
static bool lifted_step(Space* s, uint32_t param_ea)
{
    if (!g_ctx) {
        g_ctx = (spu_context*)calloc(1, sizeof(spu_context));
        g_ctx->ls = g_ctx->ls_store;
        memcpy(g_ctx->ls, g_image.data(), SPU_LS_SIZE);   // the task's LS persists from step to step; so does this one
    }
    g_space = s;
    g_halted = 0;
    spu_context* ctx = g_ctx;
    const uint32_t sp = 0x3FE80, p_ls = sp + 0x30;
    const uint8_t* p = s ? s->at(param_ea, water_task::kParamBytes) : vm_base + param_ea;
    if (!p) return false;
    memcpy(ctx->ls + p_ls, p, water_task::kParamBytes);
    void (*steps[2])(spu_context*) = { spu_0000_at_00F47380_spu_func_00003098, spu_0000_at_00F47380_spu_func_000035E0 };
    for (auto fn : steps) {
        ctx->gpr[1] = u128(); ctx->gpr[1]._u32[0] = sp;
        ctx->gpr[3] = u128(); ctx->gpr[3]._u32[0] = p_ls;
        ctx->gpr[0] = u128(); ctx->gpr[0]._u32[0] = 0x3C80;
        ctx->host_depth = 1;   // a call frame: the function's `bi $r0` returns here
        ctx->steps = 1;
        g_spu_trampoline_fn = nullptr;
        fn(ctx);
        spu_drain_call(ctx, 0x3C80);
        if (g_halted) return false;
    }
    return true;
}

// ---- comparing ----------------------------------------------------------------
static int g_fails;
static bool same(const Space& a, const Space& b, const char* what)
{
    for (size_t i = 0; i < a.ranges.size(); i++) {
        const auto &x = a.ranges[i].b, &y = b.ranges[i].b;
        for (size_t k = 0; k + 4 <= x.size(); k += 4)
            if (memcmp(&x[k], &y[k], 4)) {
                printf("[FAIL] %s: range %08X +%zX: native %08X lifted %08X\n", what, a.ranges[i].ea, k, rd32(&x[k]), rd32(&y[k]));
                g_fails++;
                return false;
            }
    }
    return true;
}

static water_task::Local g_local;
static bool native_step(Space& s, uint32_t param_ea)
{
    const water_task::Io io{ &s, space_get, space_put };
    return water_task::run_frame(param_ea, io, g_local) == water_task::Status::done;
}

// ---- made-up steps --------------------------------------------------------------
constexpr uint32_t kParamEa = 0x00F00080u, kHeightsEa = 0x10000000u, kTextureEa = 0xCA90AF80u;

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static Space make_space(int32_t W, uint32_t index, uint32_t damping, uint32_t scale, std::mt19937& rng, bool edge)
{
    Space s;
    const uint32_t w = (uint32_t)W, layers = 8u * w * w;
    uint8_t* p = s.add(kParamEa, water_task::kParamBytes).b.data();
    wr32(p + 0, kHeightsEa); wr32(p + 4, kTextureEa); wr32(p + 8, (uint32_t)W); wr32(p + 12, index);
    wr32(p + 16, damping); wr32(p + 20, scale);
    auto& h = s.add(kHeightsEa, layers + 0x1000).b;   // slack: W = 8 reads a row past the second layer
    std::uniform_real_distribution<float> small(-0.08f, 0.08f);
    std::uniform_int_distribution<uint32_t> any;
    static const uint32_t kEdge[] = { 0x00000000u, 0x80000000u, 0x00000001u, 0x807FFFFFu, 0x7F7FFFFFu, 0xFF7FFFFFu,
                                      0x7F800000u, 0x7FC00000u, 0x3F800000u, 0xBF800000u, 0x00800000u, 0x0F000000u };
    for (size_t k = 0; k + 4 <= h.size(); k += 4) {
        uint32_t v = fbits(small(rng));
        const uint32_t r = any(rng) % 1000;
        if (r < 4) v = fbits(1.0f);                          // a drop
        else if (edge && r < 20) v = kEdge[any(rng) % (sizeof kEdge / 4)];
        else if (edge && r < 24) v = any(rng);               // any bits
        wr32(&h[k], v);
    }
    s.add(kTextureEa, 32u * w * (uint32_t)(W / 8));
    return s;
}

static void synth(unsigned evolve_steps)
{
    std::mt19937 rng(20261110);
    struct Case { int32_t W; uint32_t index; float damping, scale; bool edge; };
    const Case cases[] = {
        { 128, 0, 0.98f, 8.0f, false }, { 128, 1, 0.98f, 8.0f, false }, { 128, 0, 0.995f, 0.5f, true },
        { 128, 1, 1.0f, 64.0f, true }, { 8, 0, 0.98f, 4.0f, false }, { 8, 1, 0.98f, 4.0f, true },
        { 12, 0, 0.9f, 2.0f, false }, { 16, 1, 0.98f, 3.0f, true }, { 20, 0, 0.98f, 3.0f, false },
        { 24, 1, 0.97f, 16.0f, true }, { 64, 0, 0.98f, 8.0f, false }, { 408, 1, 0.98f, 8.0f, true },
        { 4, 0, 0.98f, 8.0f, false },   // nb = 0: nothing on either side
    };
    for (const Case& c : cases) {
        Space a = make_space(c.W, c.index, fbits(c.damping), fbits(c.scale), rng, c.edge), b = a;
        const bool nat = native_step(a, kParamEa), lif = lifted_step(&b, kParamEa);
        char what[96];
        snprintf(what, sizeof what, "W %d index %u damping %g scale %g%s", c.W, c.index, c.damping, c.scale,
                 c.edge ? " edge values" : "");
        unsigned lit = 0;   // not a vacuous pass: the step wrote the texture
        for (size_t k = 0; k + 4 <= a.ranges[2].b.size(); k += 4) lit += rd32(&a.ranges[2].b[k]) != 0;
        const bool ok = same(a, b, what) && !a.outside && !b.outside && (c.W / 8 > 0 ? nat && lif && lit : !nat && lif);
        if (ok) printf("[PASS] %s\n", what);
        else if (!g_fails++) printf("[FAIL] %s (native %d lifted %d, outside %u/%u)\n", what, nat, lif, a.outside, b.outside);
    }
    // The game's evolution at 128: the PPU's drop (1.0 at a random inner
    // element of the layer the step writes), then the step, then the flip.
    Space a = make_space(128, 0, fbits(0.98f), fbits(8.0f), rng, false), b = a;
    std::uniform_int_distribution<uint32_t> cell(0, 125);
    unsigned equal = 0;
    for (unsigned n = 0; n < evolve_steps; n++) {
        const uint32_t index = n & 1, x = cell(rng) + 1, y = cell(rng) + 1;
        for (Space* s : { &a, &b }) {
            wr32(s->at(kParamEa + 12, 4), index);
            wr32(s->at(kHeightsEa + 0x10000u * index + 4u * (x * 128u + y), 4), 0x3F800000u);
        }
        native_step(a, kParamEa);
        lifted_step(&b, kParamEa);
        char what[64];
        snprintf(what, sizeof what, "evolution step %u", n + 1);
        if (!same(a, b, what)) break;
        equal++;
    }
    printf("[%s] evolution at 128: %u of %u steps equal\n", equal == evolve_steps ? "PASS" : "FAIL", equal, evolve_steps);
    g_fails += equal != evolve_steps;
    // the native step's time at the game's size, one thread
    const unsigned reps = 200;
    const auto t0 = std::chrono::steady_clock::now();
    for (unsigned n = 0; n < reps; n++) native_step(a, kParamEa);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / reps;
    printf("[info] native step at 128: %.1f us\n", us);
}

// ---- captured steps ----------------------------------------------------------------
static Space load_space(const char* path)
{
    Space s;
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    uint32_t hdr[2];
    while (fread(hdr, 4, 2, f) == 2) {
        auto& b = s.add(hdr[0], hdr[1]).b;
        if (fread(b.data(), 1, hdr[1], f) != hdr[1]) break;
    }
    fclose(f);
    return s;
}

static void snap(const char* in_path, const char* out_path)
{
    const Space in = load_space(in_path);
    if (in.ranges.size() < 3) { fprintf(stderr, "%s: expected the parameters, the heights, the texture\n", in_path); exit(2); }
    const uint32_t param_ea = in.ranges[0].ea;
    const Params p = water_task::parse_params(in.ranges[0].b.data());
    printf("step: param %08X heights %08X texture %08X width %d index %u damping %08X scale %08X\n", param_ea,
           p.heights, p.texture, p.width, p.index, p.damping, p.scale);
    Space a = in, b = in;
    const bool nat = native_step(a, param_ea), lif = lifted_step(&b, param_ea);
    if (same(a, b, "native against lifted (offline)") && nat && lif) printf("[PASS] native = lifted (offline)\n");
    if (!out_path) return;
    const Space out = load_space(out_path);
    // what the game's task wrote: the `index` layer and the texture
    const uint32_t W = (uint32_t)p.width, layer = 4u * W * W, tex = 32u * W * (uint32_t)(p.width / 8);
    auto check = [&](const Space& s, const char* who) {
        unsigned dh = 0, dt = 0;
        const uint8_t* h = s.ranges[1].b.data() + layer * p.index;
        const uint8_t* gh = out.ranges[1].b.data() + layer * p.index;
        for (uint32_t k = 0; k < layer; k += 4) dh += memcmp(h + k, gh + k, 4) != 0;
        for (uint32_t k = 0; k < tex; k += 4) dt += memcmp(s.ranges[2].b.data() + k, out.ranges[2].b.data() + k, 4) != 0;
        printf("[%s] %s against the game's: %u height(s), %u pixel(s) differ\n", dh || dt ? "FAIL" : "PASS", who, dh, dt);
        g_fails += dh || dt;
    };
    check(a, "native");
    check(b, "lifted (offline)");
}

// ---- the game's side (water_task_signal) on a stand-in guest -----------------------
// The game's layout: the 0x3D600-byte block (argument at +0x3D480, flag at
// +0x3D400), the 24 bytes at the water globals + 0x80; each step the PPU's
// index and drop, then the signal; in lifted and check modes the lifted step
// runs on guest memory after the (declined) signal, and the task is then
// marked back in its wait (the flag's used slot); in the native modes the
// lifted step runs on a copy taken before the signal and is compared with
// guest memory (the worker waited for by the quit signal, which the hook also
// declines). With a capture prefix, steps 2 and 3 are captured.
static void commit(uint32_t ea, uint32_t n)
{
    VirtualAlloc(vm_base + (ea & ~0xFFFu), ((ea & 0xFFFu) + n + 0xFFFu) & ~0xFFFu, MEM_COMMIT, PAGE_READWRITE);
}
static void game(const char* mode, unsigned steps, const char* capture)
{
    _putenv_s("DOTHEROES_NATIVE_WATER", mode);
    if (capture) {
        const std::string c = std::string("2,2:") + capture;
        _putenv_s("DOTHEROES_WATER_CAPTURE", c.c_str());
    }
    vm_base = (uint8_t*)VirtualAlloc(nullptr, 1ull << 32, MEM_RESERVE, PAGE_NOACCESS);
    const uint32_t block = 0x30000000u, globals = 0x01000000u, param = globals + 0x80, flag = block + water_task::kBlockFlag;
    commit(block, 0x3D600);
    commit(globals, 0x100);
    commit(kHeightsEa, 0x21000);
    commit(kTextureEa, 0x10000);
    uint8_t* a = vm_base + block + water_task::kBlockArgument;
    wr32(a, 0); wr32(a + 4, param); wr32(a + 8, flag); wr32(a + 12, 0);
    uint8_t* p = vm_base + param;
    wr32(p, kHeightsEa); wr32(p + 4, kTextureEa); wr32(p + 8, 128); wr32(p + 12, 0); wr32(p + 16, fbits(0.98f)); wr32(p + 20, fbits(8.0f));
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> small(-0.05f, 0.05f);
    for (uint32_t k = 0; k < 0x20000u; k += 4) wr32(vm_base + kHeightsEa + k, fbits(small(rng)));
    water_task_init();
    const bool native = strcmp(mode, "check") != 0;
    std::uniform_int_distribution<uint32_t> cell(1, 126);
    unsigned compared = 0, equal = 0;
    for (unsigned n = 0; n < steps; n++) {
        const uint32_t index = n & 1;
        wr32(p + 12, index);
        wr32(vm_base + kHeightsEa + 0x10000u * index + 4u * (cell(rng) * 128u + cell(rng)), 0x3F800000u);
        Space ref;   // the step's ranges before the signal
        ref.ranges.push_back({ param, std::vector<uint8_t>(p, p + 24) });
        ref.ranges.push_back({ kHeightsEa, std::vector<uint8_t>(vm_base + kHeightsEa, vm_base + kHeightsEa + 0x20000) });
        ref.ranges.push_back({ kTextureEa, std::vector<uint8_t>(vm_base + kTextureEa, vm_base + kTextureEa + 0x10000) });
        const int taken = water_task_signal(block, 1);
        if (taken != (native ? 1 : 0)) { printf("[FAIL] step %u: the hook returned %d\n", n + 1, taken); g_fails++; return; }
        if (!native) {
            lifted_step(nullptr, param);   // the lifted task, woken
            wr32(vm_base + flag, 0);       // events 0, nothing pending
            vm_base[flag + 8] = 0x80;      // its slot used: back in its wait
            vm_base[flag + 9] = 0;
            continue;
        }
        water_task_signal(block, 2);   // the quit's wait: the worker done
        lifted_step(&ref, param);
        Space got;
        got.ranges.push_back({ param, std::vector<uint8_t>(p, p + 24) });
        got.ranges.push_back({ kHeightsEa, std::vector<uint8_t>(vm_base + kHeightsEa, vm_base + kHeightsEa + 0x20000) });
        got.ranges.push_back({ kTextureEa, std::vector<uint8_t>(vm_base + kTextureEa, vm_base + kTextureEa + 0x10000) });
        compared++;
        char what[64];
        snprintf(what, sizeof what, "%s step %u", mode, n + 1);
        equal += same(got, ref, what);
    }
    water_task_signal(block, 2);   // the quit (check mode: the last step compared)
    WaterTaskCounts c;
    water_task_counts(&c);
    printf("[info] %s: %llu signals, %llu native steps, %llu checked (%llu equal, %llu busy), %llu coalesced, %llu unsupported\n",
           mode, (unsigned long long)c.signals, (unsigned long long)c.steps, (unsigned long long)c.checked,
           (unsigned long long)c.equal, (unsigned long long)c.busy, (unsigned long long)c.coalesced,
           (unsigned long long)c.unsupported);
    const bool ok = native ? (compared == steps && equal == steps && c.steps == steps)
                           : (c.checked == steps && c.equal == steps && c.busy == 0);
    printf("[%s] the game's side, mode %s, %u steps\n", ok ? "PASS" : "FAIL", mode, steps);
    g_fails += !ok;
}

// ---- the float mode's switches (tools/spu_native_task.cpp.in) --------------------------------------------------
// The generated code runs in the SPU's float mode (MXCSR: round toward zero, flush-to-zero, denormals-are-zero)
// and gives every call into the host its caller's mode back. Checked here: each transfer the task asks of the host
// runs in its caller's MXCSR, whatever that is; the caller has its MXCSR back after every step; a whole step run
// inside one of another step's transfers (a nested call) leaves both right; four threads at once, each in a mode of
// its own, see only theirs -- and every step's output is the same as in the host's default mode, byte for byte.
struct ModeIo {
    Space* s;
    unsigned want;
    std::atomic<unsigned>* bad;
    Space* nested;                    // run a whole step on this inside the first transfer, once
    water_task::Local* nested_local;
};
static void mode_put(void* u, uint32_t ea, const void* src, uint32_t n);
static void mode_get(void* u, void* dst, uint32_t ea, uint32_t n)
{
    ModeIo* m = (ModeIo*)u;
    if (_mm_getcsr() != m->want) m->bad->fetch_add(1);
    if (m->nested) {
        Space* inner = m->nested;
        m->nested = nullptr;
        ModeIo im{ inner, m->want, m->bad, nullptr, nullptr };
        const water_task::Io io{ &im, mode_get, mode_put };
        if (water_task::run_frame(kParamEa, io, *m->nested_local) != water_task::Status::done) m->bad->fetch_add(1);
        if (_mm_getcsr() != m->want) m->bad->fetch_add(1);   // the inner step gave this transfer its mode back
    }
    space_get(m->s, dst, ea, n);
}
static void mode_put(void* u, uint32_t ea, const void* src, uint32_t n)
{
    ModeIo* m = (ModeIo*)u;
    if (_mm_getcsr() != m->want) m->bad->fetch_add(1);
    space_put(m->s, ea, src, n);
}

static void mode_check()
{
    std::mt19937 rng(20261112);
    const Space base = make_space(128, 0, fbits(0.98f), fbits(8.0f), rng, true);
    std::atomic<unsigned> bad{ 0 };
    // 40 steps of the game's evolution in the caller's mode `mx`; at step 5 a whole step on `nested` inside it
    auto evolve = [&](Space& s, water_task::Local& l, unsigned mx, Space* nested, water_task::Local* nl) {
        _mm_setcsr(mx);
        std::mt19937 r(7);
        std::uniform_int_distribution<uint32_t> cell(0, 125);
        for (unsigned n = 0; n < 40; n++) {
            const uint32_t index = n & 1, x = cell(r) + 1, y = cell(r) + 1;
            wr32(s.at(kParamEa + 12, 4), index);
            wr32(s.at(kHeightsEa + 0x10000u * index + 4u * (x * 128u + y), 4), 0x3F800000u);
            ModeIo m{ &s, mx, &bad, n == 5 ? nested : nullptr, nl };
            const water_task::Io io{ &m, mode_get, mode_put };
            if (water_task::run_frame(kParamEa, io, l) != water_task::Status::done) bad++;
            if (_mm_getcsr() != mx) bad++;
        }
        _mm_setcsr(0x1F80u);
    };
    auto local = [] { return std::unique_ptr<water_task::Local>(new water_task::Local()); };
    Space ref = base;
    auto lr = local();
    evolve(ref, *lr, 0x1F80u, nullptr, nullptr);
    // four threads at once: round down, round up, toward zero, and flush-to-zero with denormals-are-zero
    const unsigned modes[4] = { 0x1F80u | 0x2000u, 0x1F80u | 0x4000u, 0x1F80u | 0x6000u, 0x1F80u | 0x8000u | 0x0040u };
    std::vector<Space> s(4, base);
    std::vector<std::unique_ptr<water_task::Local>> l;
    for (int i = 0; i < 4; i++) l.push_back(local());
    std::vector<std::thread> th;
    for (int i = 0; i < 4; i++) th.emplace_back([&, i] { evolve(s[i], *l[i], modes[i], nullptr, nullptr); });
    for (auto& t : th) t.join();
    for (int i = 0; i < 4; i++) {
        char what[64];
        snprintf(what, sizeof what, "thread %d, the caller's MXCSR %04X", i, modes[i]);
        if (same(s[i], ref, what)) printf("[PASS] %s: 40 steps as in the default mode\n", what);
    }
    // nested: a whole step inside a transfer of the outer one's sixth step
    Space outer = base, inner = base, one = base;
    auto lo = local(), li = local(), l1 = local();
    evolve(outer, *lo, 0x1F80u | 0x2000u, &inner, li.get());
    {
        ModeIo m{ &one, 0x1F80u, &bad, nullptr, nullptr };
        const water_task::Io io{ &m, mode_get, mode_put };
        water_task::run_frame(kParamEa, io, *l1);
    }
    if (same(outer, ref, "the outer evolution, a step nested in it")) printf("[PASS] the outer evolution with a nested step\n");
    if (same(inner, one, "the nested step")) printf("[PASS] the nested step as a step of its own\n");
    printf("[%s] the caller's MXCSR in every transfer and after every step: %u wrong\n", bad ? "FAIL" : "PASS", bad.load());
    g_fails += bad != 0;
}

int main(int argc, char** argv)
{
    g_spu_ls_rdiag = 0;   // no local-store diagnostics in this stand-in
    g_spu_ls_wdiag = 0;
    const char* elf = "build/gen/spu/images/spu_0000_at_00F47380.elf";
    if (argc >= 2 && !strcmp(argv[1], "synth")) {
        if (argc >= 4) elf = argv[3];
        load_elf(elf);
        synth(argc >= 3 ? (unsigned)atoi(argv[2]) : 300u);
    } else if (argc >= 3 && !strcmp(argv[1], "snap")) {
        if (argc >= 5) elf = argv[4];
        load_elf(elf);
        snap(argv[2], argc >= 4 ? argv[3] : nullptr);
    } else if (argc >= 2 && !strcmp(argv[1], "mxcsr")) {
        mode_check();
    } else if (argc >= 3 && !strcmp(argv[1], "game")) {
        load_elf(elf);
        game(argv[2], argc >= 4 ? (unsigned)atoi(argv[3]) : 20u, argc >= 5 ? argv[4] : nullptr);
    } else {
        fprintf(stderr, "usage: water_task_check synth [<steps>] [<elf>] | snap <wN_in.bin> [<wN_out.bin>] [<elf>]\n"
                        "       water_task_check game <1|sync|check> [<steps>] [<capture prefix>]\n");
        return 2;
    }
    printf("%s\n", g_fails ? "FAILED" : "all passed");
    return g_fails ? 1 : 0;
}
