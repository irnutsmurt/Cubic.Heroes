// The movies decoded by FFmpeg (movie_player.h).
#include "movie_player.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace movie {

namespace {
constexpr int kAhead = 6;                       // decoded frames kept ahead of the one asked for
constexpr size_t kPacketBytes = 24u << 20;      // compressed video read ahead (about 10 s of these movies)
constexpr double kSoundAhead = 1.5;             // seconds of sound decoded ahead of what the stream has taken
constexpr int kIoBuffer = 256 * 1024;

FILE* open_utf8(const std::string& path)
{
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? (size_t)n : 1, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    return _wfopen(w.c_str(), L"rb");
#else
    return fopen(path.c_str(), "rb");
#endif
}

int64_t seek64(FILE* f, int64_t off, int whence)
{
#ifdef _WIN32
    return _fseeki64(f, off, whence);
#else
    return fseeko(f, off, whence);
#endif
}

int64_t tell64(FILE* f)
{
#ifdef _WIN32
    return _ftelli64(f);
#else
    return ftello(f);
#endif
}
}  // namespace

struct Shared {
    const ff_api::Api* ff = nullptr;
    std::mutex mu;
    std::condition_variable cv;
    bool quit = false, failed = false, eof = false, flushed = false, restart_req = false, closed = false;
    // the file and FFmpeg's objects (the decoder thread's, and open/close's)
    FILE* f = nullptr;
    AVIOContext* io = nullptr;
    AVFormatContext* fmt = nullptr;
    AVCodecContext* vdec = nullptr;
    AVCodecContext* adec = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frm = nullptr;
    int vs = -1, as = -1;
    // video: compressed packets read ahead, decoded frames from the one asked for
    std::deque<AVPacket*> vpkts;
    size_t vpkt_bytes = 0;
    std::deque<std::shared_ptr<Frame>> frames;
    int next_number = 1;   // the number the next decoded frame gets
    int wanted = 1;        // the frame last asked for: the ones before it are not needed
    int gen = 0;           // advanced by a restart: a frame or packet of an older pass is dropped
    // sound: interleaved 16-bit frames in a ring, pulled by the stream
    unsigned channels = 0, rate = 0;
    std::vector<int16_t> pcm;
    size_t pcm_cap = 0, pcm_head = 0, pcm_count = 0;
    bool sound_ended = false;
    Stats stats;
};

