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
#include "native/audio/native_sound.h"   /* DOTHEROES_NATIVE_AUDIO: the native sound manager (M3) */

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

int native_call(ppu_context* ctx, uint32_t addr);

/* One traced call: the arguments as the hook found them, the lifted function run (its own hook declines while
 * `inner` is set) with its tail calls drained -- or, with the native sound manager, its answer -- and the outputs
 * as the function left them. */
void traced_call(ppu_context* ctx, const Hook& h, int& inner, bool native = false)
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
    if (native) {
        native_call(ctx, h.addr);
    } else {
        inner = 1;
        h.fn(ctx);
        while (g_trampoline_fn) { void (*fn)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; fn(ctx); }
        inner = 0;
    }
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

// ---- DOTHEROES_NATIVE_AUDIO: the native sound manager (port/native/audio/native_sound.h, M3) -----------------
// Each of the game's sound-manager methods answered by the native manager on OpenAL Soft; the lifted method
// does not run, FMOD is never initialised. An FMOD API function reached anyway is counted (it should never be:
// the sound manager is FMOD's only caller) and reported at exit.
int g_native = 0;
std::atomic<uint64_t> g_fmod_reached{0};

bool rdvec(uint32_t ea, float out[3])
{
    if (!ea) return false;
    out[0] = rdf(ea);
    out[1] = rdf(ea + 4);
    out[2] = rdf(ea + 8);
    return true;
}

extern "C" const char* ppu_vfs_root;   // the host folder the game's /dev_bdvd maps to (the runtime's ppu_fs.cpp)

// The game's own state the sound manager reads (rel27's class reads the same): the options' music and effect
// levels, the system menu's pause byte, the exit-request byte (cleared when the system asks the game to quit),
// and the byte that makes the class's shutdown return at once.
constexpr uint32_t kGame = 0x0159AB58;
constexpr uint32_t kBgmLevel = kGame + 0x948, kSeLevel = kGame + 0x94C;
constexpr uint32_t kMenuPause = kGame + 0x764, kNotExiting = kGame + 0x761;
constexpr uint32_t kNoShutdown = 0x0159B558;

void native_log(const char* line) { fprintf(stderr, "[audio] %s\n", line); }

void native_state()
{
    nsm_set_game_state((int)rd32(kBgmLevel), (int)rd32(kSeLevel), vm_base[kMenuPause] != 0, vm_base[kNotExiting] == 0);
}

// The class's init, natively: the device, the reverb, the music streamer, the voices and the player's sound list.
// The sound folder is the disc's: the game's /dev_bdvd/PS3_GAME/USRDIR/data/sound, mapped as the runtime maps the
// game's own file access (ppu_fs.cpp: /dev_bdvd/ -> the VFS root).
void native_init()
{
    static bool done = false;
    if (done) return;
    done = true;
    const std::string dir = std::string(ppu_vfs_root ? ppu_vfs_root : ".") + "/PS3_GAME/USRDIR/data/sound";
    nsm_options o{};
    o.sound_dir = dir.c_str();
    o.log = native_log;
    if (!nsm_init(&o)) fprintf(stderr, "[audio] the native sound manager is silent (no library, device or list)\n");
}

