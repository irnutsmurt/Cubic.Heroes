// A build without OpenAL Soft's headers (tools/build_deps.py not run): the native sound manager's interface
// (native_sound.h) answers as a manager that could not start -- no sound, nothing loaded -- so the game's glue
// links and DOTHEROES_NATIVE_AUDIO=1 gives a silent game rather than a broken build.
#include "native_sound.h"

extern "C" {
int nsm_init(const struct nsm_options*) { return 0; }
void nsm_load_sounds(void) {}
void nsm_shutdown(void) {}
int nsm_active(void) { return 0; }
void nsm_set_game_state(int, int, int, int) {}
uint32_t nsm_play_se(int, int, const float*) { return 0; }
uint32_t nsm_play_se_fade_in(int, int, float) { return 0; }
int nsm_is_playing(int) { return 0; }
int nsm_stop_se(int) { return 0; }
int nsm_stop_channel(uint32_t) { return 2; }
int nsm_fade_out_se(int, float) { return 1; }
int nsm_fade_out_loop_ses(float) { return 1; }
int nsm_pause_by_id(int, int) { return 0; }
void nsm_duck_se(float) {}
void nsm_unduck_se(float) {}
void nsm_play_bgm(int, int, int) {}
int nsm_fade_out_bgm(float) { return 1; }
int nsm_is_bgm_active(void) { return 0; }
int nsm_current_bgm_id(void) { return 323; }
void nsm_duck_bgm(float, float) {}
void nsm_unduck_bgm(float) {}
void nsm_set_bgm_master(float) {}
void nsm_set_sound_volume(int, float) {}
void nsm_apply_volume_options(void) {}
void nsm_set_listener(const float*, const float*) {}
void nsm_set_reverb(int) {}
int nsm_is_loaded(int) { return 0; }
void nsm_update(float) {}
void nsm_get_stats(struct nsm_stats* out) { *out = nsm_stats{}; }
int nsm_debug_channel_gain(uint32_t, float*) { return 0; }
uint32_t nsm_debug_last_bgm_channel(void) { return 0; }
int nsm_debug_channel(uint32_t, struct nsm_channel_debug* out) { *out = nsm_channel_debug{}; return 0; }
void nsm_debug_capture(nsm_capture_fn, void*) {}
void nsm_debug_device_loss(int, int) {}
}
