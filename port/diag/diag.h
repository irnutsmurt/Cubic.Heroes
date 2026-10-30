/*
 * Development diagnostics for the 3D Dot Game Heroes port.
 *
 * Crash reports and the hang watchdog. Everything here is compiled only when
 * DOTHEROES_DEV_DIAGNOSTICS is defined (on by default in development builds,
 * off in release packages), so a shipped build carries none of it.
 *
 * Environment
 *   DIAG_CRASH_DIR=<dir>     where crash/hang reports go (default ./diag)
 *   DIAG_WATCHDOG=0          disable the hang watchdog
 *   DIAG_HANG_SECS=<n>       no guest flip for n seconds = hang (default 10)
 *   DIAG_BOOT_SECS=<n>       allowance before the first flip (default 120)
 *   DIAG_HANG_EXIT=1         exit(4) after a hang report (automated tests)
 *   DIAG_DUMP_AT=<s>[,<s>]   also dump every thread at these seconds
 *   DIAG_FPS=1               log guest flips per second every 10 s
 *   DIAG_PROF=<start>,<secs> sample every thread ~1 kHz; raw samples to
 *                            <DIAG_CRASH_DIR>/profile.raw (tests/prof_report.py)
 */
#ifndef DOTHEROES_DIAG_H
#define DOTHEROES_DIAG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Install the crash handler. Call first thing in main(), before the runtime
 * installs its own fault handlers, so they chain to this one. */
void diag_install_crash_handler(void);

/* Per-thread setup (alternate signal stack so a stack overflow still
 * reports). Called by diag for the main thread; other threads call it when
 * they start if the runtime offers a hook. */
void diag_thread_init(void);

/* Start the hang watchdog thread. */
void diag_start_watchdog(void);

/* Dump every thread's guest and host state now (also the watchdog's action). */
void diag_dump_all_threads(const char* reason);

#ifdef __cplusplus
}
#endif

#endif
