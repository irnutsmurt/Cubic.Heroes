// M2's tests of the OpenAL Soft layer alone (port/native/audio; docs/OPENAL_MIGRATION_PLAN.md section 7): no
// game. The library: OpenAL32.dll / lib/libopenal.so.1 beside this program, or OPENAL_LIB=<path>.
//
//   openal_check files <sound dir>
//       every .wav read (wav_pcm) and loaded into an AL buffer with its loop points (AL_SOFT_loop_points) on a
//       loopback device: the counts, the loops, the bytes, anything refused.
//   openal_check loops <sound dir>
//       every file with a `smpl` loop played looped and rendered on a loopback device at the file's own rate
//       (nothing resampled): an effect as a resident buffer, music streamed; the output compared sample by
//       sample with the file's frames as rel27 plays them -- the intro once, then the loop region again and
//       again (a loop ending before the file's end included) -- over the intro and two passes of the loop.
//   openal_check stream <file.wav> <seconds> <once|whole|points> [<buffer ms> [<buffers>]]
//       the file streamed and rendered on a loopback device, compared sample by sample with what the loop mode
//       says, over <seconds> of audio rendered as fast as the machine goes.
//   openal_check synth
//       made-up files: the loop forms (intro and loop, a loop ending before the file's end, the whole file,
//       once) resident and streamed; a crossfade's gain curves; positional stereo and attenuation (inverse
//       distance, clamped); the EFX reverb on and off; the command queue's bound; open/close and thread
//       start/stop cycles with no object left (leaks).
//   openal_check device <file.wav> <seconds> [reopen]
//       the default output in real time: the file streamed (looped) by the audio thread for <seconds>; the
//       underruns, near-underruns and the thread's worst period; with `reopen`, the device lost on purpose half
//       way (simulate_loss), reopened by the audio thread and the stream restarted.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#endif

#include "al_api.h"
#include "al_device.h"
#include "al_stream.h"
#include "voice_pool.h"
#include "wav_pcm.h"

namespace {

al_api::Api g_al;
int g_fail = 0;

void quiet_log(const char*) {}

#define CHECK(cond, ...)                                                                                            \
    do {                                                                                                            \
        if (cond) {                                                                                                 \
            printf("  PASS ");                                                                                      \
        } else {                                                                                                    \
            printf("  FAIL ");                                                                                      \
            g_fail++;                                                                                               \
        }                                                                                                           \
        printf(__VA_ARGS__);                                                                                        \
        printf("\n");                                                                                               \
        fflush(stdout);                                                                                             \
    } while (0)

bool load_library()
{
    const char* e = getenv("OPENAL_LIB");
    std::string err;
    if (!al_api::load(&g_al, e && *e ? e : al_api::default_path(), &err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return false;
    }
    return true;
}

std::vector<std::string> wav_files(const std::string& dir)
{
    std::vector<std::string> out;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*.wav").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do out.push_back(dir + "\\" + fd.cFileName);
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 4 && (n.substr(n.size() - 4) == ".wav" || n.substr(n.size() - 4) == ".WAV"))
                out.push_back(dir + "/" + n);
        }
        closedir(d);
    }
#endif
    std::sort(out.begin(), out.end());
    return out;
}

std::string base(const std::string& p) { return p.substr(p.find_last_of("\\/") + 1); }

// A synthetic WAV (16-bit), with a `smpl` loop when loop_end > 0 (end exclusive).
std::string write_wav(const std::string& name, unsigned ch, unsigned rate, const std::vector<int16_t>& pcm,
                      uint32_t loop_start = 0, uint32_t loop_end = 0)
{
    const char* tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMPDIR");
    const std::string path = std::string(tmp ? tmp : ".") + "/openal_check_" + name + ".wav";
    FILE* f = fopen(path.c_str(), "wb");
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    const uint32_t data = (uint32_t)(pcm.size() * 2);
    const uint32_t smpl = loop_end ? 36 + 24 : 0;
    fwrite("RIFF", 1, 4, f);
    u32(4 + 24 + 8 + data + (smpl ? 8 + smpl : 0));
    fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16((uint16_t)ch); u32(rate); u32(rate * ch * 2); u16((uint16_t)(ch * 2)); u16(16);
    if (smpl) {   // before the data, as some of the game's files have it
        fwrite("smpl", 1, 4, f);
        u32(smpl);
        for (int k = 0; k < 7; k++) u32(0);
        u32(1); u32(0);                                     // one loop, no sampler data
        u32(0); u32(0); u32(loop_start); u32(loop_end - 1); u32(0); u32(0);
    }
    fwrite("data", 1, 4, f);
    u32(data);
    fwrite(pcm.data(), 2, pcm.size(), f);
    fclose(f);
    return path;
}

