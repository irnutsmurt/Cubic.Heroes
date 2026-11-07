/* memtest_checked.cpp -- the checked guest loads (the inline range,
 * alignment and poll-word tests and their slow path), compiled from the
 * lift's own ppu_recomp.h for `dotheroes --memtest` (memtest.h), whichever
 * form the build's lifted code uses. */
#ifdef PPU_BARE_LOADS
#define MT_BUILD_BARE 1
#undef PPU_BARE_LOADS
#else
#define MT_BUILD_BARE 0
#endif
#include <string.h>
#include "ppu_recomp.h"
#include "memtest.h"

extern "C" uint64_t mt_checked_read(int size, uint64_t ea)
{
    const ppu_mm m = ppu_mm_compute();
    switch (size) {
    case 1: return vm_read8_mm(&m, ea);
    case 2: return vm_read16_mm(&m, ea);
    case 4: return vm_read32_mm(&m, ea);
    default: return vm_read64_mm(&m, ea);
    }
}

extern "C" int mt_build_bare(void) { return MT_BUILD_BARE; }
