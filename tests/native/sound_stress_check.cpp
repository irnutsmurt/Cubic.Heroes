// The native sound manager under pressure, paused and through device losses (M4's automated part,
// docs/OPENAL_MIGRATION_PLAN.md section 4.5), on made-up sounds -- no game: a sound folder written here (a
// sound.lua.eu and tones, each sound its own frequency), the manager driven through native_sound.h as the game's
// hooks drive it. Each mode is one process (the manager is one per process):
//   sound_stress_check voices    a loopback device, its output captured and measured: more effects than voices,
//                                priorities, audibility, virtual channels, stale handles, loops under pressure,
//                                recovery, the channel limit, music never touched; then a 5-minute random soak
//   sound_stress_check pause     loopback: the game's pause (the system menu's flag) with music, loops, one-shots,
//                                fades, ducks, a crossfade and virtual channels; 200 cycles; quitting while paused
//   sound_stress_check device    loopback: device losses and reopens (some failing), output measured: what resumes,
//                                where, at what volume; a loss while paused; repeated losses
//   sound_stress_check realtime [losses]   the real-time path (OpenAL Soft's mixer thread through the wave writer,
//                                the music's audio thread): repeated losses (some reopens failing) at random
//                                moments, the game's thread timed, threads and AL objects counted
// (the library: OpenAL32.dll beside this program, or OPENAL_LIB; ALSOFT_CONF: the wave writer for realtime)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#endif

#include "native_sound.h"

namespace {

int g_fail = 0;
#define CHECK(cond, ...)                                                                                            \
    do {                                                                                                            \
        const bool ok_ = (cond);                                                                                    \
        printf(ok_ ? "  PASS " : "  FAIL ");                                                                        \
        if (!ok_) g_fail++;                                                                                         \
        printf(__VA_ARGS__);                                                                                        \
        printf("\n");                                                                                               \
    } while (0)

constexpr double kPi = 3.14159265358979;
constexpr int kRate = 48000;
constexpr float kDt = 1.0f / 60.0f;
constexpr double kAmp = 1000.0 / 32768.0;   // every tone's amplitude in its file (low: no limiter in any mix)
std::string g_dir;
double g_t = 0.0;   // the game's time: the dt given to updates, summed

// ---- the sound folder ----

void write_wav(const std::string& name, unsigned ch, unsigned rate, double seconds, double hz,
               double loop_start = -1, double loop_end = -1)
{
    const size_t n = (size_t)(seconds * rate);
    std::vector<int16_t> pcm(n * ch);
    for (size_t i = 0; i < n; i++)
        for (unsigned c = 0; c < ch; c++) pcm[i * ch + c] = (int16_t)std::lround(1000 * std::sin(2 * kPi * hz * i / rate));
    FILE* f = fopen((g_dir + "/" + name).c_str(), "wb");
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    const bool loop = loop_end > 0;
    const uint32_t data = (uint32_t)(pcm.size() * 2), smpl = loop ? 60 : 0;
    fwrite("RIFF", 1, 4, f);
    u32(4 + 24 + 8 + data + (smpl ? 8 + smpl : 0));
    fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16((uint16_t)ch); u32(rate); u32(rate * ch * 2); u16((uint16_t)(ch * 2)); u16(16);
    fwrite("data", 1, 4, f);
    u32(data);
    fwrite(pcm.data(), 2, pcm.size(), f);
    if (smpl) {
        fwrite("smpl", 1, 4, f);
        u32(smpl);
        for (int k = 0; k < 7; k++) u32(0);
        u32(1); u32(0); u32(0); u32(0);
        u32((uint32_t)std::lround(loop_start * rate)); u32((uint32_t)std::lround(loop_end * rate) - 1); u32(0); u32(0);
    }
    fclose(f);
}

// music: 0 (220 Hz, 4 s, looped 1-4 s), 1 (330 Hz, 3 s, looped whole); effects: 42-151 one-shots, 3 s, each its own
// frequency and a volume falling with the id; 160 a quiet loop (0.5-2 s of 2 s); 161 a 3D effect (min 2, max 30);
// 162 a 0.25 s blip; 163 a loop (whole 1 s file); 164 a loud loop (whole 1 s) for fade-ins
constexpr int kFirst = 42, kOneShots = 110;
double one_shot_hz(int id) { return 1000.0 + 25.0 * (id - kFirst); }
float one_shot_vol(int id) { return 1.0f - 0.004f * (float)(id - kFirst); }
float se_gain(float vol, int level = 3) { return (float)level * (vol * 0.11f); }

void make_folder()
{
    const char* tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMPDIR");
    g_dir = std::string(tmp ? tmp : ".") + "/sound_stress_check";
#ifdef _WIN32
    system(("mkdir \"" + g_dir + "\" 2>NUL").c_str());
#else
    system(("mkdir -p '" + g_dir + "'").c_str());
#endif
    write_wav("m0.wav", 2, kRate, 4.0, 220, 1.0, 4.0);
    write_wav("m1.wav", 2, kRate, 3.0, 330, 0.0, 3.0);
    for (int k = 0; k < kOneShots; k++) {
        char n[32];
        snprintf(n, sizeof n, "se%03d.wav", kFirst + k);
        write_wav(n, 1, 24000, 3.0, one_shot_hz(kFirst + k));
    }
    write_wav("amb160.wav", 1, kRate, 2.0, 300, 0.5, 2.0);
    write_wav("se161.wav", 1, kRate, 4.0, 600);
    write_wav("se162.wav", 1, kRate, 0.25, 700);
    write_wav("amb163.wav", 1, kRate, 1.0, 450, 0.0, 1.0);
    write_wav("amb164.wav", 1, kRate, 1.0, 800, 0.0, 1.0);
    std::string list = "Sound =\n{\n\tBGM_FadeTime = 2.0,\n\tList =\n\t{\n";
    for (int id = 0; id <= 323; id++) {
        std::string f;
        float vol = 1.0f, mn = 0, mx = 0;
        int d3 = 0;
        char n[32];
        if (id == 0) f = "m0.wav", vol = 0.5f;
        else if (id == 1) f = "m1.wav", vol = 0.5f;
        else if (id >= kFirst && id < kFirst + kOneShots) snprintf(n, sizeof n, "se%03d.wav", id), f = n, vol = one_shot_vol(id);
        else if (id == 160) f = "amb160.wav", vol = 0.3f;
        else if (id == 161) f = "se161.wav", vol = 1.0f, d3 = 1, mn = 2, mx = 30;
        else if (id == 162) f = "se162.wav", vol = 0.8f;
        else if (id == 163) f = "amb163.wav", vol = 0.6f;
        else if (id == 164) f = "amb164.wav", vol = 1.0f;
        char b[200];
        snprintf(b, sizeof b, "\t\t{\"%s\", %.4f, %d, %.1f, %.1f},\n", f.c_str(), vol, d3, mn, mx);
        list += b;
    }
    list += "\t}\n}\n";
    FILE* f = fopen((g_dir + "/sound.lua.eu").c_str(), "wb");
    fwrite(list.data(), 1, list.size(), f);
    fclose(f);
}

// ---- the output, captured (loopback) ----

struct Capture {
    std::vector<float> mono;   // (left + right) / 2 since mark()
    std::vector<float> left, right;
} g_cap;

void capture(const float* lr, int n, void*)
{
    for (int i = 0; i < n; i++) {
        g_cap.mono.push_back(0.5f * (lr[2 * i] + lr[2 * i + 1]));
        g_cap.left.push_back(lr[2 * i]);
        g_cap.right.push_back(lr[2 * i + 1]);
    }
}
void mark() { g_cap.mono.clear(); g_cap.left.clear(); g_cap.right.clear(); }

// a tone's amplitude in the mono mix between a and b seconds after mark() (Hann window, Goertzel)
double tone(double hz, double a, double b)
{
    const size_t i0 = (size_t)(a * kRate), i1 = std::min(g_cap.mono.size(), (size_t)(b * kRate));
    if (i1 <= i0 + 16) return 0.0;
    const double w = 2 * kPi * hz / kRate, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0, sw = 0;
    const size_t n = i1 - i0;
    for (size_t i = 0; i < n; i++) {
        const double win = 0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)(n - 1));
        const double s0 = g_cap.mono[i0 + i] * win + c * s1 - s2;
        s2 = s1;
        s1 = s0;
        sw += win;
    }
    return 2.0 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / sw;
}
double rms(double a, double b)
{
    const size_t i0 = (size_t)(a * kRate), i1 = std::min(g_cap.mono.size(), (size_t)(b * kRate));
    double s = 0;
    for (size_t i = i0; i < i1; i++) s += (double)g_cap.left[i] * g_cap.left[i] + (double)g_cap.right[i] * g_cap.right[i];
    return i1 > i0 ? std::sqrt(s / (2.0 * (double)(i1 - i0))) : 0.0;
}
double db(double x, double ref) { return 20.0 * std::log10(std::max(x, 1e-12) / std::max(ref, 1e-12)); }
// an effect's tone in the mono mix when real: its file's amplitude x its gain x the centred level
double se_expect(float gain) { return kAmp * gain * 0.70710678; }

