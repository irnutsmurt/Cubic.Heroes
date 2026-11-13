// The movies on FFmpeg (docs/FFMPEG_MIGRATION_PLAN.md sections 2.3 and 3, M8.2): the eleven calls the game's movie
// class makes into Bink, answered natively at their call stubs (config/m8_native_movie.json: entry hooks that may
// decline). The class itself -- its states, the textures it allocates, its drawing with its own shader, the title
// menu over the title movie, the end of a movie -- stays the game's: the player decodes each frame with FFmpeg
// (movie_player.cpp) and writes its Y, U and V planes into the set of textures the class gave, as Bink did.
//
//   sound system (0x009873F8)  before each open: Bink's sound module given -- nothing to do (1)
//   open (0x009873C8)          the file (the game's path, mapped as the runtime's file system maps it), a decoder,
//                              the sound's stream (the music's streamer, OpenAL Soft), a handle the class reads:
//                              +0 width, +4 height, +8 frames, +0xC the next frame (from 1), +0x10 the last one
//                              decoded, +0x14/+0x18 the rate, +0x20 the open flags
//   frame buffers (0x009873D8) the class's description of its texture sets: 2 sets, the planes' sizes (2)
//   take buffers (0x00987428)  each frame before decoding: the description, with the planes' addresses
//   wait (0x00987418)          is the next frame due by the clock? 0 due, 1 not yet (paused: not yet)
//   decode (0x00987438)        the next frame into the next set, the set's index at the description's +0x14
//   behind (0x00987458)        is the frame after due too? then the class decodes it as well: every frame is
//                              decoded, a late one not shown
//   next (0x00987448)          the next frame; after the last, the first again (a looping movie restarts)
//   pause (0x009873A8)         1 paused, 0 playing: the clock and the sound held together
//   close (0x009873B8)         the decoder, the sound and the handle released
//   sound end (0x009873E8)     after a close: nothing to do (0)
// The clock: a movie with sound follows its sound (the frames heard, the output's latency taken off: al_stream's
// fed stream); a silent one a steady clock from its open, less its pauses. The frame due at a time t is the one
// whose start t has reached: (n - 1) x rate_den / rate_num seconds.
//
// DOTHEROES_NATIVE_MOVIE=1 turns it on (M8.2: Bink, still in the lift, is the default; =0 keeps it off). While Bink
// is in the lift, a handle that is not this player's, or a file FFmpeg cannot open, is left to Bink (the hooks
// decline).
#include "ppu_context.h"

#include "ff_api.h"
#include "movie_player.h"
#include "al_stream.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

extern "C" uint8_t* vm_base;
extern "C" void sys_fs_translate_path(const char* ps3_path, char* host_path, int host_path_size);
extern "C" int64_t sys_memory_allocate(ppu_context* ctx);
al_stream::Streamer* nsm_movie_streamer();   // native/audio/sound_manager.cpp: the music's streamer (null: none)
// native/movie/movie_trace.cpp: a decoded frame's planes hashed (DOTHEROES_MOVIE_PLANES; else nothing)
extern "C" void movie_planes_record(const char* name, int frame, uint32_t fb);

extern "C" int g_native_movie = 0;

