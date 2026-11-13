// M8.0's trace of the movies (docs/FFMPEG_MIGRATION_PLAN.md section 2): a development lift observes the game's movie
// class, Bink's API as the game reaches it, the movie textures' creation, the functions that build a movie's path,
// the asp engine's movie schedule manager and the Lua binding named Ending (config/m8_movie_trace.json,
// tools/m8_trace_lift.sh). With DOTHEROES_MOVIE_TRACE=<file>, each observed call is written to the file: on entry
// the time, the game's frame (the flips seen), the thread, the caller, the argument registers, the strings and the
// first 64 bytes of what they point to; on return the result and the same objects again -- what each call reads
// and changes. Nothing is changed: the lifted bodies run as they are (the observer answers 2: run the body, then
// report its exit). Without the variable, or in a lift without these observers, nothing here runs.
#include "ppu_context.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" uint8_t* vm_base;
extern "C" unsigned r1_frame_count(void);   // native/render/r1_observe.cpp: the flips seen

namespace {

struct Traced {
    uint32_t addr;
    const char* what;
};
// config/m8_movie_trace.json
const Traced kTraced[] = {
    {0x0045CDD8u, "class"}, {0x0045CE64u, "class"}, {0x0045CEE0u, "class"}, {0x0045CF3Cu, "class"},
    {0x0045CFF8u, "class"}, {0x0045D088u, "class"}, {0x0045D2F8u, "class"},
    {0x00A58F18u, "bink"}, {0x00A59358u, "bink"}, {0x00A5B438u, "bink-open"}, {0x00A56EF0u, "bink"},
    {0x00A560E8u, "bink"}, {0x00A56B20u, "bink"}, {0x00A5A178u, "bink"}, {0x00A56FE8u, "bink"},
    {0x00A5B158u, "bink"}, {0x00A5D138u, "bink"}, {0x00A5D960u, "bink"},
    {0x0045AE14u, "textures"}, {0x0045B55Cu, "textures"},
    {0x0048C424u, "path"}, {0x004A9814u, "path"}, {0x00585BF0u, "path"}, {0x005CD374u, "path"},
    {0x006E5C50u, "schedule"}, {0x006E6458u, "schedule"}, {0x006E67ACu, "schedule"}, {0x006E68C8u, "schedule"},
    {0x004F0EB4u, "lua-Ending"},
};

FILE* s_out;
std::mutex s_lock;
const auto s_t0 = std::chrono::steady_clock::now();

// DOTHEROES_MOVIE_PLANES=<file> (M8.2's picture comparison): each decoded frame's planes hashed as
// tests/native/ffmpeg_decode_check hashes FFmpeg's (FNV-1a over Y, then Cb, then Cr, row by row): "<movie> <frame>
// <hash>", from Bink's decode in a lift that observes it (its open, its take-buffers and its decode) and from the
// native player's (bink_native.cpp)
FILE* s_planes;
struct BinkMovie {
    uint32_t handle, fb;
    char name[64];
};
BinkMovie s_bink[8];

inline uint32_t rd32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

BinkMovie* bink_movie(uint32_t handle, bool make)
{
    for (BinkMovie& b : s_bink)
        if (b.handle == handle) return &b;
    if (!make) return nullptr;
    for (BinkMovie& b : s_bink)
        if (!b.handle) { b.handle = handle; return &b; }
    return nullptr;
}

const char* traced(uint32_t addr)
{
    for (const Traced& t : kTraced)
        if (t.addr == addr) return t.what;
    return nullptr;
}

double now_s()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - s_t0).count();
}

bool guest_ptr(uint64_t v)
{
    return v >= 0x10000u && v < 0x40000000u;
}

// a printable string at v (at least 4 characters, ended within 160 bytes), else false
bool guest_str(uint64_t v, char* out, size_t n)
{
    if (!guest_ptr(v)) return false;
    const uint8_t* p = vm_base + (uint32_t)v;
    size_t i = 0;
    for (; i < 160 && p[i]; i++)
        if (p[i] < 0x20 || p[i] > 0x7e) return false;
    if (i < 4 || i == 160) return false;
    snprintf(out, n, "%.*s", (int)i, (const char*)p);
    return true;
}

void dump(const char* label, uint64_t v, unsigned bytes)
{
    if (!guest_ptr(v)) return;
    fprintf(s_out, "  %s %08X:", label, (uint32_t)v);
    const uint8_t* p = vm_base + (uint32_t)v;
    for (unsigned i = 0; i < bytes; i += 4)
        fprintf(s_out, " %02X%02X%02X%02X", p[i], p[i + 1], p[i + 2], p[i + 3]);
    fputc('\n', s_out);
}

// the entries of the calls in progress on each thread, for their exits
struct Frame {
    uint32_t addr;
    uint64_t r[6];   // r3..r8 at entry
    double t;
};
thread_local Frame t_stack[32];
thread_local int t_depth;

}  // namespace

// the planes of the set the description at fb holds (its +0x14), hashed and written (DOTHEROES_MOVIE_PLANES)
extern "C" void movie_planes_record(const char* name, int frame, uint32_t fb)
{
    if (!s_planes || !fb) return;
    const uint32_t set = fb + 0x18 + rd32(fb + 0x14) * 0x30;
    const uint32_t w = rd32(fb + 4), h = rd32(fb + 8), cw = rd32(fb + 0x0C), ch = rd32(fb + 0x10);
    uint64_t hash = 1469598103934665603ull;
    const struct { uint32_t at, w, h; } planes[3] = {{set + 0x00, w, h}, {set + 0x18, cw, ch}, {set + 0x0C, cw, ch}};
    for (const auto& pl : planes) {   // Y, cB (Cb), cR (Cr)
        const uint32_t buf = rd32(pl.at + 4), pitch = rd32(pl.at + 8);
        if (!buf || buf >= 0x40000000u) return;
        for (uint32_t r = 0; r < pl.h; r++) {
            const uint8_t* p = vm_base + buf + (size_t)r * pitch;
            for (uint32_t i = 0; i < pl.w; i++) hash = (hash ^ p[i]) * 1099511628211ull;
        }
    }
    std::lock_guard<std::mutex> g(s_lock);
    fprintf(s_planes, "%s %d %016llx\n", name, frame, (unsigned long long)hash);
}