// What a looped (or once) play of a file sounds like, frame by frame (rel27's rule: the intro, then the loop
// region; or the whole file again; or once, then silence).
struct Expected {
    const std::vector<int16_t>* pcm;
    unsigned ch;
    uint64_t frames, rs, re, pos = 0;
    bool once, ended = false;
    uint64_t after = 0;
    void next(float out[2])
    {
        if (!ended && pos >= (once ? frames : re)) {
            if (once) ended = true;
            else pos = rs;
        }
        if (ended) {
            // OpenAL Soft holds a finished source's last frame to the end of its mixing update (measured: up to
            // 784 frames at 44.1 kHz), then gives silence: those frames may be either (NaN: not compared)
            out[0] = out[1] = after++ < 2048 ? NAN : 0.f;
            return;
        }
        for (unsigned c = 0; c < 2; c++) out[c] = (*pcm)[pos * ch + (ch == 1 ? 0 : c)] / 32768.f;
        pos++;
    }
};

// The rendered output against the expected frames, up to one gain per channel (a mono source is panned to
// the centre: its level on each side is the pan law's, not 1).
struct Compare {
    double k[2] = {0, 0}, num[2] = {0, 0}, den[2] = {0, 0};
    bool calibrated = false;
    std::vector<float> held_out, held_exp;
    double max_err = 0;
    uint64_t n = 0, first_bad = ~0ull, bad = 0;
    void add(const float* out, const float* exp, size_t frames)
    {
        if (!calibrated) {   // the gains from the first second with sound
            held_out.insert(held_out.end(), out, out + 2 * frames);
            held_exp.insert(held_exp.end(), exp, exp + 2 * frames);
            for (size_t i = 0; i < 2 * frames; i++) {
                if (std::isnan(exp[i])) continue;
                num[i & 1] += (double)out[i] * exp[i];
                den[i & 1] += (double)exp[i] * exp[i];
            }
            if (held_out.size() < 2 * 48000 || den[0] < 1e-3 || den[1] < 1e-3) return;
            for (int c = 0; c < 2; c++) k[c] = num[c] / den[c];
            calibrated = true;
            std::vector<float> o, e;
            o.swap(held_out);
            e.swap(held_exp);
            add(o.data(), e.data(), o.size() / 2);
            return;
        }
        for (size_t i = 0; i < frames; i++, n++)
            for (int c = 0; c < 2; c++) {
                if (std::isnan(exp[2 * i + c])) continue;
                const double err = std::fabs(out[2 * i + c] - k[c] * exp[2 * i + c]);
                max_err = std::max(max_err, err);
                if (err > 2e-4) {
                    bad++;
                    if (first_bad == ~0ull) first_bad = n;
                }
            }
    }
    void finish()
    {
        if (!calibrated && !held_out.empty()) {
            for (int c = 0; c < 2; c++) k[c] = den[c] > 0 ? num[c] / den[c] : 1.0;
            calibrated = true;
            std::vector<float> o, e;
            o.swap(held_out);
            e.swap(held_exp);
            add(o.data(), e.data(), o.size() / 2);
        }
    }
};

struct Loopback {
    al_device::Device dev;
    bool open(int rate)
    {
        al_device::Options o;
        o.loopback = true;
        o.loopback_rate = rate;
        std::string err;
        if (!dev.open(&g_al, o, quiet_log, &err)) {
            fprintf(stderr, "loopback: %s\n", err.c_str());
            return false;
        }
        return true;
    }
};

double g_rendered = 0;   // seconds of audio rendered: the streamer's clock in the loopback tests
double rendered_clock(void*) { return g_rendered; }

// Plays `path` looped (or once) on a loopback device at its own rate for `seconds` and compares. Resident: a
// buffer with its loop points on a looping source; else streamed through a manual Streamer.
bool play_and_compare(const std::string& path, bool resident, al_stream::Loop loop, double seconds, Compare* cmp,
                      al_stream::Totals* totals, unsigned buffer_ms = 250, unsigned nbuf = 4)
{
    wav_pcm::Info info;
    std::vector<int16_t> pcm;
    std::string err;
    if (!wav_pcm::load(path, &info, &pcm, &err)) {
        printf("  %s: %s\n", path.c_str(), err.c_str());
        return false;
    }
    Loopback lb;
    if (!lb.open((int)info.rate)) return false;
    const al_api::Api& al = lb.dev.al();
    Expected ex{&pcm, info.channels, info.frames, 0, info.frames, 0, loop == al_stream::Loop::once, false};
    if (loop == al_stream::Loop::file_loop_points && info.has_loop) {
        ex.rs = info.loop_start;
        ex.re = info.loop_end;
    }
    al_device::Sample sample;
    unsigned src = 0;
    al_stream::Config cfg;
    cfg.manual = true;
    cfg.buffer_ms = buffer_ms;
    cfg.buffers = nbuf;
    cfg.clock = rendered_clock;
    g_rendered = 0;
    al_stream::Streamer st(lb.dev, cfg);
    al_stream::Handle h = al_stream::kNone;
    if (resident) {
        if (!al_device::load_sample(lb.dev, path, &sample, &err)) {
            printf("  %s: %s\n", path.c_str(), err.c_str());
            return false;
        }
        src = lb.dev.new_source();
        al.alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
        al.alSourcei(src, AL_DIRECT_CHANNELS_SOFT, AL_TRUE);
        al.alSourcei(src, AL_BUFFER, (ALint)sample.buffer);
        al.alSourcei(src, AL_LOOPING, ex.once ? AL_FALSE : AL_TRUE);
        if (loop == al_stream::Loop::whole_file && sample.loop_points_set) {   // whole file: no loop points
            al.alSourcei(src, AL_BUFFER, 0);
            const ALint lp[2] = {0, (ALint)info.frames};
            al.alBufferiv(sample.buffer, AL_LOOP_POINTS_SOFT, lp);
            al.alSourcei(src, AL_BUFFER, (ALint)sample.buffer);
        }
        al.alSourcePlay(src);
    } else {
        h = st.open(path, loop, 1.0f, true);
        st.service_once();
        // direct channels: a stereo stream's channels straight to the output's
        // (the streamer's own source; set through the AL with the handle's source -- tests only)
    }
    const unsigned chunk = info.rate / 50;   // 20 ms between services
    std::vector<float> out(2 * chunk), exp(2 * chunk);
    const uint64_t total = (uint64_t)(seconds * info.rate);
    for (uint64_t done = 0; done < total; done += chunk) {
        lb.dev.render(out.data(), (int)chunk);
        for (unsigned k = 0; k < chunk; k++) ex.next(&exp[2 * k]);
        cmp->add(out.data(), exp.data(), chunk);
        g_rendered += (double)chunk / info.rate;
        if (!resident) st.service_once();
    }
    cmp->finish();
    if (totals) *totals = st.totals();
    if (src) lb.dev.delete_source(src);
    if (resident) al_device::free_sample(lb.dev, &sample);
    st.shutdown();
    (void)h;
    return true;
}