namespace {

constexpr uint32_t kSlots = 16, kSlotBytes = 0x100, kPoolBytes = 0x10000;
constexpr bool kBinkFallback = true;   // M8.2: Bink is still lifted; what this player cannot do goes to it

inline uint32_t rd32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
inline void wr32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
inline void wr64(uint32_t ea, uint64_t v)
{
    wr32(ea, (uint32_t)(v >> 32));
    wr32(ea + 4, (uint32_t)v);
}

struct Movie {
    uint32_t handle = 0;
    std::string name;
    std::unique_ptr<movie::Player> player;
    al_stream::Streamer* streamer = nullptr;
    al_stream::Handle sound = al_stream::kNone;
    uint32_t fb = 0;         // the class's description of its texture sets (take buffers)
    int frames = 0;
    double period = 0;       // seconds a frame
    long long seq = 1;       // the next frame, counted on through loops (the handle's +0xC: its place in the file)
    bool paused = false;
    bool started = false;    // the steady clock (a silent movie's) runs from the class's first wait
    double game_frame = 0;   // the game's frame: the time between the class's questions, smoothed
    std::chrono::steady_clock::time_point last_wait;
    std::chrono::steady_clock::time_point t0, paused_at;
    double paused_total = 0;
    // what the close reports
    uint64_t decoded = 0, shown_late = 0, missing = 0, loops = 0;
    double late_max = 0, late_sum = 0;   // a decoded frame's start against the clock when it was decoded
};

struct State {
    std::mutex mu;
    bool tried = false, loaded = false;
    bool last_closed_ours = false;   // the sound's end after a close: this player's, or Bink's
    ff_api::Api ff;
    uint32_t pool = 0;
    Movie slots[kSlots];
} S;

// FFmpeg loaded once, at the first movie (beside the game; a missing library: Bink, or no movie)
bool ready()
{
    if (S.tried) return S.loaded;
    S.tried = true;
    std::string err;
    S.loaded = ff_api::load(&S.ff, ff_api::default_dir(), &err);
    if (S.loaded) {
        S.ff.av_log_set_level(AV_LOG_ERROR);
        fprintf(stderr, "[movie] native: FFmpeg %s loaded from %s (%s)\n", S.ff.version.c_str(), S.ff.dir.c_str(),
                S.ff.licence.c_str());
    } else {
        fprintf(stderr, "[movie] native: %s -- the movies %s\n", err.c_str(),
                kBinkFallback ? "go to Bink" : "are not played");
    }
    return S.loaded;
}

// the handles' guest page: one 64 KB allocation from the runtime's user memory (its result through a word below
// the caller's stack pointer: free while this runs on that thread)
bool pool(ppu_context* ctx)
{
    if (S.pool) return true;
    ppu_context tmp{};
    const uint32_t out = (uint32_t)ctx->gpr[1] - 0x200;
    tmp.gpr[3] = kPoolBytes;
    tmp.gpr[4] = 0x200;   // SYS_MEMORY_PAGE_SIZE_64K
    tmp.gpr[5] = out;
    wr32(out, 0);
    if (sys_memory_allocate(&tmp) != 0 || !rd32(out)) {
        fprintf(stderr, "[movie] native: no guest memory for the handles\n");
        return false;
    }
    S.pool = rd32(out);
    return true;
}

Movie* ours(uint32_t handle)
{
    if (!S.pool || handle < S.pool || handle >= S.pool + kSlots * kSlotBytes) return nullptr;
    Movie& m = S.slots[(handle - S.pool) / kSlotBytes];
    return m.handle == handle ? &m : nullptr;
}

// the stub's own work, which the caller relies on: the caller's TOC saved where its next instruction reloads it
inline int answer(ppu_context* ctx, uint64_t r3)
{
    wr64((uint32_t)ctx->gpr[1] + 0x28, ctx->gpr[2]);
    ctx->gpr[3] = r3;
    return 1;
}

double elapsed(Movie& m)
{
    if (m.sound && m.streamer) {
        const al_stream::Status st = m.streamer->status(m.sound);
        if (st.state == al_stream::State::playing)
            return st.position + std::min(0.05, std::max(0.0, m.streamer->clock_now() - st.taken));
        if (st.state == al_stream::State::paused || st.state == al_stream::State::finished) return st.position;
        if (st.state == al_stream::State::opening || st.state == al_stream::State::stopped) return 0.0;
        // failed or gone (no output): the steady clock
    }
    if (!m.started) return 0.0;
    const auto end = m.paused ? m.paused_at : std::chrono::steady_clock::now();
    return std::chrono::duration<double>(end - m.t0).count() - m.paused_total;
}

int cur_of(const Movie& m)
{
    return (int)((m.seq - 1) % m.frames) + 1;
}

// The game asks once a frame of its own (30 to uncapped). A frame is shown at the game's frame nearest its start:
// due from half a game frame before it (the game's frame measured between its questions, at most half a movie
// frame), so the next game frame would not be nearer; the class decodes a frame it will not show only when the
// next one is already half a movie frame late (the game a frame and a half behind) -- so a game frame's jitter
// neither skips a frame nor shows one twice. At 30 game frames a second the two run at one rate and keep their
// phase: a silent movie's clock starts at the class's first question, in phase with the game's frames.
constexpr double kBehind = 0.5;

bool due(Movie& m, long long seq)
{
    const double early = std::min(0.5 * m.period, 0.5 * m.game_frame);
    return !m.paused && elapsed(m) + early >= (double)(seq - 1) * m.period;
}

bool late(Movie& m, long long seq)
{
    return !m.paused && elapsed(m) >= (double)(seq - 1) * m.period + kBehind * m.period;
}

void release(Movie& m)
{
    if (m.sound && m.streamer) m.streamer->stop(m.sound);
    m.sound = al_stream::kNone;
    if (m.player) {
        const movie::Stats st = m.player->stats();
        fprintf(stderr, "[movie] native: %s closed at frame %d of %d (%lld frames through, %llu loop(s), %llu not shown: "
                "late): %llu decoded (%.2f ms a frame, the longest %.2f), %llu waited for (the longest %.1f ms), %llu "
                "not given; a frame's start against the clock when decoded: %+.1f ms on average, %+.1f at most; sound "
                "%llu frames (%llu cut), %.1f MB read\n", m.name.c_str(), cur_of(m), m.frames, m.seq - 1,
                (unsigned long long)m.loops, (unsigned long long)m.shown_late, (unsigned long long)st.frames_decoded,
                st.frames_decoded ? st.decode_us_total / 1000.0 / st.frames_decoded : 0.0, st.decode_us_max / 1000.0,
                (unsigned long long)st.waits, st.wait_us_max / 1000.0, (unsigned long long)m.missing,
                m.decoded ? m.late_sum / m.decoded * 1000.0 : 0.0, m.late_max * 1000.0,
                (unsigned long long)st.sound_frames, (unsigned long long)st.sound_cut, st.read_bytes / 1e6);
        m.player->close();
        m.player.reset();
    }
    if (m.handle) memset(vm_base + m.handle, 0, kSlotBytes);
    m = Movie{};
}

struct Init {
    Init()
    {
        const char* e = getenv("DOTHEROES_NATIVE_MOVIE");
        g_native_movie = e && *e == '1';
        if (g_native_movie) fprintf(stderr, "[movie] native: the movies on FFmpeg (DOTHEROES_NATIVE_MOVIE=1)\n");
    }
} s_init;

}  // namespace

