/* The game's audio boundary (docs/OPENAL_MIGRATION_PLAN.md section 1): entry hooks on its sound manager's
 * methods (config/audio_hooks.json) and -- for M0's baseline only -- on the FMOD Ex API functions those
 * methods call.
 *
 * M0: DOTHEROES_AUDIO_TRACE=<file> records every call through the hooks, its arguments and its results, and
 * changes nothing: each hook runs the lifted function itself (re-entering through its own hook, which then
 * declines), drains the lift's tail calls as a call site does, records the results and returns to the
 * caller as the lifted function would have. Without the variable every hook declines at its guard
 * (g_audio_hooks = 0) and the lifted functions run exactly as before.
 *
 * A line of the trace (tools/audio_trace.py reads it):
 *   <us since the start> <the game's frame count> <thread> <name> [this=<hex>] <arguments> -> <r3 hex> [out=...]
 * The arguments follow each function's codes in port/audio_hook_table.inc, one per argument after `this`
 * (r3): i an int, x a hex word, f the next float register, v a pointer to three floats read before the call
 * (printed (x,y,z)), s a C string read before the call; o, b, F out-pointers to a word, a byte, a float read
 * after it; S an out string buffer and its length, read after it. A leading '!' means the function has no
 * `this` (the arguments start at r3); '.' means `this` is the game's sound manager (not printed).
 *
 * DOTHEROES_DEV_SOUND=<t>:<op>[:<arg>[:<arg>]],... (a test switch, M0's sound lab): at <t> seconds after the
 * sound manager's first update, on the game thread at the start of an update, ask the game's own sound
 * manager for something -- `se:<id>` (playSE, no position), `stopse:<id>`, `bgm:<id>:<loop 0/1>` (playBGM),
 * `fadebgm:<seconds>` (fadeOutBGM), `fadeloops:<seconds>` (fadeOutLoopSEs) -- through its lifted methods, as
 * a call from the game would (traced like one when the trace is on). Nothing else changes. */
#include "ppu_recomp.h"   /* ppu_context, vm_base, the lifted functions */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);   // the lift's tail calls (as phys_hooks.cpp)
extern "C" { int g_audio_hooks = 0; }

