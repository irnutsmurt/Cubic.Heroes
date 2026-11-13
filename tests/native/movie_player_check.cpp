// M8.2's check of the native movie player's decoding (port/native/movie/movie_player.cpp), no game running:
//
//   movie_player_check <the player's movie folder> <the frame hashes' folder> [<FFmpeg's folder>]
//
// The hashes are tests/native/ffmpeg_decode_check's (<movie>.frames.txt: every frame of a plain decode). For each of
// the five movies, through the player as the game uses it:
//   - every frame asked for in order: each the same as the plain decode's (its planes' FNV-1a hash);
//   - every third frame asked for (the frames between not shown): those the same too -- the decoder never skips
//     a frame it has to decode, only the showing is skipped;
//   - a restart after 100 frames: the first 50 again, the same;
//   - the sound pulled as the stream's audio thread pulls it, while the frames are asked for: its total the
//     samples the packets declare (sum of each packet's declared samples, from the decode check's summary).
#include "ff_api.h"
#include "movie_player.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

ff_api::Api ff;

struct Expect {
    const char* file;
    int frames;
    long long sound_frames;   // declared (tests/native/ffmpeg_decode_check, M8.1)
};
const Expect kMovies[] = {
    {"TitleMovie_Omote_eng.bik", 2160, 0},
    {"TitleMovie_Ura_eng.bik", 2160, 0},
    {"OpeningMovie_eng.bik", 4620, 0},
    {"PlayDemoMovieUS.bik", 3465, 5543995},
    {"CreditMovie_eng.bik", 7860, 11554189},
};

uint64_t hash_frame(const movie::Frame& f)
{
    uint64_t h = 1469598103934665603ull;
    auto add = [&](const std::vector<uint8_t>& p) {
        for (uint8_t b : p) h = (h ^ b) * 1099511628211ull;
    };
    add(f.y);
    add(f.u);
    add(f.v);
    return h;
}

std::vector<uint64_t> load_hashes(const std::string& path)
{
    std::vector<uint64_t> out;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return out;
    long long n;
    unsigned long long h;
    while (fscanf(f, "%lld %llx", &n, &h) == 2) out.push_back(h);
    fclose(f);
    return out;
}

struct Pull {   // the sound pulled as the audio thread does: 250 ms at a time
    std::shared_ptr<al_stream::PcmSource> src;
    long long total = 0;
    bool ended = false;
    std::vector<int16_t> buf;
    void step()
    {
        if (!src || ended) return;
        const size_t want = src->rate() / 4;
        buf.resize(want * src->channels());
        bool e = false;
        total += (long long)src->read(buf.data(), want, &e);
        ended = e;
    }
};

bool check(const std::string& dir, const std::string& hashes_dir, const Expect& e)
{
    const std::vector<uint64_t> ref = load_hashes(hashes_dir + "/" + e.file + ".frames.txt");
    if ((int)ref.size() != e.frames) {
        printf("%s: no reference hashes (%zu)\n", e.file, ref.size());
        return false;
    }
    std::string err;
    bool ok = true;
    // in order, with the sound pulled alongside
    {
        auto p = movie::Player::open(&ff, dir + "/" + e.file, &err);
        if (!p) {
            printf("%s: %s\n", e.file, err.c_str());
            return false;
        }
        const movie::Info& in = p->info();
        Pull pull;
        pull.src = p->sound();
        int bad = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int n = 1; n <= in.frames; n++) {
            auto f = p->frame(n, 2000);
            if (!f || hash_frame(*f) != ref[(size_t)n - 1]) {
                if (bad++ < 3) printf("  frame %d: %s\n", n, f ? "differs" : "not given");
            }
            if (n % 7 == 0) pull.step();
        }
        while (pull.src && !pull.ended) {
            const long long before = pull.total;
            pull.step();
            if (pull.total == before && !pull.ended) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const movie::Stats st = p->stats();
        printf("%s: %dx%d, %d frames at %d/%d, sound %d Hz x %d; in order: %d of %d frames differ; sound %lld frames "
               "(declared %lld, %llu cut); %.1f s, decode %.2f ms a frame (max %.2f), %llu waits (max %.2f ms), %.1f MB "
               "read\n", e.file, in.width, in.height, in.frames, in.rate_num, in.rate_den, in.sound_rate,
               in.sound_channels, bad, in.frames, pull.total, e.sound_frames, (unsigned long long)st.sound_cut, s,
               st.frames_decoded ? st.decode_us_total / 1000.0 / st.frames_decoded : 0.0, st.decode_us_max / 1000.0,
               (unsigned long long)st.waits, st.wait_us_max / 1000.0, st.read_bytes / 1e6);
        if (bad || in.frames != e.frames || pull.total != e.sound_frames) ok = false;
    }
    // every third frame shown; then a restart
    {
        auto p = movie::Player::open(&ff, dir + "/" + e.file, &err);
        int bad = 0, shown = 0;
        for (int n = 1; n <= e.frames; n += 3) {
            auto f = p->frame(n, 2000);
            shown++;
            if (!f || hash_frame(*f) != ref[(size_t)n - 1]) bad++;
        }
        // restart: the first 50 again (after asking for frame 100)
        auto p2 = movie::Player::open(&ff, dir + "/" + e.file, &err);
        int bad2 = 0;
        for (int n = 1; n <= 100; n++) p2->frame(n, 2000);
        p2->restart();
        for (int n = 1; n <= 50; n++) {
            auto f = p2->frame(n, 2000);
            if (!f || hash_frame(*f) != ref[(size_t)n - 1]) bad2++;
        }
        const movie::Stats st = p->stats();
        printf("  every third frame: %d of %d differ (%llu frames decoded); after a restart: %d of 50 differ\n", bad,
               shown, (unsigned long long)st.frames_decoded, bad2);
        if (bad || bad2 || st.frames_decoded < (uint64_t)(e.frames - 2)) ok = false;
    }
    printf("  %s\n", ok ? "ok" : "FAILED");
    return ok;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <movie folder> <hashes folder> [<FFmpeg's folder>]\n", argv[0]);
        return 2;
    }
    std::string error;
    if (!ff_api::load(&ff, argc > 3 ? argv[3] : ff_api::default_dir(), &error)) {
        printf("%s\nmovie_player_check: FAILED (FFmpeg not loaded)\n", error.c_str());
        return 1;
    }
    ff.av_log_set_level(AV_LOG_ERROR);
    int failed = 0;
    for (const Expect& e : kMovies)
        if (!check(argv[1], argv[2], e)) failed++;
    printf("movie_player_check: %s\n", failed ? "FAILED" : "passed");
    return failed ? 1 : 0;
}
