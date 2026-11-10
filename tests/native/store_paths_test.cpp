/* store_paths_test.cpp -- the lifted code's guest stores, as ppu_recomp.h
 * generates them, against a small test guest space (docs/NATIVE_R8.md
 * section 5.4.1): which stores stay inline and which go out of line, at the
 * reservation bitmap's line boundaries and around the first page.
 * Built and run by tests/native/store_paths.py, once per store variant
 * (checked, mov). The out-of-line functions are stubs here that record their
 * calls: what they then do is the loader's (ppu_loader.cpp), tested in the
 * game by `dotheroes --memtest`. */
/* the out-of-line vm_read/vm_write are this file's stubs, as the loader's own
 * definitions are (it compiles the header the same way) */
#define PPU_NO_INLINE_LOADS 1
#include "ppu_recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static const uint32_t GUEST = 0x80000;                 /* the test guest space: 512 KB */
static uint8_t* g_mem;
static unsigned char g_bits[1u << 22];                /* the reservation bitmap: 4 GB / 128 / 8 */
extern "C" {
uint8_t* vm_base;
const unsigned char* const g_spu_coh_map = g_bits;
ppu_mm g_ppu_mm;
int vm_inline_ok = 1, vm_inline_st_ok = 1;
uint32_t ppu_vm_size = GUEST, ppu_hle_inject_base = 0x70000;
}

struct Call { const char* fn; uint32_t ea, n; };
static std::vector<Call> g_calls;
extern "C" {
void vm_write8(uint64_t a, uint8_t)   { g_calls.push_back({"vm_write8", (uint32_t)a, 1}); }
void vm_write16(uint64_t a, uint16_t) { g_calls.push_back({"vm_write16", (uint32_t)a, 2}); }
void vm_write32(uint64_t a, uint32_t) { g_calls.push_back({"vm_write32", (uint32_t)a, 4}); }
void vm_write64(uint64_t a, uint64_t) { g_calls.push_back({"vm_write64", (uint32_t)a, 8}); }
uint8_t vm_read8(uint64_t) { return 0; }
uint16_t vm_read16(uint64_t) { return 0; }
uint32_t vm_read32(uint64_t) { return 0; }
uint64_t vm_read64(uint64_t) { return 0; }
void vm_write_block_coh(uint64_t a, const void*, uint32_t n) { g_calls.push_back({"vm_write_block_coh", (uint32_t)a, n}); }
void vm_zero_block_coh(uint64_t a, uint32_t n) { g_calls.push_back({"vm_zero_block_coh", (uint32_t)a, n}); }
void vm_raw_store_slow(uint64_t a, const void*, uint32_t n) { g_calls.push_back({"vm_raw_store_slow", (uint32_t)a, n}); }
void vm_raw_zero_slow(uint64_t a, uint32_t n) { g_calls.push_back({"vm_raw_zero_slow", (uint32_t)a, n}); }
}

static int g_fail, g_checks;
static void reset(int64_t span)
{
    memset(g_mem, 0xAA, GUEST);
    memset(g_bits, 0, sizeof g_bits);
    g_calls.clear();
    g_ppu_mm.vb = vm_base;
    g_ppu_mm.coh = g_bits;
    g_ppu_mm.ld_span = g_ppu_mm.st_span = span;
    g_ppu_mm.poll = ppu_hle_inject_base + 0x2008u;
}
/* As spu_coh_reserve: the line's bit and the bit of the line before it (a store
 * starting there and running into this line is caught by its first line's bit). */
static void reserve(uint32_t ea)
{
    const uint32_t l = ea >> 7;
    g_bits[l >> 3] |= (unsigned char)(1u << (l & 7));
    if (l) g_bits[(l - 1) >> 3] |= (unsigned char)(1u << ((l - 1) & 7));
}
static bool untouched(uint32_t ea, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) if (g_mem[ea + i] != 0xAA) return false;
    return true;
}
/* expect: the out-of-line function called (once, with ea and n), or nullptr: none, the bytes written inline */
static void expect(const char* what, const char* fn, uint32_t ea, uint32_t n, const uint8_t* bytes = nullptr)
{
    g_checks++;
    bool ok;
    if (fn) ok = g_calls.size() == 1 && !strcmp(g_calls[0].fn, fn) && g_calls[0].ea == ea && g_calls[0].n == n &&
                 untouched(ea < GUEST ? ea : 0, ea < GUEST ? (ea + n <= GUEST ? n : GUEST - ea) : 0);
    else ok = g_calls.empty() && (!bytes || !memcmp(g_mem + ea, bytes, n));
    if (!ok) {
        g_fail++;
        printf("FAIL %s: want %s, got", what, fn ? fn : "inline");
        for (auto& c : g_calls) printf(" %s(0x%X,%u)", c.fn, c.ea, c.n);
        if (g_calls.empty()) printf(" no call");
        printf("\n");
    }
}