// ---- driving ----

void frames(int n)
{
    for (int k = 0; k < n; k++) {
        nsm_update(kDt);
        g_t += kDt;
    }
}
nsm_channel_debug dbg(uint32_t h)
{
    nsm_channel_debug d{};
    nsm_debug_channel(h, &d);
    return d;
}
nsm_stats stats()
{
    nsm_stats s{};
    nsm_get_stats(&s);
    return s;
}
// where a looping sound is after t seconds: the first pass to its loop end, then around the loop
double fold(double t, double ls, double le) { return t < le ? t : ls + std::fmod(t - le, le - ls); }
uint32_t start_music(int id)
{
    nsm_play_bgm(id, 128, 1);
    for (int k = 0; k < 10; k++) {
        frames(1);
        if (const uint32_t h = nsm_debug_last_bgm_channel()) return h;
    }
    return 0;
}

int thread_count()
{
#ifdef _WIN32
    const DWORD pid = GetCurrentProcessId();
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    int n = 0;
    if (Thread32First(s, &te)) do {
            if (te.th32OwnerProcessID == pid) n++;
        } while (Thread32Next(s, &te));
    CloseHandle(s);
    return n;
#else
    int n = 0;
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d)) n += e->d_name[0] != '.';
        closedir(d);
    }
    return n;
#endif
}

uint64_t g_rng = 0x2545F4914F6CDD1DULL;
uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 11); }
double urand() { return (rnd() & 0xFFFFFF) / (double)0x1000000; }

bool init(bool loopback)
{
    nsm_options o{};
    o.sound_dir = g_dir.c_str();
    o.loopback = loopback ? 1 : 0;
    o.log = [](const char* line) { if (getenv("STRESS_LOG")) printf("    [audio] %s\n", line); };
    if (!nsm_init(&o)) return false;
    if (loopback) nsm_debug_capture(capture, nullptr);
    nsm_set_game_state(3, 3, 0, 0);
    nsm_load_sounds();
    const float origin[3] = {0, 0, 0};
    nsm_set_listener(origin, nullptr);
    return true;
}

void shutdown_clean(const char* what)
{
    const auto t0 = std::chrono::steady_clock::now();
    nsm_shutdown();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const nsm_stats s = stats();
    CHECK(s.live_sources == 0 && s.live_buffers == 0 && ms < 500, "%s: shutdown in %.1f ms, nothing left (%d sources, %d buffers)",
          what, ms, s.live_sources, s.live_buffers);
}

// ---- voices ----

