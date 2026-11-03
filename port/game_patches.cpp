/*
 * Guards for the guest instruction patches in config/insn_patches.json.
 *
 * g_dotheroes_skip_flash_mp3
 *   FMOD Ex's PS3 MultiStream backend loads an SPU MP3 decoder from the
 *   console firmware (/dev_flash/sys/external/flashMP3.pic) while FMOD
 *   initialises, and the game retries FMOD init forever when it is missing.
 *   The port ships no firmware, and that codec is SPU code loaded at run time,
 *   which only an interpreter could execute. 3D Dot Game Heroes has no MP3
 *   data (every sound on the disc is PCM WAV), so the loader's failure exit
 *   (func_008D0EBC, 0x8D0F4C: `li r11,33` -> `li r11,0`) reports success.
 *
 *   What that leaves: the loader has already allocated the codec buffer
 *   (fixed 0x18080 bytes, func_008DE370) and recorded it, so FMOD holds a
 *   codec pointer to an EMPTY buffer. If an MP3 were ever decoded, FMOD's SPU
 *   mixer would branch into that buffer; there is no lifted code for it, so a
 *   development build stops there in strict mode (spu-unlifted-branch) rather
 *   than running anything. See docs/FEASIBILITY.md, "Explicit exceptions".
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
 */
extern "C" {
int g_dotheroes_skip_flash_mp3 = 1;
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

/* Before the guest starts. */
extern "C" void dotheroes_configure_patches(void)
{
    const char* e = getenv("DOTHEROES_FPS");
    /* display: a frame a vblank as at 60, the vblank paced to the display
     * (main.cpp, display_frame_ms) */
    const int fps = e && !strcmp(e, "display") ? 60 : e && *e ? atoi(e) : 60;
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