namespace {

constexpr uint32_t kGameFrame = 0x02CF3570;   // the game's frame count (game_patches.cpp)
constexpr uint32_t kUpdate = 0x0046DB40;      // the sound manager's update(dt), every frame on the game thread

FILE* g_trace = nullptr;
std::mutex g_trace_mu;
std::chrono::steady_clock::time_point g_t0, g_last_flush;
std::atomic<int> g_thread_ids{0};

inline uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline float rdf(uint32_t ea) { const uint32_t u = rd32(ea); float f; memcpy(&f, &u, 4); return f; }

int thread_no()
{
    static thread_local int id = -1;
    if (id < 0) id = g_thread_ids.fetch_add(1);
    return id;
}

void put_vec(std::string& s, uint32_t ea)
{
    if (!ea) { s += " null"; return; }
    char b[96];
    snprintf(b, sizeof b, " (%.5g,%.5g,%.5g)", rdf(ea), rdf(ea + 4), rdf(ea + 8));
    s += b;
}

void put_str(std::string& s, uint32_t ea, size_t max)
{
    if (!ea) { s += " null"; return; }
    s += " \"";
    for (size_t i = 0; i < max; i++) {
        const char c = (char)vm_base[ea + i];
        if (!c) break;
        s += (c >= 32 && c < 127 && c != '"') ? c : '?';
    }
    s += '"';
}

struct Hook { uint32_t addr; const char* name; const char* spec; void (*fn)(ppu_context*); };

#define AUDIO_HOOK(hex, name, spec) { 0x##hex, name, spec, func_##hex },
const Hook g_hooks[] = {
#include "audio_hook_table.inc"
};
#undef AUDIO_HOOK
constexpr int kHooks = (int)(sizeof g_hooks / sizeof g_hooks[0]);

int hook_index(uint32_t addr)
{
    for (int i = 0; i < kHooks; i++) if (g_hooks[i].addr == addr) return i;
    return -1;
}

/* One traced call: the arguments as the hook found them, the lifted function run (its own hook declines while
 * `inner` is set) with its tail calls drained, the outputs as the function left them. */
void traced_call(ppu_context* ctx, const Hook& h, int& inner)
{
    const uint64_t g[8] = { ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7], ctx->gpr[8], ctx->gpr[9], ctx->gpr[10] };
    const double f[4] = { ctx->fpr[1], ctx->fpr[2], ctx->fpr[3], ctx->fpr[4] };
    const auto t = std::chrono::steady_clock::now();
    const uint32_t frame = rd32(kGameFrame);
    std::string line;
    line.reserve(160);
    char b[160];
    const char* sp = h.spec;
    int gi = 1, fi = 0;   // the next integer argument (g[gi]: r4 first) and float argument (f[fi]: f1 first)
    if (*sp == '!') { gi = 0; sp++; }
    else if (*sp == '.') sp++;
    else { snprintf(b, sizeof b, " this=%X", (uint32_t)g[0]); line += b; }
    struct Out { char code; uint32_t ea, len; } outs[4];
    int nout = 0;
    for (; *sp && gi < 8 && fi < 4; sp++) {
        switch (*sp) {
        case 'i': snprintf(b, sizeof b, " %d", (int32_t)(uint32_t)g[gi++]); line += b; break;
        case 'x': snprintf(b, sizeof b, " %X", (uint32_t)g[gi++]); line += b; break;
        case 'f': snprintf(b, sizeof b, " %.7g", f[fi++]); line += b; break;
        case 'v': put_vec(line, (uint32_t)g[gi++]); break;
        case 's': put_str(line, (uint32_t)g[gi++], 255); break;
        case 'S': if (nout < 4 && gi < 7) outs[nout++] = { 'S', (uint32_t)g[gi], (uint32_t)g[gi + 1] }; gi += 2; break;
        default: if (nout < 4) outs[nout++] = { *sp, (uint32_t)g[gi], 0 }; gi++; break;   // o b F
        }
    }
    inner = 1;
    h.fn(ctx);
    while (g_trampoline_fn) { void (*fn)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; fn(ctx); }
    inner = 0;
    snprintf(b, sizeof b, " -> %X", (uint32_t)ctx->gpr[3]);
    line += b;
    for (int k = 0; k < nout; k++) {
        const Out& o = outs[k];
        if (!o.ea) { line += " out=null"; continue; }
        switch (o.code) {
        case 'o': snprintf(b, sizeof b, " out=%X", rd32(o.ea)); line += b; break;
        case 'b': snprintf(b, sizeof b, " out=%u", vm_base[o.ea]); line += b; break;
        case 'F': snprintf(b, sizeof b, " out=%.7g", rdf(o.ea)); line += b; break;
        case 'S': line += " out="; put_str(line, o.ea, o.len && o.len < 256 ? o.len : 255); break;
        }
    }
    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(t - g_t0).count();
    std::lock_guard<std::mutex> lk(g_trace_mu);
    fprintf(g_trace, "%lld %u %d %s%s\n", us, frame, thread_no(), h.name, line.c_str());
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last_flush > std::chrono::milliseconds(500)) { fflush(g_trace); g_last_flush = now; }
}

// ---- DOTHEROES_DEV_SOUND: the sound lab -----------------------------------------------------------------------
struct LabStep { double t; char op[12]; int id; float f; int loop; bool done; };
std::vector<LabStep> g_lab;
std::chrono::steady_clock::time_point g_lab_t0;
bool g_lab_started = false;

void lab_parse(const char* spec)
{
    std::string all(spec);
    size_t at = 0;
    while (at < all.size()) {
        size_t end = all.find(',', at);
        if (end == std::string::npos) end = all.size();
        const std::string one = all.substr(at, end - at);
        at = end + 1;
        LabStep st{};
        char op[12] = {};
        double a = 0, b = 0;
        const int n = sscanf(one.c_str(), "%lf:%11[a-z]:%lf:%lf", &st.t, op, &a, &b);
        if (n < 2) { fprintf(stderr, "[sound-lab] not understood: %s\n", one.c_str()); continue; }
        memcpy(st.op, op, sizeof st.op);
        st.id = (int)a;
        st.f = (float)a;
        st.loop = n >= 4 ? (int)b : 0;
        g_lab.push_back(st);
    }
}

