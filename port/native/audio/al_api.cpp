// OpenAL Soft loaded at run time (al_api.h).
#include "al_api.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>
#endif

namespace al_api {
namespace {

void* open_module(const std::string& path)
{
#ifdef _WIN32
    // UTF-8 path; LOAD_WITH_ALTERED_SEARCH_PATH: the DLL's own dependencies from its own folder first
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? (size_t)n : 1, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    return (void*)LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* symbol(void* module, const char* name)
{
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)module, name);
#else
    return dlsym(module, name);
#endif
}

void close_module(void* module)
{
#ifdef _WIN32
    FreeLibrary((HMODULE)module);
#else
    dlclose(module);
#endif
}

}  // namespace

std::string default_path()
{
#ifdef _WIN32
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe, n);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = slash == std::wstring::npos ? std::wstring(L".") : dir.substr(0, slash);
    dir += L"\\OpenAL32.dll";
    const int m = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(m > 0 ? (size_t)m : 1, '\0');
    if (m > 0) WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, &out[0], m, nullptr, nullptr);
    out.resize(m > 0 ? (size_t)m - 1 : 0);
    return out;
#else
    char exe[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    std::string dir = n > 0 ? std::string(exe, (size_t)n) : std::string("./x");
    dir = dir.substr(0, dir.find_last_of('/'));
    return dir + "/lib/libopenal.so.1";
#endif
}

bool load(Api* api, const std::string& path, std::string* error)
{
    *api = Api{};
    api->module = open_module(path);
    if (!api->module) {
        if (error) *error = "OpenAL Soft not found at " + path;
        return false;
    }
    api->path = path;
#define AL_API_LOAD(type, name)                                                                                     \
    api->name = (type)symbol(api->module, #name);                                                                   \
    if (!api->name) {                                                                                               \
        if (error) *error = path + " has no " #name;                                                                \
        unload(api);                                                                                                \
        return false;                                                                                               \
    }
    AL_API_FUNCTIONS(AL_API_LOAD)
#undef AL_API_LOAD
#define AL_API_LOAD_ALC(type, name) api->name = (type)api->alcGetProcAddress(nullptr, #name);
    AL_API_ALC_EXT_FUNCTIONS(AL_API_LOAD_ALC)
#undef AL_API_LOAD_ALC
    return true;
}

void load_al_extensions(Api* api)
{
#define AL_API_LOAD_AL(type, name) api->name = (type)api->alGetProcAddress(#name);
    AL_API_EXT_FUNCTIONS(AL_API_LOAD_AL)
#undef AL_API_LOAD_AL
    const char* v = api->alGetString(AL_VERSION);
    const char* r = api->alGetString(AL_RENDERER);
    api->version = v ? v : "";
    api->renderer = r ? r : "";
}

void unload(Api* api)
{
    if (api->module) close_module(api->module);
    *api = Api{};
}

}  // namespace al_api
