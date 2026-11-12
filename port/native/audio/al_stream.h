// Music streamed from the player's sound files into OpenAL Soft (docs/OPENAL_MIGRATION_PLAN.md section 3).
//
// One audio thread (Streamer) owns every stream: it opens the file, reads it into a queue of AL buffers as it
// plays (buffers x buffer_ms of audio ahead), applies gain fades, keeps the device healthy (al_device's poll:
// a lost device reopened, the streams restarted) and publishes each stream's status. The game's thread only
// queues commands -- a bounded queue: nothing it calls waits for a file or the device -- and reads statuses.
//
// Loops: a stream plays the file to the end of its loop region, then from the region's start (the file's
// `smpl` loop: the intro once, then the loop; else the whole file), or once to the file's end.
// Crossfades: two streams, one fading out (to 0, then stopped) while the other plays or fades in.
// Underruns: a source that ran dry while its file had more to play is restarted and counted; a service that
// found every buffer already played (the queue about to run dry) is counted as a near-underrun.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "al_device.h"
#include "wav_pcm.h"

namespace al_stream {

// file_loop_points: the `smpl` loop if the file has one, else the whole file; loop_points_only: the `smpl` loop if
// the file has one, else once
enum class Loop : uint8_t { once, whole_file, file_loop_points, loop_points_only };
enum class State : uint8_t { free, opening, playing, paused, stopped, finished, failed };
const char* state_name(State s);

struct Config {
    unsigned buffer_ms = 250;         // one AL buffer's length
    unsigned buffers = 4;             // buffers queued per stream: buffers x buffer_ms of audio ahead
    unsigned period_ms = 10;          // the audio thread's service period
    unsigned queue_capacity = 1024;   // commands waiting for the audio thread; more are refused and counted
    unsigned max_streams = 8;
    bool manual = false;              // no thread: the caller services (service_once), e.g. a loopback test
    float (*gain_curve)(float) = nullptr;   // fade value -> AL gain (null: linear)
    double (*clock)(void* user) = nullptr;  // seconds, for fades (null: the steady clock); a loopback test
    void* clock_user = nullptr;             // passes the audio rendered so far
    const al_device::Reverb* reverb = nullptr;   // each stream's send (the game's reverb covers the music too)
};

using Handle = uint32_t;              // 0: none
constexpr Handle kNone = 0;

struct Status {
    State state = State::free;
    double position = 0.0;            // seconds into the file of the frame being heard (approximate)
    float gain = 0.0f;                // the fade value now
    uint64_t underruns = 0, near_underruns = 0, loops = 0;
};

struct Totals {
    uint64_t opened = 0, open_failures = 0, underruns = 0, near_underruns = 0, read_errors = 0, loops = 0;
    uint64_t commands = 0, queue_overflows = 0, stale_commands = 0, restarts_after_reopen = 0;
    uint64_t services = 0, service_us_max = 0;   // the audio thread's work per period, its worst
    int live = 0;                     // streams holding a source
};

class Streamer {
public:
    Streamer(al_device::Device& dev, const Config& config);
    ~Streamer();
    Streamer(const Streamer&) = delete;
    Streamer& operator=(const Streamer&) = delete;

    // the game's thread: commands, applied by the audio thread in order
    Handle open(const std::string& path, Loop loop, float gain, bool play_now);   // kNone: no free stream
    void play(Handle h);
    void pause(Handle h, bool paused);
    void stop(Handle h);              // stops and releases (the handle goes stale)
    void halt(Handle h);              // stops playing, stays open: a later play() starts it again from the start
    void set_gain(Handle h, float gain);
    void fade(Handle h, float to, float seconds, bool stop_at_end);
    Status status(Handle h) const;    // State::free for a stale or unknown handle
    Totals totals() const;
    void sync();                      // waits until every queued command is applied (tests, shutdown)
    void shutdown();                  // stops every stream, releases every object, ends the thread

    void service_once();              // Config::manual: one period's work from the caller's thread
    // tests: a disconnect as the audio thread sees one -- every stream's source stopped and the device marked
    // lost, in one step of the audio thread
    void debug_simulate_disconnect();

private:
    struct Command {
        enum Kind : uint8_t { open, play, pause, unpause, stop, halt, gain, fade, debug_stop_all } kind;
        Handle h;
        Loop loop;
        bool flag;
        float a, b;
        std::string path;
    };
    struct Stream {
        uint32_t gen = 0;
        State state = State::free;
        FILE* f = nullptr;
        wav_pcm::Info info;
        Loop loop = Loop::once;
        uint64_t pos = 0, region_start = 0, region_end = 0;
        uint64_t frames_unqueued = 0;          // frames of buffers already played and taken back
        std::vector<uint64_t> queued_frames;   // frames in each queued buffer, oldest first
        unsigned source = 0;
        std::vector<unsigned> buffers;
        bool eof = false;
        float gain = 1.0f, fade_from = 0, fade_to = 0, fade_seconds = 0;
        bool fading = false, fade_stop = false;
        double fade_t0 = 0;
        uint64_t underruns = 0, near_underruns = 0, loops = 0;
    };

    bool push(Command&& c);
    double now() const;
    void run();
    void apply(Command& c);
    void service(Stream& s);
    bool fill(Stream& s, unsigned buffer);
    void free_objects(Stream& s);
    void release(Stream& s, State end_state);
    void set_al_gain(Stream& s);
    void publish();
    Stream* find(Handle h);
    static Handle make_handle(unsigned slot, uint32_t gen) { return (gen << 8) | (slot + 1); }

    al_device::Device& dev_;
    Config cfg_;
    std::vector<Stream> streams_;               // the audio thread's
    struct Slot {
        std::atomic<uint32_t> gen{0};          // advanced by the game's thread when it takes the slot
        std::atomic<bool> busy{false};         // taken by open(), freed by the audio thread at release
    };
    std::unique_ptr<Slot[]> slots_;
    mutable std::mutex q_mutex_;
    std::condition_variable q_cv_;
    std::deque<Command> queue_;
    uint64_t submitted_ = 0, applied_ = 0;
    std::condition_variable applied_cv_;
    mutable std::mutex status_mutex_;
    struct Published {
        uint32_t gen = 0;
        Status st;
    };
    std::vector<Published> status_;             // published each period
    Totals totals_;                             // the audio thread's; copied under status_mutex_
    Totals published_totals_;
    std::atomic<uint64_t> overflows_{0};
    std::vector<int16_t> scratch_;
    uint32_t device_gen_ = 0;
    std::atomic<bool> quit_{false};
    std::thread thread_;
    bool shut_ = false;
};

}  // namespace al_stream
