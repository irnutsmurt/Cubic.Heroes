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

/* the game's flip (0x0069DD60's entry, r1_observe.cpp): on the game's main thread, between two frames; null while
 * developer mode is off */
struct ppu_context;
extern void (*g_devtools_flip)(struct ppu_context* ctx);

/* a command from another front end (the in-game menu): queued; `done` is called with its answer on the game's thread,
 * or with an error when it is refused. Returns the command's number. */
typedef void (*devtools_reply)(void* user, int ok, const char* out);
uint64_t devtools_submit(const char* line, devtools_reply done, void* user);

#ifdef __cplusplus
}
#endif
