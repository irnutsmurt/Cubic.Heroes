// The lifter's --promote-calls check (docs/NATIVE_R8.md section 9.0).
//
// A function lifted with --promote-calls keeps the guest registers in C locals
// across its calls. After each call it reloads the registers a callee may
// change (r0-r13, f0-f13, cr, ctr, xer) and keeps the rest -- r14-r31 and
// f14-f31, which the PowerPC 64 ABI has every callee preserve -- in its
// locals. A build with PPU_PROMO_CHECK (CMake DOTHEROES_PPU_PROMO_CHECK=ON)
// compares each kept register the function still reads with the context the
// callee left, after every call: a difference is a callee that did not keep
// it, which the promotion would have hidden.
#include <atomic>
#include <cstdint>
#include <cstdio>

extern "C" {
uint64_t g_ppu_promo_checks = 0;   // calls checked (the lifted code's own counter: the game thread)
}

namespace {
std::atomic<uint64_t> g_mismatches{0};
}

extern "C" void ppu_promo_mismatch(uint32_t site, int reg, uint64_t in_ctx, uint64_t in_local)
{
    const uint64_t n = g_mismatches.fetch_add(1) + 1;
    if (n <= 40)
        fprintf(stderr, "[promo] after a call in func_%08X: %s%d is 0x%016llx in the context, the caller kept 0x%016llx\n",
                site, reg >= 32 ? "f" : "r", reg >= 32 ? reg - 32 : reg, (unsigned long long)in_ctx,
                (unsigned long long)in_local);
}

extern "C" void ppu_promo_report(void)
{
#ifdef PPU_PROMO_CHECK
    static std::atomic<int> once{0};
    if (once.exchange(1)) return;
    fprintf(stderr, "[promo] check: %llu calls checked, %llu registers not kept by a callee\n",
            (unsigned long long)g_ppu_promo_checks, (unsigned long long)g_mismatches.load());
#endif
}
