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

extern "C" int movie_trace_wanted(void)
{
    static int wanted = -1;
    if (wanted < 0) {
        const char* p = getenv("DOTHEROES_MOVIE_TRACE");
        wanted = 0;
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

// -1: not a traced address (the other observers' turn); 2: run the body, then report its exit
extern "C" int movie_trace_observe(uint32_t addr, ppu_context* ctx)
{
    const char* what = traced(addr);
    if (!what || !s_out) return -1;
    const double t = now_s();
    if (t_depth < 32) {
        Frame& f = t_stack[t_depth];
        f.addr = addr;
        for (int i = 0; i < 6; i++) f.r[i] = ctx->gpr[3 + i];
        f.t = t;
    }
    t_depth++;
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
    if (!traced(addr) || !s_out) return 0;
    const double t = now_s();
    if (t_depth > 0) t_depth--;
    const Frame* f = (t_depth < 32 && t_stack[t_depth].addr == addr) ? &t_stack[t_depth] : nullptr;
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
