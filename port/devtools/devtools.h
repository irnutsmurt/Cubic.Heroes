/* M11.0 (docs/M11_PLAN.md sections 3.3-3.5): the developer tools' core -- the command channel and the runner of Lua
 * chunks in the game's own script state. Everything here is inert unless developer mode is on
 * (DOTHEROES_DEVELOPER_MODE=1). */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* main.cpp, once, before the game runs: reads the setting; with developer mode on, sets up the channel (dev.cmd in the
 * working directory, answers in dev.out), the frame hook and the hold hook, and the on-screen tag. `elf`: the game's
 * EBOOT.elf on the player's disc (its folder holds the game's data: the maps' files the catalog reads). */
void devtools_start(const char* install_root, const char* elf);

/* 1 when developer mode is on (the tools are live) */
int devtools_on(void);

/* M11.1 (docs/M11_PLAN.md section 14): main.cpp, once, before the game's systems start: in an installed game (started
 * as a player starts it) with developer mode off, the older test switches that change the game -- life.trigger and
 * pos.trigger (DOTHEROES_DEV_STATE), DOTHEROES_DEV_CANT_DIE, DOTHEROES_DEV_CREDITS, DOTHEROES_DEV_WAVER,
 * DOTHEROES_DEV_SOUND -- are switched off; the read-only ones (the state and actors a test reads, logs, marks, the
 * harness's stop) stay. Outside an installed game (the repository's test runs) every switch works as before. */
void devtools_gate(int installed);

/* 0 while those switches are off (set by devtools_gate); 1 otherwise */
extern int g_dev_changes_ok;

/* the game's flip (0x0069DD60's entry, r1_observe.cpp): on the game's main thread, between two frames; null while
 * developer mode is off */
struct ppu_context;
extern void (*g_devtools_flip)(struct ppu_context* ctx);

/* the lift's entry observers (r1_observe.cpp) for the functions the tools watch (the player's damage): developer
 * mode only (r1_observe calls it only while devtools_on()) */
void devtools_entry(uint32_t addr, struct ppu_context* ctx);

/* a command from another front end (the in-game menu): queued; `done` is called with its answer on the game's thread,
 * or with an error when it is refused. Returns the command's number. */
typedef void (*devtools_reply)(void* user, int ok, const char* out);
uint64_t devtools_submit(const char* line, devtools_reply done, void* user);

#ifdef __cplusplus
}
#endif