// ---- files ----

int cmd_files(const std::string& dir)
{
    Loopback lb;
    if (!lb.open(48000)) return 2;
    unsigned n = 0, refused = 0, with_loop = 0, loop_set = 0, dropped = 0, mono = 0, stereo = 0;
    uint64_t bytes = 0;
    double seconds = 0;
    for (const std::string& path : wav_files(dir)) {
        n++;
        al_device::Sample s;
        std::string err;
        if (!al_device::load_sample(lb.dev, path, &s, &err)) {
            refused++;
            printf("  refused %s: %s\n", base(path).c_str(), err.c_str());
            continue;
        }
        bytes += s.info.data_bytes;
        seconds += s.seconds();
        (s.info.channels == 1 ? mono : stereo)++;
        if (s.info.has_loop) with_loop++;
        if (s.loop_points_set) loop_set++;
        if (s.info.loop_dropped) {
            dropped++;
            printf("  %s: a loop outside the data, ignored\n", base(path).c_str());
        }
        al_device::free_sample(lb.dev, &s);
    }
    const al_device::Live l = lb.dev.live();
    printf("files %u (mono %u, stereo %u), refused %u; loops %u, loop points set %u, dropped %u; %.1f MB, %.0f s of "
           "sound; live objects after %d/%d; %s %s\n",
           n, mono, stereo, refused, with_loop, loop_set, dropped, bytes / 1e6, seconds, l.sources, l.buffers,
           g_al.renderer.c_str(), g_al.version.c_str());
    CHECK(n > 0 && refused == 0, "every file loads (%u of %u)", n - refused, n);
    CHECK(with_loop == loop_set, "every loop set as the buffer's loop points (%u of %u)", loop_set, with_loop);
    CHECK(!l.any(), "no AL object left");
    return g_fail ? 1 : 0;
}

// ---- loops ----

int cmd_loops(const std::string& dir)
{
    unsigned n = 0;
    for (const std::string& path : wav_files(dir)) {
        wav_pcm::Info info;
        std::string err;
        FILE* f = wav_pcm::open_utf8(path);
        if (!f) continue;
        const bool ok = wav_pcm::parse_file(f, &info, &err);
        fclose(f);
        if (!ok || !info.has_loop) continue;
        n++;
        const bool music = info.seconds() > 30 || base(path).rfind("bgm", 0) == 0;
        const double intro = (double)info.loop_start / info.rate;
        const double loop = (double)(info.loop_end - info.loop_start) / info.rate;
        const double secs = std::min(intro + 2 * loop + 1.0, music ? 600.0 : 120.0);
        Compare c;
        al_stream::Totals t;
        if (!play_and_compare(path, !music, al_stream::Loop::file_loop_points, secs, &c, &t)) {
            g_fail++;
            continue;
        }
        CHECK(c.bad == 0 && t.underruns == 0,
              "%-28s %s %5.1f s: intro %.2f s, loop %.2f-%.2f s of %.2f s, %.0f s rendered: max error %.1e, "
              "%llu frames off%s, %llu underruns", base(path).c_str(), music ? "streamed" : "resident", info.seconds(),
              intro, intro, (double)info.loop_end / info.rate, info.seconds(), secs, c.max_err,
              (unsigned long long)c.bad, c.bad ? (" from frame " + std::to_string(c.first_bad)).c_str() : "",
              (unsigned long long)t.underruns);
    }
    printf("%u looped files, %d failed\n", n, g_fail);
    return g_fail ? 1 : 0;
}

