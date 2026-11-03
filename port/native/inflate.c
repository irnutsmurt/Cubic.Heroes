/* Raw deflate (RFC 1951), one shot, for the Edge zlib work (inflate.h).
 *
 * The bit buffer is 64 bits, refilled with one unaligned 8-byte load while 8
 * input bytes remain: the load is shifted in above the bits still held, the
 * input moves on by the whole bytes that fitted, and the part of the next byte
 * that also landed above them is that byte's own bits, so ORing it in again
 * later changes nothing. A refill leaves 56-63 bits: one length/distance pair
 * (at most 15+5+15+13 = 48 bits) never needs another.
 *
 * Codes decode through a table indexed by the next LL_BITS (DD_BITS) bits of
 * the stream; a longer code's prefix points to a subtable for the rest. */
#include "inflate.h"

#include <string.h>

enum { K_LIT = 0, K_LEN = 1, K_EOB = 2, K_SUB = 3, K_BAD = 4 };

/* An entry: bits 0-7 the bits it consumes (a subtable pointer: the main
 * table's), 8-12 the extra bits that follow (a subtable pointer: its index
 * bits), 13-15 the kind, 16-31 the value (a literal, a length or distance
 * base, a precode symbol, a subtable's offset). */
#define ENT(len, extra, kind, val) \
    ((uint32_t)(len) | (uint32_t)(extra) << 8 | (uint32_t)(kind) << 13 | (uint32_t)(val) << 16)
#define E_LEN(e)   ((e) & 0xFFu)
#define E_EXTRA(e) (((e) >> 8) & 0x1Fu)
#define E_KIND(e)  (((e) >> 13) & 7u)
#define E_VAL(e)   ((e) >> 16)

#define LL_BITS 11
#define DD_BITS 8
#define PC_BITS 7
#define LL_SIZE (2048 + 1024)
#define DD_SIZE (256 + 768)

static const uint16_t k_len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                         67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t k_len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
                                         5, 5, 5, 5, 0 };
static const uint16_t k_dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
                                          769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t k_dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
                                          11, 11, 12, 12, 13, 13 };
static const uint8_t k_precode_order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

/* The entry (without its bit count) a symbol decodes to. */
static uint32_t ll_sym(unsigned s)
{
    if (s < 256) return ENT(0, 0, K_LIT, s);
    if (s == 256) return ENT(0, 0, K_EOB, 0);
    if (s < 286) return ENT(0, k_len_extra[s - 257], K_LEN, k_len_base[s - 257]);
    return ENT(0, 0, K_BAD, 0);          /* 286, 287: in the fixed code, never valid */
}
static uint32_t dd_sym(unsigned s)
{
    return s < 30 ? ENT(0, k_dist_extra[s], K_LEN, k_dist_base[s]) : ENT(0, 0, K_BAD, 0);
}
static uint32_t pc_sym(unsigned s) { return ENT(0, 0, K_LIT, s); }

static unsigned reverse_bits(unsigned c, unsigned len)
{
    unsigned r = 0;
    while (len--) { r = r << 1 | (c & 1); c >>= 1; }
    return r;
}

/* The decode table for code lengths lens[0..n): 0, or -1 for a set of lengths
 * no valid stream has (over-subscribed, or incomplete -- which zlib allows
 * only for a single code of one bit, and never for the precode). */
