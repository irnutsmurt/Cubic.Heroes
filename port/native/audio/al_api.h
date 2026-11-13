// OpenAL Soft, loaded at run time from the game's own folder (docs/OPENAL_MIGRATION_PLAN.md section 3): the
// library is dynamically linked as its LGPL licence asks, and loaded by full path so that no other OpenAL32.dll
// on the system (a hardware router, an old install) is picked up. A missing or unusable library is reported
// and the game goes on without sound.
//
// The functions are OpenAL 1.1's plus the extensions this project uses: EFX (the dungeon reverb),
// AL_SOFT_loop_points (the sound files' own loops), ALC_EXT_disconnect, ALC_SOFT_reopen_device and
// ALC_SOFT_system_events (a device unplugged, the default output changed), AL_SOFT_direct_channels and
// ALC_SOFT_loopback (the tests: rendering without a device, faster than real time).
#pragma once
#include <string>

#define AL_NO_PROTOTYPES 1
#define ALC_NO_PROTOTYPES 1
#include "AL/al.h"
#include "AL/alc.h"
#include "AL/alext.h"
#include "AL/efx.h"

namespace al_api {

#define AL_API_FUNCTIONS(X)                                                                                         \
    X(LPALCOPENDEVICE, alcOpenDevice) X(LPALCCLOSEDEVICE, alcCloseDevice) X(LPALCCREATECONTEXT, alcCreateContext) \
    X(LPALCDESTROYCONTEXT, alcDestroyContext) X(LPALCMAKECONTEXTCURRENT, alcMakeContextCurrent)                   \
    X(LPALCGETERROR, alcGetError) X(LPALCISEXTENSIONPRESENT, alcIsExtensionPresent)                               \
    X(LPALCGETPROCADDRESS, alcGetProcAddress) X(LPALCGETSTRING, alcGetString) X(LPALCGETINTEGERV, alcGetIntegerv) \
    X(LPALGETPROCADDRESS, alGetProcAddress) X(LPALGETERROR, alGetError) X(LPALISEXTENSIONPRESENT, alIsExtensionPresent) \
    X(LPALGETSTRING, alGetString) X(LPALDISTANCEMODEL, alDistanceModel) X(LPALDOPPLERFACTOR, alDopplerFactor)    \
    X(LPALGENSOURCES, alGenSources) X(LPALDELETESOURCES, alDeleteSources) X(LPALGENBUFFERS, alGenBuffers)         \
    X(LPALDELETEBUFFERS, alDeleteBuffers) X(LPALBUFFERDATA, alBufferData) X(LPALBUFFERIV, alBufferiv)             \
    X(LPALGETBUFFERI, alGetBufferi)                                                                                 \
    X(LPALSOURCEI, alSourcei) X(LPALSOURCEF, alSourcef) X(LPALSOURCE3F, alSource3f) X(LPALSOURCE3I, alSource3i)   \
    X(LPALSOURCEFV, alSourcefv) X(LPALGETSOURCEI, alGetSourcei) X(LPALGETSOURCEF, alGetSourcef)                   \
    X(LPALSOURCEPLAY, alSourcePlay) X(LPALSOURCESTOP, alSourceStop) X(LPALSOURCEPAUSE, alSourcePause)             \
    X(LPALSOURCEREWIND, alSourceRewind)                                                                             \
    X(LPALSOURCEQUEUEBUFFERS, alSourceQueueBuffers) X(LPALSOURCEUNQUEUEBUFFERS, alSourceUnqueueBuffers)           \
    X(LPALLISTENER3F, alListener3f) X(LPALLISTENERFV, alListenerfv) X(LPALLISTENERF, alListenerf)

// Extension functions, through alGetProcAddress once a context is current; null when the library lacks the
// extension.
#define AL_API_EXT_FUNCTIONS(X)                                                                                     \
    X(LPALGENEFFECTS, alGenEffects) X(LPALDELETEEFFECTS, alDeleteEffects) X(LPALEFFECTI, alEffecti)               \
    X(LPALEFFECTF, alEffectf) X(LPALEFFECTFV, alEffectfv) X(LPALGENAUXILIARYEFFECTSLOTS, alGenAuxiliaryEffectSlots) \
    X(LPALDELETEAUXILIARYEFFECTSLOTS, alDeleteAuxiliaryEffectSlots)                                                \
    X(LPALAUXILIARYEFFECTSLOTI, alAuxiliaryEffectSloti) X(LPALAUXILIARYEFFECTSLOTF, alAuxiliaryEffectSlotf)        \
    X(LPALGENFILTERS, alGenFilters) X(LPALDELETEFILTERS, alDeleteFilters)                                         \
    X(LPALGETSOURCEI64VSOFT, alGetSourcei64vSOFT)   /* AL_SOFT_source_latency: a fed stream's heard position */

// ALC extension functions, through alcGetProcAddress (a null device asks for the library's own).
#define AL_API_ALC_EXT_FUNCTIONS(X)                                                                                 \
    X(LPALCREOPENDEVICESOFT, alcReopenDeviceSOFT) X(LPALCLOOPBACKOPENDEVICESOFT, alcLoopbackOpenDeviceSOFT)       \
    X(LPALCISRENDERFORMATSUPPORTEDSOFT, alcIsRenderFormatSupportedSOFT) X(LPALCRENDERSAMPLESSOFT, alcRenderSamplesSOFT) \
    X(LPALCEVENTCONTROLSOFT, alcEventControlSOFT) X(LPALCEVENTCALLBACKSOFT, alcEventCallbackSOFT)

struct Api {
#define AL_API_MEMBER(type, name) type name = nullptr;
    AL_API_FUNCTIONS(AL_API_MEMBER)
    AL_API_EXT_FUNCTIONS(AL_API_MEMBER)
    AL_API_ALC_EXT_FUNCTIONS(AL_API_MEMBER)
#undef AL_API_MEMBER
    void* module = nullptr;
    std::string path;      // the library loaded
    std::string version;   // alGetString(AL_VERSION), once a context exists
    std::string renderer;  // alGetString(AL_RENDERER)
};

// The library at `path` (default: default_path()). False with *error set when it is missing or lacks one of
// OpenAL 1.1's functions. The ALC extension functions are looked up at once (they need no device).
bool load(Api* api, const std::string& path, std::string* error);
// The AL extension functions and the version strings (a context current).
void load_al_extensions(Api* api);
void unload(Api* api);

// The default library path: OpenAL32.dll beside the running executable (Windows), lib/libopenal.so.1 beside
// it (Linux).
std::string default_path();

}  // namespace al_api