// One of the sound manager's methods called from the start of its update, as a call from the game would: the
// registers the update was entered with put back afterwards.
void lab_call(ppu_context* ctx, void (*fn)(ppu_context*), int r4, int r5, int r6, double f1)
{
    uint64_t gpr[32];
    double fpr[32];
    memcpy(gpr, ctx->gpr, sizeof gpr);
    memcpy(fpr, ctx->fpr, sizeof fpr);
    const uint64_t lr = ctx->lr, ctr = ctx->ctr;
    const uint32_t cr = ctx->cr, xer = ctx->xer;
    ctx->gpr[4] = (uint64_t)(int64_t)r4;
    ctx->gpr[5] = (uint64_t)(int64_t)r5;
    ctx->gpr[6] = (uint64_t)(int64_t)r6;
    ctx->fpr[1] = f1;
    ctx->lr = kUpdate;
    fn(ctx);
    while (g_trampoline_fn) { void (*f)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; f(ctx); }
    memcpy(ctx->gpr, gpr, sizeof gpr);
    memcpy(ctx->fpr, fpr, sizeof fpr);
    ctx->lr = lr; ctx->ctr = ctr; ctx->cr = cr; ctx->xer = xer;
}

void lab_tick(ppu_context* ctx)
{
    const auto now = std::chrono::steady_clock::now();
    if (!g_lab_started) { g_lab_started = true; g_lab_t0 = now; }
    const double t = std::chrono::duration<double>(now - g_lab_t0).count();
    for (LabStep& st : g_lab) {
        if (st.done || t < st.t) continue;
        st.done = true;
        const std::string op = st.op;
        fprintf(stderr, "[sound-lab] t=%.2f (frame %u) %s %g%s\n", t, rd32(kGameFrame), st.op, (double)st.f,
                op == "bgm" ? (st.loop ? " loop" : " once") : "");
        if (op == "se") lab_call(ctx, func_0046E0EC, st.id, 128, 0, 0.0);
        else if (op == "stopse") lab_call(ctx, func_00466A1C, st.id, 0, 0, 0.0);
        else if (op == "bgm") lab_call(ctx, func_0046F4BC, st.id, 128, st.loop, 0.0);
        else if (op == "fadebgm") lab_call(ctx, func_0046638C, 0, 0, 0, st.f);
        else if (op == "fadeloops") lab_call(ctx, func_00466324, 0, 0, 0, st.f);
        else fprintf(stderr, "[sound-lab] unknown step %s\n", st.op);
    }
}

int hook(ppu_context* ctx, int index, int& inner)
{
    if (inner || index < 0) return 0;   // the lifted function re-entered from traced_call
    if (!g_lab.empty() && g_hooks[index].addr == kUpdate) lab_tick(ctx);
    if (!g_trace) return 0;
    traced_call(ctx, g_hooks[index], inner);
    return 1;
}

}  // namespace

/* One host per hooked function, by the name config/audio_hooks.json gives the lifter. */
#define AUDIO_HOOK(hex, name, spec)                                                                                \
    extern "C" int dotheroes_audio_##hex(ppu_context* ctx)                                                         \
    {                                                                                                              \
        static thread_local int inner = 0;                                                                         \
        static const int index = hook_index(0x##hex);                                                              \
        return hook(ctx, index, inner);                                                                            \
    }
#include "audio_hook_table.inc"
#undef AUDIO_HOOK

/* Before the guest starts (main.cpp). */
extern "C" void dotheroes_audio_hooks_init(void)
{
    const char* lab = getenv("DOTHEROES_DEV_SOUND");
    if (lab && *lab) {
        lab_parse(lab);
        g_audio_hooks = 1;
        fprintf(stderr, "[sound-lab] %zu step(s) (DOTHEROES_DEV_SOUND)\n", g_lab.size());
    }
    const char* path = getenv("DOTHEROES_AUDIO_TRACE");
    if (!path || !*path) return;
    g_trace = fopen(path, "wb");
    if (!g_trace) { fprintf(stderr, "[audio-trace] cannot write %s\n", path); return; }
    setvbuf(g_trace, nullptr, _IOFBF, 1 << 20);
    g_t0 = g_last_flush = std::chrono::steady_clock::now();
    fprintf(g_trace, "# audio trace v1: <us> <game frame> <thread> <name> <args> -> <r3> <outs> (port/audio_hooks.cpp)\n");
    g_audio_hooks = 1;
    atexit([] { std::lock_guard<std::mutex> lk(g_trace_mu); fflush(g_trace); });   // the game's own exit
    fprintf(stderr, "[audio-trace] %d hooks, writing %s\n", kHooks, path);
}
