// The game's sound files read (wav_pcm.h).
#include "wav_pcm.h"

#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace wav_pcm {

int seek(FILE* f, uint64_t at)
{
#ifdef _WIN32
    return _fseeki64(f, (long long)at, SEEK_SET);
#else
    return fseeko(f, (off_t)at, SEEK_SET);
#endif
}

namespace {

uint64_t file_size(FILE* f)
{
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END)) return 0;
    return (uint64_t)_ftelli64(f);
#else
    if (fseeko(f, 0, SEEK_END)) return 0;
    return (uint64_t)ftello(f);
#endif
}

uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

struct Reader {
    const uint8_t* mem = nullptr;   // either memory or a file
    size_t size = 0;
    FILE* f = nullptr;
    uint64_t file_bytes = 0;
    bool read(uint64_t at, void* dst, size_t n)
    {
        if (mem) {
            if (at > size || n > size - at) return false;
            memcpy(dst, mem + at, n);
            return true;
        }
        if (seek(f, at)) return false;
        return fread(dst, 1, n, f) == n;
    }
    uint64_t total() const { return mem ? size : file_bytes; }
};

// KSDATAFORMAT_SUBTYPE_PCM's GUID after its first two bytes (WAVE_FORMAT_EXTENSIBLE)
const uint8_t kPcmGuidTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

bool parse_any(Reader& r, Info* out, std::string* error)
{
    auto fail = [error](const char* why) {
        if (error) *error = why;
        return false;
    };
    uint8_t h[12];
    if (!r.read(0, h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return fail("not a RIFF WAVE file");
    Info info;
    bool fmt = false, data = false;
    uint64_t pos = 12;
    const uint64_t end = r.total();
    uint64_t smpl_at = 0, smpl_size = 0;
    while (pos + 8 <= end) {
        uint8_t c[8];
        if (!r.read(pos, c, 8)) break;
        const uint32_t n = le32(c + 4);
        const uint64_t body = pos + 8;
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t b[40] = {};
            if (n < 16 || !r.read(body, b, n < 40 ? n : 40)) return fail("short fmt chunk");
            uint16_t tag = le16(b);
            if (tag == 0xFFFE) {   // extensible: the sub-format GUID says what the samples are
                if (n < 40) return fail("short extensible fmt chunk");
                tag = memcmp(b + 26, kPcmGuidTail, sizeof kPcmGuidTail) ? 0 : le16(b + 24);
            }
            if (tag != 1) return fail("not uncompressed PCM");
            info.channels = le16(b + 2);
            info.rate = le32(b + 4);
            info.block_align = le16(b + 12);
            info.bits = le16(b + 14);
            fmt = true;
        } else if (!memcmp(c, "data", 4)) {
            info.data_offset = body;
            info.data_bytes = n;
            if (info.data_offset + info.data_bytes > end) info.data_bytes = end - info.data_offset;   // a cut file
            data = true;
        } else if (!memcmp(c, "smpl", 4)) {
            smpl_at = body;
            smpl_size = n;
        }
        pos = body + n + (n & 1);
    }
    if (!fmt) return fail("no fmt chunk");
    if (!data) return fail("no data chunk");
    if (info.channels < 1 || info.channels > 2) return fail("not mono or stereo");
    if (info.bits != 8 && info.bits != 16) return fail("not 8- or 16-bit");
    if (info.block_align != info.channels * info.bits / 8) return fail("block alignment not channels x bytes");
    if (!info.rate) return fail("rate 0");
    info.frames = info.data_bytes / info.block_align;
    info.data_bytes = info.frames * info.block_align;
    // smpl: 36 bytes of header (the loop count at +28), then 24 bytes a loop (type +4, start +8, end +12)
    if (smpl_size >= 36 + 24) {
        uint8_t b[36], l[24];
        if (r.read(smpl_at, b, 36) && le32(b + 28) >= 1 && r.read(smpl_at + 36, l, 24)) {
            const uint64_t start = le32(l + 8), last = le32(l + 12);
            info.loop_type = le32(l + 4);
            if (start <= last && last < info.frames) {
                info.has_loop = true;
                info.loop_start = start;
                info.loop_end = last + 1;
            } else {
                info.loop_dropped = true;
            }
        }
    }
    *out = info;
    return true;
}

}  // namespace

FILE* open_utf8(const std::string& path)
{
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? (size_t)n : 1, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    return _wfopen(w.c_str(), L"rb");
#else
    return fopen(path.c_str(), "rb");
#endif
}

bool parse(const uint8_t* bytes, size_t size, Info* out, std::string* error)
{
    Reader r;
    r.mem = bytes;
    r.size = size;
    return parse_any(r, out, error);
}

bool parse_file(FILE* f, Info* out, std::string* error)
{
    Reader r;
    r.f = f;
    r.file_bytes = file_size(f);
    return parse_any(r, out, error);
}

size_t read_frames(FILE* f, const Info& info, uint64_t at, size_t frames, int16_t* out)
{
    if (at >= info.frames) return 0;
    if (frames > info.frames - at) frames = (size_t)(info.frames - at);
    if (seek(f, info.data_offset + at * info.block_align)) return 0;
    const size_t samples = frames * info.channels;
    if (info.bits == 16) {
        const size_t got = fread(out, info.block_align, frames, f);
        // the file is little-endian; so is every host this port runs on (x86-64)
        return got;
    }
    uint8_t tmp[4096];
    size_t done = 0;
    while (done < samples) {
        const size_t want = samples - done < sizeof tmp ? samples - done : sizeof tmp;
        const size_t got = fread(tmp, 1, want, f);
        for (size_t k = 0; k < got; k++) out[done + k] = (int16_t)(((int)tmp[k] - 128) << 8);
        done += got;
        if (got < want) break;
    }
    return done / info.channels;
}

bool load(const std::string& path, Info* out, std::vector<int16_t>* samples, std::string* error)
{
    FILE* f = open_utf8(path);
    if (!f) {
        if (error) *error = "cannot open";
        return false;
    }
    Info info;
    bool ok = parse_file(f, &info, error);
    if (ok) {
        samples->resize((size_t)(info.frames * info.channels));
        ok = read_frames(f, info, 0, (size_t)info.frames, samples->data()) == info.frames;
        if (!ok && error) *error = "short read";
        if (ok) *out = info;
    }
    fclose(f);
    return ok;
}

}  // namespace wav_pcm