namespace {

int io_read(void* opaque, uint8_t* buf, int size)
{
    Shared* s = (Shared*)opaque;
    const size_t n = fread(buf, 1, (size_t)size, s->f);
    s->stats.read_bytes += n;
    return n ? (int)n : AVERROR_EOF;
}

int64_t io_seek(void* opaque, int64_t offset, int whence)
{
    Shared* s = (Shared*)opaque;
    if (whence & AVSEEK_SIZE) {
        const int64_t here = tell64(s->f);
        seek64(s->f, 0, SEEK_END);
        const int64_t size = tell64(s->f);
        seek64(s->f, here, SEEK_SET);
        return size;
    }
    if (seek64(s->f, offset, whence & ~AVSEEK_FORCE) != 0) return -1;
    return tell64(s->f);
}

// the sound's decoded frames into the ring (s->mu held), growing it rather than dropping any
void push_pcm(Shared* s, const int16_t* in, size_t frames)
{
    const unsigned ch = s->channels;
    if (s->pcm_count + frames > s->pcm_cap) {
        std::vector<int16_t> grown((s->pcm_count + frames) * 2 * ch);
        for (size_t i = 0; i < s->pcm_count; i++)
            memcpy(&grown[i * ch], &s->pcm[((s->pcm_head + i) % s->pcm_cap) * ch], ch * sizeof(int16_t));
        s->pcm.swap(grown);
        s->pcm_cap = s->pcm.size() / ch;
        s->pcm_head = 0;
    }
    for (size_t i = 0; i < frames; i++) {
        const size_t at = (s->pcm_head + s->pcm_count + i) % s->pcm_cap;
        memcpy(&s->pcm[at * ch], &in[i * ch], ch * sizeof(int16_t));
    }
    s->pcm_count += frames;
}

// a sound packet decoded, cut to the samples it declares (its first word: the decoded size in bytes of 16-bit
// samples, all channels), converted to interleaved 16-bit
void decode_sound(Shared* s, AVPacket* p)
{
    const ff_api::Api& ff = *s->ff;
    const unsigned ch = s->channels;
    size_t declared = SIZE_MAX;
    if (p->size >= 4)
        declared = (size_t)(p->data[0] | p->data[1] << 8 | p->data[2] << 16 | (uint32_t)p->data[3] << 24) / (2 * ch);
    if (ff.avcodec_send_packet(s->adec, p) < 0) {
        s->stats.decode_errors++;
        return;
    }
    size_t given = 0;
    std::vector<int16_t> out;
    while (ff.avcodec_receive_frame(s->adec, s->frm) >= 0) {
        const int n = s->frm->nb_samples;
        const bool planar = s->frm->format == AV_SAMPLE_FMT_FLTP;
        const size_t keep = given >= declared ? 0 : std::min<size_t>((size_t)n, declared - given);
        if ((size_t)n > keep) s->stats.sound_cut += (size_t)n - keep;
        out.resize(keep * ch);
        for (size_t i = 0; i < keep; i++)
            for (unsigned c = 0; c < ch; c++) {
                const float x = planar ? ((const float*)s->frm->data[c])[i] : ((const float*)s->frm->data[0])[i * ch + c];
                const float v = x * 32768.0f;
                out[i * ch + c] = (int16_t)std::max(-32768.0f, std::min(32767.0f, v));
            }
        given += keep;
        ff.av_frame_unref(s->frm);
        if (keep) {
            std::lock_guard<std::mutex> lk(s->mu);
            push_pcm(s, out.data(), keep);
            s->stats.sound_frames += keep;
        }
    }
}

void decode_video(Shared* s, AVPacket* p, int gen)
{
    const ff_api::Api& ff = *s->ff;
    const auto t0 = std::chrono::steady_clock::now();
    if (ff.avcodec_send_packet(s->vdec, p) < 0 && p) {
        std::lock_guard<std::mutex> lk(s->mu);
        s->stats.decode_errors++;
        return;
    }
    while (ff.avcodec_receive_frame(s->vdec, s->frm) >= 0) {
        auto f = std::make_shared<Frame>();
        const AVFrame* a = s->frm;
        if (a->format != AV_PIX_FMT_YUV420P && a->format != AV_PIX_FMT_YUVA420P) {
            std::lock_guard<std::mutex> lk(s->mu);
            s->stats.decode_errors++;
            ff.av_frame_unref(s->frm);
            continue;
        }
        f->width = a->width;
        f->height = a->height;
        const int cw = a->width / 2, chh = a->height / 2;
        f->y.resize((size_t)a->width * a->height);
        f->u.resize((size_t)cw * chh);
        f->v.resize((size_t)cw * chh);
        for (int r = 0; r < a->height; r++)
            memcpy(&f->y[(size_t)r * a->width], a->data[0] + (size_t)r * a->linesize[0], (size_t)a->width);
        for (int r = 0; r < chh; r++) {
            memcpy(&f->u[(size_t)r * cw], a->data[1] + (size_t)r * a->linesize[1], (size_t)cw);
            memcpy(&f->v[(size_t)r * cw], a->data[2] + (size_t)r * a->linesize[2], (size_t)cw);
        }
        ff.av_frame_unref(s->frm);
        const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0).count();
        std::lock_guard<std::mutex> lk(s->mu);
        if (gen != s->gen) continue;   // decoded from a packet before a restart
        f->number = s->next_number++;
        s->frames.push_back(std::move(f));
        s->stats.frames_decoded++;
        s->stats.decode_us_total += us;
        s->stats.decode_us_max = std::max(s->stats.decode_us_max, us);
        s->cv.notify_all();
    }
}

// one packet read: the sound's decoded now, the video's queued (the packet given to the queue)
void read_one(Shared* s, int gen)
{
    const ff_api::Api& ff = *s->ff;
    const int rc = ff.av_read_frame(s->fmt, s->pkt);
    if (rc < 0) {
        std::lock_guard<std::mutex> lk(s->mu);
        if (gen != s->gen) return;   // a restart came: the seek decides what follows
        if (rc == AVERROR_EOF) s->eof = true;
        else s->failed = true;
        s->cv.notify_all();
        return;
    }
    if (s->pkt->stream_index == s->vs) {
        std::lock_guard<std::mutex> lk(s->mu);
        if (gen != s->gen) {   // read before a restart's seek
            ff.av_packet_unref(s->pkt);
            return;
        }
        s->vpkt_bytes += (size_t)s->pkt->size;
        s->vpkts.push_back(s->pkt);
        s->pkt = ff.av_packet_alloc();
        return;
    }
    if (s->pkt->stream_index == s->as) decode_sound(s, s->pkt);
    ff.av_packet_unref(s->pkt);
}

void flush(Shared* s, int gen)
{
    decode_video(s, nullptr, gen);   // what the decoder still holds (none for Bink: a frame a packet)
    std::lock_guard<std::mutex> lk(s->mu);
    s->flushed = true;
    s->sound_ended = true;
    s->cv.notify_all();
}