void voices()
{
    printf("voices: more effects than voices\n");
    const uint32_t mus = start_music(0);
    frames(30);
    const float mgain = dbg(mus).gain;
    mark();
    frames(30);
    const double m_base = tone(220, 0.0, 0.5);
    const nsm_stats base = stats();
    const uint32_t amb = nsm_play_se(160, 128, nullptr);
    frames(6);
    const double t_amb = g_t - 6 * kDt;
    CHECK(mus && dbg(amb).real, "music playing, the quiet loop on a voice");
    std::vector<uint32_t> h(kOneShots, 0);
    const double t_burst = g_t;
    for (int k = 0; k < 40; k++) h[k] = nsm_play_se(kFirst + k, 128, nullptr);
    auto reals = [&](int from, int to) {   // ids [from, to) on a voice
        int n = 0;
        for (int id = from; id < to; id++) n += dbg(h[id - kFirst]).real;
        return n;
    };
    nsm_stats s = stats();
    CHECK(reals(42, 74) == 32 && reals(74, 82) == 0 && !dbg(amb).real && dbg(amb).exists,
          "40 effects on 32 voices: the 32 most audible real (ids 42-73), the quiet loop and the 8 quietest virtual");
    CHECK(s.steals - base.steals == 1 && s.virtual_starts - base.virtual_starts == 8 && s.peak_voices == 32,
          "the 32nd took the quiet loop's voice (steals %llu), 8 started virtual, peak %d voices",
          (unsigned long long)(s.steals - base.steals), s.peak_voices);
    bool all_playing = nsm_is_playing(160) != 0;
    for (int id = 42; id < 82; id++) all_playing = all_playing && nsm_is_playing(id);
    CHECK(all_playing, "isPlaying answers for every one, virtual ones included");
    frames(1);
    CHECK(reals(42, 74) == 32 && reals(74, 82) == 0 && stats().swaps == base.swaps,
          "an update keeps them (no virtual one clearly more audible than a real one)");
    mark();
    frames(18);
    int heard = 0, silent = 0;
    for (int id = 42; id < 82; id++) {
        const double a = tone(one_shot_hz(id), 0.0, 0.3), e = se_expect(se_gain(one_shot_vol(id)));
        if (id < 74) heard += a > 0.7 * e && a < 1.3 * e;
        else silent += a < 0.03 * e;
    }
    CHECK(heard == 32 && silent == 8, "in the output: the 32 real ones at their level (%d), the virtual ones silent (%d)", heard, silent);
    const double m_now = tone(220, 0.0, 0.3);
    CHECK(std::fabs(db(m_now, m_base)) < 0.2 && dbg(mus).gain == mgain && dbg(mus).real && stats().stream_underruns == 0,
          "the music untouched: %.2f dB against before, its gain and stream unchanged, no underrun", db(m_now, m_base));

    printf("voices: priorities\n");
    const uint32_t h82 = nsm_play_se(82, 64, nullptr), h83 = nsm_play_se(83, 200, nullptr), h84 = nsm_play_se(84, 128, nullptr);
    CHECK(dbg(h82).real && !dbg(h[73 - kFirst]).real, "a more important effect (priority 64) takes the least audible real voice (id 73's)");
    CHECK(!dbg(h83).real && dbg(h83).exists, "a less important one (priority 200), loud, starts virtual");
    CHECK(!dbg(h84).real && dbg(h84).exists, "an equally important one quieter than every real one starts virtual");
    const uint64_t swaps0 = stats().swaps;
    nsm_set_sound_volume(72, 0.1f);
    nsm_apply_volume_options();
    frames(1);
    const nsm_channel_debug d73 = dbg(h[73 - kFirst]);
    CHECK(d73.real && !dbg(h[72 - kFirst]).real && stats().swaps - swaps0 == 1,
          "id 72 turned down: the most audible virtual one (73) takes its voice at the next update (1 swap)");
    CHECK(std::fabs(d73.offset - (g_t - t_burst)) < 0.03, "  and goes on where its time is: %.3f s (its time %.3f s)", d73.offset, g_t - t_burst);

    printf("voices: recovery\n");
    const uint64_t prom0 = stats().promotions;
    for (int id = 42; id < 54; id++) nsm_stop_se(id);
    frames(1);
    bool all_real = dbg(amb).real && dbg(h83).real && dbg(h84).real && dbg(h[72 - kFirst]).real;
    for (int id = 74; id < 82; id++) all_real = all_real && dbg(h[id - kFirst]).real;
    CHECK(all_real && stats().promotions - prom0 == 12, "12 stopped: the 12 waiting get the voices at the next update (%llu)",
          (unsigned long long)(stats().promotions - prom0));
    double worst = 0;
    for (int id = 74; id < 82; id++) worst = std::max(worst, std::fabs(dbg(h[id - kFirst]).offset - (g_t - t_burst)));
    const double amb_off = dbg(amb).offset, amb_want = fold(g_t - t_amb, 0.5, 2.0);
    CHECK(worst < 0.03 && std::fabs(amb_off - amb_want) < 0.03,
          "each where its time is: one-shots within %.3f s, the loop at %.3f s of its loop (want %.3f)", worst, amb_off, amb_want);
    mark();
    frames(18);
    const double a_amb = tone(300, 0.0, 0.3), e_amb = se_expect(se_gain(0.3f));
    CHECK(a_amb > 0.7 * e_amb && a_amb < 1.3 * e_amb, "the loop heard again at its level (%.2f dB)", db(a_amb, e_amb));

    printf("voices: ends and stale handles\n");
    std::vector<double> ended(kOneShots, -1);
    for (int n = 0; n < 60 * 4 && g_t - t_burst < 3.3; n++) {
        frames(1);
        for (int id = 54; id < 82; id++)
            if (id != 72 && ended[id - kFirst] < 0 && !nsm_is_playing(id)) ended[id - kFirst] = g_t - t_burst;
    }
    double worst_end = 0;
    int counted = 0;
    for (int id = 54; id < 82; id++)
        if (id != 72 && ended[id - kFirst] > 0) worst_end = std::max(worst_end, std::fabs(ended[id - kFirst] - 3.0)), counted++;
    CHECK(counted == 27 && worst_end <= kDt + 1e-6, "27 effects (always real, stolen, promoted) end at their 3 s within a frame (worst %.4f s)", worst_end);
    bool stale = true;
    for (int id = 54; id < 82; id++)
        if (id != 72) stale = stale && !dbg(h[id - kFirst]).exists && nsm_stop_channel(h[id - kFirst]) == 2;
    CHECK(stale, "their handles are stale: unknown, stopChannel answers 2");
    const uint32_t b1 = nsm_play_se(162, 128, nullptr);
    while (nsm_is_playing(162)) frames(1);
    const uint32_t b2 = nsm_play_se(162, 128, nullptr);
    CHECK(b2 && b2 != b1 && nsm_stop_channel(b1) == 2 && nsm_is_playing(162),
          "a slot used again gets a new handle; the old one stops nothing");
    nsm_stop_se(162);
    nsm_stop_se(72);
    nsm_set_sound_volume(72, one_shot_vol(72));
    nsm_stop_se(82);
    nsm_stop_se(83);
    nsm_stop_se(84);
    nsm_stop_se(160);
    frames(2);

    printf("voices: a fade-in under pressure, distance\n");
    for (int id = 90; id < 122; id++) {
        nsm_set_sound_volume(id, 0.25f);
        h[id - kFirst] = nsm_play_se(id, 128, nullptr);
    }
    const uint32_t fi = nsm_play_se_fade_in(164, 128, 1.0f);
    const double t_fi = g_t;
    CHECK(fi && !dbg(fi).real && reals(90, 122) == 32, "32 quiet effects real; a fade-in (silent at first) starts virtual");
    int real_at = -1;
    for (int n = 1; n <= 90 && real_at < 0; n++) {
        frames(1);
        if (dbg(fi).real) real_at = n;
    }
    // the fade: pending one update, then 0.33 x t; it takes a voice past 1.2 x 0.0825 (t > 0.3 s)
    CHECK(real_at >= 18 && real_at <= 22, "it takes a voice as it rises past the quiet ones (update %d, about 20)", real_at);
    const double fi_off = dbg(fi).offset, fi_want = fold(g_t - t_fi, 0.0, 1.0);
    CHECK(std::fabs(fi_off - fi_want) < 0.03, "  where its time is in its loop: %.3f s (want %.3f)", fi_off, fi_want);
    nsm_stop_se(164);
    frames(1);
    const uint32_t h3d_dummy = nsm_play_se(122, 128, nullptr);   // all 32 voices busy again
    (void)h3d_dummy;
    nsm_set_sound_volume(122, 0.25f);
    nsm_apply_volume_options();
    frames(1);
    const float far_pos[3] = {0, 0, 25};
    const uint32_t s3 = nsm_play_se(161, 128, far_pos);
    CHECK(s3 && !dbg(s3).real, "a loud 3D effect 25 away (min 2): audibility %.4f, below the quiet ones (0.0825): virtual", dbg(s3).audibility);
    const float close_by[3] = {0, 0, 24}, origin[3] = {0, 0, 0};
    nsm_set_listener(close_by, nullptr);
    frames(1);
    CHECK(dbg(s3).real, "the listener walks up to it: it takes a voice at the next update (audibility %.3f)", dbg(s3).audibility);
    nsm_set_listener(origin, nullptr);
    frames(1);
    CHECK(!dbg(s3).real && dbg(s3).exists, "and walks away: it gives the voice back");
    for (int id = 90; id < 123; id++) nsm_stop_se(id), nsm_set_sound_volume(id, one_shot_vol(id));
    nsm_stop_se(161);
    frames(2);

    printf("voices: the channel limit (100)\n");
    const nsm_stats c0 = stats();
    for (int id = 42; id < 142; id++) h[id - kFirst] = nsm_play_se(id, 128, nullptr);
    const uint32_t h142 = nsm_play_se(142, 128, nullptr);
    CHECK(stats().peak_channels == 100 && h142 == 0 && stats().refused - c0.refused == 1 && !nsm_is_playing(142),
          "100 channels in use: a 101st less audible than all is refused (handle 0)");
    const uint32_t h143 = nsm_play_se(143, 64, nullptr);
    CHECK(h143 && dbg(h143).real && !nsm_is_playing(141) && !dbg(h[141 - kFirst]).exists &&
              stats().channel_steals - c0.channel_steals == 1,
          "a more important one stops the least important channel (id 141: its handle stale) and takes a voice");
    for (int id = 42; id < 144; id++) nsm_stop_se(id);
    frames(2);

    printf("voices: a 5-minute soak (random plays, stops, pauses, fades, positions; checked every update)\n");
    const nsm_stats k0 = stats();
    std::vector<uint32_t> cur(400, 0), old;
    int bad_count = 0, bad_order = 0, bad_music = 0, bad_stale = 0, bad_live = 0, seconds = 0;
    double music_worst = 0;
    mark();
    for (int n = 0; n < 60 * 300; n++) {
        const double r = urand();
        if (r < 0.25) {
            int id = kFirst + (int)(rnd() % kOneShots);
            const uint32_t pick = rnd() % 12;
            if (pick == 0) id = 160;
            else if (pick == 1) id = 161;
            else if (pick == 2) id = 163;
            else if (pick == 3) id = 164;
            const int pr[5] = {64, 128, 128, 128, 200};
            const int prio = pr[rnd() % 5];
            if (cur[(size_t)id]) old.push_back(cur[(size_t)id]);
            if (id == 161) {
                const float p[3] = {(float)(urand() * 80 - 40), 0, (float)(urand() * 80 - 40)};
                cur[(size_t)id] = nsm_play_se(id, prio, p);
            } else if (urand() < 0.1) {
                cur[(size_t)id] = nsm_play_se_fade_in(id, prio, (float)(0.2 + urand() * 1.3));
            } else {
                cur[(size_t)id] = nsm_play_se(id, prio, nullptr);
            }
        } else if (r < 0.30) {
            nsm_stop_se(kFirst + (int)(rnd() % kOneShots));
        } else if (r < 0.33 && !old.empty()) {
            const uint32_t stale_h = old[rnd() % old.size()];
            if (!dbg(stale_h).exists && nsm_stop_channel(stale_h) != 2) bad_stale++;
        } else if (r < 0.35) {
            nsm_pause_by_id(kFirst + (int)(rnd() % kOneShots), (int)(rnd() & 1));
        } else if (r < 0.36) {
            nsm_fade_out_se(160 + (int)(rnd() % 5), 0.5f);
        } else if (r < 0.362) {
            nsm_duck_se(0.3f);
        } else if (r < 0.364) {
            nsm_unduck_se(0.3f);
        } else if (r < 0.37) {
            const float p[3] = {(float)(urand() * 40 - 20), 0, (float)(urand() * 40 - 20)};
            nsm_set_listener(p, nullptr);
        }
        if (old.size() > 256) old.erase(old.begin(), old.begin() + 128);
        const bool pause_now = (n % 1200) >= 1170;   // the game's pause, half a second every 20 s
        nsm_set_game_state(3, 3, pause_now ? 1 : 0, 0);
        frames(1);
        // after the update: at most 32 real, at most 100 channels, no virtual channel more important than a real one
        int real = 0, live = 0, max_real_prio = -1, min_virtual_prio = 1 << 30;
        for (int id = 0; id < 400; id++) {
            if (!cur[(size_t)id]) continue;
            const nsm_channel_debug d = dbg(cur[(size_t)id]);
            if (!d.exists) continue;
            live++;
            if (d.real) real++, max_real_prio = std::max(max_real_prio, d.priority);
            else min_virtual_prio = std::min(min_virtual_prio, d.priority);
        }
        if (real > 32 || live > 100) bad_count++;
        if (!pause_now && min_virtual_prio < max_real_prio) bad_order++;
        if (!nsm_is_bgm_active() || dbg(mus).gain != mgain || !dbg(mus).real) bad_music++;
        if (n % 60 == 59) {
            const nsm_stats s2 = stats();
            if (s2.live_sources != base.live_sources || s2.live_buffers != base.live_buffers) bad_live++;
            if (!pause_now && (n % 1200) < 1100) {
                const double a = tone(220, 0.05, 0.95);
                music_worst = std::max(music_worst, std::fabs(db(a, m_base)));
                seconds++;
            }
            mark();
        }
    }
    const nsm_stats k1 = stats();
    CHECK(bad_count == 0, "never more than 32 voices or 100 channels (%d updates over)", bad_count);
    CHECK(bad_order == 0, "after every update no virtual channel more important than a real one (%d updates not)", bad_order);
    CHECK(bad_stale == 0, "stale handles: stopChannel always 2, nothing else stopped (%d wrong)", bad_stale);
    CHECK(bad_live == 0, "AL objects constant every second (%d sources, %d buffers)", base.live_sources, base.live_buffers);
    CHECK(bad_music == 0 && music_worst < 0.25 && k1.stream_underruns == 0,
          "the music never interrupted: active, its gain unchanged, %d one-second windows within %.2f dB, no underrun",
          seconds, music_worst);
    printf("    soak: %llu plays, %llu stolen voices, %llu virtual starts, %llu promotions, %llu swaps, %llu channels stopped "
           "at the limit, %llu refused; peak %d voices, %d channels\n",
           (unsigned long long)(k1.se_plays - k0.se_plays), (unsigned long long)(k1.steals - k0.steals),
           (unsigned long long)(k1.virtual_starts - k0.virtual_starts), (unsigned long long)(k1.promotions - k0.promotions),
           (unsigned long long)(k1.swaps - k0.swaps), (unsigned long long)(k1.channel_steals - k0.channel_steals),
           (unsigned long long)(k1.refused - k0.refused), k1.peak_voices, k1.peak_channels);
}

