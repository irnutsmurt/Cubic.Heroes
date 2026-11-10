// The game's waver SPU task as native code (waver_task.h). Read in the lift
// (build/gen/spu/spu_0001_at_00F48F80/spu_recomp.c) and its image's
// disassembly; checked offline against the lifted image itself
// (tests/native/waver_task_check.cpp). The register names in the comments are
// the task's.
#include "waver_task.h"

#include <cstring>

#include "spu_helpers.h"   // the runtime's exact SPU single-precision arithmetic (the lifted task's own)

namespace waver_task {
namespace {

constexpr uint32_t kSize = 256, kRowBytes = 1024, kBlockRows = 8, kBlocks = 32, kBlockBytes = 8192;
constexpr uint32_t kMinusOne = 0xBF800000u;   // the field outside the ripple (r81)
constexpr uint32_t kNine10 = 0x3F666666u;     // 0.9f (r26)
constexpr uint32_t kHalf = 0x3F000000u, kOne = 0x3F800000u, kOneUp = 0x3F800001u, k255 = 0x437F0000u;
constexpr uint32_t kFlat = 0x008080FFu;       // the pixel where the field is below zero (r31)
constexpr int kCfltuScale0 = 173;             // cfltu's immediate for no scaling, as the task encodes it

inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline void st32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
inline u128 splat(uint32_t v)
{
    u128 r;
    r._u32[0] = r._u32[1] = r._u32[2] = r._u32[3] = v;
    return r;
}

// The task's DMA helper (LS 0x3770): one command per piece, in order.
void transfer(const Io& io, bool get, uint8_t* ls, uint32_t ea, uint32_t size)
{
    while (size) {
        const uint32_t n = size > 16383 ? 16384u : size > 15 ? size & ~15u : size > 7 ? 8u : size > 3 ? 4u : size > 1 ? 2u : 1u;
        if (get) io.get(io.user, ls, ea, n);
        else io.put(io.user, ea, ls, n);
        ls += n;
        ea += n;
        size -= n;
    }
}

bool past_4gb(uint32_t ea, uint32_t bytes) { return (uint64_t)ea + bytes > (1ull << 32); }

void guest_get(void* user, void* dst, uint32_t ea, uint32_t bytes) { memcpy(dst, (uint8_t*)user + ea, bytes); }
void guest_put(void* user, uint32_t ea, const void* src, uint32_t bytes) { memcpy((uint8_t*)user + ea, src, bytes); }

}  // namespace

Params parse_params(const uint8_t b[kParamBytes])
{
    Params p;
    p.field = be32(b + 0x00);
    p.texture[0] = be32(b + 0x04);
    p.texture[1] = be32(b + 0x08);
    p.select = be32(b + 0x0C);
    p.phase = be32(b + 0x10);
    p.radius = be32(b + 0x14);
    p.frequency = be32(b + 0x18);
    p.amplitude = be32(b + 0x1C);
    p.slope = be32(b + 0x20);
    return p;
}

Io guest_io(uint8_t* base) { return Io{ base, guest_get, guest_put }; }

const char* unsupported_reason(const Params& p, uint32_t param_ea)
{
    if (param_ea & 15) return "argument EA not 16-byte aligned (the task halts at LS 0x3704, before its GET)";
    if (past_4gb(param_ea, kParamBytes)) return "parameter block past 4 GB (its GET would carry into the EA's high word)";
    if (past_4gb(p.field, kFieldBytes)) return "field past 4 GB";
    if (past_4gb(p.texture_ea(), kTextureBytes)) return "texture past 4 GB";
    if (p.field & 15) return "field not 16-byte aligned (the task halts at LS 0x3254, before its first PUT)";
    if (p.texture_ea() & 15) return "texture not 16-byte aligned (the task halts at LS 0x3664, after writing the field)";
    return nullptr;
}

// LS 0x3098: 32 blocks of 8 rows into LS 0x3A10, each PUT and waited for.
bool field_step(const Params& p, const Io& io, Local& ls)
{
    const uint8_t* map = ls.params + kMapOffset;      // r16/r8: P + 0x24 + 2048 o + 256 row + x
    const uint8_t* table = ls.params + kTableOffset;  // r82: P + 0x10024
    const u128 amplitude = splat(p.amplitude);        // r38
    const uint32_t frequency = p.frequency & 0xFFFFu; // mpyu reads the low halfword
    for (uint32_t o = 0; o < kBlocks; o++) {
        for (uint32_t r = 0; r < kBlockRows; r++) {
            const uint8_t* src = map + kSize * (kBlockRows * o + r);
            uint8_t* dst = ls.rows + kRowBytes * r;
            for (uint32_t x = 0; x < kSize; x += 4) {
                u128 entry;
                bool inside[4];
                for (uint32_t k = 0; k < 4; k++) {
                    const uint32_t b = src[x + k];                                     // r25
                    const uint32_t i = (uint32_t)(uint16_t)(b - p.phase) * frequency & 127u;   // r26, r21, r20
                    entry._u32[k] = be32(table + 4 * i);                               // r37
                    inside[k] = b <= p.radius && b <= 126;                             // r18 andc r19 (r17)
                }
                const u128 v = spu_fm(entry, amplitude);                               // r36
                for (uint32_t k = 0; k < 4; k++) st32(dst + 4 * (x + k), inside[k] ? v._u32[k] : kMinusOne);
            }
        }
        const uint32_t ea = p.field + kBlockBytes * o;
        if (ea & 15) return false;   // heqi at LS 0x3254: halts with the block computed, nothing PUT
        transfer(io, false, ls.rows, ea, kBlockBytes);
    }
    return true;
}

// LS 0x32E0: per block the field's rows into LS 0x3A10 (with the row above
// and below where there is one), 8 rows of pixels into LS 0x6210, PUT.
bool normal_map(const Params& p, const Io& io, Local& ls)
{
    const u128 zero = splat(0), h_scale = splat(kNine10), half = splat(kHalf), one = splat(kOne);
    const u128 one_up = splat(kOneUp), c255 = splat(k255), slope = splat(p.slope);   // r28, r22, r25
    for (uint32_t k = 0; k < kBlocks; k++) {
        const bool first = k == 0, last = k == kBlocks - 1;
        const uint32_t ea = p.field + kBlockBytes * k - (first ? 0 : kRowBytes);   // r40
        if (ea & 15) return false;   // heqi at LS 0x33DC (the field step halts first on the same EA)
        transfer(io, true, ls.rows, ea, (first || last ? 9 : 10) * kRowBytes);    // r5
        const uint8_t* row0 = ls.rows + (first ? 0 : kRowBytes);                  // r82
        for (uint32_t j = 0; j < kBlockRows; j++) {
            const uint8_t* mid = row0 + kRowBytes * j;                               // r13
            const uint8_t* up = j ? mid - kRowBytes : ls.rows;                       // r21: block 0's row 0 is its own
            const uint8_t* down = j == kBlockRows - 1 && last ? mid : mid + kRowBytes;   // r24: row 255's is its own
            uint8_t* out = ls.out + kRowBytes * j;                                   // r18
            for (uint32_t x = 0; x < kSize; x += 4) {
                u128 h, left, right, above, below;
                for (uint32_t i = 0; i < 4; i++) {
                    const uint32_t c = x + i, l = c ? c - 1 : 0, r = c + 1 < kSize ? c + 1 : c;   // r6, r12
                    h._u32[i] = be32(mid + 4 * c);
                    left._u32[i] = be32(mid + 4 * l);
                    right._u32[i] = be32(mid + 4 * r);
                    above._u32[i] = be32(up + 4 * c);
                    below._u32[i] = be32(down + 4 * c);
                }
                const u128 h9 = spu_fm(h, h_scale);                                     // r11
                const u128 a = spu_selb(above, h9, spu_fcgt(zero, above));              // r15
                const u128 d = spu_selb(below, h9, spu_fcgt(zero, below));              // r47
                const u128 dy = spu_fs(a, d);                                           // r6
                const u128 l = spu_selb(left, h9, spu_fcgt(zero, left));                // r38
                const u128 r = spu_selb(right, h9, spu_fcgt(zero, right));              // r37
                const u128 ny = spu_fm(slope, dy);                                      // r76
                const u128 dx = spu_fs(r, l);                                           // r4
                const u128 ny2 = spu_fm(ny, ny);                                        // r19
                const u128 nx = spu_fm(dx, slope);                                      // r77
                const u128 s = spu_fma(nx, nx, ny2);                                    // r14
                const u128 y = spu_fi(s, spu_frsqest(s));                               // r17, r12
                const u128 q = spu_fm(y, s);                                            // r3
                const u128 len = spu_fma(spu_fnms(y, q, one_up), spu_fm(q, half), q);   // r10, r11, r8
                const u128 dd = spu_fa(len, one);                                       // r2
                const u128 rr = spu_fi(dd, spu_frest(dd));                              // r7, r5
                const u128 t = spu_fm(one, rr);                                         // r79
                const u128 inv = spu_fma(spu_fnms(t, dd, one), rr, t);                  // r78, r75
                const u128 xs = spu_fm(nx, inv), ys = spu_fm(ny, inv);                  // r74, r73
                const u128 bz = spu_cfltu(spu_fm(spu_fma(inv, half, half), c255), kCfltuScale0);   // r72, r69, r64
                const u128 bx = spu_cfltu(spu_fm(spu_fma(xs, half, half), c255), kCfltuScale0);    // r71, r68, r66
                const u128 by = spu_cfltu(spu_fm(spu_fma(ys, half, half), c255), kCfltuScale0);    // r70, r67, r65
                const u128 below_zero = spu_fcgt(zero, h);                              // r54
                for (uint32_t i = 0; i < 4; i++)
                    st32(out + 4 * (x + i),
                         below_zero._u32[i] ? kFlat : bx._u32[i] << 16 | by._u32[i] << 8 | bz._u32[i]);   // r63, r61, r62, r60
            }
        }
        const uint32_t tex = p.texture_ea() + kBlockBytes * k;   // r21: P + 4 + 4 (P[0xC] & 1), + r87
        if (tex & 15) return false;   // heqi at LS 0x3664: halts with block 0's rows GET, no pixel PUT
        transfer(io, false, ls.out, tex, kBlockBytes);
    }
    return true;
}

Status run_frame(uint32_t param_ea, const Io& io, Local& ls, Params* used)
{
    if (param_ea & 15) return Status::unsupported;   // the task halts before its GET
    if (past_4gb(param_ea, kParamBytes)) return Status::unsupported;
    transfer(io, true, ls.params, param_ea, kParamBytes);
    const Params p = parse_params(ls.params);
    if (used) *used = p;
    if (past_4gb(p.field, kFieldBytes) || past_4gb(p.texture_ea(), kTextureBytes)) return Status::unsupported;
    if (!field_step(p, io, ls)) return Status::unsupported;
    if (!normal_map(p, io, ls)) return Status::unsupported;
    return Status::done;
}

}  // namespace waver_task
