// Music streams (al_stream.h).
#include "al_stream.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>

namespace al_stream {

namespace {
// The streamer's thread named, for a per-thread CPU table (tools/thread_cpu.py): SetThreadDescription, Windows 10
// 1607 and later, looked up at run time (elsewhere nothing).
void name_this_thread()
{
#ifdef _WIN32
    typedef HRESULT(WINAPI * set_fn)(HANDLE, PCWSTR);
    static const set_fn set = (set_fn)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription");
    if (set) set(GetCurrentThread(), L"audio stream");
#endif
}
}  // namespace

const char* state_name(State s)
{
    switch (s) {
    case State::free: return "free";
    case State::opening: return "opening";
    case State::playing: return "playing";
    case State::paused: return "paused";
    case State::stopped: return "stopped";
    case State::finished: return "finished";
    case State::failed: return "failed";
    }
    return "?";
}

Streamer::Streamer(al_device::Device& dev, const Config& config)
    : dev_(dev), cfg_(config), streams_(config.max_streams), slots_(new Slot[config.max_streams]),
      status_(config.max_streams)
{
    if (!cfg_.manual) thread_ = std::thread([this] { run(); });
}

Streamer::~Streamer() { shutdown(); }

double Streamer::now() const
{
    if (cfg_.clock) return cfg_.clock(cfg_.clock_user);
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- the game's thread ----

bool Streamer::push(Command&& c)
{
    {
        std::lock_guard<std::mutex> lk(q_mutex_);
        if (shut_) return false;
        if (queue_.size() >= cfg_.queue_capacity) {
            const uint64_t n = ++overflows_;
            if (n == 1 || n % 100 == 0) dev_.log("stream commands: queue full (%llu refused so far)", (unsigned long long)n);
            return false;
        }
        queue_.push_back(std::move(c));
        submitted_++;
    }
    q_cv_.notify_one();
    return true;
}

Handle Streamer::open(const std::string& path, Loop loop, float gain, bool play_now, bool centre_mono,
                      float centre_level)
{
    for (unsigned k = 0; k < cfg_.max_streams; k++) {
        bool expected = false;
        if (!slots_[k].busy.compare_exchange_strong(expected, true)) continue;
        const uint32_t gen = (slots_[k].gen.fetch_add(1) + 1) & 0xFFFFFF;
        const Handle h = make_handle(k, gen);
        Command c{Command::open, h, loop, play_now, gain, centre_level, path, centre_mono};
        if (!push(std::move(c))) {
            slots_[k].busy.store(false);
            return kNone;
        }
        return h;
    }
    dev_.log("stream open refused: all %u streams in use (%s)", cfg_.max_streams, path.c_str());
    return kNone;
}

void Streamer::debug_simulate_disconnect()
{
    push(Command{Command::debug_stop_all, kNone, Loop::once, false, 0, 0, {}});
}

void Streamer::play(Handle h) { push(Command{Command::play, h, Loop::once, false, 0, 0, {}}); }
void Streamer::pause(Handle h, bool p) { push(Command{p ? Command::pause : Command::unpause, h, Loop::once, false, 0, 0, {}}); }
void Streamer::stop(Handle h) { push(Command{Command::stop, h, Loop::once, false, 0, 0, {}}); }
void Streamer::halt(Handle h) { push(Command{Command::halt, h, Loop::once, false, 0, 0, {}}); }
void Streamer::set_gain(Handle h, float g) { push(Command{Command::gain, h, Loop::once, false, g, 0, {}}); }
void Streamer::fade(Handle h, float to, float seconds, bool stop_at_end)
{
    push(Command{Command::fade, h, Loop::once, stop_at_end, to, seconds, {}});
}

Status Streamer::status(Handle h) const
{
    const unsigned slot = (h & 0xFF) - 1;
    if (h == kNone || slot >= cfg_.max_streams) return Status{};
    std::lock_guard<std::mutex> lk(status_mutex_);
    const Published& p = status_[slot];
    if (p.gen != (h >> 8)) {
        // not applied yet (opening) or gone
        Status s;
        s.state = slots_[slot].busy.load() && (slots_[slot].gen.load() & 0xFFFFFF) == (h >> 8) ? State::opening : State::free;
        return s;
    }
    return p.st;
}

Totals Streamer::totals() const
{
    std::lock_guard<std::mutex> lk(status_mutex_);
    Totals t = published_totals_;
    t.queue_overflows = overflows_.load();
    return t;
}

void Streamer::sync()
{
    if (cfg_.manual) {
        service_once();
        return;
    }
    std::unique_lock<std::mutex> lk(q_mutex_);
    const uint64_t target = submitted_;
    q_cv_.notify_one();
    applied_cv_.wait(lk, [&] { return applied_ >= target || !thread_.joinable(); });
}

void Streamer::shutdown()
{
    {
        std::lock_guard<std::mutex> lk(q_mutex_);
        if (shut_) return;
        shut_ = true;
    }
    if (thread_.joinable()) {
        quit_ = true;
        q_cv_.notify_all();
        thread_.join();
    }
    // the thread has ended: release what is left from here
    for (Stream& s : streams_)
        if (s.state != State::free) release(s, State::stopped);
    publish();
}

// ---- the audio thread ----

Streamer::Stream* Streamer::find(Handle h)
{
    const unsigned slot = (h & 0xFF) - 1;
    if (h == kNone || slot >= cfg_.max_streams) return nullptr;
    Stream& s = streams_[slot];
    if (s.state == State::free || s.gen != (h >> 8)) return nullptr;
    return &s;
}

void Streamer::run()
{
    name_this_thread();
    while (!quit_) {
        service_once();
        std::unique_lock<std::mutex> lk(q_mutex_);
        q_cv_.wait_for(lk, std::chrono::milliseconds(cfg_.period_ms), [this] { return quit_.load() || !queue_.empty(); });
    }
}

void Streamer::service_once()
{
    const auto t0 = std::chrono::steady_clock::now();
    std::deque<Command> work;
    {
        std::lock_guard<std::mutex> lk(q_mutex_);
        work.swap(queue_);
    }
    for (Command& c : work) apply(c);
    if (!work.empty()) {
        std::lock_guard<std::mutex> lk(q_mutex_);
        applied_ += work.size();
        totals_.commands += work.size();
    }
    applied_cv_.notify_all();
    // the device: a loss or a new default output is recovered here; the streams that were playing restart
    const uint32_t g = dev_.poll();
    if (g != device_gen_) {
        device_gen_ = g;
        for (Stream& s : streams_)
            if (s.state == State::playing && s.source) {
                ALint st = 0;
                dev_.al().alGetSourcei(s.source, AL_SOURCE_STATE, &st);
                if (st != AL_PLAYING) {   // from where it stopped (its queue was kept)
                    dev_.al().alSourcei(s.source, AL_SAMPLE_OFFSET, s.last_offset);
                    dev_.al().alSourcePlay(s.source);
                    totals_.restarts_after_reopen++;
                }
            }
    }
    for (Stream& s : streams_)
        if (s.state == State::playing || s.state == State::paused) service(s);
    publish();
    const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0).count();
    totals_.services++;
    totals_.service_us_max = std::max(totals_.service_us_max, us);
}

void Streamer::set_al_gain(Stream& s)
{
    if (!s.source) return;
    const float g = std::clamp(s.gain, 0.0f, 1.0f);
    dev_.al().alSourcef(s.source, AL_GAIN, (cfg_.gain_curve ? cfg_.gain_curve(g) : g) * s.level);
}

void Streamer::apply(Command& c)
{
    if (c.kind == Command::open) {
        const unsigned slot = (c.h & 0xFF) - 1;
        Stream& s = streams_[slot];
        if (s.state != State::free) release(s, State::stopped);   // cannot happen: the slot was free
        s = Stream{};
        s.gen = c.h >> 8;
        s.state = State::failed;
        s.loop = c.loop;
        s.gain = c.a;
        std::string err;
        s.f = wav_pcm::open_utf8(c.path);
        if (!s.f || !wav_pcm::parse_file(s.f, &s.info, &err)) {
            dev_.log("stream: %s: %s", c.path.c_str(), s.f ? err.c_str() : "cannot open");
            totals_.open_failures++;
            if (s.f) fclose(s.f);
            s.f = nullptr;
            return;   // stays failed until stopped (its handle reports it)
        }
        s.centre = c.centre && s.info.channels == 1;
        s.level = s.centre ? c.b : 1.0f;
        s.region_start = 0;
        s.region_end = s.info.frames;
        if (s.loop == Loop::loop_points_only) s.loop = s.info.has_loop ? Loop::file_loop_points : Loop::once;
        if (s.loop == Loop::file_loop_points && s.info.has_loop) {
            s.region_start = s.info.loop_start;
            s.region_end = s.info.loop_end;
        }
        s.source = dev_.new_source();
        for (unsigned k = 0; k < cfg_.buffers; k++) {
            const unsigned b = dev_.new_buffer();
            if (b) s.buffers.push_back(b);
        }
        if (s.source) totals_.live++;
        if (!s.source || s.buffers.size() != cfg_.buffers) {
            dev_.log("stream: %s: no AL source or buffers", c.path.c_str());
            totals_.open_failures++;
            free_objects(s);   // stays failed, holding nothing, until stopped
            return;
        }
        const al_api::Api& al = dev_.al();
        // music plays at the listener: no distance, no 3D panning, its channels as they are
        al.alSourcei(s.source, AL_SOURCE_RELATIVE, AL_TRUE);
        al.alSource3f(s.source, AL_POSITION, 0.f, 0.f, 0.f);
        al.alSourcef(s.source, AL_ROLLOFF_FACTOR, 0.f);
        // a stereo file's channels straight to the output's left and right, as the game's 2D music played (a mono
        // file plays centred: OpenAL Soft pans mono even then)
        if (dev_.caps().direct_channels) al.alSourcei(s.source, AL_DIRECT_CHANNELS_SOFT, AL_TRUE);
        if (cfg_.reverb) cfg_.reverb->attach(s.source);
        set_al_gain(s);
        totals_.opened++;
        s.state = State::stopped;
        if (c.flag) {
            Command p{Command::play, c.h, Loop::once, false, 0, 0, {}};
            apply(p);
        }
        return;
    }
    if (c.kind == Command::debug_stop_all) {
        dev_.simulate_loss();   // first, as OpenAL Soft marks a device disconnected before it stops the sources;
        for (Stream& s : streams_)   // seen by the poll that follows the commands
            if (s.source) dev_.al().alSourceStop(s.source);
        return;
    }
    Stream* sp = find(c.h);
    if (!sp) {
        totals_.stale_commands++;
        return;
    }
    Stream& s = *sp;
    const al_api::Api& al = dev_.al();
    switch (c.kind) {
    case Command::play:
        if (s.state == State::failed) break;
        if (s.state == State::paused) {
            al.alSourcePlay(s.source);
            s.state = State::playing;
            break;
        }
        al.alSourceStop(s.source);
        al.alSourcei(s.source, AL_BUFFER, 0);
        s.pos = 0;
        s.eof = false;
        s.frames_unqueued = 0;
        s.queued_frames.clear();
        for (unsigned b : s.buffers)
            if (!fill(s, b)) break;
        al.alSourcePlay(s.source);
        s.state = State::playing;
        break;
    case Command::pause:
        if (s.state == State::playing) {
            al.alSourcePause(s.source);
            al.alGetSourcei(s.source, AL_SAMPLE_OFFSET, &s.paused_offset);
            s.state = State::paused;
        }
        break;
    case Command::unpause:
        if (s.state == State::paused) {
            // a device lost while paused stopped the source and rewound its queue: from where it was paused
            ALint st = 0;
            al.alGetSourcei(s.source, AL_SOURCE_STATE, &st);
            if (st == AL_STOPPED) al.alSourcei(s.source, AL_SAMPLE_OFFSET, s.paused_offset);
            al.alSourcePlay(s.source);
            s.state = State::playing;
        }
        break;
    case Command::stop:
        release(s, State::stopped);
        break;
    case Command::halt:
        if (s.state == State::failed) break;
        al.alSourceStop(s.source);
        al.alSourcei(s.source, AL_BUFFER, 0);
        s.queued_frames.clear();
        s.frames_unqueued = 0;
        s.pos = 0;
        s.eof = false;
        s.fading = false;
        s.state = State::stopped;
        break;
    case Command::gain:
        s.fading = false;
        s.gain = c.a;
        set_al_gain(s);
        break;
    case Command::fade:
        s.fading = true;
        s.fade_from = s.gain;
        s.fade_to = c.a;
        s.fade_seconds = std::max(0.0f, c.b);
        s.fade_stop = c.flag;
        s.fade_t0 = now();
        break;
    default:
        break;
    }
}

bool Streamer::fill(Stream& s, unsigned buffer)
{
    const unsigned ch = s.info.channels;
    const size_t want_frames = std::max<size_t>(1, (size_t)s.info.rate * cfg_.buffer_ms / 1000);
    scratch_.resize(want_frames * ch);
    int16_t* dst = scratch_.data();
    size_t have = 0;
    while (have < want_frames) {
        uint64_t end = s.loop == Loop::once ? s.info.frames : s.region_end;
        if (s.pos >= end) {
            if (s.loop == Loop::once) {
                s.eof = true;
                break;
            }
            s.pos = s.region_start;
            s.loops++;
            totals_.loops++;
            end = s.region_end;
            if (end <= s.pos) {   // an empty region: nothing to loop
                s.eof = true;
                break;
            }
        }
        const size_t want = (size_t)std::min<uint64_t>(want_frames - have, end - s.pos);
        const size_t got = wav_pcm::read_frames(s.f, s.info, s.pos, want, &dst[have * ch]);
        have += got;
        s.pos += got;
        if (got < want) {
            totals_.read_errors++;
            s.eof = true;
            break;
        }
    }
    if (!have) return false;
    const al_api::Api& al = dev_.al();
    if (s.centre) {   // mono doubled into stereo
        s.mono.assign(scratch_.begin(), scratch_.begin() + (ptrdiff_t)have);
        scratch_.resize(have * 2);
        for (size_t i = 0; i < have; i++) scratch_[2 * i] = scratch_[2 * i + 1] = s.mono[i];
    }
    const bool stereo = ch == 2 || s.centre;
    al.alBufferData(buffer, stereo ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16, scratch_.data(),
                    (ALsizei)(have * (stereo ? 2 : 1) * sizeof(int16_t)), (ALsizei)s.info.rate);
    al.alSourceQueueBuffers(s.source, 1, &buffer);
    s.queued_frames.push_back(have);
    return true;
}

void Streamer::service(Stream& s)
{
    const al_api::Api& al = dev_.al();
    // the fade
    if (s.fading) {
        const float t = (float)(now() - s.fade_t0);
        const float k = s.fade_seconds > 0 ? std::min(1.0f, t / s.fade_seconds) : 1.0f;
        s.gain = s.fade_from + (s.fade_to - s.fade_from) * k;
        set_al_gain(s);
        if (k >= 1.0f) {
            s.fading = false;
            if (s.fade_stop) {
                release(s, State::stopped);
                return;
            }
        }
    }
    if (s.state != State::playing) return;
    ALint processed = 0, queued = 0, st = 0;
    al.alGetSourcei(s.source, AL_SOURCE_STATE, &st);
    // a lost device stopped it: its queue is left as it was, for the restart from where it stopped
    if (st == AL_STOPPED && !dev_.query_connected()) return;
    al.alGetSourcei(s.source, AL_BUFFERS_PROCESSED, &processed);
    al.alGetSourcei(s.source, AL_BUFFERS_QUEUED, &queued);
    if (processed > 0 && processed == queued && !s.eof && dev_.query_connected()) {
        s.near_underruns++;
        totals_.near_underruns++;
    }
    while (processed-- > 0) {
        unsigned b = 0;
        al.alSourceUnqueueBuffers(s.source, 1, &b);
        if (!s.queued_frames.empty()) {
            s.frames_unqueued += s.queued_frames.front();
            s.queued_frames.erase(s.queued_frames.begin());
        }
        if (!s.eof) fill(s, b);
    }
    al.alGetSourcei(s.source, AL_SOURCE_STATE, &st);
    al.alGetSourcei(s.source, AL_BUFFERS_QUEUED, &queued);
    if (st != AL_PLAYING && st != AL_PAUSED) {
        if (queued > 0 && dev_.query_connected()) {   // ran dry with more to play (a lost device is not an underrun)
            s.underruns++;
            totals_.underruns++;
            dev_.log("stream underrun (%llu)", (unsigned long long)totals_.underruns);
            al.alSourcePlay(s.source);
        } else if (queued == 0 && s.eof) {
            s.state = State::finished;   // a play-once stream at its end: keeps its handle until stopped
        }
    }
}

void Streamer::free_objects(Stream& s)
{
    if (s.source) {
        dev_.delete_source(s.source);   // stops it and takes its buffers back
        totals_.live--;
    }
    for (unsigned& b : s.buffers) dev_.delete_buffer(b);
    s.buffers.clear();
    s.queued_frames.clear();
    if (s.f) fclose(s.f);
    s.f = nullptr;
}

void Streamer::release(Stream& s, State end_state)
{
    const unsigned slot = (unsigned)(&s - streams_.data());
    free_objects(s);
    s.state = State::free;
    s.gen = 0;
    {
        std::lock_guard<std::mutex> lk(status_mutex_);
        status_[slot] = Published{};
    }
    slots_[slot].busy.store(false);
    (void)end_state;
}

void Streamer::publish()
{
    std::lock_guard<std::mutex> lk(status_mutex_);
    for (size_t k = 0; k < streams_.size(); k++) {
        Stream& s = streams_[k];
        Published& p = status_[k];
        if (s.state == State::free) {
            p = Published{};
            continue;
        }
        p.gen = s.gen;
        p.st.state = s.state;
        p.st.gain = s.gain;
        p.st.underruns = s.underruns;
        p.st.near_underruns = s.near_underruns;
        p.st.loops = s.loops;
        double frames = (double)s.frames_unqueued;
        if (s.source && s.info.rate) {
            ALint off = 0, st = 0;
            dev_.al().alGetSourcei(s.source, AL_SAMPLE_OFFSET, &off);
            dev_.al().alGetSourcei(s.source, AL_SOURCE_STATE, &st);
            if (st == AL_PLAYING) s.last_offset = off;   // where a restart after a loss goes on from
            else if (st == AL_STOPPED && s.state == State::playing) off = s.last_offset;   // lost: held there
            else if (st == AL_STOPPED && s.state == State::paused) off = s.paused_offset;
            frames += off;
        }
        // frames heard -> a position in the file: the intro once, then around the loop region
        const double region = (double)(s.region_end - s.region_start);
        double pos = frames;
        if (s.loop != Loop::once && frames >= (double)s.region_end && region > 0)
            pos = (double)s.region_start + std::fmod(frames - (double)s.region_end, region);
        p.st.position = s.info.rate ? pos / s.info.rate : 0.0;
    }
    published_totals_ = totals_;
}

}  // namespace al_stream
