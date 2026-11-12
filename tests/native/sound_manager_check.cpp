// The native sound manager's behaviour (port/native/audio/sound_manager.cpp) on made-up sounds, no game: a sound
// folder written here (a sound.lua.eu and tones), the manager on a loopback device stepped by update(dt) as the
// game steps it. Each case states what rel27 did (docs/OPENAL_MIGRATION_PLAN.md sections 1 and 4).
//   sound_manager_check            (the library: OpenAL32.dll beside this program, or OPENAL_LIB)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

std::string g_dir;

void write_wav(const std::string& name, unsigned ch, double seconds, double hz, uint32_t loop_start = 0,
               uint32_t loop_end = 0)
{
    const unsigned rate = 48000;
    const size_t n = (size_t)(seconds * rate);
    std::vector<int16_t> pcm(n * ch);
    for (size_t i = 0; i < n; i++)
        for (unsigned c = 0; c < ch; c++) pcm[i * ch + c] = (int16_t)(8000 * std::sin(2 * 3.14159265 * hz * i / rate));
    FILE* f = fopen((g_dir + "/" + name).c_str(), "wb");
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    const uint32_t data = (uint32_t)(pcm.size() * 2), smpl = loop_end ? 60 : 0;
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
        u32(1); u32(0); u32(0); u32(0); u32(loop_start); u32(loop_end - 1); u32(0); u32(0);
    }
    fclose(f);
}

// ids: 0 music looped by its loop points; 1 music without loop points; 2 a short music; 42 a short 2D effect;
// 43 a 3D effect (min 2, max 30); 44 a looping 2D ambience; 45 a 1.03 s 2D effect
void make_folder()
{
    const char* tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMPDIR");
    g_dir = std::string(tmp ? tmp : ".") + "/sound_manager_check";
#ifdef _WIN32
    system(("mkdir \"" + g_dir + "\" 2>NUL").c_str());
#else
    system(("mkdir -p '" + g_dir + "'").c_str());
#endif
    write_wav("m_loop.wav", 2, 4.0, 220, 48000, 4 * 48000);
    write_wav("m_plain.wav", 2, 3.0, 330);
    write_wav("m_short.wav", 2, 0.5, 440);
    write_wav("se_short.wav", 1, 0.3, 880);
    write_wav("se_3d.wav", 1, 2.0, 660);
    write_wav("se_amb.wav", 1, 1.0, 550, 0, 48000);
    write_wav("se_103.wav", 1, 1.03, 770);
    std::string list = "Sound =\n{\n\tBGM_FadeTime = 2.0,\n\tList =\n\t{\n";
    for (int id = 0; id <= 323; id++) {
        const char* f = "";
        float vol = 1.0f, mn = 0, mx = 0;
        int d3 = 0;
        if (id == 0) f = "m_loop.wav", vol = 0.4f;
        else if (id == 1) f = "m_plain.wav", vol = 0.6f;
        else if (id == 2) f = "m_short.wav", vol = 0.5f;
        else if (id == 42) f = "se_short.wav", vol = 0.7f;
        else if (id == 43) f = "se_3d.wav", vol = 1.0f, d3 = 1, mn = 2, mx = 30;
        else if (id == 44) f = "se_amb.wav", vol = 0.5f;
        else if (id == 45) f = "se_103.wav", vol = 0.8f;
        char b[160];
        snprintf(b, sizeof b, "\t\t{\"%s\", %.2f, %d, %.1f, %.1f},%s\n", f, vol, d3, mn, mx, id == 7 ? " -- a comment" : "");
        list += b;
    }
    list += "\t}\n}\n";
    FILE* f = fopen((g_dir + "/sound.lua.eu").c_str(), "wb");
    fwrite(list.data(), 1, list.size(), f);
    fclose(f);
}

const float kDt = 1.0f / 60.0f;
void frames(int n)
{
    for (int k = 0; k < n; k++) nsm_update(kDt);
}