static int build_table(uint32_t* t, unsigned tbits, unsigned tsize, const uint8_t* lens, unsigned n,
                       uint32_t (*sym)(unsigned), int strict)
{
    unsigned count[16] = { 0 }, next[16];
    for (unsigned i = 0; i < n; i++) count[lens[i]]++;
    count[0] = 0;
    int left = 1;
    unsigned max = 0;
    for (unsigned l = 1; l < 16; l++) {
        left = (left << 1) - (int)count[l];
        if (left < 0) return -1;
        if (count[l]) max = l;
    }
    if (left > 0 && max && (strict || max != 1)) return -1;
    const unsigned main = 1u << tbits;
    for (unsigned i = 0; i < main; i++) t[i] = ENT(0, 0, K_BAD, 0);
    if (!max) return 0;                  /* no codes: a distance table no match uses */
    unsigned code = 0;
    next[0] = 0;
    for (unsigned l = 1; l < 16; l++) { code = (code + count[l - 1]) << 1; next[l] = code; }
    /* Subtables: for each prefix of the main table's width, as wide as its
     * longest code needs. */
    uint8_t need[1u << LL_BITS];
    memset(need, 0, main);
    for (unsigned s = 0; s < n; s++) {
        const unsigned l = lens[s];
        if (l <= tbits) continue;
        const unsigned r = reverse_bits(next[l], l) & (main - 1);
        if (l - tbits > need[r]) need[r] = (uint8_t)(l - tbits);
        next[l]++;
    }
    unsigned top = main;
    for (unsigned p = 0; p < main; p++) {
        if (!need[p]) continue;
        const unsigned size = 1u << need[p];
        if (top + size > tsize) return -1;
        t[p] = ENT(tbits, need[p], K_SUB, top);
        for (unsigned i = 0; i < size; i++) t[top + i] = ENT(0, 0, K_BAD, 0);
        top += size;
    }
    code = 0;
    for (unsigned l = 1; l < 16; l++) { code = (code + count[l - 1]) << 1; next[l] = code; }
    for (unsigned s = 0; s < n; s++) {
        const unsigned l = lens[s];
        if (!l) continue;
        const unsigned r = reverse_bits(next[l]++, l);
        if (l <= tbits) {
            const uint32_t e = sym(s) | l;
            for (unsigned k = r; k < main; k += 1u << l) t[k] = e;
        } else {
            const unsigned p = r & (main - 1), sl = l - tbits, base = E_VAL(t[p]);
            const uint32_t e = sym(s) | sl;
            for (unsigned k = r >> tbits; k < (1u << need[p]); k += 1u << sl) t[base + k] = e;
        }
    }
    return 0;
}

