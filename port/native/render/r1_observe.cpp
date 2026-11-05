// R1's boundary capture (docs/NATIVE_PORT.md section 5): every call of the
// libgcm entry points in config/dev_capture_observers.json, as PPFX made it,
// logged with its arguments. Only a development lift calls in here
// (tools/r1_dev_lift.sh, ppu_lifter.py --entry-observers); the production lift
// has no observers and never references this file.
//
// DOTHEROES_R1_DIR=<dir> and DOTHEROES_R1_AT=<s>: from <s> - 1 seconds after
// the process started until the consumption capture has ended
// (g_r1_capture_done, rsx_draw_engine.c) and <s> + 1 has passed -- at most a
// minute -- each observed call goes to <dir>/boundary.txt:
//   C <frame> <addr> <lr> <tid> <ring ea> r3..r10 f1..f4
// then, for an argument register holding a guest address, the bytes there:
//   B <reg> <ea> <hex>
// <frame> counts the flips the game has asked for (calls of the flip,
// 0x0069DD60). The walker's frame numbers differ from these by a constant
// that differs from run to run (they start counting at different points), so
// a call is joined with the draw that consumed it by <ring ea>, the GCM
// context's current pointer: where the call's commands go
// (tools/r1_boundary_check.py). Game memory: keep it local.

#include "ppu_context.h"   // runtime/ppu: the layout the generated code shares

#include <atomic>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" uint8_t* vm_base;
extern "C" volatile int g_r1_capture_done;   // rsx_draw_engine.c

extern "C" {
int g_ppu_entry_observe = 0;
}

namespace {

std::mutex s_lock;
FILE* s_out;
double s_from = -1, s_to = -1;
std::atomic<uint32_t> s_frame{0};
int s_state = -1;   // -1 unread, 0 off, 1 waiting, 2 logging, 3 done

// Seconds since this process was created: the clock DOTHEROES_R1_AT names,
// shared with the consumption capture.
double r1_seconds()
{
#ifdef _WIN32
    FILETIME c, e, k, u, now;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    GetSystemTimeAsFileTime(&now);
    const uint64_t a = ((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime;
    const uint64_t b = ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
    return (double)(b - a) * 1e-7;
#else
    return 0;
#endif
}

bool guest_ptr(uint64_t v) { return v >= 0x10000u && v < 0xE0000000u; }

// Bytes logged behind an argument: enough for the structures and blocks
// these calls take (a vertex program's microcode is the largest).
uint32_t blob_bytes(uint32_t addr)
{
    switch (addr) {
    case 0x006A4E50u: return 2048;   // vertex program: the program and its microcode
    case 0x006A39EC: return 512;     // vertex program constants
    case 0x0069FC38: return 256;     // inline transfer source
    default: return 64;
    }
}

void open_log()
{
    const char* dir = getenv("DOTHEROES_R1_DIR");
    const char* at = getenv("DOTHEROES_R1_AT");
    if (!dir || !*dir || !at || !*at) { s_state = 0; return; }
    char path[1024];
    snprintf(path, sizeof path, "%s/boundary.txt", dir);
#ifdef _WIN32
    CreateDirectoryA(dir, NULL);
#endif
    s_out = fopen(path, "w");
    if (!s_out) { s_state = 0; return; }
    s_from = atof(at) - 1.0;
    s_to = atof(at) + 1.0;
    fprintf(s_out, "# r1 boundary log: %.1f-%.1f s\n", s_from, s_to);
    s_state = 1;
}

}  // namespace

extern "C" void ppu_entry_observe(uint32_t addr, ppu_context* ctx)
{
    if (addr == 0x0069DD60u) s_frame.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> g(s_lock);
    if (s_state == 0 || s_state == 3) return;
    if (s_state < 0) { open_log(); if (s_state == 0) return; }
    const double t = r1_seconds();
    if (t < s_from) return;
    if (t >= s_to && (g_r1_capture_done || t >= s_from + 61.0)) {
        fprintf(s_out, "# end at %.3f s\n", t);
        fclose(s_out); s_out = NULL; s_state = 3;
        g_ppu_entry_observe = 0;
        return;
    }
    s_state = 2;
    const uint32_t r3 = (uint32_t)ctx->gpr[3];
    const uint32_t ring = guest_ptr(r3) ? __builtin_bswap32(*(const uint32_t*)(vm_base + r3 + 8)) : 0;
    fprintf(s_out, "C %u %08X %08X %u %08X", s_frame.load(std::memory_order_relaxed), addr,
            (uint32_t)ctx->lr, (unsigned)ctx->thread_id, ring);
    for (int r = 3; r <= 10; r++) fprintf(s_out, " %llX", (unsigned long long)ctx->gpr[r]);
    for (int f = 1; f <= 4; f++) fprintf(s_out, " %.9g", ctx->fpr[f]);
    fputc('\n', s_out);
    const uint32_t nb = blob_bytes(addr);
    for (int r = 4; r <= 10; r++) {
        const uint64_t v = ctx->gpr[r];
        if (!guest_ptr(v)) continue;
        fprintf(s_out, "B %d %08X ", r, (uint32_t)v);
        const uint8_t* p = vm_base + (uint32_t)v;
        for (uint32_t i = 0; i < nb; i++) fprintf(s_out, "%02X", p[i]);
        fputc('\n', s_out);
    }
}

// Armed at start-up, before the guest runs: the generated code tests the flag
// on every observed call, so it is only set when a capture is asked for. A
// static initialiser rather than a call from main.cpp, so a production build,
// whose lift references nothing here, links none of this.
namespace {
struct R1ObserveInit {
    R1ObserveInit()
    {
        const char* dir = getenv("DOTHEROES_R1_DIR");
        const char* at = getenv("DOTHEROES_R1_AT");
        g_ppu_entry_observe = dir && *dir && at && *at;
    }
} s_r1_observe_init;
}  // namespace
