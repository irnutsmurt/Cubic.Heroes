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
 */
extern "C" {
int g_dotheroes_skip_flash_mp3 = 1;
}