// ---- stream ----

int cmd_stream(const std::string& path, double seconds, const std::string& mode, unsigned buffer_ms, unsigned nbuf)
{
    const al_stream::Loop loop = mode == "once" ? al_stream::Loop::once
                                 : mode == "whole" ? al_stream::Loop::whole_file : al_stream::Loop::file_loop_points;
    Compare c;
    al_stream::Totals t;
    const auto t0 = std::chrono::steady_clock::now();
    if (!play_and_compare(path, false, loop, seconds, &c, &t, buffer_ms, nbuf)) return 2;
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(c.bad == 0 && t.underruns == 0 && t.read_errors == 0,
          "%s %s: %.0f s of audio (buffers %u x %u ms) in %.1f s: max error %.1e, %llu frames off; %llu loops, "
          "%llu underruns, %llu read errors", base(path).c_str(), mode.c_str(), seconds, nbuf, buffer_ms, wall,
          c.max_err, (unsigned long long)c.bad, (unsigned long long)t.loops, (unsigned long long)t.underruns,
          (unsigned long long)t.read_errors);
    return g_fail ? 1 : 0;
}

// ---- synth ----

std::vector<int16_t> tone(unsigned ch, unsigned rate, double seconds, double hz, double amp, double hz2 = 0)
{
    const size_t n = (size_t)(seconds * rate);
    std::vector<int16_t> v(n * ch);
    for (size_t i = 0; i < n; i++)
        for (unsigned c = 0; c < ch; c++) {
            const double f = c && hz2 ? hz2 : hz;
            v[i * ch + c] = (int16_t)std::lround(32767 * amp * std::sin(2 * 3.14159265358979 * f * i / rate));
        }
    return v;
}

std::vector<int16_t> ramp_frames(unsigned ch, size_t n)   // every frame different: a loop error shows at once
{
    std::vector<int16_t> v(n * ch);
    uint32_t x = 12345;
    for (size_t i = 0; i < n * ch; i++) {
        x = x * 1103515245u + 12345u;
        v[i] = (int16_t)((int32_t)(x >> 16) - 32768) / 2;
    }
    return v;
}

void synth_loops()
{
    printf("loop forms (made-up files, every frame distinct)\n");
    struct Case {
        const char* name;
        unsigned ch, rate;
        size_t frames;
        uint32_t ls, le;   // loop (le 0: none)
        al_stream::Loop loop;
    } cases[] = {
        {"intro_and_loop", 2, 44100, 44100 * 3, 44100, 44100 * 3, al_stream::Loop::file_loop_points},
        {"loop_end_before_eof", 2, 48000, 48000 * 3, 12000, 48000 * 2, al_stream::Loop::file_loop_points},
        {"mono_effect_loop", 1, 44100, 22050, 5000, 22050, al_stream::Loop::file_loop_points},
        {"whole_file", 1, 44100, 30000, 0, 0, al_stream::Loop::whole_file},
        {"once", 2, 44100, 30000, 0, 0, al_stream::Loop::once},
        {"once_of_a_looped_file", 2, 44100, 30000, 10000, 30000, al_stream::Loop::once},
    };
    for (const Case& k : cases) {
        const std::string path = write_wav(k.name, k.ch, k.rate, ramp_frames(k.ch, k.frames), k.ls, k.le);
        for (int resident = 0; resident < 2; resident++) {
            Compare c;
            al_stream::Totals t;
            const double secs = (double)k.frames / k.rate * 3.3;
            if (!play_and_compare(path, resident, k.loop, secs, &c, &t, 100, 3)) {
                g_fail++;
                continue;
            }
            CHECK(c.bad == 0 && t.underruns == 0,
                  "%-24s %-8s %.1f s: max error %.1e, %llu frames off%s, %llu loops, gains %.4f/%.4f", k.name,
                  resident ? "resident" : "streamed", secs, c.max_err, (unsigned long long)c.bad,
                  c.bad ? (" from frame " + std::to_string(c.first_bad)).c_str() : "", (unsigned long long)t.loops,
                  c.k[0], c.k[1]);
        }
    }
}

// the amplitude of `hz` in a window of one channel (a single-bin DFT)
double amplitude(const float* x, size_t n, unsigned stride, double hz, unsigned rate)
{
    double re = 0, im = 0;
    for (size_t i = 0; i < n; i++) {
        const double a = 2 * 3.14159265358979 * hz * i / rate;
        re += x[i * stride] * std::cos(a);
        im += x[i * stride] * std::sin(a);
    }
    return 2 * std::sqrt(re * re + im * im) / n;
}

