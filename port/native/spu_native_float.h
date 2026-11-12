// The SPU's single-precision arithmetic in the SPU's own float mode, on x86-64 (SSE4.1; AVX and FMA when the
// build has them): fa, fs, fm, fma, fms, fnms exactly as spu_helpers.h computes them (the lift's semantics),
// for code that runs with MXCSR at spu_native_rz::kMxcsrSpu -- round toward zero, flush-to-zero,
// denormals-are-zero, every exception masked. tools/spu_native_task.cpp.in uses them for an image whose code
// calls no helper that needs the host's own float mode (the generator decides; docs/SPU_NON_AUDIO.md).
//
// In that mode the host's single-precision operations truncate like the SPU and read and write a denormal
// as zero, so each helper is one host operation and a few integer checks:
//  - the +0 rule: the SPU never produces -0, and a result of biased exponent 0 is +0;
//  - zeros as the SPU reads them: any word of biased exponent 0 is a zero, whatever the host makes of it --
//    a zero factor makes the product +0 (0 times an extended-range word is NaN to the host), a zero product
//    gives the addend as it is, a zero addend is +0;
//  - the range: a vector with a lane whose result is not below FLT_MAX in magnitude -- an extended-range
//    operand (biased exponent 255: Inf or NaN to the host) or a result at or above the single range
//    (extended or saturated on the SPU; round toward zero stops an overflow at FLT_MAX) -- is redone whole in
//    the wide form below: the words as doubles built from their bits, the operation in double, packed back.
// fma is fused, one truncation of a*b + c: the host's FMA where the build has it; else in double, where the
// product of two singles is exact and the sum truncated at 53 bits and then at 24 is the truncation at 24.
//
// The float operations are volatile asm: a compiler may neither fold them at build time (it would round to
// nearest) nor move them across the MXCSR writes around them; everything else here is integer work.
// tests/native/spu_float_rz_check.cpp checks every form against spu_helpers.h's exact per-lane forms
// (spu__fa_any, spu__fm_any, spu__fma_any). ps3recomp's NEON helpers do the same under FPCR (spu__rz_fa/fm/fma).
// x86-64-v2 and up (SSE4.2: a 64-bit compare in the wide form).
#pragma once
#if (defined(__x86_64__) || defined(_M_X64)) && defined(__SSE4_2__)
#define SPU_NATIVE_FLOAT_RZ_AVAILABLE 1
#include <immintrin.h>

#include <cstdint>

#include "ps3emu/ps3types.h"   // u128

namespace spu_native_rz {

#define SPU_RZ_INLINE inline __attribute__((always_inline))

constexpr uint32_t kMxcsrSpu = 0x1F80u | 0x6000u | 0x8000u | 0x0040u;   // masked, RC toward zero, FTZ, DAZ

SPU_RZ_INLINE __m128i ld(const u128& a) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(&a)); }
SPU_RZ_INLINE u128 st(__m128i v) { u128 r; _mm_storeu_si128(reinterpret_cast<__m128i*>(&r), v); return r; }
// all-ones in the lanes the SPU reads as zero: biased exponent 0, whatever the fraction
SPU_RZ_INLINE __m128i zero(__m128i x)
{
    return _mm_cmpeq_epi32(_mm_and_si128(x, _mm_set1_epi32(0x7F800000)), _mm_setzero_si128());
}
// r with every word of biased exponent 0 (a zero of either sign) made +0
SPU_RZ_INLINE __m128i flush(__m128i r) { return _mm_andnot_si128(zero(r), r); }
// the lanes a host result stands for: magnitude below FLT_MAX (FLT_MAX itself, Inf and NaN fail)
SPU_RZ_INLINE __m128i inrange(__m128i s)
{
    return _mm_cmpgt_epi32(_mm_set1_epi32(0x7F7FFFFF), _mm_and_si128(s, _mm_set1_epi32(0x7FFFFFFF)));
}
SPU_RZ_INLINE bool all4(__m128i m) { return _mm_movemask_ps(_mm_castsi128_ps(m)) == 0xF; }

