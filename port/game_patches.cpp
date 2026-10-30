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
 *   higher rate does not speed the game up: walking for a fixed 1.5 s ends
 *   in the same place at 20 and at 31 fps (tests/scripts/speed.drive).
 *   The cap was removed at the user's request; DOTHEROES_FPS selects:
 *     30  the original pacing (no patch active)
 *     60  no 30 ms floor; one vblank per frame (default)
 *     0   no 30 ms floor and no vblank wait: as fast as the host runs
 */
extern "C" {
int g_dotheroes_skip_flash_mp3 = 1;
int g_dotheroes_fps_nosleep = 0;
int g_dotheroes_fps_vblank1 = 0;
int g_dotheroes_fps_novblank = 0;
}

#include <stdio.h>
#include <stdlib.h>

/* Before the guest starts. */
extern "C" void dotheroes_configure_patches(void)
{
    const char* e = getenv("DOTHEROES_FPS");
    const int fps = e && *e ? atoi(e) : 60;
    g_dotheroes_fps_nosleep  = fps != 30;
    g_dotheroes_fps_vblank1  = fps == 60;
    g_dotheroes_fps_novblank = fps != 30 && fps != 60;
    fprintf(stderr, "[port] frame cap: %s\n",
            fps == 30 ? "30 fps (original)" : fps == 60 ? "60 fps (vblank)" : "none");
}