extern "C" int movie_trace_wanted(void)
{
    static int wanted = -1;
    if (wanted < 0) {
        const char* pl = getenv("DOTHEROES_MOVIE_PLANES");
        if (pl && *pl) {
            s_planes = fopen(pl, "w");
            if (!s_planes) fprintf(stderr, "[movie-trace] cannot write %s\n", pl);
        }
        const char* p = getenv("DOTHEROES_MOVIE_TRACE");
        wanted = s_planes ? 1 : 0;
        if (p && *p) {
            s_out = fopen(p, "w");
            if (s_out) {
                wanted = 1;
                fprintf(s_out, "# M8.0 movie trace: E entry, X exit (time s, frame, thread, address, what, ...)\n");
                fflush(s_out);
            } else {
                fprintf(stderr, "[movie-trace] cannot write %s\n", p);
            }
        }
    }
    return wanted;
}

// Bink's functions the planes' record follows (DOTHEROES_MOVIE_PLANES without the trace)
static bool planes_addr(uint32_t addr)
{
    return addr == 0x00A5B438u || addr == 0x00A56FE8u || addr == 0x00A5B158u;
}

// -1: not a traced address (the other observers' turn); 2: run the body, then report its exit
extern "C" int movie_trace_observe(uint32_t addr, ppu_context* ctx)
{
    const char* what = traced(addr);
    if (!what || (!s_out && !(s_planes && planes_addr(addr)))) return -1;
    const double t = now_s();
    if (t_depth < 32) {
        Frame& f = t_stack[t_depth];
        f.addr = addr;
        for (int i = 0; i < 6; i++) f.r[i] = ctx->gpr[3 + i];
        f.t = t;
    }
    t_depth++;
    if (s_planes && addr == 0x00A56FE8u) {   // Bink's take-buffers: the handle's texture sets
        std::lock_guard<std::mutex> g(s_lock);
        if (BinkMovie* b = bink_movie((uint32_t)ctx->gpr[3], false)) b->fb = (uint32_t)ctx->gpr[4];
    }
    if (!s_out) return 2;
    std::lock_guard<std::mutex> g(s_lock);
    fprintf(s_out, "E %.6f %u %u %08X %s lr=%08X", t, r1_frame_count(), (unsigned)ctx->thread_id, addr, what,
            (uint32_t)ctx->lr);
    for (int r = 3; r <= 10; r++) fprintf(s_out, " r%d=%llX", r, (unsigned long long)ctx->gpr[r]);
    fprintf(s_out, " f1=%.9g f2=%.9g\n", ctx->fpr[1], ctx->fpr[2]);
    char s[192];
    for (int r = 3; r <= 8; r++) {
        if (guest_str(ctx->gpr[r], s, sizeof s)) fprintf(s_out, "  s r%d \"%s\"\n", r, s);
        else if (r <= 5) {
            char l[8];
            snprintf(l, sizeof l, "m r%d", r);
            dump(l, ctx->gpr[r], 64);
        }
    }
    return 2;
}

// 1 when the exit was a traced call's
extern "C" int movie_trace_exit(uint32_t addr, ppu_context* ctx)
{
    if (!traced(addr) || (!s_out && !(s_planes && planes_addr(addr)))) return 0;
    const double t = now_s();
    if (t_depth > 0) t_depth--;
    const Frame* f = (t_depth < 32 && t_stack[t_depth].addr == addr) ? &t_stack[t_depth] : nullptr;
    if (s_planes && f && addr == 0x00A5B438u && ctx->gpr[3]) {   // Bink's open: the handle's movie
        char path[192];
        std::lock_guard<std::mutex> g(s_lock);
        if (BinkMovie* b = bink_movie((uint32_t)ctx->gpr[3], true)) {
            b->fb = 0;
            const char* base = guest_str(f->r[0], path, sizeof path) ? strrchr(path, '/') : nullptr;
            snprintf(b->name, sizeof b->name, "%s", base ? base + 1 : "?");
        }
    }
    if (s_planes && f && addr == 0x00A5B158u) {   // Bink's decode: the set it wrote
        BinkMovie* b;
        {
            std::lock_guard<std::mutex> g(s_lock);
            b = bink_movie((uint32_t)f->r[0], false);
        }
        if (b) movie_planes_record(b->name, (int)rd32((uint32_t)f->r[0] + 0x10), b->fb);
    }
    if (!s_out) return 1;
    std::lock_guard<std::mutex> g(s_lock);
    fprintf(s_out, "X %.6f %u %u %08X r3=%llX f1=%.9g dt=%.0fus\n", t, r1_frame_count(), (unsigned)ctx->thread_id,
            addr, (unsigned long long)ctx->gpr[3], ctx->fpr[1], f ? (t - f->t) * 1e6 : -1.0);
    if (f) {
        for (int r = 0; r < 3; r++) {
            char l[16];
            snprintf(l, sizeof l, "after r%d", 3 + r);
            dump(l, f->r[r], 64);
        }
    }
    if (!f || ctx->gpr[3] != f->r[0]) dump("ret", ctx->gpr[3], 96);
    fflush(s_out);
    return 1;
}
