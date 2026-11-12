// A RIFF/WAVE reader for the game's sound files (docs/OPENAL_MIGRATION_PLAN.md section 3): uncompressed PCM
// (format 1, or WAVE_FORMAT_EXTENSIBLE carrying PCM), 8 or 16 bits, mono or stereo, any rate; the `smpl`
// chunk's first loop if there is one. Every other chunk (LIST, cue, iXML, acid, JUNK, ...) is skipped. Written
// for this project from the RIFF and WAVE format descriptions; it reads the player's own files.
//
// Loops: the game's files that loop carry one `smpl` loop each (42 of 326: music with an intro before the loop
// start, loops ending before the file's end, looping ambiences and effects). rel27 (FMOD) plays every such file
// looped from the loop's start to its end, whatever the game asks (docs/OPENAL_MIGRATION_PLAN.md section 4.1);
// the sound manager follows that, so the reader only reports the loop.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace wav_pcm {

struct Info {
    uint16_t channels = 0;
    uint32_t rate = 0;
    uint16_t bits = 0;
    uint16_t block_align = 0;      // bytes per frame
    uint64_t data_offset = 0;      // the data chunk's first byte in the file
    uint64_t data_bytes = 0;       // its length (whole frames)
    uint64_t frames = 0;
    bool has_loop = false;         // a loop in `smpl` that lies inside the data
    uint32_t loop_type = 0;        // smpl's own type field (0 forward; the game's files also carry 1024)
    uint64_t loop_start = 0;       // frames, the first played again
    uint64_t loop_end = 0;         // frames, one past the last of the loop (smpl's end is inclusive)
    bool loop_dropped = false;     // a `smpl` loop outside the data: ignored
    double seconds() const { return rate ? (double)frames / rate : 0.0; }
};

// The header of a file in memory or on disk. False with *error set when it is not a PCM WAVE file this reader
// takes; a loop outside the data is dropped (has_loop false, loop_dropped true) rather than failing the file.
bool parse(const uint8_t* bytes, size_t size, Info* out, std::string* error);
bool parse_file(FILE* f, Info* out, std::string* error);

// A whole file: its header and its samples as 16-bit PCM in host order (8-bit widened).
bool load(const std::string& path, Info* out, std::vector<int16_t>* samples, std::string* error);

// `frames` frames from frame `at` of an open file (parse_file's Info) as 16-bit PCM; the frames read.
size_t read_frames(FILE* f, const Info& info, uint64_t at, size_t frames, int16_t* out);

// Large-file positioning on every platform.
int seek(FILE* f, uint64_t at);

// Opens a path given in UTF-8 (Windows: through the wide API).
FILE* open_utf8(const std::string& path);

}  // namespace wav_pcm
