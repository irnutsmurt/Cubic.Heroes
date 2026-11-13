/*
 * Guards for the guest instruction patches in config/insn_patches.json.
 *
 * g_dotheroes_fps_nosleep, g_dotheroes_fps_vblank1, g_dotheroes_fps_novblank
 *   The frame-rate cap. The main loop (0x2A5C8) holds every frame to at
 *   least 30 ms, then func_00029BD8 waits for two vblanks since the last
 *   frame: 30 fps. The game's logic steps by measured time (the loop turns
 *   sys_time_get_system_time into a per-frame delta in seconds), so a
 *   higher rate does not speed the game up -- its timers, physics and
 *   animation follow time (tests/fps_check.py) -- with one exception, the
 *   hero's step rounding (g_dotheroes_step_carry, below).
 *   The cap was removed at the user's request; DOTHEROES_FPS selects:
 *     30  the original pacing (no patch active)
 *     60  no 30 ms floor; one vblank per frame (default)
 *     display  as 60, the vblank at a whole number of the display's
 *         refreshes (72 fps on a 144 Hz display; main.cpp, display_frame_ms)
 *     0   no 30 ms floor and no vblank wait: as fast as the host runs
 *
 * g_dotheroes_step_carry (an entry hook: dotheroes_step_round)
 *   func_000A2C14 rounds a float to 0.01 by truncation: (float)(int)(v * 100
 *   + 0.005) * 0.01, the constants from its TOC. Most of its 23 callers snap
 *   a value just before converting it to an integer. Two are the hero's
 *   movement: func_00106CB8 rounds each frame's x and z step (+0x768 and
 *   +0x770 of the player object, made by func_00119BEC from the speed and
 *   the frame's time) and then moves by it. Truncating drops up to 0.01 a
 *   frame, so the more frames a second, the slower the hero walks: 8.85
 *   units a second at 30 fps, 8.70 at 60, ~8.3 at 120, against its 9.
 *   With the hook (DOTHEROES_STEP_CARRY=0 turns it off), the part dropped at
 *   those two calls is carried into the next frame's step on that axis: the
 *   steps stay multiples of 0.01, as the collision code has always had them,
 *   and add up to the speed at any frame rate. The carry restarts when the
 *   step is 0 or reverses, or the object is another. Every other call
 *   rounds exactly as before.
 *
 * g_dotheroes_tick30 (entry hooks: tick30_gate)
 *   Most of the game's objects scale their motion by the frame's time. The
 *   segmented boss of the first dungeon does not: its head's update calls on
 *   with a constant 1/30 s instead of the frame's time (the only update in the
 *   game that does), and its body segments' update (this, &dt) follows the
 *   head's trail a step a frame (+0x770), counts frames (+0x75C) for its turns
 *   and its attacks, and scales only its sideways wobble by dt -- it was
 *   written for the game's locked 30 fps. Measured in real seconds with the hero
 *   standing still (tests/boss_timing.py): the body's median speed 4.5 units a
 *   second at 30 fps, 7.5 at 60, 20 at 144, and 2.5 times the orbs. The gate
 *   runs such an update at 30 Hz: time accumulates on one clock for all of
 *   them, and the updates run only in a frame where a 1/30 s step is due, with
 *   dt reading whole steps. Above
 *   30 fps the object moves at the 30 fps game's pace, in 30 Hz steps.
 *   DOTHEROES_TICK30=0 turns it off; at DOTHEROES_FPS=30 it is off (the
 *   original pacing).
 */
extern "C" {
int g_dotheroes_fps_nosleep = 0;
int g_dotheroes_fps_vblank1 = 0;
int g_dotheroes_fps_novblank = 0;
int g_dotheroes_step_carry = 1;
}

#include "ppu_recomp.h"   /* ppu_context */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* func_000A2C14 for an entry hook: see g_dotheroes_step_carry. The rounding
 * is the lifted function's, operation for operation: a fused multiply-add
 * rounded to single, a truncating conversion (NaN and overflow as fctiwz),
 * the integer back to single, a single multiply. */
static double step_snap(double v)
{
    const double scaled = (double)(float)fma(v, 100.0, (double)0.005f);
    int32_t i;
    if (scaled != scaled) i = INT32_MIN;
    else if (scaled >= 2147483647.0) i = INT32_MAX;
    else if (scaled <= -2147483648.0) i = INT32_MIN;
    else i = (int32_t)trunc(scaled);
    return (double)(float)((double)(float)(double)i * (double)0.01f);
}