extern "C" {

// 0x009873F8: Bink's sound module given, before an open. While Bink is lifted it records it (a movie it may still
// play needs it; this player has no use for it)
int bink_native_sound_system(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lk(S.mu);
    if (!ready() || kBinkFallback) return 0;
    return answer(ctx, 1);
}

// 0x009873C8: open(path, flags) -> handle
int bink_native_open(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lk(S.mu);
    if (!ready() || !pool(ctx)) return 0;
    char ps3[512], host[1024];
    const uint32_t p = (uint32_t)ctx->gpr[3];
    size_t n = 0;
    for (; n + 1 < sizeof ps3 && vm_base[p + n]; n++) ps3[n] = (char)vm_base[p + n];
    ps3[n] = 0;
    sys_fs_translate_path(ps3, host, (int)sizeof host);
    int slot = -1;
    for (uint32_t i = 0; i < kSlots; i++)
        if (!S.slots[i].handle) { slot = (int)i; break; }
    std::string err;
    auto player = slot < 0 ? nullptr : movie::Player::open(&S.ff, host, &err);
    if (!player) {
        fprintf(stderr, "[movie] native: %s: %s -- %s\n", ps3, slot < 0 ? "no free handle" : err.c_str(),
                kBinkFallback ? "Bink plays it" : "not played");
        if (kBinkFallback) return 0;
        return answer(ctx, 0);
    }
    Movie& m = S.slots[slot];
    m.handle = S.pool + (uint32_t)slot * kSlotBytes;
    const char* base = strrchr(ps3, '/');
    m.name = base ? base + 1 : ps3;
    const movie::Info& in = player->info();
    m.frames = in.frames;
    m.period = (double)in.rate_den / in.rate_num;
    m.seq = 1;
    m.t0 = std::chrono::steady_clock::now();
    m.player = std::move(player);
    const uint32_t h = m.handle;
    memset(vm_base + h, 0, kSlotBytes);
    wr32(h + 0x00, (uint32_t)in.width);
    wr32(h + 0x04, (uint32_t)in.height);
    wr32(h + 0x08, (uint32_t)in.frames);
    wr32(h + 0x0C, 1);
    wr32(h + 0x10, 0xFFFFFFFFu);
    wr32(h + 0x14, (uint32_t)in.rate_num);
    wr32(h + 0x18, (uint32_t)in.rate_den);
    wr32(h + 0x20, (uint32_t)ctx->gpr[4]);
    if (auto src = m.player->sound()) {
        m.streamer = nsm_movie_streamer();
        if (m.streamer) m.sound = m.streamer->open_source(src, 1.0f, true);
        if (!m.sound) fprintf(stderr, "[movie] native: %s: no sound stream (the picture on a steady clock)\n", ps3);
    }
    fprintf(stderr, "[movie] native: %s opened: %dx%d, %d frames at %d/%d, %s\n", m.name.c_str(), in.width,
            in.height, in.frames, in.rate_num, in.rate_den,
            in.sound_rate ? (std::to_string(in.sound_rate) + " Hz sound x " + std::to_string(in.sound_channels)).c_str()
                          : "silent");
    return answer(ctx, h);
}

// 0x009873D8: the frame buffers' description (r4): 2 sets of a Y plane and two half-size chroma planes, no alpha
int bink_native_frame_buffers(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lk(S.mu);
    Movie* m = ours((uint32_t)ctx->gpr[3]);
    if (!m) return 0;
    const uint32_t fb = (uint32_t)ctx->gpr[4];
    const uint32_t w = rd32(m->handle), hgt = rd32(m->handle + 4);
    memset(vm_base + fb, 0, 0x18 + 2 * 0x30);
    wr32(fb + 0x00, 2);
    wr32(fb + 0x04, w);
    wr32(fb + 0x08, hgt);
    wr32(fb + 0x0C, w / 2);
    wr32(fb + 0x10, hgt / 2);
    wr32(fb + 0x14, 0);
    for (uint32_t i = 0; i < 2; i++) {
        const uint32_t set = fb + 0x18 + i * 0x30;
        wr32(set + 0x00, 1);   // Y: allocate
        wr32(set + 0x0C, 1);   // cR
        wr32(set + 0x18, 1);   // cB
    }
    return answer(ctx, 2);
}

// 0x00987428: the description with its planes' addresses, before a decode
int bink_native_take_buffers(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lk(S.mu);
    Movie* m = ours((uint32_t)ctx->gpr[3]);
    if (!m) return 0;
    m->fb = (uint32_t)ctx->gpr[4];
    return answer(ctx, m->handle);
}

// 0x00987418: 0 the next frame is due, 1 not yet
int bink_native_wait(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lk(S.mu);
    Movie* m = ours((uint32_t)ctx->gpr[3]);
    if (!m) return 0;
    const auto now = std::chrono::steady_clock::now();
    if (!m->started) {   // the steady clock's start: the game's first frame with the movie open
        m->started = true;
        m->t0 = now;
        m->paused_total = 0;
        if (m->paused) m->paused_at = m->t0;
    } else {             // the game's frame, smoothed (a hitch counts as one long frame, at most 100 ms)
        const double dt = std::min(0.1, std::chrono::duration<double>(now - m->last_wait).count());
        m->game_frame = m->game_frame > 0 ? 0.9 * m->game_frame + 0.1 * dt : dt;
    }
    m->last_wait = now;
    return answer(ctx, due(*m, m->seq) ? 0 : 1);
}

// 0x00987438: the next frame into the next texture set
int bink_native_decode(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lk(S.mu);
    Movie* m = ours((uint32_t)ctx->gpr[3]);
    if (!m) return 0;
    const int cur = cur_of(*m);
    auto f = m->player->frame(cur, 500);
    const uint32_t fb = m->fb;
    if (f && fb && rd32(fb) >= 1) {
        const uint32_t total = rd32(fb);
        const uint32_t set_i = (rd32(fb + 0x14) + 1) % total;
        const uint32_t set = fb + 0x18 + set_i * 0x30;
        struct { uint32_t at; const std::vector<uint8_t>* src; int w, h; } planes[3] = {
            {set + 0x00, &f->y, f->width, f->height},
            {set + 0x0C, &f->v, f->width / 2, f->height / 2},   // cR: Cr
            {set + 0x18, &f->u, f->width / 2, f->height / 2},   // cB: Cb
        };
        bool ok = true;
        for (const auto& pl : planes) {
            const uint32_t buf = rd32(pl.at + 4), pitch = rd32(pl.at + 8);
            if (!buf || pitch < (uint32_t)pl.w || buf >= 0x40000000u) { ok = false; continue; }
            for (int r = 0; r < pl.h; r++)
                memcpy(vm_base + buf + (size_t)r * pitch, pl.src->data() + (size_t)r * pl.w, (size_t)pl.w);
        }
        if (ok) {
            wr32(fb + 0x14, set_i);
            movie_planes_record(m->name.c_str(), cur, fb);
        }
        m->decoded++;
        const double late = elapsed(*m) - (double)(m->seq - 1) * m->period;
        m->late_sum += late;
        m->late_max = std::max(m->late_max, late);
    } else {
        m->missing++;
    }
    wr32(m->handle + 0x10, (uint32_t)cur);
    return answer(ctx, 0);
}

// 0x00987458: 1 when the frame after is due as well (the class then decodes it too); never at the last frame
int bink_native_behind(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lk(S.mu);
    Movie* m = ours((uint32_t)ctx->gpr[3]);
    if (!m) return 0;
    const bool behind = cur_of(*m) < m->frames - 1 && late(*m, m->seq + 1);
    if (behind) m->shown_late++;
    return answer(ctx, behind ? 1 : 0);
}

// 0x00987448: the next frame; after the last, the first again
int bink_native_next(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lk(S.mu);
    Movie* m = ours((uint32_t)ctx->gpr[3]);
    if (!m) return 0;
    const int before = cur_of(*m);
    m->seq++;
    if (before == m->frames) {   // a looping movie: from its start again
        m->loops++;
        m->player->restart();
    }
    wr32(m->handle + 0x0C, (uint32_t)cur_of(*m));
    return answer(ctx, 0);
}

// 0x009873A8: pause(handle, 1) / resume(handle, 0)
int bink_native_pause(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lk(S.mu);
    Movie* m = ours((uint32_t)ctx->gpr[3]);
    if (!m) return 0;
    const bool p = ctx->gpr[4] != 0;
    if (p != m->paused) {
        if (p) m->paused_at = std::chrono::steady_clock::now();
        else m->paused_total += std::chrono::duration<double>(std::chrono::steady_clock::now() - m->paused_at).count();
        m->paused = p;
        if (m->sound && m->streamer) m->streamer->pause(m->sound, p);
    }
    return answer(ctx, p ? 1 : 0);
}

// 0x009873B8: close(handle)
int bink_native_close(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lk(S.mu);
    Movie* m = ours((uint32_t)ctx->gpr[3]);
    S.last_closed_ours = m != nullptr;
    if (!m) return 0;
    release(*m);
    return answer(ctx, 0);
}

// 0x009873E8: the sound's end, after a close: this player's sound is already released; Bink's is Bink's
int bink_native_sound_end(ppu_context* ctx)
{
    std::lock_guard<std::mutex> lk(S.mu);
    if (!S.loaded || (kBinkFallback && !S.last_closed_ours)) return 0;
    return answer(ctx, 0);
}

}  // extern "C"
