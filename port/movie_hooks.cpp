/* The credits on demand, a developer's test switch (M8, docs/FFMPEG_MIGRATION_PLAN.md section 2.2): the game plays its
 * ending -- and with it the credits movie -- when its event script calls the Lua function Ending at the end of the
 * game: the binding (func_004F0EB4) calls func_004CDF70, which calls GameClass's Ending method (func_00509A98) on the
 * game's GameClass (its pointer in the global at TOC 0x01057910 + 0x3A80). With DOTHEROES_DEV_CREDITS set, the same
 * call is made from GameClass's frame function (func_00514F8C, an entry hook that declines: its body then runs as
 * usual) on the game's own thread, at the start of a frame:
 *   DOTHEROES_DEV_CREDITS=key     when F6 is pressed (port/native/dev_state.cpp's keys; the window focused)
 *   DOTHEROES_DEV_CREDITS=<s>     <s> seconds after GameClass's first frame (a test run), and on F6
 * and, either way, when a file credits.trigger appears in the game's folder (a test drive's `touch`).
 * Without the variable the guard is 0 and the hook is never entered. It is meant for a copy of a save: what the game
 * does after its credits (a cleared game's save) is the game's own. */
#include "ppu_recomp.h"   /* ppu_context, func_004CDF70 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);   // the lift's tail calls (as phys_hooks.cpp)

extern "C" int g_dev_credits = 0;
extern "C" void (*g_dev_credits_request)(void);   // port/native/dev_state.cpp: called on F6
extern "C" void dev_credits_request(void);

namespace {

constexpr uint32_t kToc = 0x01057910u;          // GameClass's group's TOC (its frame function and Ending run with it)
constexpr uint32_t kGameSlot = kToc + 0x3A80u;  // the global holding the game's GameClass pointer (func_004CDF70)

std::atomic<int> s_request{0};
double s_at = -1;         // automatic: seconds after the first frame
bool s_t0_set = false;
std::chrono::steady_clock::time_point s_t0;
int s_calls = 0;

inline uint32_t rd32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

inline void wr64(uint32_t ea, uint64_t v)
{
    uint8_t* p = vm_base + ea;
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i));
}

struct DevCreditsInit {
    DevCreditsInit()
    {
        const char* e = getenv("DOTHEROES_DEV_CREDITS");
        if (!e || !*e) return;
        g_dev_credits = 1;
        g_dev_credits_request = dev_credits_request;
        if (strcmp(e, "key") != 0) s_at = atof(e);
        if (s_at >= 0)
            fprintf(stderr, "[dev-credits] the game's Ending at %.1f s after GameClass's first frame, and on F6\n", s_at);
        else
            fprintf(stderr, "[dev-credits] the game's Ending on F6\n");
    }
} s_init;

}  // namespace

// port/native/dev_state.cpp: F6 (through g_dev_credits_request)
extern "C" void dev_credits_request(void)
{
    if (!g_dev_credits) return;
    s_request.store(1);
    fprintf(stderr, "[dev-credits] F6: the game's Ending at GameClass's next frame\n");
}

// func_00514F8C (r3 = the GameClass, r2 = kToc): its frame function. 0: its body runs as usual.
extern "C" int dotheroes_dev_credits(ppu_context* ctx)
{
    static int inner;
    if (inner) return 0;
    const auto now = std::chrono::steady_clock::now();
    if (!s_t0_set) { s_t0_set = true; s_t0 = now; }
    const double t = std::chrono::duration<double>(now - s_t0).count();
    if (s_at >= 0 && t >= s_at) {
        s_at = -1;
        s_request.store(1);
    }
    static unsigned frames;
    if (++frames % 30 == 0) {   // a test drive's `touch credits.trigger` (the run's folder is the game's)
        FILE* trig = fopen("credits.trigger", "rb");
        if (trig) {
            fclose(trig);
            remove("credits.trigger");
            s_request.store(1);
            fprintf(stderr, "[dev-credits] credits.trigger at %.1f s\n", t);
        }
    }
    if (!s_request.exchange(0)) return 0;
    const uint32_t game = rd32(rd32(kGameSlot));
    if (!game) {
        fprintf(stderr, "[dev-credits] not called at %.1f s: no GameClass yet\n", t);
        return 0;
    }
    if (game != (uint32_t)ctx->gpr[3] || (uint32_t)ctx->gpr[2] != kToc)
        fprintf(stderr, "[dev-credits] note: this frame function's r3 0x%08X, r2 0x%08X (the GameClass 0x%08X)\n",
                (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[2], game);
    const uint32_t mode = rd32(game + 0x20), step = rd32(game + 0x28);
    // a call as func_004CDF70's caller makes it: its own frame below this one, r2 the TOC; every register back after
    uint64_t g[32];
    double f[32];
    memcpy(g, ctx->gpr, sizeof g);
    memcpy(f, ctx->fpr, sizeof f);
    const uint64_t lr = ctx->lr, ctr = ctx->ctr;
    const uint32_t cr = ctx->cr, xer = ctx->xer;
    const uint32_t sp = (uint32_t)ctx->gpr[1], frame = sp - 0x100;
    wr64(frame, sp);
    ctx->gpr[1] = frame;
    ctx->gpr[2] = kToc;
    ctx->lr = 0x00514F8Cu;
    inner = 1;
    func_004CDF70(ctx);
    while (g_trampoline_fn) { void (*fn)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; fn(ctx); }
    inner = 0;
    memcpy(ctx->gpr, g, sizeof g);
    memcpy(ctx->fpr, f, sizeof f);
    ctx->lr = lr; ctx->ctr = ctr; ctx->cr = cr; ctx->xer = xer;
    s_calls++;
    fprintf(stderr, "[dev-credits] the game's Ending called at %.1f s on GameClass 0x%08X: mode %u step %u -> mode %u "
            "step %u\n", t, game, mode, step, rd32(game + 0x20), rd32(game + 0x28));
    return 0;
}