void do_restart(Shared* s)
{
    const ff_api::Api& ff = *s->ff;
    const int rc = ff.av_seek_frame(s->fmt, s->vs, 0, AVSEEK_FLAG_BACKWARD);
    if (s->vdec) ff.avcodec_flush_buffers(s->vdec);
    if (s->adec) ff.avcodec_flush_buffers(s->adec);
    std::lock_guard<std::mutex> lk(s->mu);
    if (rc < 0) s->failed = true;
    s->eof = s->flushed = false;
    s->sound_ended = false;
    s->stats.restarts++;
    s->cv.notify_all();
}

void run(std::shared_ptr<Shared> sp)
{
    Shared* s = sp.get();
    const size_t sound_target = s->as >= 0 ? (size_t)(kSoundAhead * s->rate) : 0;
    for (;;) {
        std::unique_lock<std::mutex> lk(s->mu);
        if (s->quit) break;
        if (s->restart_req) {
            s->restart_req = false;
            lk.unlock();
            do_restart(s);
            continue;
        }
        while (!s->frames.empty() && s->frames.front()->number < s->wanted) s->frames.pop_front();
        const bool need_video = (int)s->frames.size() < kAhead && !s->failed;
        const bool need_sound = s->as >= 0 && s->pcm_count < sound_target && !s->failed;
        const int gen = s->gen;
        if (need_video && !s->vpkts.empty()) {
            AVPacket* p = s->vpkts.front();
            s->vpkts.pop_front();
            s->vpkt_bytes -= (size_t)p->size;
            lk.unlock();
            decode_video(s, p, gen);
            s->ff->av_packet_free(&p);
            continue;
        }
        if ((need_video || need_sound) && !s->eof && !s->failed && s->vpkt_bytes < kPacketBytes) {
            lk.unlock();
            read_one(s, gen);
            continue;
        }
        if (s->eof && s->vpkts.empty() && !s->flushed) {
            lk.unlock();
            flush(s, gen);
            continue;
        }
        s->cv.wait_for(lk, std::chrono::milliseconds(20));
    }
}

class Sound : public al_stream::PcmSource {
public:
    explicit Sound(std::shared_ptr<Shared> s) : s_(std::move(s)) {}
    unsigned rate() const override { return s_->rate; }
    unsigned channels() const override { return s_->channels; }
    size_t read(int16_t* out, size_t frames, bool* ended) override
    {
        std::lock_guard<std::mutex> lk(s_->mu);
        const unsigned ch = s_->channels;
        const size_t n = std::min(frames, s_->pcm_count);
        for (size_t i = 0; i < n; i++)
            memcpy(&out[i * ch], &s_->pcm[((s_->pcm_head + i) % s_->pcm_cap) * ch], ch * sizeof(int16_t));
        if (s_->pcm_cap) s_->pcm_head = (s_->pcm_head + n) % s_->pcm_cap;
        s_->pcm_count -= n;
        *ended = (s_->sound_ended || s_->closed || s_->failed) && s_->pcm_count == 0;
        s_->cv.notify_all();
        return n;
    }

private:
    std::shared_ptr<Shared> s_;
};

}  // namespace

std::unique_ptr<Player> Player::open(const ff_api::Api* api, const std::string& host_path, std::string* err)
{
    std::unique_ptr<Player> p(new Player);
    p->s_ = std::make_shared<Shared>();
    Shared* s = p->s_.get();
    s->ff = api;
    const ff_api::Api& ff = *api;
    auto fail = [&](const std::string& why) {
        if (err) *err = why;
        p->close();
        return std::unique_ptr<Player>();
    };
    s->f = open_utf8(host_path);
    if (!s->f) return fail("cannot open " + host_path);
    uint8_t* buf = (uint8_t*)ff.av_malloc(kIoBuffer);
    s->io = ff.avio_alloc_context(buf, kIoBuffer, 0, s, io_read, nullptr, io_seek);
    s->fmt = ff.avformat_alloc_context();
    if (!buf || !s->io || !s->fmt) return fail("FFmpeg: out of memory");
    s->fmt->pb = s->io;
    s->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (ff.avformat_open_input(&s->fmt, "movie.bik", nullptr, nullptr) < 0) {
        s->fmt = nullptr;   // freed by the failed open
        return fail("FFmpeg cannot read " + host_path);
    }
    if (ff.avformat_find_stream_info(s->fmt, nullptr) < 0) return fail("FFmpeg: no stream information");
    for (unsigned i = 0; i < s->fmt->nb_streams; i++) {
        const AVCodecParameters* c = s->fmt->streams[i]->codecpar;
        if (c->codec_type == AVMEDIA_TYPE_VIDEO && s->vs < 0) s->vs = (int)i;
        else if (c->codec_type == AVMEDIA_TYPE_AUDIO && s->as < 0) s->as = (int)i;
    }
    if (s->vs < 0) return fail("no video stream in " + host_path);
    auto open_dec = [&](int si, AVCodecContext** out) {
        const AVCodecParameters* c = s->fmt->streams[si]->codecpar;
        const AVCodec* codec = ff.avcodec_find_decoder(c->codec_id);
        if (!codec) return false;
        *out = ff.avcodec_alloc_context3(codec);
        if (!*out || ff.avcodec_parameters_to_context(*out, c) < 0) return false;
        (*out)->thread_count = 1;
        return ff.avcodec_open2(*out, codec, nullptr) >= 0;
    };
    if (!open_dec(s->vs, &s->vdec)) return fail("no video decoder");
    if (s->as >= 0 && !open_dec(s->as, &s->adec)) s->as = -1;   // the picture without its sound
    s->pkt = ff.av_packet_alloc();
    s->frm = ff.av_frame_alloc();
    const AVStream* v = s->fmt->streams[s->vs];
    Info& in = p->info_;
    in.width = v->codecpar->width;
    in.height = v->codecpar->height;
    in.frames = (int)v->duration;   // the header's frame count (the Bink demuxer's)
    in.rate_num = v->time_base.den;  // a frame a tick: the time base is the header's rate inverted
    in.rate_den = v->time_base.num;
    if (s->as >= 0) {
        const AVCodecParameters* a = s->fmt->streams[s->as]->codecpar;
        s->rate = (unsigned)a->sample_rate;
        s->channels = (unsigned)std::min(2, a->ch_layout.nb_channels);
        in.sound_rate = (int)s->rate;
        in.sound_channels = (int)s->channels;
        if (a->ch_layout.nb_channels > 2 || !s->rate) s->as = -1, in.sound_rate = in.sound_channels = 0;
    }
    if (s->as >= 0) {
        s->pcm_cap = (size_t)s->rate * 4;
        s->pcm.assign(s->pcm_cap * s->channels, 0);
    }
    if (in.width <= 0 || in.height <= 0 || in.frames <= 0 || in.rate_num <= 0 || in.rate_den <= 0)
        return fail("unexpected movie header");
    p->thread_ = std::thread(run, p->s_);
    return p;
}

