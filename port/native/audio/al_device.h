// The output device on OpenAL Soft (docs/OPENAL_MIGRATION_PLAN.md section 3): opening, its health, recovery,
// shutdown; the accounting of every AL object this project makes; the sound files as AL buffers; the EFX reverb.
//
// - Opening: the default output (or a named one), no HRTF (the game's sound never had it; no HRTF data ships),
//   one auxiliary send a source (the reverb). A missing library or device is reported and the caller carries on
//   without sound.
// - Health: poll() -- from one thread, the audio thread (al_stream's) -- reads ALC_CONNECTED; a lost device, or
//   a new default output when the default was opened (ALC_SOFT_system_events), is reopened in place
//   (ALC_SOFT_reopen_device: the context, sources, buffers and effects survive), retried every
//   reopen_retry_ms until it succeeds. Sources playing at the loss were stopped by it: generation() counts the
//   reopens so their owners restart what should still play.
// - Shutdown: close() after every source, buffer, effect and slot is released; what is still live is reported
//   (a leak) and released.
// Thread safety: OpenAL Soft's calls are safe from any thread; this class's own state is atomics, and poll()
// is called from one thread.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "al_api.h"
#include "wav_pcm.h"

namespace al_device {

using LogFn = void (*)(const char* line);
void log_stderr(const char* line);

struct Options {
    std::string device_name;          // empty: the default output, followed when it changes
    int frequency = 0;                // 0: the device's own rate
    bool loopback = false;            // a loopback device (tests): rendered on request, stereo float
    int loopback_rate = 48000;
    unsigned reopen_retry_ms = 1000;
};

struct Caps {
    bool efx = false, eax_reverb = false, loop_points = false, direct_channels = false, disconnect = false,
         reopen = false, system_events = false, spatialize = false;
    int aux_sends = 0;
};

struct Stats {
    uint64_t disconnects = 0;         // ALC_CONNECTED seen false
    uint64_t default_changes = 0;     // the default output changed while it was in use
    uint64_t reopens = 0;             // successful reopens
    uint64_t reopen_failures = 0;
};

struct Live {
    int sources = 0, buffers = 0, effects = 0, slots = 0;
    bool any() const { return sources || buffers || effects || slots; }
};

class Device {
public:
    Device() = default;
    ~Device() { close(); }
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    bool open(al_api::Api* api, const Options& options, LogFn log_fn, std::string* error);
    void close();
    bool is_open() const { return ctx_ != nullptr; }

    uint32_t poll();                  // the audio thread: health and recovery; returns generation()
    bool connected() const { return connected_.load(); }
    uint32_t generation() const { return generation_.load(); }
    const Caps& caps() const { return caps_; }
    Stats stats() const;
    const std::string& name() const { return name_; }
    int frequency() const { return frequency_; }
    const al_api::Api& al() const { return *api_; }
    void* alc_device() const { return dev_; }
    void render(float* out, int frames);   // a loopback device: the next `frames` stereo frames
    void log(const char* fmt, ...) const;

    // AL objects, counted (Live): every one this project makes goes through these. 0 on failure.
    unsigned new_source();
    void delete_source(unsigned& id);
    unsigned new_buffer();
    void delete_buffer(unsigned& id);
    unsigned new_effect();
    void delete_effect(unsigned& id);
    unsigned new_slot();
    void delete_slot(unsigned& id);
    Live live() const;

    // forced from outside (tests): treat the device as lost now
    void simulate_loss() { lost_.store(true); }

private:
    static void ALC_APIENTRY event_callback(ALCenum type, ALCenum device_type, ALCdevice* device, ALCsizei length,
                                            const ALCchar* message, void* user) noexcept;
    bool reopen();

    al_api::Api* api_ = nullptr;
    Options options_;
    LogFn log_ = nullptr;
    void* dev_ = nullptr;
    void* ctx_ = nullptr;
    Caps caps_;
    std::string name_;
    int frequency_ = 0;
    std::atomic<bool> connected_{false}, lost_{false}, default_changed_{false};
    std::atomic<uint32_t> generation_{0};
    std::atomic<uint64_t> disconnects_{0}, default_changes_{0}, reopens_{0}, reopen_failures_{0};
    std::atomic<int> sources_{0}, buffers_{0}, effects_{0}, slots_{0};
    std::vector<int> attrs_;
    uint64_t next_retry_ms_ = 0;
    bool waiting_ = false;            // lost, not reopened yet
};

// A sound file resident as an AL buffer, its `smpl` loop set as the buffer's loop points (AL_SOFT_loop_points)
// so that a looping source plays the intro once, then the loop region.
// centre_stereo (mono files): a second buffer of the same sound as stereo, both channels the file's samples, for
// playing it unpanned at the centre straight to the outputs (AL_SOFT_direct_channels) at a fixed per-side level
// -- OpenAL's own panning of a mono source at the centre is 1.5 dB down on its sides, and depends on the speakers.
struct Sample {
    unsigned buffer = 0;
    unsigned centre_stereo = 0;
    wav_pcm::Info info;
    bool loop_points_set = false;
    double seconds() const { return info.seconds(); }
};
bool load_sample(Device& dev, const std::string& path, Sample* out, std::string* error, bool centre_stereo = false);
void free_sample(Device& dev, Sample* s);

// The reverb in I3DL2 terms (EFX implements the model; nothing here is any other library's code).
struct ReverbParams {
    float density = 1.0f, diffusion = 1.0f, gain = 0.32f, gain_hf = 0.89f, gain_lf = 1.0f;
    float decay_time = 1.49f, decay_hf_ratio = 0.83f, decay_lf_ratio = 1.0f;
    float reflections_gain = 0.05f, reflections_delay = 0.007f;
    float late_gain = 1.26f, late_delay = 0.011f;
    float air_absorption_gain_hf = 0.994f, hf_reference = 5000.0f, lf_reference = 250.0f;
    float room_rolloff = 0.0f;
    bool decay_hf_limit = true;
};

// One reverb in one auxiliary slot: sources send to it while it is enabled (the dungeon reverb is on or off for
// the whole mix).
class Reverb {
public:
    bool create(Device& dev, const ReverbParams& p);
    void set(const ReverbParams& p);
    void enable(bool on);             // the slot's effect attached or detached
    bool enabled() const { return on_; }
    void attach(unsigned source) const;   // the source's send 0 to the slot (or to nothing when unavailable)
    void destroy();
    unsigned slot() const { return slot_; }

private:
    Device* dev_ = nullptr;
    unsigned effect_ = 0, slot_ = 0;
    bool eax_ = false, on_ = false;
};

}  // namespace al_device