// ---- the game's pause (the system menu's flag: everything held, the manager's clock stopped) ----

int g_counted = 0;   // updates the manager's clock moved in (not paused)
void step(bool paused, int n = 1, int level = 3)
{
    for (int k = 0; k < n; k++) {
        nsm_set_game_state(level, level, paused ? 1 : 0, 0);
        nsm_update(kDt);
        g_t += kDt;
        if (!paused) g_counted++;
    }
}

void pause_cases()
{
    printf("pause: everything held, then on from where it was\n");
    const uint32_t mus = start_music(0);
    frames(30);
    const double t_mus = g_t;
    mark();
    frames(30);
    const double m_base = tone(220, 0.0, 0.5);
    const uint32_t amb = nsm_play_se(160, 128, nullptr);
    const uint32_t one = nsm_play_se(42, 128, nullptr);
    const uint32_t fi = nsm_play_se_fade_in(164, 128, 2.0f);
    const uint32_t lp = nsm_play_se_fade_in(163, 128, 0.05f);   // a loop the game can fade out (fadeOutSE takes these)
    step(false, 20);
    nsm_fade_out_se(163, 1.0f);   // a loop fading out over 1 s
    nsm_duck_bgm(1.0f, 0.25f);    // the music ducking to a quarter over 1 s
    step(false, 20);
    step(true);                   // the update that pauses (its clock does not move)
    // the music's pause reaches its audio thread at that thread's next step (10 ms): from the next update on, held
    const double mus_at_pause = dbg(mus).offset;
    step(true);
    const double mus_late = dbg(mus).offset - mus_at_pause;
    const uint32_t hs[5] = {mus, amb, one, fi, lp};
    nsm_channel_debug before[5];
    for (int k = 0; k < 5; k++) before[k] = dbg(hs[k]);
    mark();
    step(true, 300);
    bool held = nsm_is_playing(160) && nsm_is_playing(42) && nsm_is_playing(164) && nsm_is_playing(163) && nsm_is_bgm_active();
    double moved = 0;
    bool gains = true;
    for (int k = 0; k < 5; k++) {
        const nsm_channel_debug d = dbg(hs[k]);
        moved = std::max(moved, std::fabs(d.offset - before[k].offset));
        gains = gains && d.gain == before[k].gain && d.exists && d.paused;
    }
    CHECK(held, "5 s paused: every sound still answers isPlaying, the music still active");
    CHECK(mus_late >= 0 && mus_late <= 0.011, "the music paused within its audio thread's step (%.4f s more of it)", mus_late);
    CHECK(moved < 0.002 && gains, "nothing moved: offsets within %.4f s; the fade-in, the fade-out and the duck held", moved);
    CHECK(rms(0.1, 5.0) < 1e-6, "silence while paused (rms %.2g)", rms(0.1, 5.0));
    step(false, 30);
    // the fades went on with the manager's clock: 39 updates of the fade-in before (one pending), 19 of the
    // fade-out (one pending), 20 of the duck; 30 more of each
    const float fi_want = 0.33f * (69 * kDt) / 2.0f, lp_want = (1.0f - 49 * kDt) * se_gain(0.6f);
    const float du_want = 0.165f * ((1.0f - 50 * kDt) + 0.25f * 50 * kDt);
    CHECK(std::fabs(dbg(fi).gain - fi_want) < 1e-4f && std::fabs(dbg(lp).gain - lp_want) < 1e-4f &&
              std::fabs(dbg(mus).gain - du_want) < 1e-4f,
          "the fades go on where they were: fade-in %.4f (want %.4f), fade-out %.4f (%.4f), duck %.4f (%.4f)",
          dbg(fi).gain, fi_want, dbg(lp).gain, lp_want, dbg(mus).gain, du_want);
    const double one_off = dbg(one).offset - before[2].offset;
    CHECK(std::fabs(one_off - 0.5) < 0.02, "the one-shot goes on from where it was: +%.3f s after 0.5 s", one_off);
    const double amb_want = fold(40 * kDt + 30 * kDt, 0.5, 2.0);
    CHECK(std::fabs(dbg(amb).offset - amb_want) < 0.02, "the loop at %.3f s (its unpaused time folded: %.3f)", dbg(amb).offset, amb_want);
    const double mpos = dbg(mus).offset, mwant = fold(before[0].offset + 0.5, 1.0, 4.0);
    CHECK(std::fabs(mpos - mwant) < 0.03, "the music at %.3f s (from %.3f, +0.5 s: %.3f)", mpos, before[0].offset, mwant);
    (void)t_mus;

    printf("pause: 200 cycles\n");
    nsm_unduck_bgm(0.3f);
    nsm_stop_se(164);
    nsm_stop_se(42);
    step(false, 30);
    nsm_stop_se(160);
    const uint32_t amb2 = nsm_play_se(160, 128, nullptr);
    const int c_amb = g_counted;
    const double m0 = dbg(mus).offset;
    const int c_mus = g_counted;
    uint32_t one2 = 0, fo = 0;
    int c_one = -1, c_fo = -1, one_end = -1, fo_end = -1;
    double worst_amb = 0, worst_mus = 0;
    for (int cyc = 0; cyc < 200; cyc++) {
        if (cyc == 20) one2 = nsm_play_se(45, 128, nullptr), c_one = g_counted;
        if (cyc == 30) fo = nsm_play_se_fade_in(164, 128, 0.05f), c_fo = g_counted;
        if (cyc == 32) nsm_fade_out_se(164, 1.0f), c_fo = g_counted;
        const int run = 1 + (int)(rnd() % 20), hold = 1 + (int)(rnd() % 30);
        for (int k = 0; k < run; k++) {
            step(false);
            if (one2 && one_end < 0 && !nsm_is_playing(45)) one_end = g_counted - c_one;
            if (fo && fo_end < 0 && c_fo >= 0 && !nsm_is_playing(164)) fo_end = g_counted - c_fo;
        }
        worst_amb = std::max(worst_amb, std::fabs(dbg(amb2).offset - fold((g_counted - c_amb) * kDt, 0.5, 2.0)));
        worst_mus = std::max(worst_mus, std::fabs(dbg(mus).offset - fold(m0 + (g_counted - c_mus) * kDt, 1.0, 4.0)));
        step(true, hold);
    }
    step(false);
    CHECK(worst_amb < 0.02, "a loop through 200 pauses: where its unpaused time says, within %.4f s", worst_amb);
    CHECK(worst_mus < 0.03, "the music through them: within %.4f s of its unpaused time", worst_mus);
    CHECK(one_end == 180 || one_end == 181, "a 3 s one-shot across pauses ends after 3 s of unpaused time (%d updates, 180)", one_end);
    CHECK(fo_end >= 61 && fo_end <= 63, "a 1 s fade-out across pauses ends after 1 s of unpaused time (%d updates, 62)", fo_end);
    CHECK(stats().stream_underruns == 0, "no underrun");

    printf("pause: a crossfade, a virtual channel, quitting while paused\n");
    nsm_fade_out_bgm(1.0f);
    nsm_play_bgm(1, 128, 1);
    step(false);
    step(true, 120);
    CHECK(nsm_current_bgm_id() == 323 && dbg(mus).exists, "paused in a crossfade: the old track held fading, the new one waiting");
    int to_active = 0;
    for (; to_active < 10 && nsm_current_bgm_id() != 1; to_active++) step(false);
    CHECK(to_active <= 3, "unpaused: the new track starts (%d updates)", to_active);
    step(false, 62);
    CHECK(!dbg(mus).exists, "the old track faded out over 1 s of unpaused time and was unloaded");
    mark();
    step(false, 30);
    CHECK(std::fabs(db(tone(330, 0.0, 0.5), m_base)) < 0.3 && tone(220, 0.0, 0.5) < 0.01 * m_base,
          "the new track heard at its level (%.2f dB), the old one gone", db(tone(330, 0.0, 0.5), m_base));
    std::vector<uint32_t> fill;
    for (int id = 42; id < 74; id++) fill.push_back(nsm_play_se(id, 128, nullptr));
    const uint32_t v = nsm_play_se(140, 128, nullptr);
    step(false, 10);
    const double v0 = dbg(v).offset;
    step(true, 120);
    const double v1 = dbg(v).offset;
    step(false, 30);
    const double v2 = dbg(v).offset;
    CHECK(!dbg(v).real && v1 == v0 && std::fabs(v2 - v0 - 0.5) < 1e-6, "a virtual channel's time held while paused (%.3f, %.3f, +%.3f)", v0, v1, v2 - v0);
    for (int id = 42; id < 74; id++) nsm_stop_se(id);
    nsm_stop_se(140);
    step(true, 5);   // quitting while paused: the shutdown below
}