extern "C" void dotheroes_step_round(ppu_context* ctx)
{
    /* The hero's two step calls in func_00106CB8, by their return addresses:
     * x (+0x768), then z (+0x770); the player object is in r30. */
    static struct { uint32_t obj; float carry; } s_axis[2];
    const uint32_t lr = (uint32_t)ctx->lr;
    const int axis = lr == 0x00106FB8u ? 0 : lr == 0x00106FCCu ? 1 : -1;
    const double v = ctx->fpr[1];
    if (axis < 0) {
        ctx->fpr[1] = step_snap(v);
        return;
    }
    const uint32_t obj = (uint32_t)ctx->gpr[30];
    float& carry = s_axis[axis].carry;
    if (s_axis[axis].obj != obj || v == 0.0 || v != v || (carry != 0.0f && (v > 0.0) != (carry > 0.0f))) {
        s_axis[axis].obj = obj;
        carry = 0.0f;
    }
    const double want = (double)(float)(v + (double)carry);
    const double step = step_snap(want);
    carry = (float)(want - step);
    ctx->fpr[1] = step;
}

/* See g_dotheroes_tick30. `dt_ea` (r4) is the caller's frame time, shared by
 * the objects it walks: replaced for this call and put back. The update is
 * re-entered through its own hook (`inner` declines).
 *
 * One 30 Hz clock for every gated object, as the original's one frame was:
 * the frame's time is added once a game frame (the frame is the game's own
 * count, 0x02CF3570, which func_0094F290 steps at the frame's start, before
 * the objects' walk), and in a frame where a step is due every gated object
 * takes it -- the head and the segments that read its trail stay in step,
 * whenever each was made. Time passes for the clock only in frames with a
 * gated call: in a pause or a menu that stops the walk, or with no boss, it
 * stands. DOTHEROES_TICK30_LOG=1: what the clock did (a line per object first
 * seen or seen again after a gap, a frame with more than one step, a summary
 * every 5 s). */
extern "C" { int g_dotheroes_tick30 = 1; }
static int g_tick30_log = 0;