static inline uint64_t load64le(const uint8_t* p)
{
    uint64_t v;
    memcpy(&v, p, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap64(v);
#endif
    return v;
}

long dh_inflate_raw(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap, size_t* src_used)
{
    const uint8_t* in = src;
    const uint8_t* const in_end = src + src_len;
    uint8_t* out = dst;
    uint8_t* const out_end = dst + dst_cap;
    uint64_t bitbuf = 0;
    unsigned bitsleft = 0, overrun = 0;
    uint32_t ll[LL_SIZE], dd[DD_SIZE], pc[1u << PC_BITS];

    /* At least 56 bits held. Past the input's end, zero bytes: a valid stream
     * reads at most 7 of them ahead of its last bit. */
#define REFILL()                                                              \
    do {                                                                      \
        if (in_end - in >= 8) {                                               \
            bitbuf |= load64le(in) << bitsleft;                               \
            in += (63 - bitsleft) >> 3;                                       \
            bitsleft |= 56;                                                   \
        } else {                                                              \
            while (bitsleft <= 56) {                                          \
                if (in < in_end) bitbuf |= (uint64_t)*in++ << bitsleft;       \
                else if (++overrun > 8) return DH_INFLATE_SHORT;              \
                bitsleft += 8;                                                \
            }                                                                 \
        }                                                                     \
    } while (0)
#define DROP(nb) (bitbuf >>= (nb), bitsleft -= (nb))
#define BITS(nb) ((unsigned)(bitbuf & ((1ull << (nb)) - 1)))

    for (;;) {
        REFILL();
        const unsigned final = BITS(1);
        DROP(1);
        const unsigned type = BITS(2);
        DROP(2);
        if (type == 0) {                 /* stored */
            DROP(bitsleft & 7);
            const unsigned len = BITS(16), nlen = (unsigned)(bitbuf >> 16) & 0xFFFF;
            DROP(32);
            if (len != (~nlen & 0xFFFFu)) return DH_INFLATE_BAD_DATA;
            /* The whole bytes still held go back to the input. */
            unsigned back = bitsleft >> 3;
            if (back < overrun) return DH_INFLATE_SHORT;
            in -= back - overrun;
            overrun = 0;
            bitbuf = 0;
            bitsleft = 0;
            if ((size_t)(in_end - in) < len) return DH_INFLATE_SHORT;
            if ((size_t)(out_end - out) < len) return DH_INFLATE_NO_ROOM;
            memcpy(out, in, len);
            in += len;
            out += len;
        } else if (type == 3) {
            return DH_INFLATE_BAD_DATA;
        } else {
            if (type == 1) {             /* the fixed code */
                uint8_t lens[288];
                unsigned i = 0;
                for (; i < 144; i++) lens[i] = 8;
                for (; i < 256; i++) lens[i] = 9;
                for (; i < 280; i++) lens[i] = 7;
                for (; i < 288; i++) lens[i] = 8;
                build_table(ll, LL_BITS, LL_SIZE, lens, 288, ll_sym, 0);
                for (i = 0; i < 32; i++) lens[i] = 5;
                build_table(dd, DD_BITS, DD_SIZE, lens, 32, dd_sym, 0);
            } else {                     /* a code of its own */
                const unsigned hlit = BITS(5) + 257;
                DROP(5);
                const unsigned hdist = BITS(5) + 1;
                DROP(5);
                const unsigned hclen = BITS(4) + 4;
                DROP(4);
                if (hlit > 286 || hdist > 30) return DH_INFLATE_BAD_DATA;
                uint8_t plens[19] = { 0 };
                for (unsigned i = 0; i < hclen; i++) {
                    if (bitsleft < 3) REFILL();
                    plens[k_precode_order[i]] = (uint8_t)BITS(3);
                    DROP(3);
                }
                if (build_table(pc, PC_BITS, 1u << PC_BITS, plens, 19, pc_sym, 1)) return DH_INFLATE_BAD_DATA;
                uint8_t lens[286 + 30];
                const unsigned n = hlit + hdist;
                for (unsigned i = 0; i < n;) {
                    REFILL();
                    const uint32_t e = pc[BITS(PC_BITS)];
                    if (E_KIND(e) == K_BAD) return DH_INFLATE_BAD_DATA;
                    DROP(E_LEN(e));
                    const unsigned s = E_VAL(e);
                    unsigned rep;
                    uint8_t v = 0;
                    if (s < 16) {
                        lens[i++] = (uint8_t)s;
                        continue;
                    } else if (s == 16) {
                        if (!i) return DH_INFLATE_BAD_DATA;
                        v = lens[i - 1];
                        rep = 3 + BITS(2);
                        DROP(2);
                    } else if (s == 17) {
                        rep = 3 + BITS(3);
                        DROP(3);
                    } else {
                        rep = 11 + BITS(7);
                        DROP(7);
                    }
                    if (i + rep > n) return DH_INFLATE_BAD_DATA;
                    memset(lens + i, v, rep);
                    i += rep;
                }
                if (!lens[256]) return DH_INFLATE_BAD_DATA;
                if (build_table(ll, LL_BITS, LL_SIZE, lens, hlit, ll_sym, 0) ||
                    build_table(dd, DD_BITS, DD_SIZE, lens + hlit, hdist, dd_sym, 0))
                    return DH_INFLATE_BAD_DATA;
            }
            for (;;) {
                REFILL();
                uint32_t e = ll[BITS(LL_BITS)];
                if (E_KIND(e) == K_SUB) {
                    DROP(LL_BITS);
                    e = ll[E_VAL(e) + BITS(E_EXTRA(e))];
                }
                DROP(E_LEN(e));
                const unsigned kind = E_KIND(e);
                if (kind == K_LIT) {
                    if (out == out_end) return DH_INFLATE_NO_ROOM;
                    *out++ = (uint8_t)E_VAL(e);
                    continue;
                }
                if (kind == K_EOB) break;
                if (kind != K_LEN) return DH_INFLATE_BAD_DATA;
                const size_t len = E_VAL(e) + BITS(E_EXTRA(e));
                DROP(E_EXTRA(e));
                e = dd[BITS(DD_BITS)];
                if (E_KIND(e) == K_SUB) {
                    DROP(DD_BITS);
                    e = dd[E_VAL(e) + BITS(E_EXTRA(e))];
                }
                if (E_KIND(e) != K_LEN) return DH_INFLATE_BAD_DATA;
                DROP(E_LEN(e));
                const size_t dist = E_VAL(e) + BITS(E_EXTRA(e));
                DROP(E_EXTRA(e));
                if (dist > (size_t)(out - dst)) return DH_INFLATE_BAD_DATA;
                if (len > (size_t)(out_end - out)) return DH_INFLATE_NO_ROOM;
                const uint8_t* from = out - dist;
                uint8_t* const end = out + len;
                if (dist >= 8 && (size_t)(out_end - end) >= 8) {
                    /* 8 bytes at a time; up to 7 written past the match,
                     * overwritten by what follows (or room to spare). */
                    do {
                        memcpy(out, from, 8);
                        out += 8;
                        from += 8;
                    } while (out < end);
                } else if (dist == 1) {
                    memset(out, *from, len);
                } else {
                    while (out < end) *out++ = *from++;
                }
                out = end;
            }
        }
        if (final) break;
    }
    /* The bytes the stream used: those taken in, less the whole ones still
     * held (zero bytes past the end included). */
    const size_t used = (size_t)(in - src) + overrun - (bitsleft >> 3);
    if (used > src_len) return DH_INFLATE_SHORT;
    if (src_used) *src_used = used;
    return (long)(out - dst);
#undef REFILL
#undef DROP
#undef BITS
}
