// The movies decoded by FFmpeg (docs/FFMPEG_MIGRATION_PLAN.md section 3, M8.2): one movie file, as the player's disc
// holds it, demuxed and decoded on a thread of its own.
//
// The thread reads the file through an I/O context of its own (the file as it is: nothing converted, nothing
// cached to disk), decodes the sound as its packets come (each packet cut to the samples it declares: FFmpeg
// decodes a packet's last transform block whole) into a ring the sound's stream pulls from (al_stream::PcmSource,
// the music's streamer), queues the compressed video packets, and decodes the video a few frames ahead of the one
// asked for. Every frame is decoded, in order: a frame shown late is skipped by not showing it, never by not
// decoding it (each depends on the one before). A movie can start again from its first frame (a looping movie).
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "al_stream.h"
#include "ff_api.h"

namespace movie {

struct Info {
    int width = 0, height = 0, frames = 0;
    int rate_num = 0, rate_den = 0;           // frames a second: rate_num / rate_den (the header's)
    int sound_rate = 0, sound_channels = 0;   // 0: silent
};

struct Frame {
    int number = 0;                // 1..frames
    int width = 0, height = 0;     // the luma plane's; each chroma plane half that each way
    std::vector<uint8_t> y, u, v;  // tightly packed (pitch: width, width / 2); u = Cb, v = Cr
};

struct Stats {
    uint64_t frames_decoded = 0, decode_us_total = 0, decode_us_max = 0;
    uint64_t sound_frames = 0;          // sound frames decoded (cut to the declared ones)
    uint64_t sound_cut = 0;             // frames FFmpeg decoded beyond a packet's declared samples
    uint64_t waits = 0, wait_us_max = 0;   // frame() calls that waited for the decoder, the longest
    uint64_t read_bytes = 0, restarts = 0;
    int decode_errors = 0;
};

struct Shared;

class Player {
public:
    ~Player();
    // the file at host_path; null with *err set when FFmpeg cannot open or decode it
    static std::unique_ptr<Player> open(const ff_api::Api* api, const std::string& host_path, std::string* err);
    const Info& info() const { return info_; }
    // frame n (1..frames) once decoded, waiting up to timeout_ms for the decoder; the frames before n are dropped
    // (decoded, not needed again). null: n is past the end, the decoder failed, or the wait timed out
    std::shared_ptr<const Frame> frame(int n, int timeout_ms);
    // start again from the first frame (and the sound from its start)
    void restart();
    std::shared_ptr<al_stream::PcmSource> sound() const;   // null: a silent movie
    Stats stats() const;
    void close();   // the thread ended, FFmpeg's objects freed (the destructor does it too)

private:
    Player() = default;
    Info info_;
    std::shared_ptr<Shared> s_;
    std::thread thread_;
};

}  // namespace movie