int native_call(ppu_context* ctx, uint32_t addr)
{
    const int r4 = (int)(int32_t)(uint32_t)ctx->gpr[4], r5 = (int)(int32_t)(uint32_t)ctx->gpr[5];
    const int r6 = (int)(int32_t)(uint32_t)ctx->gpr[6];
    const float f1 = (float)ctx->fpr[1], f2 = (float)ctx->fpr[2];
    float a[3], b[3];
    uint64_t r3 = 0;
    native_state();
    switch (addr) {
    case 0x0046E0EC: r3 = nsm_play_se(r4, r5, rdvec((uint32_t)r6, a) ? a : nullptr); break;   // playSE(id, prio, pos)
    case 0x0046DD6C: r3 = nsm_play_se_fade_in(r4, r6, f1); break;     // playSEFadeIn(id, (unused), prio, T)
    case 0x00465AC0: r3 = (uint64_t)nsm_is_playing(r4); break;         // isPlaying(id)
    case 0x00466A1C: r3 = (uint64_t)nsm_stop_se(r4); break;            // stopSE(id)
    case 0x00465C28: r3 = (uint64_t)nsm_stop_channel((uint32_t)ctx->gpr[4]); break;   // stopChannel(handle)
    case 0x004666FC: r3 = (uint64_t)nsm_fade_out_se(r4, f1); break;    // fadeOutSE(id, T)
    case 0x00466324: r3 = (uint64_t)nsm_fade_out_loop_ses(f1); break;  // fadeOutLoopSEs(T)
    case 0x00466418: r3 = (uint64_t)nsm_pause_by_id(r4, r5); break;    // pauseById(id, paused)
    case 0x0046F4BC: nsm_play_bgm(r4, r5, r6); break;                  // playBGM(id, prio, loop)
    case 0x0046638C: r3 = (uint64_t)nsm_fade_out_bgm(f1); break;       // fadeOutBGM(T)
    case 0x004662A8: r3 = (uint64_t)nsm_is_bgm_active(); break;        // isBGMActive()
    case 0x004660AC: r3 = (uint64_t)(int64_t)nsm_current_bgm_id(); break;   // currentBGMId()
    case 0x0046624C: nsm_duck_bgm(f1, f2); break;                      // duckBGM(T, level)
    case 0x004661F4: nsm_unduck_bgm(f1); break;                        // unduckBGM(T)
    case 0x00466184: nsm_duck_se(f1); break;                           // duckSE(T)
    case 0x00466128: nsm_unduck_se(f1); break;                         // unduckSE(T)
    case 0x0046C1AC: nsm_set_bgm_master(f1); break;                    // setBGMMaster(m)
    case 0x0046C364: nsm_set_sound_volume(r4, f1); break;              // setSoundVolume(id, volume)
    case 0x0046C3F0: nsm_apply_volume_options(); break;                // applyVolumeOptions()
    case 0x00465D9C:                                                   // setListener(pos, velocity)
        if (rdvec((uint32_t)ctx->gpr[4], a)) nsm_set_listener(a, rdvec((uint32_t)ctx->gpr[5], b) ? b : nullptr);
        break;
    case 0x00465D60: nsm_set_reverb(r4); break;                        // setReverb(on)
    case 0x004664E0: r3 = (uint64_t)nsm_is_loaded(r4); break;          // isLoaded(id)
    case 0x0046DB40: nsm_update(f1); break;                            // update(dt)
    case 0x00465E30: native_init(); break;                             // init()
    case 0x00471604: nsm_load_sounds(); break;                         // loadSounds(): the effects resident
    case 0x00472A14: break;                                            // preload(): the files are read by loadSounds
    case 0x0046E6D4: if (!vm_base[kNoShutdown]) nsm_shutdown(); break; // shutdown()
    case 0x004708B4: break;                                            // the stream-open callback: never called
    default: return 0;
    }
    ctx->gpr[3] = r3;
    return 1;
}

int hook(ppu_context* ctx, int index, int& inner)
{
    if (inner || index < 0) return 0;   // the lifted function re-entered from traced_call
    if (!g_lab.empty() && g_hooks[index].addr == kUpdate) lab_tick(ctx);
    if (g_native) {
        const uint32_t addr = g_hooks[index].addr;
        if (addr >= 0x00800000) {   // an FMOD API function: never expected with the native manager
            if (g_fmod_reached++ == 0) fprintf(stderr, "[audio] FMOD reached with the native sound manager: %s\n", g_hooks[index].name);
            return 0;
        }
        if (g_trace) {
            traced_call(ctx, g_hooks[index], inner, true);
            return 1;
        }
        return native_call(ctx, addr);
    }
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
    // DOTHEROES_NATIVE_AUDIO=1 (M3, a switch while the old path is kept for comparisons): the native sound manager
    const char* nat = getenv("DOTHEROES_NATIVE_AUDIO");
    if (nat && *nat == '1') {
        g_native = 1;
        g_audio_hooks = 1;
        fprintf(stderr, "[audio] the native sound manager (OpenAL Soft) in place of FMOD (DOTHEROES_NATIVE_AUDIO)\n");
        atexit([] {
            fprintf(stderr, "[audio] at exit: FMOD API calls reached %llu\n", (unsigned long long)g_fmod_reached.load());
        });
    }
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
