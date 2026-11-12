// The native sound manager (native_sound.h; docs/OPENAL_MIGRATION_PLAN.md section 1, M3): the game's sound-manager
// class answered on OpenAL Soft. Written for this project from the game's observed behaviour (M0's traces of
// rel27) and the documented meaning of each method; nothing of FMOD's or the game's code is in it.
//
// What the game sees, as rel27 showed it:
// - Sounds by id from the player's sound list (sound.lua.eu): music ids are streamed by playBGM, effects (ids
//   42-322) are resident once loadSounds has run. A file with `smpl` loop points loops from its loop start to its
//   loop end whatever the game asks (an effect played once, music played "once"); the rest play once (effects)
//   or as the game asks (music).
// - Records: every sound the game started, until it ends or is stopped -- an effect (playSE), an effect fading in
//   (playSEFadeIn), the music. Each record has a state: 1 effect; 2 music; 3 music ducking; 4 music unducking;
//   5 fade-in pending; 6 fading in; 7 effect after its fade-in; 8 effect ducking out; 9 effect ducked (silent,
//   playing); 11/12 music fading out; 13/14 effect fading out. The fade states move linearly over their time in
//   seconds of the game's own dt (update); a state named "pending" waits one update.
// - One instance per id: a new play of an id whose first record is still playing stops all of the id's records.
// - Volume: effects list volume x effect level x 0.11 (x the fade); music list volume x master x music level x
//   0.11 (x the fade or duck); levels are the game's option integers (3 by default); clamped to 0-1.
// - isPlaying: the id's first record's channel still sounding; a sound's end shows within a frame.
// - Music: playBGM opens the stream (an id already open: nothing changes); it starts at full volume on the second
//   update after the open completes; a "once" track that ends is unloaded; fadeOutBGM fades the music out, then
//   it is stopped and unloaded. isBGMActive / currentBGMId answer from records in states 2-4 (323: none).
// - The system menu pauses everything (the game's flag) and stops the manager's clock until it closes.
// - 3D: the game's frame is left-handed (+x right); OpenAL's is right-handed: x is mirrored. Inverse distance,
//   clamped, from the list's min and max (rolloff 1); a position given only at the start; a 3D effect played
//   without a position sounds at the listener. The listener never turns (forward +z, up +y).
// - Reverb: one EFX reverb on the whole mix (effects and music), on or off by setReverb, with the parameters the
//   game asked of its old library in I3DL2 terms (docs/OPENAL_MIGRATION_PLAN.md section 3).
#include "native_sound.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "al_api.h"
#include "al_device.h"
#include "al_stream.h"
#include "sound_list.h"
#include "voice_pool.h"

