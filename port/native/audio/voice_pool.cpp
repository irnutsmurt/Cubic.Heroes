// The effect voices (voice_pool.h).
#include "voice_pool.h"

#include <algorithm>
#include <cmath>

namespace voice_pool {

bool Pool::create(al_device::Device& dev, int voices, int max_channels, const al_device::Reverb* reverb)
{
    destroy();
    dev_ = &dev;
    reverb_ = reverb;
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

int Pool::free_voice() const
{
    for (size_t k = 0; k < voices_.size(); k++)
        if (voices_[k].channel < 0) return (int)k;
    return -1;
}

int Pool::victim(const ChannelInfo& want) const
{
    int best = -1;
    for (size_t k = 0; k < voices_.size(); k++) {
        const Channel& c = channels_[(size_t)voices_[k].channel];
        if (c.info.priority < want.priority) continue;   // more important than the new one: kept
        if (best < 0) {
            best = (int)k;
            continue;
        }
        const Channel& b = channels_[(size_t)voices_[best].channel];
        if (c.info.priority > b.info.priority || (c.info.priority == b.info.priority && c.info.gain < b.info.gain))
            best = (int)k;
    }
    return best;
}

void Pool::apply(unsigned src, const ChannelInfo& i)
{
    const al_api::Api& al = dev_->al();
    al.alSourcef(src, AL_GAIN, std::clamp(i.gain, 0.0f, 1.0f));
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
        stats_.refused++;
        return 0;
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

void Pool::pause(Handle h, bool p)
{
    Channel* c = get(h);
    if (!c || c->paused == p) return;
    c->paused = p;
    if (c->voice >= 0) {
        const unsigned src = voices_[(size_t)c->voice].source;
        if (p) dev_->al().alSourcePause(src);
        else dev_->al().alSourcePlay(src);
    }
}

void Pool::set_gain(Handle h, float gain)
{
    Channel* c = get(h);
    if (!c || c->info.gain == gain) return;
    c->info.gain = gain;
    if (c->voice >= 0) dev_->al().alSourcef(voices_[(size_t)c->voice].source, AL_GAIN, std::clamp(gain, 0.0f, 1.0f));
}

void Pool::set_position(Handle h, const float pos[3])
{
    Channel* c = get(h);
    if (!c) return;
    std::copy(pos, pos + 3, c->info.position);
    if (c->voice >= 0 && c->info.positional) dev_->al().alSourcefv(voices_[(size_t)c->voice].source, AL_POSITION, pos);
}

bool Pool::valid(Handle h) const { return get(h) != nullptr; }
bool Pool::playing(Handle h) const
{
    const Channel* c = get(h);
    if (!c) return false;
    if (c->paused || !dev_->connected()) return true;
    if (c->voice < 0) return c->info.looping || c->elapsed < c->info.length;
    ALint st = 0;
    dev_->al().alGetSourcei(voices_[(size_t)c->voice].source, AL_SOURCE_STATE, &st);
    return st == AL_PLAYING || st == AL_PAUSED || st == AL_INITIAL;
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

void Pool::update(double dt)
{
    const al_api::Api& al = dev_->al();
    for (size_t k = 0; k < channels_.size(); k++) {
        Channel& c = channels_[k];
        if (!c.used) continue;
        if (c.voice >= 0) {
            ALint st = 0;
            al.alGetSourcei(voices_[(size_t)c.voice].source, AL_SOURCE_STATE, &st);
            if (!c.paused) c.elapsed += dt;
            if (st == AL_STOPPED && !c.paused && dev_->connected()) free_channel(k);   // played to its end
        } else if (!c.paused) {
            c.elapsed += dt;
            if (!c.info.looping && c.elapsed >= c.info.length) free_channel(k);   // a virtual one-shot ran out
        }
    }
    // virtual channels onto free voices, the most important first
    for (;;) {
        const int v = free_voice();
        if (v < 0) break;
        int best = -1;
        for (size_t k = 0; k < channels_.size(); k++) {
            const Channel& c = channels_[k];
            if (!c.used || c.voice >= 0) continue;
            if (best < 0 || c.info.priority < channels_[(size_t)best].info.priority) best = (int)k;
        }
        if (best < 0) break;
        attach((size_t)best, v);
        stats_.promotions++;
    }
}

void Pool::restart_after_loss()
{
    for (const Voice& v : voices_) {
        if (v.channel < 0) continue;
        const Channel& c = channels_[(size_t)v.channel];
        ALint st = 0;
        dev_->al().alGetSourcei(v.source, AL_SOURCE_STATE, &st);
        if (!c.paused && st != AL_PLAYING) {
            // where it was: its elapsed time (a disconnect stopped it and rewound nothing we can read)
            dev_->al().alSourceRewind(v.source);
            const double off = offset_of(c);
            if (off > 0) dev_->al().alSourcef(v.source, AL_SEC_OFFSET, (float)off);
            dev_->al().alSourcePlay(v.source);
        }
    }
}

}  // namespace voice_pool
