// FMOD Ex's codec_raw SPU plugin as native code (fmod_codec_raw.h). Read in
// the lift (build/gen/spu/spu_0008_at_00FA9B80/spu_recomp.c) and the image's
// disassembly; checked offline against the lifted overlay itself
// (tests/native/fmod_codec_raw_check.cpp).
//
// Every value that can reach local store as a whole quadword is computed as
// one, with the runtime's own SPU helpers, in the operation order of the
// image (the File's 16-byte scalar fields, the stack frames, the bss): words
// 1-3 are register lanes the SPU compiler stores along with word 0, and they
// are reproduced, not approximated. The register names in the comments (rN)
// and the labels (L_xxx: the link address) are the image's, to make the port
// checkable against the lift line by line; what each block does in FMOD's
// terms is in the comments and in the header.
#include "fmod_codec_raw.h"

#include <cstring>

#include "spu_helpers.h"   // the runtime's exact SPU semantics (the lifted overlay's own)

namespace fmod_codec_raw {

unsigned long long g_path_hits[kPathCount];

#ifdef FMOD_CODEC_RAW_COVERAGE
#define COV(p) (++g_path_hits[(p)])
#else
#define COV(p) ((void)0)
#endif

namespace {

using Q = u128;

inline uint32_t w0(const Q& q) { return q._u32[0]; }
// The preferred halfword (bytes 2-3 of word 0), as the lift's brhz/brhnz test it.
inline uint16_t h0(const Q& q) { return q._u16[0]; }

struct Run {
    uint8_t* ls;
    uint32_t d;           // the relocation delta (the lift's _rl)
    const Channels* ch;
    uint32_t halt = 0;    // the link PC of a halt (heqi), else 0
};

// Local store, as spu_ls_read128/spu_ls_write128 address it: the quadword at
// (address & 0x3FFF0), big-endian words, word 0 first.
inline Q lq(const Run& c, uint32_t a)
{
    const uint8_t* p = c.ls + (a & 0x3FFF0u);
    Q v;
    for (int i = 0; i < 4; i++)
        v._u32[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    return v;
}
inline void stq(Run& c, uint32_t a, const Q& v)
{
    uint8_t* p = c.ls + (a & 0x3FFF0u);
    for (int i = 0; i < 4; i++) {
        p[4 * i] = (uint8_t)(v._u32[i] >> 24);
        p[4 * i + 1] = (uint8_t)(v._u32[i] >> 16);
        p[4 * i + 2] = (uint8_t)(v._u32[i] >> 8);
        p[4 * i + 3] = (uint8_t)v._u32[i];
    }
}
// lqd/stqd: base register's word 0 + displacement; lqx/stqx: two registers.
inline Q lq(const Run& c, const Q& base, int32_t off) { return lq(c, w0(base) + (uint32_t)off); }
inline void stq(Run& c, const Q& base, int32_t off, const Q& v) { stq(c, w0(base) + (uint32_t)off, v); }
inline Q lqx(const Run& c, const Q& a, const Q& b) { return lq(c, w0(a) + w0(b)); }
inline void stqx(Run& c, const Q& a, const Q& b, const Q& v) { stq(c, w0(a) + w0(b), v); }

// Channels: the runtime takes word 0 of a written register and returns a read
// value in word 0 of an otherwise zero register.
inline Q rdch(Run& c, uint32_t channel) { return spu_pref_u32(c.ch->rdch(c.ch->user, channel)); }
inline void wrch(Run& c, uint32_t channel, const Q& v) { c.ch->wrch(c.ch->user, channel, w0(v)); }

inline Q link(const Run& c, uint32_t a) { return spu_link(c.d + a); }   // brsl's return address

// heqi rX, 0: the lift's HALT-ASSERT.
inline bool halts(Run& c, const Q& v, uint32_t pc)
{
    if ((int32_t)w0(v) != 0) return false;
    c.halt = pc;
    COV(kPathHalt);
    return true;
}

// The SPU compiler's inline unsigned 32-bit division, every lane as the image
// does it (the quotient and the remainder of n / v; the loop is on word 0 of
// the shifted bit; the quad shifts carry bits across lanes). The caller has
// tested v (heqi).
struct Div { Q q, rem; };
Div udiv(const Q& n, const Q& v)
{
    const Q sh = spu_sf(spu_clz(n), spu_clz(v));   // clz(v) - clz(n)
    Q bit = spu_shl(spu_il(1), sh);
    Q dv = spu_shl(v, sh);
    Q q = spu_fsmbi(0x0);
    Q rem = spu_ori(n, 0);
    do {
        const Q t = spu_or(q, bit);
        bit = spu_rotqmbii(bit, -1);
        const Q gt = spu_clgt(dv, rem);
        const Q diff = spu_sf(dv, rem);
        dv = spu_rotqmbii(dv, -1);
        q = spu_selb(t, q, gt);
        rem = spu_selb(diff, rem, gt);
    } while (w0(bit));
    return Div{ q, rem };
}

// a * b, 32 bits, every lane (mpyh, mpyh, mpyu, two adds).
inline Q mul32(const Q& a, const Q& b) { return spu_a(spu_a(spu_mpyh(a, b), spu_mpyh(b, a)), spu_mpyu(a, b)); }

// A byte's bits reversed (the input already & 255), every lane.
Q bitrev8(const Q& x)
{
    const Q b0 = spu_shli(spu_andi(x, 1), 7), b1 = spu_shli(spu_andi(x, 2), 5), b2 = spu_shli(spu_andi(x, 4), 3);
    const Q b3 = spu_andi(x, 8);
    const Q b4 = spu_rotmai(spu_andi(x, 16), -1), b5 = spu_rotmai(spu_andi(x, 32), -3), b6 = spu_rotmai(spu_andi(x, 64), -5);
    const Q b7 = spu_rotmi(x, -7);
    return spu_or(spu_or(spu_or(spu_or(spu_or(spu_or(spu_or(b0, b1), b2), spu_a(b3, b3)), b4), b5), b6), b7);
}

// The callee-saved registers a function may save and change, as the caller
// has them (what a callee stores in its frame and reloads from it).
struct Saved { Q r0, r1, r80, r81, r82, r83, r84, r85, r86, r126; };

Saved from_gpr(const u128* g)
{
    return Saved{ g[0], g[1], g[80], g[81], g[82], g[83], g[84], g[85], g[86], g[126] };
}
void to_gpr(const Saved& s, u128* g)
{
    g[0] = s.r0; g[1] = s.r1;
    g[80] = s.r80; g[81] = s.r81; g[82] = s.r82; g[83] = s.r83; g[84] = s.r84; g[85] = s.r85; g[86] = s.r86;
    g[126] = s.r126;
}

// ---- 0x1A80 MemoryFile::reallySeek(this, pos): +0x348 = min(pos, length) ----
Q really_seek(Run& c, const Q& r1, const Q& self, const Q& pos)
{
    const Q r12 = lq(c, self, 0x340);
    const Q r11 = lq(c, self, 0x10);
    const Q r3 = spu_il(0);
    const Q r8 = spu_cwd(r1, 8);
    const Q r6 = spu_shufb(pos, r12, r8);
    const Q r5 = spu_rotqbyi(r11, 4);   // the length
    const Q r10 = spu_rotqbyi(r6, 8);
    const Q r4 = spu_clgt(pos, r5);
    const Q r9 = spu_selb(r10, r5, r4);
    stq(c, self, 0x340, spu_shufb(r9, r6, r8));
    return r3;
}

// ---- 0x1AC8 MemoryFile::reallyRead(this, dst, n, got*): the one DMA ----------
Q really_read(Run& c, const Q& r1, const Q& a3, const Q& a4, const Q& a5, const Q& a6)
{
    const Q r22 = spu_il(22);
    const Q r30 = lq(c, a3, 0x340);
    const Q r21 = spu_il(22);
    const Q r29 = lq(c, a3, 0x10);
    Q r14 = spu_ori(a5, 0);
    Q r17 = lq(c, c.d + 0x2F20);              // the previous decrementer
    const Q r13 = spu_ori(a3, 0);
    const Q r16 = rdch(c, SPU_RdDec);
    const Q r3 = spu_il(0);
    const Q r12 = spu_shlqbyi(a4, 0);
    const Q r28 = spu_rotqbyi(r30, 8);        // the offset (+0x348)
    const Q r27 = spu_rotqbyi(r29, 4);        // the length
    const Q r7 = spu_clgt(r16, r17);
    const Q r26 = spu_a(a5, r28);
    Q r5 = spu_fsmbi(0x0);
    const Q r25 = spu_sf(r28, r27);           // what is left
    const Q r18 = spu_clgt(r26, r27);
    const Q r23 = spu_andi(r25, 15);
    const Q r24 = spu_andi(r25, -16);
    const Q r20 = spu_ceqi(r23, 0);
    Q r11 = spu_ai(r24, 16);
    const Q r9 = spu_ceqi(r18, 0);            // it fits
    Q r10 = spu_selb(r22, r21, r20);
    const Q r8 = spu_selb(r11, r25, r20);     // what is left, rounded up to 16
    const Q r15 = spu_selb(r10, r3, r9);      // the result: 0 or 22
    r10 = spu_shlqbyi(a6, 0);
    r11 = spu_selb(r8, r14, r9);              // the DMA size
    r14 = spu_selb(r25, r14, r9);             // the bytes reported
    COV(w0(r9) ? kPathReadFits : kPathReadClipped);
    if (!w0(r7)) {
        // milliseconds since the previous read: (prev - now) / 79.8e6 * 1000
        COV(kPathPaceAccumulate);
        const Q r36 = spu_sf(r16, r17);
        Q r31 = spu_ilhu(0x4C98);
        const Q r34 = spu_csflt(r36, 155);
        r31 = spu_iohl(r31, 0x34D8);          // 79800000.0f
        const Q r35 = spu_ilhu(0x447A);       // 1000.0f
        const Q r33 = spu_frest(r31);
        const Q r4 = spu_fi(r31, r33);
        const Q r32 = spu_fm(r34, r35);
        const Q r6 = spu_fm(r32, r4);
        r17 = spu_fnms(r6, r31, r32);
        r5 = spu_fma(r17, r4, r6);
    } else {
        COV(kPathDecWrapped);
    }
    // L_1B78: the pacing of reads from 0xC0000000-0xCF8FFFFF
    Q r39 = spu_ilhu(0xF8F);
    const Q r40 = spu_ilhu(0x4000);
    const Q r41 = lq(c, r13, 0x340);
    r39 = spu_iohl(r39, 0xFFFF);
    const Q r42 = lq(c, c.d + 0x2F10);
    const Q r9b = spu_rotqbyi(r41, 12);       // the data's EA (+0x34C) in word 0
    const Q r7b = spu_fa(r5, r42);
    const Q r38 = spu_a(r9b, r40);
    const Q r37 = spu_clgt(r38, r39);
    stq(c, c.d + 0x2F10, r7b);
    if (!w0(r37)) {
        const Q r44 = spu_ilhu(0x3F80);       // 1.0f
        const Q r43 = spu_fcgt(r7b, r44);
        if (w0(r43)) {
            COV(kPathPaceSpin);
            Q r4 = spu_il(0);
            const Q r5b = spu_il(17500);
            Q r45;
            do {
                r4 = spu_ai(r4, 1);
                (void)rdch(c, SPU_RdDec);
                r45 = spu_ceq(r4, r5b);
            } while (!w0(r45));
            stq(c, c.d + 0x2F10, spu_il(0));
        }
    }
    // L_1BD8: the EA (64-bit add: carry into EAH), the alignment checks, the GET
    const Q r8b = spu_andi(r12, 15);
    const Q r5c = lq(c, r13, 0x340);
    const Q r3b = spu_il(16384);
    const Q r71 = spu_rotqmbyi(r9b, -4);
    const Q r7c = spu_ceqi(r8b, 0);
    const Q r75 = lq(c, c.d + 0x2C50);
    const Q r78 = spu_clgt(r11, r3b);
    const Q r67 = spu_sfi(r7c, 0);
    const Q r2 = rdch(c, SPU_RdDec);
    const Q r77 = spu_nor(r78, r78);
    const Q r76 = spu_andi(r11, 15);
    const Q r79 = spu_rotqbyi(r5c, 8);
    const Q r66 = spu_sfi(r77, 0);
    const Q r73 = spu_ceqi(r76, 0);
    stq(c, c.d + 0x2F20, r2);
    const Q r64 = spu_sfi(r73, 0);
    const Q r72 = spu_rotqmbyi(r79, -4);
    const Q r74 = spu_cg(r71, r72);
    Q r60 = spu_shufb(r74, r74, r75);
    r60 = spu_addx(r71, r72, r60);
    const Q r59 = spu_shlqbyi(r60, 4);
    const Q r70 = spu_andi(r59, 15);
    const Q r69 = spu_ceqi(r70, 0);
    const Q r68 = spu_sfi(r69, 0);
    const Q r65 = spu_and(r67, r68);
    const Q r63 = spu_and(r65, r66);
    const Q r62 = spu_and(r63, r64);
    if (halts(c, r62, 0x01C50)) return Q();
    wrch(c, MFC_LSA, r12);
    wrch(c, MFC_EAH, r60);
    wrch(c, MFC_EAL, r59);
    wrch(c, MFC_Size, r11);
    wrch(c, MFC_TagID, spu_il(1));
    wrch(c, MFC_Cmd, spu_il(64));                // GET
    wrch(c, MFC_WrTagMask, spu_il(2));
    wrch(c, MFC_WrTagUpdate, spu_il(2));
    (void)rdch(c, MFC_RdTagStat);
    const Q r52 = spu_cwd(r1, 8);
    const Q r48 = spu_cwd(r10, 0);
    const Q r54 = lq(c, r13, 0x340);
    const Q r53 = spu_rotqbyi(r54, 8);
    const Q r51 = spu_a(r53, r14);
    stq(c, r13, 0x340, spu_shufb(r51, r54, r52));  // offset += bytes
    const Q r47 = lq(c, r10, 0);
    stq(c, r10, 0, spu_shufb(r14, r47, r48));      // *got = bytes
    return spu_ori(r15, 0);
}

// ---- 0x760 File: position rounded down to the buffer size; reallySeek ---------
Q realign(Run& c, const Q& r1, const Q& self)
{
    const Q r15 = spu_il(0);
    const Q r17 = lq(c, self, 0x180);
    const Q r16 = lq(c, self, 0x1C0);
    stq(c, self, 0x190, r15);
    stq(c, self, 0x1B0, r15);
    const Q r19 = lq(c, self, 0x230);
    const Q r11 = spu_cwd(r1, 4);
    if (halts(c, r17, 0x00780)) return Q();
    const Div q = udiv(r16, r17);
    const Q r13 = spu_il(-769);
    const Q r14 = spu_rotqbyi(r19, 4);
    const Q r4 = mul32(r17, q.q);
    const Q r2 = spu_and(r14, r13);
    stq(c, self, 0x230, spu_shufb(r2, r19, r11));   // flags &= ~0x300
    const Q r10 = spu_sf(r4, r16);
    stq(c, self, 0x1D0, r4);
    stq(c, self, 0x1E0, r4);
    stq(c, self, 0x170, r10);
    return really_seek(c, r1, self, r4);
}

// ---- 0x818 File: read into the buffer (refill) ---------------------------------
// f: 0x818's registers (r80 = this, r1 = its frame).
void refill_body(Run& c, Saved& f)
{
    const Q self = f.r80;
    Q r5, r6, r7, r8, r9, r10;
    unsigned pieces = 0;
    {
        const Q r13 = lq(c, self, 0x230);
        r5 = lq(c, self, 0x190);
        r10 = lq(c, self, 0x1A0);
        const Q r12 = spu_cwd(f.r1, 4);
        const Q r8a = lq(c, self, 0x340);
        r6 = spu_rotqbyi(r13, 4);
        r7 = lq(c, self, 0x1B0);
        f.r81 = spu_sf(r5, r10);                   // to read: +0x1A0 - +0x190
        r6 = spu_ori(r6, 16);
        stq(c, self, 0x230, spu_shufb(r6, r13, r12));
        if (!w0(f.r81)) goto L_AD8;
        const Q r10b = spu_a(r8a, r7);
        f.r84 = spu_fsmbi(0x0);
        f.r85 = spu_ilhu(0x42C8);                 // 100.0f
        f.r82 = spu_a(r10b, r5);                   // buffer + 0x1B0 + 0x190
    }
L_978:
    {
        if (pieces++) COV(kPathRefillPieces);
        const Q r16 = spu_il(0);
        const Q r5a = spu_ori(f.r81, 0);
        const Q r15 = lq(c, f.r1, 0x20);
        const Q r4 = spu_ori(f.r82, 0);
        const Q r7a = spu_cwd(f.r1, 0);
        const Q r3 = spu_ori(self, 0);
        const Q r6a = spu_ai(f.r1, 32);
        stq(c, f.r1, 0x20, spu_shufb(r16, r15, r7a));   // got = 0 (a local)
        const Q got = really_read(c, f.r1, r3, r4, r5a, r6a);   // 0x1CC0 -> 0x1AC8
        if (c.halt) return;
        f.r83 = spu_ori(got, 0);
        const Q r3b = spu_ceqi(got, 22);
        if (w0(f.r83)) {
            // L_8B0
            const Q r26 = spu_sfi(r3b, 0);
            const Q r29 = lq(c, self, 0x10);
            const Q r5b = lq(c, self, 0x1E0);
            r8 = lq(c, f.r1, 0x20);
            const Q m9 = spu_cwd(f.r1, 8);
            const Q m6 = spu_cwd(f.r1, 12);
            const Q m7 = spu_cwd(f.r1, 4);
            const Q r28 = spu_rotqbyi(r29, 4);
            const Q r19 = spu_ceqi(r5b, 0);
            f.r81 = spu_ori(r8, 0);
            const Q r27 = spu_ceqi(r28, -1);
            const Q r25 = spu_sfi(r27, 0);
            const Q r24 = spu_and(r25, r26);
            const Q r23 = spu_ceqbi(r24, 0);
            const Q r22 = spu_xsbh(r23);
            const Q r21 = spu_ceqhi(r22, 0);
            const Q r20 = spu_nor(r21, r21);
            const Q r18 = spu_xshw(r20);
            const Q r17 = spu_or(r18, r19);
            if (!h0(r17)) {
                // L_A78: the length was unknown and the data ended: +0x14, +0x18, +0x1C = +0x1E0
                COV(kPathRefillLength);
                const Q r33 = lq(c, self, 0x10);
                const Q r32 = spu_shufb(r5b, r33, m9);
                const Q r31 = spu_shufb(r5b, r32, m6);
                stq(c, self, 0x10, spu_shufb(r5b, r31, m7));
            }
            goto L_900;
        }
        r8 = lq(c, f.r1, 0x20);
        f.r82 = spu_a(f.r82, r8);
        if (w0(r8)) goto L_908;
        goto L_9C0;
    }
L_900:
    f.r82 = spu_a(f.r82, r8);
    if (!w0(r8)) goto L_9C0;
L_908:
    {
        r5 = spu_clgt(r8, f.r81);
        f.r81 = spu_sf(r8, f.r81);
        if (w0(r5)) goto L_9C0;
        r9 = lq(c, self, 0x180);
        const Q r51 = lq(c, self, 0x1E0);
        const Q r49 = lq(c, self, 0x1C0);
        const Q r50 = lq(c, self, 0x230);
        const Q r44 = spu_cuflt(r9, 155);
        const Q r48 = spu_a(r8, r51);
        const Q r47 = spu_cuflt(r49, 155);
        const Q r46 = spu_cuflt(r48, 155);
        r6 = spu_rotqbyi(r50, 4);
        stq(c, self, 0x1E0, r48);
        const Q r34 = spu_andi(r6, 64);
        // the percentage: (+0x1E0 - +0x1C0) / +0x180 * 100, never below 0
        const Q r45 = spu_frest(r44);
        const Q r43 = spu_fs(r46, r47);
        const Q r42 = spu_fi(r44, r45);
        const Q r41 = spu_fm(r43, r42);
        const Q r40 = spu_fnms(r41, r44, r43);
        const Q r39 = spu_fma(r40, r42, r41);
        const Q r38 = spu_fm(r39, f.r85);
        const Q r36 = spu_cflts(r38, 173);
        const Q r37 = spu_cgti(r36, -1);
        stq(c, self, 0x200, spu_selb(f.r84, r36, r37));
        if (w0(r34)) goto L_A90;
        if (!w0(f.r81)) goto L_AC8;
        goto L_978;
    }
L_9C0:
    {
        const Q r52 = lq(c, self, 0x230);
        r10 = lq(c, self, 0x1A0);
        r7 = lq(c, self, 0x1B0);
        r9 = lq(c, self, 0x180);
        r6 = spu_rotqbyi(r52, 4);
    }
L_9D4:
    if (w0(r7)) goto L_AB0;
L_9D8:
    r6 = spu_ori(r6, 256);
    stq(c, self, 0x230, spu_shufb(r6, lq(c, self, 0x230), spu_cwd(f.r1, 4)));
L_9F0:
    {
        const Q r63 = spu_a(r7, r10);
        const Q r62 = spu_clgt(r9, r63);
        stq(c, self, 0x1B0, r63);
        if (!w0(r62)) {
            COV(kPathRefillWrap);
            stq(c, self, 0x1B0, spu_il(0));
        }
        // L_A10
        const Q r67 = spu_andi(r6, -145);
        const Q r66 = lq(c, self, 0x230);
        const Q r68 = spu_cwd(f.r1, 4);
        stq(c, self, 0x210, f.r83);
        stq(c, self, 0x190, spu_il(0));
        stq(c, self, 0x230, spu_shufb(r67, r66, r68));
        // a nonzero word at +0x230 calls 0x1E60, which returns 0 and touches nothing
        return;
    }
L_A90:
    {
        COV(kPathRefillFlag40);
        r6 = spu_andi(r6, -65);
        const Q r54 = lq(c, self, 0x230);
        const Q r55 = spu_cwd(f.r1, 4);
        r7 = lq(c, self, 0x1B0);
        r10 = lq(c, self, 0x1A0);
        stq(c, self, 0x230, spu_shufb(r6, r54, r55));
        if (!w0(r7)) goto L_9D8;
        goto L_AB0;
    }
L_AB0:
    r6 = spu_iohl(r6, 0x200);
    stq(c, self, 0x230, spu_shufb(r6, lq(c, self, 0x230), spu_cwd(f.r1, 4)));
    goto L_9F0;
L_AC8:
    r10 = lq(c, self, 0x1A0);
    r7 = lq(c, self, 0x1B0);
    goto L_9D4;
L_AD8:
    COV(kPathRefillNothing);
    f.r83 = spu_il(0);
    r9 = lq(c, self, 0x180);
    goto L_9D4;
}

Q refill(Run& c, Saved& s, const Q& a3, const Q& a4)
{
    const Q sp = s.r1;
    const Q r2 = spu_ceqbi(a4, 0);
    stq(c, sp, -0x10, s.r80);
    const Q r4 = spu_xsbh(r2);
    stq(c, sp, 0x10, s.r0);
    Saved f = s;
    f.r80 = a3;
    stq(c, sp, -0x20, s.r81);
    stq(c, sp, -0x30, s.r82);
    stq(c, sp, -0x40, s.r83);
    stq(c, sp, -0x50, s.r84);
    stq(c, sp, -0x60, s.r85);
    stq(c, sp, -0x70, s.r126);
    stq(c, sp, -0xA0, s.r1);
    f.r1 = spu_ai(s.r1, -160);
    // r4 (the argument) nonzero and a nonzero word at +0x230: a call to 0x1E58,
    // which returns 0 and touches nothing (its result is not used).
    (void)r4;
    refill_body(c, f);
    if (c.halt) return Q();
    // L_A48
    const Q r3 = spu_ori(f.r83, 0);
    s = f;
    s.r0 = lq(c, f.r1, 0xB0);
    s.r1 = spu_ai(f.r1, 160);
    s.r80 = lq(c, s.r1, -0x10);
    s.r81 = lq(c, s.r1, -0x20);
    s.r82 = lq(c, s.r1, -0x30);
    s.r83 = lq(c, s.r1, -0x40);
    s.r84 = lq(c, s.r1, -0x50);
    s.r85 = lq(c, s.r1, -0x60);
    s.r126 = lq(c, s.r1, -0x70);
    return r3;
}

// ---- 0xAE8 File: is the buffer good for the position? refill or realign -------
// f: 0xAE8's registers (r80 = this). Returns r7 (the result).
Q fill_body(Run& c, Saved& f, Q r5, const Q& r8, const Q& r2)
{
    const Q self = f.r80;
    Q r3, r4, r6, r7;
    if (w0(r2)) goto L_CA0;
    {
        const Q r19 = spu_il(640);
        const Q r20 = lq(c, self, 0x230);
        r7 = lq(c, self, 0x1A0);
        r4 = lq(c, self, 0x180);
        const Q r18 = spu_rotqbyi(r20, 4);
        const Q r16 = spu_sf(r7, r5);
        const Q r12 = spu_clgt(r4, r7);
        const Q r14 = spu_clgt(r16, r8);
        const Q r17 = spu_and(r18, r19);
        r6 = spu_nor(r12, r12);
        const Q r15 = spu_ceqi(r17, 0);
        const Q r13 = spu_nor(r15, r15);
        const Q r11 = spu_andc(r14, r13);
        const Q r10 = spu_ceqi(r11, 0);
        const Q r9 = spu_nor(r10, r10);
        r3 = spu_andc(r9, r6);
        if (w0(r3)) goto L_CA0;
        // the percentage: (+0x1E0 - +0x1C0) / +0x180 * 100
        const Q r29 = spu_cuflt(r4, 155);
        const Q r33 = lq(c, self, 0x1E0);
        const Q r32 = spu_cuflt(r8, 155);
        const Q r24 = spu_ilhu(0x42C8);
        const Q r31 = spu_cuflt(r33, 155);
        const Q r30 = spu_frest(r29);
        const Q r27 = spu_fi(r29, r30);
        const Q r28 = spu_fs(r31, r32);
        const Q r26 = spu_fm(r28, r27);
        const Q r25 = spu_fnms(r26, r29, r28);
        const Q r23 = spu_fma(r25, r27, r26);
        const Q r22 = spu_fm(r23, r24);
        const Q r21 = spu_cflts(r22, 173);
        r4 = spu_cgti(r21, -1);
        stq(c, self, 0x200, r21);
        if (!w0(r4)) goto L_D38;
        r4 = lq(c, self, 0x190);
        if (w0(r4)) goto L_D3C;
    }
L_BAC:
    {
        const Q r36 = spu_sf(r8, r5);
        r5 = spu_a(r7, r36);
        const Q r35 = spu_ai(r5, -1);
        if (halts(c, r7, 0x00BB8)) return Q();
        r5 = udiv(r35, r7).q;
        if (!w0(r4)) goto L_CB0;
    }
L_C0C:
    if (w0(spu_cgti(r5, 2))) goto L_D2C;
    {
        const Q r38 = lq(c, self, 0x170);
        stq(c, self, 0x1E0, r4);
        stq(c, self, 0x1D0, spu_sf(r38, r8));
    }
L_C24:
    {
        const Q r56 = spu_rotqbyi(lq(c, self, 0x230), 4);
        const Q r55 = spu_andi(r56, 1);
        const Q r54 = spu_ceqbi(r55, 0);
        const Q r53 = spu_xsbh(r54);
        if (!h0(r53)) goto L_E28;
    }
L_C3C:
    COV(kPathFillRefill);
    f.r0 = link(c, 0xC48);
    r3 = refill(c, f, self, spu_il(1));
    if (c.halt) return Q();
    {
        const Q r65 = spu_ceqi(r3, 0);
        r7 = spu_shlqbyi(r3, 0);
        r6 = spu_ceqi(r3, 22);
        const Q r64 = spu_nor(r65, r65);
        const Q r63 = spu_nor(r6, r6);
        const Q r61 = spu_sfi(r64, 0);
        const Q r62 = spu_sfi(r63, 0);
        const Q r60 = spu_and(r61, r62);
        const Q r59 = spu_ceqbi(r60, 0);
        const Q r58 = spu_xsbh(r59);
        if (h0(r58)) goto L_D48;                   // 0 or 22
        if (!w0(spu_ceqi(r3, 21))) goto L_D10;
        COV(kPathFillResult21);
        const Q r72 = lq(c, self, 0x230);
        const Q r70 = spu_cwd(f.r1, 4);
        const Q r71 = spu_rotqbyi(r72, 4);
        stq(c, self, 0x230, spu_shufb(spu_ori(r71, 32), r72, r70));
        goto L_D10;
    }
L_CA0:
    r5 = spu_il(-1);
    r4 = lq(c, self, 0x190);
    if (w0(r4)) goto L_C0C;
L_CB0:
    {
        const Q r45 = spu_ceqi(r5, 2);
        r6 = lq(c, self, 0x180);
        r4 = lq(c, self, 0x1A0);
        const Q r43 = spu_sfi(r45, 0);
        const Q r44 = spu_clgt(r6, r4);
        const Q r42 = spu_sfi(r44, 0);
        const Q r41 = spu_and(r42, r43);
        const Q r40 = spu_ceqbi(r41, 0);
        const Q r39 = spu_xsbh(r40);
        if (!h0(r39)) goto L_D08;
        const Q r52 = spu_ceq(r6, r4);
        const Q r51 = spu_ceqi(r5, 1);
        const Q r49 = spu_sfi(r52, 0);
        const Q r50 = spu_sfi(r51, 0);
        const Q r48 = spu_and(r49, r50);
        const Q r47 = spu_ceqbi(r48, 0);
        const Q r46 = spu_xsbh(r47);
        if (!h0(r46)) goto L_D08;
        if (!w0(r5)) goto L_C3C;
        goto L_C24;
    }
L_D08:
    r7 = spu_il(0);
L_D10:
    return r7;
L_D2C:
    stq(c, self, 0x190, spu_il(0));
    goto L_CB0;
L_D38:
    COV(kPathFillNegativePercent);
    r4 = lq(c, self, 0x190);
L_D3C:
    stq(c, self, 0x200, spu_il(0));
    goto L_BAC;
L_D48:
    {
        r6 = spu_sfi(r6, 0);
        const Q r17 = lq(c, self, 0x180);
        const Q r8b = lq(c, self, 0x1A0);
        const Q r16 = lq(c, self, 0x230);
        const Q r13 = lq(c, self, 0x10);
        f.r81 = spu_cwd(f.r1, 4);
        const Q r14 = spu_ceq(r17, r8b);
        const Q r15 = spu_rotqbyi(r16, 4);
        const Q r12 = spu_sfi(r14, 0);
        const Q r11 = spu_rotqbyi(r13, 4);
        const Q r3b = spu_and(r12, r6);
        const Q r2b = spu_andi(r15, -33);
        const Q r79 = spu_ceqbi(r3b, 0);
        const Q r75 = spu_ceqi(r11, -1);
        const Q r9 = spu_shufb(r2b, r16, f.r81);
        const Q r78 = spu_xsbh(r79);
        const Q r77 = spu_ceqhi(r78, 0);
        stq(c, self, 0x230, r9);                   // flags &= ~0x20
        const Q r76 = spu_nor(r77, r77);
        const Q r74 = spu_xshw(r76);
        const Q r73 = spu_andc(r75, r74);
        if (h0(r73)) goto L_D10;
        const Q r20 = lq(c, self, 0x1D0);
        const Q r4b = lq(c, self, 0x170);
        const Q r19 = spu_a(r20, r8b);
        stq(c, self, 0x1E0, r20);
        const Q r18 = spu_clgt(r8b, r4b);
        stq(c, self, 0x1D0, r19);
        if (w0(r18)) goto L_D10;
    }
    COV(kPathFillAgain);
    f.r0 = link(c, 0xDD0);
    r3 = refill(c, f, self, spu_il(1));
    if (c.halt) return Q();
    {
        const Q r29 = spu_ceqi(r3, 0);
        r7 = spu_shlqbyi(r3, 0);
        const Q r28 = spu_ceqi(r3, 22);
        const Q r27 = spu_nor(r29, r29);
        const Q r26 = spu_nor(r28, r28);
        const Q r24 = spu_sfi(r27, 0);
        const Q r25 = spu_sfi(r26, 0);
        const Q r23 = spu_and(r24, r25);
        const Q r22 = spu_ceqbi(r23, 0);
        const Q r21 = spu_xsbh(r22);
        if (h0(r21)) goto L_E3C;
        if (!w0(spu_ceqi(r3, 21))) goto L_D10;
        COV(kPathFillResult21);
        const Q r35 = lq(c, self, 0x230);
        const Q r34 = spu_rotqbyi(r35, 4);
        stq(c, self, 0x230, spu_shufb(spu_ori(r34, 32), r35, f.r81));
        goto L_D10;
    }
L_E28:
    COV(kPathFillRealign);
    f.r0 = link(c, 0xE30);
    r3 = realign(c, f.r1, self);
    if (c.halt) return Q();
    r7 = spu_ori(r3, 0);
    if (!w0(r3)) goto L_C3C;
    goto L_D10;
L_E3C:
    {
        const Q r41 = lq(c, self, 0x230);
        const Q r38 = lq(c, self, 0x1D0);
        const Q r39 = lq(c, self, 0x1A0);
        const Q r40 = spu_rotqbyi(r41, 4);
        stq(c, self, 0x1E0, r38);
        const Q r36 = spu_a(r38, r39);
        const Q r37 = spu_andi(r40, -33);
        stq(c, self, 0x1D0, r36);
        stq(c, self, 0x230, spu_shufb(r37, r41, f.r81));
        goto L_D10;
    }
}

Q fill(Run& c, Saved& s, const Q& a3)
{
    const Q sp = s.r1;
    const Q r5 = lq(c, a3, 0x1D0);
    const Q r8 = lq(c, a3, 0x1C0);
    stq(c, sp, -0x10, s.r80);
    Saved f = s;
    f.r80 = a3;
    stq(c, sp, 0x10, s.r0);
    stq(c, sp, -0x20, s.r81);
    stq(c, sp, -0x30, s.r126);
    stq(c, sp, -0x50, s.r1);
    const Q r2 = spu_clgt(r8, r5);
    f.r1 = spu_ai(s.r1, -80);
    const Q r7 = fill_body(c, f, r5, r8, r2);
    if (c.halt) return Q();
    // L_D10
    const Q r3 = spu_ori(r7, 0);
    s = f;   // r82-r86 as the refill left them
    s.r0 = lq(c, f.r1, 0x60);
    s.r1 = spu_ai(f.r1, 80);
    s.r80 = lq(c, s.r1, -0x10);
    s.r81 = lq(c, s.r1, -0x20);
    s.r126 = lq(c, s.r1, -0x30);
    return r3;
}

// ---- File::read's copy out of the File's buffer --------------------------------
// L_FF0 (both 16-byte aligned): whole quadwords, then the tail merged (L_1244).
void copy_aligned(Run& c, const Q& r12, const Q& r13, const Q& r14)
{
    COV(kPathCopyAligned);
    Q r4 = spu_ori(r14, 0), r3 = spu_shlqbyi(r12, 0), r6 = spu_shlqbyi(r13, 0);
    if (w0(spu_clgti(r14, 15))) {
        Q r5 = spu_il(0), more;
        do {
            r4 = spu_ai(r4, -16);
            const Q r34 = lqx(c, r5, r12);
            more = spu_clgti(r4, 15);
            stqx(c, r5, r13, r34);
            r5 = spu_ai(r5, 16);
        } while (w0(more));
        r3 = spu_a(r12, r5);
        r6 = spu_a(r13, r5);
    }
    if (w0(r4)) {
        COV(kPathCopyTail);
        const Q r36 = lq(c, r3, 0);
        const Q r40 = spu_sfi(r4, 0);
        const Q r38 = lq(c, r6, 0);
        const Q r37 = spu_rotqmby(spu_il(-1), r40);
        stq(c, r6, 0, spu_selb(r36, r38, r37));
    }
}

// L_12A0 (either unaligned): the first destination quadword through a shift
// pattern (rodata 0x2C60 + 16 x the offset difference) and masks, then whole
// quadwords from two source quadwords, then the tail.
void copy_unaligned(Run& c, const Q& r126, const Q& r12, const Q& r13, const Q& r14, const Q& r5in, const Q& r15)
{
    COV(kPathCopyUnaligned);
    const Q r61 = spu_sf(r15, r5in);
    const Q r57 = lq(c, r12, 0);
    const Q r65 = spu_ila(0x2C60);
    const Q r58 = lq(c, r12, 0x10);
    const Q r64 = spu_shli(r61, 4);
    const Q r53 = spu_fsmbi(0xFFFF);
    const Q r63 = spu_a(r65, r126);
    const Q r45 = lq(c, r13, 0);
    const Q r62 = spu_a(r14, r15);
    const Q r59 = spu_cgti(r61, 0);
    const Q r60 = spu_sfi(r62, 0);
    const Q r56 = lqx(c, r64, r63);
    const Q r41 = spu_clgti(r62, 15);
    const Q r51 = spu_rotqmby(r57, r61);
    const Q r5 = spu_sfi(r15, 16);
    const Q r49 = spu_rotqmby(r53, r60);
    const Q r55 = spu_ceqi(r59, 0);
    const Q r54 = spu_ceqi(r41, 0);
    const Q r46 = spu_shlqby(r53, r5);
    const Q r50 = spu_fsm(r55);
    const Q r52 = spu_shufb(r57, r58, r56);
    const Q r47 = spu_fsm(r54);
    const Q r48 = spu_or(r46, r49);
    const Q r43 = spu_selb(r52, r51, r50);
    const Q r44 = spu_selb(r46, r48, r47);
    stq(c, r13, 0, spu_selb(r43, r45, r44));
    if (!w0(r41)) return;
    Q r10 = spu_a(r12, r5);
    const Q r70 = spu_ila(0x2C60);
    const Q r71 = spu_andi(r10, 15);
    const Q r68 = spu_a(r70, r126);
    const Q r69 = spu_shli(r71, 4);
    Q r7 = spu_sf(r5, r14);
    Q r11 = spu_a(r13, r5);
    const Q r67 = spu_clgti(r7, 15);
    const Q r8 = lqx(c, r69, r68);
    if (w0(r67)) {
        Q r72;
        do {
            r7 = spu_ai(r7, -16);
            const Q r74 = lq(c, r10, 0);
            r10 = spu_ai(r10, 16);
            r72 = spu_clgti(r7, 15);
            const Q r75 = lq(c, r10, 0);
            stq(c, r11, 0, spu_shufb(r74, r75, r8));
            r11 = spu_ai(r11, 16);
        } while (w0(r72));
    }
    if (!w0(r7)) return;
    COV(kPathCopyTail);
    const Q r4 = spu_sfi(r7, 0);
    const Q r5b = lq(c, r10, 0x10);
    const Q r2 = spu_il(-1);
    const Q r3 = lq(c, r10, 0);
    const Q r79 = lq(c, r11, 0);
    const Q r78 = spu_rotqmby(r2, r4);
    const Q r77 = spu_shufb(r3, r5b, r8);
    stq(c, r11, 0, spu_selb(r77, r79, r78));
}

// ---- 0xE70 File::read(this, buffer, size, count, bytesread*) --------------------
// f: File::read's registers. Returns r12 (the result).
Q file_read_body(Run& c, Saved& f, const Q& a4, const Q& a5, const Q& a6)
{
    const Q self = f.r80;
    if (!w0(a4)) {
        COV(kPathFileReadNoBuffer);
        return spu_il(37);
    }
    Q n = mul32(a5, a6);                           // r8 = size * count
    if (!w0(spu_cgti(n, -1))) {
        COV(kPathFileReadNegative);
        return spu_il(37);
    }
    f.r86 = spu_il(0);
    {
        const Q r19 = lq(c, self, 0x230);
        const Q r14 = lq(c, self, 0x10);
        const Q r9 = lq(c, self, 0x1C0);
        const Q r12 = lq(c, self, 0x1F0);
        const Q r17 = spu_cwd(f.r1, 4);
        const Q r18 = spu_rotqbyi(r19, 4);
        const Q r13 = spu_rotqbyi(r14, 4);
        const Q r10 = spu_a(n, r9);
        const Q r16 = spu_andi(r18, -65);
        const Q r7 = spu_a(r12, r13);              // the end: start + length
        const Q r11 = spu_shufb(r16, r19, r17);
        const Q r6 = spu_clgt(r10, r7);
        stq(c, self, 0x230, r11);                  // flags &= ~0x40
        if (w0(r6)) {
            const Q r20 = spu_clgt(r9, r7);
            if (w0(r20)) {
                COV(kPathFileReadPastEnd);
                return spu_il(19);
            }
            COV(kPathFileReadClipped);
            n = spu_sf(r9, r7);
            f.r86 = spu_il(1);
        }
    }
    // L_F38
    Q result = spu_il(0);
    f.r83 = spu_fsmbi(0x0);
    if (w0(n)) {
        f.r81 = n;
        for (;;) {
            // L_F48
            f.r0 = link(c, 0xF50);
            const Q got = fill(c, f, spu_ori(self, 0));
            if (c.halt) return Q();
            result = spu_shlqbyi(got, 0);
            Q bs;                                    // r6: the block size (+0x1A0)
            if (w0(spu_ceqi(got, 22))) {
                // L_13A8: the data ended: go on only if the buffer is partial or the length known
                COV(kPathFillEof);
                const Q r27 = lq(c, self, 0x10);
                bs = lq(c, self, 0x1A0);
                const Q r28 = lq(c, self, 0x180);
                const Q r26 = spu_rotqbyi(r27, 4);
                const Q r25 = spu_ceq(bs, r28);
                const Q r24 = spu_ceqi(r26, -1);
                const Q r23 = spu_nor(r25, r25);
                const Q r22 = spu_nor(r24, r24);
                if (!w0(spu_or(r23, r22))) break;
            } else if (w0(got)) {
                break;
            } else {
                bs = lq(c, self, 0x1A0);
            }
            // L_F64
            Q pos = lq(c, self, 0x170);              // r4: the buffer position
            if (halts(c, bs, 0x00F68)) return Q();
            const Q r30 = udiv(pos, bs).rem;
            Q amount = spu_sf(r30, bs);               // r14: left in this block
            const Q r29 = spu_clgt(f.r81, amount);
            Q r12, r13, r5, r15, r31;
            if (!w0(r29)) {
                // L_1270: the last piece
                r13 = spu_a(f.r83, f.r82);
                amount = spu_ori(f.r81, 0);
                r12 = lq(c, self, 0x340);
                r15 = spu_andi(r13, 15);
                r12 = spu_a(pos, r12);
                r5 = spu_andi(r12, 15);
                r31 = spu_or(r5, r15);
            } else {
                const Q r10 = lq(c, self, 0x340);
                if (!w0(amount)) goto L_1030;
                r12 = spu_a(r10, pos);
                r13 = spu_a(f.r82, f.r83);
                r5 = spu_andi(r12, 15);
                r15 = spu_andi(r13, 15);
                r31 = spu_or(r5, r15);
            }
            if (w0(r31)) copy_unaligned(c, f.r126, r12, r13, amount, r5, r15);
            else copy_aligned(c, r12, r13, amount);
            pos = lq(c, self, 0x170);
        L_1030:
            {
                const Q r6 = spu_a(amount, pos);
                const Q r8 = lq(c, self, 0x180);
                stq(c, self, 0x170, r6);
                if (!w0(spu_clgt(r8, r6))) {
                    COV(kPathBufferWrap);
                    stq(c, self, 0x170, spu_il(0));
                }
                // L_1050
                f.r81 = spu_sf(amount, f.r81);
                const Q r13b = lq(c, self, 0x1C0);
                f.r83 = spu_a(f.r83, amount);
                stq(c, self, 0x1C0, spu_a(r13b, amount));
                if (!w0(f.r81)) {
                    result = spu_il(0);
                    break;
                }
            }
        }
    }
    // L_106C: byte order
    if (w0(spu_ceqi(f.r84, 2))) {
        // L_13D8: 16-bit elements swapped unless flag 0x8
        const Q r15 = spu_rotqbyi(lq(c, self, 0x230), 4);
        if (!w0(spu_andi(r15, 8))) {
            const Q r9 = spu_rotmi(f.r83, -1);
            if (w0(r9)) {
                COV(kPathSwap16);
                Q r8 = spu_il(0), r7 = spu_il(0), r17;
                const Q r10 = spu_ila(0xFF00);
                do {
                    const Q r28 = spu_a(r7, f.r82);
                    const Q r26 = lqx(c, r7, f.r82);
                    r8 = spu_ai(r8, 1);
                    const Q r21 = spu_chx(r7, f.r82);
                    const Q r27 = spu_ai(r28, 14);
                    r17 = spu_ceq(r9, r8);
                    const Q r25 = spu_rotqby(r26, r27);
                    const Q r24 = spu_and(r10, r25);
                    const Q r22 = spu_shlhi(r25, 8);
                    const Q r23 = spu_rotmai(r24, -8);
                    stqx(c, r7, f.r82, spu_shufb(spu_or(r22, r23), r26, r21));
                    r7 = spu_ai(r7, 2);
                } while (!w0(r17));
            }
        }
    } else {
        const Q r34 = spu_ceqi(f.r84, 4);
        const Q r33 = lq(c, self, 0x230);
        const Q r30 = spu_nor(r34, r34);
        const Q r32 = spu_rotqbyi(r33, 4);
        const Q r31 = spu_andi(r32, 8);
        const Q r10 = spu_ceqi(r31, 0);
        const Q r29 = spu_nor(r10, r10);
        if (!w0(spu_or(r30, r29))) {
            // L_1520: 32-bit elements swapped
            const Q r11 = spu_rotmi(f.r83, -2);
            if (w0(r11)) {
                COV(kPathSwap32);
                Q r10b = spu_il(0), r9 = spu_il(0), r35;
                const Q r13 = spu_ila(0xFF00), r14 = spu_ilhu(0xFF);
                do {
                    const Q r50 = spu_a(r9, f.r82);
                    const Q r49 = lqx(c, r9, f.r82);
                    r10b = spu_ai(r10b, 1);
                    const Q r39 = spu_cwx(r9, f.r82);
                    r35 = spu_ceq(r11, r10b);
                    const Q r46 = spu_rotqby(r49, r50);
                    const Q r47 = spu_and(r46, r13);
                    const Q r48 = spu_and(r46, r14);
                    const Q r45 = spu_rotmi(r46, -24);
                    const Q r44 = spu_rotmi(r48, -8);
                    const Q r43 = spu_shli(r47, 8);
                    const Q r42 = spu_shli(r46, 24);
                    const Q r38 = spu_or(spu_or(r42, r43), spu_or(r44, r45));
                    stqx(c, r9, f.r82, spu_shufb(r38, r49, r39));
                    r9 = spu_ai(r9, 4);
                } while (!w0(r35));
            }
        }
    }
    // L_10A0: elements = bytes / size
    if (halts(c, f.r84, 0x010A0)) return Q();
    const Q elements = udiv(f.r83, f.r84).q;      // r13
    if (w0(lq(c, self, 0x150))) {
        // a key: (bytes / size) bytes [sic] decrypted, the key index advancing
        const Q r84 = spu_rotqbyi(lq(c, self, 0x230), 4);
        const Q r83 = spu_and(r84, spu_il(1024));
        const Q r14 = spu_il(0);
        Q r11 = spu_il(0);
        if (w0(r83)) {
            if (w0(elements)) {
                // L_1458: XOR with the key, then the bits reversed
                COV(kPathDecryptXorFirst);
                Q r55;
                do {
                    const Q r22 = spu_a(r11, f.r82);
                    const Q r21 = lq(c, self, 0x160);
                    const Q r19 = lqx(c, r11, f.r82);
                    const Q r20 = spu_ai(r22, 13);
                    const Q r64 = spu_cbx(r11, f.r82);
                    const Q r18 = spu_a(r21, self);
                    const Q r15 = spu_rotqby(r19, r20);
                    const Q r17 = spu_ai(r18, 13);
                    const Q r16 = lq(c, r18, 0x130);
                    const Q r81 = spu_rotqby(r16, r17);
                    const Q r70 = spu_andi(spu_xor(r15, r81), 255);
                    stqx(c, r11, f.r82, spu_shufb(bitrev8(r70), r19, r64));
                    r11 = spu_ai(r11, 1);
                    const Q r60 = lq(c, self, 0x160);
                    const Q r59 = lq(c, self, 0x150);
                    r55 = spu_ceq(elements, r11);
                    const Q r57 = spu_ai(r60, 1);
                    const Q r58 = spu_cgt(r59, r57);
                    stq(c, self, 0x160, spu_selb(r14, r57, r58));
                } while (!w0(r55));
            }
        } else if (w0(elements)) {
            // L_1120: the bits reversed, then XOR with the key
            COV(kPathDecrypt);
            Q r23;
            do {
                const Q r62 = spu_a(r11, f.r82);
                const Q r60 = lqx(c, r11, f.r82);
                const Q r9 = spu_cbx(r11, f.r82);
                const Q r61 = spu_ai(r62, 13);
                const Q r59 = spu_rotqby(r60, r61);
                const Q r31 = bitrev8(spu_andi(r59, 255));
                const Q r10 = spu_shufb(r31, r60, r9);
                stqx(c, r11, f.r82, r10);
                const Q r36 = lq(c, self, 0x160);
                const Q r35 = spu_a(r36, self);
                const Q r34 = spu_ai(r35, 13);
                const Q r33 = lq(c, r35, 0x130);
                const Q r32 = spu_rotqby(r33, r34);
                stqx(c, r11, f.r82, spu_shufb(spu_xor(r31, r32), r10, r9));
                r11 = spu_ai(r11, 1);
                const Q r28 = lq(c, self, 0x160);
                const Q r27 = lq(c, self, 0x150);
                r23 = spu_ceq(elements, r11);
                const Q r25 = spu_ai(r28, 1);
                const Q r26 = spu_cgt(r27, r25);
                stq(c, self, 0x160, spu_selb(r14, r25, r26));
            } while (!w0(r23));
        }
    }
    // L_11E0: *bytesread = elements (a null pointer: the word at 0 rewritten as it is)
    {
        const Q p = f.r85;
        if (!w0(p)) COV(kPathBytesReadNull);
        const Q r64 = spu_ceqi(p, 0);
        const Q r65 = lq(c, p, 0);
        const Q r14 = spu_cwd(p, 0);
        const Q r63 = spu_rotqby(r65, p);
        const Q r11 = spu_selb(elements, r63, r64);
        stq(c, p, 0, spu_shufb(r11, r65, r14));
    }
    if (w0(result)) return result;
    const Q r13 = spu_ceqbi(f.r86, 0);
    if (h0(spu_xsbh(r13))) return result;
    COV(kPathResultEof);
    return spu_il(22);                             // clipped at the end
}

Q file_read(Run& c, Saved& s, const Q& a3, const Q& a4, const Q& a5, const Q& a6, const Q& a7)
{
    const Q sp = s.r1;
    stq(c, sp, -0x80, s.r126);                     // 0xE70
    const Q r2 = spu_ila(0xE80);
    Saved f = s;
    stq(c, sp, -0x10, s.r80);
    f.r80 = spu_ori(a3, 0);
    stq(c, sp, -0x30, s.r82);
    f.r82 = spu_ori(a4, 0);
    stq(c, sp, -0x50, s.r84);
    f.r126 = spu_sf(r2, link(c, 0xE80));           // {delta, -0xE80, -0xE80, -0xE80}
    stq(c, sp, -0x60, s.r85);
    f.r84 = spu_ori(a5, 0);
    stq(c, sp, 0x10, s.r0);
    f.r85 = spu_ori(a7, 0);
    stq(c, sp, -0x20, s.r81);
    stq(c, sp, -0x40, s.r83);
    stq(c, sp, -0x70, s.r86);
    stq(c, sp, -0xA0, s.r1);
    f.r1 = spu_ai(s.r1, -160);
    const Q r12 = file_read_body(c, f, a4, a5, a6);
    if (c.halt) return Q();
    // L_1210
    const Q r3 = spu_ori(r12, 0);
    s.r0 = lq(c, f.r1, 0xB0);
    s.r1 = spu_ai(f.r1, 160);
    s.r80 = lq(c, s.r1, -0x10);
    s.r81 = lq(c, s.r1, -0x20);
    s.r82 = lq(c, s.r1, -0x30);
    s.r83 = lq(c, s.r1, -0x40);
    s.r84 = lq(c, s.r1, -0x50);
    s.r85 = lq(c, s.r1, -0x60);
    s.r86 = lq(c, s.r1, -0x70);
    s.r126 = lq(c, s.r1, -0x80);
    return r3;
}

// ---- 0x550 File::seek(this, pos, mode): a leaf -----------------------------------
Q seek(Run& c, const Q& r1, const Q& a3, const Q& a4, const Q& a5)
{
    const Q self = spu_shlqbyi(a3, 0);             // r6
    const Q r8 = spu_ori(a4, 0);                   // the position asked
    Q r4, r5, r7, r9, r10, r8b;
    if (w0(spu_clgti(a5, 2))) {
        COV(kPathSeekBadMode);
        return spu_il(37);
    }
    {
        const Q r10a = lq(c, a3, 0x230);
        const Q m = spu_cwd(r1, 4);
        const Q r9a = spu_rotqbyi(r10a, 4);
        r10 = spu_andi(r9a, -65);                  // flags & ~0x40
        stq(c, self, 0x230, spu_shufb(r10, r10a, m));
    }
    if (w0(a5)) {
        if (w0(spu_ceqi(a5, 1))) {
            // L_748: from the current position
            COV(kPathSeekCur);
            const Q r14 = lq(c, self, 0x10);
            const Q r13 = lq(c, self, 0x1C0);
            r7 = lq(c, self, 0x1F0);
            r5 = spu_rotqbyi(r14, 4);
            r4 = spu_a(r8, r13);
        } else if (w0(spu_ceqi(a5, 2))) {
            // L_6F4: from the end
            COV(kPathSeekEnd);
            const Q r17 = lq(c, self, 0x10);
            r7 = lq(c, self, 0x1F0);
            r5 = spu_rotqbyi(r17, 4);
            r4 = spu_a(spu_a(r7, r5), r8);
        } else {
            goto L_6E0;
        }
    } else {
        COV(kPathSeekSet);
        const Q r11 = lq(c, self, 0x10);
        r7 = lq(c, self, 0x1F0);
        r5 = spu_rotqbyi(r11, 4);
        r4 = spu_a(r8, r7);                        // start + pos
    }
    {
        // L_598: past the end: to the end, or to 0 for a negative position
        const Q r3 = spu_a(r7, r5);
        if (w0(spu_clgt(r4, r3))) {
            COV(kPathSeekClamp);
            r4 = spu_shlqbyi(r3, 0);
            if (!w0(spu_cgti(r8, -1))) goto L_6E0;
        }
    }
    // L_5B4
    r9 = lq(c, self, 0x1D0);
    if (w0(r9)) goto L_6EC;
L_5BC:
    if (w0(spu_andi(r10, 1))) goto L_6EC;
    r8b = lq(c, self, 0x1A0);
    if (!w0(spu_clgt(r8b, r4))) goto L_740;
L_5D0:
    {
        const Q r30 = spu_andi(r10, 1);
        r5 = lq(c, self, 0x180);
        const Q r29 = spu_ceqi(r30, 0);
        const Q r28 = spu_nor(r29, r29);
        const Q r27 = spu_clgt(r5, r9);
        const Q r26 = spu_ceq(r5, r8b);
        const Q r25 = spu_or(r28, r27);
        const Q r23 = spu_nor(r26, r26);
        const Q r24 = spu_ceqi(r25, 0);
        const Q r22 = spu_nor(r24, r24);
        if (!w0(spu_or(r23, r22))) {
            // L_718: a non-seekable buffer: only within what it holds
            const Q r34 = spu_sf(r5, r9);
            const Q r33 = spu_ceqi(r9, 0);
            const Q r32 = spu_clgt(r34, r4);
            if (w0(spu_andc(r32, r33))) goto L_740;
            if (!w0(spu_clgt(spu_a(r9, r5), r4))) goto L_740;
        }
    }
    // L_600
    {
        const Q r2 = lq(c, self, 0x150);
        stq(c, self, 0x1C0, r4);                   // the position
        if (w0(r2)) {
            COV(kPathSeekKey);
            if (halts(c, r2, 0x00610)) return Q();
            stq(c, self, 0x160, udiv(r4, r2).rem); // the key index
        }
    }
    // L_664
    if (!w0(r5)) {
        COV(kPathSeekUnbuffered);
        return really_seek(c, r1, self, r4);       // L_710
    }
    COV(kPathSeekBuffered);
    r7 = spu_il(0);
    {
        const Q r39 = lq(c, self, 0x1C0);
        if (halts(c, r5, 0x00670)) return Q();
        stq(c, self, 0x170, udiv(r39, r5).rem);    // the buffer position
    }
    return spu_ori(r7, 0);
L_6E0:
    COV(kPathSeekNegative);
    r4 = spu_il(0);
    r9 = lq(c, self, 0x1D0);
    if (!w0(r9)) goto L_5BC;
L_6EC:
    r8b = lq(c, self, 0x1A0);
    goto L_5D0;
L_740:
    COV(kPathSeekCannot);
    return spu_il(20);
}

// ---- 0x1FC0 __muldi3, 0x2078 __udivdi3 (libgcc), instruction by instruction ---------
// On the image's registers r (a copy of the register file): their lanes 2-3
// (and the inputs' lanes) reach the File's +0x1C0 words 1-3 through
// setposition. Returns 0, or the link PC of a halt. Generated from the lift
// (ctx->gpr -> r, a transfer to another of its blocks -> goto L_<address>).
uint32_t muldi3(u128* r)
{
    r[44] = spu_ori(r[3], 0);
    r[45] = spu_rotqbyi(r[4], 8);
    r[37] = spu_ila(0x10000);
    r[14] = spu_cwd(r[1], 0);
    r[13] = spu_ila(0xFFFF);
    r[43] = spu_rotqbyi(r[44], 8);
    r[28] = spu_shlqbyi(r[45], 12);
    r[8] = spu_cwd(r[1], 4);
    r[33] = spu_shlqbyi(r[43], 12);
    r[42] = spu_rotmi(r[28], -16);
    r[19] = spu_mpyh(r[28], r[3]);
    r[41] = spu_rotmi(r[33], -16);
    r[21] = spu_mpyu(r[33], r[28]);
    r[40] = spu_mpyu(r[33], r[42]);
    r[22] = spu_mpyh(r[33], r[4]);
    r[36] = spu_mpyu(r[41], r[28]);
    r[30] = spu_mpyu(r[41], r[42]);
    r[23] = spu_mpyh(r[4], r[33]);
    r[20] = spu_mpyh(r[3], r[28]);
    r[38] = spu_rotmi(r[21], -16);
    r[17] = spu_mpyu(r[33], r[4]);
    r[10] = spu_and(r[21], r[13]);
    r[39] = spu_a(r[40], r[36]);
    r[32] = spu_a(r[30], r[37]);
    r[25] = spu_a(r[38], r[39]);
    r[15] = spu_mpyu(r[28], r[3]);
    r[35] = spu_clgt(r[36], r[25]);
    r[27] = spu_rotmi(r[25], -16);
    r[31] = spu_ceqi(r[35], 0);
    r[18] = spu_shli(r[25], 16);
    r[26] = spu_selb(r[32], r[30], r[31]);
    r[16] = spu_a(r[22], r[23]);
    r[24] = spu_a(r[26], r[27]);
    r[7] = spu_a(r[18], r[10]);
    r[12] = spu_a(r[19], r[20]);
    r[11] = spu_shufb(r[24], r[24], r[14]);
    r[9] = spu_a(r[16], r[17]);
    r[6] = spu_a(r[12], r[15]);
    r[5] = spu_shufb(r[7], r[11], r[8]);
    r[2] = spu_a(r[5], r[9]);
    r[3] = spu_a(r[6], r[2]);
    r[3] = spu_shufb(r[3], r[5], r[14]);
    return 0;
}

uint32_t udivdi3(u128* r)
{
    r[13] = spu_ori(r[3], 0);
    r[3] = spu_rotqbyi(r[3], 8);
    r[8] = spu_ori(r[4], 0);
    r[4] = spu_rotqbyi(r[4], 8);
    r[18] = spu_shlqbyi(r[3], 12);
    r[12] = spu_shlqbyi(r[4], 12);
    if (r[8]._u32[0] != 0) goto L_22C4;
    r[2] = spu_clgt(r[12], r[13]);
    if (r[2]._u32[0] != 0) goto L_28C0;
    if (r[12]._u32[0] == 0) goto L_283C;
    r[74] = spu_ori(r[12], 0);
    r[73] = spu_ila(0xFFFF);
    r[14] = spu_clz(r[74]);
    r[11] = spu_rotmi(r[12], -16);
    r[15] = spu_and(r[12], r[73]);
    r[4] = spu_sf(r[12], r[13]);
    r[8] = spu_il(1);
    if (r[14]._u32[0] != 0) goto L_25F0;
L_20C0:
    if ((int32_t)r[11]._u32[0] == 0) return 0x020C0;
    r[23] = spu_clz(r[11]);
    r[20] = spu_clz(r[4]);
    r[21] = spu_il(1);
    r[79] = spu_fsmbi(0x0);
    r[23] = spu_sf(r[20], r[23]);
    r[35] = spu_ori(r[4], 0);
    r[21] = spu_shl(r[21], r[23]);
    r[20] = spu_shl(r[11], r[23]);
L_20E8:
    r[3] = spu_or(r[79], r[21]);
    r[21] = spu_rotqmbii(r[21], -1);
    r[22] = spu_clgt(r[20], r[35]);
    r[23] = spu_sf(r[20], r[35]);
    r[20] = spu_rotqmbii(r[20], -1);
    r[79] = spu_selb(r[3], r[79], r[22]);
    r[35] = spu_selb(r[23], r[35], r[22]);
    if (r[21]._u32[0] != 0) goto L_20E8;
    r[22] = spu_rotmi(r[18], -16);
    if ((int32_t)r[11]._u32[0] == 0) return 0x02118;
    r[16] = spu_clz(r[11]);
    r[10] = spu_clz(r[4]);
    r[13] = spu_il(1);
    r[7] = spu_fsmbi(0x0);
    r[16] = spu_sf(r[10], r[16]);
    r[9] = spu_ori(r[4], 0);
    r[13] = spu_shl(r[13], r[16]);
    r[10] = spu_shl(r[11], r[16]);
L_2140:
    r[17] = spu_or(r[7], r[13]);
    r[13] = spu_rotqmbii(r[13], -1);
    r[14] = spu_clgt(r[10], r[9]);
    r[16] = spu_sf(r[10], r[9]);
    r[10] = spu_rotqmbii(r[10], -1);
    r[7] = spu_selb(r[17], r[7], r[14]);
    r[9] = spu_selb(r[16], r[9], r[14]);
    if (r[13]._u32[0] != 0) goto L_2140;
    r[78] = spu_ila(0xFFFF);
    r[33] = spu_mpyh(r[79], r[15]);
    r[17] = spu_shlqbyi(r[8], 0);
    r[34] = spu_mpyu(r[15], r[79]);
    r[21] = spu_shli(r[35], 16);
    r[9] = spu_ai(r[79], -1);
    r[16] = spu_ai(r[9], -1);
    r[27] = spu_or(r[21], r[22]);
    r[19] = spu_a(r[33], r[34]);
    r[30] = spu_a(r[27], r[12]);
    r[25] = spu_clgt(r[19], r[27]);
    r[29] = spu_clgt(r[19], r[30]);
    r[32] = spu_clgt(r[12], r[30]);
    r[6] = spu_a(r[30], r[12]);
    r[31] = spu_ceqi(r[29], 0);
    r[8] = spu_ceqi(r[32], 0);
    r[5] = spu_selb(r[6], r[30], r[31]);
    r[28] = spu_ceqi(r[25], 0);
    r[20] = spu_selb(r[30], r[5], r[8]);
    r[26] = spu_ceqi(r[29], 0);
    r[13] = spu_selb(r[20], r[27], r[28]);
    r[7] = spu_selb(r[16], r[9], r[26]);
    r[2] = spu_sf(r[19], r[13]);
    r[10] = spu_selb(r[9], r[7], r[8]);
    if ((int32_t)r[11]._u32[0] == 0) return 0x021D0;
    r[22] = spu_clz(r[11]);
    r[19] = spu_clz(r[2]);
    r[20] = spu_il(1);
    r[64] = spu_fsmbi(0x0);
    r[22] = spu_sf(r[19], r[22]);
    r[4] = spu_ori(r[2], 0);
    r[20] = spu_shl(r[20], r[22]);
    r[19] = spu_shl(r[11], r[22]);
L_21F8:
    r[23] = spu_or(r[64], r[20]);
    r[20] = spu_rotqmbii(r[20], -1);
    r[21] = spu_clgt(r[19], r[4]);
    r[22] = spu_sf(r[19], r[4]);
    r[19] = spu_rotqmbii(r[19], -1);
    r[64] = spu_selb(r[23], r[64], r[21]);
    r[4] = spu_selb(r[22], r[4], r[21]);
    if (r[20]._u32[0] != 0) goto L_21F8;
    if ((int32_t)r[11]._u32[0] == 0) return 0x02220;
    r[16] = spu_clz(r[11]);
    r[7] = spu_clz(r[2]);
    r[13] = spu_il(1);
    r[8] = spu_fsmbi(0x0);
    r[16] = spu_sf(r[7], r[16]);
    r[3] = spu_ori(r[2], 0);
    r[13] = spu_shl(r[13], r[16]);
    r[7] = spu_shl(r[11], r[16]);
L_2248:
    r[19] = spu_or(r[8], r[13]);
    r[13] = spu_rotqmbii(r[13], -1);
    r[14] = spu_clgt(r[7], r[3]);
    r[16] = spu_sf(r[7], r[3]);
    r[7] = spu_rotqmbii(r[7], -1);
    r[8] = spu_selb(r[19], r[8], r[14]);
    r[3] = spu_selb(r[16], r[3], r[14]);
    if (r[13]._u32[0] != 0) goto L_2248;
    r[24] = spu_ceqi(r[25], 0);
    r[75] = spu_mpyu(r[15], r[64]);
    r[74] = spu_mpyh(r[64], r[15]);
    r[77] = spu_shli(r[4], 16);
    r[68] = spu_ai(r[64], -1);
    r[76] = spu_selb(r[10], r[79], r[24]);
    r[14] = spu_selb(r[77], r[18], r[78]);
    r[18] = spu_shli(r[76], 16);
    r[72] = spu_a(r[12], r[14]);
    r[73] = spu_a(r[74], r[75]);
    r[70] = spu_clgt(r[12], r[72]);
    r[71] = spu_clgt(r[73], r[72]);
    r[69] = spu_clgt(r[73], r[14]);
    r[7] = spu_a(r[68], r[71]);
    r[67] = spu_ceqi(r[70], 0);
    r[65] = spu_ceqi(r[69], 0);
    r[66] = spu_selb(r[68], r[7], r[67]);
    r[12] = spu_selb(r[66], r[64], r[65]);
    r[5] = spu_or(r[12], r[18]);
    goto L_22D8;
L_22C4:
    r[17] = spu_clgt(r[8], r[13]);
    if (r[17]._u32[0] == 0) goto L_22EC;
L_22D0:
    r[17] = spu_il(0);
    r[5] = spu_fsmbi(0x0);
L_22D8:
    r[12] = spu_cwd(r[1], 0);
    r[13] = spu_cwd(r[1], 4);
    r[20] = spu_shufb(r[17], r[7], r[12]);
    r[3] = spu_shufb(r[5], r[20], r[13]);
    return 0;
L_22EC:
    r[36] = spu_ori(r[8], 0);
    r[20] = spu_clz(r[36]);
    if (r[20]._u32[0] == 0) goto L_2B00;
    r[65] = spu_sfi(r[20], 32);
    r[66] = spu_shl(r[8], r[20]);
    r[68] = spu_sfi(r[65], 0);
    r[51] = spu_sfi(r[65], 0);
    r[67] = spu_rotm(r[12], r[68]);
    r[52] = spu_sfi(r[65], 0);
    r[64] = spu_rotm(r[18], r[51]);
    r[52] = spu_rotm(r[13], r[52]);
    r[29] = spu_or(r[66], r[67]);
    r[11] = spu_shl(r[13], r[20]);
    r[51] = spu_rotmi(r[29], -16);
    r[63] = spu_ila(0xFFFF);
    r[35] = spu_ila(0xFFFF);
    r[36] = spu_or(r[11], r[64]);
    if ((int32_t)r[51]._u32[0] == 0) return 0x02330;
    r[9] = spu_clz(r[51]);
    r[2] = spu_clz(r[52]);
    r[3] = spu_il(1);
    r[37] = spu_fsmbi(0x0);
    r[9] = spu_sf(r[2], r[9]);
    r[62] = spu_ori(r[52], 0);
    r[3] = spu_shl(r[3], r[9]);
    r[2] = spu_shl(r[51], r[9]);
L_2358:
    r[14] = spu_or(r[37], r[3]);
    r[3] = spu_rotqmbii(r[3], -1);
    r[8] = spu_clgt(r[2], r[62]);
    r[9] = spu_sf(r[2], r[62]);
    r[2] = spu_rotqmbii(r[2], -1);
    r[37] = spu_selb(r[14], r[37], r[8]);
    r[62] = spu_selb(r[9], r[62], r[8]);
    if (r[3]._u32[0] != 0) goto L_2358;
    r[61] = spu_rotmi(r[36], -16);
    r[40] = spu_and(r[29], r[63]);
    r[24] = spu_shl(r[12], r[20]);
    r[15] = spu_mpyu(r[29], r[37]);
    r[59] = spu_mpyh(r[37], r[29]);
    r[60] = spu_shli(r[62], 16);
    r[44] = spu_ai(r[37], -1);
    r[78] = spu_rotmi(r[24], -16);
    r[48] = spu_ai(r[44], -1);
    r[49] = spu_or(r[60], r[61]);
    r[14] = spu_ila(0x10000);
    r[45] = spu_a(r[59], r[15]);
    r[46] = spu_a(r[49], r[29]);
    r[41] = spu_clgt(r[45], r[49]);
    r[54] = spu_clgt(r[45], r[46]);
    r[58] = spu_clgt(r[29], r[46]);
    r[57] = spu_ceqi(r[54], 0);
    r[56] = spu_a(r[46], r[29]);
    r[23] = spu_ceqi(r[58], 0);
    r[55] = spu_selb(r[56], r[46], r[57]);
    r[50] = spu_ceqi(r[41], 0);
    r[42] = spu_shlqbyi(r[23], 0);
    r[53] = spu_selb(r[46], r[55], r[23]);
    r[47] = spu_ceqi(r[54], 0);
    r[7] = spu_selb(r[53], r[49], r[50]);
    r[43] = spu_selb(r[48], r[44], r[47]);
    r[7] = spu_sf(r[45], r[7]);
    r[38] = spu_selb(r[44], r[43], r[42]);
    if ((int32_t)r[51]._u32[0] == 0) return 0x023F0;
    r[22] = spu_clz(r[51]);
    r[15] = spu_clz(r[7]);
    r[17] = spu_il(1);
    r[27] = spu_fsmbi(0x0);
    r[22] = spu_sf(r[15], r[22]);
    r[39] = spu_ori(r[7], 0);
    r[17] = spu_shl(r[17], r[22]);
    r[15] = spu_shl(r[51], r[22]);
L_2418:
    r[23] = spu_or(r[27], r[17]);
    r[17] = spu_rotqmbii(r[17], -1);
    r[19] = spu_clgt(r[15], r[39]);
    r[22] = spu_sf(r[15], r[39]);
    r[15] = spu_rotqmbii(r[15], -1);
    r[27] = spu_selb(r[23], r[27], r[19]);
    r[39] = spu_selb(r[22], r[39], r[19]);
    if (r[17]._u32[0] != 0) goto L_2418;
    r[60] = spu_shl(r[18], r[20]);
    r[17] = spu_ceqi(r[41], 0);
    r[18] = spu_ila(0xFFFF);
    r[8] = spu_mpyh(r[27], r[40]);
    r[22] = spu_mpyu(r[40], r[27]);
    r[6] = spu_shli(r[39], 16);
    r[32] = spu_ai(r[27], -1);
    r[5] = spu_selb(r[38], r[37], r[17]);
    r[21] = spu_ai(r[32], -1);
    r[69] = spu_selb(r[6], r[36], r[35]);
    r[26] = spu_shli(r[5], 16);
    r[25] = spu_a(r[69], r[29]);
    r[64] = spu_a(r[8], r[22]);
    r[9] = spu_clgt(r[29], r[25]);
    r[19] = spu_clgt(r[64], r[25]);
    r[30] = spu_ceqi(r[9], 0);
    r[34] = spu_ceqi(r[19], 0);
    r[33] = spu_clgt(r[64], r[69]);
    r[31] = spu_selb(r[21], r[32], r[34]);
    r[70] = spu_ceqi(r[33], 0);
    r[28] = spu_selb(r[32], r[31], r[30]);
    r[4] = spu_a(r[25], r[29]);
    r[16] = spu_selb(r[28], r[27], r[70]);
    r[2] = spu_ceqi(r[19], 0);
    r[16] = spu_or(r[16], r[26]);
    r[3] = spu_selb(r[4], r[25], r[2]);
    r[77] = spu_rotmi(r[16], -16);
    r[65] = spu_mpyu(r[16], r[24]);
    r[79] = spu_mpyu(r[16], r[78]);
    r[10] = spu_ceqi(r[9], 0);
    r[74] = spu_mpyu(r[77], r[24]);
    r[66] = spu_mpyu(r[77], r[78]);
    r[71] = spu_selb(r[25], r[3], r[10]);
    r[75] = spu_rotmi(r[65], -16);
    r[12] = spu_selb(r[71], r[69], r[70]);
    r[62] = spu_and(r[65], r[18]);
    r[76] = spu_a(r[79], r[74]);
    r[68] = spu_a(r[66], r[14]);
    r[72] = spu_a(r[75], r[76]);
    r[57] = spu_sf(r[64], r[12]);
    r[73] = spu_clgt(r[74], r[72]);
    r[11] = spu_rotmi(r[72], -16);
    r[67] = spu_ceqi(r[73], 0);
    r[61] = spu_shli(r[72], 16);
    r[63] = spu_selb(r[68], r[66], r[67]);
    if ((int32_t)r[51]._u32[0] == 0) return 0x024F8;
    r[10] = spu_clz(r[51]);
    r[3] = spu_clz(r[7]);
    r[8] = spu_il(1);
    r[5] = spu_fsmbi(0x0);
    r[10] = spu_sf(r[3], r[10]);
    r[4] = spu_ori(r[7], 0);
    r[8] = spu_shl(r[8], r[10]);
    r[3] = spu_shl(r[51], r[10]);
L_2520:
    r[12] = spu_or(r[5], r[8]);
    r[8] = spu_rotqmbii(r[8], -1);
    r[9] = spu_clgt(r[3], r[4]);
    r[10] = spu_sf(r[3], r[4]);
    r[3] = spu_rotqmbii(r[3], -1);
    r[5] = spu_selb(r[12], r[5], r[9]);
    r[4] = spu_selb(r[10], r[4], r[9]);
    if (r[8]._u32[0] != 0) goto L_2520;
    r[56] = spu_a(r[63], r[11]);
    r[15] = spu_a(r[61], r[62]);
    r[5] = spu_shlqbyi(r[16], 0);
    r[59] = spu_ceq(r[57], r[56]);
    r[58] = spu_clgt(r[15], r[60]);
    r[54] = spu_sfi(r[59], 0);
    if ((int32_t)r[51]._u32[0] == 0) return 0x02568;
    r[19] = spu_clz(r[51]);
    r[14] = spu_clz(r[52]);
    r[15] = spu_il(1);
    r[4] = spu_fsmbi(0x0);
    r[19] = spu_sf(r[14], r[19]);
    r[3] = spu_ori(r[52], 0);
    r[15] = spu_shl(r[15], r[19]);
    r[14] = spu_shl(r[51], r[19]);
L_2590:
    r[20] = spu_or(r[4], r[15]);
    r[15] = spu_rotqmbii(r[15], -1);
    r[17] = spu_clgt(r[14], r[3]);
    r[19] = spu_sf(r[14], r[3]);
    r[14] = spu_rotqmbii(r[14], -1);
    r[4] = spu_selb(r[20], r[4], r[17]);
    r[3] = spu_selb(r[19], r[3], r[17]);
    if (r[15]._u32[0] != 0) goto L_2590;
    r[55] = spu_sfi(r[58], 0);
    r[46] = spu_clgt(r[56], r[57]);
    r[17] = spu_fsmbi(0x0);
    r[53] = spu_and(r[54], r[55]);
    r[50] = spu_ceqbi(r[53], 0);
    r[49] = spu_xsbh(r[50]);
    r[48] = spu_ceqhi(r[49], 0);
    r[47] = spu_nor(r[48], r[48]);
    r[45] = spu_xshw(r[47]);
    r[44] = spu_andc(r[45], r[46]);
    if (r[44]._u32[0] != 0) goto L_22D8;
    r[5] = spu_ai(r[16], -1);
    goto L_22D8;
L_25F0:
    r[12] = spu_shl(r[12], r[14]);
    r[63] = spu_sfi(r[14], 32);
    r[60] = spu_shl(r[13], r[14]);
    r[11] = spu_sfi(r[63], 0);
    r[62] = spu_sfi(r[63], 0);
    r[58] = spu_rotm(r[13], r[11]);
    r[11] = spu_rotmi(r[12], -16);
    r[61] = spu_rotm(r[18], r[62]);
    r[32] = spu_ila(0xFFFF);
    r[18] = spu_shl(r[18], r[14]);
    if ((int32_t)r[11]._u32[0] == 0) return 0x02618;
    r[19] = spu_clz(r[11]);
    r[15] = spu_clz(r[58]);
    r[16] = spu_il(1);
    r[34] = spu_fsmbi(0x0);
    r[19] = spu_sf(r[15], r[19]);
    r[59] = spu_ori(r[58], 0);
    r[16] = spu_shl(r[16], r[19]);
    r[15] = spu_shl(r[11], r[19]);
L_2640:
    r[20] = spu_or(r[34], r[16]);
    r[16] = spu_rotqmbii(r[16], -1);
    r[17] = spu_clgt(r[15], r[59]);
    r[19] = spu_sf(r[15], r[59]);
    r[15] = spu_rotqmbii(r[15], -1);
    r[34] = spu_selb(r[20], r[34], r[17]);
    r[59] = spu_selb(r[19], r[59], r[17]);
    if (r[16]._u32[0] != 0) goto L_2640;
    r[33] = spu_or(r[60], r[61]);
    if ((int32_t)r[11]._u32[0] == 0) return 0x02670;
    r[13] = spu_clz(r[11]);
    r[21] = spu_clz(r[58]);
    r[22] = spu_il(1);
    r[10] = spu_fsmbi(0x0);
    r[13] = spu_sf(r[21], r[13]);
    r[9] = spu_ori(r[58], 0);
    r[22] = spu_shl(r[22], r[13]);
    r[21] = spu_shl(r[11], r[13]);
L_2698:
    r[17] = spu_or(r[10], r[22]);
    r[22] = spu_rotqmbii(r[22], -1);
    r[23] = spu_clgt(r[21], r[9]);
    r[13] = spu_sf(r[21], r[9]);
    r[21] = spu_rotqmbii(r[21], -1);
    r[10] = spu_selb(r[17], r[10], r[23]);
    r[9] = spu_selb(r[13], r[9], r[23]);
    if (r[22]._u32[0] != 0) goto L_2698;
    r[57] = spu_rotmi(r[33], -16);
    r[54] = spu_mpyh(r[34], r[12]);
    r[55] = spu_mpyu(r[12], r[34]);
    r[56] = spu_shli(r[59], 16);
    r[15] = spu_ila(0xFFFF);
    r[40] = spu_ai(r[34], -1);
    r[15] = spu_and(r[12], r[15]);
    r[45] = spu_or(r[56], r[57]);
    r[44] = spu_ai(r[40], -1);
    r[50] = spu_a(r[45], r[12]);
    r[42] = spu_a(r[54], r[55]);
    r[53] = spu_clgt(r[12], r[50]);
    r[48] = spu_clgt(r[42], r[50]);
    r[51] = spu_a(r[50], r[12]);
    r[52] = spu_ceqi(r[48], 0);
    r[23] = spu_ceqi(r[53], 0);
    r[37] = spu_clgt(r[42], r[45]);
    r[49] = spu_selb(r[51], r[50], r[52]);
    r[38] = spu_shlqbyi(r[23], 0);
    r[46] = spu_ceqi(r[37], 0);
    r[47] = spu_selb(r[50], r[49], r[23]);
    r[43] = spu_ceqi(r[48], 0);
    r[41] = spu_selb(r[47], r[45], r[46]);
    r[39] = spu_selb(r[44], r[40], r[43]);
    r[36] = spu_sf(r[42], r[41]);
    if ((int32_t)r[11]._u32[0] == 0) return 0x02728;
    r[22] = spu_clz(r[11]);
    r[19] = spu_clz(r[36]);
    r[20] = spu_il(1);
    r[79] = spu_fsmbi(0x0);
    r[22] = spu_sf(r[19], r[22]);
    r[35] = spu_ori(r[36], 0);
    r[20] = spu_shl(r[20], r[22]);
    r[19] = spu_shl(r[11], r[22]);
L_2750:
    r[23] = spu_or(r[79], r[20]);
    r[20] = spu_rotqmbii(r[20], -1);
    r[21] = spu_clgt(r[19], r[35]);
    r[22] = spu_sf(r[19], r[35]);
    r[19] = spu_rotqmbii(r[19], -1);
    r[79] = spu_selb(r[23], r[79], r[21]);
    r[35] = spu_selb(r[22], r[35], r[21]);
    if (r[20]._u32[0] != 0) goto L_2750;
    if ((int32_t)r[11]._u32[0] == 0) return 0x02778;
    r[17] = spu_clz(r[11]);
    r[5] = spu_clz(r[36]);
    r[13] = spu_il(1);
    r[9] = spu_fsmbi(0x0);
    r[17] = spu_sf(r[5], r[17]);
    r[2] = spu_ori(r[36], 0);
    r[13] = spu_shl(r[13], r[17]);
    r[5] = spu_shl(r[11], r[17]);
L_27A0:
    r[19] = spu_or(r[9], r[13]);
    r[13] = spu_rotqmbii(r[13], -1);
    r[16] = spu_clgt(r[5], r[2]);
    r[17] = spu_sf(r[5], r[2]);
    r[5] = spu_rotqmbii(r[5], -1);
    r[9] = spu_selb(r[19], r[9], r[16]);
    r[2] = spu_selb(r[17], r[2], r[16]);
    if (r[13]._u32[0] != 0) goto L_27A0;
    r[22] = spu_selb(r[40], r[39], r[38]);
    r[21] = spu_ceqi(r[37], 0);
    r[5] = spu_mpyh(r[79], r[15]);
    r[30] = spu_mpyu(r[15], r[79]);
    r[31] = spu_shli(r[35], 16);
    r[13] = spu_ai(r[79], -1);
    r[6] = spu_selb(r[22], r[34], r[21]);
    r[27] = spu_ai(r[13], -1);
    r[4] = spu_selb(r[31], r[33], r[32]);
    r[75] = spu_shli(r[6], 16);
    r[19] = spu_a(r[4], r[12]);
    r[78] = spu_a(r[5], r[30]);
    r[25] = spu_clgt(r[12], r[19]);
    r[29] = spu_clgt(r[78], r[19]);
    r[20] = spu_a(r[19], r[12]);
    r[28] = spu_ceqi(r[29], 0);
    r[16] = spu_clgt(r[78], r[4]);
    r[9] = spu_ceqi(r[25], 0);
    r[10] = spu_selb(r[20], r[19], r[28]);
    r[24] = spu_selb(r[27], r[13], r[28]);
    r[8] = spu_ceqi(r[16], 0);
    r[7] = spu_selb(r[19], r[10], r[9]);
    r[2] = spu_selb(r[13], r[24], r[9]);
    r[77] = spu_selb(r[7], r[4], r[8]);
    r[76] = spu_selb(r[2], r[79], r[8]);
    r[4] = spu_sf(r[78], r[77]);
    r[8] = spu_or(r[76], r[75]);
    goto L_20C0;
L_283C:
    r[72] = spu_il(1);
    r[73] = spu_ila(0xFFFF);
    if ((int32_t)r[12]._u32[0] == 0) return 0x02848;
    r[8] = spu_clz(r[12]);
    r[5] = spu_clz(r[72]);
    r[6] = spu_il(1);
    r[71] = spu_fsmbi(0x0);
    r[8] = spu_sf(r[5], r[8]);
    r[3] = spu_ori(r[72], 0);
    r[6] = spu_shl(r[6], r[8]);
    r[5] = spu_shl(r[12], r[8]);
L_2870:
    r[9] = spu_or(r[71], r[6]);
    r[6] = spu_rotqmbii(r[6], -1);
    r[7] = spu_clgt(r[5], r[3]);
    r[8] = spu_sf(r[5], r[3]);
    r[5] = spu_rotqmbii(r[5], -1);
    r[71] = spu_selb(r[9], r[71], r[7]);
    r[3] = spu_selb(r[8], r[3], r[7]);
    if (r[6]._u32[0] != 0) goto L_2870;
    r[12] = spu_ori(r[71], 0);
    r[8] = spu_il(1);
    r[74] = spu_ori(r[12], 0);
    r[11] = spu_rotmi(r[12], -16);
    r[14] = spu_clz(r[74]);
    r[15] = spu_and(r[12], r[73]);
    r[4] = spu_sf(r[12], r[13]);
    if (r[14]._u32[0] == 0) goto L_20C0;
    goto L_25F0;
L_28C0:
    r[70] = spu_ori(r[12], 0);
    r[57] = spu_ila(0xFFFF);
    r[63] = spu_clz(r[70]);
    r[69] = spu_sfi(r[63], 32);
    r[67] = spu_shl(r[12], r[63]);
    r[68] = spu_sfi(r[69], 0);
    r[64] = spu_ceqi(r[63], 0);
    r[65] = spu_shl(r[13], r[63]);
    r[66] = spu_rotm(r[18], r[68]);
    r[24] = spu_selb(r[67], r[12], r[64]);
    r[60] = spu_shl(r[18], r[63]);
    r[30] = spu_rotmi(r[24], -16);
    r[62] = spu_or(r[65], r[66]);
    r[34] = spu_and(r[24], r[57]);
    r[31] = spu_selb(r[62], r[13], r[64]);
    r[29] = spu_selb(r[60], r[18], r[64]);
    if ((int32_t)r[30]._u32[0] == 0) return 0x02900;
    r[14] = spu_clz(r[30]);
    r[9] = spu_clz(r[31]);
    r[10] = spu_il(1);
    r[32] = spu_fsmbi(0x0);
    r[14] = spu_sf(r[9], r[14]);
    r[58] = spu_ori(r[31], 0);
    r[10] = spu_shl(r[10], r[14]);
    r[9] = spu_shl(r[30], r[14]);
L_2928:
    r[15] = spu_or(r[32], r[10]);
    r[10] = spu_rotqmbii(r[10], -1);
    r[11] = spu_clgt(r[9], r[58]);
    r[14] = spu_sf(r[9], r[58]);
    r[9] = spu_rotqmbii(r[9], -1);
    r[32] = spu_selb(r[15], r[32], r[11]);
    r[58] = spu_selb(r[14], r[58], r[11]);
    if (r[10]._u32[0] != 0) goto L_2928;
    r[56] = spu_rotmi(r[29], -16);
    r[53] = spu_mpyh(r[32], r[24]);
    r[54] = spu_mpyu(r[24], r[32]);
    r[55] = spu_shli(r[58], 16);
    r[38] = spu_ai(r[32], -1);
    r[43] = spu_ai(r[38], -1);
    r[44] = spu_or(r[55], r[56]);
    r[41] = spu_a(r[53], r[54]);
    r[49] = spu_a(r[44], r[24]);
    r[23] = spu_clgt(r[41], r[44]);
    r[47] = spu_clgt(r[41], r[49]);
    r[52] = spu_clgt(r[24], r[49]);
    r[51] = spu_ceqi(r[47], 0);
    r[50] = spu_a(r[49], r[24]);
    r[39] = spu_ceqi(r[52], 0);
    r[48] = spu_selb(r[50], r[49], r[51]);
    r[45] = spu_ceqi(r[23], 0);
    r[46] = spu_selb(r[49], r[48], r[39]);
    r[42] = spu_ceqi(r[47], 0);
    r[40] = spu_selb(r[46], r[44], r[45]);
    r[37] = spu_selb(r[43], r[38], r[42]);
    r[35] = spu_sf(r[41], r[40]);
    if ((int32_t)r[30]._u32[0] == 0) return 0x029A8;
    r[21] = spu_clz(r[30]);
    r[17] = spu_clz(r[35]);
    r[19] = spu_il(1);
    r[7] = spu_fsmbi(0x0);
    r[21] = spu_sf(r[17], r[21]);
    r[33] = spu_ori(r[35], 0);
    r[19] = spu_shl(r[19], r[21]);
    r[17] = spu_shl(r[30], r[21]);
L_29D0:
    r[22] = spu_or(r[7], r[19]);
    r[19] = spu_rotqmbii(r[19], -1);
    r[20] = spu_clgt(r[17], r[33]);
    r[21] = spu_sf(r[17], r[33]);
    r[17] = spu_rotqmbii(r[17], -1);
    r[7] = spu_selb(r[22], r[7], r[20]);
    r[33] = spu_selb(r[21], r[33], r[20]);
    if (r[19]._u32[0] != 0) goto L_29D0;
    r[21] = spu_ceqi(r[23], 0);
    r[22] = spu_selb(r[38], r[37], r[39]);
    if ((int32_t)r[30]._u32[0] == 0) return 0x02A00;
    r[16] = spu_clz(r[30]);
    r[23] = spu_clz(r[35]);
    r[9] = spu_il(1);
    r[10] = spu_fsmbi(0x0);
    r[16] = spu_sf(r[23], r[16]);
    r[2] = spu_ori(r[35], 0);
    r[9] = spu_shl(r[9], r[16]);
    r[23] = spu_shl(r[30], r[16]);
L_2A28:
    r[17] = spu_or(r[10], r[9]);
    r[9] = spu_rotqmbii(r[9], -1);
    r[15] = spu_clgt(r[23], r[2]);
    r[16] = spu_sf(r[23], r[2]);
    r[23] = spu_rotqmbii(r[23], -1);
    r[10] = spu_selb(r[17], r[10], r[15]);
    r[2] = spu_selb(r[16], r[2], r[15]);
    if (r[9]._u32[0] != 0) goto L_2A28;
    r[26] = spu_mpyu(r[34], r[7]);
    r[25] = spu_mpyh(r[7], r[34]);
    r[27] = spu_selb(r[22], r[32], r[21]);
    if ((int32_t)r[30]._u32[0] == 0) return 0x02A60;
    r[22] = spu_clz(r[30]);
    r[19] = spu_clz(r[31]);
    r[20] = spu_il(1);
    r[10] = spu_fsmbi(0x0);
    r[22] = spu_sf(r[19], r[22]);
    r[3] = spu_ori(r[31], 0);
    r[20] = spu_shl(r[20], r[22]);
    r[19] = spu_shl(r[30], r[22]);
L_2A88:
    r[23] = spu_or(r[10], r[20]);
    r[20] = spu_rotqmbii(r[20], -1);
    r[21] = spu_clgt(r[19], r[3]);
    r[22] = spu_sf(r[19], r[3]);
    r[19] = spu_rotqmbii(r[19], -1);
    r[10] = spu_selb(r[23], r[10], r[21]);
    r[3] = spu_selb(r[22], r[3], r[21]);
    if (r[20]._u32[0] != 0) goto L_2A88;
    r[28] = spu_shli(r[33], 16);
    r[20] = spu_ila(0xFFFF);
    r[15] = spu_ai(r[7], -1);
    r[8] = spu_shli(r[27], 16);
    r[19] = spu_selb(r[28], r[29], r[20]);
    r[17] = spu_a(r[25], r[26]);
    r[16] = spu_a(r[24], r[19]);
    r[11] = spu_clgt(r[17], r[19]);
    r[14] = spu_clgt(r[17], r[16]);
    r[17] = spu_fsmbi(0x0);
    r[13] = spu_clgt(r[24], r[16]);
    r[6] = spu_a(r[15], r[14]);
    r[10] = spu_ceqi(r[13], 0);
    r[5] = spu_ceqi(r[11], 0);
    r[9] = spu_selb(r[15], r[6], r[10]);
    r[7] = spu_selb(r[9], r[7], r[5]);
    r[5] = spu_or(r[7], r[8]);
    goto L_22D8;
L_2B00:
    r[43] = spu_clgt(r[12], r[18]);
    r[42] = spu_clgt(r[13], r[8]);
    r[17] = spu_fsmbi(0x0);
    r[41] = spu_nor(r[43], r[43]);
    r[40] = spu_sfi(r[42], 0);
    r[23] = spu_sfi(r[41], 0);
    r[5] = spu_il(1);
    r[39] = spu_or(r[40], r[23]);
    r[38] = spu_ceqbi(r[39], 0);
    r[37] = spu_xsbh(r[38]);
    if (r[37]._u16[0] == 0) goto L_22D8;
    goto L_22D0;
}

// ---- 0x2B0 CodecRaw::setPosition(this, subsound, position, postype) ------------------
// On a copy of the register file (the two helpers read registers the caller
// left); then File::seek.
void set_position(Run& c, u128* g, bool adjust)
{
    Q r[128];
    std::memcpy(r, g, sizeof r);
    if (adjust) {                                  // 0x3D0: the codec state -> this
        r[2] = spu_ai(r[3], -28);
        r[3] = spu_ceqi(r[3], 0);
        r[3] = spu_andc(r[2], r[3]);
    }
    r[6] = spu_ceqi(r[6], 8);                      // postype 8: bytes already
    r[7] = spu_ori(r[5], 0);
    stq(c, r[1], -0x20, r[81]);
    r[81] = spu_ori(r[3], 0);
    r[3] = spu_rotqmbyi(r[5], -4);                 // (u64)position
    stq(c, r[1], 0x10, r[0]);
    stq(c, r[1], -0x10, r[80]);
    stq(c, r[1], -0x40, r[1]);
    r[1] = spu_ai(r[1], -64);
    if (!w0(r[6])) {
        // bytes = position * wf[0x10C] / wf[0x110], down to a multiple of wf[0x114]
        COV(kPathSetPosPcm);
        r[18] = spu_il(268);
        r[19] = lq(c, r[81], 0x20);
        r[80] = spu_rotqby(r[19], r[81]);          // the wave format
        r[17] = spu_ai(r[80], 268);
        r[16] = lqx(c, r[80], r[18]);
        r[15] = spu_rotqby(r[16], r[17]);
        r[4] = spu_rotqmbyi(r[15], -4);
        r[0] = link(c, 0x304);
        if (muldi3(r)) return;                     // (it cannot halt)
        r[14] = lq(c, r[80], 0x110);
        r[13] = spu_rotqby(r[14], r[80]);
        r[4] = spu_rotqmbyi(r[13], -4);
        r[0] = link(c, 0x314);
        if (const uint32_t h = udivdi3(r)) {
            c.halt = h;
            COV(kPathHalt);
            return;
        }
        r[10] = spu_il(276);
        r[9] = spu_ai(r[80], 276);
        r[12] = spu_shlqbyi(r[3], 4);
        r[8] = lqx(c, r[80], r[10]);
        r[11] = spu_rotqby(r[8], r[9]);
        if (halts(c, r[11], 0x00328)) return;
        r[4] = udiv(r[12], r[11]).q;
        r[7] = mul32(r[11], r[4]);
    } else {
        COV(kPathSetPosRaw);
    }
    // L_390
    r[25] = spu_il(184);
    r[0] = lq(c, r[1], 0x50);
    r[26] = spu_il(252);
    r[5] = spu_fsmbi(0x0);
    r[22] = spu_ai(r[81], 184);
    r[21] = lqx(c, r[81], r[25]);
    r[24] = spu_ai(r[81], 252);
    r[23] = lqx(c, r[81], r[26]);
    r[1] = spu_ai(r[1], 64);
    r[80] = lq(c, r[1], -0x10);
    r[81] = lq(c, r[1], -0x20);
    r[20] = spu_rotqby(r[21], r[22]);              // this[0xB8]: the data's start
    r[3] = spu_rotqby(r[23], r[24]);               // this[0xFC]: the File
    r[4] = spu_a(r[7], r[20]);
    const Q res = seek(c, r[1], r[3], r[4], r[5]); // L_550, a tail jump
    if (c.halt) return;
    g[0] = r[0];
    g[1] = r[1];
    g[80] = r[80];
    g[81] = r[81];
    g[3] = res;
}

// ---- 0x3E0 CodecRaw::read(this, buffer, bytes, bytesread*) -------------------------
void read(Run& c, u128* g, bool adjust)
{
    Q r3 = g[3];
    if (adjust) {                                  // 0x490: the codec state -> this
        const Q r2 = spu_ai(r3, -28);
        r3 = spu_andc(r2, spu_ceqi(r3, 0));
    }
    const Q sp = g[1];
    const Q r11 = spu_ai(r3, 252);
    const Q r14 = lq(c, r3, 0x20);
    const Q r8 = spu_ori(r3, 0);
    const Q r7 = spu_ori(g[6], 0);
    stq(c, sp, -0x10, g[80]);
    const Q r80 = spu_ori(g[6], 0);
    stq(c, sp, 0x10, g[0]);
    const Q r6 = spu_rotmi(g[5], -1);              // 0x400
    stq(c, sp, -0x30, g[1]);
    const Q r9 = spu_ori(g[5], 0);
    const Q r10 = spu_shlqbyi(r11, 0);
    const Q r1 = spu_ai(g[1], -48);
    const Q wf = spu_rotqby(r14, r3);              // the wave format
    const Q r13 = lq(c, wf, 0x100);
    const Q r12 = spu_rotqby(r13, wf);
    if (w0(spu_ceqi(r12, 2))) {
        // L_458 PCM16: 16-bit elements, then *bytesread *= 2
        COV(kPathRead16);
        const Q r15 = lqx(c, r8, spu_il(252));
        const Q file = spu_rotqby(r15, r11);
        Saved s = from_gpr(g);
        s.r0 = link(c, 0x468);
        s.r1 = r1;
        s.r80 = r80;
        const Q res = file_read(c, s, file, g[4], spu_il(2), r6, r7);
        if (c.halt) return;
        const Q r10b = lq(c, s.r80, 0);
        const Q r8b = spu_cwd(s.r80, 0);
        const Q r9b = spu_rotqby(r10b, s.r80);
        stq(c, s.r80, 0, spu_shufb(spu_a(r9b, r9b), r10b, r8b));
        s.r0 = lq(c, s.r1, 0x40);
        s.r1 = spu_ai(s.r1, 48);
        s.r80 = lq(c, s.r1, -0x10);
        to_gpr(s, g);
        g[3] = res;
        return;
    }
    // bytes: a tail jump to File::read with the caller's r0, r1, r80 back
    COV(kPathRead8);
    Saved s = from_gpr(g);
    s.r0 = lq(c, r1, 0x40);
    s.r1 = spu_ai(r1, 48);
    const Q count = spu_shlqbyi(r9, 0);
    const Q r11b = lqx(c, r8, spu_il(252));
    s.r80 = lq(c, s.r1, -0x10);
    const Q file = spu_rotqby(r11b, r10);
    const Q res = file_read(c, s, file, g[4], spu_il(1), count, r7);
    if (c.halt) return;
    to_gpr(s, g);
    g[3] = res;
}

// ---- 0x1C8 the descriptor ----------------------------------------------------------
void descriptor(Run& c, u128* g)
{
    COV(kPathDescriptor);
    const Q r14 = spu_ila(0x1D0);
    const Q r13 = link(c, 0x1D0);
    const Q r12 = spu_ila(0x490);
    const Q r2 = spu_ila(0x3D0);
    const Q r10 = lq(c, c.d + 0x2EB0);
    const Q r3 = spu_ila(0x2EA0);
    const Q r9 = lq(c, c.d + 0x2EC0);
    const Q r79 = spu_sf(r14, r13);                // {delta, -0x1D0, -0x1D0, -0x1D0}
    const Q r7 = spu_cwd(g[1], 8);
    const Q r8 = spu_cwd(g[1], 0);
    stq(c, c.d + 0x2EB0, spu_shufb(spu_a(r12, r79), r10, r7));   // desc->read
    stq(c, c.d + 0x2EC0, spu_shufb(spu_a(r2, r79), r9, r8));     // desc->setposition
    g[3] = spu_a(r3, r79);
}

}  // namespace

Result call(uint8_t* ls, u128* gpr, uint32_t entry_lsa, uint32_t reloc_delta, const Channels& ch, uint32_t* halt_pc)
{
    Run c{ ls, reloc_delta, &ch };
    switch (entry_lsa - reloc_delta) {
    case kEntryDescriptor:
    case 0x1C8: descriptor(c, gpr); break;
    case kEntryRead: read(c, gpr, true); break;
    case 0x3E0: read(c, gpr, false); break;
    case kEntrySetPosition: set_position(c, gpr, true); break;
    case 0x2B0: set_position(c, gpr, false); break;
    case kEntryFileRead: {
        Saved s = from_gpr(gpr);
        const Q res = file_read(c, s, gpr[3], gpr[4], gpr[5], gpr[6], gpr[7]);
        if (c.halt) break;
        to_gpr(s, gpr);
        gpr[3] = res;
        break;
    }
    case kEntryFileSeek: {
        const Q res = seek(c, gpr[1], gpr[3], gpr[4], gpr[5]);
        if (!c.halt) gpr[3] = res;
        break;
    }
    default: return Result::declined;
    }
    if (c.halt) {
        if (halt_pc) *halt_pc = c.halt;
        return Result::halted;
    }
    return Result::handled;
}

const char* path_name(int p)
{
    static const char* const names[kPathCount] = {
        "descriptor", "read (bytes)", "read (PCM16)", "setposition (bytes)", "setposition (PCM)",
        "File::read: no buffer (37)", "File::read: negative size (37)", "File::read: past the end (19)",
        "File::read: clipped to the end", "copy: aligned", "copy: unaligned", "copy: tail", "buffer position wraps",
        "fill returned 22", "swap 16-bit", "swap 32-bit", "decrypt", "decrypt (XOR first)", "bytesread null",
        "result 22 (end)", "fill: refill", "fill: realign", "fill: refill again", "fill: result 21", "fill: negative percentage",
        "refill: several pieces", "refill: length learnt", "refill: flag 0x40", "refill: +0x1B0 wraps", "refill: nothing to read",
        "reallyRead: fits", "reallyRead: clipped (22)", "pacing: elapsed accumulated", "pacing: spin", "pacing: decrementer above",
        "seek: set", "seek: current", "seek: end", "seek: bad mode (37)", "seek: negative", "seek: clamped",
        "seek: cannot (20)", "seek: key index", "seek: buffered", "seek: unbuffered",
        "halt",
    };
    return p >= 0 && p < kPathCount ? names[p] : "?";
}

}  // namespace fmod_codec_raw