namespace {

constexpr float kLevelScale = 0.11f;
constexpr int kNoBgm = 323;
constexpr int kFirstEffect = 42, kLastEffect = 322;
constexpr uint32_t kMusicTag = 0x80000000u;   // a music channel's handle in the debug calls (effects' have bit 31 clear)
// A mono sound not placed in 3D (a 2D sound, a 3D one without a position, mono music) at the centre: each side at
// 0.707 of its gain, as rel27's mix had it (measured: 0.712 in the sound lab's capture). Played as a stereo copy
// straight to the outputs, so the level does not depend on OpenAL's panning (1.5 dB lower at the centre on stereo
// speakers) or on the speaker layout.
constexpr float kCentre = 0.70710678f;

enum Status { st_none = 0, st_opening = 2, st_loaded = 3, st_opened = 4, st_started = 5 };
enum State {
    se_one = 1, bgm_steady = 2, bgm_duck = 3, bgm_unduck = 4, se_fadein_pending = 5, se_fadein = 6, se_steady = 7,
    se_duck = 8, se_ducked = 9, bgm_fadeout_pending = 11, bgm_fadeout = 12, se_fadeout_pending = 13, se_fadeout = 14
};

struct Entry {
    int status = st_none;
    float volume = 0.0f;
    bool is3d = false;
    float min = 0.0f, max = 0.0f;
    int priority = 0;
    int counter = 0;                 // music: -1 loops, else plays left
    bool music = false;
    al_device::Sample sample;        // an effect's buffer
    al_stream::Handle stream = al_stream::kNone;
};

struct Record {
    int state = se_one;
    int id = -1;
    float T = 0.0f, t = 0.0f, D = 0.0f;
    bool music = false;
    uint32_t ch = 0;                 // the effect's pool handle, or the music's stream handle
    bool pending = false;            // music: its start is queued, not yet seen playing
    float gain = 0.0f;               // the gain last given
};

struct Manager {
    std::mutex mu;
    bool active = false, loopback = false;
    nsm_log_fn log = nullptr;
    al_api::Api api;
    al_device::Device dev;
    al_device::Reverb reverb;
    std::unique_ptr<al_stream::Streamer> streamer;
    voice_pool::Pool pool;
    sound_list::List list;
    std::string dir;
    std::map<int, Entry> entries;    // by id, in id order
    std::vector<Record> records;
    float master = 1.0f;
    int level_bgm = 3, level_se = 3;
    bool menu_flag = false, exit_requested = false;
    bool menu_paused = false;
    uint32_t last_bgm = 0, device_gen = 0;
    uint64_t se_plays = 0, bgm_plays = 0;
    int peak_records = 0;
    double render_carry = 0.0;
    std::vector<float> render_buf;
};

Manager& M()
{
    static Manager m;
    return m;
}

void logf(const char* fmt, ...)
{
    Manager& m = M();
    if (!m.log) return;
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    m.log(b);
}

void device_log(const char* line) { logf("%s", line); }

float clamp01(float v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; }   // NaN: 0

float full_se(const Entry& e) { return (float)M().level_se * (e.volume * kLevelScale); }
float full_bgm(const Entry& e) { return e.volume * M().master * (float)M().level_bgm * kLevelScale; }

const Entry* entry_of(int id)
{
    auto it = M().entries.find(id);
    return it == M().entries.end() ? nullptr : &it->second;
}

void to_al(const float* game, float* al)   // the game's left-handed frame to OpenAL's: x mirrored
{
    al[0] = -game[0];
    al[1] = game[1];
    al[2] = game[2];
}

// ---- channels: an effect on the voice pool, the music on the streamer ----

void ch_gain(Record& r, float g)
{
    Manager& m = M();
    g = clamp01(g);
    if (r.music) {
        if (std::fabs(g - r.gain) > 1e-7f || r.pending) m.streamer->set_gain(r.ch, g);
    } else {
        m.pool.set_gain(r.ch, g);
    }
    r.gain = g;
}

bool ch_playing(Record& r)
{
    Manager& m = M();
    if (!r.music) return m.pool.playing(r.ch);
    const al_stream::Status s = m.streamer->status(r.ch);
    if (r.pending) {
        if (s.state == al_stream::State::playing || s.state == al_stream::State::paused) r.pending = false;
        else if (s.state == al_stream::State::free || s.state == al_stream::State::failed) r.pending = false;
        else return true;   // queued
    }
    return s.state == al_stream::State::playing || s.state == al_stream::State::paused;
}

void ch_stop(Record& r)
{
    if (r.music) M().streamer->halt(r.ch);
    else M().pool.stop(r.ch);
}

void ch_pause(Record& r, bool p)
{
    if (r.music) M().streamer->pause(r.ch, p);
    else M().pool.pause(r.ch, p);
}

int first_record(int id)
{
    const std::vector<Record>& rs = M().records;
    for (size_t k = 0; k < rs.size(); k++)
        if (rs[k].id == id) return (int)k;
    return -1;
}

void stop_id(int id)   // every record of the id stopped and erased
{
    std::vector<Record>& rs = M().records;
    for (size_t k = 0; k < rs.size();) {
        if (rs[k].id == id) {
            ch_stop(rs[k]);
            rs.erase(rs.begin() + (ptrdiff_t)k);
        } else {
            k++;
        }
    }
}

void unload(int id)   // the id's records and its sound released, its entry gone
{
    Manager& m = M();
    stop_id(id);
    auto it = m.entries.find(id);
    if (it == m.entries.end()) return;
    Entry& e = it->second;
    if (e.stream) m.streamer->stop(e.stream);
    if (e.sample.buffer) al_device::free_sample(m.dev, &e.sample);
    m.entries.erase(it);
}

void push(const Record& r)
{
    Manager& m = M();
    m.records.push_back(r);
    m.peak_records = std::max(m.peak_records, (int)m.records.size());
}

uint32_t start_effect(int id, int priority, const float* pos, bool fade_in, float T)
{
    Manager& m = M();
    auto it = m.entries.find(id);
    if (it == m.entries.end() || (it->second.status != st_loaded && it->second.status != st_opened)) return 0;
    Entry& e = it->second;
    if (e.music || !e.sample.buffer) return 0;   // an opened music stream: the game never plays one as an effect
    const int k = first_record(id);
    if (k >= 0 && ch_playing(m.records[(size_t)k])) stop_id(id);
    voice_pool::ChannelInfo ci;
    ci.sound = id;
    ci.priority = priority;
    const wav_pcm::Info& wi = e.sample.info;
    ci.looping = wi.has_loop;   // a file with loop points loops, as rel27 played it
    ci.length = wi.seconds();
    ci.loop_start = wi.has_loop ? (double)wi.loop_start / wi.rate : 0.0;
    ci.loop_end = wi.has_loop ? (double)wi.loop_end / wi.rate : 0.0;
    ci.direct = wi.channels == 2;
    ci.gain = fade_in ? 0.0f : clamp01(full_se(e));
    unsigned buffer = e.sample.buffer;
    if (e.is3d && pos && !fade_in) {   // a 2D sound ignores a position; a 3D one without one sounds at the listener
        ci.positional = true;
        to_al(pos, ci.position);
        ci.ref_distance = std::max(e.min, 0.01f);
        ci.max_distance = std::max(e.max, ci.ref_distance);
    } else if (wi.channels == 1 && e.sample.centre_stereo) {   // at the centre, unpanned
        buffer = e.sample.centre_stereo;
        ci.direct = true;
        ci.level = kCentre;
    }
    const uint32_t h = m.pool.play(buffer, ci);
    if (!h) return 0;
    Record r;
    r.state = fade_in ? se_fadein_pending : se_one;
    r.id = id;
    r.T = T;
    r.ch = h;
    r.gain = ci.gain;
    push(r);
    m.se_plays++;
    return h;
}

void start_music(int id, Entry& e)
{
    Manager& m = M();
    Record r;
    r.state = bgm_steady;
    r.id = id;
    r.music = true;
    r.ch = e.stream;
    r.pending = true;
    ch_gain(r, full_bgm(e));
    m.streamer->play(e.stream);
    push(r);
    m.last_bgm = e.stream | kMusicTag;
    m.bgm_plays++;
}

void render(double dt)   // loopback (tests): the audio of dt seconds, discarded, in 10 ms steps with the streams'
{                         // work between them, as the audio thread does it in real time
    Manager& m = M();
    m.render_carry += dt * m.dev.frequency();
    const int frames = (int)m.render_carry;
    m.render_carry -= frames;
    const int step = std::max(1, m.dev.frequency() / 100);
    for (int left = frames; left > 0;) {
        const int n = std::min(left, step);
        m.render_buf.resize((size_t)n * 2);
        m.dev.render(m.render_buf.data(), n);
        m.streamer->service_once();
        left -= n;
    }
}

}  // namespace