SPU_RZ_INLINE __m128i add(__m128i a, __m128i b)
{
    __m128 r;
#if defined(__AVX__)
    __asm__ volatile("vaddps %2, %1, %0" : "=x"(r) : "x"(_mm_castsi128_ps(a)), "x"(_mm_castsi128_ps(b)));
#else
    r = _mm_castsi128_ps(a);
    __asm__ volatile("addps %1, %0" : "+x"(r) : "x"(_mm_castsi128_ps(b)));
#endif
    return _mm_castps_si128(r);
}
SPU_RZ_INLINE __m128i mul(__m128i a, __m128i b)
{
    __m128 r;
#if defined(__AVX__)
    __asm__ volatile("vmulps %2, %1, %0" : "=x"(r) : "x"(_mm_castsi128_ps(a)), "x"(_mm_castsi128_ps(b)));
#else
    r = _mm_castsi128_ps(a);
    __asm__ volatile("mulps %1, %0" : "+x"(r) : "x"(_mm_castsi128_ps(b)));
#endif
    return _mm_castps_si128(r);
}
// a*b + c truncated once
SPU_RZ_INLINE __m128i fmadd(__m128i a, __m128i b, __m128i c)
{
    __m128 r = _mm_castsi128_ps(c);
#if defined(__FMA__)
    __asm__ volatile("vfmadd231ps %2, %1, %0" : "+x"(r) : "x"(_mm_castsi128_ps(a)), "x"(_mm_castsi128_ps(b)));
#else
    const __m128 fa = _mm_castsi128_ps(a), fb = _mm_castsi128_ps(b);
    __m128d alo = _mm_cvtps_pd(fa), ahi = _mm_cvtps_pd(_mm_movehl_ps(fa, fa));
    const __m128d blo = _mm_cvtps_pd(fb), bhi = _mm_cvtps_pd(_mm_movehl_ps(fb, fb));
    const __m128d clo = _mm_cvtps_pd(r), chi = _mm_cvtps_pd(_mm_movehl_ps(r, r));
    __m128 lo, hi;
    __asm__ volatile("mulpd %2, %0\n\taddpd %3, %0\n\tcvtpd2ps %0, %1" : "+x"(alo), "=x"(lo) : "x"(blo), "x"(clo));
    __asm__ volatile("mulpd %2, %0\n\taddpd %3, %0\n\tcvtpd2ps %0, %1" : "+x"(ahi), "=x"(hi) : "x"(bhi), "x"(chi));
    r = _mm_movelh_ps(lo, hi);
#endif
    return _mm_castps_si128(r);
}

// ---- The wide form: every lane exact, extended range included -------------------------------------------
// For a vector with a lane the single-precision result cannot stand for (an extended-range operand or a
// result at or above FLT_MAX: about 1% of the waver's vectors, mostly don't-care lanes beside a scalar). Each
// SPU word becomes a double built from its bits by integer work -- a word of biased exponent 0 is 0.0, any
// other is sign * 1.frac * 2^(be - 127), exponent 255 an ordinary binade -- so the extended range is plain
// arithmetic; the operation runs in double in the same mode (round toward zero), where a product of two such
// singles is exact and a sum or a fused sum, truncated at 53 bits and then at 24, is the truncation at 24;
// and the result is packed back as spu_helpers.h's spu__sf_pack does: 24 bits kept, a biased exponent above
// 255 saturated (magnitude 0x7FFFFFFF), below 1 (and an exact zero) +0. In the host's own mode (round to
// nearest) the sum would not be the truncation: these run only in the SPU's mode.

