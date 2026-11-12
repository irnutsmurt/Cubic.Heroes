// The effect voices of the native sound manager (docs/OPENAL_MIGRATION_PLAN.md section 3, "Voices"): a fixed
// set of AL sources (FMOD's default of 32 real voices) shared by every effect, and channels -- what the game
// holds a handle to -- each either on a voice (real) or waiting for one (virtual: playing silently, its time
// counted, put on a voice where it would be when one is free).
//
// Importance: the priority number first (0 the most important, 256 the least; the game gives every sound 128),
// then the audibility -- the gain the sound manager gives it times its distance attenuation from the listener
// (OpenAL's inverse distance, clamped) -- as FMOD's virtual voices ranked them.
// - A new channel when every voice is busy takes the voice of the least important real channel if it is more
//   important than that one (or as important and at least as audible); else it starts virtual. A channel that
//   loses its voice goes virtual: it keeps its time and its handle, and isPlaying still answers for it.
// - Each update: voices freed by ended channels go to the most important virtual channels; then a virtual channel
//   that has become more important than a real one (a fade-in rising, the listener moving) takes its voice --
//   with equal priorities only when clearly more audible (kSwapMargin), so two similar sounds do not trade the
//   voice back and forth.
// - Channels: at most max_channels (FMOD's System::init(100) in the game). A new channel when all are in use
//   stops the least important one (its handle goes stale) if it is at least as important; else it is refused.
// - A device loss stops every source. A channel whose source is found stopped has ended if it is a one-shot at
//   its length (its time within kEndSlack of it); stopped earlier, or a loop, it is taken as ended only once the
//   audio thread's next full poll still finds the device connected and not reopened (al_device's poll_count,
//   connected_now, generation) -- until then it answers by its time, so a loop stopped by a disconnect keeps its
//   record and handle. No ALC call on the game's thread: a reopen holds OpenAL Soft's device lock. After a
//   reopen each real channel is restarted where its time says it is, unless it is a one-shot whose time ran out
//   meanwhile.
// Music is not here (al_stream): effects never take its voice. In rel27 FMOD's 32 voices were shared with the
// music's one or two streams, at the same priority (128).
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
    float level = 1.0f;         // a fixed factor on it (a centred mono sound played as stereo: its per-side level)
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
    int channels_in_use() const;

    Handle play(unsigned buffer, const ChannelInfo& info);   // 0: every channel in use by more important ones
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
    // the listener (OpenAL's frame), for the audibility of positional channels
    void set_listener(const float pos[3]);
    void stop_all();
    // tests: every voice's source stopped, as a device loss stops them (the caller marks the device lost first)
    void debug_stop_sources();
    // tests: where a channel is in its buffer (seconds; a real one: its source's offset; a virtual one: its time
    // folded into its loop); false for a stale handle
    bool debug_offset(Handle h, double* seconds) const;
    double audibility(const ChannelInfo& info) const;

    template <class F> void for_each(F f) const   // f(handle, info, real, paused)
    {
        for (size_t k = 0; k < channels_.size(); k++)
            if (channels_[k].used) f(handle(k), channels_[k].info, channels_[k].voice >= 0, channels_[k].paused);
    }
    struct Stats {
        uint64_t plays = 0, steals = 0, virtual_starts = 0, promotions = 0, swaps = 0, channel_steals = 0,
                 refused = 0, restarts = 0;
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
        mutable uint64_t stopped_poll = 0;   // found stopped early: the poll count that settles it (0: not)
    };
    enum class Stop { sounding, ended, unsure };
    struct Voice {
        unsigned source = 0;
        int channel = -1;           // the channel on it, -1: free
    };
    Channel* get(Handle h);
    const Channel* get(Handle h) const;
    Handle handle(size_t slot) const { return (channels_[slot].gen << 12) | (uint32_t)(slot + 1); }   // bit 31 clear
    int free_voice() const;
    int victim(const ChannelInfo& want) const;   // the voice to take, -1: none
    int least_important_real() const;            // a voice, -1: none in use
    int most_important_virtual() const;          // a channel slot, -1: none
    // a ranks before b: more important (lower priority number), then more audible
    bool before(const ChannelInfo& a, const ChannelInfo& b) const;
    void restart_after_loss();
    Stop stop_state(const Channel& c) const;     // a real channel's source: sounding, ended, or not known yet
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
    float listener_[3] = {0, 0, 0};
    uint32_t generation_ = 0;                     // the device's, as of the last restart
};

}  // namespace voice_pool
