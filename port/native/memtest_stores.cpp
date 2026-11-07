/* memtest_stores.cpp -- the lifted code's guest stores as this build compiles
 * them (checked or mov; ppu_recomp.h's inline helpers, through a function's
 * snapshot of the memory model as lifted code takes one), for `dotheroes
 * --memtest`'s store groups (memtest.h; docs/NATIVE_R8.md section 5.4.1).
 * What they send out of line runs the loader's own paths: VM_WRITE_COH,
 * vm_raw_store_slow, the null page's drop. */
#include <string.h>
#include "ppu_recomp.h"
#include "memtest.h"

extern "C" uint8_t* vm_base;

static uint64_t be(const uint8_t* b, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | b[i];
    return v;
}

extern "C" int mt_store_size(int kind)
{
    switch (kind) {
    case MT_ST8: return 1;
    case MT_ST16: case MT_BRX16: return 2;
    case MT_ST32: case MT_BRX32: return 4;
    case MT_ST64: case MT_BRX64: case MT_STVLX8: return 8;
    case MT_STVX: case MT_STVX_G: return 16;
    default: return 128;   /* MT_DCBZ */
    }
}

extern "C" void mt_store(int kind, uint64_t ea, const uint8_t* b)
{
    const ppu_mm m = g_ppu_mm;   /* a lifted function's PPU_MM_SNAP */
    switch (kind) {
    case MT_ST8: vm_write8_mm(&m, ea, b[0]); break;
    case MT_ST16: vm_write16_mm(&m, ea, (uint16_t)be(b, 2)); break;
    case MT_ST32: vm_write32_mm(&m, ea, (uint32_t)be(b, 4)); break;
    case MT_ST64: vm_write64_mm(&m, ea, be(b, 8)); break;
    /* the byte-reversed stores: the register's bytes land in host order, so
     * a value whose host bytes are b lands as b */
    case MT_BRX16: { uint16_t r; memcpy(&r, b, 2); VM_RAW_ST(uint16_t, ea, r); break; }
    case MT_BRX32: { uint32_t r; memcpy(&r, b, 4); VM_RAW_ST(uint32_t, ea, r); break; }
    case MT_BRX64: { uint64_t r; memcpy(&r, b, 8); VM_RAW_ST(uint64_t, ea, r); break; }
    case MT_STVX: vm_raw_st16_mm(&m, ea, b); break;
    case MT_STVX_G: vm_raw_st16(ea, b); break;
    case MT_STVLX8: vm_raw_st_bytes(ea, b, 8); break;
    default: vm_raw_zero_line(ea); break;
    }
}

extern "C" void mt_store_model(int on)
{
    if (!on) { memset(&g_ppu_mm, 0, sizeof g_ppu_mm); return; }
    /* as ppu_mm_publish computes it for a game run on Windows (the whole
     * guest space mapped, no diagnostic armed): the inline span from 0x10000
     * to the raw SPU windows at 0xE0000000 */
    g_ppu_mm.vb = vm_base;
    g_ppu_mm.coh = g_spu_coh_map;
    g_ppu_mm.ld_span = g_ppu_mm.st_span = 0xE0000000ll - 0x10000;
    g_ppu_mm.poll = ppu_hle_inject_base + 0x2008u;
}