// ---- the device (loopback: the losses simulated, the reopens real in this class's logic) ----

void device_cases()
{
    printf("device: a loss and its reopen\n");
    const uint32_t mus = start_music(0);
    const uint32_t amb = nsm_play_se(160, 128, nullptr), lp = nsm_play_se(163, 128, nullptr), one = nsm_play_se(42, 128, nullptr);
    const int c0 = g_counted;
    step(false, 30);
    mark();
    step(false, 30);
    const double m_b = tone(220, 0, 0.5), a_b = tone(300, 0, 0.5), l_b = tone(450, 0, 0.5), o_b = tone(1000, 0, 0.5);
    const nsm_stats s0 = stats();
    const double mpos0 = dbg(mus).offset;
    nsm_debug_device_loss(0, 0);
    step(false);
    nsm_stats s1 = stats();
    CHECK(s1.device_losses - s0.device_losses == 1 && s1.device_reopens - s0.device_reopens == 1 &&
              s1.effect_restarts - s0.effect_restarts == 3 && s1.stream_restarts - s0.stream_restarts == 1,
          "lost and reopened: the 3 effects and the music restarted");
    const double amb_want = fold((g_counted - c0) * kDt, 0.5, 2.0), lp_want = fold((g_counted - c0) * kDt, 0.0, 1.0);
    CHECK(std::fabs(dbg(amb).offset - amb_want) < 0.02 && std::fabs(dbg(lp).offset - lp_want) < 0.02 &&
              std::fabs(dbg(one).offset - (g_counted - c0) * kDt) < 0.02,
          "the effects go on where their time is (loop %.3f/%.3f, loop %.3f/%.3f, one-shot %.3f)", dbg(amb).offset, amb_want,
          dbg(lp).offset, lp_want, dbg(one).offset);
    CHECK(std::fabs(dbg(mus).offset - mpos0) < 0.03, "the music from where it stopped (%.3f s, was %.3f)", dbg(mus).offset, mpos0);
    mark();
    step(false, 30);
    CHECK(std::fabs(db(tone(220, 0, 0.5), m_b)) < 0.3 && std::fabs(db(tone(300, 0, 0.5), a_b)) < 0.3 &&
              std::fabs(db(tone(450, 0, 0.5), l_b)) < 0.3 && std::fabs(db(tone(1000, 0, 0.5), o_b)) < 0.3,
          "heard again at their levels (music %.2f, loops %.2f %.2f, one-shot %.2f dB)", db(tone(220, 0, 0.5), m_b),
          db(tone(300, 0, 0.5), a_b), db(tone(450, 0, 0.5), l_b), db(tone(1000, 0, 0.5), o_b));
    CHECK(stats().live_sources == s0.live_sources && stats().live_buffers == s0.live_buffers, "no source or buffer more");

    printf("device: an outage (two reopens failing), the volume changed meanwhile\n");
    nsm_stop_se(42);
    const uint32_t ends = nsm_play_se(45, 128, nullptr);   // 3 s: ends during the outage
    const int c45 = g_counted;
    step(false, 150);
    const nsm_stats s2 = stats();
    nsm_debug_device_loss(2, 0);
    mark();
    int ended_at = -1, loops_held = 0, n = 0;
    bool volume_set = false;
    while (stats().device_reopens == s2.device_reopens && n < 600) {   // the retries are a second apart (wall time)
        step(false, 1, volume_set ? 2 : 3);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        n++;
        if (n == 30) volume_set = true, nsm_set_game_state(2, 2, 0, 0), nsm_apply_volume_options();
        if (ended_at < 0 && !nsm_is_playing(45)) ended_at = g_counted - c45;
        loops_held += nsm_is_playing(160) && nsm_is_playing(163) && nsm_is_bgm_active();
    }
    const nsm_stats s3 = stats();
    CHECK(s3.reopen_failures - s2.reopen_failures == 2 && s3.device_reopens - s2.device_reopens == 1,
          "the reopen failed twice, then worked (%d updates out)", n);
    CHECK(loops_held == n, "throughout: the loops and the music still answer as playing (%d of %d updates)", loops_held, n);
    CHECK(ended_at == 180 || ended_at == 181, "a one-shot ending during it answers by its time (%d updates, 180)", ended_at);
    CHECK(rms(0.1, std::min(1.5, n * kDt - 0.1)) < 1e-6, "silence while the device is out");
    step(false, 2, 2);
    CHECK(!dbg(ends).exists && dbg(amb).real && dbg(lp).real, "after: the ended one-shot not restarted, the loops on voices");
    mark();
    step(false, 30, 2);
    const double k23 = 20 * std::log10(2.0 / 3.0);
    CHECK(std::fabs(db(tone(220, 0, 0.5), m_b) - k23) < 0.3 && std::fabs(db(tone(300, 0, 0.5), a_b) - k23) < 0.3 &&
              tone(one_shot_hz(45), 0, 0.5) < 0.01 * o_b,
          "the music and the loops at the levels set during the outage (%.2f, %.2f dB; want %.2f); the one-shot silent",
          db(tone(220, 0, 0.5), m_b), db(tone(300, 0, 0.5), a_b), k23);

    printf("device: a loss while paused\n");
    step(true, 10, 2);
    const double pa = dbg(amb).offset, pm = dbg(mus).offset;
    nsm_debug_device_loss(0, 0);
    step(true, 30, 2);
    step(false, 1, 2);
    CHECK(std::fabs(dbg(amb).offset - pa) < 0.025 && std::fabs(dbg(mus).offset - pm) < 0.03,
          "unpaused after it: the loop and the music from where they were paused (%.3f/%.3f, %.3f/%.3f)", dbg(amb).offset, pa,
          dbg(mus).offset, pm);
    mark();
    step(false, 30, 2);
    CHECK(std::fabs(db(tone(220, 0, 0.5), m_b) - k23) < 0.3 && std::fabs(db(tone(300, 0, 0.5), a_b) - k23) < 0.3,
          "and heard (%.2f, %.2f dB)", db(tone(220, 0, 0.5), m_b), db(tone(300, 0, 0.5), a_b));

    printf("device: 30 losses in a row\n");
    const nsm_stats s4 = stats();
    int survived = 0;
    for (int k = 0; k < 30; k++) {
        step(false, 1 + (int)(rnd() % 40), 2);
        nsm_debug_device_loss(0, 0);
        step(false, 2, 2);
        survived += dbg(amb).real && dbg(lp).real && nsm_is_bgm_active() && dbg(mus).real;
    }
    const nsm_stats s5 = stats();
    CHECK(survived == 30 && s5.device_losses - s4.device_losses == 30 && s5.device_reopens - s4.device_reopens == 30,
          "every loss recovered: the loops on voices, the music playing (%d of 30)", survived);
    CHECK(s5.live_sources == s0.live_sources && s5.live_buffers == s0.live_buffers && s5.stream_underruns == 0,
          "no source or buffer more, no underrun");
}

