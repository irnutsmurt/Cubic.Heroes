// The waver (SPU image 2) in the game: its set-up and its one task a frame
// native (waver_task.h for the task itself; docs/SPU_NON_AUDIO.md section 4).
//
// What the game does [C, the lift]: the set-up (func_003CEB48, through the TOC
// stub func_009842F8 from func_0018B708) makes a SPURS instance (W + 0x60), a
// taskset (+0x58), a 0x3D600-byte block (+0x5C) and an event flag; each frame
// while the waver runs (func_003CEEE8 from the frame function func_000810F8)
// the PPU writes the parameters, waits for the previous frame's task
// (func_004735DC on the block) and creates the next (func_004736E0 on the
// taskset); when its time is up it waits, frees the block (func_00473510),
// destroys the taskset (func_0047353C) and the instance (func_004734A0); the
// shutdown (func_003CEAC0) does the same. W is the waver's state, 0x02AB2200
// (the TOC slot 0x01054804).
//
// Native (DOTHEROES_NATIVE_WAVER unset or 1, the default): the set-up is the
// port's (port/phys_hooks.cpp: the game's own stores and texture calls, no
// SPURS) and puts the markers below where the instance, taskset and block
// went; the shared SPURS wrappers' entry hooks act on the markers only -- a
// task's creation runs the frame natively on a worker thread of its own, the
// wait waits for it, the teardown's calls do nothing -- and pass everything
// else (the water's lifted path, a lifted waver) to the game's code.
// DOTHEROES_NATIVE_WAVER=0: the SPU task as before. =check: the SPU task, and
// at each of its creations the native frame on a copy of the inputs, compared
// with what the SPU task wrote once the game has waited for it.
#pragma once
#include <cstdint>

namespace waver_task {
constexpr uint32_t kNativeBlock = 0xFFFFF011u, kNativeTaskset = 0xFFFFF012u, kNativeInstance = 0xFFFFF013u;
constexpr uint32_t kStateSlot = 0x01054804u;   // the TOC slot holding W (r2 - 0x310C, TOC 0x01057910)
}

extern "C" int g_waver_hook;                       // the hooks' guard: native or check
extern "C" void waver_game_init(void);             // at start-up
extern "C" void waver_game_report(void);           // once (the test stop and atexit)
extern "C" int waver_native(void);                 // 1: set-up and frames native
extern "C" int waver_checking(void);               // 1: check mode
extern "C" void waver_native_started(uint32_t w);  // the native set-up's, W
extern "C" void waver_native_create(void);         // a frame: run natively (after any one still running)
extern "C" void waver_native_wait(void);           // returns once no native frame is running
extern "C" void waver_native_stopped(void);        // the teardown's: waits, counts
extern "C" void waver_check_create(uint32_t w);    // a lifted task created: the previous compared, this one's inputs taken
extern "C" void waver_check_end(void);             // the lifted block freed: the last one compared