// two SPU words (the low 32 bits of each 64-bit lane) as doubles
SPU_RZ_INLINE __m128d wide_d(__m128i w64)
{
    const __m128i be = _mm_and_si128(_mm_srli_epi64(w64, 23), _mm_set1_epi64x(0xFF));
    const __m128i bits = _mm_or_si128(
        _mm_or_si128(_mm_slli_epi64(_mm_and_si128(w64, _mm_set1_epi64x(0x80000000ll)), 32),
                     _mm_slli_epi64(_mm_add_epi64(be, _mm_set1_epi64x(1023 - 127)), 52)),
        _mm_slli_epi64(_mm_and_si128(w64, _mm_set1_epi64x(0x7FFFFF)), 29));
    return _mm_castsi128_pd(_mm_andnot_si128(_mm_cmpeq_epi64(be, _mm_setzero_si128()), bits));
}
// two doubles as SPU words (the low 32 bits of each 64-bit lane): truncated, saturated, +0 for zero
SPU_RZ_INLINE __m128i wide_w(__m128d r)
{
    const __m128i u = _mm_castpd_si128(r);
    const __m128i sign = _mm_and_si128(_mm_srli_epi64(u, 32), _mm_set1_epi64x(0x80000000ll));
    const __m128i e = _mm_sub_epi64(_mm_and_si128(_mm_srli_epi64(u, 52), _mm_set1_epi64x(0x7FF)),
                                    _mm_set1_epi64x(1023 - 127));
    __m128i w = _mm_or_si128(_mm_or_si128(sign, _mm_slli_epi64(e, 23)),
                             _mm_and_si128(_mm_srli_epi64(u, 29), _mm_set1_epi64x(0x7FFFFF)));
    w = _mm_blendv_epi8(w, _mm_or_si128(sign, _mm_set1_epi64x(0x7FFFFFFF)), _mm_cmpgt_epi64(e, _mm_set1_epi64x(255)));
    const __m128i none = _mm_or_si128(_mm_cmpgt_epi64(_mm_set1_epi64x(1), e),
                                      _mm_cmpeq_epi64(_mm_slli_epi64(u, 1), _mm_setzero_si128()));
    return _mm_andnot_si128(none, w);
}
SPU_RZ_INLINE __m128i lo64(__m128i x) { return _mm_cvtepu32_epi64(x); }
SPU_RZ_INLINE __m128i hi64(__m128i x) { return _mm_cvtepu32_epi64(_mm_srli_si128(x, 8)); }
// the words of two halves' results back in one vector
SPU_RZ_INLINE __m128i join(__m128i lo, __m128i hi)
{
    return _mm_unpacklo_epi64(_mm_shuffle_epi32(lo, 0x08), _mm_shuffle_epi32(hi, 0x08));
}
SPU_RZ_INLINE __m128d wide_add(__m128d a, __m128d b)
{
#if defined(__AVX__)
    __asm__ volatile("vaddpd %2, %1, %0" : "=x"(a) : "x"(a), "x"(b));
#else
    __asm__ volatile("addpd %1, %0" : "+x"(a) : "x"(b));
#endif
    return a;
}
// exact (two 24-bit significands): the mode does not matter, but kept in asm beside the add
SPU_RZ_INLINE __m128d wide_mul(__m128d a, __m128d b)
{
#if defined(__AVX__)
    __asm__ volatile("vmulpd %2, %1, %0" : "=x"(a) : "x"(a), "x"(b));
#else
    __asm__ volatile("mulpd %1, %0" : "+x"(a) : "x"(b));
#endif
    return a;
}

inline __attribute__((noinline, cold)) u128 fa_fix(u128, __m128i, u128 a, u128 b, uint32_t negb)
{
    const __m128i x = ld(a), y = _mm_xor_si128(ld(b), _mm_set1_epi32((int)negb));
    const __m128i lo = wide_w(wide_add(wide_d(lo64(x)), wide_d(lo64(y))));
    const __m128i hi = wide_w(wide_add(wide_d(hi64(x)), wide_d(hi64(y))));
    return st(join(lo, hi));
}
inline __attribute__((noinline, cold)) u128 fm_fix(u128, __m128i, u128 a, u128 b)
{
    const __m128i x = ld(a), y = ld(b);
    const __m128i lo = wide_w(wide_mul(wide_d(lo64(x)), wide_d(lo64(y))));
    const __m128i hi = wide_w(wide_mul(wide_d(hi64(x)), wide_d(hi64(y))));
    return st(join(lo, hi));
}
inline __attribute__((noinline, cold)) u128 fma_fix(u128, __m128i, u128 a, u128 b, u128 c, uint32_t negp,
                                                    uint32_t negc)
{
    const __m128i x = _mm_xor_si128(ld(a), _mm_set1_epi32((int)negp)), y = ld(b);
    const __m128i z = _mm_xor_si128(ld(c), _mm_set1_epi32((int)negc));
    const __m128i lo = wide_w(wide_add(wide_mul(wide_d(lo64(x)), wide_d(lo64(y))), wide_d(lo64(z))));
    const __m128i hi = wide_w(wide_add(wide_mul(wide_d(hi64(x)), wide_d(hi64(y))), wide_d(hi64(z))));
    return st(join(lo, hi));
}

