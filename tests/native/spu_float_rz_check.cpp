// port/native/spu_native_float.h against spu_helpers.h's exact per-lane forms (the lift's semantics): fa, fs,
// fm, fma, fms and fnms in the SPU's float mode (MXCSR round toward zero, flush-to-zero, denormals-are-zero)
// against spu__fa_any / spu__fm_any / spu__fma_any in the host's (round to nearest, nothing flushed), bit for
// bit, on words drawn from every class the SPU distinguishes: its zeros (biased exponent 0, any fraction,
// either sign), the smallest and largest normals, extended range (biased exponent 255, any fraction), words
// at FLT_MAX, ordinary values, and pairs and triples whose exponents differ by 0 to 64 (an add's alignment,
// cancellation, an fma's addend far from the product); every vector mixes the classes across its four lanes,
// so the fix-up paths run beside the fast one; then the wide form (the fix-up) alone on every vector. No
// game data.
//
//   spu_float_rz_check [<millions of vectors per operation> [<seed>]]   (default 4, 1)
//
// Built with the bundled Zig from the repository root, at the levels the game is built for:
//   build/toolchain/zig-x86_64-windows-0.17.0/zig.exe c++ -target x86_64-windows-gnu -mcpu=x86_64_v3 -O2 -std=c++17
//       -I port/native -I build/ref/ps3recomp/runtime/spu -I build/ref/ps3recomp/include
//       tests/native/spu_float_rz_check.cpp -o <scratch>/spu_float_rz_check_v3.exe
//   (and -mcpu=x86_64_v2: no AVX or FMA, the fma in double).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "spu_helpers.h"   // the exact per-lane forms
#include "spu_native_float.h"

#if !SPU_NATIVE_FLOAT_RZ_AVAILABLE
int main() { std::puts("spu_native_float.h: not available on this target (x86-64 with SSE4.1)"); return 1; }
#else

namespace {

std::mt19937_64 g_rng;
uint32_t rnd32() { return (uint32_t)g_rng(); }

uint32_t make(uint32_t sign, uint32_t be, uint32_t frac) { return sign << 31 | (be & 0xFF) << 23 | (frac & 0x7FFFFF); }

// a word of one of the SPU's classes; `near` (>= 0): a normal within a few binades of that biased exponent
uint32_t word(int near = -1)
{
    const uint32_t s = rnd32() & 1, f = rnd32() & 0x7FFFFF;
    if (near >= 0) {
        int be = near + (int)(rnd32() % 129) - 64;   // an exponent gap of 0..64 either way
        if (be < 0) be = 0;
        if (be > 255) be = 255;
        return make(s, (uint32_t)be, f);
    }
    switch (rnd32() % 16) {
    case 0: return make(s, 0, 0);                         // +-0
    case 1: return make(s, 0, f | 1);                     // a denormal pattern: an SPU zero
    case 2: return make(s, 255, f);                       // extended range
    case 3: return make(s, 254, 0x7FFFFF);                // FLT_MAX
    case 4: return make(s, 254, f);                       // near the top
    case 5: return make(s, 1 + rnd32() % 3, f);           // the smallest normals
    case 6: return make(s, 127, rnd32() % 4 ? 0 : f);     // 1.0 and its binade
    case 7: return rnd32();                               // any bits
    case 8: return make(s, 120 + rnd32() % 16, f & 0x7FFF00);   // few significant bits (exact sums)
    default: return make(s, 1 + rnd32() % 254, f);        // an ordinary normal
    }
}

struct Op {
    const char* name;
    int args;
    u128 (*fast)(const u128&, const u128&, const u128&);
    uint32_t (*exact)(uint32_t, uint32_t, uint32_t);
};
u128 f_fa(const u128& a, const u128& b, const u128&) { return rz_fa(a, b); }
u128 f_fs(const u128& a, const u128& b, const u128&) { return rz_fs(a, b); }
u128 f_fm(const u128& a, const u128& b, const u128&) { return rz_fm(a, b); }
u128 f_fma(const u128& a, const u128& b, const u128& c) { return rz_fma(a, b, c); }
u128 f_fms(const u128& a, const u128& b, const u128& c) { return rz_fms(a, b, c); }
u128 f_fnms(const u128& a, const u128& b, const u128& c) { return rz_fnms(a, b, c); }
// the wide form alone, on every vector (it runs only for vectors with an out-of-range lane in the forms above)
const __m128i kNone = _mm_setzero_si128();
u128 w_fa(const u128& a, const u128& b, const u128&) { return spu_native_rz::fa_fix(a, kNone, a, b, 0); }
u128 w_fs(const u128& a, const u128& b, const u128&) { return spu_native_rz::fa_fix(a, kNone, a, b, 0x80000000u); }
u128 w_fm(const u128& a, const u128& b, const u128&) { return spu_native_rz::fm_fix(a, kNone, a, b); }
u128 w_fma(const u128& a, const u128& b, const u128& c) { return spu_native_rz::fma_fix(a, kNone, a, b, c, 0, 0); }
u128 w_fms(const u128& a, const u128& b, const u128& c)
{
    return spu_native_rz::fma_fix(a, kNone, a, b, c, 0, 0x80000000u);
}
u128 w_fnms(const u128& a, const u128& b, const u128& c)
{
    return spu_native_rz::fma_fix(a, kNone, a, b, c, 0x80000000u, 0);
}
uint32_t e_fa(uint32_t x, uint32_t y, uint32_t) { return spu__fa_any(x, y); }
uint32_t e_fs(uint32_t x, uint32_t y, uint32_t) { return spu__fa_any(x, y ^ 0x80000000u); }
uint32_t e_fm(uint32_t x, uint32_t y, uint32_t) { return spu__fm_any(x, y); }
uint32_t e_fma(uint32_t x, uint32_t y, uint32_t z) { return spu__fma_any(x, y, z, 0, 0); }
uint32_t e_fms(uint32_t x, uint32_t y, uint32_t z) { return spu__fma_any(x, y, z, 0, 0x80000000u); }
uint32_t e_fnms(uint32_t x, uint32_t y, uint32_t z) { return spu__fma_any(x, y, z, 0x80000000u, 0); }
const Op kOps[] = {
    { "fa", 2, f_fa, e_fa },   { "fs", 2, f_fs, e_fs },     { "fm", 2, f_fm, e_fm },
    { "fma", 3, f_fma, e_fma }, { "fms", 3, f_fms, e_fms }, { "fnms", 3, f_fnms, e_fnms },
    { "wide fa", 2, w_fa, e_fa },   { "wide fs", 2, w_fs, e_fs },     { "wide fm", 2, w_fm, e_fm },
    { "wide fma", 3, w_fma, e_fma }, { "wide fms", 3, w_fms, e_fms }, { "wide fnms", 3, w_fnms, e_fnms },
};

// the SPU mode's results, in a function of its own so that nothing of the exact side runs in that mode
__attribute__((noinline)) void run_fast(const Op& op, const u128* a, const u128* b, const u128* c, u128* out, size_t n)
{
    const unsigned host = _mm_getcsr();
    _mm_setcsr(spu_native_rz::kMxcsrSpu);
    for (size_t i = 0; i < n; i++) out[i] = op.fast(a[i], b[i], c[i]);
    _mm_setcsr(host);
}
__attribute__((noinline)) void run_exact(const Op& op, const u128* a, const u128* b, const u128* c, u128* out, size_t n)
{
    for (size_t i = 0; i < n; i++)
        for (int l = 0; l < 4; l++) out[i]._u32[l] = op.exact(a[i]._u32[l], b[i]._u32[l], c[i]._u32[l]);
}

}  // namespace

