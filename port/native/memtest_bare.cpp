/* memtest_bare.cpp -- the bare guest loads (PPU_BARE_LOADS), compiled from
 * the lift's own ppu_recomp.h for `dotheroes --memtest` (memtest.h). */
#if defined(__x86_64__) || defined(_M_X64)
#ifndef PPU_BARE_LOADS
#define PPU_BARE_LOADS 1
#endif
#include <string.h>
#include "ppu_recomp.h"
#include "memtest.h"

extern "C" uint64_t mt_bare_read(int size, uint64_t ea)
{
    const ppu_mm m = ppu_mm_compute();
    switch (size) {
    case 1: return vm_read8_mm(&m, ea);
    case 2: return vm_read16_mm(&m, ea);
    case 4: return vm_read32_mm(&m, ea);
    default: return vm_read64_mm(&m, ea);
    }
}

/* The loop is here, around the inlined load, so the compiler sees what a
 * lifted spin on a guest word looks like. */
extern "C" int mt_bare_spin(int size, uint64_t ea, uint64_t iters)
{
    const ppu_mm m = ppu_mm_compute();
    switch (size) {
    case 1: for (uint64_t i = 0; i < iters; i++) if (vm_read8_mm(&m, ea)) return 1; return 0;
    case 2: for (uint64_t i = 0; i < iters; i++) if (vm_read16_mm(&m, ea)) return 1; return 0;
    case 4: for (uint64_t i = 0; i < iters; i++) if (vm_read32_mm(&m, ea)) return 1; return 0;
    default: for (uint64_t i = 0; i < iters; i++) if (vm_read64_mm(&m, ea)) return 1; return 0;
    }
}

extern "C" uint64_t mt_bare_torn(int size, uint64_t ea, uint64_t a, uint64_t b, uint64_t n)
{
    const ppu_mm m = ppu_mm_compute();
    uint64_t torn = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t v;
        switch (size) {
        case 2: v = vm_read16_mm(&m, ea); break;
        case 4: v = vm_read32_mm(&m, ea); break;
        default: v = vm_read64_mm(&m, ea); break;
        }
        torn += (v != a && v != b);
    }
    return torn;
}
#else
/* Not x86-64: the bare loads are not established here (the build's loads stay
 * checked); the test reads through the checked form in their place. */
#include <string.h>
#include "ppu_recomp.h"
#include "memtest.h"
extern "C" uint64_t mt_bare_read(int size, uint64_t ea) { return mt_checked_read(size, ea); }
extern "C" int mt_bare_spin(int size, uint64_t ea, uint64_t iters)
{
    for (uint64_t i = 0; i < iters; i++) if (mt_checked_read(size, ea)) return 1;
    return 0;
}
extern "C" uint64_t mt_bare_torn(int size, uint64_t ea, uint64_t a, uint64_t b, uint64_t n)
{
    uint64_t torn = 0;
    for (uint64_t i = 0; i < n; i++) { const uint64_t v = mt_checked_read(size, ea); torn += (v != a && v != b); }
    return torn;
}
#endif