void synth_crossfade()
{
    printf("crossfade (two streams, 2 s: one to 0 and stopped, one from 0 to 1)\n");
    const unsigned rate = 48000;
    const std::string a = write_wav("xf_a", 2, rate, tone(2, rate, 4, 1000, 0.4));
    const std::string b = write_wav("xf_b", 2, rate, tone(2, rate, 4, 3000, 0.4));
    Loopback lb;
    if (!lb.open(rate)) {
        g_fail++;
        return;
    }
    al_stream::Config cfg;
    cfg.manual = true;
    cfg.clock = rendered_clock;
    g_rendered = 0;
    al_stream::Streamer st(lb.dev, cfg);
    const al_stream::Handle ha = st.open(a, al_stream::Loop::whole_file, 1.0f, true);
    const al_stream::Handle hb = st.open(b, al_stream::Loop::whole_file, 0.0f, true);
    st.service_once();
    const unsigned win = rate / 50;
    std::vector<float> out(2 * win);
    double worst_a = 0, worst_b = 0, a0 = 0, b1 = 0;
    bool a_stopped = false;
    for (int w = 0; w < 200; w++) {   // 4 s in 20 ms windows; the fades start at 1 s
        if (w == 50) {
            st.fade(ha, 0.0f, 2.0f, true);
            st.fade(hb, 1.0f, 2.0f, false);
            st.service_once();
        }
        lb.dev.render(out.data(), (int)win);
        g_rendered += (double)win / rate;
        st.service_once();
        const double ga = amplitude(out.data(), win, 2, 1000, rate), gb = amplitude(out.data(), win, 2, 3000, rate);
        if (w < 50) a0 = std::max(a0, ga);
        // the expected fade values at the window's middle (the gain moves within a window: half a step of error)
        const double t = (w + 0.5) * 0.02 - 1.0;
        const double ka = w < 50 ? 1.0 : std::max(0.0, 1.0 - t / 2.0), kb = w < 50 ? 0.0 : std::min(1.0, t / 2.0);
        if (w >= 50 && w < 150) {
            worst_a = std::max(worst_a, std::fabs(ga / 0.4 / 0.70711 - ka) * 0 + std::fabs(ga - ka * a0));
            worst_b = std::max(worst_b, std::fabs(gb - kb * a0));
        }
        if (w >= 160) b1 = std::max(b1, gb);
        if (w == 160) a_stopped = st.status(ha).state == al_stream::State::free;
    }
    // a0 is the full level (stereo direct: 0.4); the curves within 2% of full scale of the linear ramps
    CHECK(worst_a < 0.02 * a0 + 0.004 && worst_b < 0.02 * a0 + 0.004,
          "linear fades: the outgoing stream within %.4f of its ramp, the incoming within %.4f (full level %.3f)",
          worst_a, worst_b, a0);
    CHECK(a_stopped && std::fabs(b1 - a0) < 0.01 * a0, "the outgoing stream stopped and released at its fade's end; "
          "the incoming at full level (%.3f)", b1);
    st.shutdown();
    CHECK(!lb.dev.live().any(), "no AL object left");
}

void synth_spatial()
{
    printf("positional stereo and attenuation (inverse distance clamped: reference 2, maximum 30, rolloff 1)\n");
    const unsigned rate = 48000;
    const std::string path = write_wav("tone_mono", 1, rate, tone(1, rate, 1, 1000, 0.5));
    Loopback lb;
    if (!lb.open(rate)) {
        g_fail++;
        return;
    }
    const al_api::Api& al = lb.dev.al();
    al_device::Sample s;
    std::string err;
    al_device::load_sample(lb.dev, path, &s, &err);
    const unsigned src = lb.dev.new_source();
    al.alSourcei(src, AL_BUFFER, (ALint)s.buffer);
    al.alSourcei(src, AL_LOOPING, AL_TRUE);
    al.alSourcef(src, AL_REFERENCE_DISTANCE, 2.0f);
    al.alSourcef(src, AL_MAX_DISTANCE, 30.0f);
    al.alSourcef(src, AL_ROLLOFF_FACTOR, 1.0f);
    const float ori[6] = {0, 0, -1, 0, 1, 0};   // OpenAL's own frame: forward -z, up +y, right +x
    al.alListener3f(AL_POSITION, 0, 0, 0);
    al.alListenerfv(AL_ORIENTATION, ori);
    al.alSourcePlay(src);
    std::vector<float> out(2 * 9600);
    auto measure = [&](float x, float y, float z, double* l, double* r) {
        al.alSource3f(src, AL_POSITION, x, y, z);
        lb.dev.render(out.data(), 9600);   // let the gains settle (OpenAL Soft fades a change in)
        lb.dev.render(out.data(), 9600);
        *l = amplitude(out.data(), 9600, 2, 1000, rate);
        *r = amplitude(out.data() + 1, 9600, 2, 1000, rate);
    };
    double l, r, l0, r0;
    measure(0, 0, -2, &l0, &r0);
    CHECK(std::fabs(20 * std::log10(l0 / r0)) < 0.1, "ahead at the reference distance: balanced (L %.4f, R %.4f)", l0, r0);
    measure(2, 0, 0, &l, &r);
    CHECK(r > 2 * l, "to the right: right %.1f dB above left", 20 * std::log10(r / std::max(l, 1e-9)));
    measure(-2, 0, 0, &l, &r);
    CHECK(l > 2 * r, "to the left: left %.1f dB above right", 20 * std::log10(l / std::max(r, 1e-9)));
    const double ref = std::sqrt(l0 * l0 + r0 * r0);
    double worst = 0;
    for (float d : {1.0f, 2.0f, 3.0f, 5.0f, 10.0f, 20.0f, 30.0f, 45.0f, 100.0f}) {
        measure(0, 0, -d, &l, &r);
        const double got = 20 * std::log10(std::sqrt(l * l + r * r) / ref);
        const double dc = std::min(std::max((double)d, 2.0), 30.0);
        const double want = 20 * std::log10(2.0 / (2.0 + (dc - 2.0)));
        worst = std::max(worst, std::fabs(got - want));
    }
    CHECK(worst < 0.25, "level against distance 1-100 (clamped below 2 and above 30) within %.3f dB of 2/(2+(d-2))", worst);
    lb.dev.delete_source(const_cast<unsigned&>(src));
    al_device::free_sample(lb.dev, &s);
    CHECK(!lb.dev.live().any(), "no AL object left");
}