// a + (b ^ negb): a word the SPU reads as zero is zero to the host too (DAZ); an exact zero sum is +0
SPU_RZ_INLINE u128 fadd(const u128& a, const u128& b, uint32_t negb)
{
    const __m128i x = ld(a), y = _mm_xor_si128(ld(b), _mm_set1_epi32((int)negb));
    const __m128i s = add(x, y);
    const __m128i ok = inrange(s);
    const __m128i r = flush(s);
    return all4(ok) ? st(r) : fa_fix(st(r), ok, a, b, negb);
}
// a zero factor makes the lane +0 whatever the other operand is
SPU_RZ_INLINE u128 fmul(const u128& a, const u128& b)
{
    const __m128i x = ld(a), y = ld(b);
    const __m128i zp = _mm_or_si128(zero(x), zero(y));
    const __m128i s = mul(x, y);
    const __m128i ok = _mm_or_si128(inrange(s), zp);
    const __m128i r = _mm_andnot_si128(zp, flush(s));
    return all4(ok) ? st(r) : fm_fix(st(r), ok, a, b);
}
// (+-a*b) + (+-c): a zero product gives the addend as it is (extended range too), a zero addend is +0
SPU_RZ_INLINE u128 fmuladd(const u128& a, const u128& b, const u128& c, uint32_t negp, uint32_t negc)
{
    const __m128i xa = ld(a), y = ld(b), zc = ld(c);
    const __m128i zp = _mm_or_si128(zero(xa), zero(y));
    const __m128i zi = _mm_andnot_si128(zero(zc), _mm_xor_si128(zc, _mm_set1_epi32((int)negc)));
    const __m128i x = _mm_xor_si128(xa, _mm_set1_epi32((int)negp));
    const __m128i s = fmadd(x, y, zi);
    const __m128i ok = _mm_or_si128(inrange(s), zp);
    const __m128i r = _mm_blendv_epi8(flush(s), zi, zp);
    return all4(ok) ? st(r) : fma_fix(st(r), ok, a, b, c, negp, negc);
}

}  // namespace spu_native_rz

// The generated code's names (tools/gen_spu_native.py writes rz_fa for the lift's spu_fa, and so on).
SPU_RZ_INLINE u128 rz_fa(const u128& a, const u128& b) { return spu_native_rz::fadd(a, b, 0); }
SPU_RZ_INLINE u128 rz_fs(const u128& a, const u128& b) { return spu_native_rz::fadd(a, b, 0x80000000u); }
SPU_RZ_INLINE u128 rz_fm(const u128& a, const u128& b) { return spu_native_rz::fmul(a, b); }
SPU_RZ_INLINE u128 rz_fma(const u128& a, const u128& b, const u128& c)    // a*b + c
{
    return spu_native_rz::fmuladd(a, b, c, 0, 0);
}
SPU_RZ_INLINE u128 rz_fms(const u128& a, const u128& b, const u128& c)    // a*b - c
{
    return spu_native_rz::fmuladd(a, b, c, 0, 0x80000000u);
}
SPU_RZ_INLINE u128 rz_fnms(const u128& a, const u128& b, const u128& c)   // c - a*b
{
    return spu_native_rz::fmuladd(a, b, c, 0x80000000u, 0);
}
#else
#define SPU_NATIVE_FLOAT_RZ_AVAILABLE 0
#endif