static uint32_t guest_u32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static float guest_float(uint32_t ea)
{
    const uint32_t u = guest_u32(ea);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static void guest_float_set(uint32_t ea, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(u >> 24); p[1] = (uint8_t)(u >> 16); p[2] = (uint8_t)(u >> 8); p[3] = (uint8_t)u;
}

static const uint32_t kGameFrame = 0x02CF3570;   /* the game's frame count (docs/NATIVE_R8.md section 3) */
static struct {
    uint32_t frame;        /* the game frame the clock last advanced in */
    int      due;          /* steps due in that frame */
    float    acc;          /* time not yet stepped */
    /* DOTHEROES_TICK30_LOG */
    uint32_t log_frame0, frames, due_frames, steps, max_due;
    double   dt_sum;
    struct { uint32_t obj, last_frame, calls; } obj[16];
} g_t30 = { 0xFFFFFFFFu, 0, 0.0f, 0, 0, 0, 0, 0, 0.0, {} };

static void tick30_log_call(uint32_t obj, uint32_t frame, float dt, int due, void (*body)(ppu_context*))
{
    const char* what = body == func_001E47C4 ? "head" : "segment";
    int k = -1, free_k = -1;
    for (int i = 0; i < 16; i++) {
        if (g_t30.obj[i].obj == obj) { k = i; break; }
        if (!g_t30.obj[i].obj && free_k < 0) free_k = i;
    }
    if (k < 0) {
        k = free_k >= 0 ? free_k : (int)(obj >> 4 & 15);
        g_t30.obj[k].obj = obj; g_t30.obj[k].calls = 0;
        fprintf(stderr, "[tick30] %s %08X first call at game frame %u (dt %.4f, clock %.4f s owed)\n", what, obj, frame, dt, g_t30.acc);
    } else if (frame - g_t30.obj[k].last_frame > 30) {
        fprintf(stderr, "[tick30] %s %08X called again after %u game frames (dt %.4f, %d step%s due)\n",
                what, obj, frame - g_t30.obj[k].last_frame, dt, due, due == 1 ? "" : "s");
    }
    g_t30.obj[k].last_frame = frame;
    g_t30.obj[k].calls++;
}

static int tick30_gate(ppu_context* ctx, void (*body)(ppu_context*))
{
    static thread_local int inner = 0;
    if (inner) return 0;   /* the update itself, called below */
    const uint32_t dt_ea = (uint32_t)ctx->gpr[4];
    const float dt = guest_float(dt_ea);
    const uint32_t frame = guest_u32(kGameFrame);
    if (frame != g_t30.frame) {   /* the frame's first gated call: the clock advances */
        if (g_tick30_log && frame - g_t30.frame > 30 && g_t30.frame != 0xFFFFFFFFu)
            fprintf(stderr, "[tick30] gated calls resume after %u game frames without one\n", frame - g_t30.frame);
        g_t30.frame = frame;
        g_t30.acc += dt;
        /* the 0.2: a frame a hair under 1/30 s (30 fps with jitter) still takes its step */
        int due = (int)floor(g_t30.acc * 30.0f + 0.2f);
        if (due < 0) due = 0;
        g_t30.due = due;
        g_t30.acc -= (float)due / 30.0f;
        if (g_tick30_log) {
            if (!g_t30.frames) g_t30.log_frame0 = frame;
            g_t30.frames++; g_t30.dt_sum += dt;
            if (due) g_t30.due_frames++;
            g_t30.steps += (uint32_t)due;
            if ((uint32_t)due > g_t30.max_due) g_t30.max_due = (uint32_t)due;
            if (due > 1) fprintf(stderr, "[tick30] game frame %u: %d steps due at once (dt %.4f)\n", frame, due, dt);
            if (g_t30.dt_sum >= 5.0) {
                fprintf(stderr, "[tick30] %.2f s of frame time over %u game frames with gated calls: %u steps "
                        "(%.2f a second), %u frames stepped, at most %u at once\n",
                        g_t30.dt_sum, g_t30.frames, g_t30.steps, g_t30.steps / g_t30.dt_sum, g_t30.due_frames, g_t30.max_due);
                g_t30.frames = g_t30.due_frames = g_t30.steps = g_t30.max_due = 0;
                g_t30.dt_sum = 0.0;
            }
        }
    }
    if (g_tick30_log) tick30_log_call((uint32_t)ctx->gpr[3], frame, dt, g_t30.due, body);
    if (g_t30.due <= 0) return 1;   /* not due: this frame's call does nothing */
    {   /* an object steps once a game frame, however often it is called in it */
        static struct { uint32_t obj, frame; } s[64];
        const uint32_t obj = (uint32_t)ctx->gpr[3];
        uint32_t i = (obj >> 4) * 2654435761u >> 26, k = 0;
        while (k < 64 && s[i].obj && s[i].obj != obj) { i = (i + 1) & 63; k++; }
        if (k == 64) { i = (obj >> 4) * 2654435761u >> 26; s[i].frame = frame - 1; }   /* full: take the slot */
        if (s[i].obj == obj && s[i].frame == frame) {
            static int said = 0;
            if (said++ < 8) fprintf(stderr, "[tick30] %08X called again in game frame %u: not stepped twice\n", obj, frame);
            return 1;
        }
        s[i].obj = obj; s[i].frame = frame;
    }
    guest_float_set(dt_ea, (float)g_t30.due / 30.0f);
    inner = 1;
    body(ctx);
    inner = 0;
    guest_float_set(dt_ea, dt);
    return 1;
}

extern "C" int dotheroes_tick30_boss_segment(ppu_context* ctx) { return tick30_gate(ctx, func_001F2DC0); }
extern "C" int dotheroes_tick30_boss_head(ppu_context* ctx) { return tick30_gate(ctx, func_001E47C4); }

/* Before the guest starts. */
extern "C" void dotheroes_configure_patches(void)
{
    const char* e = getenv("DOTHEROES_FPS");
    /* display: a frame a vblank as at 60, the vblank paced to the display
     * (main.cpp, display_frame_ms) */
    const int fps = e && !strcmp(e, "display") ? 60 : e && *e ? atoi(e) : 60;
    {
        const char* t30 = getenv("DOTHEROES_TICK30");
        g_dotheroes_tick30 = fps != 30 && !(t30 && *t30 == '0');
        const char* t30log = getenv("DOTHEROES_TICK30_LOG");
        g_tick30_log = t30log && *t30log == '1';
        fprintf(stderr, "[port] frame-counted boss updates: %s\n", g_dotheroes_tick30 ? "gated to 30 Hz" : "every frame");
    }
    g_dotheroes_fps_nosleep  = fps != 30;
    g_dotheroes_fps_vblank1  = fps == 60;
    g_dotheroes_fps_novblank = fps != 30 && fps != 60;
    fprintf(stderr, "[port] frame cap: %s\n",
            fps == 30 ? "30 fps (original)" : e && !strcmp(e, "display") ? "a frame a vblank, the display's pace"
            : fps == 60 ? "60 fps (vblank)" : "none");
    const char* sc = getenv("DOTHEROES_STEP_CARRY");
    g_dotheroes_step_carry = !(sc && *sc == '0');
    fprintf(stderr, "[port] hero step rounding: %s\n",
            g_dotheroes_step_carry ? "remainder carried (DOTHEROES_STEP_CARRY=0: truncated, as the game does)"
                                   : "truncated, as the game does");
}
