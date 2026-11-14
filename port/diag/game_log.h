/* The game's log (M9, docs/M9_LAUNCHER_PLAN.md section 20): levels, tagged events, the session's record.
 *
 * The log itself is the game's stdout and stderr: <install>/logs/game.log for a player's session (main.cpp,
 * installed_defaults), the console otherwise. The runtime buffers both and flushes them every 50 ms, at exit, on
 * abort() and on a crash (ps3recomp runtime/platform/ps3_stdio.c). This adds:
 *   - levels: DOTHEROES_LOG_LEVEL=error|warning|info|debug|trace (info by default; Debug and Trace off). They
 *     filter the events below; the game's and runtime's own lines are written at every level;
 *   - the session's record: the time, build, OS, CPU, memory and the active settings;
 *   - tagged events: [frame] frame-time spikes, [shader] pipelines built during play, [crash] a crash or an
 *     unexpected exception, [vulkan] the driver's warnings and errors (the runtime's backend);
 *   - the size cap: DOTHEROES_LOG_CAP_MB (64 by default, 0 none): past it, a line says so and the rest of the
 *     session's output is dropped (the session's start, where its set-up and first errors are, is kept). */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum { DH_LOG_ERROR = 0, DH_LOG_WARNING = 1, DH_LOG_INFO = 2, DH_LOG_DEBUG = 3, DH_LOG_TRACE = 4 };

int  dh_log_level(void);
void dh_log(int level, const char* tag, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
/* A game session's log begins: its record, the crash record's handlers, the spike and size watch. `log_path`: the
 * log file (empty: the console, no size cap). Before the runtime starts (its crash flush chains to this one). */
void dh_log_session_start(const char* log_path);
int  dh_log_session_active(void);
/* A setting read from the player's files, for the session's record */
void dh_log_note_setting(const char* name, const char* value);

#ifdef __cplusplus
}
#endif