void synth_reverb()
{
    printf("EFX reverb (one slot; decay 1.0 s): a 50 ms burst, on and off\n");
    const unsigned rate = 48000;
    std::vector<int16_t> burst = ramp_frames(1, rate / 20);
    burst.resize(rate * 2, 0);
    const std::string path = write_wav("burst", 1, rate, burst);
    double tail[2] = {0, 0}, slope = 0;
    for (int on = 0; on < 2; on++) {
        Loopback lb;
        if (!lb.open(rate)) {
            g_fail++;
            return;
        }
        if (!lb.dev.caps().efx) {
            CHECK(false, "EFX available");
            return;
        }
        al_device::ReverbParams p;
        p.gain = 0.89f;
        p.gain_hf = 1.0f;
        p.decay_time = 1.0f;
        p.decay_hf_ratio = 0.5f;
        p.reflections_gain = 0.0f;
        p.reflections_delay = 0.02f;
        p.late_gain = 1.0f;
        p.late_delay = 0.04f;
        al_device::Reverb rv;
        CHECK(rv.create(lb.dev, p), "reverb created (%s)", lb.dev.caps().eax_reverb ? "EAX reverb" : "standard reverb");
        rv.enable(on);
        const al_api::Api& al = lb.dev.al();
        al_device::Sample s;
        std::string err;
        al_device::load_sample(lb.dev, path, &s, &err);
        unsigned src = lb.dev.new_source();
        al.alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
        al.alSourcei(src, AL_BUFFER, (ALint)s.buffer);
        rv.attach(src);
        al.alSourcePlay(src);
        std::vector<float> out(2 * rate * 2);
        lb.dev.render(out.data(), rate * 2);
        auto energy = [&](double t0, double t1) {
            double e = 0;
            for (size_t i = (size_t)(t0 * rate); i < (size_t)(t1 * rate); i++) e += out[2 * i] * out[2 * i] + out[2 * i + 1] * out[2 * i + 1];
            return e / ((t1 - t0) * rate);
        };
        const double eb = energy(0.0, 0.05);
        tail[on] = 10 * std::log10(std::max(energy(0.15, 0.45), 1e-30) / eb);
        if (on) slope = 10 * std::log10(energy(0.25, 0.45) / energy(0.65, 0.85)) / 0.4;   // dB per second
        lb.dev.delete_source(src);
        al_device::free_sample(lb.dev, &s);
        rv.destroy();
        CHECK(!lb.dev.live().any(), "no AL object left (reverb %s)", on ? "on" : "off");
    }
    CHECK(tail[0] < -90, "off: no tail after the burst (%.0f dB)", tail[0]);
    CHECK(tail[1] > -40, "on: a tail after the burst (%.1f dB against the burst)", tail[1]);
    const double t60 = slope > 0 ? 60.0 / slope : 0;
    CHECK(t60 > 0.5 && t60 < 2.0, "on: the tail decays at %.1f dB/s, a decay time of %.2f s (set: 1.0 s)", slope, t60);
}

void synth_queue()
{
    printf("the command queue's bound (capacity 16, nothing serviced)\n");
    Loopback lb;
    if (!lb.open(48000)) {
        g_fail++;
        return;
    }
    const std::string path = write_wav("q", 1, 48000, tone(1, 48000, 1, 440, 0.2));
    al_stream::Config cfg;
    cfg.manual = true;
    cfg.queue_capacity = 16;
    al_stream::Streamer st(lb.dev, cfg);
    const al_stream::Handle h = st.open(path, al_stream::Loop::whole_file, 1.0f, true);
    for (int k = 0; k < 40; k++) st.set_gain(h, k / 40.0f);
    const al_stream::Totals t = st.totals();
    CHECK(t.queue_overflows == 25, "41 commands into 16 places: %llu refused and counted, none blocked",
          (unsigned long long)t.queue_overflows);
    st.service_once();
    CHECK(st.status(h).state == al_stream::State::playing, "the queued ones applied (%s)",
          al_stream::state_name(st.status(h).state));
    st.stop(h);
    st.service_once();
    CHECK(st.status(h).state == al_stream::State::free, "a stopped stream's handle is stale");
    st.set_gain(h, 1.0f);
    st.service_once();
    CHECK(st.totals().stale_commands == 1, "a command for a stale handle ignored and counted");
    st.shutdown();
    CHECK(!lb.dev.live().any(), "no AL object left");
}

