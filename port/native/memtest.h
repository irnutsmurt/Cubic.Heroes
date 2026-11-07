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

/* The test, on the guest space main.cpp reserved (`vm_size` bytes from
 * vm_base: the 4 GB and the guard). Returns the process exit code: 0 when
 * every check passed. */
int dotheroes_memtest(uint64_t vm_size);

#ifdef __cplusplus
}
#endif