/* a scalar store as a lifted function makes it: through its snapshot */
static void st(uint32_t ea, uint32_t n, uint64_t v)
{
    const ppu_mm m = g_ppu_mm;
    switch (n) {
    case 1: vm_write8_mm(&m, ea, (uint8_t)v); break;
    case 2: vm_write16_mm(&m, ea, (uint16_t)v); break;
    case 4: vm_write32_mm(&m, ea, (uint32_t)v); break;
    default: vm_write64_mm(&m, ea, v); break;
    }
}

int main()
{
    g_mem = (uint8_t*)calloc(GUEST + 4096, 1);
    vm_base = g_mem;
    const int64_t span = GUEST - 0x10000;
    const uint32_t L = 0x20000;                      /* a 128-byte line; L + 128 the next */
    const uint8_t be4[4] = {0x11, 0x22, 0x33, 0x44}, be8[8] = {1, 2, 3, 4, 5, 6, 7, 8}, be2[2] = {0xAB, 0xCD};

    /* 1. Scalar stores at a line's end. Nothing reserved: inline, whether or not they run into the
     * next line. The next line reserved: every store starting in this line goes out of line (its bit
     * is set with the next line's), among them each one that runs into the reserved line. */
    for (uint32_t n : {2u, 4u, 8u}) {
        for (uint32_t off = 128 - n - 1; off <= 127; off++) {
            static const char* fns[9] = {0, 0, "vm_write16", 0, "vm_write32", 0, 0, 0, "vm_write64"};
            const uint64_t v = n == 2 ? 0xABCD : n == 4 ? 0x11223344 : 0x0102030405060708ull;
            const uint8_t* bytes = n == 2 ? be2 : n == 4 ? be4 : be8;
            char what[80];
            reset(span);
            st(L + off, n, v);
            snprintf(what, sizeof what, "%u-byte store at line+%u, nothing reserved", n, off);
            expect(what, nullptr, L + off, n, bytes);
            reset(span);
            reserve(L + 128);
            st(L + off, n, v);
            snprintf(what, sizeof what, "%u-byte store at line+%u, the next line reserved", n, off);
            expect(what, fns[n], L + off, n);
        }
    }
    reset(span); st(L + 127, 1, 0x5A); { const uint8_t b = 0x5A; expect("byte store at line+127", nullptr, L + 127, 1, &b); }

    /* 2. The reservation bitmap: the store's first line reserved, its second reserved, neither */
    reset(span); reserve(L); st(L + 124, 4, 0x11223344); expect("store, its line reserved", "vm_write32", L + 124, 4);
    /* inside the line before a reserved one: out of line too (the bitmap's superset), never wrong */
    reset(span); reserve(L + 128); st(L + 124, 4, 0x11223344); expect("store before a reserved line", "vm_write32", L + 124, 4);
    reset(span); reserve(L + 256); st(L + 124, 4, 0x11223344); expect("store two lines before a reserved one", nullptr, L + 124, 4, be4);
    reset(span); reserve(L + 128); st(L + 126, 4, 0x11223344); expect("store into a reserved line", "vm_write32", L + 126, 4);
    reset(span); reserve(L); st(L + 126, 4, 0x11223344); expect("store out of a reserved line", "vm_write32", L + 126, 4);
    reset(span); reserve(L + 128); st(L + 121, 8, 0x0102030405060708ull); expect("8 bytes into a reserved line", "vm_write64", L + 121, 8);

    /* 3. The range: the first 64 KB and the end of the space out of line, inside it inline */
    reset(span); st(0x0, 4, 1); expect("store at 0", "vm_write32", 0x0, 4);
    reset(span); st(0xFFC, 4, 1); expect("store in the first page", "vm_write32", 0xFFC, 4);
    reset(span); st(0xFFFE, 4, 1); expect("store across 0x10000", "vm_write32", 0xFFFE, 4);
    reset(span); st(0x10000, 4, 0x11223344); expect("store at 0x10000", nullptr, 0x10000, 4, be4);
    reset(span); st(GUEST - 4, 4, 0x11223344); expect("store ending at the space's end", nullptr, GUEST - 4, 4, be4);
    reset(span); st(GUEST - 2, 4, 1); expect("store past the space's end", "vm_write32", GUEST - 2, 4);
    reset(0); st(L, 4, 1); expect("store with a diagnostic armed (span 0)", "vm_write32", L, 4);

    /* 4. Raw stores (byte-reversed, vector, the unaligned forms, dcbz): the same range */
    reset(span); VM_RAW_ST(uint32_t, 0x800, 0x44332211u); expect("byte-reversed store in the null page", "vm_raw_store_slow", 0x800, 4);
    reset(span); VM_RAW_ST(uint16_t, 0xFFFF, (uint16_t)1); expect("byte-reversed store across 0x10000", "vm_raw_store_slow", 0xFFFF, 2);
    reset(span); VM_RAW_ST(uint32_t, L, 0x44332211u); { const uint8_t b[4] = {0x11, 0x22, 0x33, 0x44}; expect("byte-reversed store", nullptr, L, 4, b); }
    reset(span); reserve(L); VM_RAW_ST(uint32_t, L, 0x44332211u); expect("byte-reversed store, reserved", "vm_write_block_coh", L, 4);
    reset(span); reserve(L + 128); VM_RAW_ST(uint32_t, L + 126, 1u); expect("byte-reversed store into a reserved line", "vm_write_block_coh", L + 126, 4);
    reset(span); VM_RAW_ST(uint32_t, GUEST - 2, 1u); expect("byte-reversed store past the end", "vm_raw_store_slow", GUEST - 2, 4);
    const uint8_t v16[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    reset(span); vm_raw_st16(0x0, v16); expect("stvx at 0", "vm_raw_store_slow", 0x0, 16);
    reset(span); vm_raw_st16(0xFFF0, v16); expect("stvx below 0x10000", "vm_raw_store_slow", 0xFFF0, 16);
    reset(span); vm_raw_st16(L + 112, v16); expect("stvx", nullptr, L + 112, 16, v16);
    reset(span); reserve(L); vm_raw_st16(L + 112, v16); expect("stvx, reserved", "vm_write_block_coh", L + 112, 16);
    { reset(span); const ppu_mm m = g_ppu_mm; vm_raw_st16_mm(&m, 0x10, v16); expect("stvx (snapshot) in the null page", "vm_raw_store_slow", 0x10, 16); }
    { reset(span); const ppu_mm m = g_ppu_mm; vm_raw_st16_mm(&m, L, v16); expect("stvx (snapshot)", nullptr, L, 16, v16); }
    { reset(span); reserve(L); const ppu_mm m = g_ppu_mm; vm_raw_st16_mm(&m, L, v16); expect("stvx (snapshot), reserved", "vm_write_block_coh", L, 16); }
    { reset(0); const ppu_mm m = g_ppu_mm; vm_raw_st16_mm(&m, L, v16); expect("stvx (snapshot), diagnostic armed", "vm_raw_store_slow", L, 16); }
    reset(span); vm_raw_st_bytes(0x8, v16, 8); expect("stvlx in the null page", "vm_raw_store_slow", 0x8, 8);
    reset(span); vm_raw_st_bytes(0x8, v16, 0); expect("stvlx of nothing in the null page", nullptr, 0x8, 0);
    reset(span); vm_raw_st_bytes(L + 4, v16, 12); expect("stvlx", nullptr, L + 4, 12, v16);
    reset(span); vm_raw_zero_line(0xF80); expect("dcbz in the first page", "vm_raw_zero_slow", 0xF80, 128);
    reset(span); vm_raw_zero_line(0xFF80); expect("dcbz below 0x10000", "vm_raw_zero_slow", 0xFF80, 128);
    { reset(span); vm_raw_zero_line(L); uint8_t z[128] = {0}; expect("dcbz", nullptr, L, 128, z); }
    reset(span); reserve(L); vm_raw_zero_line(L); expect("dcbz, reserved", "vm_zero_block_coh", L, 128);
    reset(0); vm_raw_zero_line(L); expect("dcbz, diagnostic armed", "vm_raw_zero_slow", L, 128);

    printf("%s: %d checks, %d failed\n",
#ifdef PPU_STORE_MOV
           "mov",
#else
           "checked",
#endif
           g_checks, g_fail);
    return g_fail ? 1 : 0;
}
