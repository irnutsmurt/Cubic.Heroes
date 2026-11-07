/* memtest.h -- `dotheroes --memtest`: the bare loads' assumptions, tested
 * (docs/NATIVE_R8.md section 5.3).
 *
 * The lifted code's guest loads come in two forms, chosen when the lift is
 * compiled (ppu_recomp.h): checked (the inline range, alignment and poll-word
 * tests, an out-of-line slow path behind them) and bare (PPU_BARE_LOADS: the
 * load and its byte swap). memtest_checked.cpp and memtest_bare.cpp compile
 * each form from the same generated header; memtest.cpp runs both against
 * the real guest space -- reserved, its pages committed on first touch, as a
 * game run has it -- and compares them with what the bytes say. Run before
 * the game is loaded, in a process of its own: it writes into the guest
 * space. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One guest load of `size` bytes (1, 2, 4, 8) at `ea`, big-endian. */
uint64_t mt_checked_read(int size, uint64_t ea);
uint64_t mt_bare_read(int size, uint64_t ea);
/* The bare load in a loop until the word is nonzero: 1 if it saw the change
 * within `iters` loads, 0 if not (a load hoisted out of the loop never sees
 * it). */
int mt_bare_spin(int size, uint64_t ea, uint64_t iters);
/* `n` bare loads of a word another thread flips between `a` and `b`: how
 * many returned neither (a torn access). */
uint64_t mt_bare_torn(int size, uint64_t ea, uint64_t a, uint64_t b, uint64_t n);
/* Whether this build's lifted code uses the bare form. */
int mt_build_bare(void);

/* One guest store as this build's lifted code makes it (memtest_stores.cpp):
 * `kind` below, `bytes` the bytes it puts in guest memory, in order
 * (mt_store_size of them; dcbz takes none). docs/NATIVE_R8.md section 5.4.1. */
enum { MT_ST8 = 1, MT_ST16 = 2, MT_ST32 = 4, MT_ST64 = 8,   /* stb .. std */
       MT_BRX16 = 102, MT_BRX32 = 104, MT_BRX64 = 108,      /* sthbrx, stwbrx, stdbrx */
       MT_STVX = 116,      /* stvx inside a lifted function (its snapshot) */
       MT_STVX_G = 117,    /* stvx outside one (the global model) */
       MT_STVLX8 = 118,    /* stvlx/stvrx: 8 bytes */
       MT_DCBZ = 128 };
void mt_store(int kind, uint64_t ea, const uint8_t* bytes);
int mt_store_size(int kind);
/* The memory model a game run publishes (the inline spans), or none (0):
 * the test runs before the loader publishes one. */
void mt_store_model(int on);

/* The test, on the guest space main.cpp reserved (`vm_size` bytes from
 * vm_base: the 4 GB and the guard). Returns the process exit code: 0 when
 * every check passed. */
int dotheroes_memtest(uint64_t vm_size);

#ifdef __cplusplus
}
#endif
