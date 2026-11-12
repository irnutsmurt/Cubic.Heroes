// M3's replay (docs/OPENAL_MIGRATION_PLAN.md section 4, "Automated gates"): rel27's own sound-manager calls, as
// M0 traced them in the game (DOTHEROES_AUDIO_TRACE), given to the native sound manager (port/native/audio) in the
// same order with the same arguments and the same update steps, on a loopback device rendered in step with the
// game's dt (no game, faster than real time). Compared with what rel27 answered or did:
//   - every answer the game reads: isPlaying, isBGMActive, currentBGMId, isLoaded, fadeOutLoopSEs, fadeOutBGM,
//     fadeOutSE, stopChannel -- the same value at the same call;
//   - every effect play: a handle where rel27 gave one, none where it gave none;
//   - every volume rel27 gave a channel (FMOD's setVolume: the game sets them every frame and through its
//     fades) against the native channel's gain at that moment, effects and music (music channels paired in the
//     order they started); and each music start's update against rel27's;
//   - no underrun, nothing left at shutdown.
//   sound_replay <trace> <sound dir> [--levels <bgm> <se>] [--report <file>] [--limit-s <seconds>] [--wav <file>]
//                [--reverb name=value,...]
// --wav: the native mix written (48 kHz stereo float WAV) from the trace's SM.init, as rel27's capture starts there
// (its device opened): the same calls on both engines, for comparing their output (M4).
// The library: OpenAL32.dll beside this program, or OPENAL_LIB=<path>.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "native_sound.h"
#include "trace_reader.h"