int main(int argc, char** argv)
{
    const double millions = argc > 1 ? atof(argv[1]) : 4.0;
    g_rng.seed(argc > 2 ? strtoull(argv[2], nullptr, 0) : 1);
    if (_mm_getcsr() != 0x1F80u) std::printf("[info] the host's MXCSR is 0x%04X\n", _mm_getcsr());
    _mm_setcsr(0x1F80u);   // the exact forms in the host's default mode
    const size_t batch = 1 << 16, total = (size_t)(millions * 1e6);
    std::vector<u128> a(batch), b(batch), c(batch), fast(batch), exact(batch);
    bool ok = true;
    for (const Op& op : kOps) {
        uint64_t lanes = 0, bad = 0;
        for (size_t done = 0; done < total; done += batch) {
            for (size_t i = 0; i < batch; i++)
                for (int l = 0; l < 4; l++) {
                    const uint32_t x = word();
                    const int mode = (int)(rnd32() % 4);   // 0: independent words; else near x's exponent
                    const int bx = (int)(x >> 23 & 0xFF);
                    a[i]._u32[l] = x;
                    b[i]._u32[l] = mode == 0 ? word() : mode == 1 ? word(bx) : word(127);
                    // fma: an addend near the product's exponent (cancellation) or anywhere
                    const int by = (int)(b[i]._u32[l] >> 23 & 0xFF);
                    c[i]._u32[l] = mode >= 2 ? word(bx + by - 127 < 0 ? 0 : bx + by - 127) : word();
                }
            run_fast(op, a.data(), b.data(), c.data(), fast.data(), batch);
            run_exact(op, a.data(), b.data(), c.data(), exact.data(), batch);
            for (size_t i = 0; i < batch; i++)
                for (int l = 0; l < 4; l++) {
                    lanes++;
                    if (fast[i]._u32[l] == exact[i]._u32[l]) continue;
                    if (bad++ < 8)
                        std::printf("[FAIL] %s(%08X, %08X%s%08X) = %08X, exact %08X\n", op.name, a[i]._u32[l],
                                    b[i]._u32[l], op.args == 3 ? ", " : "", op.args == 3 ? c[i]._u32[l] : 0u,
                                    fast[i]._u32[l], exact[i]._u32[l]);
                }
        }
        std::printf("[%s] %-9s %llu lanes, %llu different\n", bad ? "FAIL" : "PASS", op.name,
                    (unsigned long long)lanes, (unsigned long long)bad);
        if (bad) ok = false;
    }
    std::puts(ok ? "all passed" : "FAILED");
    return ok ? 0 : 1;
}
#endif
