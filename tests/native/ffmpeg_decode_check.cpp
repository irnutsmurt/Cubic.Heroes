// M8.1 (docs/FFMPEG_MIGRATION_PLAN.md section 6): FFmpeg loaded the game's way (port/native/movie/ff_api.cpp, by
// full path), and every frame and every sample of the player's movies decoded, no game running.
//
//   ffmpeg_decode_check <the player's movie folder> [<out folder>] [<FFmpeg's folder>]
//
// For each of the game's five movies (the file names below, facts from their headers; the files are the
// player's own, read as they are): the file is read through an I/O context of this program's (as the player
// reads it: its own file access, nothing converted), every packet demuxed, every video frame and every sound
// sample decoded, the decoders flushed at the end. Checked against the header: the frame count, 30 frames a
// second, 1280x720 planar YUV 4:2:0, one keyframe (the first); the sound's rate and channels; and every packet's
// declared samples decoded. No decode error and no warning from FFmpeg. Measured: the decode time per video frame
// (one thread, C only: mean, p50, p95, p99, max against the 33.3 ms a frame lasts) and the bytes read.
// Written to <out>/<movie>.frames.txt: a hash of each decoded frame's planes (FNV-1a 64), for the native
// player's own decode to be compared with (M8.2); <out>/ffmpeg_decode_check.txt: the summary.
#include "ff_api.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Expect {
    const char* file;
    int frames;
    int rate;       // the sound's sample rate; 0: no sound
    int channels;
};
// docs/FFMPEG_MIGRATION_PLAN.md section 1. All five: 1280x720 at the header's 10000000/333333 frames a second
// (30.00003; FFmpeg's time base is its inverse, a frame a tick).
const int kRateNum = 10000000, kRateDen = 333333;
const Expect kMovies[] = {
    {"TitleMovie_Omote_eng.bik", 2160, 0, 0},
    {"TitleMovie_Ura_eng.bik", 2160, 0, 0},
    {"OpeningMovie_eng.bik", 4620, 0, 0},
    {"PlayDemoMovieUS.bik", 3465, 48000, 2},
    {"CreditMovie_eng.bik", 7860, 44100, 2},
};

ff_api::Api ff;
int g_log_warnings;
std::string g_log_first;

void log_cb(void* avcl, int level, const char* fmt, va_list vl)
{
    if (level > AV_LOG_WARNING) return;
    char line[1024];
    int prefix = 1;
    ff.av_log_format_line2(avcl, level, fmt, vl, line, sizeof line, &prefix);
    if (g_log_warnings++ < 8) {
        fprintf(stderr, "  FFmpeg: %s", line);
        if (g_log_first.empty()) g_log_first = line;
    }
}

const char* nz(const char* s)
{
    return s ? s : "?";
}

std::string err_text(int e)
{
    char b[128];
    ff.av_strerror(e, b, sizeof b);
    return b;
}

// the I/O context's file access (the player's will be the runtime's)
struct File {
    FILE* f = nullptr;
    uint64_t bytes = 0;
    int seeks = 0;
};

int io_read(void* opaque, uint8_t* buf, int size)
{
    File* fl = (File*)opaque;
    const size_t n = fread(buf, 1, (size_t)size, fl->f);
    fl->bytes += n;
    return n ? (int)n : AVERROR_EOF;
}

int64_t io_seek(void* opaque, int64_t offset, int whence)
{
    File* fl = (File*)opaque;
#ifdef _WIN32
#define SEEK64 _fseeki64
#define TELL64 _ftelli64
#else
#define SEEK64 fseeko
#define TELL64 ftello
#endif
    if (whence & AVSEEK_SIZE) {
        const int64_t here = TELL64(fl->f);
        SEEK64(fl->f, 0, SEEK_END);
        const int64_t size = TELL64(fl->f);
        SEEK64(fl->f, here, SEEK_SET);
        return size;
    }
    fl->seeks++;
    if (SEEK64(fl->f, offset, whence & ~AVSEEK_FORCE) != 0) return -1;
    return TELL64(fl->f);
}

struct Hash {
    uint64_t h = 1469598103934665603ull;
    void add(const uint8_t* p, size_t n)
    {
        for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    }
};

double pct(std::vector<double> v, double p)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)(p * (double)(v.size() - 1) + 0.5))];
}

struct Result {
    bool ok = true;
    std::string why;
    void fail(const std::string& w)
    {
        if (ok) why = w;
        ok = false;
    }
};

