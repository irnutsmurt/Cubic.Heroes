// The output device, its objects, samples and reverb (al_device.h).
#include "al_device.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>

namespace al_device {
namespace {

uint64_t now_ms()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

void log_stderr(const char* line)
{
    fprintf(stderr, "[audio] %s\n", line);
    fflush(stderr);
}

void Device::log(const char* fmt, ...) const
{
    if (!log_) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_(buf);
}

void ALC_APIENTRY Device::event_callback(ALCenum type, ALCenum device_type, ALCdevice* device, ALCsizei,
                                         const ALCchar* message, void* user) noexcept
{
    // OpenAL Soft's own thread: flags only
    Device* self = (Device*)user;
    if (device_type != ALC_PLAYBACK_DEVICE_SOFT) return;
    if (type == ALC_EVENT_TYPE_DEFAULT_DEVICE_CHANGED_SOFT && self->options_.device_name.empty()) {
        self->default_changed_.store(true);
    } else if (type == ALC_EVENT_TYPE_DEVICE_REMOVED_SOFT && (void*)device == self->dev_) {
        self->lost_.store(true);
    }
    (void)message;
}

bool Device::open(al_api::Api* api, const Options& options, LogFn log_fn, std::string* error)
{
    close();
    api_ = api;
    options_ = options;
    log_ = log_fn;
    const al_api::Api& al = *api_;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        close();
        return false;
    };
    attrs_.clear();
    if (options.loopback) {
        if (!al.alcLoopbackOpenDeviceSOFT || !al.alcRenderSamplesSOFT) return fail("no ALC_SOFT_loopback");
        dev_ = al.alcLoopbackOpenDeviceSOFT(nullptr);
        attrs_ = {ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT, ALC_FORMAT_TYPE_SOFT, ALC_FLOAT_SOFT, ALC_FREQUENCY,
                  options.loopback_rate};
    } else {
        dev_ = al.alcOpenDevice(options.device_name.empty() ? nullptr : options.device_name.c_str());
        if (options.frequency > 0) attrs_.insert(attrs_.end(), {ALC_FREQUENCY, options.frequency});
    }
    if (!dev_) return fail(options.loopback ? "no loopback device" : "no output device");
    ALCdevice* d = (ALCdevice*)dev_;
    attrs_.insert(attrs_.end(), {ALC_HRTF_SOFT, ALC_FALSE, ALC_MAX_AUXILIARY_SENDS, 1, 0});
    ctx_ = al.alcCreateContext(d, attrs_.data());
    if (!ctx_ || !al.alcMakeContextCurrent((ALCcontext*)ctx_)) return fail("no context");
    al_api::load_al_extensions(api_);
    caps_ = Caps{};
    caps_.efx = al.alcIsExtensionPresent(d, "ALC_EXT_EFX") && al.alGenEffects && al.alGenAuxiliaryEffectSlots;
    caps_.loop_points = al.alIsExtensionPresent("AL_SOFT_loop_points");
    caps_.direct_channels = al.alIsExtensionPresent("AL_SOFT_direct_channels");
    caps_.spatialize = al.alIsExtensionPresent("AL_SOFT_source_spatialize");
    caps_.disconnect = al.alcIsExtensionPresent(d, "ALC_EXT_disconnect");
    caps_.reopen = al.alcIsExtensionPresent(d, "ALC_SOFT_reopen_device") && al.alcReopenDeviceSOFT;
    caps_.system_events = al.alcIsExtensionPresent(nullptr, "ALC_SOFT_system_events") && al.alcEventControlSOFT &&
                          al.alcEventCallbackSOFT;
    if (caps_.efx) al.alcGetIntegerv(d, ALC_MAX_AUXILIARY_SENDS, 1, &caps_.aux_sends);
    if (caps_.efx) {   // EAX reverb (the I3DL2 model with its extensions) when the library has it
        al.alGetError();
        unsigned e = 0;
        al.alGenEffects(1, &e);
        al.alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
        caps_.eax_reverb = al.alGetError() == AL_NO_ERROR;
        al.alDeleteEffects(1, &e);
    }
    const char* n = al.alcGetString(d, options.loopback ? ALC_DEVICE_SPECIFIER : ALC_ALL_DEVICES_SPECIFIER);
    name_ = n ? n : "";
    al.alcGetIntegerv(d, ALC_FREQUENCY, 1, &frequency_);
    // the game's 3D model: inverse distance, clamped (the sound manager sets each source's reference and maximum)
    al.alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
    al.alDopplerFactor(0.0f);
    if (caps_.system_events && !options.loopback) {
        const ALCenum events[] = {ALC_EVENT_TYPE_DEFAULT_DEVICE_CHANGED_SOFT, ALC_EVENT_TYPE_DEVICE_REMOVED_SOFT};
        al.alcEventCallbackSOFT(&Device::event_callback, this);
        al.alcEventControlSOFT(2, events, ALC_TRUE);
    }
    connected_.store(true);
    lost_.store(false);
    default_changed_.store(false);
    waiting_ = false;
    log("device: %s, %d Hz; %s %s; EFX %s (%d send), loop points %s, reopen %s, events %s", name_.c_str(),
        frequency_, al.renderer.c_str(), al.version.c_str(), caps_.efx ? (caps_.eax_reverb ? "EAX reverb" : "reverb") : "no",
        caps_.aux_sends, caps_.loop_points ? "yes" : "no", caps_.reopen ? "yes" : "no", caps_.system_events ? "yes" : "no");
    return true;
}

void Device::close()
{
    if (!api_) return;
    const al_api::Api& al = *api_;
    const Live l = live();
    if (l.any() && ctx_)
        log("shutdown: %d sources, %d buffers, %d effects, %d slots still live (a leak)", l.sources, l.buffers,
            l.effects, l.slots);
    if (caps_.system_events && al.alcEventCallbackSOFT && !options_.loopback && dev_) {
        const ALCenum events[] = {ALC_EVENT_TYPE_DEFAULT_DEVICE_CHANGED_SOFT, ALC_EVENT_TYPE_DEVICE_REMOVED_SOFT};
        al.alcEventControlSOFT(2, events, ALC_FALSE);
        al.alcEventCallbackSOFT(nullptr, nullptr);
    }
    if (ctx_) {
        al.alcMakeContextCurrent(nullptr);
        al.alcDestroyContext((ALCcontext*)ctx_);   // releases whatever was left
    }
    if (dev_) al.alcCloseDevice((ALCdevice*)dev_);
    ctx_ = dev_ = nullptr;
    connected_.store(false);
    sources_ = buffers_ = effects_ = slots_ = 0;
    api_ = nullptr;
}

Stats Device::stats() const
{
    Stats s;
    s.disconnects = disconnects_.load();
    s.default_changes = default_changes_.load();
    s.reopens = reopens_.load();
    s.reopen_failures = reopen_failures_.load();
    return s;
}

bool Device::reopen()
{
    const al_api::Api& al = *api_;
    if (!caps_.reopen || options_.loopback) return false;
    const char* name = options_.device_name.empty() ? nullptr : options_.device_name.c_str();
    if (!al.alcReopenDeviceSOFT((ALCdevice*)dev_, name, attrs_.data())) return false;
    const char* n = al.alcGetString((ALCdevice*)dev_, ALC_ALL_DEVICES_SPECIFIER);
    name_ = n ? n : "";
    al.alcGetIntegerv((ALCdevice*)dev_, ALC_FREQUENCY, 1, &frequency_);
    return true;
}

uint32_t Device::poll()
{
    if (!ctx_ || options_.loopback) return generation_.load();
    const al_api::Api& al = *api_;
    if (!waiting_) {
        ALCint c = 1;
        if (caps_.disconnect) al.alcGetIntegerv((ALCdevice*)dev_, ALC_CONNECTED, 1, &c);
        const bool lost = !c || lost_.exchange(false);
        const bool moved = default_changed_.exchange(false);
        if (lost) {
            disconnects_++;
            connected_.store(false);
            log("device lost: %s", name_.c_str());
            waiting_ = true;
            next_retry_ms_ = 0;
        } else if (moved) {
            default_changes_++;
            log("the default output changed: moving to it");
            waiting_ = true;
            next_retry_ms_ = 0;
        }
    }
    if (waiting_ && now_ms() >= next_retry_ms_) {
        if (reopen()) {
            waiting_ = false;
            connected_.store(true);
            reopens_++;
            generation_++;
            log("device reopened: %s, %d Hz", name_.c_str(), frequency_);
        } else {
            const uint64_t f = ++reopen_failures_;
            if (f == 1 || f % 30 == 0) log("device reopen failed (%llu so far); retrying", (unsigned long long)f);
            next_retry_ms_ = now_ms() + options_.reopen_retry_ms;
        }
    }
    return generation_.load();
}

void Device::render(float* out, int frames)
{
    if (options_.loopback && api_ && dev_) api_->alcRenderSamplesSOFT((ALCdevice*)dev_, out, frames);
}

unsigned Device::new_source()
{
    unsigned id = 0;
    api_->alGetError();
    api_->alGenSources(1, &id);
    if (api_->alGetError() != AL_NO_ERROR) return 0;
    sources_++;
    if (caps_.efx) api_->alSource3i(id, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
    return id;
}

void Device::delete_source(unsigned& id)
{
    if (!id || !api_) return;
    api_->alSourceStop(id);
    api_->alSourcei(id, AL_BUFFER, 0);
    api_->alDeleteSources(1, &id);
    sources_--;
    id = 0;
}

unsigned Device::new_buffer()
{
    unsigned id = 0;
    api_->alGetError();
    api_->alGenBuffers(1, &id);
    if (api_->alGetError() != AL_NO_ERROR) return 0;
    buffers_++;
    return id;
}

void Device::delete_buffer(unsigned& id)
{
    if (!id || !api_) return;
    api_->alGetError();
    api_->alDeleteBuffers(1, &id);
    if (api_->alGetError() != AL_NO_ERROR) log("buffer %u still in use at delete", id);
    else buffers_--;
    id = 0;
}

unsigned Device::new_effect()
{
    if (!caps_.efx) return 0;
    unsigned id = 0;
    api_->alGetError();
    api_->alGenEffects(1, &id);
    if (api_->alGetError() != AL_NO_ERROR) return 0;
    effects_++;
    return id;
}

void Device::delete_effect(unsigned& id)
{
    if (!id || !api_) return;
    api_->alDeleteEffects(1, &id);
    effects_--;
    id = 0;
}

unsigned Device::new_slot()
{
    if (!caps_.efx) return 0;
    unsigned id = 0;
    api_->alGetError();
    api_->alGenAuxiliaryEffectSlots(1, &id);
    if (api_->alGetError() != AL_NO_ERROR) return 0;
    slots_++;
    return id;
}

void Device::delete_slot(unsigned& id)
{
    if (!id || !api_) return;
    api_->alDeleteAuxiliaryEffectSlots(1, &id);
    slots_--;
    id = 0;
}

Live Device::live() const
{
    Live l;
    l.sources = sources_.load();
    l.buffers = buffers_.load();
    l.effects = effects_.load();
    l.slots = slots_.load();
    return l;
}

// ---- samples ----

namespace {

// One AL buffer of `frames` frames, its loop points from `info`; 0 on failure.
unsigned make_buffer(Device& dev, const std::string& path, const wav_pcm::Info& info, const int16_t* pcm, bool stereo,
                     bool* loop_points_set, std::string* error)
{
    const al_api::Api& al = dev.al();
    unsigned b = dev.new_buffer();
    if (!b) {
        if (error) *error = "no AL buffer";
        return 0;
    }
    al.alGetError();
    al.alBufferData(b, stereo ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16, pcm,
                    (ALsizei)(info.frames * (stereo ? 2 : 1) * sizeof(int16_t)), (ALsizei)info.rate);
    if (al.alGetError() != AL_NO_ERROR) {
        if (error) *error = "alBufferData refused the samples";
        dev.delete_buffer(b);
        return 0;
    }
    if (info.has_loop && dev.caps().loop_points) {
        const ALint lp[2] = {(ALint)info.loop_start, (ALint)info.loop_end};
        al.alBufferiv(b, AL_LOOP_POINTS_SOFT, lp);
        const bool ok = al.alGetError() == AL_NO_ERROR;
        if (loop_points_set) *loop_points_set = ok;
        if (!ok) dev.log("%s: loop points refused (%d-%d)", path.c_str(), lp[0], lp[1]);
    }
    return b;
}

}  // namespace

bool load_sample(Device& dev, const std::string& path, Sample* out, std::string* error, bool centre_stereo)
{
    *out = Sample{};
    std::vector<int16_t> pcm;
    if (!wav_pcm::load(path, &out->info, &pcm, error)) return false;
    const bool stereo = out->info.channels == 2;
    out->buffer = make_buffer(dev, path, out->info, pcm.data(), stereo, &out->loop_points_set, error);
    if (!out->buffer) return false;
    if (centre_stereo && !stereo) {
        std::vector<int16_t> dup(pcm.size() * 2);
        for (size_t i = 0; i < pcm.size(); i++) dup[2 * i] = dup[2 * i + 1] = pcm[i];
        out->centre_stereo = make_buffer(dev, path, out->info, dup.data(), true, nullptr, error);
        if (!out->centre_stereo) {
            dev.delete_buffer(out->buffer);
            return false;
        }
    }
    return true;
}

void free_sample(Device& dev, Sample* s)
{
    dev.delete_buffer(s->buffer);
    dev.delete_buffer(s->centre_stereo);
}

// ---- reverb ----

bool Reverb::create(Device& dev, const ReverbParams& p)
{
    destroy();
    if (!dev.caps().efx) return false;
    dev_ = &dev;
    eax_ = dev.caps().eax_reverb;
    effect_ = dev.new_effect();
    slot_ = dev.new_slot();
    if (!effect_ || !slot_) {
        destroy();
        return false;
    }
    dev.al().alEffecti(effect_, AL_EFFECT_TYPE, eax_ ? AL_EFFECT_EAXREVERB : AL_EFFECT_REVERB);
    set(p);
    on_ = false;
    return true;
}

void Reverb::set(const ReverbParams& p)
{
    if (!effect_) return;
    const al_api::Api& al = dev_->al();
    if (eax_) {
        al.alEffectf(effect_, AL_EAXREVERB_DENSITY, p.density);
        al.alEffectf(effect_, AL_EAXREVERB_DIFFUSION, p.diffusion);
        al.alEffectf(effect_, AL_EAXREVERB_GAIN, p.gain);
        al.alEffectf(effect_, AL_EAXREVERB_GAINHF, p.gain_hf);
        al.alEffectf(effect_, AL_EAXREVERB_GAINLF, p.gain_lf);
        al.alEffectf(effect_, AL_EAXREVERB_DECAY_TIME, p.decay_time);
        al.alEffectf(effect_, AL_EAXREVERB_DECAY_HFRATIO, p.decay_hf_ratio);
        al.alEffectf(effect_, AL_EAXREVERB_DECAY_LFRATIO, p.decay_lf_ratio);
        al.alEffectf(effect_, AL_EAXREVERB_REFLECTIONS_GAIN, p.reflections_gain);
        al.alEffectf(effect_, AL_EAXREVERB_REFLECTIONS_DELAY, p.reflections_delay);
        al.alEffectf(effect_, AL_EAXREVERB_LATE_REVERB_GAIN, p.late_gain);
        al.alEffectf(effect_, AL_EAXREVERB_LATE_REVERB_DELAY, p.late_delay);
        al.alEffectf(effect_, AL_EAXREVERB_AIR_ABSORPTION_GAINHF, p.air_absorption_gain_hf);
        al.alEffectf(effect_, AL_EAXREVERB_HFREFERENCE, p.hf_reference);
        al.alEffectf(effect_, AL_EAXREVERB_LFREFERENCE, p.lf_reference);
        al.alEffectf(effect_, AL_EAXREVERB_ROOM_ROLLOFF_FACTOR, p.room_rolloff);
        al.alEffecti(effect_, AL_EAXREVERB_DECAY_HFLIMIT, p.decay_hf_limit ? AL_TRUE : AL_FALSE);
    } else {
        al.alEffectf(effect_, AL_REVERB_DENSITY, p.density);
        al.alEffectf(effect_, AL_REVERB_DIFFUSION, p.diffusion);
        al.alEffectf(effect_, AL_REVERB_GAIN, p.gain);
        al.alEffectf(effect_, AL_REVERB_GAINHF, p.gain_hf);
        al.alEffectf(effect_, AL_REVERB_DECAY_TIME, p.decay_time);
        al.alEffectf(effect_, AL_REVERB_DECAY_HFRATIO, p.decay_hf_ratio);
        al.alEffectf(effect_, AL_REVERB_REFLECTIONS_GAIN, p.reflections_gain);
        al.alEffectf(effect_, AL_REVERB_REFLECTIONS_DELAY, p.reflections_delay);
        al.alEffectf(effect_, AL_REVERB_LATE_REVERB_GAIN, p.late_gain);
        al.alEffectf(effect_, AL_REVERB_LATE_REVERB_DELAY, p.late_delay);
        al.alEffectf(effect_, AL_REVERB_AIR_ABSORPTION_GAINHF, p.air_absorption_gain_hf);
        al.alEffectf(effect_, AL_REVERB_ROOM_ROLLOFF_FACTOR, p.room_rolloff);
        al.alEffecti(effect_, AL_REVERB_DECAY_HFLIMIT, p.decay_hf_limit ? AL_TRUE : AL_FALSE);
    }
    if (on_) al.alAuxiliaryEffectSloti(slot_, AL_EFFECTSLOT_EFFECT, (ALint)effect_);   // the slot takes a copy
}

void Reverb::enable(bool on)
{
    if (!slot_) return;
    on_ = on;
    dev_->al().alAuxiliaryEffectSloti(slot_, AL_EFFECTSLOT_EFFECT, on ? (ALint)effect_ : AL_EFFECT_NULL);
}

void Reverb::attach(unsigned source) const
{
    if (!slot_ || !source) return;
    dev_->al().alSource3i(source, AL_AUXILIARY_SEND_FILTER, (ALint)slot_, 0, AL_FILTER_NULL);
}

void Reverb::destroy()
{
    if (!dev_) return;
    if (slot_) dev_->al().alAuxiliaryEffectSloti(slot_, AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
    dev_->delete_slot(slot_);
    dev_->delete_effect(effect_);
    dev_ = nullptr;
    on_ = false;
}

}  // namespace al_device