void synth_lifecycle()
{
    printf("lifecycle: open/close cycles, thread start/stop, failures\n");
    const std::string a = write_wav("lc_a", 2, 44100, tone(2, 44100, 2, 440, 0.2));
    const std::string b = write_wav("lc_b", 1, 44100, tone(1, 44100, 1, 660, 0.2), 1000, 44100);
    Loopback lb;
    if (!lb.open(48000)) {
        g_fail++;
        return;
    }
    al_device::Live worst;
    for (int cycle = 0; cycle < 100; cycle++) {   // threaded: the audio thread started and ended each time
        al_stream::Config cfg;
        cfg.period_ms = 2;
        al_stream::Streamer st(lb.dev, cfg);
        al_stream::Handle h[3] = {st.open(a, al_stream::Loop::whole_file, 1, true),
                                  st.open(b, al_stream::Loop::file_loop_points, 1, true),
                                  st.open(a, al_stream::Loop::once, 1, false)};
        if (cycle % 2) st.stop(h[0]);
        st.fade(h[1], 0.0f, 0.001f, true);
        st.sync();
        std::vector<float> out(2 * 480);
        lb.dev.render(out.data(), 480);
        st.sync();
        // shutdown with streams still open (h[2], maybe h[0]): released by it
        st.shutdown();
        const al_device::Live l = lb.dev.live();
        if (l.any()) worst = l;
        (void)h;
    }
    CHECK(!worst.any(), "100 streamer lifetimes (threads started, ended): no AL object left");
    for (int k = 0; k < 300; k++) {
        al_device::Sample s;
        std::string err;
        al_device::load_sample(lb.dev, k % 2 ? a : b, &s, &err);
        al_device::free_sample(lb.dev, &s);
    }
    CHECK(!lb.dev.live().any(), "300 samples loaded and freed: no AL object left");
    al_stream::Config cfg;
    cfg.manual = true;
    al_stream::Streamer st(lb.dev, cfg);
    const al_stream::Handle bad = st.open("no/such/file.wav", al_stream::Loop::once, 1, true);
    st.service_once();
    CHECK(st.status(bad).state == al_stream::State::failed && st.totals().open_failures == 1,
          "a missing file: the stream fails (%s), counted, nothing held", al_stream::state_name(st.status(bad).state));
    st.stop(bad);
    st.service_once();
    al_stream::Handle hs[9];
    for (int k = 0; k < 9; k++) hs[k] = st.open(a, al_stream::Loop::whole_file, 1, false);
    CHECK(hs[8] == al_stream::kNone, "a ninth stream refused (8 at most), the others given");
    st.shutdown();
    CHECK(!lb.dev.live().any(), "no AL object left");
    for (int k = 0; k < 20; k++) {
        Loopback again;
        again.open(44100);
    }
    CHECK(true, "20 device opens and closes");
}

void synth_voices()
{
    printf("effect voices (32 real; priorities; virtual channels; stale handles)\n");
    Loopback lb;
    if (!lb.open(48000)) {
        g_fail++;
        return;
    }
    const std::string one = write_wav("v_short", 1, 48000, tone(1, 48000, 0.2, 500, 0.1));
    const std::string loop = write_wav("v_loop", 1, 48000, tone(1, 48000, 0.5, 700, 0.1), 0, 24000);
    al_device::Sample s1, s2;
    std::string err;
    al_device::load_sample(lb.dev, one, &s1, &err);
    al_device::load_sample(lb.dev, loop, &s2, &err);
    voice_pool::Pool pool;
    CHECK(pool.create(lb.dev, 32, 256, nullptr), "32 voices made");
    std::vector<voice_pool::Handle> low, high;
    voice_pool::ChannelInfo li;
    li.looping = true;
    li.length = 0.5;
    li.priority = 200;
    for (int k = 0; k < 32; k++) low.push_back(pool.play(s2.buffer, li));
    voice_pool::ChannelInfo hi;
    hi.length = 0.2;
    hi.priority = 10;
    for (int k = 0; k < 4; k++) high.push_back(pool.play(s1.buffer, hi));
    int low_real = 0;
    for (auto h : low) low_real += pool.real(h);
    CHECK(pool.stats().steals == 4 && low_real == 28 && pool.real(high[3]),
          "4 important one-shots take 4 of the 32 voices from looping low-priority channels (%llu steals; %d of 32 "
          "loops still real)", (unsigned long long)pool.stats().steals, low_real);
    voice_pool::ChannelInfo lower = li;
    lower.priority = 250;
    const voice_pool::Handle v = pool.play(s2.buffer, lower);
    CHECK(pool.valid(v) && !pool.real(v), "a less important channel when all voices are busy starts virtual");
    std::vector<float> out(2 * 4800);
    for (int k = 0; k < 10; k++) {   // 1 s: the one-shots end, their voices go back to virtual channels
        lb.dev.render(out.data(), 4800);
        pool.update(0.1);
    }
    low_real = 0;
    for (auto h : low) low_real += pool.real(h);
    CHECK(!pool.valid(high[0]) && low_real == 32 && pool.stats().promotions >= 4,
          "the one-shots ended (handles stale); the virtual loops back on voices (%llu promotions, %d of 32 real)",
          (unsigned long long)pool.stats().promotions, low_real);
    pool.stop(high[0]);   // a stale handle: nothing happens
    pool.stop(low[0]);
    CHECK(!pool.valid(low[0]), "a stopped channel's handle is stale");
    pool.stop_all();
    pool.destroy();
    al_device::free_sample(lb.dev, &s1);
    al_device::free_sample(lb.dev, &s2);
    CHECK(!lb.dev.live().any(), "no AL object left");
}