namespace {

FILE* g_wav = nullptr;
bool g_wav_on = false;   // from the trace's SM.init (the game updates the class before it, silently)
uint64_t g_wav_frames = 0;

void wav_header(FILE* f, uint64_t frames)
{
    const uint32_t data = (uint32_t)(frames * 8), rate = 48000;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    u32(36 + data);
    fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(3); u16(2); u32(rate); u32(rate * 8); u16(8); u16(32);
    fwrite("data", 1, 4, f);
    u32(data);
}

void wav_capture(const float* lr, int n, void*)
{
    if (!g_wav_on) return;
    fwrite(lr, sizeof(float) * 2, (size_t)n, g_wav);
    g_wav_frames += (uint64_t)n;
}

using trace_reader::Call;

int argi(const Call& c, size_t k) { return k < c.args.size() ? (int)strtol(c.args[k].c_str(), nullptr, 10) : 0; }
uint32_t argx(const Call& c, size_t k) { return k < c.args.size() ? (uint32_t)strtoul(c.args[k].c_str(), nullptr, 16) : 0; }
float argf(const Call& c, size_t k) { return k < c.args.size() ? strtof(c.args[k].c_str(), nullptr) : 0.f; }
bool argv3(const Call& c, size_t k, float v[3]) { return k < c.args.size() && trace_reader::vec3(c.args[k], v); }

struct Diff {
    double s;
    uint32_t frame;
    std::string what;
};

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: sound_replay <trace> <sound dir> [--levels <bgm> <se>] [--report <file>] [--limit-s <s>]\n");
        return 2;
    }
    int bgm_level = 3, se_level = 3;
    const char* report = nullptr;
    double limit_s = 1e30;
    const char* reverb = nullptr;   // --reverb name=value,...: the reverb's parameters changed (the calibration)
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--levels") && i + 2 < argc) {
            bgm_level = atoi(argv[++i]);
            se_level = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--report") && i + 1 < argc) {
            report = argv[++i];
        } else if (!strcmp(argv[i], "--limit-s") && i + 1 < argc) {
            limit_s = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--reverb") && i + 1 < argc) {
            reverb = argv[++i];
        } else if (!strcmp(argv[i], "--wav") && i + 1 < argc) {
            g_wav = fopen(argv[++i], "wb");
            if (g_wav) wav_header(g_wav, 0);
        }
    }
    std::vector<Call> calls = trace_reader::read(argv[1]);
    if (calls.empty()) {
        fprintf(stderr, "no calls in %s\n", argv[1]);
        return 2;
    }
    nsm_options o{};
    o.sound_dir = argv[2];
    o.log = [](const char* l) { fprintf(stderr, "[audio] %s\n", l); };
    o.loopback = 1;
    o.reverb = reverb;
    if (!nsm_init(&o)) {
        fprintf(stderr, "the native sound manager did not start\n");
        return 2;
    }
    nsm_set_game_state(bgm_level, se_level, 0, 0);
    if (g_wav) nsm_debug_capture(wav_capture, nullptr);
    std::vector<Diff> diffs;
    std::map<std::string, std::pair<uint64_t, uint64_t>> queries;   // name -> (same, different)
    std::map<uint32_t, uint32_t> handles;                            // rel27's channel -> native
    std::deque<std::pair<uint32_t, uint64_t>> rel_bgm;               // rel27 music channels, their update
    std::deque<std::pair<uint32_t, uint64_t>> nat_bgm;               // native music channels, their update
    uint64_t vol_checked = 0, vol_off = 0, bgm_starts = 0, bgm_late = 0, bgm_early = 0, updates = 0;
    double worst_vol = 0;
    const uint64_t t0 = calls.front().us;
    std::string last_sm;
    auto diff = [&](const Call& c, const std::string& what) {
        if (diffs.size() < 200000) diffs.push_back({(c.us - t0) / 1e6, c.frame, what});
    };
    auto query = [&](const Call& c, int native) {
        auto& q = queries[c.name];
        if (native == (int)c.r3) {
            q.first++;
        } else {
            q.second++;
            char b[200];
            snprintf(b, sizeof b, "%s %s: rel27 %d, native %d", c.name.c_str(), c.args.empty() ? "" : c.args[0].c_str(),
                     (int)c.r3, native);
            diff(c, b);
        }
    };
    auto pair_bgm = [&]() {
        while (!rel_bgm.empty() && !nat_bgm.empty()) {
            handles[rel_bgm.front().first] = nat_bgm.front().first;
            const int64_t d = (int64_t)nat_bgm.front().second - (int64_t)rel_bgm.front().second;
            bgm_starts++;
            if (d > 0) bgm_late++;
            if (d < 0) bgm_early++;
            rel_bgm.pop_front();
            nat_bgm.pop_front();
        }
    };
    size_t n_sm = 0;
    for (size_t k = 0; k < calls.size(); k++) {
        const Call& c = calls[k];
        if ((c.us - t0) / 1e6 > limit_s) break;
        const std::string& n = c.name;
        if (n.rfind("SM.", 0) == 0) {
            n_sm++;
            last_sm = n;
        }
        float v1[3], v2[3];
        if (n == "SM.init") g_wav_on = true;
        if (n == "SM.update") {
            nsm_update(argf(c, 0));
            updates++;
            if (const uint32_t b = nsm_debug_last_bgm_channel()) nat_bgm.push_back({b, updates});
            pair_bgm();
        } else if (n == "SM.loadSounds") {
            nsm_load_sounds();
        } else if (n == "SM.playSE") {
            const uint32_t h = nsm_play_se(argi(c, 0), argi(c, 1), argv3(c, 2, v1) ? v1 : nullptr);
            if ((h != 0) != (c.r3 != 0))
                diff(c, "playSE " + c.args[0] + (h ? ": native played, rel27 did not" : ": rel27 played, native did not"));
            if (c.r3) handles[c.r3] = h;
        } else if (n == "SM.playSEFadeIn") {
            const uint32_t h = nsm_play_se_fade_in(argi(c, 0), argi(c, 2), argf(c, 3));
            if ((h != 0) != (c.r3 != 0)) diff(c, "playSEFadeIn " + c.args[0] + ": a handle in one only");
            if (c.r3) handles[c.r3] = h;
        } else if (n == "SM.isPlaying") query(c, nsm_is_playing(argi(c, 0)));
        else if (n == "SM.isBGMActive") query(c, nsm_is_bgm_active());
        else if (n == "SM.currentBGMId") query(c, nsm_current_bgm_id());
        else if (n == "SM.isLoaded") query(c, nsm_is_loaded(argi(c, 0)));
        else if (n == "SM.fadeOutLoopSEs") query(c, nsm_fade_out_loop_ses(argf(c, 0)));
        else if (n == "SM.fadeOutBGM") query(c, nsm_fade_out_bgm(argf(c, 0)));
        else if (n == "SM.fadeOutSE") query(c, nsm_fade_out_se(argi(c, 0), argf(c, 1)));
        else if (n == "SM.stopSE") nsm_stop_se(argi(c, 0));
        else if (n == "SM.stopChannel") {
            auto it = handles.find(argx(c, 0));
            query(c, nsm_stop_channel(it == handles.end() ? 0 : it->second));
        } else if (n == "SM.pauseById") nsm_pause_by_id(argi(c, 0), argi(c, 1));
        else if (n == "SM.playBGM") nsm_play_bgm(argi(c, 0), argi(c, 1), argi(c, 2));
        else if (n == "SM.duckBGM") nsm_duck_bgm(argf(c, 0), argf(c, 1));
        else if (n == "SM.unduckBGM") nsm_unduck_bgm(argf(c, 0));
        else if (n == "SM.duckSE") nsm_duck_se(argf(c, 0));
        else if (n == "SM.unduckSE") nsm_unduck_se(argf(c, 0));
        else if (n == "SM.setBGMMaster") nsm_set_bgm_master(argf(c, 0));
        else if (n == "SM.setSoundVolume") nsm_set_sound_volume(argi(c, 0), argf(c, 1));
        else if (n == "SM.applyVolumeOptions") nsm_apply_volume_options();
        else if (n == "SM.setListener") {
            if (argv3(c, 0, v1)) nsm_set_listener(v1, argv3(c, 1, v2) ? v2 : nullptr);
        } else if (n == "SM.setReverb") nsm_set_reverb(argi(c, 0));
        else if (n == "FMOD.System.playSound" && last_sm == "SM.update" && !c.outs.empty()) {   // rel27 started music
            rel_bgm.push_back({(uint32_t)strtoul(c.outs[0].c_str(), nullptr, 16), updates});
            pair_bgm();
        } else if (n == "FMOD.Channel.setVolume") {
            auto it = handles.find(c.self);
            if (it == handles.end() || !it->second) continue;
            float g = 0;
            if (!nsm_debug_channel_gain(it->second, &g)) continue;   // gone natively (isPlaying compares ends)
            const double want = std::min(1.0, std::max(0.0, (double)argf(c, 0))), err = std::fabs(g - want);
            vol_checked++;
            worst_vol = std::max(worst_vol, err);
            if (err > 0.002 && err > 0.01 * want) {
                vol_off++;
                char b[200];
                snprintf(b, sizeof b, "volume of channel %X%s: rel27 %.4f, native %.4f", c.self,
                         (it->second & 0x80000000u) ? " (music)" : "", want, g);
                diff(c, b);
            }
        }
    }
    nsm_stats st{};
    nsm_get_stats(&st);
    nsm_shutdown();
    if (g_wav) {
        wav_header(g_wav, g_wav_frames);
        fclose(g_wav);
    }
    nsm_stats after{};
    nsm_get_stats(&after);
    uint64_t q_same = 0, q_diff = 0;
    for (auto& q : queries) {
        q_same += q.second.first;
        q_diff += q.second.second;
    }
    printf("%s: %zu sound-manager calls, %llu updates (%.0f s); answers %llu same, %llu different", argv[1], n_sm,
           (unsigned long long)updates, (calls.back().us - t0) / 1e6, (unsigned long long)q_same, (unsigned long long)q_diff);
    for (auto& q : queries)
        printf("; %s %llu/%llu", q.first.c_str() + 3, (unsigned long long)q.second.first,
               (unsigned long long)(q.second.first + q.second.second));
    printf("\n  volumes: %llu compared, %llu off (worst %.4f); music starts paired %llu (%llu later, %llu earlier "
           "than rel27's update); effects %llu, music %llu; peak voices %d, records %d; steals %llu, underruns %llu; "
           "left at shutdown: %d sources, %d buffers\n", (unsigned long long)vol_checked, (unsigned long long)vol_off,
           worst_vol, (unsigned long long)bgm_starts, (unsigned long long)bgm_late, (unsigned long long)bgm_early,
           (unsigned long long)st.se_plays, (unsigned long long)st.bgm_plays, st.peak_voices, st.peak_records,
           (unsigned long long)st.steals, (unsigned long long)st.stream_underruns, after.live_sources, after.live_buffers);
    if (report) {
        if (FILE* f = fopen(report, "w")) {
            for (const Diff& d : diffs) fprintf(f, "%.3f %u %s\n", d.s, d.frame, d.what.c_str());
            fclose(f);
        }
    }
    for (size_t k = 0; k < diffs.size() && k < 12; k++)
        printf("  %.3f s (frame %u): %s\n", diffs[k].s, diffs[k].frame, diffs[k].what.c_str());
    return diffs.empty() && !after.live_sources && !after.live_buffers && !bgm_late && !bgm_early ? 0 : 1;
}