Result check_movie(const std::string& dir, const std::string& out, const Expect& e, FILE* summary)
{
    Result r;
    const std::string path = dir + "/" + e.file;
    File fl;
    fl.f = fopen(path.c_str(), "rb");
    if (!fl.f) {
        r.fail("not found: " + path);
        return r;
    }
    g_log_warnings = 0;
    g_log_first.clear();
    const int kBuf = 1 << 16;
    uint8_t* iobuf = (uint8_t*)ff.av_malloc(kBuf);
    AVIOContext* io = ff.avio_alloc_context(iobuf, kBuf, 0, &fl, io_read, nullptr, io_seek);
    AVFormatContext* fmt = ff.avformat_alloc_context();
    fmt->pb = io;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    int rc = ff.avformat_open_input(&fmt, e.file, nullptr, nullptr);
    if (rc < 0) {
        r.fail("open: " + err_text(rc));
        fclose(fl.f);
        return r;
    }
    rc = ff.avformat_find_stream_info(fmt, nullptr);
    if (rc < 0) r.fail("stream info: " + err_text(rc));

    int vs = -1;
    std::vector<int> as;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        const AVCodecParameters* p = fmt->streams[i]->codecpar;
        if (p->codec_type == AVMEDIA_TYPE_VIDEO && vs < 0) vs = (int)i;
        else if (p->codec_type == AVMEDIA_TYPE_AUDIO) as.push_back((int)i);
    }
    // the decoders: one per stream, one thread each
    std::vector<AVCodecContext*> dec(fmt->nb_streams, nullptr);
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        if ((int)i != vs && std::find(as.begin(), as.end(), (int)i) == as.end()) continue;
        const AVCodecParameters* p = fmt->streams[i]->codecpar;
        const AVCodec* c = ff.avcodec_find_decoder(p->codec_id);
        if (!c) {
            r.fail(std::string("no decoder for ") + ff.avcodec_get_name(p->codec_id));
            continue;
        }
        dec[i] = ff.avcodec_alloc_context3(c);
        ff.avcodec_parameters_to_context(dec[i], p);
        dec[i]->thread_count = 1;
        if ((rc = ff.avcodec_open2(dec[i], c, nullptr)) < 0) r.fail("open decoder: " + err_text(rc));
    }
    if (vs < 0) r.fail("no video stream");
    const AVStream* v = vs >= 0 ? fmt->streams[vs] : nullptr;
    char head[512];
    // the header as FFmpeg's Bink demuxer keeps it: the frame count as the stream's duration, the frame rate as its
    // time base (a frame a tick)
    snprintf(head, sizeof head, "%s: %s %dx%d, time base %d/%d, %lld frames in the header; %d sound stream(s)",
             e.file, v ? ff.avcodec_get_name(v->codecpar->codec_id) : "-", v ? v->codecpar->width : 0,
             v ? v->codecpar->height : 0, v ? v->time_base.num : 0, v ? v->time_base.den : 0,
             v ? (long long)v->duration : 0, (int)as.size());
    printf("%s\n", head);
    fprintf(summary, "%s\n", head);
    if (v) {
        if (v->duration != e.frames) r.fail("header frames " + std::to_string(v->duration));
        if (v->time_base.num != kRateDen || v->time_base.den != kRateNum) r.fail("not 10000000/333333 a second");
        if (v->codecpar->width != 1280 || v->codecpar->height != 720) r.fail("not 1280x720");
    }
    if ((int)as.size() != (e.rate ? 1 : 0)) r.fail("sound streams: " + std::to_string(as.size()));
    for (int a : as) {
        const AVCodecParameters* p = fmt->streams[a]->codecpar;
        if (p->sample_rate != e.rate || p->ch_layout.nb_channels != e.channels)
            r.fail("sound " + std::to_string(p->sample_rate) + " Hz, " + std::to_string(p->ch_layout.nb_channels) +
                   " channels");
    }

    FILE* fh = fopen((out + "/" + e.file + ".frames.txt").c_str(), "w");
    AVPacket* pkt = ff.av_packet_alloc();
    AVFrame* fr = ff.av_frame_alloc();
    long long vpackets = 0, vframes = 0, keys = 0, declared = 0, samples = 0, apackets = 0;
    std::vector<double> vms;
    double ams = 0;
    double peak = 0, sumsq = 0;
    Hash ahash;
    auto receive = [&](int si) {
        AVCodecContext* d = dec[si];
        for (;;) {
            const auto t0 = std::chrono::steady_clock::now();
            const int g = ff.avcodec_receive_frame(d, fr);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (g == AVERROR(EAGAIN) || g == AVERROR_EOF) return ms;
            if (g < 0) {
                r.fail("decode: " + err_text(g));
                return ms;
            }
            if (si == vs) {
                if (fr->format != AV_PIX_FMT_YUV420P || fr->width != 1280 || fr->height != 720)
                    r.fail(std::string("frame format ") + nz(ff.av_get_pix_fmt_name((AVPixelFormat)fr->format)));
                Hash h;
                for (int pl = 0; pl < 3; pl++) {
                    const int w = pl ? fr->width / 2 : fr->width, hgt = pl ? fr->height / 2 : fr->height;
                    for (int y = 0; y < hgt; y++) h.add(fr->data[pl] + (size_t)y * fr->linesize[pl], (size_t)w);
                }
                if (fh) fprintf(fh, "%lld %016llx\n", vframes, (unsigned long long)h.h);
                vframes++;
            } else {
                if (fr->format != AV_SAMPLE_FMT_FLTP && fr->format != AV_SAMPLE_FMT_FLT)
                    r.fail(std::string("sample format ") + nz(ff.av_get_sample_fmt_name((AVSampleFormat)fr->format)));
                const int ch = fr->ch_layout.nb_channels;
                const bool planar = fr->format == AV_SAMPLE_FMT_FLTP;
                for (int c = 0; c < ch; c++) {
                    const float* s = planar ? (const float*)fr->data[c] : (const float*)fr->data[0] + c;
                    const int step = planar ? 1 : ch;
                    for (int i = 0; i < fr->nb_samples; i++) {
                        const double x = s[(size_t)i * step];
                        peak = std::max(peak, x < 0 ? -x : x);
                        sumsq += x * x;
                    }
                    ahash.add((const uint8_t*)s, (size_t)fr->nb_samples * sizeof(float) * (planar ? 1 : ch));
                }
                samples += fr->nb_samples;
            }
            ff.av_frame_unref(fr);
            return ms;
        }
    };
    auto decode = [&](int si, AVPacket* p) {
        const auto t0 = std::chrono::steady_clock::now();
        const int s = ff.avcodec_send_packet(dec[si], p);
        if (s < 0 && s != AVERROR_EOF) r.fail("send: " + err_text(s));
        // every frame a packet makes (the video: one; the sound: its blocks; the flush: what is left)
        for (;;) {
            const long long before = si == vs ? vframes : samples;
            receive(si);
            if ((si == vs ? vframes : samples) == before) break;
        }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    // the sound, packet by packet: the samples a packet declares (its first word: the decompressed size in bytes
    // of 16-bit samples, as the demuxer reads it for the timestamps) against the samples its blocks decode to
    long long uneven = 0, excess_max = 0, short_max = 0, last_uneven = -1;
    std::string uneven_at;   // the first such packets: their index (of the sound's packets) and the difference
    while ((rc = ff.av_read_frame(fmt, pkt)) >= 0) {
        const int si = pkt->stream_index;
        if (si == vs) {
            vpackets++;
            if (pkt->flags & AV_PKT_FLAG_KEY) keys++;
            vms.push_back(decode(si, pkt));
        } else if (si < (int)dec.size() && dec[si]) {
            apackets++;
            const int ch = fmt->streams[si]->codecpar->ch_layout.nb_channels;
            const long long want = pkt->size >= 4
                ? (long long)(pkt->data[0] | pkt->data[1] << 8 | pkt->data[2] << 16 | (uint32_t)pkt->data[3] << 24) /
                      (2 * ch)
                : 0;
            declared += want;
            const long long before = samples;
            ams += decode(si, pkt);
            const long long got = samples - before;
            if (got != want) {
                uneven++;
                excess_max = std::max(excess_max, got - want);
                short_max = std::max(short_max, want - got);
                char u[160];
                snprintf(u, sizeof u, "%s%lld (%+lld)", uneven_at.empty() ? "" : ", ", apackets - 1, got - want);
                if (uneven <= 8) uneven_at += u;
                last_uneven = apackets - 1;
            }
        }
        ff.av_packet_unref(pkt);
    }
    if (rc != AVERROR_EOF) r.fail("read: " + err_text(rc));
    for (unsigned i = 0; i < dec.size(); i++)
        if (dec[i]) decode((int)i, nullptr);   // flush

    if (vframes != e.frames || vpackets != e.frames)
        r.fail("decoded " + std::to_string(vframes) + " frames of " + std::to_string(vpackets) + " packets");
    if (keys != 1) r.fail("keyframes: " + std::to_string(keys));
    // every declared sample decoded: each packet gives exactly the samples it declares, but the last, whose final
    // transform block is decoded whole (less than a block more: the player plays the declared ones)
    if (e.rate && (short_max > 0 || samples < declared || (uneven && (uneven > 1 || last_uneven != apackets - 1)) ||
                   excess_max >= 2048))
        r.fail("sound: " + std::to_string(samples) + " samples decoded, " + std::to_string(declared) +
               " declared; packets not as declared: " + uneven_at);
    // the sound lasts as long as the picture: within a frame
    if (e.rate && std::fabs((double)declared / e.rate - (double)e.frames * kRateDen / kRateNum) > (double)kRateDen /
                                                                                                    kRateNum)
        r.fail("sound " + std::to_string((double)declared / e.rate) + " s against the picture's " +
               std::to_string((double)e.frames * kRateDen / kRateNum) + " s");
    if (g_log_warnings) r.fail(std::to_string(g_log_warnings) + " FFmpeg warning(s): " + g_log_first);

    double mean = 0;
    for (double x : vms) mean += x;
    mean = vms.empty() ? 0 : mean / (double)vms.size();
    char line[1024];
    snprintf(line, sizeof line,
             "  video: %lld frames decoded (%lld packets, %lld keyframe); %.2f ms a frame (p50 %.2f, p95 %.2f, "
             "p99 %.2f, max %.2f; one thread)\n"
             "  sound: %lld samples decoded a channel (%lld declared, %lld packets, %lld of them not as declared%s%s%s:"
             " up to %lld more, %lld fewer; %.3f s declared), peak %.3f, rms %.4f, hash %016llx; %.0f ms decoding\n"
             "  read: %.1f MB, %d seeks; frame hashes: %s.frames.txt\n  %s%s\n",
             vframes, vpackets, keys, mean, pct(vms, 0.5), pct(vms, 0.95), pct(vms, 0.99), pct(vms, 1.0), samples,
             declared, apackets, uneven, uneven ? " (packet " : "", uneven_at.c_str(), uneven ? ")" : "", excess_max,
             short_max, e.rate ? (double)declared / e.rate : 0.0, peak,
             samples ? std::sqrt(sumsq / (double)samples / (e.channels ? e.channels : 1)) : 0.0,
             (unsigned long long)ahash.h, ams, (double)fl.bytes / 1e6, fl.seeks, e.file,
             r.ok ? "ok" : "FAILED: ", r.ok ? "" : r.why.c_str());
    fputs(line, stdout);
    fputs(line, summary);
    fflush(stdout);

    if (fh) fclose(fh);
    ff.av_frame_free(&fr);
    ff.av_packet_free(&pkt);
    for (AVCodecContext*& d : dec)
        if (d) ff.avcodec_free_context(&d);
    ff.avformat_close_input(&fmt);
    ff.av_free(io->buffer);
    ff.avio_context_free(&io);
    fclose(fl.f);
    return r;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <movie folder> [<out folder>] [<FFmpeg's folder>]\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    const std::string out = argc > 2 ? argv[2] : ".";
    std::string error;
    const std::string lib = argc > 3 ? argv[3] : ff_api::default_dir();
    if (!ff_api::load(&ff, lib, &error)) {
        fprintf(stderr, "%s\n", error.c_str());
        printf("ffmpeg_decode_check: FAILED (FFmpeg not loaded)\n");
        return 1;
    }
    ff.av_log_set_level(AV_LOG_WARNING);
    ff.av_log_set_callback(log_cb);
    FILE* summary = fopen((out + "/ffmpeg_decode_check.txt").c_str(), "w");
    if (!summary) summary = stderr;
    char head[512];
    snprintf(head, sizeof head, "FFmpeg %s loaded from %s (avutil %u.%u, avcodec %u.%u, avformat %u.%u; %s)\n",
             ff.version.c_str(), ff.dir.c_str(), ff.avutil_version() >> 16, (ff.avutil_version() >> 8) & 255,
             ff.avcodec_version() >> 16, (ff.avcodec_version() >> 8) & 255, ff.avformat_version() >> 16,
             (ff.avformat_version() >> 8) & 255, ff.licence.c_str());
    fputs(head, stdout);
    fputs(head, summary);
    int failed = 0;
    for (const Expect& e : kMovies)
        if (!check_movie(dir, out, e, summary).ok) failed++;
    const std::string verdict = failed ? "FAILED (" + std::to_string(failed) + " of 5 movies)" : "passed";
    printf("ffmpeg_decode_check: %s\n", verdict.c_str());
    fprintf(summary, "ffmpeg_decode_check: %s\n", verdict.c_str());
    if (summary != stderr) fclose(summary);
    ff_api::unload(&ff);
    return failed ? 1 : 0;
}
