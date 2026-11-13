// FFmpeg, loaded at run time from the game's own folder (docs/FFMPEG_MIGRATION_PLAN.md sections 3-4): the movie
// decoder's libraries (libavutil, libavcodec, libavformat; LGPL-2.1-or-later) are dynamically linked as their
// licence asks, and loaded by full path, as OpenAL Soft is (native/audio/al_api.h), so that no other FFmpeg on the
// system is picked up and a player can replace them with their own build. A missing or unusable library is
// reported; the caller decides what a movie does without it.
//
// The functions are those the movie player and its tests use: open a file through an I/O context of the
// caller's (the player's own file access), read its packets, decode its video and sound, and the version,
// licence and log functions. Each library's major version must be the one these headers declare (the ABI).
#pragma once
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
}

namespace ff_api {

#define FF_API_AVUTIL(X)                                                                                            \
    X(avutil_version) X(avutil_license) X(av_version_info) X(av_frame_alloc) X(av_frame_free) X(av_frame_unref)  \
    X(av_malloc) X(av_free) X(av_log_set_level) X(av_log_set_callback) X(av_log_format_line2) X(av_strerror)     \
    X(av_get_sample_fmt_name) X(av_get_pix_fmt_name) X(av_sample_fmt_is_planar)

#define FF_API_AVCODEC(X)                                                                                           \
    X(avcodec_version) X(avcodec_license) X(avcodec_find_decoder) X(avcodec_alloc_context3)                      \
    X(avcodec_free_context) X(avcodec_parameters_to_context) X(avcodec_open2) X(avcodec_send_packet)             \
    X(avcodec_receive_frame) X(avcodec_flush_buffers) X(av_packet_alloc) X(av_packet_free) X(av_packet_unref)    \
    X(avcodec_get_name)

#define FF_API_AVFORMAT(X)                                                                                          \
    X(avformat_version) X(avformat_license) X(avformat_alloc_context) X(avformat_open_input)                     \
    X(avformat_find_stream_info) X(avformat_close_input) X(av_read_frame) X(av_seek_frame) X(avio_alloc_context) \
    X(avio_context_free)

struct Api {
#define FF_API_MEMBER(name) decltype(&::name) name = nullptr;
    FF_API_AVUTIL(FF_API_MEMBER)
    FF_API_AVCODEC(FF_API_MEMBER)
    FF_API_AVFORMAT(FF_API_MEMBER)
#undef FF_API_MEMBER
    void* modules[3] = {};   // avutil, avcodec, avformat
    std::string dir;         // the folder they were loaded from
    std::string version;     // av_version_info(): "n9.0.2"
    std::string licence;     // avcodec_license()
};

// The libraries in `dir` (default: default_dir()), in dependency order. False with *error set when one is
// missing, lacks a function, or has another major version than these headers'.
bool load(Api* api, const std::string& dir, std::string* error);
void unload(Api* api);

// The library file names (this platform's), in load order.
const char* const* library_names();
// The default folder: the running executable's own (Windows), its lib/ (Linux) -- where the setup installs them.
std::string default_dir();

}  // namespace ff_api
