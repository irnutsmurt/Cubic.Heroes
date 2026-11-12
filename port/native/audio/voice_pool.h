// The effect voices of the native sound manager (docs/OPENAL_MIGRATION_PLAN.md section 3, "Voices"): a fixed
// set of AL sources (FMOD's default of 32 real voices) shared by every effect, and channels -- what the game
// holds a handle to -- each either on a voice (real) or waiting for one (virtual: playing silently, its time
// counted, put on a voice where it would be when one is free).
//
// Choosing a voice when all are busy: the new channel takes the voice of the least important playing channel
// -- the highest priority number (0 the most important), then the quietest -- if it is at least as important;
// otherwise it starts virtual. A channel that loses its voice goes virtual.
// Handles: non-zero, the slot and a generation, so a handle kept after its channel ended is recognised and
// ignored (the game keeps some to stop them later).
// One thread (the sound manager's caller, under its lock) uses a pool.
#pragma once
#include <cstdint>
#include <vector>

#include "al_device.h"

namespace voice_pool {

using Handle = uint32_t;   // 0: none

struct ChannelInfo {
    int sound = -1;             // the sound id
    int priority = 128;
    bool looping = false;       // the buffer looped (its loop points, or the whole buffer)
    double length = 0.0;        // seconds of the whole buffer
    double loop_start = 0.0, loop_end = 0.0;   // seconds; loop_end 0: the buffer's end
    float gain = 1.0f;          // the AL gain wanted (the sound manager's volume model)
    bool positional = false;    // 3D: at `position`, attenuated; else at the listener, unattenuated
    bool direct = false;        // a stereo buffer's channels straight to the output (AL_SOFT_direct_channels)
    float position[3] = {0, 0, 0};   // OpenAL's frame
    float ref_distance = 1.0f, max_distance = 10000.0f;
};

class Pool {
public:
    bool create(al_device::Device& dev, int voices, int max_channels, const al_device::Reverb* reverb);
    void destroy();
    int voices() const { return (int)voices_.size(); }

    Handle play(unsigned buffer, const ChannelInfo& info);   // 0: no channel record free (a hard limit)
    void stop(Handle h);
    void pause(Handle h, bool paused);
    void set_gain(Handle h, float gain);
    void set_position(Handle h, const float pos[3]);
    bool valid(Handle h) const;                 // the channel exists (playing, paused or virtual)
    // still sounding as FMOD's isPlaying answers it: paused counts; a virtual channel until its time runs out; a
    // real one until its source stops (while the device is lost: as before the loss)
    bool playing(Handle h) const;
    bool real(Handle h) const;
    bool paused(Handle h) const;
    const ChannelInfo* info(Handle h) const;
    // the audio state, from the sound manager's update: ended channels freed (their handles go stale), virtual
    // channels' time advanced and given free voices. dt: seconds since the last call.
    void update(double dt);
    // after a device loss stopped the sources: every real, unpaused channel started again where it was
    void restart_after_loss();
    void stop_all();

    template <class F> void for_each(F f) const   // f(handle, info, real, paused)
    {
        for (size_t k = 0; k < channels_.size(); k++)
            if (channels_[k].used) f(handle(k), channels_[k].info, channels_[k].voice >= 0, channels_[k].paused);
    }
    struct Stats {
        uint64_t plays = 0, steals = 0, virtual_starts = 0, promotions = 0, refused = 0;
        int peak_real = 0, peak_channels = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    struct Channel {
        uint32_t gen = 0;
        bool used = false, paused = false;
        int voice = -1;             // index in voices_, -1: virtual
        unsigned buffer = 0;
        ChannelInfo info;
        double elapsed = 0.0;       // seconds played, kept for virtual channels
    };
    struct Voice {
        unsigned source = 0;
        int channel = -1;           // the channel on it, -1: free
    };
    Channel* get(Handle h);
    const Channel* get(Handle h) const;
    Handle handle(size_t slot) const { return (channels_[slot].gen << 12) | (uint32_t)(slot + 1); }   // bit 31 clear
    int free_voice() const;
    int victim(const ChannelInfo& want) const;   // the voice to take, -1: none
    void apply(unsigned source, const ChannelInfo& info);
    void attach(size_t slot, int voice);          // plays the channel on the voice from its elapsed time
    void detach(size_t slot);                     // the channel loses its voice (stays, virtual)
    double offset_of(const Channel& c) const;     // where in the buffer `elapsed` is (looping folded)
    void free_channel(size_t slot);

    al_device::Device* dev_ = nullptr;
    const al_device::Reverb* reverb_ = nullptr;
    std::vector<Voice> voices_;
    std::vector<Channel> channels_;
    Stats stats_;
};

}  // namespace voice_pool