int cmd_synth()
{
    synth_loops();
    synth_crossfade();
    synth_spatial();
    synth_reverb();
    synth_queue();
    synth_lifecycle();
    synth_voices();
    printf("synth: %d failed\n", g_fail);
    return g_fail ? 1 : 0;
}

// ---- device ----

int cmd_device(const std::string& path, double seconds, bool reopen)
{
    al_device::Device dev;
    std::string err;
    if (!dev.open(&g_al, al_device::Options{}, al_device::log_stderr, &err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    al_stream::Config cfg;
    al_stream::Streamer st(dev, cfg);
    const al_stream::Handle h = st.open(path, al_stream::Loop::file_loop_points, 0.5f, true);
    const auto t0 = std::chrono::steady_clock::now();
    double last_pos = 0, before = 0;
    bool reopened = false, advancing_after = false;
    unsigned not_playing = 0;
    for (int s = 1; s <= (int)seconds; s++) {
        std::this_thread::sleep_until(t0 + std::chrono::seconds(s));
        const al_stream::Status stt = st.status(h);
        if (stt.state != al_stream::State::playing) not_playing++;
        if (reopen && !reopened && s >= seconds / 2) {
            before = stt.position;
            st.debug_simulate_disconnect();   // what a disconnect does: the sources stop, the device is lost
            reopened = true;
        } else if (reopened && !advancing_after && s > seconds / 2 + 2) {
            advancing_after = stt.position != last_pos;
        }
        last_pos = stt.position;
        if (s % 600 == 0) {
            const al_stream::Totals t = st.totals();
            printf("  %d s: underruns %llu, near %llu, worst period %llu us, loops %llu\n", s,
                   (unsigned long long)t.underruns, (unsigned long long)t.near_underruns,
                   (unsigned long long)t.service_us_max, (unsigned long long)t.loops);
            fflush(stdout);
        }
    }
    const al_stream::Totals t = st.totals();
    const al_device::Stats ds = dev.stats();
    st.shutdown();
    const al_device::Live l = dev.live();
    printf("%s: %.0f s on %s: %llu underruns, %llu near-underruns, %llu loops, worst period %llu us over %llu; "
           "device: %llu lost, %llu reopened, %llu reopen failures\n", base(path).c_str(), seconds, dev.name().c_str(),
           (unsigned long long)t.underruns, (unsigned long long)t.near_underruns, (unsigned long long)t.loops,
           (unsigned long long)t.service_us_max, (unsigned long long)t.services, (unsigned long long)ds.disconnects,
           (unsigned long long)ds.reopens, (unsigned long long)ds.reopen_failures);
    CHECK(t.underruns == 0 && t.read_errors == 0, "no underrun, no read error");
    CHECK(not_playing == 0, "the stream playing at every check");
    if (reopen)
        CHECK(ds.reopens == 1 && t.restarts_after_reopen == 1 && advancing_after && t.underruns == 0,
              "the device lost at %.0f s (position %.1f s; its sources stopped), reopened, the stream restarted (%llu) "
              "and advancing", seconds / 2, before, (unsigned long long)t.restarts_after_reopen);
    CHECK(!l.any(), "shutdown: no AL object left");
    dev.close();
    return g_fail ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: openal_check files <dir> | loops <dir> | stream <wav> <s> <mode> [ms [n]] | synth | "
                        "device <wav> <s> [reopen]\n");
        return 2;
    }
    if (!load_library()) return 2;
    const std::string cmd = argv[1];
    if (cmd == "files" && argc > 2) return cmd_files(argv[2]);
    if (cmd == "loops" && argc > 2) return cmd_loops(argv[2]);
    if (cmd == "stream" && argc > 4)
        return cmd_stream(argv[2], atof(argv[3]), argv[4], argc > 5 ? (unsigned)atoi(argv[5]) : 250,
                          argc > 6 ? (unsigned)atoi(argv[6]) : 4);
    if (cmd == "synth") return cmd_synth();
    if (cmd == "device" && argc > 3) return cmd_device(argv[2], atof(argv[3]), argc > 4 && !strcmp(argv[4], "reopen"));
    fprintf(stderr, "unknown command\n");
    return 2;
}
