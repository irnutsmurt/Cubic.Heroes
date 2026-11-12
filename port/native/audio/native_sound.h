// The native sound manager's interface for the game's glue (port/audio_hooks.cpp): plain values only, no
// OpenAL types, so the glue compiles on the player's machine without OpenAL Soft's headers. Each function
// answers one of the game's sound-manager methods (docs/OPENAL_MIGRATION_PLAN.md section 1) as rel27 answered
// it; the implementation (sound_manager.cpp) is this project's own, on OpenAL Soft. Positions and vectors are in
// the game's frame (FMOD's default left-handed one: +x right, +y up, +z forward); the implementation converts.
// Thread safety: every function may be called from any thread (one lock inside).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nsm_log_fn)(const char* line);

struct nsm_options {
    const char* sound_dir;      // host path of the folder holding sound.lua.eu and the .wav files (UTF-8)
    nsm_log_fn log;             // diagnostics ("[audio] ..."), may be null
    int voices;                 // effect voices (0: 32, FMOD's default)
    int loopback;               // tests: no device; each update renders its dt of audio (silently)
    const char* reverb;         // tests: the reverb's parameters changed, "name=value,..." (gain, gain_hf, decay,
                                // hf_ratio, reflections, reflections_delay, late, late_delay, diffusion, density);
                                // null: the game's
};

// The manager's life. init: the library, the device, the reverb, the music streamer, the effect voices and the
// sound list (true when audio runs; false: no library, no device or no list -- every other call then answers as
// a manager with nothing loaded would). load_sounds: the effects (ids 42-322) resident, as the game's
// loadSounds. shutdown: everything released.
int nsm_init(const struct nsm_options* o);
void nsm_load_sounds(void);
void nsm_shutdown(void);
int nsm_active(void);

// The game's own state the manager reads, set by the glue before each call: the options' music and effect
// levels (the game's integers, 3 by default), the system menu's pause, the game's exit request.
void nsm_set_game_state(int bgm_level, int se_level, int system_paused, int exit_requested);

// The game's methods (sound_manager.cpp describes each one's behaviour).
uint32_t nsm_play_se(int id, int priority, const float* pos /* null: none */);
uint32_t nsm_play_se_fade_in(int id, int priority, float seconds);
int nsm_is_playing(int id);
int nsm_stop_se(int id);
int nsm_stop_channel(uint32_t handle);
int nsm_fade_out_se(int id, float seconds);
int nsm_fade_out_loop_ses(float seconds);
int nsm_pause_by_id(int id, int paused);
void nsm_duck_se(float seconds);
void nsm_unduck_se(float seconds);
void nsm_play_bgm(int id, int priority, int loop);
int nsm_fade_out_bgm(float seconds);
int nsm_is_bgm_active(void);
int nsm_current_bgm_id(void);
void nsm_duck_bgm(float seconds, float level);
void nsm_unduck_bgm(float seconds);
void nsm_set_bgm_master(float master);
void nsm_set_sound_volume(int id, float volume);
void nsm_apply_volume_options(void);
void nsm_set_listener(const float* pos, const float* velocity);
void nsm_set_reverb(int on);
int nsm_is_loaded(int id);
void nsm_update(float dt);

// diagnostics: counters for tests and the shutdown report
struct nsm_stats {
    uint64_t se_plays, bgm_plays, steals, virtual_starts, stream_underruns, queue_overflows, device_losses,
        device_reopens;
    int peak_voices, peak_records, live_sources, live_buffers;
    // the effect voices: virtual channels given a freed voice, a voice taken by a more important virtual channel,
    // a channel stopped for a new one at the channel limit, a play refused there, effects restarted after a
    // device reopen; the music streams restarted after one; reopens that failed (retried)
    uint64_t promotions, swaps, channel_steals, refused, effect_restarts, stream_restarts, reopen_failures;
    int peak_channels, live_channels, live_records;   // effect channels and sound-manager records now
};
void nsm_get_stats(struct nsm_stats* out);
// tests: the gain the volume model last gave a channel (what rel27 gave FMOD's setVolume): an effect's handle,
// or a music channel's (nsm_debug_last_bgm_channel); 0 when it is not a live channel
int nsm_debug_channel_gain(uint32_t handle, float* gain);
uint32_t nsm_debug_last_bgm_channel(void);   // the music channel the last update started, 0 if none
// tests: a channel as the voices see it (an effect's handle, or a music channel's): on a voice (or, music,
// playing), paused, the gain last given, where it is in its file (seconds), its audibility, its record's state
// (0: none), its priority; 0 when it is not a live channel
struct nsm_channel_debug {
    int exists, music, real, paused, state, priority;
    float gain, audibility;
    double offset;
};
int nsm_debug_channel(uint32_t handle, struct nsm_channel_debug* out);
// tests: each block a loopback manager renders (stereo float frames) handed to fn
typedef void (*nsm_capture_fn)(const float* stereo, int frames, void* user);
void nsm_debug_capture(nsm_capture_fn fn, void* user);
// tests: the output device lost now (every source stopped, as a disconnect does), then reopened by the audio
// thread -- the first failed_reopens attempts failing (each retried a second later), each attempt taking
// reopen_ms (a slow output)
void nsm_debug_device_loss(int failed_reopens, int reopen_ms);

#ifdef __cplusplus
}
#endif
