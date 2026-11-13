// FFmpeg loaded at run time (ff_api.h).
#include "ff_api.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>
#endif

namespace ff_api {
namespace {

#ifdef _WIN32
const char* const kNames[] = {"avutil-61.dll", "avcodec-63.dll", "avformat-63.dll", nullptr};
const char kSep = '\\';
#else
const char* const kNames[] = {"libavutil.so.61", "libavcodec.so.63", "libavformat.so.63", nullptr};
const char kSep = '/';
#endif
const unsigned kMajor[] = {LIBAVUTIL_VERSION_MAJOR, LIBAVCODEC_VERSION_MAJOR, LIBAVFORMAT_VERSION_MAJOR};

void* open_module(const std::string& path)
{
#ifdef _WIN32
    // UTF-8 path; LOAD_WITH_ALTERED_SEARCH_PATH: a library's own dependencies (avcodec's avutil) from its folder
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? (size_t)n : 1, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    return (void*)LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    // the libraries' runpath is $ORIGIN: avformat finds the avcodec and avutil beside it
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

std::string exe_dir()
{
#ifdef _WIN32
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe, n);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = slash == std::wstring::npos ? std::wstring(L".") : dir.substr(0, slash);
    const int m = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(m > 0 ? (size_t)m : 1, '\0');
    if (m > 0) WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, &out[0], m, nullptr, nullptr);
    out.resize(m > 0 ? (size_t)m - 1 : 0);
    return out;
#else
    char exe[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    std::string dir = n > 0 ? std::string(exe, (size_t)n) : std::string("./x");
    return dir.substr(0, dir.find_last_of('/'));
#endif
}

}  // namespace

const char* const* library_names()
{
    return kNames;
}

std::string default_dir()
{
#ifdef _WIN32
    return exe_dir();
#else
    return exe_dir() + "/lib";
#endif
}

bool load(Api* api, const std::string& dir, std::string* error)
{
    *api = Api{};
    api->dir = dir;
    for (int i = 0; i < 3; i++) {
        const std::string path = dir + kSep + kNames[i];
        api->modules[i] = open_module(path);
        if (!api->modules[i]) {
            if (error) *error = "FFmpeg: " + path + " not found or not loadable";
            unload(api);
            return false;
        }
    }
#define FF_API_LOAD(lib, name)                                                                                      \
    api->name = (decltype(api->name))symbol(api->modules[lib], #name);                                              \
    if (!api->name) {                                                                                               \
        if (error) *error = std::string("FFmpeg: ") + kNames[lib] + " has no " #name;                                \
        unload(api);                                                                                                \
        return false;                                                                                               \
    }
#define FF_API_LOAD_UTIL(name) FF_API_LOAD(0, name)
#define FF_API_LOAD_CODEC(name) FF_API_LOAD(1, name)
#define FF_API_LOAD_FORMAT(name) FF_API_LOAD(2, name)
    FF_API_AVUTIL(FF_API_LOAD_UTIL)
    FF_API_AVCODEC(FF_API_LOAD_CODEC)
    FF_API_AVFORMAT(FF_API_LOAD_FORMAT)
#undef FF_API_LOAD_UTIL
#undef FF_API_LOAD_CODEC
#undef FF_API_LOAD_FORMAT
#undef FF_API_LOAD
    // the ABI: each library's major version is the one these headers were written against
    const unsigned got[] = {api->avutil_version() >> 16, api->avcodec_version() >> 16, api->avformat_version() >> 16};
    for (int i = 0; i < 3; i++) {
        if (got[i] != kMajor[i]) {
            if (error)
                *error = std::string("FFmpeg: ") + kNames[i] + " is major version " + std::to_string(got[i]) +
                         ", the game needs " + std::to_string(kMajor[i]);
            unload(api);
            return false;
        }
    }
    const char* v = api->av_version_info();
    const char* l = api->avcodec_license();
    api->version = v ? v : "";
    api->licence = l ? l : "";
    return true;
}

void unload(Api* api)
{
    for (int i = 2; i >= 0; i--)
        if (api->modules[i]) close_module(api->modules[i]);
    *api = Api{};
}

}  // namespace ff_api