// ---- the real-time path: OpenAL Soft's mixer thread (the wave writer) and the music's audio thread ----

int realtime(int losses)
{
    printf("realtime: %d device losses (every third with two failed reopens, every fifth slow: 300 ms)\n", losses);
    const int th0 = thread_count();
    if (!init(false)) return 2;
    using clk = std::chrono::steady_clock;
    auto next = clk::now();
    auto frame = [&]() {
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
        nsm_update(kDt);
    };
    nsm_play_bgm(0, 128, 1);
    for (int k = 0; k < 120 && !nsm_is_bgm_active(); k++) frame();
    const uint32_t amb = nsm_play_se(160, 128, nullptr), lp = nsm_play_se(163, 128, nullptr);
    for (int k = 0; k < 120; k++) frame();
    const int th1 = thread_count();
    const nsm_stats s0 = stats();
    double worst_ms = 0;
    std::vector<double> times;
    int failures = 0, bad = 0, done = 0, n = 0, since = 0;
    while (done < losses && n < losses * 400) {
        n++;
        since++;
        // the next loss once the last one is over (a loss during an outage is a new one only after the reopen),
        // 45 to 135 updates after it
        if (since >= 45 + (int)(rnd() % 90) && stats().device_reopens - s0.device_reopens == (uint64_t)done) {
            since = 0;
            const int f = done % 3 == 2 ? 2 : 0;
            nsm_debug_device_loss(f, done % 5 == 4 ? 300 : 0);
            failures += f;
            done++;
        }
        if (n % 7 == 0) nsm_play_se(42 + (int)(rnd() % 60), 128, nullptr);
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
        const auto t0 = clk::now();
        nsm_update(kDt);
        const bool ok = nsm_is_playing(160) && nsm_is_playing(163) && nsm_is_bgm_active();
        const double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
        times.push_back(ms);
        worst_ms = std::max(worst_ms, ms);
        bad += !ok;
    }
    for (int k = 0; k < 600 && stats().device_reopens - s0.device_reopens < (uint64_t)losses; k++) frame();
    for (int k = 0; k < 60; k++) frame();
    const double a0 = dbg(amb).offset;
    for (int k = 0; k < 30; k++) frame();
    const double a1 = dbg(amb).offset;
    const nsm_stats s1 = stats();
    const int th2 = thread_count();
    std::sort(times.begin(), times.end());
    CHECK(s1.device_losses - s0.device_losses == (uint64_t)losses && s1.device_reopens - s0.device_reopens == (uint64_t)losses &&
              s1.reopen_failures - s0.reopen_failures == (uint64_t)failures,
          "%d losses, %d reopens, %d failed reopens retried", (int)(s1.device_losses - s0.device_losses),
          (int)(s1.device_reopens - s0.device_reopens), (int)(s1.reopen_failures - s0.reopen_failures));
    CHECK(bad == 0, "every update: the loops and the music answered as playing (%d not)", bad);
    CHECK(dbg(amb).real && dbg(lp).real && std::fabs(fold(a0 + 0.5, 0.5, 2.0) - a1) < 0.05,
          "after: the loops on voices and moving (%.3f -> %.3f in 0.5 s)", a0, a1);
    CHECK(s1.stream_underruns == 0, "no music underrun (%llu)", (unsigned long long)s1.stream_underruns);
    CHECK(worst_ms < 5.0, "the game's thread never waited: update and isPlaying p50 %.3f ms, p99 %.3f ms, worst %.3f ms",
          times[times.size() / 2], times[times.size() * 99 / 100], worst_ms);
    CHECK(th2 == th1, "threads: %d before the losses, %d after (%d before the manager)", th1, th2, th0);
    CHECK(s1.live_sources == s0.live_sources && s1.live_buffers == s0.live_buffers, "AL objects: %d sources, %d buffers, as before",
          s1.live_sources, s1.live_buffers);
    shutdown_clean("realtime");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const int th3 = thread_count();
    // OpenAL Soft keeps process-wide helpers once loaded (its device-list and notification threads): a second
    // manager's life, with losses, must leave no thread more than the first
    if (!init(false)) return 2;
    for (int k = 0; k < 60; k++) frame();
    nsm_debug_device_loss(0, 0);
    for (int k = 0; k < 60; k++) frame();
    nsm_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const int th4 = thread_count();
    CHECK(th4 == th3, "a second manager's life leaves no thread more: %d after the first shutdown, %d after the second "
          "(%d before any)", th3, th4, th0);
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "";
    make_folder();
    if (mode == "voices") {
        if (!init(true)) return 2;
        voices();
        shutdown_clean("voices");
    } else if (mode == "pause") {
        if (!init(true)) return 2;
        pause_cases();
        shutdown_clean("quitting while paused");
    } else if (mode == "device") {
        if (!init(true)) return 2;
        device_cases();
        shutdown_clean("device");
    } else if (mode == "realtime") {
        if (realtime(argc > 2 ? atoi(argv[2]) : 20)) return 2;
    } else {
        printf("usage: %s voices|pause|device|realtime [losses]\n", argv[0]);
        return 2;
    }
    printf("sound_stress_check %s: %d failed\n", mode.c_str(), g_fail);
    return g_fail ? 1 : 0;
}
