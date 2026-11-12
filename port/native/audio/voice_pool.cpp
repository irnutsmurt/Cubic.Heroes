// The effect voices (voice_pool.h).
#include "voice_pool.h"

#include <algorithm>
#include <cmath>

namespace voice_pool {

namespace {
// a virtual channel of the same priority takes a real one's voice only when this much more audible (1.6 dB)
constexpr double kSwapMargin = 1.2;
// a one-shot found stopped this close to its length (or past it) ended by itself: its time is the game's dt summed,
// its source's end the audio clock's (and OpenAL Soft's last mixing update)
constexpr double kEndSlack = 0.1;
}

bool Pool::create(al_device::Device& dev, int voices, int max_channels, const al_device::Reverb* reverb)
{
    destroy();
    dev_ = &dev;
    reverb_ = reverb;
    generation_ = dev.generation();
    for (int k = 0; k < voices; k++) {
        Voice v;
        v.source = dev.new_source();
        if (!v.source) break;
        voices_.push_back(v);
    }
    channels_.assign((size_t)max_channels, Channel{});
    return (int)voices_.size() == voices;
}

void Pool::destroy()
{
    if (!dev_) return;
    for (Voice& v : voices_) dev_->delete_source(v.source);
    voices_.clear();
    channels_.clear();
    dev_ = nullptr;
}

Pool::Channel* Pool::get(Handle h)
{
    const size_t slot = (h & 0xFFF) - 1;
    if (!h || slot >= channels_.size()) return nullptr;
    Channel& c = channels_[slot];
    return c.used && c.gen == (h >> 12) ? &c : nullptr;
}

const Pool::Channel* Pool::get(Handle h) const { return const_cast<Pool*>(this)->get(h); }

double Pool::audibility(const ChannelInfo& i) const
{
    double a = (double)std::clamp(i.gain, 0.0f, 1.0f) * i.level;
    if (i.positional) {   // AL_INVERSE_DISTANCE_CLAMPED, rolloff 1
        const double dx = i.position[0] - listener_[0], dy = i.position[1] - listener_[1],
                     dz = i.position[2] - listener_[2];
        const double ref = std::max(i.ref_distance, 1e-3f), mx = std::max((double)i.max_distance, ref);
        const double d = std::clamp(std::sqrt(dx * dx + dy * dy + dz * dz), ref, mx);
        a *= ref / (ref + (d - ref));
    }
    return a;
}

bool Pool::before(const ChannelInfo& a, const ChannelInfo& b) const
{
    if (a.priority != b.priority) return a.priority < b.priority;
    return audibility(a) > audibility(b);
}

int Pool::free_voice() const
{
    for (size_t k = 0; k < voices_.size(); k++)
        if (voices_[k].channel < 0) return (int)k;
    return -1;
}

int Pool::least_important_real() const
{
    int worst = -1;
    for (size_t k = 0; k < voices_.size(); k++) {
        if (voices_[k].channel < 0) continue;
        if (worst < 0 || before(channels_[(size_t)voices_[(size_t)worst].channel].info,
                                channels_[(size_t)voices_[k].channel].info))
            worst = (int)k;
    }
    return worst;
}

int Pool::most_important_virtual() const
{
    int best = -1;
    for (size_t k = 0; k < channels_.size(); k++) {
        const Channel& c = channels_[k];
        if (!c.used || c.voice >= 0) continue;
        if (best < 0 || before(c.info, channels_[(size_t)best].info)) best = (int)k;
    }
    return best;
}

int Pool::victim(const ChannelInfo& want) const
{
    const int v = least_important_real();
    if (v < 0) return -1;
    const ChannelInfo& has = channels_[(size_t)voices_[(size_t)v].channel].info;
    if (want.priority < has.priority) return v;
    if (want.priority == has.priority && audibility(want) >= audibility(has)) return v;
    return -1;
}

void Pool::apply(unsigned src, const ChannelInfo& i)
{
    const al_api::Api& al = dev_->al();
    al.alSourcef(src, AL_GAIN, std::clamp(i.gain, 0.0f, 1.0f) * i.level);
    al.alSourcei(src, AL_LOOPING, i.looping ? AL_TRUE : AL_FALSE);
    if (i.positional) {
        al.alSourcei(src, AL_SOURCE_RELATIVE, AL_FALSE);
        al.alSourcefv(src, AL_POSITION, i.position);
        al.alSourcef(src, AL_REFERENCE_DISTANCE, i.ref_distance);
        al.alSourcef(src, AL_MAX_DISTANCE, i.max_distance);
        al.alSourcef(src, AL_ROLLOFF_FACTOR, 1.0f);
    } else {
        al.alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
        al.alSource3f(src, AL_POSITION, 0.f, 0.f, 0.f);
        al.alSourcef(src, AL_ROLLOFF_FACTOR, 0.0f);
    }
    if (dev_->caps().direct_channels) al.alSourcei(src, AL_DIRECT_CHANNELS_SOFT, i.direct ? AL_TRUE : AL_FALSE);
    if (reverb_) reverb_->attach(src);
}

double Pool::offset_of(const Channel& c) const
{
    const double t = c.elapsed;
    if (!c.info.looping) return std::min(t, c.info.length);
    const double ls = c.info.loop_start, le = c.info.loop_end > 0 ? c.info.loop_end : c.info.length;
    if (t < le) return t;                     // still in the first pass (the intro and the loop once)
    if (le <= ls) return 0.0;
    return ls + std::fmod(t - le, le - ls);   // then around the loop region
}

void Pool::attach(size_t slot, int v)
{
    Channel& c = channels_[slot];
    Voice& voice = voices_[(size_t)v];
    const al_api::Api& al = dev_->al();
    al.alSourceStop(voice.source);
    al.alSourcei(voice.source, AL_BUFFER, (ALint)c.buffer);
    apply(voice.source, c.info);
    const double off = offset_of(c);
    if (off > 0) al.alSourcef(voice.source, AL_SEC_OFFSET, (float)off);
    voice.channel = (int)slot;
    c.voice = v;
    c.stopped_poll = 0;
    if (c.paused) al.alSourcePause(voice.source);
    else al.alSourcePlay(voice.source);
}

void Pool::detach(size_t slot)
{
    Channel& c = channels_[slot];
    if (c.voice < 0) return;
    Voice& voice = voices_[(size_t)c.voice];
    const al_api::Api& al = dev_->al();
    float off = 0;
    al.alGetSourcef(voice.source, AL_SEC_OFFSET, &off);
    // elapsed: the first pass and the loops so far are not known from the offset alone; keep it at least here
    c.elapsed = std::max(c.elapsed, (double)off);
    al.alSourceStop(voice.source);
    al.alSourcei(voice.source, AL_BUFFER, 0);
    voice.channel = -1;
    c.voice = -1;
}

void Pool::free_channel(size_t slot)
{
    detach(slot);
    channels_[slot].used = false;
}

Handle Pool::play(unsigned buffer, const ChannelInfo& info)
{
    size_t slot = channels_.size();
    for (size_t k = 0; k < channels_.size(); k++)
        if (!channels_[k].used) {
            slot = k;
            break;
        }
    if (slot == channels_.size()) {
        // every channel in use: the least important goes (stopped, its handle stale) if the new one is at least
        // as important and as audible
        size_t worst = channels_.size();
        for (size_t k = 0; k < channels_.size(); k++)
            if (worst == channels_.size() || before(channels_[worst].info, channels_[k].info)) worst = k;
        if (worst == channels_.size() || before(channels_[worst].info, info)) {
            stats_.refused++;
            return 0;
        }
        free_channel(worst);
        stats_.channel_steals++;
        slot = worst;
    }
    Channel& c = channels_[slot];
    c.gen = (c.gen + 1) & 0x7FFFF;   // 19 bits: a handle's top bit stays clear (the sound manager tags music)
    if (!c.gen) c.gen = 1;
    c.used = true;
    c.paused = false;
    c.voice = -1;
    c.buffer = buffer;
    c.info = info;
    c.elapsed = 0.0;
    stats_.plays++;
    int v = free_voice();
    if (v < 0) {
        v = victim(info);
        if (v >= 0) {
            stats_.steals++;
            detach((size_t)voices_[(size_t)v].channel);   // the old channel goes virtual
        }
    }
    if (v >= 0) attach(slot, v);
    else stats_.virtual_starts++;
    int real = 0, used = 0;
    for (const Voice& vv : voices_) real += vv.channel >= 0;
    for (const Channel& cc : channels_) used += cc.used;
    stats_.peak_real = std::max(stats_.peak_real, real);
    stats_.peak_channels = std::max(stats_.peak_channels, used);
    return handle(slot);
}

void Pool::stop(Handle h)
{
    if (Channel* c = get(h)) free_channel((size_t)(c - channels_.data()));
}

void Pool::stop_all()
{
    for (size_t k = 0; k < channels_.size(); k++)
        if (channels_[k].used) free_channel(k);
}

void Pool::debug_stop_sources()
{
    for (const Voice& v : voices_) dev_->al().alSourceStop(v.source);
}

void Pool::pause(Handle h, bool p)
{
    Channel* c = get(h);
    if (!c || c->paused == p) return;
    c->paused = p;
    if (c->voice >= 0) {
        const unsigned src = voices_[(size_t)c->voice].source;
        const al_api::Api& al = dev_->al();
        if (p) {
            al.alSourcePause(src);
        } else {
            // a device lost while paused stopped the source and rewound it: from where its time says, unless its
            // time ran out (a one-shot: left stopped, it ends at the next update)
            ALint st = 0;
            al.alGetSourcei(src, AL_SOURCE_STATE, &st);
            if (st == AL_STOPPED) {
                if (!c->info.looping && c->elapsed >= c->info.length - kEndSlack) return;
                const double off = offset_of(*c);
                if (off > 0) al.alSourcef(src, AL_SEC_OFFSET, (float)off);
            }
            al.alSourcePlay(src);
        }
    }
}

void Pool::set_gain(Handle h, float gain)
{
    Channel* c = get(h);
    if (!c || c->info.gain == gain) return;
    c->info.gain = gain;
    if (c->voice >= 0)
        dev_->al().alSourcef(voices_[(size_t)c->voice].source, AL_GAIN, std::clamp(gain, 0.0f, 1.0f) * c->info.level);
}

void Pool::set_position(Handle h, const float pos[3])
{
    Channel* c = get(h);
    if (!c) return;
    std::copy(pos, pos + 3, c->info.position);
    if (c->voice >= 0 && c->info.positional) dev_->al().alSourcefv(voices_[(size_t)c->voice].source, AL_POSITION, pos);
}

void Pool::set_listener(const float pos[3]) { std::copy(pos, pos + 3, listener_); }

int Pool::channels_in_use() const
{
    int n = 0;
    for (const Channel& c : channels_) n += c.used;
    return n;
}

Pool::Stop Pool::stop_state(const Channel& c) const
{
    ALint st = 0;
    dev_->al().alGetSourcei(voices_[(size_t)c.voice].source, AL_SOURCE_STATE, &st);
    if (st == AL_PLAYING || st == AL_PAUSED || st == AL_INITIAL) {
        c.stopped_poll = 0;
        return Stop::sounding;
    }
    const uint32_t g = dev_->generation();
    if (g != generation_ || !dev_->connected_now()) return Stop::unsure;   // lost, or reopened and not restarted
    if (!c.info.looping && c.elapsed >= c.info.length - kEndSlack) return Stop::ended;
    // stopped early, or a loop stopped: a disconnect unless a poll begun after this finds the device connected.
    // The count rises as a poll begins: at +2 the poll begun after this one has finished.
    const uint64_t p = dev_->poll_count();
    if (!c.stopped_poll) {
        c.stopped_poll = p + 2;
        return Stop::unsure;
    }
    return p >= c.stopped_poll && dev_->connected_now() && dev_->generation() == g ? Stop::ended : Stop::unsure;
}

bool Pool::valid(Handle h) const { return get(h) != nullptr; }

bool Pool::playing(Handle h) const
{
    const Channel* c = get(h);
    if (!c) return false;
    if (c->paused) return true;
    const bool by_time = c->info.looping || c->elapsed < c->info.length;   // a virtual one, or during a loss
    if (c->voice < 0) return by_time;
    switch (stop_state(*c)) {
    case Stop::sounding: return true;
    case Stop::ended: return false;
    default: return by_time;
    }
}

bool Pool::real(Handle h) const
{
    const Channel* c = get(h);
    return c && c->voice >= 0;
}
bool Pool::paused(Handle h) const
{
    const Channel* c = get(h);
    return c && c->paused;
}
const ChannelInfo* Pool::info(Handle h) const
{
    const Channel* c = get(h);
    return c ? &c->info : nullptr;
}

bool Pool::debug_offset(Handle h, double* seconds) const
{
    const Channel* c = get(h);
    if (!c) return false;
    if (c->voice >= 0) {
        float off = 0;
        dev_->al().alGetSourcef(voices_[(size_t)c->voice].source, AL_SEC_OFFSET, &off);
        *seconds = off;
    } else {
        *seconds = offset_of(*c);
    }
    return true;
}

void Pool::update(double dt)
{
    for (size_t k = 0; k < channels_.size(); k++) {
        Channel& c = channels_[k];
        if (!c.used) continue;
        if (!c.paused) c.elapsed += dt;
        if (c.voice >= 0) {
            if (!c.paused && stop_state(c) == Stop::ended) free_channel(k);   // played to its end
        } else if (!c.paused && !c.info.looping && c.elapsed >= c.info.length) {
            free_channel(k);   // a virtual one-shot ran out
        }
    }
    // a reopened device: the real channels from where their time now is (a one-shot whose time ran out is left
    // stopped; it is taken as ended at the next update)
    if (dev_->generation() != generation_) restart_after_loss();
    // free voices to the most important virtual channels
    for (int v = free_voice(); v >= 0; v = free_voice()) {
        const int best = most_important_virtual();
        if (best < 0) break;
        attach((size_t)best, v);
        stats_.promotions++;
    }
    // a virtual channel now more important than a real one takes its voice
    for (size_t n = 0; n < voices_.size(); n++) {
        const int best = most_important_virtual(), worst = least_important_real();
        if (best < 0 || worst < 0) break;
        const ChannelInfo& b = channels_[(size_t)best].info;
        const ChannelInfo& w = channels_[(size_t)voices_[(size_t)worst].channel].info;
        const bool takes = b.priority < w.priority ||
                           (b.priority == w.priority && audibility(b) > audibility(w) * kSwapMargin);
        if (!takes) break;
        detach((size_t)voices_[(size_t)worst].channel);
        attach((size_t)best, worst);
        stats_.swaps++;
    }
}

void Pool::restart_after_loss()
{
    generation_ = dev_->generation();
    for (const Voice& v : voices_) {
        if (v.channel < 0) continue;
        const Channel& c = channels_[(size_t)v.channel];
        c.stopped_poll = 0;
        if (c.paused || (!c.info.looping && c.elapsed >= c.info.length - kEndSlack)) continue;   // ended meanwhile
        ALint st = 0;
        dev_->al().alGetSourcei(v.source, AL_SOURCE_STATE, &st);
        if (st != AL_PLAYING) {
            // where it was: its elapsed time (a disconnect stopped it and rewound nothing we can read)
            dev_->al().alSourceRewind(v.source);
            const double off = offset_of(c);
            if (off > 0) dev_->al().alSourcef(v.source, AL_SEC_OFFSET, (float)off);
            dev_->al().alSourcePlay(v.source);
            stats_.restarts++;
        }
    }
}

}  // namespace voice_pool