extern "C" {

int nsm_init(const nsm_options* o)
{
    Manager& m = M();
    std::lock_guard<std::mutex> lk(m.mu);
    if (m.active) return 1;
    m.log = o->log;
    m.loopback = o->loopback != 0;
    m.dir = o->sound_dir ? o->sound_dir : ".";
    std::string err;
    const char* lib = getenv("OPENAL_LIB");
    if (!al_api::load(&m.api, lib && *lib ? lib : al_api::default_path(), &err)) {
        logf("%s: no sound", err.c_str());
        return 0;
    }
    al_device::Options dopt;
    dopt.loopback = m.loopback;
    if (!m.dev.open(&m.api, dopt, device_log, &err)) {
        logf("%s: no sound", err.c_str());
        al_api::unload(&m.api);
        return 0;
    }
    if (!sound_list::load(m.dir + "/sound.lua.eu", &m.list, &err)) {
        logf("%s: no sound", err.c_str());
        m.dev.close();
        al_api::unload(&m.api);
        return 0;
    }
    // the reverb the game asked for: its old library's SFX reverb with only Room changed (-100 mB), in I3DL2 terms
    al_device::ReverbParams p;
    p.gain = 0.891251f;        // Room -100 mB
    p.gain_hf = 1.0f;          // RoomHF 0 mB
    p.gain_lf = 1.0f;          // RoomLF 0 mB
    p.decay_time = 1.0f;
    p.decay_hf_ratio = 0.5f;
    p.reflections_gain = 0.0f;        // Reflections -10000 mB
    p.reflections_delay = 0.02f;
    p.late_gain = 1.0f;               // Reverb 0 mB
    p.late_delay = 0.04f;
    p.diffusion = 1.0f;
    p.density = 1.0f;
    p.hf_reference = 5000.0f;
    p.lf_reference = 250.0f;
    p.air_absorption_gain_hf = 1.0f;
    p.room_rolloff = 0.0f;
    const bool rev = m.reverb.create(m.dev, p);
    al_stream::Config sc;
    sc.manual = m.loopback;
    sc.reverb = rev ? &m.reverb : nullptr;
    m.streamer.reset(new al_stream::Streamer(m.dev, sc));
    m.pool.create(m.dev, o->voices > 0 ? o->voices : 32, 512, rev ? &m.reverb : nullptr);
    const al_api::Api& al = m.api;
    const float ori[6] = {0, 0, 1, 0, 1, 0};   // the game's forward +z, up +y (x mirrored: +x is the right)
    al.alListenerfv(AL_ORIENTATION, ori);
    al.alListener3f(AL_POSITION, 0, 0, 0);
    m.device_gen = m.dev.generation();
    m.active = true;
    logf("native sound manager: %zu sounds listed, %d voices, reverb %s", m.list.entries.size(), m.pool.voices(),
         rev ? "yes" : "none");
    return 1;
}

void nsm_load_sounds(void)
{
    Manager& m = M();
    std::lock_guard<std::mutex> lk(m.mu);
    if (!m.active) return;
    int loaded = 0, failed = 0;
    size_t bytes = 0;
    for (int id = kFirstEffect; id <= kLastEffect && id < (int)m.list.entries.size(); id++) {
        Entry& e = m.entries[id];
        if (e.status == st_loaded || e.status == st_opened) continue;
        const sound_list::Entry& le = m.list.entries[(size_t)id];
        if (le.file.empty()) continue;
        std::string err;
        al_device::Sample s;
        if (!al_device::load_sample(m.dev, m.dir + "/" + le.file, &s, &err, true)) {
            failed++;
            logf("%s: %s", le.file.c_str(), err.c_str());
            m.entries.erase(id);
            continue;
        }
        e = Entry{};
        e.status = st_loaded;
        e.volume = le.volume;
        e.is3d = le.is3d;
        e.min = le.is3d ? le.min_distance : 0.0f;
        e.max = le.is3d ? le.max_distance : 0.0f;
        e.sample = s;
        bytes += (size_t)s.info.data_bytes;
        loaded++;
    }
    logf("effects loaded: %d (%.1f MB), %d failed", loaded, bytes / 1e6, failed);
}

void nsm_shutdown(void)
{
    Manager& m = M();
    std::lock_guard<std::mutex> lk(m.mu);
    if (!m.active) return;
    for (Record& r : m.records) ch_stop(r);
    m.records.clear();
    for (auto& kv : m.entries) {
        if (kv.second.stream) m.streamer->stop(kv.second.stream);
        if (kv.second.sample.buffer) al_device::free_sample(m.dev, &kv.second.sample);
    }
    m.entries.clear();
    m.pool.destroy();
    m.streamer->shutdown();
    const al_stream::Totals t = m.streamer->totals();
    m.streamer.reset();
    m.reverb.destroy();
    const al_device::Live l = m.dev.live();
    const al_device::Stats ds = m.dev.stats();
    logf("shutdown: effects %llu, music %llu; underruns %llu; device lost %llu, reopened %llu; left: %d sources, "
         "%d buffers, %d effects, %d slots", (unsigned long long)m.se_plays, (unsigned long long)m.bgm_plays,
         (unsigned long long)t.underruns, (unsigned long long)ds.disconnects, (unsigned long long)ds.reopens,
         l.sources, l.buffers, l.effects, l.slots);
    m.dev.close();
    al_api::unload(&m.api);
    m.active = false;
}

int nsm_active(void)
{
    std::lock_guard<std::mutex> lk(M().mu);
    return M().active ? 1 : 0;
}

void nsm_set_game_state(int bgm_level, int se_level, int system_paused, int exit_requested)
{
    Manager& m = M();
    std::lock_guard<std::mutex> lk(m.mu);
    m.level_bgm = bgm_level;
    m.level_se = se_level;
    m.menu_flag = system_paused != 0;
    m.exit_requested = exit_requested != 0;
}

uint32_t nsm_play_se(int id, int priority, const float* pos)
{
    std::lock_guard<std::mutex> lk(M().mu);
    return M().active ? start_effect(id, priority, pos, false, 0.0f) : 0;
}

uint32_t nsm_play_se_fade_in(int id, int priority, float seconds)
{
    std::lock_guard<std::mutex> lk(M().mu);
    return M().active ? start_effect(id, priority, nullptr, true, seconds) : 0;
}

int nsm_is_playing(int id)
{
    std::lock_guard<std::mutex> lk(M().mu);
    if (!M().active) return 0;
    const int k = first_record(id);
    return k >= 0 && ch_playing(M().records[(size_t)k]) ? 1 : 0;
}

int nsm_stop_se(int id)
{
    std::lock_guard<std::mutex> lk(M().mu);
    if (M().active) stop_id(id);
    return 0;
}

int nsm_stop_channel(uint32_t handle)
{
    std::lock_guard<std::mutex> lk(M().mu);
    std::vector<Record>& rs = M().records;
    for (size_t k = 0; k < rs.size(); k++)
        if (!rs[k].music && rs[k].ch == handle && handle) {
            ch_stop(rs[k]);
            rs.erase(rs.begin() + (ptrdiff_t)k);
            return 0;
        }
    return 2;   // no channel with that handle: the stop fails
}

int nsm_fade_out_se(int id, float seconds)
{
    std::lock_guard<std::mutex> lk(M().mu);
    const Entry* e = entry_of(id);
    if (!e || (e->status != st_loaded && e->status != st_opened)) return 1;
    bool any = false;
    for (Record& r : M().records)
        if (r.id == id && (r.state == se_fadein_pending || r.state == se_steady)) {
            r.state = se_fadeout_pending;
            r.T = seconds;
            r.t = 0.0f;
            any = true;
        }
    return any ? 0 : 1;
}

int nsm_fade_out_loop_ses(float seconds)
{
    std::lock_guard<std::mutex> lk(M().mu);
    bool any = false;
    for (Record& r : M().records)
        if (r.state == se_fadein_pending || r.state == se_steady) {
            r.state = se_fadeout_pending;
            r.T = seconds;
            r.t = 0.0f;
            any = true;
        }
    return any ? 0 : 1;
}

int nsm_pause_by_id(int id, int paused)
{
    std::lock_guard<std::mutex> lk(M().mu);
    for (Record& r : M().records)
        if (r.id == id) ch_pause(r, paused != 0);
    return 0;
}

void nsm_duck_se(float seconds)
{
    std::lock_guard<std::mutex> lk(M().mu);
    for (Record& r : M().records)
        if (r.state == se_fadein_pending || r.state == se_fadein || r.state == se_steady) {
            r.state = se_duck;
            r.T = seconds - r.t;
            r.t = 0.0f;
        }
}

void nsm_unduck_se(float seconds)
{
    std::lock_guard<std::mutex> lk(M().mu);
    for (Record& r : M().records)
        if (r.state == se_ducked) {
            r.state = se_fadein_pending;
            r.T = seconds - r.t;
            r.t = 0.0f;
        }
}

void nsm_play_bgm(int id, int priority, int loop)
{
    Manager& m = M();
    std::lock_guard<std::mutex> lk(m.mu);
    if (!m.active) return;
    const int k = first_record(id);
    if (k >= 0 && ch_playing(m.records[(size_t)k])) unload(id);   // the same track again: from its start
    if (id < 0 || id >= (int)m.list.entries.size() || m.list.entries[(size_t)id].file.empty()) {
        logf("playBGM %d: no file in the sound list", id);
        return;
    }
    if (m.entries.count(id)) return;   // already open: nothing changes
    Entry e;
    e.music = true;
    e.status = st_opening;
    e.volume = m.list.entries[(size_t)id].volume;
    e.priority = priority;
    e.counter = loop ? -1 : 1;
    // a file with loop points loops whatever the game asks; one without loops as asked
    e.stream = m.streamer->open(m.dir + "/" + m.list.entries[(size_t)id].file,
                                loop ? al_stream::Loop::file_loop_points : al_stream::Loop::loop_points_only, 0.0f,
                                false, true, kCentre);
    if (!e.stream) {
        logf("playBGM %d: no stream", id);
        return;
    }
    m.entries[id] = e;
}

int nsm_fade_out_bgm(float seconds)
{
    std::lock_guard<std::mutex> lk(M().mu);
    bool any = false;
    for (Record& r : M().records) {
        if (r.state == bgm_steady || r.state == bgm_unduck) {
            r.state = bgm_fadeout_pending;
            r.T = seconds;
            r.t = 0.0f;
            any = true;
        } else if (r.state == bgm_duck) {   // as rel27: the fade starts from the duck's time
            r.state = bgm_fadeout_pending;
            r.t = r.T;
            r.T = seconds;
            any = true;
        }
    }
    return any ? 0 : 1;
}

int nsm_is_bgm_active(void)
{
    std::lock_guard<std::mutex> lk(M().mu);
    for (const Record& r : M().records)
        if (r.state == bgm_steady || r.state == bgm_duck || r.state == bgm_unduck) return 1;
    return 0;
}

int nsm_current_bgm_id(void)
{
    std::lock_guard<std::mutex> lk(M().mu);
    for (const Record& r : M().records)
        if (r.state == bgm_steady || r.state == bgm_duck || r.state == bgm_unduck) return r.id;
    return kNoBgm;
}

void nsm_duck_bgm(float seconds, float level)
{
    std::lock_guard<std::mutex> lk(M().mu);
    for (Record& r : M().records)
        if (r.state == bgm_steady) {
            r.state = bgm_duck;
            r.T = seconds;
            r.D = level;
            r.t = 0.0f;
        }
}

void nsm_unduck_bgm(float seconds)
{
    std::lock_guard<std::mutex> lk(M().mu);
    for (Record& r : M().records)
        if (r.state == bgm_duck) {
            r.state = bgm_unduck;
            r.T = seconds;
            r.t = 0.0f;
        }
}

void nsm_set_bgm_master(float master)
{
    Manager& m = M();
    std::lock_guard<std::mutex> lk(m.mu);
    m.master = master;
    if (!m.active) return;
    for (Record& r : m.records)
        if (r.state == bgm_steady)
            if (const Entry* e = entry_of(r.id)) ch_gain(r, full_bgm(*e));
}

void nsm_set_sound_volume(int id, float volume)
{
    std::lock_guard<std::mutex> lk(M().mu);
    M().entries[id].volume = volume;   // an id not loaded gets an empty entry, as rel27's
}

void nsm_apply_volume_options(void)
{
    Manager& m = M();
    std::lock_guard<std::mutex> lk(m.mu);
    if (!m.active) return;
    for (Record& r : m.records) {
        const Entry* e = entry_of(r.id);
        if (!e) continue;
        if (r.state == bgm_steady) ch_gain(r, full_bgm(*e));
        else if (r.state == se_one || r.state == se_steady) ch_gain(r, full_se(*e));
    }
}

void nsm_set_listener(const float* pos, const float* velocity)
{
    Manager& m = M();
    std::lock_guard<std::mutex> lk(m.mu);
    if (!m.active || !pos) return;
    float p[3];
    to_al(pos, p);
    m.api.alListenerfv(AL_POSITION, p);
    (void)velocity;   // always zero in the game; no Doppler
}

void nsm_set_reverb(int on)
{
    std::lock_guard<std::mutex> lk(M().mu);
    if (M().active) M().reverb.enable(on != 0);
}

int nsm_is_loaded(int id)
{
    std::lock_guard<std::mutex> lk(M().mu);
    const Entry* e = entry_of(id);
    return e && (e->status == st_loaded || e->status == st_opened) ? 1 : 0;
}

void nsm_update(float dt)
{
    Manager& m = M();
    std::lock_guard<std::mutex> lk(m.mu);
    if (!m.active) return;
    m.last_bgm = 0;
    if (m.loopback) {   // tests: the audio of the frame, then the streams' work
        render(dt);
        m.streamer->service_once();
    }
    if (m.dev.generation() != m.device_gen) {   // a lost device reopened: the effects go on where they were
        m.device_gen = m.dev.generation();
        m.pool.restart_after_loss();
    }
    // the system menu: everything paused, the manager's clock stopped, until it closes
    if (m.menu_flag && !m.menu_paused) {
        m.menu_paused = true;
        for (Record& r : m.records) ch_pause(r, true);
        return;
    }
    if (m.menu_paused) {
        if (m.menu_flag) return;
        m.menu_paused = false;
        for (Record& r : m.records) ch_pause(r, false);   // also ends any pauseById
    }
    m.pool.update(dt);
    // a music stream whose open completed (unless the game is exiting)
    for (auto& kv : m.entries) {
        Entry& e = kv.second;
        if (e.status != st_opening || m.exit_requested) continue;
        const al_stream::State s = m.streamer->status(e.stream).state;
        if (s != al_stream::State::opening && s != al_stream::State::failed && s != al_stream::State::free)
            e.status = st_opened;
    }
    // records whose sound ended
    for (size_t k = 0; k < m.records.size();) {
        if (!ch_playing(m.records[k])) m.records.erase(m.records.begin() + (ptrdiff_t)k);
        else k++;
    }
    // fades
    for (size_t k = 0; k < m.records.size();) {
        Record& r = m.records[k];
        const Entry* e = entry_of(r.id);
        const float fs = e ? full_se(*e) : 0.0f, fb = e ? full_bgm(*e) : 0.0f;
        bool erased = false;
        switch (r.state) {
        case se_fadein_pending: r.state = se_fadein; break;
        case bgm_fadeout_pending: r.state = bgm_fadeout; break;
        case se_fadeout_pending: r.state = se_fadeout; break;
        case bgm_duck: case bgm_unduck: case se_fadein: case se_duck: case bgm_fadeout: case se_fadeout:
            if (r.T < r.t) {   // the fade is over
                switch (r.state) {
                case bgm_unduck: r.state = bgm_steady; break;
                case se_fadein: r.state = se_steady; r.t = 0.0f; break;
                case se_duck: r.state = se_ducked; r.t = 0.0f; break;
                case bgm_fadeout: {
                    const int id = r.id;
                    ch_stop(r);
                    m.records.erase(m.records.begin() + (ptrdiff_t)k);
                    unload(id);
                    k = 0;   // the list changed: from the start (the records already done are steady)
                    erased = true;
                    break;
                }
                case se_fadeout:
                    ch_stop(r);
                    m.records.erase(m.records.begin() + (ptrdiff_t)k);
                    erased = true;
                    break;
                default: break;   // a duck that is over stays ducked
                }
            } else {
                r.t += dt;
                const float p = std::min(r.t, r.T) / r.T;   // T 0: not a number, given as silence
                float g = 0.0f;
                switch (r.state) {
                case bgm_duck: g = fb * ((1.0f - p) + r.D * p); break;
                case bgm_unduck: g = fb * (r.D * (1.0f - p) + p); break;
                case se_fadein: g = p * fs; break;
                default: g = (1.0f - p) * (r.state == bgm_fadeout ? fb : fs); break;
                }
                ch_gain(r, g);
            }
            break;
        default: break;
        }
        if (!erased) k++;
    }
    // music: an open stream starts on the update after its open; a "once" track that ended is unloaded
    for (auto it = m.entries.begin(); it != m.entries.end();) {
        const int id = it->first;
        Entry& e = it->second;
        if (e.music && e.status == st_opened) {
            e.status = st_started;
            ++it;
            continue;
        }
        if (e.music && e.status == st_started) {
            bool playing = false;
            for (Record& r : m.records)
                if (r.id == id && ch_playing(r)) playing = true;
            if (!playing) {
                if (e.counter == -1) {
                    start_music(id, e);
                } else if (e.counter > 0) {
                    e.counter--;
                    start_music(id, e);
                } else {
                    ++it;
                    unload(id);
                    continue;
                }
            }
        }
        ++it;
    }
}

void nsm_get_stats(nsm_stats* out)
{
    Manager& m = M();
    std::lock_guard<std::mutex> lk(m.mu);
    *out = nsm_stats{};
    out->se_plays = m.se_plays;
    out->bgm_plays = m.bgm_plays;
    out->peak_records = m.peak_records;
    if (!m.active) return;
    out->steals = m.pool.stats().steals;
    out->virtual_starts = m.pool.stats().virtual_starts;
    out->peak_voices = m.pool.stats().peak_real;
    const al_stream::Totals t = m.streamer->totals();
    out->stream_underruns = t.underruns;
    out->queue_overflows = t.queue_overflows;
    const al_device::Stats ds = m.dev.stats();
    out->device_losses = ds.disconnects;
    out->device_reopens = ds.reopens;
    const al_device::Live l = m.dev.live();
    out->live_sources = l.sources;
    out->live_buffers = l.buffers;
}

int nsm_debug_channel_gain(uint32_t handle, float* gain)
{
    std::lock_guard<std::mutex> lk(M().mu);
    const bool music = (handle & kMusicTag) != 0;
    const uint32_t ch = handle & ~kMusicTag;
    for (const Record& r : M().records)
        if (r.music == music && r.ch == ch) {
            *gain = r.gain;
            return 1;
        }
    return 0;
}

uint32_t nsm_debug_last_bgm_channel(void)
{
    std::lock_guard<std::mutex> lk(M().mu);
    return M().last_bgm;
}

}  // extern "C"