float gain(uint32_t h)
{
    float g = -1;
    return nsm_debug_channel_gain(h, &g) ? g : -1.0f;
}

}  // namespace

int main()
{
    make_folder();
    nsm_options o{};
    o.sound_dir = g_dir.c_str();
    o.loopback = 1;
    o.log = [](const char*) {};
    if (!nsm_init(&o)) {
        printf("the manager did not start\n");
        return 2;
    }
    nsm_set_game_state(3, 3, 0, 0);
    nsm_load_sounds();
    printf("effects and their records\n");
    CHECK(nsm_is_loaded(42) && nsm_is_loaded(45) && !nsm_is_loaded(0) && !nsm_is_loaded(46),
          "effects loaded by loadSounds (42-322 with a file); music and empty ids not");
    const uint32_t h1 = nsm_play_se(42, 128, nullptr);
    CHECK(h1 != 0 && std::fabs(gain(h1) - 0.7f * 3 * 0.11f) < 1e-6f, "an effect: a handle, volume 0.7 x 3 x 0.11 = %.4f", gain(h1));
    const uint32_t h2 = nsm_play_se(42, 128, nullptr);
    CHECK(h2 && h2 != h1 && gain(h1) < 0 && nsm_stop_channel(h1) == 2, "the same id again: the first stopped (its handle stale, stopChannel 2)");
    CHECK(nsm_play_se(46, 128, nullptr) == 0 && nsm_play_se(0, 128, nullptr) == 0, "an id with no sound, and music, as an effect: no handle");
    const uint32_t h45 = nsm_play_se(45, 128, nullptr);
    int playing_frames = 0;
    for (int k = 0; k < 120 && nsm_is_playing(45); k++, playing_frames++) frames(1);
    CHECK(playing_frames >= 61 && playing_frames <= 63, "a 1.03 s effect answers isPlaying for %d frames of 1/60 s (ends within a frame of its length)", playing_frames);
    (void)h45;
    const uint32_t amb = nsm_play_se(44, 128, nullptr);
    frames(180);
    CHECK(nsm_is_playing(44) && gain(amb) > 0, "a file with loop points loops though played as a one-shot (3 s on a 1 s file)");
    nsm_stop_se(44);
    CHECK(!nsm_is_playing(44), "stopSE ends it");

    printf("fades\n");
    const uint32_t fi = nsm_play_se_fade_in(44, 128, 1.0f);
    CHECK(fi && gain(fi) == 0.0f, "a fade-in starts silent");
    frames(1);
    CHECK(gain(fi) == 0.0f, "the first update only arms it (pending)");
    frames(30);
    const float full = 0.5f * 3 * 0.11f;
    CHECK(std::fabs(gain(fi) - full * 0.5f) < 0.01f, "half way after 0.5 s: %.4f of %.4f", gain(fi), full);
    frames(40);
    CHECK(std::fabs(gain(fi) - full) < 1e-5f, "at full after 1 s: %.4f", gain(fi));
    CHECK(nsm_fade_out_loop_ses(0.5f) == 0 && nsm_fade_out_loop_ses(0.5f) == 1, "fadeOutLoopSEs: 0 when it took one, 1 when none was left");
    frames(16);
    CHECK(std::fabs(gain(fi) - full * 0.5f) < 0.02f, "fading out: %.4f half way", gain(fi));
    frames(30);
    CHECK(!nsm_is_playing(44), "stopped at the fade-out's end");
    const uint32_t d = nsm_play_se_fade_in(44, 128, 0.5f);
    frames(60);
    nsm_duck_se(0.5f);
    frames(16);
    CHECK(gain(d) > 0 && gain(d) < full, "duckSE fades it down (%.4f)", gain(d));
    frames(30);
    CHECK(gain(d) == 0.0f && nsm_is_playing(44), "ducked: silent but playing");
    nsm_unduck_se(0.5f);
    frames(40);
    CHECK(std::fabs(gain(d) - full) < 1e-5f, "unduckSE fades it back in (%.4f)", gain(d));
    nsm_stop_se(44);

    printf("music\n");
    CHECK(nsm_current_bgm_id() == 323 && !nsm_is_bgm_active(), "no music: currentBGMId 323, isBGMActive 0");
    nsm_play_bgm(0, 128, 1);
    int to_active = 0;
    for (; to_active < 10 && !nsm_is_bgm_active(); to_active++) frames(1);
    CHECK(to_active == 2 && nsm_current_bgm_id() == 0, "it starts on the second update after its open (%d)", to_active);
    nsm_play_bgm(0, 128, 1);
    frames(3);
    CHECK(nsm_is_bgm_active() && nsm_current_bgm_id() == 0, "playBGM of the playing track: restarted from its start");
    const uint32_t m0 = nsm_debug_last_bgm_channel();
    (void)m0;
    nsm_duck_bgm(0.5f, 0.25f);
    frames(40);
    nsm_unduck_bgm(0.5f);
    frames(40);
    CHECK(nsm_is_bgm_active(), "duck and unduck keep it active");
    CHECK(nsm_fade_out_bgm(1.0f) == 0, "fadeOutBGM: 0 when it took the music");
    frames(2);
    CHECK(!nsm_is_bgm_active() && nsm_current_bgm_id() == 323, "fading out: no longer the active music");
    nsm_play_bgm(1, 128, 1);
    frames(3);
    CHECK(nsm_is_bgm_active() && nsm_current_bgm_id() == 1, "the next track starts while the old one fades (two streams)");
    frames(80);
    nsm_play_bgm(0, 128, 1);
    frames(3);
    CHECK(nsm_current_bgm_id() == 1, "the old track was unloaded at its fade's end: a new playBGM of it opens it again");
    nsm_fade_out_bgm(0.2f);
    frames(30);
    nsm_play_bgm(2, 128, 0);
    frames(3);
    CHECK(nsm_current_bgm_id() == 2, "a track asked once");
    frames(60);
    CHECK(!nsm_is_bgm_active(), "a once track without loop points ends and is unloaded");

    printf("pause, 3D\n");
    const uint32_t p = nsm_play_se(44, 128, nullptr);
    nsm_set_game_state(3, 3, 1, 0);
    frames(120);
    CHECK(nsm_is_playing(44), "the system menu pauses everything (still playing)");
    nsm_set_game_state(3, 3, 0, 0);
    frames(1);
    nsm_pause_by_id(44, 1);
    frames(10);
    CHECK(nsm_is_playing(44), "pauseById: paused counts as playing");
    nsm_set_game_state(3, 3, 1, 0);
    frames(1);
    nsm_set_game_state(3, 3, 0, 0);
    frames(1);
    CHECK(nsm_is_playing(44) && gain(p) > 0, "leaving the system menu unpauses everything, pauseById included");
    nsm_stop_se(44);
    const float lis[3] = {10, 0, 10}, right[3] = {14, 0, 10};
    nsm_set_listener(lis, nullptr);
    const uint32_t s3 = nsm_play_se(43, 128, right);
    const uint32_t s0 = nsm_play_se(43, 128, nullptr);
    CHECK(s3 && s0, "a 3D effect with a position and one without (the second stops the first: one instance)");

    nsm_stats st{};
    nsm_get_stats(&st);
    nsm_shutdown();
    nsm_stats after{};
    nsm_get_stats(&after);
    CHECK(after.live_sources == 0 && after.live_buffers == 0, "shutdown: nothing left (%llu effects, %llu music played)",
          (unsigned long long)st.se_plays, (unsigned long long)st.bgm_plays);
    printf("sound_manager_check: %d failed\n", g_fail);
    return g_fail ? 1 : 0;
}