std::shared_ptr<const Frame> Player::frame(int n, int timeout_ms)
{
    Shared* s = s_.get();
    std::unique_lock<std::mutex> lk(s->mu);
    if (n > s->wanted) s->wanted = n;
    s->cv.notify_all();
    const auto t0 = std::chrono::steady_clock::now();
    bool waited = false;
    for (;;) {
        for (const auto& f : s->frames)
            if (f->number == n) {
                if (waited) {
                    const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::steady_clock::now() - t0).count();
                    s->stats.waits++;
                    s->stats.wait_us_max = std::max(s->stats.wait_us_max, us);
                }
                return f;
            }
        if (!s->frames.empty() && s->frames.front()->number > n) return nullptr;   // gone (asked for late)
        if (s->failed || s->closed || (s->flushed && s->next_number <= n)) return nullptr;
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeout_ms)) return nullptr;
        waited = true;
        s->cv.wait_for(lk, std::chrono::milliseconds(5));
    }
}

void Player::restart()
{
    std::lock_guard<std::mutex> lk(s_->mu);
    s_->restart_req = true;
    s_->gen++;
    for (AVPacket* p : s_->vpkts) s_->ff->av_packet_free(&p);
    s_->vpkts.clear();
    s_->vpkt_bytes = 0;
    s_->frames.clear();
    s_->next_number = 1;
    s_->wanted = 1;
    s_->cv.notify_all();
}

std::shared_ptr<al_stream::PcmSource> Player::sound() const
{
    if (!s_ || s_->as < 0) return nullptr;
    return std::make_shared<Sound>(s_);
}

Stats Player::stats() const
{
    std::lock_guard<std::mutex> lk(s_->mu);
    return s_->stats;
}

void Player::close()
{
    if (!s_) return;
    Shared* s = s_.get();
    {
        std::lock_guard<std::mutex> lk(s->mu);
        s->quit = true;
        s->cv.notify_all();
    }
    if (thread_.joinable()) thread_.join();
    const ff_api::Api& ff = *s->ff;
    std::lock_guard<std::mutex> lk(s->mu);
    if (s->closed) return;
    s->closed = true;
    s->sound_ended = true;
    for (AVPacket* p : s->vpkts) ff.av_packet_free(&p);
    s->vpkts.clear();
    s->frames.clear();
    if (s->pkt) ff.av_packet_free(&s->pkt);
    if (s->frm) ff.av_frame_free(&s->frm);
    if (s->vdec) ff.avcodec_free_context(&s->vdec);
    if (s->adec) ff.avcodec_free_context(&s->adec);
    if (s->fmt) ff.avformat_close_input(&s->fmt);
    if (s->io) {
        ff.av_free(s->io->buffer);
        ff.avio_context_free(&s->io);
    }
    if (s->f) fclose(s->f);
    s->f = nullptr;
}

Player::~Player()
{
    close();
}

}  // namespace movie
